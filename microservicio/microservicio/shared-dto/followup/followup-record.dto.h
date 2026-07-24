#pragma once
struct FollowupRecordDto {
    int followup_id;
    int family_id;
    int risk_assessment_id;
    const char* record_number;
    const char* compliance_status;
};
