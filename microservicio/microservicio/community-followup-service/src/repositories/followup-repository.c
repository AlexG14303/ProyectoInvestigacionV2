#include "followup-repository.h"
#include <stdio.h>
#include <string.h>

static void append_filter(char *out, size_t out_size, const char *field, const char *value) {
    if (value == NULL || value[0] == '\0') return;
    size_t len = strlen(out);
    if (len + 1 < out_size) {
        snprintf(out + len, out_size - len, "&%s=eq.%s", field, value);
    }
}

void cf_build_followup_list_query(const char *family_id,
                                  const char *risk_assessment_id,
                                  const char *compliance_status,
                                  char *out,
                                  size_t out_size) {
    if (out == NULL || out_size == 0) return;
    snprintf(out, out_size, "order=followup_id.desc");
    append_filter(out, out_size, "family_id", family_id);
    append_filter(out, out_size, "risk_assessment_id", risk_assessment_id);
    append_filter(out, out_size, "compliance_status", compliance_status);
}

void cf_build_followup_id_filter(int followup_id, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return;
    snprintf(out, out_size, "followup_id=eq.%d", followup_id);
}

void cf_build_compliance_alerts_query(char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return;
    snprintf(out, out_size, "compliance_status=eq.NO_CUMPLE&order=followup_id.desc");
}
