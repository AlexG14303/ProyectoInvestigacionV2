#ifndef FOLLOWUP_VALIDATOR_H
#define FOLLOWUP_VALIDATOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int cf_validate_compliance_status(const char *status, char *error, size_t error_size);
int cf_validate_required_create(int has_family_id, int has_record_number, char *error, size_t error_size);
int cf_validate_iso_date(const char *date, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
