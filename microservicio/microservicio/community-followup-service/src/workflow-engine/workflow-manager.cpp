#include "workflow-manager.h"

std::string cf_workflow_completion_status(const std::string& requested_status) {
    if (requested_status.empty()) {
        return "SI_CUMPLE";
    }
    return requested_status;
}

std::string cf_workflow_completion_event_name() {
    return "followup.completed";
}
