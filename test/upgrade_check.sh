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

"$PSQL" -X -q -d postgres -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1
exit $fail
