-- plx cross-cutting feature tests: things not tied to one dialect's syntax
-- (OUT params, triggers, record-field assignment, idempotency, string escaping,
-- semantic edge cases, and DO blocks in every dialect).
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = notice;

-- OUT parameters
CREATE FUNCTION f_out(a int, b int, OUT sum int, OUT diff int) LANGUAGE plxruby AS $$
sum = a + b
diff = a - b
$$;
SELECT * FROM f_out(10, 3);

-- trigger function assigning to NEW.field (plxruby)
CREATE TABLE f_trg(id int, tag text);
CREATE FUNCTION f_trg_fn() RETURNS trigger LANGUAGE plxruby AS $$
NEW.tag = "set-#{NEW.id}"
return NEW
$$;
CREATE TRIGGER f_tr BEFORE INSERT ON f_trg FOR EACH ROW EXECUTE FUNCTION f_trg_fn();
INSERT INTO f_trg(id) VALUES (7);
SELECT tag FROM f_trg;

-- trigger function in plxpython3
CREATE TABLE f_trg2(id int, tag text);
CREATE FUNCTION f_trg_py() RETURNS trigger LANGUAGE plxpython3 AS $$
NEW.tag = f"py-{NEW.id}"
return NEW
$$;
CREATE TRIGGER f_tr2 BEFORE INSERT ON f_trg2 FOR EACH ROW EXECUTE FUNCTION f_trg_py();
INSERT INTO f_trg2(id) VALUES (9);
SELECT tag FROM f_trg2;

-- CREATE OR REPLACE is idempotent and re-transpiles the new body
CREATE FUNCTION f_idem() RETURNS int LANGUAGE plxruby AS $$ return 1 $$;
CREATE OR REPLACE FUNCTION f_idem() RETURNS int LANGUAGE plxruby AS $$ return 2 $$;
SELECT f_idem();
SELECT prosrc ~ '^/[*]plx:v1:' AS has_sentinel FROM pg_proc WHERE proname = 'f_idem';

-- string escaping: apostrophe inside interpolation
CREATE FUNCTION f_apos(n int) RETURNS text LANGUAGE plxruby AS $$
return "it's #{n} o'clock"
$$;
SELECT f_apos(9);

-- escape sequences lower to an E-string
CREATE FUNCTION f_esc() RETURNS boolean LANGUAGE plxruby AS $$
return "a\tb\nc" = E'a\tb\nc'
$$;
SELECT f_esc();

-- literal percent in a RAISE message is preserved
CREATE FUNCTION f_pct() RETURNS void LANGUAGE plxruby AS $$
raise notice: "50% done for #{1 + 1} items"
$$;
SELECT f_pct();

-- exclusive range yields the exact element set (built via interpolation, since
-- + stays numeric)
CREATE FUNCTION f_excl() RETURNS text LANGUAGE plxruby AS $$
out = "" #:: text
for i in 1...4
  out = "#{out}#{i}"
end
return out
$$;
SELECT f_excl();

-- SQL three-valued equality: == maps to plain = (not null-aware), so a
-- comparison involving NULL is unknown and the condition is not taken.
CREATE FUNCTION f_null(a int, b int) RETURNS text LANGUAGE plxruby AS $$
if a == b
  return "eq"
end
return "ne"
$$;
SELECT f_null(NULL, NULL) AS null_vs_null_is_ne,
       f_null(1, NULL) AS val_vs_null_is_ne,
       f_null(2, 2) AS eq;
-- the literal-nil form is the only null special case
CREATE FUNCTION f_isnil(a int) RETURNS text LANGUAGE plxruby AS $$
return a == nil ? "nil" : "notnil"
$$;
SELECT f_isnil(NULL) AS nil, f_isnil(5) AS notnil;

-- DO blocks in every dialect
DO LANGUAGE plxruby $$ raise notice: "ruby do #{2 * 3}" $$;
DO LANGUAGE plxphp $$ raise('notice', 'php do ' . (2 * 3)); $$;
DO LANGUAGE plxjs $$ raise("notice", `js do ${2 * 3}`); $$;
DO LANGUAGE plxpython3 $$
raise('notice', f'py do {2 * 3}')
$$;

-- interpolating a NULL propagates it, so the whole value is NULL in every
-- dialect, the way SQL || behaves
CREATE FUNCTION f_interp_rb(x int) RETURNS text LANGUAGE plxruby AS $$ return "[#{x}]" $$;
CREATE FUNCTION f_interp_php(x int) RETURNS text LANGUAGE plxphp AS $$ return "[{$x}]"; $$;
CREATE FUNCTION f_interp_js(x int) RETURNS text LANGUAGE plxjs AS $$ return `[${x}]`; $$;
CREATE FUNCTION f_interp_py(x int) RETURNS text LANGUAGE plxpython3 AS $$ return f"[{x}]" $$;
SELECT f_interp_rb(NULL) IS NULL AS rb, f_interp_php(NULL) IS NULL AS php,
       f_interp_js(NULL) IS NULL AS js, f_interp_py(NULL) IS NULL AS py;
SELECT f_interp_rb(7) AS rb7;

-- but a message built for RAISE keeps each value as an empty string, so one
-- NULL cannot swallow the text the message was written to carry
CREATE FUNCTION f_msg_rb(x int) RETURNS void LANGUAGE plxruby AS $$
raise notice: "value is [#{x}] here"
$$;
CREATE FUNCTION f_msg_py(x int) RETURNS void LANGUAGE plxpython3 AS $$
raise('notice', f'value is [{x}] here')
$$;
SELECT f_msg_rb(7), f_msg_rb(NULL);
SELECT f_msg_py(7), f_msg_py(NULL);

-- OUT / INOUT parameters (audit): supported across dialects
CREATE FUNCTION f_out(a int, OUT b int, OUT c int) LANGUAGE plxruby AS $$
b = a * 2
c = a + 1
$$;
SELECT b, c FROM f_out(5);
CREATE FUNCTION f_inout(INOUT x int) LANGUAGE plxphp AS $$ $x = $x + 100; $$;
SELECT f_inout(1);

-- RETURNS TABLE: the table columns are OUT parameters, so emit a row with a bare
-- emit / return_next (an argument form is rejected by plpgsql here)
CREATE FUNCTION f_table(n int) RETURNS TABLE(k int, sq int) LANGUAGE plxruby AS $$
for i in 1..n
  k = i
  sq = i * i
  emit
end
return
$$;
SELECT string_agg(k || ':' || sq, ',') FROM f_table(3);

-- SELECT INTO with fetch_one! is STRICT (raises when no row is returned)
CREATE FUNCTION f_strict() RETURNS int LANGUAGE plxruby AS $$
begin
  r = fetch_one!("SELECT 1 AS a WHERE false")
  return r.a
rescue => e
  return -1
end
$$;
SELECT f_strict() AS strict_no_row;

-- a literal with a backslash reads the same whatever standard_conforming_strings
-- the caller uses: generated code writes it as E'...', so with the setting off
-- the backslash cannot swallow the closing quote and run the next literal as code
CREATE FUNCTION lit_go() RETURNS text LANGUAGE plxgo AS $P$
a := "C:\\"
b := "; RAISE EXCEPTION $$go: injected$$; --"
return a
$P$;
CREATE FUNCTION lit_rb() RETURNS text LANGUAGE plxruby AS $P$
a = 'C:\\'
b = "; RAISE EXCEPTION $$rb: injected$$; --"
return a
$P$;
CREATE FUNCTION lit_php() RETURNS text LANGUAGE plxphp AS $P$
$a = 'C:\\';
$b = '; RAISE EXCEPTION $$php: injected$$; --';
return $a;
$P$;
CREATE FUNCTION lit_tsql() RETURNS varchar LANGUAGE plxtsql AS $P$
DECLARE @a varchar(10) = 'C:\';
DECLARE @b varchar(80) = '; RAISE EXCEPTION $$tsql: injected$$; --';
RETURN @a;
$P$;
CREATE FUNCTION lit_plsql() RETURNS text LANGUAGE plxplsql AS $P$
DECLARE a text := 'C:\'; b text := '; RAISE EXCEPTION $$plsql: injected$$; --';
BEGIN RETURN a; END;
$P$;
CREATE FUNCTION lit_cob() RETURNS text LANGUAGE plxcobol AS $P$
DATA DIVISION.
WORKING-STORAGE SECTION.
01 A PIC X(10) VALUE "C:\".
01 B PIC X(60) VALUE "; RAISE EXCEPTION $$cob: injected$$; --".
PROCEDURE DIVISION.
    RETURN A.
$P$;
SET standard_conforming_strings = off;
SELECT lit_go(), lit_rb(), lit_php(), lit_tsql(), lit_plsql(), lit_cob();
RESET standard_conforming_strings;
SELECT lit_go(), lit_rb(), lit_php(), lit_tsql(), lit_plsql(), lit_cob();

-- interpolation in a SQL argument: a value is bound (or quoted), never spliced
-- as SQL text, wherever it sits; ident() quotes identifiers, literal() and raw()
-- splice on request
CREATE TABLE tpl_t (id int, name text);
INSERT INTO tpl_t VALUES (1, 'alice'), (2, 'bob');
CREATE TABLE "tpl Odd" (id int);
INSERT INTO "tpl Odd" VALUES (7);
CREATE TABLE tpl_log (m text);

-- a value round-trips exactly in each position, in a planned statement ($n)
-- and in a utility statement (quote_nullable), and nothing it contains runs
CREATE FUNCTION tpl_roundtrip(v text) RETURNS text LANGUAGE plxjs AS $$
  execute(`DELETE FROM tpl_log`);
  execute(`INSERT INTO tpl_log VALUES (${v})`);
  execute(`INSERT INTO tpl_log VALUES ('${v}')`);
  execute(`INSERT INTO tpl_log VALUES ('<${v}>')`);
  execute(`COMMENT ON TABLE tpl_log IS '${v}'`);
  let r = fetch_one(`SELECT count(*) AS c FROM tpl_log
                     WHERE m = $1 OR m = '<' || $1 || '>'`, v);
  let d = fetch_one(`SELECT obj_description('tpl_log'::regclass) AS d`);
  return `${r.c}|${d.d = v}`;
$$;
SELECT v, tpl_roundtrip(v) AS "rows|comment_ok"
FROM (VALUES ($x$plain$x$), ($x$it's$x$), ($x$x'); DROP TABLE tpl_t; --$x$),
             ($x$C:\$x$), ($x$a\'b$x$), ($x$$$; RAISE EXCEPTION 'boom'; $$$x$)) AS p(v);
SET standard_conforming_strings = off;
SELECT tpl_roundtrip($x$C:\'; DROP TABLE tpl_t; --$x$) AS "rows|comment_ok (scs off)";
RESET standard_conforming_strings;
SELECT count(*) AS tpl_t_survives FROM tpl_t;

-- the generated SQL: values become $n after the call's own binds
CREATE FUNCTION tpl_fetch(uname text, lim int) RETURNS text LANGUAGE plxruby AS $$
  u = fetch_one("SELECT name FROM tpl_t WHERE name = '#{uname}' LIMIT $1", lim)
  return u.name
$$;
SELECT tpl_fetch('bob', 1), tpl_fetch($x$x' OR name = 'bob$x$, 1) IS NULL AS injection_blocked;
SELECT regexp_replace(prosrc, '^.*?BEGIN\n|END;\n/\*plx-orig.*$', '', 'gs')
FROM pg_proc WHERE proname = 'tpl_fetch';

-- static SQL: '#{x}' is the variable, not the literal text 'x'
CREATE FUNCTION tpl_static(uname text) RETURNS int LANGUAGE plxpython3 AS $$
u = fetch_one(f"SELECT id FROM tpl_t WHERE name = '{uname}'")
return u.id
$$;
SELECT tpl_static('bob');

-- ident() quotes an identifier (and makes the call dynamic); PHP spells it
-- {ident($t)}
CREATE FUNCTION tpl_ident(t text, minid int) RETURNS bigint LANGUAGE plxphp AS $$
  $r = fetch_one("SELECT count(*) AS c FROM {ident($t)} WHERE id >= {$minid}");
  return $r->c;
$$;
SELECT tpl_ident('tpl Odd', 7), tpl_ident('tpl_t', 2);

-- raw() splices deliberately
CREATE FUNCTION tpl_raw(ord text) RETURNS text LANGUAGE plxruby AS $$
  s = ""
  query("SELECT name FROM tpl_t ORDER BY #{raw(ord)}").each do |r|
    s << r.name
  end
  return s
$$;
SELECT tpl_raw('name DESC');

-- rejected: an interpolation inside a quoted identifier, ident() inside a string
DO $d$ BEGIN
  EXECUTE $q$CREATE FUNCTION tpl_bad1(t text) RETURNS void LANGUAGE plxjs AS $b$ execute(`SELECT * FROM "${t}"`); $b$$q$;
EXCEPTION WHEN OTHERS THEN RAISE WARNING 'caught: %', SQLERRM; END $d$;
DO $d$ BEGIN
  EXECUTE $q$CREATE FUNCTION tpl_bad2(t text) RETURNS void LANGUAGE plxjs AS $b$ execute(`SELECT '${ident(t)}'`); $b$$q$;
EXCEPTION WHEN OTHERS THEN RAISE WARNING 'caught: %', SQLERRM; END $d$;
DO $d$ BEGIN
  EXECUTE $q$CREATE FUNCTION tpl_bad3(c text) RETURNS void LANGUAGE plxjs AS $b$ execute(`COMMENT ON TABLE tpl_t IS 'x ${c}'`); $b$$q$;
EXCEPTION WHEN OTHERS THEN RAISE WARNING 'caught: %', SQLERRM; END $d$;
DROP TABLE tpl_t, "tpl Odd", tpl_log;
