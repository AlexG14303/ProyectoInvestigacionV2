INSERT INTO followup_records (family_id, risk_assessment_id, record_number, analysis_date, evaluation_date, risk_description, scheduled_activities, family_commitment, health_team_commitment, responsible_staff_id, compliance_status, noncompliance_causes)
VALUES (3, 12, 'SEG-APS-003', CURRENT_DATE, CURRENT_DATE + INTERVAL '10 days', 'Seguimiento preventivo comunitario.', 'Control de compromiso familiar.', 'Mantener contacto con equipo APS.', 'Registrar evolución longitudinal.', 103, 'SI_CUMPLE', NULL)
ON CONFLICT (record_number) DO NOTHING;
