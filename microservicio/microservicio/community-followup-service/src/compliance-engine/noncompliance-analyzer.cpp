#include "noncompliance-analyzer.h"

std::string cf_noncompliance_default_cause(const std::string& cause) {
    if (cause.empty()) {
        return "Causa de incumplimiento pendiente de registrar";
    }
    return cause;
}
