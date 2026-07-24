#ifndef ACTIVITY_VALIDATOR_H
#define ACTIVITY_VALIDATOR_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int cf_validate_activity_text(const char *activity, char *error, size_t error_size);
#ifdef __cplusplus
}
#endif
#endif
