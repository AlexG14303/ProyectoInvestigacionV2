#ifndef LONGITUDINAL_SUMMARY_DTO_H
#define LONGITUDINAL_SUMMARY_DTO_H

typedef struct LongitudinalSummaryDto {
    int family_id;
    int total_followups;
    int completed_followups;
    int noncompliance_alerts;
} LongitudinalSummaryDto;

#endif
