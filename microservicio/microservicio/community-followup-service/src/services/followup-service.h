#ifndef FOLLOWUP_SERVICE_H
#define FOLLOWUP_SERVICE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *cf_default_compliance_status(void);
int cf_prepare_compliance_status(const char *requested_status, char *out, size_t out_size, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
