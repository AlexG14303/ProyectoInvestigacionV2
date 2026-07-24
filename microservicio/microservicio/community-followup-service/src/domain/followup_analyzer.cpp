#include "followup_analyzer.h"

namespace cf::domain {

std::string riskLevelLabel(RiskLevel level) {
    switch (level) {
        case RiskLevel::Alto:
            return "ALTO";
        case RiskLevel::Medio:
            return "MEDIO";
        case RiskLevel::Bajo:
        default:
            return "BAJO";
    }
}

RiskLevel classifyRisk(const json& record) {
    const std::string status = record.value("compliance_status", "PARCIAL");
    if (status == "NO_CUMPLE") return RiskLevel::Alto;
    if (status == "SI_CUMPLE") return RiskLevel::Bajo;
    return RiskLevel::Medio;
}

}  // namespace cf::domain
