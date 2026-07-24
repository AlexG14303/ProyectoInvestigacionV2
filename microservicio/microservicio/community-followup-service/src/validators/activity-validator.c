#include "activity-validator.h"
#include <stdio.h>
#include <string.h>
int cf_validate_activity_text(const char *activity, char *error, size_t error_size) {
    if (!activity || strlen(activity) < 5) {
        if (error && error_size > 0) snprintf(error, error_size, "scheduled_activities debe describir la actividad programada");
        return 0;
    }
    return 1;
}
