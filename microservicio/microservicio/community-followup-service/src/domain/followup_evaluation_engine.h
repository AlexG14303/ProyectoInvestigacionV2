#pragma once

#include <string>
#include <vector>

#include "json.hpp"

namespace cf::domain {

using nlohmann::json;

struct EvaluationResult {
    int riskScore = 0;
    std::string riskLevel;         // BAJO | MEDIO | ALTO
    int complianceScore = 0;
    std::string priority;          // NORMAL | MODERADA | URGENTE
    std::vector<std::string> alerts;
    std::vector<std::string> recommendations;
    int rulesEvaluated = 0;
    int rulesTriggered = 0;
    double processingTimeMs = 0.0;
    bool parallelEnabled = false;
    int threadsUsed = 1;
};

// Motor de evaluación de UN seguimiento comunitario. Ejecuta 74 reglas
// independientes del dominio (estado de cumplimiento, fuerza del compromiso
// familiar y del equipo de salud, palabras críticas en causas de
// incumplimiento y en la descripción de riesgo, retraso por fecha, calidad
// del registro) en paralelo con OpenMP, y termina con 6 reglas de
// combinación/escalamiento que corren de forma SECUENCIAL a propósito,
// porque dependen del resultado ya agregado de las 74 anteriores (no pueden
// evaluarse de forma independiente).
//
// No hace ninguna llamada de red ni de base de datos. Se llama UNA vez por
// request, ANTES de la inserción — nunca se insertan varios registros ni se
// llama desde dentro de la región paralela.
EvaluationResult evaluateFollowup(const json& record);

// Número total de reglas (74 paralelas + 6 de combinación). Se expone para
// que /health pueda reportar `rules_configured` sin duplicar el número.
int totalRuleCount();

}  // namespace cf::domain
