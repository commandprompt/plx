# plx - PGXS build
MODULE_big = plx
OBJS = src/plx_core.o src/plx_transpile.o src/plx_dialect_ruby.o src/plx_dialect_php.o

EXTENSION = plx
DATA = plx--1.0.sql

# pg_regress suite (make installcheck). test/run_corpus.py is an additional
# Ruby corpus runner.
REGRESS = plxruby plxphp plx_errors
REGRESS_OPTS = --inputdir=test --outputdir=test

# Point at the source-built PG 18 (not any distro pg_config on PATH).
PG_CONFIG ?= /usr/local/pgsql/bin/pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
