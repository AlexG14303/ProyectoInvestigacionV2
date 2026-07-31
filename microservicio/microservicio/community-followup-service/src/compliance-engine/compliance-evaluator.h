#ifndef COMPLIANCE_EVALUATOR_H
#define COMPLIANCE_EVALUATOR_H

#include <string>
#include <string_view>

// Fix S6009: mismo motivo que en commitment-tracker.h — el parámetro solo
// se compara, nunca se copia.
bool cf_is_noncompliance_status(std::string_view compliance_status);
std::string cf_compliance_alert_filter();

#endif
