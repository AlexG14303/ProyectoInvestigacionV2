#include "followup-query-builder.h"
#include <string.h>

const char *cf_followup_table_name(void) {
    return "followup_records";
}

int cf_is_allowed_followup_field(const char *field) {
    static const char *allowed[] = {
        "family_id",
        "risk_assessment_id",
        "record_number",
        "analysis_date",
        "evaluation_date",
        "risk_description",
        "scheduled_activities",
        "family_commitment",
        "health_team_commitment",
        "responsible_staff_id",
        "compliance_status",
        "noncompliance_causes",
        NULL
    };

    if (field == NULL) return 0;
    for (int i = 0; allowed[i] != NULL; ++i) {
        if (strcmp(field, allowed[i]) == 0) return 1;
    }
    return 0;
}
