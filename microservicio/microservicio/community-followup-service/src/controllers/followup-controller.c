#include "followup-controller.h"

const char *cf_followup_base_path(void) {
    return "/followups";
}

const char *cf_followup_legacy_path(void) {
    return "/followup";
}

const char *cf_compliance_alerts_path(void) {
    return "/compliance/alerts";
}
