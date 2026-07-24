#include "followup_batch_processor.h"

#include <algorithm>
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

    const int n = static_cast<int>(rawRecords.size());
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
        } catch (const std::exception& e) {
            item.valid = false;
            item.error = e.what();
        } catch (...) {
            item.valid = false;
            item.error = "Error desconocido validando el registro";
        }
        result.items[static_cast<std::size_t>(i)] = std::move(item);
    }

    // Conteo final por reducción. Ya no hay trabajo variable por elemento
    // (solo leer un bool/string ya calculado), así que schedule(static) es
    // la opción de menor overhead.
    long ok = 0, bad = 0, alto = 0, medio = 0, bajo = 0;
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

    const int n = static_cast<int>(records.size());
    const auto& cfg = config();
    const bool useParallel = n >= cfg.minParallelBatchSize;
    const int threads = useParallel ? std::min(cfg.ompNumThreads, std::max(n, 1)) : 1;

    long total = 0, completed = 0, nonCompliant = 0, partial = 0;
    long alto = 0, medio = 0, bajo = 0;

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
            case RiskLevel::Alto:
                ++alto;
                break;
            case RiskLevel::Medio:
                ++medio;
                break;
            case RiskLevel::Bajo:
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

    std::mt19937 rng(20260713u);  // semilla fija -> reproducible entre corridas
    std::uniform_int_distribution<int> statusDist(0, 2);
    static const char* statuses[3] = {"SI_CUMPLE", "PARCIAL", "NO_CUMPLE"};

    for (int i = 0; i < count; ++i) {
        records.push_back(json{{"followup_id", i + 1},
                                {"family_id", (i % 5000) + 1},
                                {"record_number", "SIM-" + std::to_string(i + 1)},
                                {"compliance_status", statuses[statusDist(rng)]}});
    }
    return records;
}

}  // namespace cf::parallel
