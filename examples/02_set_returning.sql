-- Set-returning function: expand a period into its month boundaries.
-- Shown in plxpython3 (RETURNS TABLE) and plxts (RETURNS SETOF).
--   psql -f examples/02_set_returning.sql
CREATE EXTENSION IF NOT EXISTS plx;

-- RETURNS TABLE: the columns are OUT parameters, emit each row with return_next
CREATE OR REPLACE FUNCTION month_starts(from_year int, n int)
RETURNS TABLE(idx int, month_start date) LANGUAGE plxpython3 AS $$
for i in range(0, n):
    idx = i + 1
    month_start = make_date(from_year, 1, 1) + (i || ' months')::interval
    return_next
return
$$;

SELECT * FROM month_starts(2026, 3);

-- RETURNS SETOF <scalar>: return_next takes the value
CREATE OR REPLACE FUNCTION squares(n int) RETURNS SETOF int LANGUAGE plxts AS $$
for (let i: number = 1; i <= n; i++) {
  return_next(i * i);
}
return;
$$;

SELECT array_agg(x) FROM squares(5) x;   -- {1,4,9,16,25}
