#include "followup_batch_processor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <random>
#include <string>

#include <omp.h>

#include "parallel_config.h"
#include "../domain/followup_analyzer.h"
#include "../domain/followup_validator.h"

namespace cf::parallel {

using cf::domain::classifyRisk;
using cf::domain::cleanAndValidateFollowup;
using cf::domain::riskLevelLabel;
using cf::domain::RiskLevel;

BatchProcessResult processBatchCpuPhase(const std::vector<json>& rawRecords, bool partialUpdate) {
    const auto t0 = std::chrono::steady_clock::now();

    const auto n = static_cast<int>(rawRecords.size());
    BatchProcessResult result;
    result.recordsReceived = n;

    // Preasignado por índice: cada hilo escribe SOLO en su propia posición
    // (result.items[i]). Nada de push_back concurrente, nada de mutex.
    result.items.resize(static_cast<std::size_t>(n));

    const auto& cfg = config();
    const bool useParallel = n >= cfg.minParallelBatchSize;
    const int threads = useParallel ? std::min(cfg.ompNumThreads, std::max(n, 1)) : 1;
    result.parallelEnabled = useParallel;
    result.threadsUsed = threads;

    // FASE 2 — validar, normalizar y clasificar. Puramente CPU/memoria: no
    // hay ninguna llamada a PostgREST ni a disco aquí dentro.
    // schedule(dynamic, 64): el costo por registro varía (un registro
    // inválido corta temprano con una excepción; uno válido corre las 12
    // validaciones completas + clasificación), así que un reparto dinámico
    // por bloques de 64 balancea mejor que uno estático.
    #pragma omp parallel for num_threads(threads) if (useParallel) schedule(dynamic, 64)
    for (int i = 0; i < n; ++i) {
        BatchItemResult item;
        try {
            json normalized = cleanAndValidateFollowup(rawRecords[static_cast<std::size_t>(i)], partialUpdate);
            item.riskLevel = riskLevelLabel(classifyRisk(normalized));
            item.normalized = std::move(normalized);
            item.valid = true;
        // NOSONAR (cpp:S1181, "capturar una excepción más específica"):
        // cleanAndValidateFollowup() puede lanzar distintos tipos
        // (std::invalid_argument, std::runtime_error, json::parse_error,
        // etc.) y el manejo es idéntico para todos — marcar el ítem como
        // inválido y guardar el mensaje. Capturar std::exception aquí es
        // el patrón correcto para aislar el fallo de UN registro sin
        // interrumpir el resto del lote; distinguir subtipos no cambiaría
        // la lógica, solo la duplicaría.
        } catch (const std::exception& e) {  // NOSONAR
            item.valid = false;
            item.error = e.what();
        // NOSONAR (cpp:S2738, "capturar un tipo de excepción específico"):
        // red de seguridad final para cualquier throw que no derive de
        // std::exception (poco común, pero posible en C++). Sin este
        // catch-all, un valor no estándar lanzado por una dependencia
        // tumbaría todo el hilo OpenMP en vez de marcarse como un solo
        // ítem inválido.
        } catch (...) {  // NOSONAR
            item.valid = false;
            item.error = "Error desconocido validando el registro";
        }
        result.items[static_cast<std::size_t>(i)] = std::move(item);
    }

    // Conteo final por reducción. Ya no hay trabajo variable por elemento
    // (solo leer un bool/string ya calculado), así que schedule(static) es
    // la opción de menor overhead.
    long ok = 0;
    long bad = 0;
    long alto = 0;
    long medio = 0;
    long bajo = 0;
    #pragma omp parallel for num_threads(threads) if (useParallel) \
        reduction(+ : ok, bad, alto, medio, bajo) schedule(static)
    for (int i = 0; i < n; ++i) {
        const auto& item = result.items[static_cast<std::size_t>(i)];
        if (item.valid) {
            ++ok;
            if (item.riskLevel == "ALTO") ++alto;
            else if (item.riskLevel == "MEDIO") ++medio;
            else ++bajo;
        } else {
            ++bad;
        }
    }

    result.recordsValid = ok;
    result.recordsInvalid = bad;
    result.riskAlto = alto;
    result.riskMedio = medio;
    result.riskBajo = bajo;

    const auto t1 = std::chrono::steady_clock::now();
    result.processingTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return result;
}

AnalyticsSummary computeAnalyticsSummary(const std::vector<json>& records) {
    const auto t0 = std::chrono::steady_clock::now();

    const auto n = static_cast<int>(records.size());
    const auto& cfg = config();
    const bool useParallel = n >= cfg.minParallelBatchSize;
    const int threads = useParallel ? std::min(cfg.ompNumThreads, std::max(n, 1)) : 1;

    long total = 0;
    long completed = 0;
    long nonCompliant = 0;
    long partial = 0;
    long alto = 0;
    long medio = 0;
    long bajo = 0;

    // Fix S6177 ("using enum" para RiskLevel, C++20): el proyecto compila
    // con CXX_STANDARD 20 (ver CMakeLists.txt), así que esta feature está
    // disponible sin riesgo de compatibilidad.
    using enum RiskLevel;

    #pragma omp parallel for num_threads(threads) if (useParallel) \
        reduction(+ : total, completed, nonCompliant, partial, alto, medio, bajo) schedule(static)
    for (int i = 0; i < n; ++i) {
        const auto& record = records[static_cast<std::size_t>(i)];
        const std::string status = record.value("compliance_status", "");
        ++total;
        if (status == "SI_CUMPLE") ++completed;
        else if (status == "NO_CUMPLE") ++nonCompliant;
        else ++partial;

        switch (classifyRisk(record)) {
            case Alto:
                ++alto;
                break;
            case Medio:
                ++medio;
                break;
            case Bajo:
                ++bajo;
                break;
        }
    }

    AnalyticsSummary summary;
    summary.total = total;
    summary.completed = completed;
    summary.nonCompliant = nonCompliant;
    summary.partial = partial;
    summary.riskAlto = alto;
    summary.riskMedio = medio;
    summary.riskBajo = bajo;
    summary.compliancePercentage =
        total > 0 ? (100.0 * static_cast<double>(completed) / static_cast<double>(total)) : 0.0;
    summary.parallelEnabled = useParallel;
    summary.threadsUsed = threads;

    const auto t1 = std::chrono::steady_clock::now();
    summary.processingTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return summary;
}

std::vector<json> generateSyntheticRecords(int count) {
    std::vector<json> records;
    if (count <= 0) return records;
    records.reserve(static_cast<std::size_t>(count));

    // NOSONAR: mt19937 con semilla FIJA es intencional aquí — este generador
    // se usa exclusivamente para simular datos de prueba (endpoint
    // ?simulate=N de /analytics/summary), nunca para nada relacionado con
    // seguridad (no genera tokens, contraseñas, IDs de sesión ni ningún
    // valor criptográfico). La semilla fija es justamente el punto: permite
    // que las corridas de benchmark sean reproducibles entre ejecuciones.
    // Un generador criptográficamente seguro (std::random_device) sería
    // más lento y, al no ser determinista, rompería la reproducibilidad
    // que este código busca.
    std::mt19937 rng(20260713u);  // NOSONAR: PRNG no-criptográfico intencional, ver comentario arriba
    // Fix S6012 ("evitar especificar argumentos de template, usar CTAD"):
    // los literales 0 y 2 ya permiten deducir el tipo int.
    std::uniform_int_distribution statusDist(0, 2);
    // Fix S5945 ("usar std::array en vez de un arreglo C-style").
    static constexpr std::array statuses = {"SI_CUMPLE", "PARCIAL", "NO_CUMPLE"};

    for (int i = 0; i < count; ++i) {
        // NOSONAR (cpp:S6185, "usar std::format en vez de concatenar"):
        // std::format requiere GCC 13+; el Dockerfile usa ubuntu:22.04,
        // que trae GCC 11.4 por defecto — no compilaría con ese toolchain.
        records.push_back(json{{"followup_id", i + 1},
                                {"family_id", (i % 5000) + 1},
                                {"record_number", "SIM-" + std::to_string(i + 1)},  // NOSONAR
                                {"compliance_status", statuses[static_cast<std::size_t>(statusDist(rng))]}});
    }
    return records;
}

}  // namespace cf::parallel
