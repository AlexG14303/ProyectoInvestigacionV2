#include "followup_validator.h"

#include <stdexcept>
#include <string>

extern "C" {
#include "database/followup-query-builder.h"
#include "services/followup-service.h"
#include "validators/followup-validator.h"
}

namespace cf::domain {

namespace {

void validateIntegerField(const json& data, const char* field, bool required) {
    if (!data.contains(field) || data[field].is_null()) {
        if (required) {
            throw std::invalid_argument(std::string(field) + " es obligatorio");
        }
        return;
    }
    if (!data[field].is_number_integer()) {
        throw std::invalid_argument(std::string(field) + " debe ser entero");
    }
}

void validateStringField(const json& data, const char* field, bool required, std::size_t maxLen = 0) {
    if (!data.contains(field) || data[field].is_null()) {
        if (required) {
            throw std::invalid_argument(std::string(field) + " es obligatorio");
        }
        return;
    }
    if (!data[field].is_string()) {
        throw std::invalid_argument(std::string(field) + " debe ser texto");
    }
    if (maxLen > 0 && data[field].get<std::string>().size() > maxLen) {
        throw std::invalid_argument(std::string(field) + " supera la longitud máxima permitida");
    }
}

void validateStatus(const json& data) {
    if (!data.contains("compliance_status") || data["compliance_status"].is_null()) return;
    if (!data["compliance_status"].is_string()) {
        throw std::invalid_argument("compliance_status debe ser texto");
    }
    char error[160] = {0};
    const std::string status = data["compliance_status"].get<std::string>();
    if (!cf_validate_compliance_status(status.c_str(), error, sizeof(error))) {
        throw std::invalid_argument(error);
    }
}

void validateDate(const json& data, const char* field) {
    if (!data.contains(field) || data[field].is_null()) return;
    if (!data[field].is_string()) {
        throw std::invalid_argument(std::string(field) + " debe ser texto con formato YYYY-MM-DD");
    }
    char error[160] = {0};
    const std::string date = data[field].get<std::string>();
    if (!cf_validate_iso_date(date.c_str(), error, sizeof(error))) {
        throw std::invalid_argument(error);
    }
}

}  // namespace

void validateIsoDateField(const json& data, const char* field) {
    validateDate(data, field);
}

json cleanAndValidateFollowup(const json& input, bool partialUpdate) {
    if (!input.is_object()) {
        throw std::invalid_argument("Cada seguimiento debe ser un objeto JSON");
    }

    json data = json::object();
    for (auto it = input.begin(); it != input.end(); ++it) {
        if (cf_is_allowed_followup_field(it.key().c_str())) {
            data[it.key()] = it.value();
        }
    }

    if (!partialUpdate) {
        char error[160] = {0};
        const int hasFamilyId = data.contains("family_id") && !data["family_id"].is_null();
        const int hasRecordNumber = data.contains("record_number") && !data["record_number"].is_null();
        if (!cf_validate_required_create(hasFamilyId, hasRecordNumber, error, sizeof(error))) {
            throw std::invalid_argument(error);
        }
        if (!data.contains("compliance_status")) {
            data["compliance_status"] = cf_default_compliance_status();
        }
    }

    if (data.empty() && partialUpdate) {
        throw std::invalid_argument("No se enviaron campos válidos para actualizar");
    }

    validateIntegerField(data, "family_id", !partialUpdate);
    validateIntegerField(data, "risk_assessment_id", false);
    validateStringField(data, "record_number", !partialUpdate, 80);
    validateStringField(data, "risk_description", false);
    validateStringField(data, "scheduled_activities", false);
    validateStringField(data, "family_commitment", false);
    validateStringField(data, "health_team_commitment", false);
    validateIntegerField(data, "responsible_staff_id", false);
    validateStringField(data, "noncompliance_causes", false);
    validateStatus(data);
    validateDate(data, "analysis_date");
    validateDate(data, "evaluation_date");

    return data;
}

}  // namespace cf::domain
