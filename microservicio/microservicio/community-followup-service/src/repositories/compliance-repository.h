#ifndef COMPLIANCE_REPOSITORY_H
#define COMPLIANCE_REPOSITORY_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void cf_build_noncompliance_query(char *out, size_t out_size);
void cf_build_pending_query(char *out, size_t out_size);
#ifdef __cplusplus
}
#endif
#endif
