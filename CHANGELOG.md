# Changelog

All notable changes to plx are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and plx uses the extension
version in `plx.control` (currently `1.0`).

## [1.2] - unreleased

### Added

- `plxplsql`, an Oracle PL/SQL dialect. PL/SQL and plpgsql are both Ada-descended,
  so most of the language (`DECLARE`/`BEGIN`/`EXCEPTION`/`END`, `IF`/`ELSIF`,
  `LOOP`/`WHILE`/`FOR`, `CASE`, `:=`, `||`, cursors, `%TYPE`) passes through
  unchanged. plxplsql is a layout-preserving rewriter that translates the Oracle
  spellings: `NUMBER`/`VARCHAR2`/`PLS_INTEGER`/... types, `DBMS_OUTPUT.PUT_LINE`,
  `RAISE_APPLICATION_ERROR`, `EXECUTE IMMEDIATE`, `FROM DUAL`, `NVL`,
  `seq.NEXTVAL`, `SYSDATE`, and `CURSOR c IS`. Function signatures use PostgreSQL
  types; the body is PL/SQL. See [doc/plxplsql.md](doc/plxplsql.md).
- `plxts`, a TypeScript dialect: the plxjs dialect plus `let x: T` type
  annotations, which map TypeScript types (`number`, `string`, `boolean`,
  `bigint`, `T[]`, `T | null`) to SQL types and otherwise accept a SQL type name
  verbatim. See [doc/plxts.md](doc/plxts.md).
- `doc/DEBUGGING.md`: correlating runtime errors to your dialect source, and a
  `plx_source()` helper that recovers the embedded original body.

### Upgrading

- `ALTER EXTENSION plx UPDATE TO '1.2'` (see `plx--1.1.1--1.2.sql`).

## [1.1.1] - 2026-07-15

Code-only patch release (no catalog changes). Upgrade with
`ALTER EXTENSION plx UPDATE TO '1.1.1'` after installing the new module.

### Fixed

- Compilation on PostgreSQL 13, 14, and 15: `plx_strbuild.c` included `varatt.h`
  unconditionally, but that header was only split out of `postgres.h` in
  PostgreSQL 16, so plx 1.1 did not build on 13-15. Guard the include. The full
  regression suite now passes on PostgreSQL 13 through 18 (verified in CI).
- plxcobol: a crash (out-of-bounds read) on a body truncated at
  `PERFORM VARYING ... UNTIL`; it now errors cleanly.
- plxcobol: `ADD a b GIVING c` and other multi-addend `ADD`/`SUBTRACT` forms were
  rejected; parse an operand list. `MULTIPLY`/`DIVIDE` remain single-source.
- plxcobol: multi-argument SQL function calls in expressions (`mod(a, b)`) were
  broken because the tokenizer stripped commas everywhere; keep commas inside
  parentheses.
- plxcobol: a `GREATER/LESS ... OR EQUAL` comparison at the end of a condition
  dropped the "OR EQUAL"; a `PICTURE` repeat-count integer overflow; and an
  unterminated string literal silently lost its last character.

### Added

- Continuous integration (GitHub Actions) running the full 9-suite regression on
  a PostgreSQL 13 through 18 matrix.
- plxcobol coverage in the fuzzer and the corpus runner, and plxcobol rejection
  tests in the error suite.

## [1.1] - 2026-07-14

### Added

- `plxcobol`, a COBOL dialect (ISO/IEC 1989:2023, COBOL 2023, free format), at
  full plpgsql construct parity. It has its own front end (verb-driven tokenizer
  and parser): `WORKING-STORAGE` declarations with `PICTURE`/`TYPE`/`CONSTANT`
  mapped to SQL types; `MOVE`/`COMPUTE` and the `ADD`/`SUBTRACT`/`MULTIPLY`/
  `DIVIDE` verbs; `IF`/`END-IF`; `EVALUATE` (simple and `EVALUATE TRUE`);
  `PERFORM` in the `UNTIL`, `VARYING`, `TIMES`, inline, query (`OVER`), and array
  (`OVER ARRAY`) forms; `GOBACK RETURNING`, `RETURN-NEXT`, `RETURN-QUERY`;
  `EXECUTE`; cursors (`OPEN-CURSOR`/`FETCH-CURSOR`/`MOVE-CURSOR`/`CLOSE-CURSOR`);
  exception handling (`BEGIN-TRY`/`WHEN`/`END-TRY`) with stacked diagnostics via
  `GET`; `RAISE`, `DISPLAY`, `ASSERT`, `CALL`, `COMMIT`/`ROLLBACK`. Data names
  are mapped to plpgsql identifiers (lower-cased, hyphens to underscores).
  See [doc/plxcobol.md](doc/plxcobol.md).
- plxcobol: `STRING-APPEND <expr> TO <var>`, which lowers to the `plx_strbuild`
  string builder (the COBOL counterpart of the other dialects' append operators),
  and `%` as the modulo operator in expressions.
- Regression suite `plxcobol` added to `make installcheck`.
- Benchmarks now cover plxcobol (`bench/BENCHMARKS.md`): it matches plpgsql on
  arith, strbuild, iter, and call, and is about 1.3x on the branch workload
  because `EVALUATE` lowers to `CASE`. A `PERFORM VARYING v FROM a BY 1 UNTIL
  v > b` counting loop lowers to a plpgsql integer `FOR` loop (other `PERFORM
  VARYING` forms use `WHILE`).

### Upgrading

- Existing 1.0 installations upgrade in place with
  `ALTER EXTENSION plx UPDATE TO '1.1'` (see `plx--1.0--1.1.sql`).

## [1.0] - 2026-07-14

Initial release.

### Added

- Dialect-pluggable front end that transpiles to plpgsql at `CREATE FUNCTION`
  time. The generated plpgsql is stored in `pg_proc.prosrc` and executed by the
  standard plpgsql handler, with no separate language runtime in the backend.
- Dialects, each at full plpgsql construct parity:
  - `plxruby`, a Ruby dialect.
  - `plxphp`, a PHP dialect.
  - `plxjs`, a JavaScript dialect.
  - `plxpython3`, a Python dialect.
- Language names carry a `plx` prefix, so the extension coexists with the native
  PL/Ruby and PL/PHP in the same database.
- Coverage of the plpgsql surface from every dialect: typed local declarations
  and inference, control flow, loops with labels, query iteration, array
  iteration, single-row fetch, dynamic SQL, cursors, diagnostics, set-returning
  functions, error raising and handling, assertions, and trigger functions with
  `NEW`, `OLD`, and the `TG_` variables.
- Idempotent re-transpile: a sentinel comment records the transpiler version and
  embeds the original source, so re-processing a stored function is a no-op.
- `plx_strbuild`, an expanded-object string builder with amortized-O(1) append,
  addressing plpgsql's O(n^2) in-loop string concatenation. The transpiler lowers
  the dialect append operators (`s << x`, `$s .= x`, `s += x` on a string) onto
  it. On PostgreSQL 18 this is about 170x faster than the `s := s || 'x'` idiom
  and faster than the native PLs' own append. The acceleration relies on the
  PostgreSQL 18 planner support request `SupportRequestModifyInPlace`; on
  PostgreSQL 13 to 17 the builder is correct but not accelerated.
- Benchmark harness (`bench/run_bench.py`) covering five workloads against
  plpgsql, the plx dialects, PL/Perl, PL/Python3, and the native PL/Ruby and
  PL/PHP, with results in `bench/BENCHMARKS.md`.
- Regression suite (`make installcheck`) across the dialects, features, output,
  errors, and the string builder, plus a corpus runner and a fuzzer.
- Documentation: per-dialect chapters, a plpgsql parity matrix, a user guide
  drawn from the PL/pgSQL manual, an architecture document, a transpiler
  specification, and a compatibility note.

### Compatibility

- Tested against PostgreSQL 13, 14, 15, 16, 17, and 18. The full regression suite
  passes on each.

[1.1.1]: https://github.com/commandprompt/plx/releases/tag/v1.1.1
[1.1]: https://github.com/commandprompt/plx/releases/tag/v1.1
[1.0]: https://github.com/commandprompt/plx/releases/tag/v1.0
