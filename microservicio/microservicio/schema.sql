
DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_type WHERE typname = 'compliance_status_enum') THEN
        CREATE TYPE compliance_status_enum AS ENUM ('SI_CUMPLE', 'PARCIAL', 'NO_CUMPLE');
    END IF;
END$$;

CREATE TABLE IF NOT EXISTS followup_records (
    followup_id SERIAL PRIMARY KEY,
    family_id INTEGER NOT NULL,
    risk_assessment_id INTEGER,
    record_number VARCHAR(50) NOT NULL UNIQUE,
    analysis_date DATE,
    evaluation_date DATE,
    risk_description TEXT,
    scheduled_activities TEXT,
    family_commitment TEXT,
    health_team_commitment TEXT,
    responsible_staff_id INTEGER,
    compliance_status compliance_status_enum NOT NULL DEFAULT 'PARCIAL',
    noncompliance_causes TEXT,
    created_at TIMESTAMP NOT NULL DEFAULT NOW(),
    updated_at TIMESTAMP NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_followup_family_id ON followup_records(family_id);
CREATE INDEX IF NOT EXISTS idx_followup_risk_assessment_id ON followup_records(risk_assessment_id);
CREATE INDEX IF NOT EXISTS idx_followup_compliance_status ON followup_records(compliance_status);
CREATE INDEX IF NOT EXISTS idx_followup_evaluation_date ON followup_records(evaluation_date);

CREATE OR REPLACE FUNCTION set_updated_at()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_followup_records_updated_at ON followup_records;
CREATE TRIGGER trg_followup_records_updated_at
BEFORE UPDATE ON followup_records
FOR EACH ROW EXECUTE FUNCTION set_updated_at();

-- Primera fila: no se especifica compliance_status, así que toma el
-- DEFAULT ('PARCIAL') definido en la tabla. Evita repetir el literal
-- una tercera vez en el archivo.
INSERT INTO followup_records (
    family_id,
    risk_assessment_id,
    record_number,
    analysis_date,
    evaluation_date,
    risk_description,
    scheduled_activities,
    family_commitment,
    health_team_commitment,
    responsible_staff_id,
    noncompliance_causes
) VALUES
(1, 10, 'SEG-APS-001', CURRENT_DATE, CURRENT_DATE + INTERVAL '15 days', 'Familia con riesgo sanitario medio.', 'Visita domiciliaria y control comunitario.', 'Asistir a controles programados.', 'Realizar seguimiento APS.', 101, NULL)
ON CONFLICT (record_number) DO NOTHING;

INSERT INTO followup_records (
    family_id,
    risk_assessment_id,
    record_number,
    analysis_date,
    evaluation_date,
    risk_description,
    scheduled_activities,
    family_commitment,
    health_team_commitment,
    responsible_staff_id,
    compliance_status,
    noncompliance_causes
) VALUES
(2, 11, 'SEG-APS-002', CURRENT_DATE, CURRENT_DATE + INTERVAL '7 days', 'Familia con riesgo alto por incumplimiento de controles.', 'Reagendar visita, verificar barreras de acceso.', 'Actualizar datos y cumplir cita.', 'Emitir alerta de seguimiento.', 102, 'NO_CUMPLE', 'No asistió a la actividad programada.')
ON CONFLICT (record_number) DO NOTHING;

-- Rol anónimo usado por PostgREST para el laboratorio.
DO $$
BEGIN
    CREATE ROLE web_anon NOLOGIN;
EXCEPTION WHEN duplicate_object THEN
    RAISE NOTICE 'Role web_anon ya existe, omitiendo.';
END$$;

GRANT USAGE ON SCHEMA public TO web_anon;
GRANT SELECT, INSERT, UPDATE, DELETE ON followup_records TO web_anon;
GRANT ALL ON SEQUENCE followup_records_followup_id_seq TO web_anon;
