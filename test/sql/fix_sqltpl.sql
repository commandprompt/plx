-- Regression tests for the SQL template scanner: how an interpolation in the
-- SQL argument of execute, fetch_one, query and friends is lowered.
-- Each case names its PR 24 review finding ([n]).
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;
-- fs_err(q) runs q and returns 'SQLSTATE: message' if it fails. The message
-- leaves out the error position, which differs between PostgreSQL 13
-- (plpgsql prefixes each expression with SELECT) and later versions.
CREATE FUNCTION fs_err(q text) RETURNS text LANGUAGE plpgsql AS $h$
BEGIN
  EXECUTE q;
  RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
  RETURN SQLSTATE || ': ' || SQLERRM;
END $h$;
CREATE TABLE fs_victim (id int);

-- [2]: an apostrophe inside a SQL comment does not open a string, so the
-- quoted literal after it is still seen as quoted and the value stays data
CREATE FUNCTION fs_c_block(c text) RETURNS text LANGUAGE plxruby AS $$
execute("COMMENT /* the owner's note */ ON TABLE fs_victim IS '#{c}'")
d = fetch_one("SELECT obj_description('fs_victim'::regclass) AS d")
return d.d
$$;
SELECT fs_c_block('hello');
SELECT fs_c_block('; DROP TABLE fs_victim; --');
SELECT to_regclass('fs_victim') IS NOT NULL AS victim_survives;
CREATE FUNCTION fs_c_line(c text) RETURNS text LANGUAGE plxruby AS $$
execute("COMMENT -- the owner's note\nON TABLE fs_victim IS '#{c}'")
d = fetch_one("SELECT obj_description('fs_victim'::regclass) AS d")
return d.d
$$;
SELECT fs_c_line('; DROP TABLE fs_victim; --');
SELECT to_regclass('fs_victim') IS NOT NULL AS victim_survives;
-- [2]: block comments nest
CREATE FUNCTION fs_c_nested(c text) RETURNS text LANGUAGE plxjs AS $$
execute(`COMMENT /* outer /* it's */ still's */ ON TABLE fs_victim IS '${c}'`);
let d = fetch_one(`SELECT obj_description('fs_victim'::regclass) AS d`);
return d.d;
$$;
SELECT fs_c_nested('; DROP TABLE fs_victim; --');
SELECT to_regclass('fs_victim') IS NOT NULL AS victim_survives;
-- [2]: static SQL reads the value, not the variable's name
CREATE FUNCTION fs_c_static(x text) RETURNS text LANGUAGE plxjs AS $$
let r = fetch_one(`SELECT /* the user's tag */ '${x}' AS v`);
return r.v;
$$;
SELECT fs_c_static('hello');
CREATE FUNCTION fs_c_static_line(x text) RETURNS text LANGUAGE plxjs AS $$
let r = fetch_one(`SELECT 'a' -- it's
  || '${x}' AS v`);
return r.v;
$$;
SELECT fs_c_static_line('hello');
-- [2]: an interpolation inside a SQL comment is rejected when the function is
-- created: it would silently do nothing
CREATE FUNCTION fs_c_in_block(x text) RETURNS text LANGUAGE plxjs AS $$
let r = fetch_one(`SELECT 1 AS v /* ${x} */`);
return r.v;
$$;
CREATE FUNCTION fs_c_in_line(c text) RETURNS void LANGUAGE plxruby AS $$
execute("COMMENT ON TABLE fs_victim IS 'x' -- #{c}\n")
$$;

-- [4]: a dollar-quoted string is skipped as a whole, so quotes inside it do
-- not count, and a $ inside a name or a $n parameter does not start one
CREATE FUNCTION fs_d_skip(x text) RETURNS text LANGUAGE plxjs AS $f$
let r = fetch_one(`SELECT $q$it's$q$ || '${x}' AS v`);
return r.v;
$f$;
SELECT fs_d_skip('hello');
CREATE FUNCTION fs_d_skip_dyn(x text) RETURNS text LANGUAGE plxjs AS $f$
let r = fetch_one(`SELECT $$it's$$ || '${x}' || $1 AS v`, '!');
return r.v;
$f$;
SELECT fs_d_skip_dyn('hello');
CREATE FUNCTION fs_d_name(x text) RETURNS text LANGUAGE plxjs AS $f$
let r = fetch_one(`SELECT v FROM (SELECT 1 AS a$q$, '${x}' AS v) AS s`);
return r.v;
$f$;
SELECT fs_d_name('hello');
-- [4]: an interpolation inside a dollar-quoted string is rejected: a $tag$ in
-- the value would end the string, and the value would run as SQL
CREATE FUNCTION fs_d_dyn(t text, c text) RETURNS void LANGUAGE plxjs AS $f$
execute(`COMMENT ON TABLE ${ident(t)} IS $c$${c}$c$`);
$f$;
CREATE FUNCTION fs_d_dyn_rb(t text, c text) RETURNS void LANGUAGE plxruby AS $f$
execute("COMMENT ON TABLE #{ident(t)} IS $c$ #{c} $c$")
$f$;
CREATE FUNCTION fs_d_static(s text) RETURNS text LANGUAGE plxjs AS $f$
let r = fetch_one(`SELECT $q$ ${s} $q$ AS c`);
return r.c;
$f$;
CREATE FUNCTION fs_d_py(x text) RETURNS void LANGUAGE plxpython3 AS $f$
execute(f"SELECT $q$ {x} $q$ AS v")
$f$;
-- [4]: E'' strings with backslash escapes, U&'' strings, doubled quotes and
-- quoted identifiers are each read as one token
CREATE FUNCTION fs_l_forms(x text) RETURNS text LANGUAGE plxruby AS $f$
r = fetch_one("SELECT E'it\\'s' || U&'d\\0061t\\+000061' || 'it''s' || '#{x}' AS \"it's\"")
return r["it's"]
$f$;
SELECT fs_l_forms('hello');
-- [4]: an interpolation inside a U&'', B'', X'', N'' or typed literal is
-- rejected: the value cannot be spliced into such a literal
CREATE FUNCTION fs_l_uesc(x text) RETURNS text LANGUAGE plxruby AS $f$
r = fetch_one("SELECT U&'\\0041#{x}' AS v")
return r.v
$f$;
CREATE FUNCTION fs_l_nchar(x text) RETURNS text LANGUAGE plxjs AS $f$
let r = fetch_one(`SELECT N'${x}' AS v`);
return r.v;
$f$;
CREATE FUNCTION fs_l_typed(x text) RETURNS text LANGUAGE plxjs AS $f$
let r = fetch_one(`SELECT text'a${x}' AS v`);
return r.v;
$f$;
-- [4]: an interpolation joined to a SQL name, number or $ sign is rejected:
-- it would change the name ($2 in v$2 is part of the name) or form a $tag$
CREATE FUNCTION fs_a_name(x text) RETURNS text LANGUAGE plxjs AS $f$
let r = fetch_one(`SELECT count${x} AS v`);
return r.v;
$f$;
CREATE FUNCTION fs_a_after(x text) RETURNS text LANGUAGE plxruby AS $f$
r = fetch_one("SELECT #{x}abc FROM fs_victim WHERE 1 = $1", 1)
return r.abc
$f$;
CREATE FUNCTION fs_a_dollar(t text) RETURNS void LANGUAGE plxjs AS $f$
execute(`SELECT $${ident(t)}$ AS v`);
$f$;
-- [4]: with standard_conforming_strings off (PostgreSQL 13 to 18), a literal
-- in dynamic SQL that ends in a backslash still ends there, so the value
-- after it stays data. fs_scs_off() turns the setting off where the server
-- allows it; 19 and later reject the setting and the cases run with it on.
CREATE PROCEDURE fs_scs_off() LANGUAGE plpgsql AS $h$
BEGIN
  PERFORM set_config('standard_conforming_strings', 'off', false);
EXCEPTION WHEN OTHERS THEN NULL;
END $h$;
CREATE FUNCTION fs_scs_note(c text) RETURNS text LANGUAGE plxruby AS $f$
execute("COMMENT ON TABLE fs_victim IS 'C:\\'\n'#{c}'")
d = fetch_one("SELECT obj_description('fs_victim'::regclass) AS d")
return d.d
$f$;
CREATE FUNCTION fs_scs_bound(x text) RETURNS text LANGUAGE plxruby AS $f$
r = fetch_one("SELECT 'C:\\' || '#{x}' || $1 AS v", '!')
return r.v
$f$;
CALL fs_scs_off();
SELECT fs_scs_note('; DROP TABLE fs_victim; --');
SELECT fs_scs_bound('hello');
RESET standard_conforming_strings;
SELECT to_regclass('fs_victim') IS NOT NULL AS victim_survives;
SELECT fs_scs_note('hello');
-- [4]: a -- comment ends at a carriage return as well as at a newline
CREATE FUNCTION fs_c_cr(c text) RETURNS text LANGUAGE plxruby AS $f$
execute("COMMENT ON TABLE fs_victim IS -- note\r'#{c}'")
d = fetch_one("SELECT obj_description('fs_victim'::regclass) AS d")
return d.d
$f$;
SELECT fs_c_cr('after a carriage return');

-- [3]: typed literals (text'...') and N'...' literals are ordinary strings to
-- PostgreSQL, so they too are written as E'...' when they hold a backslash.
-- With standard_conforming_strings off, the backslash must not swallow the
-- closing quote and run the text after it as SQL.
CREATE FUNCTION fs_t_typed() RETURNS text LANGUAGE plxplsql AS $f$
BEGIN
  RETURN text'C:\' || '
|| 42::text; --';
END;
$f$;
CREATE FUNCTION fs_t_nchar() RETURNS text LANGUAGE plxplsql AS $f$
BEGIN
  RETURN N'C:\' || '
|| 42::text; --';
END;
$f$;
CREATE FUNCTION fs_t_varchar() RETURNS text LANGUAGE plxplsql AS $f$
BEGIN
  RETURN varchar'C:\' || '
|| 42::text; --';
END;
$f$;
CREATE FUNCTION fs_t_static() RETURNS text LANGUAGE plxjs AS $f$
let r = fetch_one(`SELECT text'C:\\' || '
|| 42::text; --' AS v`);
return r.v;
$f$;
CREATE FUNCTION fs_t_dyn() RETURNS text LANGUAGE plxjs AS $f$
let r = fetch_one(`SELECT text'C:\\' || $1 AS v`, '!');
return r.v;
$f$;
-- plpgsql prepares a statement at its first run, so the first calls are
-- the ones made with the setting off
CALL fs_scs_off();
SELECT fs_t_typed() = E'C:\\\n|| 42::text; --' AS typed_ok,
       fs_t_nchar() = E'C:\\\n|| 42::text; --' AS nchar_ok,
       fs_t_varchar() = E'C:\\\n|| 42::text; --' AS varchar_ok,
       fs_t_static() = E'C:\\\n|| 42::text; --' AS static_ok;
SELECT fs_t_dyn();
RESET standard_conforming_strings;
SELECT fs_t_typed() = E'C:\\\n|| 42::text; --' AS typed_ok,
       fs_t_nchar() = E'C:\\\n|| 42::text; --' AS nchar_ok,
       fs_t_varchar() = E'C:\\\n|| 42::text; --' AS varchar_ok,
       fs_t_static() = E'C:\\\n|| 42::text; --' AS static_ok;
SELECT fs_t_dyn();
-- [3]: a -- comment ends at a carriage return, so a literal after one is
-- rewritten too
CREATE FUNCTION fs_t_cr() RETURNS text LANGUAGE plxplsql
AS E'BEGIN\n  -- note\r  RETURN ''C:\\'' || ''\n|| 42::text; --'';\nEND;\n';
CALL fs_scs_off();
SELECT fs_t_cr() = E'C:\\\n|| 42::text; --' AS cr_ok;
RESET standard_conforming_strings;
SELECT fs_t_cr() = E'C:\\\n|| 42::text; --' AS cr_ok;
-- [22]: in static SQL, a value interpolated where SQL needs a name fails
-- when the function is created. It used to become the bare variable name,
-- which plpgsql does not replace there, so FROM #{fs_tab} read the table
-- named fs_tab whatever the variable held.
CREATE TABLE fs_tab (x int);
INSERT INTO fs_tab VALUES (1), (2), (3);
CREATE TABLE fs_users (id int);
INSERT INTO fs_users VALUES (1);
SELECT fs_err($q$CREATE FUNCTION fs_i_from(fs_tab text) RETURNS bigint LANGUAGE plxruby AS $f$
u = fetch_one("SELECT count(*) AS c FROM #{fs_tab}")
return u.c
$f$$q$);
SELECT fs_err($q$CREATE FUNCTION fs_i_perform(fs_tab text) RETURNS void LANGUAGE plxjs AS $f$
perform(`DELETE FROM ${fs_tab}`);
$f$$q$);
SELECT fs_err($q$CREATE FUNCTION fs_i_query(fs_tab text) RETURNS SETOF int LANGUAGE plxjs AS $f$
return_query(`SELECT x FROM ${fs_tab}`);
$f$$q$);
SELECT fs_err($q$CREATE FUNCTION fs_i_alias(a text) RETURNS int LANGUAGE plxjs AS $f$
let r = fetch_one(`SELECT 1 AS ${a}`);
return 1;
$f$$q$);
SELECT count(*) AS fs_tab_rows FROM fs_tab;
-- [22]: a value where SQL takes an expression still works, after FROM too
CREATE FUNCTION fs_i_value(n int, s text) RETURNS text LANGUAGE plxjs AS $f$
let r = fetch_one(`SELECT substring(${s} FROM ${n}) || count(*) AS v FROM fs_tab WHERE x >= ${n} LIMIT ${n}`);
return r.v;
$f$;
SELECT fs_i_value(2, 'abcdef');
-- [review of PR 24 fixes]: an E'...' string whose text ends in an unpaired
-- backslash right before an interpolation is rejected. PostgreSQL would read
-- the quote plx writes after the backslash as an escaped quote, so plx and
-- PostgreSQL would disagree about where the string ends, and a later
-- literal() or ident() value could run as SQL.
CREATE TABLE fs_secret (s text);
INSERT INTO fs_secret VALUES ('TOPSECRET');
CREATE FUNCTION fs_e_lit(x text, y text) RETURNS text LANGUAGE plxruby AS $f$
r = fetch_one("SELECT E'a\\#{x}' AS v, #{literal(y)} AS w")
return r.v
$f$;
CREATE FUNCTION fs_e_ident(x text, y text) RETURNS text LANGUAGE plxruby AS $f$
r = fetch_one("SELECT E'a\\#{x}' AS v, 1 AS #{ident(y)}")
return r.v
$f$;
CREATE FUNCTION fs_e_js(x text, y text) RETURNS text LANGUAGE plxjs AS $f$
let r = fetch_one(`SELECT E'a\\${x}' AS v, ${literal(y)} AS w`);
return r.v;
$f$;
CREATE FUNCTION fs_e_bind(x text, y text) RETURNS text LANGUAGE plxjs AS $f$
let r = fetch_one(`SELECT E'a\\${x}' AS v, $1 AS w`, y);
return r.v;
$f$;
CREATE FUNCTION fs_e_static(x text) RETURNS text LANGUAGE plxruby AS $f$
r = fetch_one("SELECT E'a\\#{x}' AS v")
return r.v
$f$;
-- a doubled backslash is one literal backslash, and the value follows it
CREATE FUNCTION fs_e_ok(x text, y text) RETURNS text LANGUAGE plxruby AS $f$
r = fetch_one("SELECT E'a\\\\#{x}' || #{literal(y)} AS v")
return r.v
$f$;
SELECT fs_e_ok('b', ') || (SELECT s FROM fs_secret) || (');
CREATE FUNCTION fs_e_ok_static(x text) RETURNS text LANGUAGE plxruby AS $f$
r = fetch_one("SELECT E'a\\\\#{x}' AS v")
return r.v
$f$;
SELECT fs_e_ok_static('b');
