#ifndef COMPLIANCE_EVALUATOR_H
#define COMPLIANCE_EVALUATOR_H

#include <string>

bool cf_is_noncompliance_status(const std::string& compliance_status);
std::string cf_compliance_alert_filter();

#endif
