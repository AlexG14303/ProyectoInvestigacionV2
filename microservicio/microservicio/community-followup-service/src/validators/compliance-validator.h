#ifndef COMPLIANCE_VALIDATOR_H
#define COMPLIANCE_VALIDATOR_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int cf_validate_noncompliance_cause(const char *status, const char *cause, char *error, size_t error_size);
#ifdef __cplusplus
}
#endif
#endif
