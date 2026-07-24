CREATE TABLE IF NOT EXISTS compliance_rules (
    rule_id SERIAL PRIMARY KEY,
    rule_code VARCHAR(40) UNIQUE NOT NULL,
    description TEXT NOT NULL,
    active BOOLEAN NOT NULL DEFAULT TRUE
);
