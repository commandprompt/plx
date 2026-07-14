# plx Architecture

plx lets a function body be written in a Ruby, PHP, JavaScript, or Python
dialect and executed by the standard plpgsql interpreter. This document
describes how that works in the extension as built.

## The core idea

A plx language is a PostgreSQL procedural language whose call handler is
plpgsql's own call handler. The dialect body never runs directly. At
`CREATE FUNCTION` time plx transpiles the body to plpgsql text and stores that
text in `pg_proc.prosrc`. At run time plpgsql compiles and executes that stored
text like any other plpgsql function.

There is no separate language runtime. plx is a C extension that parses the
dialect syntax and emits plpgsql; the execution engine is plpgsql.

## Catalog wiring

For each dialect, `plx--1.0.sql` creates a language whose parts are:

- `HANDLER` is `plx_call_handler`, which is bound to plpgsql's exported
  `plpgsql_call_handler` symbol (`AS '$libdir/plpgsql', 'plpgsql_call_handler'`).
  Binding a fresh `pg_proc` row to plpgsql's symbol is legal because
  `CreateProceduralLanguage` only checks that the handler returns
  `language_handler`.
- `VALIDATOR` is the dialect's validator (for example `plx_ruby_validator`),
  which does the transpilation.
- `INLINE` is the dialect's inline handler, for `DO` blocks.

Because the call handler is plpgsql's own, run-time execution is plpgsql with no
plx code on the hot path. plx inherits SPI setup, plan caching and invalidation,
polymorphic argument resolution, `OUT`/`SETOF` handling, and trigger dispatch
from plpgsql automatically.

## The validator: transpile at DDL time

`ProcedureCreate` calls the language validator after inserting the `pg_proc` row,
on every `CREATE` and `CREATE OR REPLACE`, and on `pg_restore`. The dialect
validator (in `plx_core.c`, `plx_generic_validator`):

1. Reads the raw dialect body from `pg_proc.prosrc`.
2. If the text already begins with the plx sentinel (`/*plx:v1:...*/`), it is
   already plpgsql (a restore or a repeated validation), so it returns without
   change. This makes the pass idempotent and keeps dump and restore correct.
3. Otherwise it builds a `PlxFuncMeta` from the pg_proc row (argument names,
   types, and modes via `get_func_arg_info`; return type; `proretset`; prokind),
   calls the dialect's `transpile` function, and rewrites `pg_proc.prosrc` with
   the resulting plpgsql via `CatalogTupleUpdate`.

The stored text is:

```
/*plx:v1:<dialect>:<hash>*/
[DECLARE ...]
BEGIN
  <emitted plpgsql>
END;
/*plx-orig:b64$<base64 of the original dialect body>$plx-orig*/
```

The sentinel gates idempotency. The base64 trailer preserves the original source
so it survives dump and restore and can be recovered.

`DO` blocks have no `pg_proc` row. The inline handler transpiles the block text
and delegates to plpgsql's own inline handler with a rebuilt `InlineCodeBlock`.

## Why the binding holds across versions

The only symbol that fmgr resolves from another module is
`plpgsql_call_handler`, which is a global fmgr entry point in every supported
release (PostgreSQL 13 to 18). plx does not call any plpgsql-internal symbol
(the compiler, executor, or scanner functions), so it does not depend on the
symbol visibility of those internals. See [COMPATIBILITY.md](COMPATIBILITY.md).

## The transpiler

The transpiler does not parse the source language's expression grammar.
plpgsql expressions are SQL expressions, so the transpiler is a statement-level
restructurer: it finds statement and block boundaries, hoists typed `DECLARE`s,
rewrites a fixed set of operators and interpolations, and passes the remaining
expression text through to plpgsql and SQL unchanged.

It is dialect-pluggable through a `PlxSurface` (in `plx_int.h`) that each dialect
supplies. The surface describes what varies between languages:

- the keyword table, mapping each dialect's spellings to canonical keywords;
- the block style: keyword-delimited (`end`), brace-delimited (`{ }`), or
  indentation (INDENT/DEDENT);
- comment syntax, the variable sigil (for example `$`), the string-concatenation
  operator, and how string interpolation is written (`#{}`, `$var` and `{$e}`,
  `${}` template literals, or f-strings).

The shared code (`plx_transpile.c`) is dialect-neutral: the lexer, the three
block parsers, the expression rewriter, DECLARE-hoisting and type inference, the
statement lowering, and the intrinsics (`query`, `fetch_one`, `perform`,
`execute`, `return_query`, cursors, and so on) are all driven by the surface.

## Files

```
plx.control, plx--1.0.sql     extension control and install SQL
src/plx.h                     public ABI (PlxDialect, PlxFuncMeta)
src/plx_int.h                 internal ABI (PlxSurface, canonical keywords)
src/plx_core.c                PL handler binding, registry, generic validator
                              and inline handler
src/plx_transpile.c           the shared transpiler
src/plx_dialect_ruby.c        the plxruby surface and trampolines
src/plx_dialect_php.c         the plxphp surface and trampolines
src/plx_dialect_js.c          the plxjs surface and trampolines
src/plx_dialect_python.c      the plxpython3 surface and trampolines
```

Everything links into a single `plx.so`. A dialect is a `PlxSurface` plus three
small trampolines (validator, inline handler, and the shared call-handler
binding), registered in `_PG_init`. Adding a dialect is a new surface and a few
`CREATE LANGUAGE` lines; it does not touch the shared transpiler.

## Trust

The plx languages are declared `TRUSTED`. A plx function executes as plpgsql
through plpgsql's call handler, so it can do what a plpgsql function can do and
nothing more: no filesystem, no network, no arbitrary native code, and all SQL
runs with the caller's privileges. plx embeds no language runtime, which is the
difference from the native PL/Ruby and PL/PHP, which are untrusted because they
load a full interpreter into the backend. The transpiler is C that parses
untrusted input at `CREATE FUNCTION` time; it has a recursion-depth limit and is
fuzzed (see `test/fuzz.py`).

## Related documents

- [TRANSPILER.md](TRANSPILER.md): the original transpiler design specification.
- [PARITY.md](PARITY.md): the plpgsql construct parity matrix.
- The per-dialect chapters: [plxruby](plxruby.md), [plxphp](plxphp.md),
  [plxjs](plxjs.md), [plxpython3](plxpython3.md).
