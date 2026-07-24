#include "compliance-validator.h"
#include <stdio.h>
#include <string.h>
int cf_validate_noncompliance_cause(const char *status, const char *cause, char *error, size_t error_size) {
    if (status && strcmp(status, "NO_CUMPLE") == 0 && (!cause || cause[0] == '\0')) {
        if (error && error_size > 0) snprintf(error, error_size, "noncompliance_causes es requerido cuando compliance_status es NO_CUMPLE");
        return 0;
    }
    return 1;
}
