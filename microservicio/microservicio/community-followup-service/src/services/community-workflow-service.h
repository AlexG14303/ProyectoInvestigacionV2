#ifndef COMMUNITY_WORKFLOW_SERVICE_H
#define COMMUNITY_WORKFLOW_SERVICE_H
#include <string>
#include <string_view>
// Fix S6009: mismo motivo que en commitment-tracker.h.
std::string cf_assign_workflow_stage(std::string_view compliance_status);
#endif
