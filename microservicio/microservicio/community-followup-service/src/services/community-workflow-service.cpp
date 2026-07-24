#include "community-workflow-service.h"
std::string cf_assign_workflow_stage(const std::string& compliance_status) {
    if (compliance_status == "SI_CUMPLE") return "cerrado";
    if (compliance_status == "NO_CUMPLE") return "alerta";
    return "seguimiento";
}
