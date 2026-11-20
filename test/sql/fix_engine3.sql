-- Regression tests for the PR 24 review findings owned by the engine3 group.
-- Each case names its finding as R<n>.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS plx;
-- e3_err(q) runs q and returns 'SQLSTATE: message' if it fails. The message
-- leaves out the error position, which differs between PostgreSQL 13
-- (plpgsql prefixes each expression with SELECT) and later versions.
CREATE FUNCTION e3_err(q text) RETURNS text LANGUAGE plpgsql AS $h$
BEGIN
  EXECUTE q;
  RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
  RETURN SQLSTATE || ': ' || SQLERRM;
END $h$;

-- R1: deeply nested brackets transpile in linear time (the ** rewriter used
-- to scan each group to its end at every bracket, which took seconds here)
SET statement_timeout = '1s';
SET check_function_bodies = off;
DO $d$ BEGIN EXECUTE format('CREATE FUNCTION e3_r1_js() RETURNS text LANGUAGE plxjs AS %L', 'let a = 1; return ' || repeat('("x" + ', 16000) || 'a' || repeat(')', 16000) || ';'); END $d$;
DO $d$ BEGIN EXECUTE format('CREATE FUNCTION e3_r1_ts() RETURNS int LANGUAGE plxts AS %L', 'let a: number = 1; return ' || repeat('(', 32000) || 'a' || repeat(')', 32000) || ';'); END $d$;
DO $d$ BEGIN EXECUTE format('CREATE FUNCTION e3_r1_php() RETURNS int LANGUAGE plxphp AS %L', '$a = 1; return ' || repeat('[', 32000) || '$a' || repeat(']', 32000) || ';'); END $d$;
DO $d$ BEGIN EXECUTE format('CREATE FUNCTION e3_r1_py() RETURNS int LANGUAGE plxpython3 AS %L', 'a = 1' || chr(10) || 'return ' || repeat('abs(', 24000) || 'a' || repeat(')', 24000)); END $d$;
DO $d$ BEGIN EXECUTE format('CREATE FUNCTION e3_r1_rb() RETURNS int LANGUAGE plxruby AS %L', 'a = 1' || chr(10) || 'return ' || repeat('abs(', 24000) || 'a' || repeat(')', 24000)); END $d$;
RESET check_function_bodies;
RESET statement_timeout;
-- the ** inside a group is still rewritten
CREATE FUNCTION e3_r1_pow() RETURNS int LANGUAGE plxjs AS $$
return abs(((2 ** 3 ** 2)) - 0) + (1 + (2 ** 2));
$$;
SELECT e3_r1_pow();
