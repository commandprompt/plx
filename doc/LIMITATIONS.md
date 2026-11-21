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
  more: no filesystem access, no network access, no arbitrary native code. SQL
  runs with the privileges of the calling role, or of the function owner for a
  `SECURITY DEFINER` function, exactly as in plpgsql. This is the point of the
  design, and the reason plx is a trusted language while the native PL/Ruby or
  PL/PHP are not. Because plx is a syntax layer over plpgsql, USAGE on a plx
  language is equivalent to USAGE on plpgsql; see
  [Trust and security](USERGUIDE.md#trust-and-security).
- **Transaction control only in procedures.** A function runs inside the
  caller's transaction, so `COMMIT` and `ROLLBACK` raise `invalid transaction
  termination` there. In a `CREATE PROCEDURE` invoked with `CALL` (outside an
  explicit transaction block), `commit()`/`rollback()` (COBOL `COMMIT`/`ROLLBACK`,
  PL/SQL `COMMIT;`) work as in plpgsql. plxtsql and plxgo do not support
  transaction control. Savepoint management is not available in any dialect.
- **The signature is SQL.** The parameter and return types in `CREATE FUNCTION`
  are parsed by PostgreSQL before plx sees the body, so they must be PostgreSQL
  type names (`integer`, `numeric`, `text`, `timestamp`, ...). Dialect type names
  (`NUMBER`, `int64`, `let x: number`) apply only inside the body.
- **A value interpolated as an `UPDATE ... SET` target names a column.** In a
  static SQL call, a value interpolated where SQL needs a name is written in
  parentheses, so `FROM #{t}` fails at `CREATE FUNCTION`. `UPDATE t SET #{c} = v`
  becomes `SET (c) = v`, which PostgreSQL reads as a one-column list. With a
  plain value `v` the statement fails when it runs, with `source for a
  multiple-column UPDATE item must be a sub-SELECT or ROW() expression`. With
  a sub-`SELECT` or `ROW()` it updates the column named `c`, whatever the
  variable `c` holds. To choose the column at run time, write `#{ident(c)}`. See [Dynamic SQL](DYNAMIC-SQL.md).
- **Function-scoped locals.** plx hoists every local declaration into a single
  plpgsql `DECLARE` block, so locals are visible for the whole function. There is
  no per-block (per-`{}`, per-`if`) scope, and a name cannot be redeclared with a
  new type partway through. plxlua keeps Lua's block scope by giving a local
  that shadows another, or that is redeclared with a new type, its own plpgsql
  variable.
- **Names that differ only by case are one name.** plpgsql ignores case in
  names, so `total` and `Total` would be the same variable. In plxruby, plxphp,
  plxjs, plxts, plxpython3 and plxgo, where case matters, `CREATE FUNCTION`
  rejects a body that spells one name two ways, for example a local `A` beside
  a local or parameter `a`, a loop variable `I` beside a local `i`, or a loop
  label `outer` on a loop inside a loop labelled `Outer`. Rename one of them.
  SQL folds an unquoted parameter name to lower case, so a parameter declared
  as `userId` may be written `userId` in the body, as long as the body always
  spells it the same way. plxlua instead gives each spelling of a local its
  own plpgsql variable, as Lua treats them as different names.
- **No SQL reserved word as a variable name.** plx writes variable names into
  the generated SQL without quotes. A variable named after a reserved word,
  such as `array`, makes `CREATE FUNCTION` fail with a syntax error. When
  `check_function_bodies` is off, the function is created and fails with the
  syntax error when it runs. Rename the variable.
- **No NUL character in a string.** A PostgreSQL text value cannot hold the
  byte 0, so `CREATE FUNCTION` rejects a string escape that produces it, such as
  `\0` or `\x00`. Other escapes keep the meaning they have in the dialect:
  `"\x41"` is `A` and `"\v"` is a vertical tab.
- **Parentheses in a mix of bitwise operators.** SQL gives `&`, `|`, `#` (XOR),
  `<<` and `>>` one precedence and reads them from left to right. In plxruby,
  plxphp, plxjs, plxts and plxpython3, `CREATE FUNCTION` rejects a mix that the
  dialect would group differently, such as `a ^ b & c` or `a | b & c`. Write
  `a ^ (b & c)`. A mix that SQL groups the same way, such as `a & b | c`, is
  accepted.
- **One assignment per statement.** In plxruby, plxphp, plxjs, plxts and
  plxpython3, `CREATE FUNCTION` rejects a chained assignment such as
  `a = b = 0`, `a += b = 1` or `a = b <<= 2`. Assign each variable in its own
  statement.
- **SQL semantics, not the source language's.** Decimal literals are exact
  `numeric`, not IEEE 754 floating point. Comparisons use SQL three-valued logic,
  so a comparison involving NULL is unknown rather than false, and `==`/`===`
  become `=`. Integer division and modulo truncate toward zero. Conditions must
  be boolean expressions; source-language "truthiness" (a bare value used as a
  condition) is not emulated, except that plxlua treats a parameter or local
  of a known non-boolean type used as a condition as a nil test. A non-boolean condition is not rejected at
  `CREATE FUNCTION` time. When the function runs, plpgsql coerces it through
  its text form: a value whose text is a boolean literal (`0`, `1`, `t`, `yes`,
  `off`, ...) is silently accepted, and anything else raises `invalid input
  syntax for type boolean`. Always write an explicit comparison.
  Interpolating a NULL propagates it: the value is concatenated as-is, so the
  whole string becomes NULL the way SQL `||` does. A message being built for
  `RAISE` is an exception: it keeps each interpolated value as an empty string,
  so one NULL cannot swallow the message. The append operators follow the same
  rule. `s << x`, `$s .= $x` and `s += x` give NULL when x or s is NULL, as
  `s := s || x` does. This differs from PHP, where `.=` treats null as an empty
  string, and from JavaScript, where it appends the text "null". Use
  `coalesce(x, '')` to treat a NULL as empty. A PHP variable whose first use is
  `.=` starts as an empty string. A message built with appends is not covered
  by the RAISE exception: if a part was NULL, the raised message is `<NULL>`.
  plxcobol's `STRING-APPEND` does not follow the append rule: it treats a NULL
  as nothing to append, because COBOL has no NULL. Nor does plxcsharp's `+=`
  on a string: it treats a NULL as an empty string, as C# does.
- **A NULL errcode makes RAISE fail.** In plxruby, a NULL value in the
  `detail:`, `hint:`, `column:` or `constraint:` option of `raise` becomes an
  empty string, the same as in the message, so your error is still raised with
  its own SQLSTATE. A NULL `errcode:` is not replaced, because plx will not make
  up a SQLSTATE. The `raise` then fails with `RAISE statement option cannot be
  null` (SQLSTATE 22004) instead of raising your error. A NULL in an `assert`
  message replaces your message with plpgsql's generic `assertion failed`. Wrap
  such values in `coalesce(x, '')`.
- **String concatenation with `+` is not string concatenation.** In every dialect
  except plxtsql, plxgo and plxcsharp, `+` is SQL numeric addition. Use the dialect's string
  form: interpolation (Ruby/PHP/Python/JS/TS), `..` (Lua), `||` (PL/SQL), or `CONCAT(...)`.
  In plxcobol `||` is rejected. plxtsql writes `+` as `||` only when every
  operand is a known string, such as a `VARCHAR` variable or a string literal;
  see [plxtsql.md](plxtsql.md#string-concatenation-with). plxgo writes `+` as
  `||` only when every operand is a known string, such as a string literal or
  a `string` local; see [plxgo.md](plxgo.md#string-concatenation-with). In plxgo
  `||` is logical OR. plxcsharp resolves the operand types, so `+` there is
  `||` when an operand is known to be a string and numeric addition otherwise.
- **Type inference is limited.** Where a dialect infers a local's type from its
  initializer (`x = 5`, `x := 5`, `let x = ...`), plx infers only from a literal
  (int, decimal, string, boolean), an array literal, or a direct type
  conversion. plxlua also infers from a typed variable and from the operators
  its chapter lists. When the right-hand side is a call or a compound
  expression, declare the type explicitly (`x #:: int`, `var x T`, `let x: T`,
  `local x --:: int`, a `DECLARE`). plxcsharp's `var` also takes the type of
  a typed variable, a cast or a mapped .NET member.
- **String-builder acceleration needs PostgreSQL 18.** plx lowers the dialect
  append operators (`<<`, `.=`, `+=`, `STRING-APPEND`) onto `plx_strbuild` for
  amortized-O(1) building, as is plxlua's `s = s .. e` on a string local. The
  in-place path relies on a PostgreSQL 18 planner hook; on 13 through 17 the
  result is correct but the append is O(n^2), the same as plain concatenation. See [COMPATIBILITY.md](COMPATIBILITY.md).
- **Only `text` and `varchar` locals use the string builder.** The append
  operators turn a local into a `plx_strbuild` only when it has no declared type
  or is declared `text` or `varchar` without a length. A local declared with
  another string type, such as `varchar(20)`, `char(4)` or `citext`, keeps that
  type. Its append becomes `s := s || x`, so plpgsql still checks the length
  (`value too long for type character varying(20)`) and a NULL value makes the
  result NULL. That append is not accelerated. In plxruby, `<<` on an array
  local appends an element (`array_append`). In plxphp, `.=` on an array local
  is rejected at `CREATE FUNCTION` time.
- **Choose plx's schema when you install it.** Functions that use the append
  operators name the extension's `plx_strbuild` type and its append function
  with the schema plx is installed in, so they work whatever their
  `search_path` is. The schema name is stored in each such function, so:
  - `ALTER EXTENSION plx SET SCHEMA` fails with
    `extension "plx" does not support SET SCHEMA`. Give the schema in
    `CREATE EXTENSION plx SCHEMA ...`.
  - A function restored into a database where plx is in another schema (from
    a `pg_dump -n` of one schema, say) fails when it runs. The error is
    `schema "..." does not exist` when that schema is missing in the target,
    and otherwise `type "public.plx_strbuild" does not exist` or
    `function public.plx_tsql_int4(record) does not exist`, with the source
    schema in place of `public`. Install plx in the same schema in the target
    database, or run the function's `CREATE FUNCTION` again.
  - A function created before 3.0.0 keeps the unqualified names it was
    generated with. Until you run its `CREATE FUNCTION` again, it needs plx's
    schema on its `search_path`.

  plxtsql does the same for a conversion to an integer type (`CAST`,
  `CONVERT` or an assignment to an integer variable) of a value whose type it
  cannot see, such as a column or a function call: it calls `plx_tsql_int2`,
  `plx_tsql_int4` or `plx_tsql_int8` with plx's schema. Other functions are
  not affected. See [Trust and security](USERGUIDE.md#trust-and-security).
- **Runtime errors point at the generated plpgsql.** plpgsql requires all
  declarations at the top, so line numbers in a runtime error refer to the
  transpiled body, not your dialect source. `plx_source()` recovers the embedded
  original, and the mapping is usually close. See [DEBUGGING.md](DEBUGGING.md).
- **A depth cap guards pathological input.** Deeply nested expressions or
  statements (hundreds of levels) are rejected at `CREATE FUNCTION` time with a
  `nested too deeply` error rather than risking a stack overflow.
- **What is always reachable.** Every plpgsql statement type (assignment,
  `IF`/`CASE`, all loops, `RETURN`/set-returning, cursors, dynamic SQL, `RAISE`,
  exception handling, triggers, `GET DIAGNOSTICS`) is expressible from every
  dialect, except that plxtsql has no cursors or transaction control and plxgo
  has no transaction control. See
  [PARITY.md](PARITY.md) for the construct-by-construct matrix.

A dialect gives you its syntax over SQL semantics, not its standard library.
With the exception of the `fmt`/`strings`/`math`/`strconv` subset plxgo maps and
the `string`/`math`/`table` subset plxlua maps and the .NET subset plxcsharp
maps, a method or library call in a body is passed through and has to name a
function PostgreSQL actually has. `s.upcase`, `s.toUpperCase()`, `s.upper()` and
`strtoupper($s)` all transpile without complaint and then fail when the
function is called; write `upper(s)` instead. The same applies to any other
library call you would reach for out of habit. A call that happens to name a
PostgreSQL function runs that function, which can give a different answer
with no error: plxphp `count($arr)` is SQL's aggregate `count()` and returns
1 for any array. Write `cardinality($arr)`.

## What each dialect does not support

The per-dialect chapter is authoritative; this is a quick reference. Some
items are rejected at `CREATE FUNCTION` time; others (closures, literals of
unsupported types, library calls) are not translated, so they pass through and
fail when the function runs.

### plxruby ([chapter](plxruby.md))

- Method, class, and module definitions (`def`, `class`, `module`); gems.
- Blocks and lambdas beyond the recognized `.each` forms.
- Hash and array literals as general values.
- `||=` and `&&=`.
- `redo`, `retry`.
- Predicate and bang methods (`x.zero?`, `arr.empty?`, `s.strip!`); use the SQL
  form (`x = 0`, `cardinality(arr) = 0`).

### plxphp ([chapter](plxphp.md))

- Function, class, and namespace definitions; `use`, includes, closures.
- Array and object literals as general values.
- A `for` that is not a counting loop. Only `for ($v = LO; $v < HI; $v++)` and
  `+= K` steps, under the rules in the [chapter](plxphp.md#loops).
- `foreach` over a key/value pair: the key is ignored (value iteration only).
- `switch` fall-through (end each case with `break` or `return`).

### plxjs ([chapter](plxjs.md))

- Function, arrow-function, and class definitions; `import`.
- Object and array literals as general values.
- `for...in`. `for...of` treats anything other than `query(...)` as an array,
  so a non-array fails when the function runs.
- A `for` that is not a counting loop ([chapter](plxjs.md#loops)).
- Block scoping: `let`/`const` in an inner block assign the function-scoped
  variable of the same name.
- `switch` fall-through.

### plxts ([chapter](plxts.md))

- Everything plxjs rejects, plus type declarations (`interface`, `type`, `enum`),
  and type-only imports. Generic annotations (`Array<T>`) and nested arrays
  (`number[][]`) are not mapped; use `T[]` or a SQL type name.
- Multiple annotated declarators in one statement (`let a: number = 1, b = 2;`);
  use one declaration per statement.
- Annotations are not type-checked; they only set the plpgsql declaration type.

### plxpython3 ([chapter](plxpython3.md))

- `def`, `class`, `lambda`, decorators, comprehensions, generators.
- `import`, modules, and the Python standard library.
- Tuples, lists, dicts, and sets as general values.
- `match`/`case`, and the conditional expression `a if c else b`.
- String prefixes other than `f` (`r`, `b`, `u`).
- Floor division `//`; `/` on integers truncates toward zero.
- The `\N{name}` string escape; write `\u` or `\U` with the code point.
- A negative `range()` step held in a variable; write a negative step as a
  literal, such as `range(10, 0, -3)`.

### plxgo ([chapter](plxgo.md))

- Goroutines and channels (`go`, `chan`, `select`, `<-`), `defer`, `goto`.
- Labelled statements (`outer:` before a loop, `break outer`, `continue outer`).
  `CREATE FUNCTION` fails, with a syntax error that does not name the label.
  Use a flag variable and an unlabelled `break`.
- Wrap-around of integer types narrower than 64 bits, and unsigned types.
  `int8`, `int16`, `uint8` and `byte` are `smallint`, `int32` and `int` are
  `integer`, and the unsigned types are signed SQL types. A conversion keeps
  the value and arithmetic does not wrap: `uint8(300)` is 300 (Go: 44),
  `uint32(-1)` is -1 (Go: 4294967295), and a `byte` holding 255 incremented
  is 256 (Go: 0). A value that leaves the SQL type's range fails with
  `smallint out of range` or `integer out of range`. A left shift of a
  `smallint` drops the bits past 16 instead, so an `int8` holding 3 shifted
  by 14 is -16384 (Go: 0).
- `len(s)` of a string counts characters, as SQL `length()` does, where Go
  counts UTF-8 bytes: `len("héllo")` is 5 (Go: 6). Use `octet_length(s)` for
  the byte count.
- Nested functions and closures (`func`).
- `map`, `chan`, `struct`, and `interface` types; declare data with SQL types or
  slices.
- `switch` `fallthrough`.
- `make`, which plx rejects; write a slice literal or
  `array_fill(0, []int{n})`.
- `+` joins strings only when plx can see that every operand is a string: a
  string literal, a `string` local, a `text` or `varchar` parameter, or a call
  that returns a string. With a row field, an array, `jsonb`, `char(n)` or a
  SQL function call among the operands, `+` stays SQL `+`; write
  `"x" + string(r.a)` or `fmt.Sprintf("x%s", r.a)`.
- `for i := range s` over a slice; use `for _, v := range s` or a counting loop.
- Only a subset of `fmt`/`strings`/`math`/`strconv` is mapped; other calls pass
  through and must be valid PostgreSQL functions.
- `fmt.Sprintf` renders every operand in its SQL text form. A `-` flag and a
  width are kept, so `%-8d` still pads; as in Go, a longer operand prints in
  full. A `*` width or precision takes the next argument, as in Go. An explicit argument index (`%[1]d`) is rejected, in `fmt.Printf` too. Go's other flags and its precision field
  are dropped, so `%.2f` prints the operand in full rather than rounding it. The
  verbs that change an operand's representation rather than its padding do not
  do so either: `%x`, `%o`, `%b`, `%e` and `%q` all produce the operand's text,
  so `fmt.Sprintf("%x", 255)` yields `255` and not `ff`. Convert explicitly (for
  example `to_hex`) where the representation matters.

### plxlua ([chapter](plxlua.md))

- Function definitions (`function f()`, `local function`, anonymous functions)
  other than `pcall(function() ... end)`; closures.
- Metatables, coroutines, and tables as maps (keyed constructors, `pairs`).
- Multiple return values (except into OUT parameters), varargs (`...`),
  `select`.
- `goto` and labels other than the loop idioms (`goto continue` with
  `::continue::` ending the body, and a break target right after a loop).
- The `os`, `io`, `debug`, `package`, `coroutine` and `utf8` libraries; Lua
  patterns (`string.find`, `string.match`, `string.gsub`); only a subset of
  `string`, `math` and `table` is mapped.
- Lua's integer/float subtypes: `/` and `%` follow SQL (`7 / 2` is `3`, `-7 % 2`
  is `-1`), and `and`/`or` in a value give a boolean except for the
  `x or d`, `c and a or b` and `c and v or d` idioms.
- `ipairs` visits NULL elements instead of stopping at the first nil.

### plxcsharp ([chapter](plxcsharp.md))

- Methods, local functions, lambdas, classes, structs, records, interfaces,
  enums, namespaces, `using`, attributes, generics, preprocessor directives.
- `object`, `dynamic`, tuples, `ref`/`out` arguments, pointers, LINQ, and the
  collection classes (`List<T>`, `Dictionary<K,V>`); use arrays or SQL.
- `new` other than for arrays, `new StringBuilder()`, and the exception after
  `throw`.
- `goto`, labels, `lock`, `async`/`await`, `checked`/`unchecked`.
- `case` patterns (`case int n:`, `case > 5:`, `case null:`, `when` guards);
  switch-expression and `is` patterns beyond constants, `_`, `null`, `not null`
  and `< <= > >=`.
- Interpolation format specifiers other than `:F`, and alignment.
- `catch` of exception classes other than the mapped ones, and `when` filters
  other than `e.SqlState` comparisons.
- .NET members beyond the mapped subset (`string`, `Math`, `Convert`,
  `DateTime`, `Guid`, `StringBuilder`, ...).
- A NULL operand of a string `+` makes the result NULL, where C# treats null as
  an empty string; `string.Concat` keeps the C# behaviour.
- Local names that PostgreSQL or PL/pgSQL reserve (`user`, `order`, `limit`,
  `end`, ...), and two locals whose names differ only in case.

### plxcobol ([chapter](plxcobol.md))

- Program structure beyond a single procedure body: the `IDENTIFICATION`,
  `ENVIRONMENT`, and `CONFIGURATION` divisions, and named paragraphs with
  out-of-line `PERFORM <paragraph>`.
- Group items (subordinate entries under an `01`) in `WORKING-STORAGE`; declare
  elementary items, or use a `TYPE` clause for a composite type.
- Fixed-format source (column areas); use free format.
- Report Writer, screen sections, and object orientation. The standard COBOL
  intrinsic function library is not translated (a `FUNCTION name(...)` call
  passes through and fails when the function runs); use SQL functions in
  expressions.
- Truncation in `MOVE` to an alphanumeric item. A `PIC X(n)` item is
  `varchar(n)`, so `MOVE` of a longer string fails when it runs with
  `value too long for type character varying(n)`, where COBOL truncates on
  the right. Write `MOVE LEFT(s, n) TO item`.
- The `||` operator and `::` casts inside `COMPUTE` and conditions; use
  `CONCAT(...)` and `CAST(x AS type)`. `||`, like any character COBOL does not
  use (`!`, `$`, `@`, and others), is rejected at `CREATE FUNCTION` time
  outside a string literal or comment. A `::` cast fails when the function
  runs.

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
- Oracle exception names other than `NO_DATA_FOUND` and `TOO_MANY_ROWS`; use the
  PostgreSQL condition name (`division_by_zero` for `ZERO_DIVIDE`,
  `unique_violation` for `DUP_VAL_ON_INDEX`).
- `c%NOTFOUND`/`c%FOUND` cursor attributes; use `NOT FOUND`/`FOUND`.

### plxtsql ([chapter](plxtsql.md))

- `@@ROWCOUNT`, `@@ERROR`, and other `@@` globals (except `@@IDENTITY`); use `GET
  DIAGNOSTICS` or an exception handler.
- Table variables (`DECLARE @t TABLE`) and cursors (`DECLARE c CURSOR`).
- `GOTO`, `WAITFOR`, labels.
- Transaction control (`BEGIN TRAN`, `COMMIT`, `ROLLBACK`).
- Calling a stored procedure by name; only `EXEC('<sql>')` dynamic SQL.
- `+` for string concatenation (use `||` or `CONCAT(...)`).
- The range of `TINYINT`. plx stores a `TINYINT` as `smallint`, so a
  `TINYINT` variable and `CONVERT(TINYINT, x)` hold values outside 0 to 255:
  `CONVERT(TINYINT, 300)` is 300 and `CONVERT(TINYINT, -1)` is -1, where SQL
  Server raises an arithmetic overflow error. Check the range yourself where
  it matters.
- T-SQL precedence for `^`, `&` and `|` followed by `+` or `-`: PostgreSQL
  binds them more loosely, so `1 ^ 2 + 3` gives 4, not 6. Write
  `(1 ^ 2) + 3`.
- `SELECT @t = @t + v FROM t`, where the assignment list reads a variable it
  assigns; use an aggregate such as `SUM(v)` or `string_agg(...)`.
- The 30-character default for a string type without a length. SQL Server
  gives 30 characters to `VARCHAR`, `NVARCHAR` and `CHAR` in `CAST` and
  `CONVERT`. plx gives 30 only to `CAST(x AS nvarchar)`. `CAST(x AS varchar)`
  and `CONVERT(nvarchar, x)` keep every character, and `CAST(x AS char)` keeps
  1, because PostgreSQL `char` means `char(1)`. Write the length.
- Keeping the writes of a `TRY` block that fails. When an error reaches
  `CATCH`, plx undoes the database changes that the `TRY` block made, because
  plpgsql rolls an exception block back to a savepoint. SQL Server keeps them.
  Make a write that must survive before `BEGIN TRY` or in `CATCH`. See
  [A CATCH block undoes the writes made in TRY](plxtsql.md#a-catch-block-undoes-the-writes-made-in-try).
- Truncation in an implicit conversion to an integer type outside the function
  body. `CAST`, `CONVERT` and assignment to an integer variable truncate as in
  SQL Server, but `RETURN 2.7` from a function that returns `int` gives 3, and
  so does an `INSERT` or `UPDATE` of 2.7 into an `int` column. SQL Server gives
  2. Write `CAST(e AS int)`. See
  [Conversion to an integer type](plxtsql.md#conversion-to-an-integer-type).

## Working around a gap

Because every dialect compiles to the same plpgsql, you can write the awkward
part of a function in whichever dialect expresses it best, or drop to a SQL
statement inside any dialect (all of them pass SQL text through to `query`,
`execute`, `RETURN QUERY`, and the like). A construct a dialect rejects is
reported at `CREATE FUNCTION` time with a source line, so those gaps surface
immediately. `CREATE FUNCTION` then makes the checks plpgsql makes for its own
functions. It rejects pseudo-type arguments and results that plpgsql does not
allow, such as `cstring` or `internal`. When `check_function_bodies` is on (the
default), it also compiles the generated plpgsql, which rejects syntax errors
and trigger functions that declare arguments. This compile takes as long as
the first call's compile would, which can be seconds for a very large
function. `pg_restore` turns `check_function_bodies` off, as it does for
plpgsql. Library calls and
expressions passed through to SQL are checked for syntax only; names and types
in them are not checked until the function runs.
