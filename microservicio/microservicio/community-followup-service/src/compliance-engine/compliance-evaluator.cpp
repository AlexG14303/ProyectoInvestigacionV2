#include "compliance-evaluator.h"

bool cf_is_noncompliance_status(std::string_view compliance_status) {
    return compliance_status == "NO_CUMPLE";
}

std::string cf_compliance_alert_filter() {
    return "compliance_status=eq.NO_CUMPLE&order=followup_id.desc";
}
