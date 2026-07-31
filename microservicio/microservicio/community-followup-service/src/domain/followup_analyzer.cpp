#include "followup_analyzer.h"

namespace cf::domain {

std::string riskLevelLabel(RiskLevel level) {
    // Fix S6177 ("using enum" para RiskLevel, C++20): el proyecto compila
    // con CXX_STANDARD 20 (ver CMakeLists.txt).
    using enum RiskLevel;
    switch (level) {
        case Alto:
            return "ALTO";
        case Medio:
            return "MEDIO";
        case Bajo:
        default:
            return "BAJO";
    }
}

RiskLevel classifyRisk(const json& record) {
    using enum RiskLevel;
    const std::string status = record.value("compliance_status", "PARCIAL");
    if (status == "NO_CUMPLE") return Alto;
    if (status == "SI_CUMPLE") return Bajo;
    return Medio;
}

}  // namespace cf::domain
