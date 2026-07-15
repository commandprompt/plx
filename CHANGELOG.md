# Changelog

All notable changes to plx are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and plx uses the extension
version in `plx.control` (currently `1.0`).

## [Unreleased]

### Added

- plxtsql: a trigger can now assign to `NEW` fields with `SET NEW.col = e`, which
  lowers to `NEW.col := e`, so a Transact-SQL trigger can rewrite the row and not
  only validate it. A `SET` whose target is a qualified name with a top-level `=`
  is an assignment; `SET NOCOUNT ON` and other session options are still ignored.
  Qualified names also emit without a stray space around the dot. Covered by a new
  plxtsql trigger regression test.

## [1.2.2] - 2026-07-15

Code-only patch release (no catalog changes). Upgrade with
`ALTER EXTENSION plx UPDATE TO '1.2.2'` after installing the new module.

### Fixed

- plxphp: assigning to a record field with the arrow form (`$NEW->col = e`, the
  documented and idiomatic PHP spelling) raised "unsupported operator in
  statement"; only the array-element form worked. The arrow lvalue now lowers to
  `NEW.col := e`, so trigger functions can stamp `NEW` fields with `$NEW->col`.
  Covered by a new plxphp trigger regression test.

## [1.2.1] - 2026-07-15

Code-only patch release (no catalog changes). Upgrade with
`ALTER EXTENSION plx UPDATE TO '1.2.1'` after installing the new module.

### Fixed

- Build on PostgreSQL 19 and 20 with a C23 toolchain (for example gcc 15). There,
  `pg_noreturn` expands to the standard `[[noreturn]]` attribute, whose placement
  is strict; plx wrote it after the storage class (`static pg_noreturn void`),
  which C23 rejects. It is now the first token of the declaration
  (`pg_noreturn static void`), matching PostgreSQL's own convention, and still
  compiles on 13 through 18. The full suite passes on PostgreSQL 13 through 18
  plus 19beta and 20devel built from source. See [doc/COMPATIBILITY.md](doc/COMPATIBILITY.md).
- A plxgo regression test (0-based indexing) declared its accumulator with `:=`
  from a non-inferable expression, so the function failed to create and the test
  silently checked the failure rather than indexing; it now uses `var sum int`.

## [1.2] - 2026-07-15

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
- `plxtsql`, a Transact-SQL (SQL Server) dialect. T-SQL is not Ada-descended, so
  plxtsql is a restructuring front end with its own tokenizer and parser: it
  hoists `@`-variables and inline `DECLARE` into the plpgsql `DECLARE` block,
  rewrites `SET`/`SELECT @x =` assignments, turns `IF`/`WHILE ... BEGIN ... END`
  into `THEN ... END IF` / `LOOP ... END LOOP`, maps `TRY`/`CATCH` to an
  `EXCEPTION` block, and translates the type and function libraries (`INT`,
  `NVARCHAR(MAX)`, `DATETIME`, `ISNULL`, `IIF`, `CONVERT`, `LEN`, `GETDATE`,
  `PRINT`, `RAISERROR`, `THROW`, ...). See [doc/plxtsql.md](doc/plxtsql.md).
- `plxgo`, a Go dialect. Go's parenless `if`/`for`, `:=` short declarations with
  type inference, `for ... range`, and no-fallthrough `switch` differ enough from
  plpgsql that plxgo is a restructuring front end with its own tokenizer
  (including Go's automatic semicolon insertion) and parser. It hoists
  `var`/`:=`/`const` declarations, rewrites assignment (including `a, b = x, y`
  parallel assignment to `SELECT ... INTO`), turns `if`/`for`/`switch` into
  `IF`/`WHILE`/`FOR`/`FOREACH`/`IF-ELSIF`, maps `panic`/`fmt.Println` to `RAISE`,
  translates the type and a stdlib library (`strings`, `math`, `strconv`, `len`,
  `append`, type conversions), and provides `emit`/`execute`/`range query()` SQL
  intrinsics. See [doc/plxgo.md](doc/plxgo.md).
- `plxcobol` tables: `OCCURS n` maps a `WORKING-STORAGE` item to a PostgreSQL
  array, with `WS-ARR(i)` subscripts as both lvalues and expressions and
  `PERFORM v OVER ARRAY` iteration.
- `doc/DEBUGGING.md`: correlating runtime errors to your dialect source, and a
  `plx_source()` helper that recovers the embedded original body.

### Hardened

A whole-project adversarial audit (fresh-eyes review of every front end plus
mutation fuzzing of all nine dialects) fixed a set of transpiler defects, all now
covered by tests:

- Backend crashes: an unbounded intrinsic argument list read past a fixed stack
  array (17+ arguments to `call`/`query`/`execute`); the Python parser had no
  recursion-depth guard (deep nesting exhausted the C stack); a COBOL `USAGE`
  clause at end of input stepped past the token-array sentinel. All now error.
- Backend hangs: a stray `)`/`]` (Go) or dangling `ELSE` (T-SQL) at statement
  position, and a `for`/`if`/`else-if` recursion in Go, could spin or overflow;
  all now error cleanly.
- Wrong or invalid output: Go slice subscripts are 0-based (rewritten to
  PostgreSQL's 1-based arrays); a COBOL compound `UNTIL` no longer misfolds into
  the integer-`FOR` bound; a JS/PHP `switch` with no case, only `default`, or a
  `case` after `default` now errors instead of emitting uncompilable plpgsql; the
  `?:` elvis operator, a value-less Go `const`, and several empty-argument forms
  (`fmt.Println()`, `panic()`, `PRINT;`, `RAISERROR()`) are handled or rejected.

The mutation fuzzer (`test/fuzz.py`) and corpus now cover all nine dialects.

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
