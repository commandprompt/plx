# plx / plpgsql Feature Parity

Target: every plpgsql capability is expressible in each plx dialect. The
reference is the plpgsql statement set in `src/pl/plpgsql/src/plpgsql.h`
(`PLpgSQL_stmt_*`) plus its declaration forms.

Every construct below is supported in all five column dialects — **plxruby**,
**plxphp**, **plxjs**, **plxpython3**, **plxcobol** — except where a footnote
notes an equivalent form. The cells show the dialect spelling; full syntax and
semantics are in the per-dialect chapters ([plxruby](plxruby.md),
[plxphp](plxphp.md), [plxjs](plxjs.md), [plxpython3](plxpython3.md),
[plxcobol](plxcobol.md)).

**plxplsql** (Oracle PL/SQL) is not shown as a separate column because its
spelling *is* the plpgsql form: PL/SQL and plpgsql share `IF/ELSIF`, `LOOP`,
`WHILE`, `FOR`, `CASE`, `:=`, `||`, `RAISE`, cursors, `%TYPE`, and the
`EXCEPTION` section, so those constructs pass through unchanged. plxplsql adds
the Oracle spellings (`DBMS_OUTPUT.PUT_LINE`, `RAISE_APPLICATION_ERROR`, `EXECUTE
IMMEDIATE`, `NVL`, `seq.NEXTVAL`, `NUMBER`/`VARCHAR2`, ...) on top. See
[plxplsql.md](plxplsql.md).

**plxts** (TypeScript) is the plxjs column plus `let x: T` type annotations
(mapped to SQL types); every construct is the plxjs spelling. See
[plxts.md](plxts.md).

## Control flow

| plpgsql | plxruby | plxphp | plxjs | plxpython3 | plxcobol |
|---|---|---|---|---|---|
| assignment (`:=`) | `x = e` | `$x = e;` | `x = e;` | `x = e` | `MOVE e TO x` · `COMPUTE x = e` |
| `IF`/`ELSIF`/`ELSE` | `if`/`elsif`/`else`/`end` | `if`/`elseif`/`else` | `if`/`else if`/`else` | `if`/`elif`/`else:` | `IF`/`ELSE`/`END-IF` |
| simple `CASE` | `case x`/`when` | `switch`/`case` | `switch`/`case` | if/elif ¹ | `EVALUATE`/`WHEN` |
| searched `CASE` | `case`/`when` (no subject) | if/else ¹ | if/else ¹ | if/elif ¹ | `EVALUATE TRUE`/`WHEN` |
| `LOOP` | `loop do`/`end` | `while (true)` | `while (true)` | `while True:` | `PERFORM`/`END-PERFORM` |
| `WHILE` | `while` · `until` | `while` | `while` | `while` | `PERFORM UNTIL c` |
| integer `FOR` | `for i in 1..n` | `for ($i=…; …; $i++)` | `for (let i=…; …; i++)` | `for i in range(…)` | `PERFORM VARYING` · `PERFORM n TIMES` |
| `FOR` over query | `query(sql).each do \|r\|` | `foreach (query(sql) as $r)` | `for (const r of query(sql))` | `for r in query(sql):` | `PERFORM r OVER "sql"` |
| `FOR` over dynamic query | same, non-literal SQL / binds | same | same | same | `… OVER "sql" USING …` |
| `FOREACH` over array | `arr.each do \|v\|` | `foreach ($a as $v)` | `for (const v of arr)` | `for v in arr:` | `PERFORM v OVER ARRAY a` |
| `EXIT` / `CONTINUE` | `break` / `next` | `break` / `continue` | `break` / `continue` | `break` / `continue` | `EXIT PERFORM` / `EXIT PERFORM CYCLE` |
| loop labels ² | `label:` + `break label` | `label:` + `break label` | `label:` (native) | — | — |

¹ Python has no `match`/`case`, and PHP/JS `switch` is a simple `CASE` only, so a
searched (arbitrary-condition) or Python multi-way branch uses `if`/`elif`(`else
if`)/`else` — the same control flow, lowered to plpgsql `IF`. Ruby `case` and
COBOL `EVALUATE` / `EVALUATE TRUE` express both simple and searched `CASE`
directly.
² Loop labels (`label:` before a loop with `break label` / `next label`) are
native in JavaScript and a plx extension in Ruby and PHP; not offered in Python
or COBOL.

## Return, SQL, and data access

| plpgsql | plxruby | plxphp | plxjs | plxpython3 | plxcobol |
|---|---|---|---|---|---|
| `RETURN` | `return e` | `return $e;` | `return e;` | `return e` | `GOBACK RETURNING e` |
| `RETURN NEXT` | `emit e` · `return_next e` | `return_next($e)` | `return_next(e)` | `return_next(e)` | `RETURN-NEXT e` |
| `RETURN QUERY` | `return_query(sql)` | `return_query(sql)` | `return_query(sql)` | `return_query(sql)` | `RETURN-QUERY "sql"` |
| `PERFORM` | `perform(sql)` | `perform(sql)` | `perform(sql)` | `perform(sql)` | `EXECUTE "sql"` |
| `EXECUTE` (dynamic) | `execute(sql, a)` | `execute(sql, $a)` | `execute(sql, a)` | `execute(sql, a)` | `EXECUTE "sql" USING a` |
| SQL / `SELECT INTO` | `fetch_one(sql)` · `fetch_one!` | `fetch_one(sql)` | `fetch_one(sql)` | `fetch_one(sql)` | `EXECUTE "sql" INTO v` |
| cursors `OPEN`/`FETCH`/`MOVE`/`CLOSE` | `open_cursor` · `fetch_from` · `move_cursor` · `close_cursor` | (same call form) | (same call form) | (same call form) | `OPEN-` · `FETCH-` · `MOVE-` · `CLOSE-CURSOR` |
| `CALL` procedure | `call("p", a)` | `call("p", $a)` | `call("p", a)` | `call("p", a)` | `CALL "p" USING a` |
| `COMMIT` / `ROLLBACK` | `commit()` / `rollback()` | (same) | (same) | (same) | `COMMIT` / `ROLLBACK` |

The call-form intrinsics (`perform`, `execute`, `fetch_one`, the `*_cursor`
functions, `call`, `commit`, `rollback`, `return_next`, `return_query`) share the
same name in plxruby, plxphp, plxjs, and plxpython3; only the argument sigils and
string-literal/interpolation syntax differ. COBOL uses dedicated verbs.

## Errors and diagnostics

| plpgsql | plxruby | plxphp | plxjs | plxpython3 | plxcobol |
|---|---|---|---|---|---|
| `RAISE` | `raise "m"` · `raise notice: "m"` | `throw …` · `raise("notice", "m")` | `throw …` · `raise("notice", "m")` | `raise ValueError(…)` · `raise("notice", "m")` | `RAISE <level> "m"` · `DISPLAY` |
| `ASSERT` | `assert(c, m)` | `assert(c, m)` | `assert(c, m)` | `assert c, m` | `ASSERT c` |
| exception handling | `begin`/`rescue`/`ensure`/`end` | `try`/`catch`/`finally` | `try`/`catch`/`finally` | `try`/`except`/`finally` | `BEGIN-TRY`/`WHEN`/`END-TRY` |
| `GET DIAGNOSTICS` (`ROW_COUNT`) | `row_count()` | `row_count()` | `row_count()` | `row_count()` | `GET ROW-COUNT INTO v` |
| `FOUND` | `found?` | `found()` | `found()` | `found()` | `FOUND` |
| `GET STACKED DIAGNOSTICS` | `e.message` · `e.detail` · … | `$e->message` · `$e->detail` · … | `e.message` · `e.detail` · … | `e.message` · `e.detail` · … | `GET MESSAGE` · `GET DETAIL` · … `INTO v` |

Stacked-diagnostics fields available in all dialects: `message` (`SQLERRM`),
`sqlstate` (`SQLSTATE`), `detail`, `hint`, `constraint`, `column`, `table`,
`schema`, `datatype`. A specific condition is caught by name
(`PG::UniqueViolation` / `\PG::UniqueViolation` / `WHEN unique-violation`).

## Declarations and triggers

| plpgsql | plxruby | plxphp | plxjs | plxpython3 | plxcobol |
|---|---|---|---|---|---|
| `CONSTANT` | `x = 5 #:: int const` | `$x = 5 /*:: int const */` | `let x = 5 /*:: int const */` | `x = 5  #:: int const` | `01 X CONSTANT AS 5` |
| `%TYPE` / `%ROWTYPE` | `e #:: t%ROWTYPE` | `$e /*:: t%ROWTYPE */` | `let e /*:: t%ROWTYPE */` | `e #:: t%ROWTYPE` | `01 E TYPE t%ROWTYPE` |
| trigger (`NEW`/`OLD`/`TG_`) | `NEW.col = e`; `return NEW` | `$NEW->col = e; return $NEW;` | `NEW.col = e; return NEW;` | `NEW.col = e`; `return NEW` | `MOVE e TO NEW.col`; `GOBACK RETURNING NEW` |
| nested block `DECLARE` ³ | function-scoped | function-scoped | function-scoped | function-scoped | function-scoped |

³ Locals are function-scoped in every dialect, matching the source language's
scoping (Ruby method locals, JavaScript `var`, PHP function scope, Python
function scope, COBOL `WORKING-STORAGE`). plpgsql's per-block `DECLARE` is not a
separate construct; an exception block (`begin/rescue`, `try/catch`, `BEGIN-TRY`)
provides the nested `BEGIN/EXCEPTION/END` structure sharing the function scope.

## Notes

- Type inference: an integer, numeric, text, or boolean literal infers the local
  type in plxruby/plxphp/plxjs/plxpython3; otherwise annotate it (`#:: type`,
  `/*:: type */`). plxcobol declares types from `WORKING-STORAGE` `PICTURE`
  clauses or a `TYPE` clause.
- Trigger functions: a function returning `trigger` runs as a trigger in every
  dialect. `NEW`, `OLD`, and the `TG_` variables are available, and assignment to
  a record field (`NEW.col = e`) or array element is supported (the
  qualified/subscripted lvalue form; a bare `NEW = e` is not).

This file is updated as constructs land.
