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

-- A4: plx_strbuild has binary I/O and a collation, like text
CREATE TABLE a4_sb(v plx_strbuild);
INSERT INTO a4_sb VALUES ('b'), ('a'), ('c'), ('a');
SELECT DISTINCT v FROM a4_sb ORDER BY v;
SELECT v, count(*) FROM a4_sb GROUP BY v ORDER BY v;
SELECT 'B'::plx_strbuild < 'a'::plx_strbuild COLLATE "C" AS c_order;
-- binary COPY writes and reads the type
SELECT '/tmp/plx_fix_core_a4_' || pg_backend_pid() || '.bin' AS a4_file \gset
COPY a4_sb TO :'a4_file' (FORMAT binary);
CREATE TABLE a4_sb2(v plx_strbuild);
COPY a4_sb2 FROM :'a4_file' (FORMAT binary);
SELECT v FROM a4_sb2 ORDER BY v;

-- A3: appending a NULL makes the string NULL, as || does (except plxcobol)
CREATE FUNCTION a3_rb(x text) RETURNS text LANGUAGE plxruby AS $$
s = "a" #:: text
s << x
s << "b"
return s
$$;
CREATE FUNCTION a3_rb_nullacc(x text) RETURNS text LANGUAGE plxruby AS $$
s = x #:: text
s << "b"
return s
$$;
CREATE FUNCTION a3_rb_coalesce(x text) RETURNS text LANGUAGE plxruby AS $$
s = "a" #:: text
s << coalesce(x, '')
s << "b"
return s
$$;
SELECT a3_rb(NULL) IS NULL AS rb_null, a3_rb('X') AS rb,
       a3_rb_nullacc(NULL) IS NULL AS rb_nullacc_null, a3_rb_nullacc('X') AS rb_nullacc,
       a3_rb_coalesce(NULL) AS rb_coalesce;
CREATE FUNCTION a3_php(x text) RETURNS text LANGUAGE plxphp AS $$
$s = "a" /*:: text */;
$s .= $x;
$s .= "b";
return $s;
$$;
-- with interpolation, the whole string becomes NULL, not just one segment
CREATE FUNCTION a3_php_seg(x text) RETURNS text LANGUAGE plxphp AS $$
$s = "" /*:: text */;
$s .= "1:{$x} ";
$s .= "2:b ";
return $s;
$$;
-- a variable whose first use is .= starts as an empty string, as in PHP
CREATE FUNCTION a3_php_undecl(x text) RETURNS text LANGUAGE plxphp AS $$
$s .= "a";
$s .= $x;
return $s;
$$;
CREATE FUNCTION a3_php_loop(n int) RETURNS text LANGUAGE plxphp AS $$
for ($i = 0; $i < $n; $i++) {
  $s .= "x";
}
return $s;
$$;
SELECT a3_php(NULL) IS NULL AS php_null, a3_php('X') AS php,
       a3_php_seg(NULL) IS NULL AS seg_null, a3_php_seg('a') AS seg,
       a3_php_undecl(NULL) IS NULL AS undecl_null, a3_php_undecl('X') AS undecl,
       a3_php_loop(3) AS php_loop;
CREATE FUNCTION a3_js(x text) RETURNS text LANGUAGE plxjs AS $$
let s = "a" /*:: text */;
s += x;
s += "b";
return s;
$$;
CREATE FUNCTION a3_ts(x text) RETURNS text LANGUAGE plxts AS $$
let s: string = "a";
s += x;
s += "b";
return s;
$$;
CREATE FUNCTION a3_py(x text) RETURNS text LANGUAGE plxpython3 AS $$
s = "a" #:: text
s += x
s += "b"
return s
$$;
CREATE FUNCTION a3_py_nullacc(x text) RETURNS text LANGUAGE plxpython3 AS $$
s = x #:: text
s += "b"
return s
$$;
SELECT a3_js(NULL) IS NULL AS js_null, a3_js('X') AS js,
       a3_ts(NULL) IS NULL AS ts_null, a3_ts('X') AS ts,
       a3_py(NULL) IS NULL AS py_null, a3_py('X') AS py,
       a3_py_nullacc(NULL) IS NULL AS py_nullacc_null, a3_py_nullacc('X') AS py_nullacc;
-- plxcobol STRING-APPEND still treats a NULL as nothing to append
CREATE FUNCTION a3_cob(x text) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-S PIC X(1) VALUE "a".
PROCEDURE DIVISION.
    STRING-APPEND X TO WS-S
    STRING-APPEND "b" TO WS-S
    GOBACK RETURNING WS-S.
$$;
SELECT a3_cob(NULL) AS cob_null, a3_cob('X') AS cob;
-- the generated code calls the strict append, except in plxcobol
SELECT proname, prosrc LIKE '%plx_sb_append_strict(%' AS strict_append,
       prosrc LIKE '%plx_sb_append(%' AS plain_append,
       prosrc LIKE '%plx_strbuild := '''';%' AS starts_empty
FROM pg_proc WHERE proname IN ('a3_rb', 'a3_cob', 'a3_php_undecl')
ORDER BY proname;
-- the strict append itself
SELECT plx_sb_append_strict('a'::text::plx_strbuild, NULL)::text IS NULL AS null_suffix,
       plx_sb_append_strict(NULL, 'b')::text IS NULL AS null_builder,
       plx_sb_append_strict('a'::text::plx_strbuild, 'b')::text AS both;
-- a message built with appends and then raised shows <NULL> if a part was NULL
CREATE FUNCTION a3_rb_raise(x text) RETURNS text LANGUAGE plxruby AS $$
s = "bad: " #:: text
s << x
raise s
$$;
\set VERBOSITY terse
SELECT a3_rb_raise(NULL);
SELECT a3_rb_raise('X');
\set VERBOSITY default

-- A4: ALTER EXTENSION plx UPDATE from 2.0.0 gives the same objects as
-- CREATE EXTENSION, and a plx_strbuild column made under 2.0.0 gets the
-- collation. This drops and re-creates the extension, so it stays last.
CREATE TEMP VIEW plx_objects AS
SELECT pg_describe_object(d.classid, d.objid, 0) AS object,
  CASE d.classid
  WHEN 'pg_proc'::regclass THEN
    (SELECT row(pg_get_function_result(p.oid), p.prosrc, p.probin, p.prokind,
                p.provolatile, p.proisstrict, p.prosupport::regproc,
                p.proparallel, p.proacl)::text
     FROM pg_proc p WHERE p.oid = d.objid)
  WHEN 'pg_type'::regclass THEN
    (SELECT row(t.typlen, t.typbyval, t.typtype, t.typcategory,
                t.typinput::regproc, t.typoutput::regproc,
                t.typreceive::regproc, t.typsend::regproc, t.typcollation,
                t.typstorage, t.typalign, a.typcollation,
                a.typreceive::regproc, a.typsend::regproc)::text
     FROM pg_type t LEFT JOIN pg_type a ON a.oid = t.typarray
     WHERE t.oid = d.objid)
  WHEN 'pg_language'::regclass THEN
    (SELECT row(l.lanpltrusted, l.lanplcallfoid::regproc,
                l.laninline::regproc, l.lanvalidator::regproc,
                l.lanacl)::text
     FROM pg_language l WHERE l.oid = d.objid)
  WHEN 'pg_cast'::regclass THEN
    (SELECT row(c.castfunc, c.castcontext, c.castmethod)::text
     FROM pg_cast c WHERE c.oid = d.objid)
  END AS detail,
  obj_description(d.objid, d.classid::regclass::name) AS comment
FROM pg_depend d
WHERE d.refclassid = 'pg_extension'::regclass AND d.deptype = 'e'
  AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname = 'plx');
CREATE TEMP TABLE plx_fresh AS SELECT * FROM plx_objects;
SELECT count(*) > 30 AS has_objects FROM plx_fresh;
DROP EXTENSION plx CASCADE;
CREATE EXTENSION plx VERSION '2.0.0';
CREATE TABLE a4_old(v plx_strbuild);
INSERT INTO a4_old VALUES ('b'), ('a'), ('b');
-- A4: range types over plx_strbuild made under 2.0.0 get the collation too
CREATE TYPE a4_rng AS RANGE (subtype = plx_strbuild);
CREATE DOMAIN a4_rdom AS plx_strbuild;
CREATE TYPE a4_drng AS RANGE (subtype = a4_rdom);
ALTER EXTENSION plx UPDATE TO '3.0.0';
-- objects only a fresh install has, then objects only the update has
SELECT * FROM plx_fresh EXCEPT SELECT * FROM plx_objects;
SELECT * FROM plx_objects EXCEPT SELECT * FROM plx_fresh;
SELECT DISTINCT v FROM a4_old ORDER BY v;
SELECT rngtypid::regtype, rngcollation FROM pg_range
WHERE rngtypid IN ('a4_rng'::regtype, 'a4_drng'::regtype) ORDER BY rngtypid::regtype::text;
SELECT a4_rng('a', 'c') @> 'b'::plx_strbuild AS rng_has,
       a4_drng('a', 'c') @> 'b'::a4_rdom AS drng_has;
