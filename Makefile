# plx - PGXS build
MODULE_big = plx
OBJS = src/plx_core.o src/plx_transpile.o src/plx_strbuild.o src/plx_dialect_ruby.o src/plx_dialect_php.o src/plx_dialect_js.o src/plx_dialect_python.o src/plx_dialect_cobol.o src/plx_dialect_plsql.o src/plx_dialect_ts.o src/plx_dialect_tsql.o src/plx_dialect_go.o

EXTENSION = plx
DATA = plx--1.0.sql plx--1.1.sql plx--1.1.1.sql plx--1.2.sql plx--1.0--1.1.sql plx--1.1--1.1.1.sql plx--1.1.1--1.2.sql

# pg_regress suite (make installcheck). test/run_corpus.py is an additional
# Ruby corpus runner.
REGRESS = plxruby plxphp plxjs plxpython3 plxcobol plxplsql plxts plxtsql plxgo plx_features plx_output plx_strbuild plx_errors
REGRESS_OPTS = --inputdir=test --outputdir=test

# Point at the source-built PG 18 (not any distro pg_config on PATH).
PG_CONFIG ?= /usr/local/pgsql/bin/pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# PGXS does not track header dependencies; declare them so a header change
# rebuilds every object (the shared enum/struct layout must stay consistent).
$(OBJS): src/plx.h src/plx_int.h
