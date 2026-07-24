#ifndef FOLLOWUP_EVENT_PUBLISHER_H
#define FOLLOWUP_EVENT_PUBLISHER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void cf_build_followup_event_json(const char *event_name,
                                  int followup_id,
                                  int family_id,
                                  const char *record_number,
                                  const char *compliance_status,
                                  char *out,
                                  size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
