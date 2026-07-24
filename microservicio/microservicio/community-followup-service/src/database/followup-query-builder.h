#ifndef FOLLOWUP_QUERY_BUILDER_H
#define FOLLOWUP_QUERY_BUILDER_H

#ifdef __cplusplus
extern "C" {
#endif

int cf_is_allowed_followup_field(const char *field);
const char *cf_followup_table_name(void);

#ifdef __cplusplus
}
#endif

#endif
