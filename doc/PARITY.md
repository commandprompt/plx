# plx / plpgsql Feature Parity

Target: every plpgsql capability is expressible in each plx dialect (plxruby,
plxphp, plxjs, plxpython3). The reference is the plpgsql statement set in
`src/pl/plpgsql/src/plpgsql.h` (`PLpgSQL_stmt_*`) plus its declaration forms.

Legend: done / partial / todo. All four dialects track the same status unless
noted.

| plpgsql construct | plx status | dialect syntax |
|---|---|---|
| assignment (`:=`) | done | `x = e` / `$x = e;` / `x = e;` |
| `IF / ELSIF / ELSE` | done | if/elsif(elseif/else if)/else |
| `CASE` (simple + searched) | done | `case/when`, `switch/case` |
| `LOOP` | done | `loop do` / (while true) |
| `WHILE` | done | while / until |
| `FOR` integer | done | `for i in 1..n` / C-for |
| `FOR` over query | done | `query(sql).each` / `foreach`/`for..of` |
| `FOR` over dynamic query | done | same, non-literal SQL or binds |
| `EXIT` / `CONTINUE` (+ labels) | done | next/break (+ `WHEN`); `label: <loop>` and `break label` |
| `RETURN` / `RETURN NEXT` / `RETURN QUERY` | done | return / emit / return_query |
| `RAISE` | done | raise / throw |
| `ASSERT` | done | `assert(cond[, msg])` |
| `PERFORM` | done | `perform(sql)` |
| `EXECUTE` (dynamic) | done | `execute(sql, args)` |
| SQL statement / `SELECT INTO` | done | `fetch_one` / perform / execute |
| `GET DIAGNOSTICS` (ROW_COUNT) | done | `x = row_count()` |
| `FOUND` | done | `found()` / `found?` |
| `GET STACKED DIAGNOSTICS` | done | `e.message`, `e.sqlstate`, `e.detail`, `e.hint`, `e.constraint`, `e.column`, `e.table`, `e.schema`, `e.datatype` |
| `CALL` procedure | done | `call("proc", args)` |
| `COMMIT` / `ROLLBACK` | done | `commit()` / `rollback()` |
| `FOREACH` over array | done | `arr.each` / `foreach($a as $v)` / `for (v of arr)` (v annotated) |
| cursors: `OPEN`/`FETCH`/`MOVE`/`CLOSE` | done | `open_cursor(sql)`, `fetch_from(c)`, `move_cursor(c[,n])`, `close_cursor(c)` |
| declarations: `CONSTANT` | done | annotation suffix `const` (e.g. `x = 5 #:: int const`) |
| declarations: `%TYPE` / `%ROWTYPE` | done | annotation type text (e.g. `e #:: tbl%ROWTYPE`) |
| nested block with local `DECLARE` | by design | locals are function-scoped (matches Ruby method / JS var / PHP function scope); `begin/rescue` provides a nested BEGIN/EXCEPTION block sharing that scope |

This file is updated as constructs land.

## Notes

- Trigger functions: a function returning `trigger` runs as a trigger. `NEW`,
  `OLD`, and the `TG_` variables are available, and assignment to a record field
  (`NEW.col = e`) or an array element (`arr[i] = e`) is supported. This is the
  qualified/subscripted lvalue form; a bare `NEW = e` is not.
- Variable scope: plx locals are function-scoped, which matches the natural
  scoping of the source languages (Ruby method locals, JavaScript `var`, PHP
  function scope). plpgsql's per-block `DECLARE` is not exposed as a separate
  construct; a `begin/rescue` (`try/catch`) block gives the nested
  `BEGIN/EXCEPTION/END` structure and shares the function scope.
- Labels: `label: <loop>` before a loop and `break label` / `next label` lower
  to plpgsql `<<label>>` and `EXIT label` / `CONTINUE label`. The `label:` prefix
  is native in JavaScript and is accepted as a plx extension in Ruby and PHP.
