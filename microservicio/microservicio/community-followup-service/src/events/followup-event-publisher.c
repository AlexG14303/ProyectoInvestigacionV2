#include "followup-event-publisher.h"
#include <stdio.h>

void cf_build_followup_event_json(const char *event_name,
                                  int followup_id,
                                  int family_id,
                                  const char *record_number,
                                  const char *compliance_status,
                                  char *out,
                                  size_t out_size) {
    if (out == NULL || out_size == 0) return;
    snprintf(out, out_size,
        "{\"event_name\":\"%s\",\"event_version\":\"1.0\",\"producer\":\"community-followup-service\",\"payload\":{\"followup_id\":%d,\"family_id\":%d,\"record_number\":\"%s\",\"compliance_status\":\"%s\"}}",
        event_name ? event_name : "followup.updated",
        followup_id,
        family_id,
        record_number ? record_number : "",
        compliance_status ? compliance_status : "");
}
