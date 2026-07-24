#pragma once

#include <string>

#include "json.hpp"

namespace cf::domain {

using nlohmann::json;

enum class RiskLevel { Bajo, Medio, Alto };

std::string riskLevelLabel(RiskLevel level);

// Clasifica el nivel de riesgo de un registro YA validado, a partir de
// compliance_status. Regla simple e intencionalmente explícita (fácil de
// ajustar si el criterio real del dominio es otro):
//   SI_CUMPLE  -> Bajo
//   PARCIAL    -> Medio
//   NO_CUMPLE  -> Alto
RiskLevel classifyRisk(const json& record);

}  // namespace cf::domain
