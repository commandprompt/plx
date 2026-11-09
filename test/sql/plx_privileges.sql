-- plx languages execute as plpgsql, so they follow USAGE on plpgsql
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;
DROP ROLE IF EXISTS regress_plx_user;
CREATE ROLE regress_plx_user;
GRANT CREATE ON SCHEMA public TO regress_plx_user;

-- with USAGE on plpgsql (the default), a plain role can use plx
SET ROLE regress_plx_user;
CREATE FUNCTION priv_ok() RETURNS int LANGUAGE plxruby AS $$ return 1 $$;
SELECT priv_ok();
DO LANGUAGE plxjs $$ let x = 1; $$;
RESET ROLE;

-- revoking plpgsql revokes plx: CREATE FUNCTION and DO both fail
REVOKE USAGE ON LANGUAGE plpgsql FROM PUBLIC;
SET ROLE regress_plx_user;
CREATE FUNCTION priv_denied() RETURNS int LANGUAGE plxruby AS $$ return 1 $$;
DO LANGUAGE plxplsql $$ BEGIN RAISE NOTICE 'ran'; END; $$;
-- a body that already carries the transpiled header is gated too
CREATE FUNCTION priv_sentinel() RETURNS int LANGUAGE plxjs AS $$/*plx:v1:plxjs:00000000*/
BEGIN RETURN 1; END;
$$;
-- an existing function keeps running, as an existing plpgsql function does
SELECT priv_ok();
RESET ROLE;

-- superusers are unaffected
DO LANGUAGE plxjs $$ let x = 1; $$;

GRANT USAGE ON LANGUAGE plpgsql TO PUBLIC;
DROP FUNCTION priv_ok();
REVOKE CREATE ON SCHEMA public FROM regress_plx_user;
DROP ROLE regress_plx_user;
