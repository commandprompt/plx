# plx Limitations

Every plx function body is transpiled to plpgsql at `CREATE FUNCTION` time and
stored in `pg_proc.prosrc`. Execution is performed by the standard plpgsql
interpreter. Constructs that have no plpgsql lowering are rejected at
`CREATE FUNCTION` time with an error and a source line number.

Status: three dialects are implemented and covered by the pg_regress suite
(`test/sql/plxruby.sql`, `test/sql/plxphp.sql`, `test/sql/plxjs.sql`, plus
`test/sql/plx_errors.sql`): `plxruby` (Ruby), `plxphp` (PHP), and `plxjs`
(JavaScript). All run on PostgreSQL 18.4.

The constructs in the table below are supported in all three dialects, each with
its own syntax. The Ruby and PHP columns are shown; the plxjs equivalents follow
JavaScript syntax (brace blocks, `let`/`const`/`var`, template literals) and are
described in the per-dialect notes.

## Supported constructs

| Capability | Ruby (`plxruby`) | PHP (`plxphp`) | plpgsql lowering |
|---|---|---|---|
| Return a value | `return e` | `return $e;` | `RETURN e;` |
| Local variable | `x = e` | `$x = e;` | DECLARE + `:=` (constant literal first-assignment is folded into the DECLARE) |
| Type annotation | `x #:: numeric` | `$x = e /*:: numeric */;` | typed DECLARE |
| Type inference | integer, numeric, text, boolean literals | same | inferred DECLARE type |
| Conditionals | `if/elsif/else`, `unless` | `if/elseif/else` (and `else if`) | `IF/ELSIF/ELSE/END IF` |
| While loop | `while`, `until` | `while` | `WHILE ... LOOP` |
| Counting loop | `for i in 1..n`, `1...n` | `for ($i=LO; $i<=HI; $i++)` | `FOR i IN LO..HI LOOP` |
| Range iteration | `(1..n).each do \|i\|` | (use `for`) | `FOR i IN 1..n LOOP` |
| Loop control | `next`, `break` (and modifiers) | `continue`, `break` | `CONTINUE`, `EXIT` (with `WHEN`) |
| Query iteration | `query(sql).each do \|row\|` | `foreach (query(sql) as $row)` | `FOR row IN <sql> LOOP` (row is RECORD) |
| Query with binds | `query(sql, a, b).each` | `foreach (query(sql, $a) as $row)` | `FOR row IN EXECUTE sql USING a LOOP` |
| Fetch one row | `u = fetch_one(sql)` | `$u = fetch_one(sql);` | `SELECT * INTO u FROM (sql) t;` |
| Fetch strict | `fetch_one!(sql)` | `fetch_one!(sql)` | `SELECT ... INTO STRICT` |
| Run SQL, discard | `perform(sql)` | `perform(sql);` | DML verbatim, or `PERFORM * FROM (sql) t` |
| Dynamic SQL | `execute(sql, a)` | `execute(sql, $a);` | `EXECUTE sql USING a;` |
| Set return | `emit`, `return_next e` | `emit()`, `return_next($e);` | `RETURN NEXT [e];` |
| Return a query | `return_query(sql)` | `return_query(sql);` | `RETURN QUERY sql;` |
| Exceptions | `begin/rescue/ensure` | `try/catch/finally` | `BEGIN ... EXCEPTION ... END` |
| Exception info | `e.message`, `e.sqlstate` | `$e->message`, `$e->sqlstate` | `SQLERRM`, `SQLSTATE` |
| Raise or throw | `raise level: "m", errcode: "X"` | `throw new Exception("m")` | `RAISE ... USING ERRCODE = 'X'` |
| String interpolation | `"a #{e} b"` | `"a $e b"`, `"a {$e} b"` | `'a ' \|\| (e)::text \|\| ' b'` |
| Concatenation | (interpolation) | `$a . $b` | `a \|\| b` |
| Comparison | `==`, `!=` | `==`, `===`, `!=`, `!==` | `=`, `<>`; `x == nil`/`x == null` becomes `x IS NULL` |
| Boolean logic | `&&`, `\|\|`, `!`, `and`, `or`, `not` | `&&`, `\|\|`, `!` | `AND`, `OR`, `NOT` |
| Ternary | `c ? a : b` | `c ? a : b` | `CASE WHEN c THEN a ELSE b END` |
| Record field | `row.col`, `row[:col]`, `row['col']` | `$row->col`, `$row['col']` | `row.col` |

Expressions that are not in the list above are passed through to plpgsql and SQL
unchanged, so any SQL-valid expression, operator, function call, or cast works.

## Rejected constructs

Rejected at `CREATE FUNCTION` time with a line number.

| Rejected | Reason |
|---|---|
| Method or function definitions (`def`, `function`) | The engine is plpgsql. Define separate SQL functions instead. |
| Classes, objects, modules, packages, imports | No plpgsql representation. |
| Blocks, closures, lambdas (beyond the recognized `.each`/`foreach` forms) | plpgsql has no first-class functions. |
| Hash and array literals as values | No direct plpgsql representation. |
| `case`/`when`, `switch`/`case` | Use `if`/`elsif`. |
| Non-boolean loop or branch conditions | plpgsql conditions must be boolean. Truthiness of `0`, `""`, `[]` is not emulated. Write an explicit comparison. |
| Ruby `\|\|=`, `&&=`; keyword `and`/`or` in value position | Cannot be lowered while preserving the source semantics. |
| Multiple assignment (`a, b = ...`) | Not yet implemented. |
| Non-counting `for` loops in PHP | Only `for ($v=LO; $v</<=HI; $v++/$v+=K)` is supported. |
| `foreach` over an array in PHP | Only `foreach (query(...) as $row)` is supported. |
| Heredocs, `%w`/`%q` literals, PHP nowdoc | Not yet implemented. |
| Loop variable referenced after the loop | plpgsql integer loop variables are local to the loop. |

## Semantic differences from the source language

These are intentional. plx pins semantics to SQL and plpgsql.

- Decimal literals infer `numeric`, not a floating-point type. Arithmetic is exact.
- Comparisons use SQL three-valued logic. `==` and `!=` are lowered null-aware.
  Positive `if`/`while` conditions treat NULL as false, matching plpgsql.
- Integer division and modulo follow SQL rules.
- In Ruby, `String#+` remains SQL numeric `+`. Use interpolation for concatenation.
- In PHP, `.` is string concatenation and maps to `||`.

## Per-dialect notes

### plxruby
- Type annotation is `#:: type` at the end of a line.
- `query`, `fetch_one`, `perform`, `execute`, `return_query`, `emit` are intrinsics.
- Exception class mapping: `PG::UniqueViolation` maps to `unique_violation`, and similar for other listed classes.

### plxphp
- Variables use the `$` sigil, which is removed in the generated plpgsql.
- Type annotation is a `/*:: type */` block comment placed before the `;`.
- Only counting `for` loops and `foreach (query(...) as $row)` are supported.
- `try/catch/finally` maps to `BEGIN/EXCEPTION/END`. `catch (\Exception $e)` maps to `WHEN OTHERS`. `$e->message` and `$e->sqlstate` map to `SQLERRM` and `SQLSTATE`.
- `throw new Exception("msg")` maps to `RAISE EXCEPTION`.
- `raise('notice'|'warning'|'info'|'log'|'debug'|'exception', message)` emits a `RAISE` at that level; `raise(message)` raises an exception. This call form is also accepted in plxruby (in addition to the `raise notice: "..."` keyword form).

### plxjs
- Blocks are brace-delimited. Declarations use `let`, `const`, or `var`, which are stripped in the generated plpgsql.
- Type annotation is a `/*:: type */` block comment placed before the `;`.
- Interpolation uses template literals: `` `text ${expr} text` ``. Plain `"..."` and `'...'` strings do not interpolate. Use template literals for string building; `+` remains numeric addition.
- Loops: C-style `for (let i = LO; i < HI; i++)`, `while`, and `for (const row of query(...))` for row iteration.
- `try/catch/finally` maps to `BEGIN/EXCEPTION/END`. `catch (e)` maps to `WHEN OTHERS`; `e.message` and `e.sqlstate` map to `SQLERRM` and `SQLSTATE`. `throw new Error("msg")` maps to `RAISE EXCEPTION`.
- `raise('notice'|'warning'|..., message)` emits a `RAISE` at that level.
- `===`/`!==` and `==`/`!=` all lower to `=`/`<>`; `null` and `undefined` lower to `NULL`.

## Other dialects
- `plxpython3` (Python) is planned. Python is indentation-based and requires an INDENT/DEDENT block mode in the shared framework.

## References
- `ARCHITECTURE.md`: design and the verified plpgsql symbol table.
- `TRANSPILER.md`: transpiler specification.
