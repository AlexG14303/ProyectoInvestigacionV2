#pragma once

#include <string>
#include <vector>

#include "json.hpp"

namespace cf::parallel {

using nlohmann::json;

struct BatchItemResult {
    bool valid = false;
    std::string error;
    json normalized;
    std::string riskLevel;
};

struct BatchProcessResult {
    std::vector<BatchItemResult> items;
    long recordsReceived = 0;
    long recordsValid = 0;
    long recordsInvalid = 0;
    long riskAlto = 0;
    long riskMedio = 0;
    long riskBajo = 0;
    bool parallelEnabled = false;
    int threadsUsed = 1;
    double processingTimeMs = 0.0;
};

struct AnalyticsSummary {
    long total = 0;
    long completed = 0;
    long nonCompliant = 0;
    long partial = 0;
    long riskAlto = 0;
    long riskMedio = 0;
    long riskBajo = 0;
    double compliancePercentage = 0.0;
    bool parallelEnabled = false;
    int threadsUsed = 1;
    double processingTimeMs = 0.0;
};

// FASE CPU-BOUND. Valida + normaliza + clasifica cada registro del lote.
// No hace ninguna llamada de red ni de base de datos — eso es deliberado:
// la persistencia (I/O) se maneja aparte, fuera de esta función, en bloques
// controlados. Decide internamente si conviene usar OpenMP según
// MIN_PARALLEL_BATCH_SIZE (cláusula if() de OpenMP: por debajo del umbral
// se ejecuta secuencialmente sin ningún overhead de creación de equipo de
// hilos; en o por encima del umbral, se reparte con num_threads() fijado
// explícitamente desde la configuración).
BatchProcessResult processBatchCpuPhase(const std::vector<json>& rawRecords, bool partialUpdate);

// Calcula agregados (conteos por reducción + clasificación de riesgo) sobre
// una colección ya cargada en memoria — ya sea el resultado de una consulta
// previa a PostgREST, o datos sintéticos generados solo para pruebas de CPU.
AnalyticsSummary computeAnalyticsSummary(const std::vector<json>& records);

// Genera registros sintéticos EN MEMORIA únicamente; no toca la base de
// datos. Sirve para ejercitar GET /followups/analytics/summary con volumen
// controlado y reproducible aunque la tabla real tenga pocos registros.
std::vector<json> generateSyntheticRecords(int count);

}  // namespace cf::parallel
