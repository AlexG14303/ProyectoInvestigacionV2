#ifndef FOLLOWUP_REPOSITORY_H
#define FOLLOWUP_REPOSITORY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void cf_build_followup_list_query(const char *family_id,
                                  const char *risk_assessment_id,
                                  const char *compliance_status,
                                  char *out,
                                  size_t out_size);
void cf_build_followup_id_filter(int followup_id, char *out, size_t out_size);
void cf_build_compliance_alerts_query(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
