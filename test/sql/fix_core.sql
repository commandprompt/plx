-- Regression tests for bugs found by the 2026-11-19 audit (fix_core).
-- Each case names its audit finding (A<n>).
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS plx;
CREATE SCHEMA fc;
SET search_path = fc, public;
-- The CREATE-time errors below come from plpgsql's compiler, whose context
-- lines differ between PostgreSQL majors, so show only the message.
\set VERBOSITY terse

-- A0: CREATE FUNCTION runs plpgsql's signature checks and compiles the body
-- a pseudo-type result is rejected
CREATE FUNCTION a0_cstring(x int) RETURNS cstring LANGUAGE plxruby AS $$
return nil
$$;
-- a pseudo-type argument is rejected
CREATE FUNCTION a0_internal(x internal) RETURNS int LANGUAGE plxjs AS $$
return 1;
$$;
-- a body that plpgsql cannot parse is rejected
CREATE FUNCTION a0_syntax() RETURNS int LANGUAGE plxruby AS $$
return 1 2
$$;
-- a trigger function cannot declare arguments
CREATE FUNCTION a0_trg_args(a int) RETURNS trigger LANGUAGE plxpython3 AS $$
return None
$$;
-- none of the rejected functions was stored
SELECT count(*) FROM pg_proc WHERE proname LIKE 'a0\_%';
-- the pseudo-types plpgsql allows are still accepted
CREATE FUNCTION a0_trg() RETURNS trigger LANGUAGE plxruby AS $$
return nil
$$;
CREATE FUNCTION a0_poly(x anyelement) RETURNS anyelement LANGUAGE plxruby AS $$
return x
$$;
CREATE FUNCTION a0_void() RETURNS void LANGUAGE plxphp AS $$
return;
$$;
SELECT a0_poly(41 + 1), a0_void();
-- with check_function_bodies off (as pg_restore runs), the body is not
-- compiled, so a bad body and trigger arguments pass as they do in plpgsql;
-- the signature checks still run
SET check_function_bodies = off;
CREATE FUNCTION a0_syntax() RETURNS int LANGUAGE plxruby AS $$
return 1 2
$$;
CREATE FUNCTION a0_trg_args(a int) RETURNS trigger LANGUAGE plxpython3 AS $$
return None
$$;
CREATE FUNCTION a0_cstring(x int) RETURNS cstring LANGUAGE plxruby AS $$
return nil
$$;
RESET check_function_bodies;
SELECT a0_syntax();
\set VERBOSITY default
