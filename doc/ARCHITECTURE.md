# plx Architecture

plx lets a function body be written in a Ruby, PHP, JavaScript, TypeScript,
Python, Go, COBOL, Oracle PL/SQL, or Transact-SQL dialect and executed by the
standard plpgsql interpreter. This document describes how that works in the
extension as built.

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
- the block style, used as a lexer hint (indentation-tokenized vs.
  newline-tokenized);
- comment syntax, the variable sigil (for example `$`), the string-concatenation
  operator, and how string interpolation is written (`#{}`, `$var` and `{$e}`,
  `${}` template literals, or f-strings);
- `parse_body`, the front-end entry point (see below), and
  `self_contained_block`, a flag for dialects that emit their own
  `DECLARE`/`BEGIN`/`END` (PL/SQL).

### Engine and front ends

The transpiler is split into a dialect-neutral **engine** and per-dialect
**front ends**, selected through the `parse_body` function pointer on the
surface (a vtable method). `plx_transpile()` just calls
`cx->surf->parse_body(cx)` and then runs one assemble tail; there is no
per-dialect branching in the driver.

- The **engine** lives in `plx_transpile.c` and is declared to the front ends
  through `plx_engine.h`: the shared byte lexer (`plx_lex`), the expression
  rewriter (`plx_rewrite_expr`), the leaf-statement emitter and intrinsics
  (`query`, `fetch_one`, `perform`, `execute`, `return_query`, cursors, and so
  on), the symbol table, string/interpolation decoding, and the final
  DECLARE-hoisting + assemble. It contains no dialect-specific code.
- Each **front end** owns its dialect's tokenizer, parser, and statement
  lowering, and implements `parse_body` by transforming `cx->body` into
  `cx->out`. The text-family dialects (`php`/`js`/`ts`) share a brace parser in
  `plx_parse_brace.c`; Ruby (keyword-`end`) and Python (indentation) parse on
  top of the shared lexer inside their own translation units; and the
  standalone dialects (COBOL, PL/SQL, T-SQL, Go) run their own tokenizer and
  emitter, calling back into the engine only for shared services.

## Files

```
plx.control, plx--1.0.sql     extension control and install SQL
src/plx.h                     public ABI (PlxDialect, PlxFuncMeta)
src/plx_int.h                 internal ABI (PlxSurface, canonical keywords)
src/plx_engine.h              engine interface: PlxCtx, tokens, symtab, and the
                              plx_* entry points the front ends call
src/plx_core.c                PL handler binding, registry, generic validator
                              and inline handler
src/plx_transpile.c           the dialect-neutral engine + plx_transpile() driver
src/plx_strbuild.c            string-builder intrinsic helpers
src/plx_parse_brace.c         shared brace front end (php/js/ts) + TS preprocess
src/plx_dialect_ruby.c        the plxruby surface + Ruby front end
src/plx_dialect_php.c         the plxphp surface (parse_body -> brace front end)
src/plx_dialect_js.c          the plxjs surface (parse_body -> brace front end)
src/plx_dialect_ts.c          the plxts surface (parse_body -> brace front end)
src/plx_dialect_python.c      the plxpython3 surface + Python front end
src/plx_dialect_go.c          the plxgo surface + Go front end
src/plx_dialect_cobol.c       the plxcobol surface + COBOL front end
src/plx_dialect_plsql.c       the plxplsql surface + PL/SQL front end
src/plx_dialect_tsql.c        the plxtsql surface + T-SQL front end
```

Everything links into a single `plx.so`. A dialect is a `PlxSurface` (including
its `parse_body` front end) plus small trampolines (validator, inline handler,
and the shared call-handler binding), registered in `_PG_init`. Adding a dialect
is a new `plx_dialect_X.c` (a surface with its `parse_body`, plus a few
`CREATE LANGUAGE` lines), and does not touch the shared engine.

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
  [plxjs](plxjs.md), [plxts](plxts.md), [plxpython3](plxpython3.md),
  [plxgo](plxgo.md), [plxcobol](plxcobol.md), [plxplsql](plxplsql.md),
  [plxtsql](plxtsql.md).
