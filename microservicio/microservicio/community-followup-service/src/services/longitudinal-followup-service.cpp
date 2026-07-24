#include "longitudinal-followup-service.h"
std::string cf_longitudinal_summary_label(int followup_count) {
    if (followup_count <= 0) return "sin seguimiento previo";
    if (followup_count == 1) return "primer seguimiento registrado";
    return "seguimiento longitudinal activo";
}
