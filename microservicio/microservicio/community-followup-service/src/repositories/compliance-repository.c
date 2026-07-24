#include "compliance-repository.h"
#include <stdio.h>
void cf_build_noncompliance_query(char *out, size_t out_size) {
    if (out && out_size > 0) snprintf(out, out_size, "compliance_status=eq.NO_CUMPLE&order=evaluation_date.asc");
}
void cf_build_pending_query(char *out, size_t out_size) {
    if (out && out_size > 0) snprintf(out, out_size, "compliance_status=eq.PARCIAL&order=evaluation_date.asc");
}
