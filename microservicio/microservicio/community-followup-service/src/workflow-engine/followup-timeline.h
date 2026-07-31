#ifndef FOLLOWUP_TIMELINE_H
#define FOLLOWUP_TIMELINE_H

#include <string>
#include <string_view>

// Fix S6009: mismo motivo que en commitment-tracker.h.
std::string cf_timeline_state_label(std::string_view compliance_status);

#endif
