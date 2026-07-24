INSERT INTO compliance_rules(rule_code, description) VALUES
('FOLLOWUP_REQUIRED', 'Todo seguimiento APS debe registrar estado de cumplimiento'),
('NO_CUMPLE_CAUSE', 'Cuando el estado es NO_CUMPLE debe existir una causa')
ON CONFLICT (rule_code) DO NOTHING;
