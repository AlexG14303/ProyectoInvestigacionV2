#include "followup-timeline.h"

std::string cf_timeline_state_label(std::string_view compliance_status) {
    if (compliance_status == "SI_CUMPLE") return "seguimiento completado";
    if (compliance_status == "NO_CUMPLE") return "requiere alerta de cumplimiento";
    return "seguimiento en proceso";
}
