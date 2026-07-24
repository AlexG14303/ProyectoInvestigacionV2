#include "followup-service.h"
#include "followup-validator.h"
#include <stdio.h>

const char *cf_default_compliance_status(void) {
    return "PARCIAL";
}

int cf_prepare_compliance_status(const char *requested_status, char *out, size_t out_size, char *error, size_t error_size) {
    const char *status = (requested_status == NULL || requested_status[0] == '\0')
        ? cf_default_compliance_status()
        : requested_status;

    if (!cf_validate_compliance_status(status, error, error_size)) {
        return 0;
    }

    if (out != NULL && out_size > 0) {
        snprintf(out, out_size, "%s", status);
    }
    return 1;
}
