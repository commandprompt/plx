# Gaps and limitations

plx transpiles a dialect function body to plpgsql at `CREATE FUNCTION` time, and
the standard plpgsql interpreter runs it. That design sets a hard boundary: a plx
function can express what plpgsql can express, in the surface syntax of the
dialect, and nothing more. This page collects the constraints that apply to every
dialect, then what each individual dialect does not support. Each per-dialect
chapter has the authoritative, complete list; the summaries here link to it.

Most of these are deliberate. plx pins semantics to SQL and plpgsql rather than
emulating the source language's runtime, so a function behaves the same no matter
which dialect it was written in.

## Constraints shared by every dialect

These follow from running as plpgsql and are not specific to any one dialect.

- **Trusted sandbox.** A plx function does what a plpgsql function can do and no
  more: no filesystem access, no network access, no arbitrary native code. All
  SQL runs with the privileges of the calling role. This is the point of the
  design, and the reason plx is a trusted language while the native PL/Ruby or
  PL/PHP are not.
- **No transaction control.** A function runs inside the caller's transaction, so
  `COMMIT`, `ROLLBACK`, `BEGIN TRANSACTION`, and savepoint management are not
  available. This is a plpgsql function boundary, not a plx choice.
- **The signature is SQL.** The parameter and return types in `CREATE FUNCTION`
  are parsed by PostgreSQL before plx sees the body, so they must be PostgreSQL
  type names (`integer`, `numeric`, `text`, `timestamp`, ...). Dialect type names
  (`NUMBER`, `int64`, `let x: number`) apply only inside the body.
- **Function-scoped locals.** plx hoists every local declaration into a single
  plpgsql `DECLARE` block, so locals are visible for the whole function. There is
  no per-block (per-`{}`, per-`if`) scope, and a name cannot be redeclared with a
  new type partway through.
- **SQL semantics, not the source language's.** Decimal literals are exact
  `numeric`, not IEEE 754 floating point. Comparisons use SQL three-valued logic,
  so a comparison involving NULL is unknown rather than false, and `==`/`===`
  become `=`. Integer division and modulo truncate toward zero. Conditions must
  be boolean expressions; source-language "truthiness" (a bare value used as a
  condition) is not emulated. Interpolating a NULL yields an empty string, and
  never turns the whole string NULL.
- **String concatenation with `+` is not string concatenation.** In every dialect
  `+` is SQL numeric addition. Use the dialect's string form: interpolation
  (Ruby/PHP/Python/JS/TS), `||`, or `CONCAT(...)`.
- **Type inference is limited.** Where a dialect infers a local's type from its
  initializer (`x = 5`, `x := 5`, `let x = ...`), plx infers only from a literal
  (int, decimal, string, boolean), an array literal, or a direct type
  conversion. When the right-hand side is a call or a compound expression, declare
  the type explicitly (`x #:: int`, `var x T`, `let x: T`, a `DECLARE`).
- **String-builder acceleration needs PostgreSQL 18.** plx lowers the dialect
  append operators (`<<`, `.=`, `+=`, `STRING-APPEND`) onto `plx_strbuild` for
  amortized-O(1) building. The in-place path relies on a PostgreSQL 18 planner
  hook; on 13 through 17 the result is correct but the append is O(n^2), the same
  as plain concatenation. See [COMPATIBILITY.md](COMPATIBILITY.md).
- **Runtime errors point at the generated plpgsql.** plpgsql requires all
  declarations at the top, so line numbers in a runtime error refer to the
  transpiled body, not your dialect source. `plx_source()` recovers the embedded
  original, and the mapping is usually close. See [DEBUGGING.md](DEBUGGING.md).
- **A depth cap guards pathological input.** Deeply nested expressions or
  statements (hundreds of levels) are rejected at `CREATE FUNCTION` time rather
  than risking a stack overflow.
- **What is always reachable.** Every plpgsql statement type is expressible from
  every dialect (assignment, `IF`/`CASE`, all loops, `RETURN`/set-returning,
  cursors, dynamic SQL, `RAISE`, exception handling, triggers, `GET
  DIAGNOSTICS`). See [PARITY.md](PARITY.md) for the construct-by-construct matrix.

## What each dialect does not support

The per-dialect chapter is authoritative; this is a quick reference.

### plxruby ([chapter](plxruby.md))

- Method, class, and module definitions (`def`, `class`, `module`); gems.
- Blocks and lambdas beyond the recognized `.each` forms.
- Hash and array literals as general values.
- `||=`, `&&=`, and `and`/`or` in value position.
- `redo`, `retry`.
- Predicate and bang methods (`x.zero?`, `arr.empty?`, `s.strip!`); use the SQL
  form (`x = 0`, `cardinality(arr) = 0`).

### plxphp ([chapter](plxphp.md))

- Function, class, and namespace definitions; `use`, includes, closures.
- Array and object literals as general values.
- Non-counting `for`; only `for ($v = LO; $v < HI; $v++)` and `+= K` steps.
- `foreach` over a key/value pair (value iteration only).
- `switch` fall-through (end each case with `break` or `return`).

### plxjs ([chapter](plxjs.md))

- Function, arrow-function, and class definitions; `import`.
- Object and array literals as general values.
- `for...in`, and `for...of` over anything other than `query(...)` or an array.
- `switch` fall-through.

### plxts ([chapter](plxts.md))

- Everything plxjs rejects, plus type declarations (`interface`, `type`, `enum`),
  generics beyond `T[]`, and type-only imports.
- Multiple annotated declarators in one statement (`let a: number = 1, b = 2;`);
  use one declaration per statement.
- Annotations are not type-checked; they only set the plpgsql declaration type.

### plxpython3 ([chapter](plxpython3.md))

- `def`, `class`, `lambda`, decorators, comprehensions, generators.
- `import`, modules, and the Python standard library.
- Tuples, lists, dicts, and sets as general values.
- `match`/`case`, and the conditional expression `a if c else b`.

### plxgo ([chapter](plxgo.md))

- Goroutines and channels (`go`, `chan`, `select`, `<-`), `defer`, `goto`.
- Nested functions and closures (`func`).
- `map`, `chan`, `struct`, and `interface` types; declare data with SQL types or
  slices.
- `switch` `fallthrough`.
- `+` for string concatenation (use `||` or build a slice and `array_to_string`).
- Only a subset of `fmt`/`strings`/`math`/`strconv` is mapped; other calls pass
  through and must be valid PostgreSQL functions.

### plxcobol ([chapter](plxcobol.md))

- Program structure beyond a single procedure body: the `IDENTIFICATION`,
  `ENVIRONMENT`, and `CONFIGURATION` divisions, and named paragraphs with
  out-of-line `PERFORM <paragraph>`.
- Group items (subordinate entries under an `01`) in `WORKING-STORAGE`; declare
  elementary items, or use a `TYPE` clause for a composite type.
- Fixed-format source (column areas); use free format.
- Report Writer, screen sections, object orientation, and the standard COBOL
  intrinsic function library (use SQL functions in expressions).

### plxplsql ([chapter](plxplsql.md))

- Packages, and package-qualified calls other than `DBMS_OUTPUT`; the wider
  `DBMS_*` / `UTL_*` library.
- Collections and associative arrays, `BULK COLLECT`, `FORALL`, pipelined
  functions, records beyond `%ROWTYPE`.
- Autonomous transactions and other pragmas.
- Oracle-only SQL (`(+)`, `CONNECT BY`, `DECODE`, Oracle hints); use the
  PostgreSQL equivalent.
- Oracle spellings inside a `DBMS_OUTPUT.PUT_LINE`/`RAISE_APPLICATION_ERROR`
  argument, schema-qualified `seq.NEXTVAL`, nested-`DECLARE` type names, and
  parameterized cursors.

### plxtsql ([chapter](plxtsql.md))

- `@@ROWCOUNT`, `@@ERROR`, and other `@@` globals (except `@@IDENTITY`); use `GET
  DIAGNOSTICS` or an exception handler.
- Table variables (`DECLARE @t TABLE`) and cursors (`DECLARE c CURSOR`).
- `GOTO`, `WAITFOR`, labels.
- Transaction control (`BEGIN TRAN`, `COMMIT`, `ROLLBACK`).
- Calling a stored procedure by name; only `EXEC('<sql>')` dynamic SQL.
- `+` for string concatenation (use `||` or `CONCAT(...)`).
- Mutating a trigger's `NEW` fields. `SET NEW.col = e` is read as a session
  option, and `NEW.col := e` does not parse, so a plxtsql trigger can validate or
  reject a row (for example with `THROW`) but not rewrite it. Write a trigger that
  rewrites `NEW` in another dialect.

## Working around a gap

Because every dialect compiles to the same plpgsql, you can write the awkward
part of a function in whichever dialect expresses it best, or drop to a SQL
statement inside any dialect (all of them pass SQL text through to `query`,
`execute`, `RETURN QUERY`, and the like). A construct a dialect rejects is
reported at `CREATE FUNCTION` time with a source line, so gaps surface
immediately rather than at run time.
