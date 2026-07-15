-- Dynamic SQL and an explicit cursor.
-- Dynamic aggregate in plxphp; cursor walk in plxplsql (Oracle PL/SQL).
--   psql -f examples/03_dynamic_and_cursor.sql
CREATE EXTENSION IF NOT EXISTS plx;

CREATE TABLE IF NOT EXISTS sale (region text, amount numeric);
TRUNCATE sale;
INSERT INTO sale VALUES ('east', 10), ('east', 20), ('west', 5), ('west', 7);

-- Dynamic SQL with a bind parameter, executed for its effect.
CREATE OR REPLACE FUNCTION bump(region text, by numeric) RETURNS void LANGUAGE plxphp AS $$
execute("UPDATE sale SET amount = amount + $1 WHERE region = $2", $by, $region);
$$;
SELECT bump('west', 3);   -- west rows become 8 and 10

-- Fetch a single aggregate. The parameter is named distinctly from the column
-- to avoid an ambiguous reference in the query.
CREATE OR REPLACE FUNCTION region_total(reg text) RETURNS numeric LANGUAGE plxphp AS $$
$r = fetch_one("SELECT sum(amount) AS s FROM sale WHERE region = {$reg}");
return $r->s;
$$;
SELECT region_total('east') AS east_30, region_total('west') AS west_18;

-- Explicit cursor in Oracle PL/SQL syntax.
CREATE OR REPLACE FUNCTION top_region() RETURNS text LANGUAGE plxplsql AS $$
  CURSOR c IS
    SELECT region, sum(amount) AS total
    FROM sale GROUP BY region ORDER BY total DESC;
  r record;
  best text;
BEGIN
  OPEN c;
  FETCH c INTO r;         -- first row is the largest
  best := r.region;
  CLOSE c;
  RETURN best;
END;
$$;
SELECT top_region() AS should_be_east;
