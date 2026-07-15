#!/usr/bin/env bash
# Build PostgreSQL 13-17 from source, build plx against each, run the regress
# suite. Logs a PASS/FAIL line per version. Run as root in the container.
set -uo pipefail
export MAKEFLAGS="-j$(nproc)"
SRCREPO=/usr/local/src/postgresql        # existing REL_18 clone (reused as a reference)
PLX=/root/plxsrc
SUMMARY=/root/pg_matrix_summary.txt
: > "$SUMMARY"

for v in 13 14 15 16 17; do
  PREFIX=/usr/local/pg$v
  SRC=/usr/local/src/pg$v
  PORT=54$v
  DATA=$PREFIX/data
  LOG=/root/pg${v}_build.log
  echo "===== PostgreSQL $v =====" | tee -a "$SUMMARY"

  if [ ! -x "$PREFIX/bin/pg_config" ]; then
    rm -rf "$SRC"
    git clone --depth 1 --branch "REL_${v}_STABLE" \
      https://git.postgresql.org/git/postgresql.git "$SRC" >"$LOG" 2>&1
    ( cd "$SRC" && ./configure --prefix="$PREFIX" --without-icu --without-readline >>"$LOG" 2>&1 \
      && make >>"$LOG" 2>&1 && make install >>"$LOG" 2>&1 \
      && make -C src/pl/plpgsql install >>"$LOG" 2>&1 )
    if [ ! -x "$PREFIX/bin/pg_config" ]; then
      echo "pg$v: BUILD FAILED (see $LOG)" | tee -a "$SUMMARY"; continue
    fi
  fi

  # symbol check: plpgsql handlers must be resolvable cross-.so
  GLOBAL=$(readelf -sW "$PREFIX/lib/plpgsql.so" 2>/dev/null | awk '$8=="plpgsql_call_handler"{print $5}')
  echo "pg$v: plpgsql_call_handler binding=$GLOBAL" | tee -a "$SUMMARY"

  # build plx against this version in a clean copy
  B=/root/plxbuild_$v
  rm -rf "$B"; mkdir -p "$B"
  cp -r "$PLX/src" "$PLX/Makefile" "$PLX/plx.control" "$PLX/test" "$B/"
  cp "$PLX"/plx--*.sql "$B/"
  if ! make -C "$B" PG_CONFIG="$PREFIX/bin/pg_config" >"/root/plx_pg${v}.log" 2>&1; then
    echo "pg$v: plx COMPILE FAILED (see /root/plx_pg${v}.log)" | tee -a "$SUMMARY"; continue
  fi
  make -C "$B" install PG_CONFIG="$PREFIX/bin/pg_config" >>"/root/plx_pg${v}.log" 2>&1

  # fresh cluster
  id postgres >/dev/null 2>&1 || useradd -m postgres
  rm -rf "$DATA"; mkdir -p "$DATA"; chown -R postgres "$PREFIX"
  runuser -u postgres -- "$PREFIX/bin/initdb" -D "$DATA" -U postgres --auth=trust >>"/root/plx_pg${v}.log" 2>&1
  runuser -u postgres -- "$PREFIX/bin/pg_ctl" -D "$DATA" -o "-p $PORT -k /tmp" -l "$DATA/server.log" -w start >>"/root/plx_pg${v}.log" 2>&1

  # run the regression suite
  if make -C "$B" installcheck PG_CONFIG="$PREFIX/bin/pg_config" \
       PGHOST=/tmp PGPORT=$PORT PGUSER=postgres >"/root/plx_pg${v}_regress.log" 2>&1; then
    echo "pg$v: REGRESS PASS" | tee -a "$SUMMARY"
  else
    echo "pg$v: REGRESS FAIL (see /root/plx_pg${v}_regress.log and $B/test/regression.diffs)" | tee -a "$SUMMARY"
  fi
  runuser -u postgres -- "$PREFIX/bin/pg_ctl" -D "$DATA" -w stop >>"/root/plx_pg${v}.log" 2>&1
done
echo "PG_MATRIX_DONE" | tee -a "$SUMMARY"
