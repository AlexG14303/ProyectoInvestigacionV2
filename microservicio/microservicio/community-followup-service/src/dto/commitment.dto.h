#ifndef COMMITMENT_DTO_H
#define COMMITMENT_DTO_H

typedef struct CommitmentDto {
    int followup_id;
    char family_commitment[512];
    char health_team_commitment[512];
    char compliance_status[32];
} CommitmentDto;

#endif
