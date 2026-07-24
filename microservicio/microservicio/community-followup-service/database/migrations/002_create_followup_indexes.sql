CREATE INDEX IF NOT EXISTS idx_followup_family_id ON followup_records(family_id);
CREATE INDEX IF NOT EXISTS idx_followup_risk_assessment_id ON followup_records(risk_assessment_id);
CREATE INDEX IF NOT EXISTS idx_followup_compliance_status ON followup_records(compliance_status);
CREATE INDEX IF NOT EXISTS idx_followup_evaluation_date ON followup_records(evaluation_date);
