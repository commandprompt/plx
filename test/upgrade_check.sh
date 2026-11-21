#!/bin/bash
# Upgrade checks that need more than one session, which pg_regress cannot
# hold open. Needs the extension files installed and a running server, like
# installcheck. PLX_PSQL names psql; PGHOST, PGPORT and PGUSER work as usual.
# Exits 0 when every check passes, 1 otherwise.
set -u
PSQL=${PLX_PSQL:-psql}
DB=plx_upgrade_check
fail=0
q() { "$PSQL" -X -q -v ON_ERROR_STOP=1 "$@"; }

"$PSQL" -X -q -d postgres -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1
q -d postgres -c "CREATE DATABASE $DB" || exit 1

# 1. PR 24 review r2 u1[1]: another session's temporary table with an
#    expression index must not stop ALTER EXTENSION plx UPDATE, which looks
#    for plx_strbuild index keys. The update must not read another session's
#    temporary tables.
q -d $DB -c "CREATE EXTENSION plx VERSION '2.0.0'" || exit 1
( printf "CREATE TEMP TABLE scratch(id int);\nCREATE INDEX ON scratch ((id + 1));\nSELECT pg_sleep(6);\n" |
  "$PSQL" -X -q -d $DB >/dev/null ) &
holder=$!
sleep 2
if out=$(q -d $DB -c "ALTER EXTENSION plx UPDATE" 2>&1); then
  echo "ok   update with another session's temp table"
else
  echo "FAIL update with another session's temp table:"; echo "$out"; fail=1
fi
wait $holder
v=$(q -At -d $DB -c "SELECT extversion FROM pg_extension WHERE extname = 'plx'")
d=$(q -At -d $DB -c "SELECT default_version FROM pg_available_extensions WHERE name = 'plx'")
if [ "$v" = "$d" ]; then
  echo "ok   extension version is $v"
else
  echo "FAIL extension version is $v, expected $d"; fail=1
fi

# 2. PR 24 review r3 s1: the update finds the type of each index and
#    partition key expression. It must not run the expression: a planned
#    query folds an IMMUTABLE call with constant arguments, so a table owner's
#    function would run as the superuser who runs the update.
"$PSQL" -X -q -d postgres -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1
"$PSQL" -X -q -d postgres -c "DROP ROLE IF EXISTS plx_upg_mallory" >/dev/null 2>&1
q -d postgres -c "CREATE DATABASE $DB" || exit 1
q -d $DB >/dev/null <<'SQL' || exit 1
CREATE EXTENSION plx VERSION '2.0.0';
CREATE ROLE plx_upg_mallory;
CREATE SCHEMA mal AUTHORIZATION plx_upg_mallory;
SET ROLE plx_upg_mallory;
CREATE FUNCTION mal.grab() RETURNS int LANGUAGE plpgsql VOLATILE AS $f$
BEGIN
  IF (SELECT rolsuper FROM pg_catalog.pg_roles WHERE rolname = current_user) THEN
    EXECUTE 'ALTER ROLE plx_upg_mallory SUPERUSER';
  END IF;
  RETURN 0;
END $f$;
CREATE FUNCTION mal.k(int) RETURNS int LANGUAGE plpgsql IMMUTABLE AS $f$
BEGIN PERFORM mal.grab(); RETURN $1; END $f$;
CREATE TABLE mal.t (a int, s text);
CREATE INDEX ON mal.t ((a + mal.k(1)));
CREATE TABLE mal.p (a int) PARTITION BY RANGE ((a + mal.k(2)));
RESET ROLE;
SQL
if ! out=$(q -d $DB -c "ALTER EXTENSION plx UPDATE" 2>&1); then
  echo "FAIL update with user functions in key expressions:"; echo "$out"; fail=1
fi
s=$(q -At -d $DB -c "SELECT rolsuper FROM pg_roles WHERE rolname = 'plx_upg_mallory'")
if [ "$s" = "f" ]; then
  echo "ok   update runs no key expression"
else
  echo "FAIL update ran a key expression: plx_upg_mallory rolsuper=$s"; fail=1
fi

# 3. PR 24 review r4: nor may it parse the expression again. Parsing a
#    literal of a domain array type calls the domain's CHECK, which can be a
#    table owner's function.
"$PSQL" -X -q -d postgres -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1
"$PSQL" -X -q -d postgres -c "DROP ROLE IF EXISTS plx_upg_mallory" >/dev/null 2>&1
q -d postgres -c "CREATE DATABASE $DB" || exit 1
q -d $DB >/dev/null <<'SQL' || exit 1
CREATE EXTENSION plx VERSION '2.0.0';
CREATE ROLE plx_upg_mallory;
CREATE SCHEMA mal AUTHORIZATION plx_upg_mallory;
SET ROLE plx_upg_mallory;
CREATE FUNCTION mal.grab(int) RETURNS boolean LANGUAGE plpgsql AS $f$
BEGIN
  IF (SELECT rolsuper FROM pg_catalog.pg_roles WHERE rolname = current_user) THEN
    EXECUTE 'ALTER ROLE plx_upg_mallory SUPERUSER';
  END IF;
  RETURN true;
END $f$;
CREATE DOMAIN mal.d AS int CHECK (mal.grab(VALUE));
CREATE TABLE mal.t (a int);
CREATE INDEX ON mal.t ((a + ('{1}'::mal.d[])[1]));
CREATE TABLE mal.p (a int) PARTITION BY RANGE ((a + ('{2}'::mal.d[])[1]));
RESET ROLE;
SQL
if ! out=$(q -d $DB -c "ALTER EXTENSION plx UPDATE" 2>&1); then
  echo "FAIL update with domain arrays in key expressions:"; echo "$out"; fail=1
fi
s=$(q -At -d $DB -c "SELECT rolsuper FROM pg_roles WHERE rolname = 'plx_upg_mallory'")
if [ "$s" = "f" ]; then
  echo "ok   update parses no key expression"
else
  echo "FAIL update parsed a key expression: plx_upg_mallory rolsuper=$s"; fail=1
fi

# 4. PR 24 review r4: a whole-row index key, ((t)), has the table's row
#    type, which has no collation. The update gave it the collation of the
#    table's first column (a plx_strbuild), so a dump of the database did not
#    restore. Also: a temporary relation of the upgrading session must not
#    stop the update.
"$PSQL" -X -q -d postgres -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1
q -d postgres -c "CREATE DATABASE $DB" || exit 1
q -d $DB >/dev/null <<'SQL' || exit 1
CREATE EXTENSION plx VERSION '2.0.0';
CREATE TABLE wr (h plx_strbuild, n int);
CREATE INDEX wr_b ON wr ((wr));
CREATE INDEX wr_e ON wr USING hash ((h::plx_strbuild));
SQL
if ! out=$(printf "CREATE TEMP TABLE plx_key_probe(x int);\nALTER EXTENSION plx UPDATE;\n" | q -d $DB 2>&1); then
  echo "FAIL update with a temporary relation in the session:"; echo "$out"; fail=1
fi
c=$(q -At -d $DB -c "SELECT string_agg(c.relname || '=' || i.indcollation::text, ' ' ORDER BY c.relname) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE i.indrelid = 'wr'::regclass")
if [ "$c" = "wr_b=0 wr_e=100" ]; then
  echo "ok   key collations after update"
else
  echo "FAIL key collations after update: $c (expected wr_b=0 wr_e=100)"; fail=1
fi

# 5. 3.0.0 -> 3.1.0 adds the plxcsharp and plxlua languages: after the
#    update both run a function, and the extension has the same functions
#    as a fresh install.
"$PSQL" -X -q -d postgres -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1
q -d postgres -c "CREATE DATABASE $DB" || exit 1
fns="SELECT count(*) || ' ' || md5(string_agg(p.oid::regprocedure::text, ',' ORDER BY p.oid::regprocedure::text)) FROM pg_proc p JOIN pg_depend d ON d.objid = p.oid AND d.classid = 'pg_proc'::regclass AND d.deptype = 'e' JOIN pg_extension x ON x.oid = d.refobjid AND x.extname = 'plx'"
q -d $DB -c "CREATE EXTENSION plx" >/dev/null || exit 1
fresh=$(q -At -d $DB -c "$fns")
q -d $DB -c "DROP EXTENSION plx" >/dev/null || exit 1
q -d $DB -c "CREATE EXTENSION plx VERSION '3.0.0'" >/dev/null || exit 1
if ! out=$(q -d $DB -c "ALTER EXTENSION plx UPDATE TO '3.1.0'" 2>&1); then
  echo "FAIL update from 3.0.0 to 3.1.0:"; echo "$out"; fail=1
fi
upd=$(q -At -d $DB -c "$fns")
r=$(q -At -d $DB 2>&1 <<'SQL'
CREATE FUNCTION up_cs(n int) RETURNS int LANGUAGE plxcsharp AS $$ return n * 2; $$;
CREATE FUNCTION up_lua(n int) RETURNS int LANGUAGE plxlua AS $$ return n + 1 $$;
SELECT up_cs(20) + up_lua(1);
SQL
)
if [ "$upd" = "$fresh" ] && [ "$r" = "42" ]; then
  echo "ok   update 3.0.0 to 3.1.0 adds the plxcsharp and plxlua languages"
else
  echo "FAIL update 3.0.0 to 3.1.0: functions '$upd' (fresh '$fresh'), languages gave '$r'"; fail=1
fi

"$PSQL" -X -q -d postgres -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1
"$PSQL" -X -q -d postgres -c "DROP ROLE IF EXISTS plx_upg_mallory" >/dev/null 2>&1
exit $fail
