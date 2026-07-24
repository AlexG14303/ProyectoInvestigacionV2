#ifndef SCHEDULED_ACTIVITY_DTO_H
#define SCHEDULED_ACTIVITY_DTO_H

typedef struct ScheduledActivityDto {
    int followup_id;
    char scheduled_activities[512];
    char evaluation_date[11];
} ScheduledActivityDto;

#endif
