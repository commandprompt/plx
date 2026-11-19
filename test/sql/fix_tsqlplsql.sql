-- Regression tests for bugs found by the 2026-11-19 audit (fix_tsqlplsql).
-- Each case names its audit finding (A<n>).
\set VERBOSITY terse
-- A42: PL/SQL SELECT INTO and EXECUTE IMMEDIATE INTO raise NO_DATA_FOUND and TOO_MANY_ROWS
CREATE TABLE fx42_amt(v int);
INSERT INTO fx42_amt VALUES (1), (2), (3);
CREATE FUNCTION fx42_sel(lim int) RETURNS int LANGUAGE plxplsql AS $$
  x NUMBER;
BEGIN
  SELECT v INTO x FROM fx42_amt WHERE v > lim;
  RETURN x;
EXCEPTION
  WHEN NO_DATA_FOUND THEN
    RETURN -1;
  WHEN TOO_MANY_ROWS THEN
    RETURN -2;
END;
$$;
SELECT fx42_sel(2) AS one, fx42_sel(100) AS none, fx42_sel(0) AS many;
CREATE FUNCTION fx42_dyn(lim int) RETURNS int LANGUAGE plxplsql AS $$
  x NUMBER;
BEGIN
  EXECUTE IMMEDIATE 'SELECT v FROM fx42_amt WHERE v > $1' INTO x USING lim;
  RETURN x;
EXCEPTION
  WHEN NO_DATA_FOUND THEN
    RETURN -1;
  WHEN TOO_MANY_ROWS THEN
    RETURN -2;
END;
$$;
SELECT fx42_dyn(2) AS one, fx42_dyn(100) AS none, fx42_dyn(0) AS many;
CREATE FUNCTION fx42_case(lim int) RETURNS text LANGUAGE plxplsql AS $$
  s VARCHAR2(10);
BEGIN
  IF lim >= 0 THEN
    SELECT CASE WHEN v > 2 THEN 'big' ELSE 'small' END INTO s FROM fx42_amt WHERE v > lim;
  END IF;
  RETURN s;
EXCEPTION
  WHEN TOO_MANY_ROWS THEN
    RETURN 'many';
END;
$$;
SELECT fx42_case(2) AS one, fx42_case(0) AS many;
