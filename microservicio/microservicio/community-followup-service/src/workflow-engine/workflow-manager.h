#ifndef WORKFLOW_MANAGER_H
#define WORKFLOW_MANAGER_H

#include <string>

std::string cf_workflow_completion_status(const std::string& requested_status);
std::string cf_workflow_completion_event_name();

#endif
