-- Audit trigger: stamp a row's updated_at and bump a version counter.
-- Shown in plxruby; every dialect can write a trigger the same way.
--   psql -f examples/01_audit_trigger.sql
CREATE EXTENSION IF NOT EXISTS plx;

CREATE TABLE IF NOT EXISTS account (
    id         int PRIMARY KEY,
    balance    numeric NOT NULL,
    version    int NOT NULL DEFAULT 0,
    updated_at timestamptz
);

CREATE OR REPLACE FUNCTION account_stamp() RETURNS trigger LANGUAGE plxruby AS $$
NEW.version = OLD.version + 1
NEW.updated_at = clock_timestamp()
return NEW
$$;

DROP TRIGGER IF EXISTS account_stamp ON account;
CREATE TRIGGER account_stamp
    BEFORE UPDATE ON account
    FOR EACH ROW EXECUTE FUNCTION account_stamp();

INSERT INTO account(id, balance) VALUES (1, 100) ON CONFLICT DO NOTHING;
UPDATE account SET balance = balance + 50 WHERE id = 1;
UPDATE account SET balance = balance - 20 WHERE id = 1;

SELECT id, balance, version FROM account WHERE id = 1;   -- version = 2
