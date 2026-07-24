#include "compliance-evaluator.h"

bool cf_is_noncompliance_status(const std::string& compliance_status) {
    return compliance_status == "NO_CUMPLE";
}

std::string cf_compliance_alert_filter() {
    return "compliance_status=eq.NO_CUMPLE&order=followup_id.desc";
}
