# Changelog

All notable changes to plx are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/), and plx uses the extension
version in `plx.control` (currently `3.1.0`).

## [3.1.0] - unreleased

3.1.0 adds two dialects, plxlua and plxcsharp. Nothing that 3.0.0 does
changes: functions in the other dialects transpile as before.

### Added

- **plxlua, a Lua dialect** (`LANGUAGE plxlua`). Lua's keyword-`end` blocks,
  `local` declarations with type inference and `--::` annotations, `<const>`,
  `if`/`elseif`, `while`, `repeat ... until`, the numeric `for` (any step) and
  `for ... in query(...)` / `ipairs(...)`, the `goto continue` idiom, and
  `return` into OUT parameters. Errors are raised with `error(...)`,
  `error{message = ..., sqlstate = ...}`, `assert` and `print`, and handled
  with `pcall(function() ... end)`, whose `err` carries `err.sqlstate` and the
  other diagnostics fields. In expressions: `..`, `~=`, `nil`, `#`, table
  constructors as arrays, `//`, and the `x or d`, `c and a or b` and
  `c and v or d` idioms, with a subset of the `string`, `math` and `table`
  libraries. Locals keep Lua's block scope, and `s = s .. e` uses the string
  builder. See [doc/plxlua.md](doc/plxlua.md).
- **plxcsharp, a C# dialect** (`LANGUAGE plxcsharp`). Declarations put the type
  first and C# types map to PostgreSQL types (`int` to `integer`, `string` to
  `text`, `decimal` to `numeric`, `DateTime` to `timestamp`, `T[]` to arrays);
  `var` takes the type of its value. Regular, verbatim (`@"..."`), raw
  (`"""..."""`) and interpolated (`$"..."`) strings work, and an interpolated
  string passed to `query`, `execute` and the other SQL intrinsics is a SQL
  template whose values are bound. `+` is string concatenation when an operand
  is known to be a string. Statements: `if`, `while`, `do`/`while`, `for`,
  `foreach` over a query, an array or a string, `switch` and switch
  expressions, `try`/`catch`/`finally` with exception classes and
  `when (e.SqlState == ...)` filters, `throw`, `yield return` for
  set-returning functions, `Console.WriteLine` as `RAISE NOTICE`, and
  `StringBuilder` and `s += x` on the string builder, with a set of mapped
  .NET members (`string`, `Math`, `Convert`, `DateTime`, `Guid`, ...). See
  [doc/plxcsharp.md](doc/plxcsharp.md).
- `make oraclecheck` runs plxlua and plxcsharp programs and compares their
  results with what real Lua and C# print for the same programs.

### Upgrading

These steps are for a database administrator who upgrades plx from 3.0.0 to
3.1.0. You need superuser access to each database that has the plx
extension.

1. Install the new build on the server.
2. Restart the server, or start new sessions, so that they load the new build.
3. In each database that has plx, run:

   ```sql
   ALTER EXTENSION plx UPDATE;
   ```

   The update adds the `plxlua` and `plxcsharp` languages and their handler
   functions. It changes no existing object. From 2.0.0, the same command
   updates through 3.0.0 to 3.1.0; read the 3.0.0 Upgrading steps first.

To check the result, run
`SELECT extversion FROM pg_extension WHERE extname = 'plx';`. It returns
`3.1.0`.

## [3.0.0] - 2026-11-20

3.0.0 is a major release. It holds the fixes from a security audit and from a
code audit of every dialect. Many fixes make a function compute what its
source language computes, so a function can give a different result once you
re-create it under 3.0.0. Other fixes reject code that 2.0.0 accepted. Read
Breaking changes and Upgrading before you install this release. The fixes
first planned for a 2.1.0 release are part of this release; there is no
2.1.0, and 2.0.0 updates straight to 3.0.0.

### Breaking changes

Each entry below changes what a function computes, rejects code that 2.0.0
accepted, or stops an operation that used to work. A function that already
exists keeps the plpgsql it was created with. It behaves as before until you
run its `CREATE FUNCTION` again (see Upgrading).

#### All dialects

- **`CREATE FUNCTION` makes plpgsql's own checks.** Every plx language now
  rejects result and argument types that plpgsql does not allow, such as
  `cstring` or `internal`. When `check_function_bodies` is on, it also
  compiles the generated plpgsql. A body that plpgsql cannot parse, or a
  trigger function that declares arguments, now fails at `CREATE FUNCTION`
  instead of at every call. For a very large function this compile can take
  seconds. `pg_restore` runs with `check_function_bodies` off, so it still
  restores such functions.
- **The extension is no longer relocatable** (`relocatable = false`).
  Functions now store plx's schema name, so `ALTER EXTENSION plx SET SCHEMA`
  would leave them pointing at the old schema. It now fails with
  `extension "plx" does not support SET SCHEMA`. Choose the schema at
  `CREATE EXTENSION plx SCHEMA ...` time. Existing functions keep the
  unqualified names they were generated with until their DDL is run again.
  For the same reason, a function that uses a string append, or a plxtsql
  conversion to an integer type of a value whose type plx cannot see (which
  calls `plx_tsql_int2`, `plx_tsql_int4` or `plx_tsql_int8`), and is restored
  into a database where plx is in a different schema (a `pg_dump -n` of one
  schema, say) fails when it runs: with
  `schema "..." does not exist` when that schema is missing in the target,
  and otherwise with `type "public.plx_strbuild" does not exist` or
  `function public.plx_tsql_int4(record) does not exist` (with the source
  schema's name in place of `public`). Install plx in the same
  schema in the target, or run the function's DDL again after restoring.
  Other functions are not affected.
- **`CREATE FUNCTION` and `DO` in a plx language need USAGE on plpgsql.** A
  role without that privilege now gets `permission denied for language
  plpgsql`. See "Revoking plpgsql now revokes plx" under Security.
- **A declared type must be a type name.** A type annotation or declared type
  that is not a type name, `name%TYPE` or `name%ROWTYPE` is now rejected at
  `CREATE FUNCTION`. See "A type annotation could run code on entry" under
  Security.

#### Several dialects

Each entry names the dialects it affects.

- **An interpolated value in a SQL argument is bound, never spliced as SQL.**
  In `query`, `fetch_one`, `execute`, `open_cursor`, `return_query` and
  `perform`, an interpolation in the SQL string (`#{x}`, `{$x}`, `${x}`,
  f-string `{x}`) used to be concatenated into dynamic SQL unquoted, so data
  could inject SQL. Adding a bind argument to a safe static call silently made
  it injectable. Now a value becomes a `$n` parameter passed with `USING`
  (after the call's own bind arguments). In a statement that takes no
  parameters (`CREATE`, `ALTER`, `SET`, `COMMENT`, ...), the value is spliced
  through `quote_nullable()`. `'#{x}'` is the value of `x` in static and
  dynamic SQL alike (it used to be the literal text `'x'` in static SQL), and
  an interpolation inside a longer literal concatenates the value. `'#{x}'` now has the type of `x`; before, it was an untyped literal that
  took the type its context needed. So with a `text` variable, `ts > '#{since}'`
  on a `date` column now fails with `operator does not exist: date > text`;
  write `'#{since}'::date`. A literal after a built-in type name, such as
  `interval '#{n} days'`, becomes `CAST(('' || (n)::text || ' days') AS interval)`,
  and `interval '#{s}'` becomes `CAST((s)::text AS interval)`. An interval
  field after such a literal, `interval '#{n}' hour`, is rejected: write
  `interval '#{n} hours'`. An
  interpolation inside a quoted identifier, a SQL comment, a dollar-quoted
  string or a prefixed literal (`U&'...'`, `N'...'`, `text'...'`) is
  rejected, and so is one joined to a SQL name or `$` sign (`v#{x}`), and one
  right after an unpaired backslash in an `E'...'` string (the SQL text
  `E'a\#{x}'`, written `E'a\\#{x}'` in a dialect string), where
  PostgreSQL would read the quote plx writes next as an escaped quote. The
  lowering reads the SQL string as PostgreSQL does, so a quote inside a
  comment or a dollar-quoted string does not open a string. A literal that
  contains a backslash is written as `E'...'` in dynamic SQL too, so the SQL
  reads the same whatever `standard_conforming_strings` the caller uses. A
  value interpolated where SQL needs a name (`FROM #{t}`) is now an error
  until the body uses `ident()` or `raw()` (see Added). In static SQL the
  value is written in parentheses, `FROM (t)`, so `CREATE FUNCTION` fails with
  a syntax error; before, it read the table named `t` whatever the variable
  held. One static position is not caught: `UPDATE t SET #{c} = ...`
  becomes `SET (c) = ...`, a one-column list: with a sub-`SELECT` on the
  right it still updates the column named `c`, as 2.0.1 did; see
  doc/LIMITATIONS.md. In dynamic SQL the statement fails when it runs, with
  `syntax error at or near "$n"`. A value interpolated to build SQL text on
  purpose is now bound as data. See doc/DYNAMIC-SQL.md.
- **An expression interpolated in static SQL is one value** (plxruby,
  plxphp, plxjs, plxts, plxpython3). In a static call, an expression interpolated outside quotes is now written in
  parentheses, so the SQL around it cannot split it, as in dynamic SQL, which
  binds the whole value. 2.0.1 wrote it bare: `SELECT ${a + b} * 10` (plxjs,
  and `#{a + b} * 10` in plxruby) ran as `SELECT a + b * 10` and gave 21 for
  `a = 1, b = 2`. It now runs as `SELECT (a + b) * 10` and gives 30. In the
  same way, plxpython3 `f"SELECT {a or b} AND false"` gave true for
  `a = true, b = false` and now gives false.
- **`finally`/`ensure` now runs on `return`, `break` and `continue`** (plxjs,
  plxts, plxphp, plxruby, plxpython3), as in the source languages. Before, a
  `return` inside `try` left the function without running `finally`. The
  finally body is now emitted once. An exit from the try (or from a catch arm)
  records which exit is pending, leaves the block, runs the finally body, then
  completes the exit. An error is caught, the finally body runs, and the error
  is re-raised with its SQLSTATE, message, detail, hint and object names
  (schema, table, column, constraint, type). A `return` or an error in the
  finally body replaces the pending exit or error, as in the source languages.
  Two differences remain. An error re-raised after `finally` carries the
  location of the re-raise in its `CONTEXT`, not the original statement's.
  When it names any object, the object names it does not have are sent as
  empty fields rather than left out (psql shows them in verbose mode). As
  before, the finally body does not run when the statement is cancelled (a
  query cancel or `statement_timeout`) or an `assert` fails: `WHEN OTHERS` does
  not catch those two errors.
- **Appending a NULL to a string makes the string NULL** (plxruby, plxphp,
  plxjs, plxts, plxpython3). `s << x`, `$s .= $x` and `s += x` on a string now
  give the same result as `s := s || x`. If `x` is NULL, or `s` is already
  NULL, `s` becomes NULL and stays NULL until you assign it again. Before, a
  NULL value was skipped and a NULL string started again from empty, so
  `$s .= "id:{$x} "` could drop a whole segment without any sign. In plxphp, a
  variable whose first use is `.=` starts as an empty string, as in PHP.
  plxcobol `STRING-APPEND` does not change. To treat a NULL as empty, append
  `coalesce(x, '')`. A message built with appends and then raised now shows
  `<NULL>` if any part was NULL.
- **An append keeps a typed local's type** (plxruby, plxphp, plxjs, plxts,
  plxpython3). An append (`<<`, `.=`, `+=`) no longer turns a local declared
  `varchar(n)`, `char(n)`, `citext` or an array into a string builder. Such a
  local keeps its type, so a length limit is enforced, and an append to a
  `varchar(n)` local can now raise `value too long`. A NULL appended to such a
  local makes it NULL, as SQL `||` does. Ruby `<<` on an array appends an
  element, and PHP `.=` on an array is rejected at `CREATE FUNCTION`.
- **A `switch` with no default arm does nothing when no arm matches.** This
  covers `switch` (plxjs, plxts, plxphp), `case` (plxruby) and `EVALUATE`
  (plxcobol). Before, it raised `case not found`, because a plpgsql `CASE`
  statement without `ELSE` does. It now does nothing, as in the source
  language. plxplsql keeps the error, which is also Oracle's behaviour.
- **An interpolated string as a local's first value runs in place.** The first
  top-level assignment of a string literal is folded into the `DECLARE`
  section as the variable's initializer, which runs when the function starts.
  An interpolating string (Ruby `"#{v}"`, PHP `"$v"`, JS `` `${v}` ``, Python
  `f"{v}"`) was folded too, so it read its variables before the statements
  above it had run: `v = "v0"; v = "v2"; s = "got #{v}"` returned `got v0`.
  Such an assignment now runs in place. A plain literal is still folded.
- **A constant may not read a local variable.** A constant becomes a plpgsql
  `CONSTANT`, computed when the function starts, so
  `v = 1; v = 2; c = v + 1 #:: int const` gave `c = 2`. A constant may now use
  only literals, parameters and earlier constants. Reading another local is
  rejected at `CREATE FUNCTION`.
- **A block annotation before an initializer is rejected** (plxjs, plxphp).
  `let n /*:: bigint */ = 3;` silently dropped the initializer, so `n` was
  NULL. That form is now rejected with a hint to write
  `let n = 3 /*:: bigint */;`.
- **Names that differ only in case are rejected** (plxruby, plxphp, plxjs,
  plxts, plxpython3, plxgo). `CREATE FUNCTION` now rejects a body that spells
  one local or parameter two ways (`a` and `A`, or `N` beside parameter `n`).
  plx used to merge them into one plpgsql variable, so a second declaration
  overwrote the first and an assignment to `N` overwrote parameter `n`. A
  counting-loop variable that spells an earlier local or parameter another
  way, such as `for (let I = ...)` beside `let i`, is rejected too; before, it
  hid the local. Rename one of the names.
- **Nested loop labels that differ only in case are rejected** (plxjs, plxts,
  plxphp, plxpython3, plxruby). plpgsql ignores case in labels too, so in
  `Outer: for ... { outer: for ... { continue Outer; } }` the `continue` went
  to the inner loop and the function returned a wrong value. `CREATE FUNCTION`
  now rejects a loop label that spells the label of an enclosing loop another
  way. Rename one of the labels.
- **An escaped interpolation in SQL text is plain text** (plxphp, plxjs,
  plxts, plxruby). In the SQL argument of `query`, `fetch_one` and the other
  SQL calls, `\$x`, `\${x}` and `\#{x}` put a backslash and the value of `x`
  into the SQL, so plxphp `fetch_one("SELECT '\$x' AS v")` returned `\5`
  where `$x` held 5. They now put the text `$x`, `${x}` or `#{x}` there, as
  in a string value. A plxpython3 f-string is unchanged: `\{x}` keeps the
  backslash and still interpolates, as in Python.
  plxphp `"\{$x}"` keeps the backslash and the braces and interpolates
  `$x`, as PHP does: with `$x` 5 it gives `\{5}`, in a string value and in
  SQL. Before, it gave `{5}` in both.
- **A quote or brace inside an interpolation does not end it** (plxruby,
  plxpython3, plxjs, plxts, plxphp). plx ended an interpolated expression at
  the first `}`, even inside a string in the expression, so plxpython3
  `f"{'}'}"` gave `'}` instead of `}`. It now gives `}`. Other forms that
  failed now work: the Ruby idiom `"#{s == "a" ? "A" : "other"}"`, Python
  `f"{concat('}', x)}"`, and `?`, `:` and `**` inside a string nested in an
  interpolation, such as Ruby `"[#{"a?b:c"}]"`, which are now text.
- **An integer literal with a leading zero is octal** (plxruby, plxphp,
  plxjs), as in the source language, so `0755` is 493, not 755. plxpython3 and
  plxts reject such a literal at `CREATE FUNCTION` and suggest `0o755`.
- **`^` is bitwise XOR, and `**` binds as in the source language** (plxruby,
  plxphp, plxjs, plxts, plxpython3). `^` now becomes SQL `#` instead of SQL
  exponent, so `6 ^ 3` is 5, not 216. `**` is now right-associative and binds
  tighter than unary minus, so `2 ** 3 ** 2` is 512 (was 64) and `-2 ** 2` is
  -4 (was 4). A mix of bitwise operators that SQL would group differently from
  the dialect, such as `a ^ b & c` or `a | b & c`, is now rejected at
  `CREATE FUNCTION` with a request for parentheses; it used to compute a
  different value. A mix that SQL groups the same way, such as `a & b | c`,
  still works.
- **A chained assignment is rejected** (plxruby, plxphp, plxjs, plxts,
  plxpython3). `a = b = 0` used to compile to `a := b = 0`, leaving `b`
  unchanged and giving `a` a boolean. It is now rejected at `CREATE FUNCTION`,
  and so is a chain with a compound operator such as `a = b <<= 2`,
  `a = b &= 3` or `a = b ??= 4`, which used to fail only when the function ran.
- **String escapes keep their meaning** (plxruby, plxphp, plxjs, plxts,
  plxpython3). `"\x41"` is `A` and `"\v"` is a vertical tab; they used to lose
  the backslash and become literal letters. A NUL character (`\0`) is now
  rejected at `CREATE FUNCTION`, because text cannot hold it. In plxphp and
  plxpython3 an unknown escape such as `\q` keeps its backslash, as in those
  languages.
- **Adjacent string literals are concatenated** (plxruby, plxpython3).
  `"a" "b"` is `ab`. They used to merge into one SQL literal with a quote
  (`a'b`), and an escape string next to one ending in a backslash could
  swallow the code after it. A plxpython3 triple-quoted string is now one
  literal, so `"""hello"""` is `hello`, not `'hello'`, and quotes inside it
  are text.
- **A comment inside an expression is left out of the SQL** (plxphp, plxjs,
  plxts, plxpython3, plxruby). A comment inside a multi-line expression used to
  be copied into the generated SQL. PHP `$a # note` became SQL XOR and
  returned a wrong value, a JS `//` comment containing `--` hid the rest of the
  line, and an apostrophe in a comment opened a SQL string. A re-created PHP
  function with a `#` comment inside an expression now computes the PHP
  result.
- **`int(x)` and `x.to_i` truncate toward zero** (plxpython3, plxruby), as
  Python and Ruby do: `int(2.7)` is 2 and `int(-2.5)` is -2. Before, they
  rounded, which is what a SQL cast to integer does. Ruby `"4.7".to_i` now
  gives 4 instead of an error. In plxpython3, `int()` of a boolean that plx
  cannot see is boolean, such as a function returning boolean, now fails with
  `cannot cast type boolean to numeric`; write `int(f(x) == True)` instead. In
  plxruby, `.to_i`, `.to_f` and `.to_s` on an interpolated string, such as
  `"#{a}5".to_i`, now convert the whole string. Before, they converted only
  its last literal piece, so `"#{a}5".to_i * 2` gave 110 for `a = 1` instead
  of 30.
- **A range loop variable keeps its last value** (plxpython3, plxruby).
  plxpython3 `for i in range(...)` and plxruby `for i in lo..hi` now leave the
  loop variable at its last value after the loop or after a `break`, as Python
  and Ruby do. Before, the variable kept the value it had before the loop.
- **A `catch` class catches only its own condition** (plxphp, plxjs, plxts).
  Before, any class plx did not know (for example `PG::SerializationFailure`,
  `DivisionByZeroError`, a typo, or `A | B`) caught every error, and in plxphp
  so did a catch with no variable, such as `catch (PG::SerializationFailure)`.
  Now `PG::CamelCase` maps to the plpgsql condition `camel_case`, `A | B`
  catches both, and `Exception`, `Throwable` and `Error` still catch
  everything. Any other class, with or without a variable, is rejected at
  `CREATE FUNCTION`.
- **`break` inside an `if` or `try` within a `switch` arm leaves the switch**
  (plxjs, plxts, plxphp), as in JavaScript and PHP. Before, inside a loop it
  left the loop (a wrong result with no error), and outside a loop it was
  rejected with `break outside a loop`.
- **A C-style `for` that is not a plain counting loop is rejected** (plxjs,
  plxts, plxphp), with a hint to use `while`. Before, it was lowered to an
  integer `FOR` that ignored a condition on another variable, ignored changes
  the body made to the bound or loop variable, and lost the loop variable's
  final value. Also rejected now: a counting `for` whose variable an enclosing
  loop reads again before the `for` (in its condition, or earlier in its body),
  and in plxjs and plxts a `for (var i ...)` whose variable is read after the
  loop. A read inside a string interpolation counts, as in `${i}` or
  `"stopped at $i"`, including the SQL text of `query` and `fetch_one`. An
  escaped interpolation such as `\$i` or `\${i}` is text, not a read, and
  neither is a field of the same name, such as `${r.i}`. Plain counting loops
  compile as before.
- **`throw` of an expression that is not a constructor call is rejected**
  (plxjs, plxts, plxphp), for example `throw a(x) + b(y)`. Before, it silently
  raised only the first call's first argument.

#### plxphp

- **Properties and elements interpolate.** `"$row->name"`, `"$a[1]"`,
  `"$a[$i]"` and `"$row[name]"` inside a double-quoted string interpolate the
  property or element, as in PHP. Before, plx spliced the whole variable and
  left `->name` or `[1]` as literal text.
- **`break N` and `continue N` reach the Nth enclosing loop or `switch`**, as
  in PHP. Before, the level was ignored and only the innermost loop was left.
  A `break N` that leaves a `switch` or a labelled loop inside two or more
  `try`/`finally` blocks runs every `finally` it leaves. A level of 0, a level
  above the nesting depth, and a `continue N` that lands on a switch are
  rejected. `continue` or `continue 1` inside a switch arm is rejected too:
  PHP runs it as `break`, so write `break`, or `continue 2` to continue the
  loop. plxjs and plxts reject a number after `break` or `continue`.

#### plxruby

- **`each_with_index do |v, i|` over an array or a range sets the index**,
  counting from 0. Before, the index was never set, so the block read the
  variable's old value or failed when called. The index exists only inside
  its block, as in Ruby: a parameter or variable with the same name is no
  longer hidden for the whole function or overwritten by the loop, and a read
  of the index after the loop gets the outer variable, or fails if there is
  none. Using the same name for the value and the index (`|v, v|`) is an
  error, and so is `each do |v, i|` with two block variables.

#### plxpython3

- **`except` with an unknown class is rejected.** A class plx does not know
  (such as `KeyError`, or a misspelt `PG::` name) now fails at
  `CREATE FUNCTION` instead of silently catching every error. A tuple such as
  `except (PG::UniqueViolation, PG::CheckViolation)` now catches only those
  conditions; before, it caught every error.
- **String prefixes other than `f` are rejected.** `r"..."`, `b"..."`,
  `u"..."` and `rf"..."` now fail at `CREATE FUNCTION`. Before, `r"..."`
  failed only when called.

#### plxgo

- **`var x T` starts at Go's zero value** (0, `""`, `false`) instead of NULL,
  and a `var` inside a loop body is reset on every iteration. `var e [N]T`
  starts as N zero elements (`var e [3]int` is `{0,0,0}`), not NULL.
- **A three-clause `for` loop keeps Go's meaning.** Such loops now lower to
  `WHILE` instead of an integer `FOR`. The condition is re-tested on every
  pass, the body may change the loop variable, `for i = ...` updates an
  existing variable, a float loop variable stays a float, and `continue` runs
  the post statement in every three-clause loop (before, it could loop
  forever). `for i := i; ...` starts at the outer `i`. A loop whose start value
  is a function call (`for i := f(); ...`) or a row field is now rejected;
  declare the variable first. A start value from an element of a slice
  declared in the body (`for i := s[0]; ...`) is accepted.
- **A `switch` runs `default` only when no case matches**, wherever `default`
  stands. A leading `default` used to win always, and a middle `default`
  failed with a syntax error. `break` inside a switch leaves the switch
  instead of the enclosing loop.
- **A tagged `switch` evaluates its tag once** and compares each case value as
  a whole, so `switch nextval(...)` advances the sequence once and
  `case a || b:` means tag == (a || b).
- **Bitwise operators keep Go's meaning and precedence.** `^` is XOR (it was
  SQL power, so `6 ^ 3` gave 216), unary `^x` and `&^` work, `1 << 2 + 1` is
  5, and `<<=`, `>>=`, `&=`, `|=`, `^=` and `&^=` are accepted. Unary `^x`
  binds tighter than `+` and `*`, so `^y + 1` gives `-y`.
- **An integer constant shifted by `<<` or `>>` is a 64-bit value.** SQL typed
  the literal as a 32-bit integer and took the shift count mod 32, so
  `var m int64 = 1 << n` gave -2147483648 for n = 31 and 1 for n = 32, and
  `1 << 40` gave 256. plx now shifts the constant as `bigint`, which gives
  Go's values. This includes a named constant, a constant expression or a
  rune literal, such as `mask << 28` after `const mask = 0xFF`, `(1) << 40`,
  `2 * 1 << 40` and `'a' << 40`. Stored in an `int` (a 32-bit
  `integer` in plx), a result that does not fit now fails with
  `integer out of range` instead of wrapping.
- **A shift count at least as large as the width of the value gives Go's
  result.** SQL took the count mod 64 for a `bigint` and mod 32 for an
  `integer`, so with `n` = 64, `1 << n` gave 1 and `x >> n` gave `x`. Go gives 0
  for `<<`, and 0 or -1 for `>>`, and so does plx now. A negative count fails,
  as it panics in Go. An expression with more than 64 shifts by a count
  that is not a literal, nested in one another, is rejected at
  `CREATE FUNCTION`.
- **An `int` shifted left out of the range of a 32-bit integer fails** with
  `integer out of range`, whatever the count. plx maps `int` to `integer`,
  and SQL dropped the high bits: `x << 30` with `x` 3 gave -1073741824 (Go's
  64-bit `int` gives 3221225472). Declare the variable `int64` to get Go's
  value.
- **Integer conversions truncate.** `int(x)`, `int64(x)` and the other integer
  conversions truncate a float toward zero, as Go does, instead of rounding
  (`int(3.7)` is now 3). `int(x)` and `int64(x)` of a row field or an
  expression stay exact for integers beyond 2^53, and `int(r.n)` of a text
  field works.
- **A `*` width or precision in `fmt.Sprintf` takes the next argument**, as in
  Go. plx printed `%*d` as text and gave every later verb the wrong argument,
  so `fmt.Sprintf("[%*d|%s]", 5, 42, "x")` gave `[%*d|5]`; it now gives
  `[   42|x]`. A `*` precision takes its argument and is then dropped, like a
  written precision. Where the directives and arguments do not line up, a `*`
  is rejected at `CREATE FUNCTION`.
- **A `fmt.Sprintf` width pads but never truncates.** `%3d` of 12345 gave
  `"123"`; it now gives the full value. An explicit argument index such as
  `%[1]d` is rejected at `CREATE FUNCTION`, in `fmt.Sprintf` and
  `fmt.Printf`, instead of producing garbled text or printing the values in
  the wrong order.
- **Literals keep their value.** String and rune literals decode every Go
  escape (`\x`, octal, `\u`, `\U`, `\a` `\b` `\f` `\v`). A rune literal such
  as `'\xe9'` or `'\351'` is the code point 233 (see the next entry). An unknown
  escape (such as `"\d"`), a NUL, or bytes that are not valid text are
  rejected at `CREATE FUNCTION`. Hexadecimal, octal (`0o` and leading `0`),
  binary and `_`-separated integer literals keep their value on every
  supported PostgreSQL version (`017` was 17, `0o17` was 0, and on PostgreSQL
  13 to 15 `0x1F` was 0). A number with letters glued to it is rejected.
- **A rune literal is an integer, its code point**, as in Go. plx emitted it
  as a SQL string, so `var d rune = '7'` stored 7 instead of 55,
  `var r rune = 'a'` failed with `invalid input syntax for type integer`, and
  `'7' - '0'` failed with `operator is not unique`. `c := 'a'` now declares an
  `integer`. A rune literal must hold exactly one character. `string(x)` of an
  integer is now the character with that code point, as in Go, instead of the
  number's digits: `string(65)` is `"A"`, not `"65"`. This includes the
  variable of a `for i := 1; ...; i++` loop. Use `strconv.Itoa` for
  the digits. `%c` in `fmt.Sprintf` prints the character. `string(x)` of a
  float, `numeric` or bool value gave its text and is now rejected at
  `CREATE FUNCTION`, as Go rejects it. Format such a value with `fmt.Sprintf`.
- **`+` between a string and a number is rejected** at `CREATE FUNCTION`, as
  Go rejects it. Before, `n + "1"` with an integer `n` returned `n + 1`,
  because SQL read the literal as a number. Convert the number with
  `strconv.Itoa` or `fmt.Sprintf`.
- **`strconv.Itoa(x)` gives a string.** It was lowered to `(x)::integer`, so
  its result compared and sorted as a number:
  `strconv.Itoa(10) < strconv.Itoa(9)` gave false. It is now `(x)::text`, so
  that comparison gives true, as in Go. Assigning the result to a `string`
  variable gives the same text as before.

#### plxcobol

- **`PERFORM VARYING ... BY 1 UNTIL v > b` no longer hides its control
  variable.** After the loop, and after `EXIT PERFORM`, the variable holds the
  COBOL value (for example N+1, not its old value). A `MOVE` to the variable or
  to the bound inside the body now affects the loop, and a `PIC 9(18)` control
  variable no longer fails with `integer out of range`.
- **A character that COBOL does not use is rejected.** `!`, `$`, `@` or `|`
  outside a string literal or comment now fails at `CREATE FUNCTION`. Before,
  it was silently dropped, so `A != B` compiled to `a = b`, `$1` to the literal
  1, and `@ A` to `a`. Bodies that used `||` now fail to create; use
  `CONCAT(...)`.
- **Stacked `WHEN` phrases share their statements.** In `EVALUATE TRUE`, and
  for a `WHEN` stacked directly before `WHEN OTHER`, the stacked phrases now
  share the statements that follow, as in COBOL. Before, the earlier `WHEN` got
  an empty arm, so a matching value did nothing.
- **A store into a numeric PICTURE item truncates.** `MOVE`, `COMPUTE`, `ADD`,
  `SUBTRACT`, `MULTIPLY`, `DIVIDE`, the `PERFORM VARYING` step and a decimal
  `VALUE` clause now truncate the excess fractional digits, as COBOL does.
  `COMPUTE WS-I = 7.9` stores 7, not 8; 2.0 / 3 in `PIC 9(3)V99` stores 0.66,
  not 0.67; `PIC 9(4) VALUE 2.5` starts at 2, not 3. `DIVIDE` into a numeric
  PICTURE item keeps the quotient's fraction, so `DIVIDE 2 INTO 7 GIVING` a
  `PIC 9V99` item stores 3.50, not 3.00. The `/` operator in `COMPUTE` still
  follows SQL integer division. Write `ROUNDED` after the receiving field to
  round half away from zero (`DIVIDE 4 INTO 3 ROUNDED` stores 1); it was
  rejected before. `ROUNDED MODE` is not supported.
- **`DISPLAY` shows a table element and rejects other expressions.**
  `DISPLAY WS-ARR(2)` now shows element 2, not the whole array. A `DISPLAY`
  operand that is not a data name, table element or literal (such as a
  function call) is now rejected at `CREATE FUNCTION` instead of being
  silently dropped.
- **`DIVIDE a BY b` without `GIVING` is rejected** at `CREATE FUNCTION`, as
  COBOL requires. Before, it overwrote the divisor `b` with `a / b`.

#### plxtsql

- **`SELECT @v = expr FROM ...` assigns once per row**, as in SQL Server. The
  variable ends with the last row's value and keeps its old value when no row
  matches. Before, it took the first row and became NULL when no row matched.
  An assignment list that reads a variable it assigns (the accumulate idiom
  `SELECT @t = @t + v FROM t`) is now rejected at `CREATE FUNCTION`, because
  plx cannot compute it the way SQL Server does; use `SUM()` or
  `string_agg()`. The error names the entry that reads the variable. A list
  of more than 64 assignments is no longer split, and an entry with no `=` is
  now an error instead of emitting invalid code.
- **`[bracketed]` and `"quoted"` identifiers keep their quotes.** They become
  PostgreSQL quoted identifiers, so `[user]` reads the column `user` instead of
  returning `current_user`. A `[bracketed]` identifier is folded to lower case
  and then quoted, so `[Orders]` still matches a table created as `orders`,
  and `[Order Details]` works. A `"double-quoted"` identifier keeps its case,
  so `"Name"` reads the column `"Name"` instead of `name`. An unterminated `[`
  or `"` is now an error at `CREATE FUNCTION`.
- **`^` is bitwise exclusive OR and `LEN()` ignores trailing spaces**, as in
  SQL Server. `a ^ b` now becomes `a # b`. Before, it became PostgreSQL
  exponentiation, so `5 ^ 3` gave 125 instead of 6. `LEN(x)` now becomes
  `length(rtrim(x))`, so `LEN('abc   ')` gives 3 instead of 6. T-SQL `^`, `&`
  and `|` bind as tightly as `+` and `-`, but PostgreSQL binds `#`, `&` and `|`
  more loosely, so `1 ^ 2 + 3` gives 4 in plx, not 6. Write `(1 ^ 2) + 3`.
- **Unary `~` applies to its operand only**, as in SQL Server, where it binds
  more tightly than any other operator. plx now writes `~a` as `(~a)`. Before,
  it passed `~` through, and PostgreSQL binds it more loosely than `+` and
  `*`, so `~1 + 2` computed `~(1 + 2)` and gave -4. It now gives 0.
- **`RAISERROR` with severity 10 or lower prints a notice and continues.** It
  used to raise an error, so the progress-message idiom
  `RAISERROR('step', 0, 1) WITH NOWAIT` stopped the function on its first run.
  As in SQL Server, a severity of 10 or lower now gives `RAISE NOTICE`, and a
  `CATCH` block does not catch it. A severity of 11 or higher, -1 or
  `ERROR_SEVERITY()` still raises an error. A variable severity is checked at
  run time, and a NULL severity raises an error. A trailing `WITH NOWAIT`,
  `WITH SETERROR` or `WITH LOG` used to be emitted as a separate statement,
  which failed at run time with `syntax error at end of input` whenever the
  error did not fire. These options are now accepted, and `WITH LOG` also
  writes the message to the server log. An unknown option is rejected at
  `CREATE FUNCTION`. A function that used a severity of 10 or lower to stop on
  purpose now continues; to keep the old behaviour, use severity 16 or
  `THROW`.
- **`CAST` to `BIT` gives a `boolean` and `CAST` to `MONEY` gives
  `numeric(19,4)`**, as `DECLARE` and `CONVERT` already did. Before, `CAST`
  passed the name to PostgreSQL, whose `bit` keeps only the lowest bit, so
  `CAST(2 AS bit)` gave 0 instead of 1. PostgreSQL's `money` turned into text
  such as `$2.50`. A `boolean` has its own text forms: a cast to text gives
  `true` or `false`, and `CONCAT` gives `t` or `f`. So
  `CONCAT('a', CAST(1 AS bit))` now gives `at`; before, and in SQL Server, it
  gave `a1`. `CAST` to `MONEY` now rejects a string with a currency sign:
  `CAST('$5' AS money)` fails with `invalid input syntax for type numeric`.
  Before, it gave `$5.00`, and SQL Server gives 5.00. Remove the sign before
  the cast.
- **A conversion to an integer type truncates, and `DECIMAL` with no
  precision is `decimal(18,0)`**, as in SQL Server. `CAST` and `CONVERT` to
  `INT`, `INTEGER`, `BIGINT` or `SMALLINT` (and `CONVERT` to `TINYINT`), and an
  assignment to a variable of one of these types, now truncate a fraction
  toward zero. Before, they rounded: `CAST(1.5 AS int)` gave 2 instead of 1,
  `CONVERT(int, -2.5)` gave -3 instead of -2, and `DECLARE @i int = 2.7` gave 3
  instead of 2. A bare `DECIMAL`, `NUMERIC` or `DEC` in `DECLARE`, `CAST` or
  `CONVERT` now becomes `numeric(18,0)`. Before, it was PostgreSQL `numeric`,
  which keeps every digit: `DECLARE @d DECIMAL = 2.7` gave 2.7 instead of 3.
  `RETURN` and `INSERT` into an integer column still round; see
  doc/LIMITATIONS.md. For a value whose type plx cannot see, such as a column
  or a function call, the generated code calls the new functions
  `plx_tsql_int2`, `plx_tsql_int4` and `plx_tsql_int8`, which
  `ALTER EXTENSION plx UPDATE TO '3.0.0'` installs. A value of a domain over
  `numeric`, `real` or `double precision`, such as a T-SQL alias type,
  truncates too.

#### plxplsql

- **`SELECT ... INTO` and `EXECUTE IMMEDIATE ... INTO` raise `NO_DATA_FOUND`
  and `TOO_MANY_ROWS`**, as in Oracle. plx now emits `INTO STRICT`. Before, a
  query with no row set the target to NULL, and a query with more than one row
  kept the first row, so the Oracle exception handlers never ran. A function
  that relied on the old NULL or first-row result now raises an error. plx
  does not add `STRICT` to `WITH ... INSERT INTO`, to
  `WITH ... UPDATE/DELETE ... RETURNING ... INTO`, or to an `INTO` that already
  has `STRICT`.

### Upgrading

These steps are for a database administrator who upgrades plx from 2.0.0 to
3.0.0. You need superuser access to the server and to each database that has
the plx extension.

1. Install the new build on the server.
2. Restart the server, or start new sessions, so that they load the new build.
   The transpiler fixes need only this step.

   **Caution:** Run step 3 in each database right after this step, before
   anyone creates or replaces a plx function there. Until step 3 runs, a
   function that uses a string append (Ruby `<<`, PHP `.=`, or `+=` on a
   string in plxjs, plxts or plxpython3) is created without an error, but
   every call to it fails. So does a plxtsql function or `DO` block that
   converts a value whose type plx cannot see, such as a column or a
   function call, to an integer type (`CAST`, `CONVERT`, or an assignment to
   an `INT`, `BIGINT`, `SMALLINT` or `TINYINT` variable):

   ```
   ERROR:  function public.plx_sb_append_strict(plx_strbuild, text) does not exist
   ```

   or, for plxtsql,

   ```
   ERROR:  function public.plx_tsql_int4(record) does not exist
   ```

   The schema in the message is the one plx is installed in. The new build
   generates calls to these functions, and only step 3 creates them. After step
   3, the function works without being created again. Until step 3,
   `ALTER EXTENSION plx SET SCHEMA` is also still allowed, and it would break
   the functions the new build creates.
3. Right after step 2, in each database that has plx, run:

   ```sql
   ALTER EXTENSION plx UPDATE;
   ```

   The update records the extension as non-relocatable. It adds binary send
   and receive functions to `plx_strbuild` and makes it collatable. It gives
   the collation to every type built on `plx_strbuild` (its array type,
   domains, domains over those domains, and their array types) and to
   existing columns, range types, index keys and partition keys of all of
   these, as a fresh install does. It also adds the functions that string
   appends and plxtsql integer conversions call in 3.0.0. An expression
   stored before the update keeps no collation until you re-create the
   object that holds it: a view's query, a generated column, a column
   `DEFAULT`, a `CHECK` constraint, a row security policy, and the body of a
   SQL-standard function (`BEGIN ATOMIC`). Until then, an operation that
   sorts `plx_strbuild` values from it can fail with
   `could not determine which collation to use for string comparison`:
   `ORDER BY`, `DISTINCT ON`, `PARTITION BY`, `GREATEST` and `LEAST`, and
   `GROUP BY` or `DISTINCT` when the planner sorts. Comparison operators such
   as `<` and `=`, `min()` and `max()` still work.

   To check the result, run
   `SELECT extversion, extrelocatable FROM pg_extension WHERE extname = 'plx';`.
   It returns `3.0.0` and `f`.
4. List the plx functions in the database:

   ```sql
   SELECT p.oid::regprocedure AS function, l.lanname AS language
     FROM pg_proc p
     JOIN pg_language l ON l.oid = p.prolang
    WHERE l.lanname LIKE 'plx%'
    ORDER BY 1;
   ```

   Each of these keeps the plpgsql it was created with, so it behaves as it did
   under 2.0.0 until you run its `CREATE FUNCTION` again.
5. Find the functions that a breaking change affects. plx has no query that
   does this exactly. Read each function's source with the `plx_source()`
   helper in doc/DEBUGGING.md, and compare it with Breaking changes. To narrow
   the list, search the source for the construct that a change concerns, for
   example `^`:

   ```sql
   SELECT p.oid::regprocedure AS function
     FROM pg_proc p
     JOIN pg_language l ON l.oid = p.prolang
    WHERE l.lanname LIKE 'plx%'
      AND plx_source(p.oid) LIKE '%^%';
   ```

   The search matches text, so it also finds the construct inside strings and
   comments.
6. Re-create each function from its source with `CREATE OR REPLACE FUNCTION`,
   then test it. The function then uses the 3.0.0 code.

### Security

Every fix in this section concerns input to `CREATE FUNCTION` or `DO`, which
any role with USAGE on a plx language can send (the languages are trusted, so
PUBLIC has USAGE by default).

- **Backend crash on a type annotation at the start of an expression.** A
  `/*:: T */` or `#:: T` annotation as the first token of an `if`/`while`
  condition, a call argument, or a statement made the transpiler copy a span
  from a NULL pointer, killing the backend with SIGSEGV (and resetting every
  session on the server). Affected plxjs, plxts, plxphp, plxruby, plxpython3,
  including DO blocks. Annotations are now skipped at span edges.
- **Backend crash on a loop over `query()` with no arguments** (plxjs, plxts,
  plxphp, plxpython3). `CREATE FUNCTION` or `DO` now fails with
  `query requires a SQL argument`.
- **Stack-overflow crash on deeply nested expressions in plxtsql, plxgo and
  plxcobol.** Their expression emitters recursed once per nested call,
  subscript or literal without `check_stack_depth()`, so a body like
  `ISNULL(ISNULL(...))` or `int(int(...))` some 50,000 levels deep crashed the
  backend. They now raise `expression nested too deeply`, like the shared
  rewriter already did.
- **Exponential output from nested `finally`/`ensure`.** Each finally body was
  emitted once per exit path, so a finally nested inside a finally doubled the
  output per level: a 668-byte function stored a 94 MB body, and a slightly
  deeper one drove a backend to gigabytes of memory. The finally body is now
  emitted once (see Breaking changes), so output grows linearly with nesting.
- **String literal escaping its quotes.** A dialect string that ended in a lone
  backslash, inside an interpolation (plxjs/plxts `${'...\'}`, plxpython3
  f-strings), was emitted with that backslash bare inside an `E'...'` literal,
  where it escaped the closing quote. The next string literal in the body then
  ran as code, so a function whose source showed only inert string assignments
  could execute arbitrary SQL.
- **String literals that depend on `standard_conforming_strings`.** plpgsql
  reads a function body with the caller's setting, which any role may change.
  Literals that plxgo, plxtsql, plxplsql and plxcobol emit, and Ruby and PHP
  single-quoted strings, were written as plain `'...'`, so with the setting off
  a literal ending in a backslash swallowed its closing quote and the following
  text ran as code; the caller could change what a `SECURITY DEFINER` function
  did. The finished body is now rewritten so that every ordinary literal
  containing a backslash is an `E'...'` literal with the backslash doubled,
  which reads the same under either setting and, with it on, has the value the
  original had. This covers every dialect, and SQL passed through verbatim.
  A typed literal (`text'C:\'`) becomes `text E'C:\\'`, and an `N'...'`
  literal becomes `NCHAR E'...'`, the form PostgreSQL reads `N'...'` as. A
  `--` comment ends at a carriage return as well as at a newline, as in
  PostgreSQL, so a literal after one is rewritten too.
- **Quadratic, uninterruptible transpile time.** Local-variable lookup was a
  linear scan, so a body with 40,000 distinct variables took 47 seconds; the
  plxpython3 lexer rescanned every run of blank lines from each newline, so
  400,000 blank lines took 43 seconds. Neither honoured `statement_timeout`.
  Locals are now hashed (keyed as PostgreSQL reads an identifier, down-cased
  and truncated to 63 bytes, so no name defeats the index), blank and
  annotation lines are scanned once, and the lexer, rewriter and local table
  check for interrupts. (The same fix stops plxpython3 from
  counting blank lines twice in error line numbers.)
- **Quadratic, uninterruptible transpile time in plxtsql and plxplsql.** A
  plxtsql body with many unbalanced call parentheses (`f(f(f(...`), or a
  plxplsql body with many unbalanced `DBMS_OUTPUT.PUT_LINE(` or
  `RAISE_APPLICATION_ERROR(` calls, took quadratic time to transpile and could
  not be interrupted, so any role could tie up a backend. So did deeply nested
  `CONVERT`, `IIF`, `CHARINDEX` or `RAISE_APPLICATION_ERROR` calls with no
  comma, and deeply nested `CAST(... AS type)`. These now transpile in linear
  time, and `statement_timeout` can cancel the transpile.
- **Quadratic, uninterruptible transpile time for nested brackets.** In
  plxjs, plxts, plxphp, plxpython3 and plxruby, the rewriter for `**` scanned
  each bracketed group to its end at every `(` or `[`, so deeply nested
  brackets took quadratic time: 60,000 levels of `("x" + ...)` took 44
  seconds, and `statement_timeout` could not cancel it. The rewriter now finds
  every group in one pass, which takes 33 milliseconds for the same body, and
  checks for interrupts as it goes.
- **Quadratic, uninterruptible transpile time and a crash in plxgo string
  `+`.** To decide whether a `+` joins strings, plxgo typed each bracketed
  operand again at every level of nesting. A body of nested `("x" + (...))`
  therefore took quadratic time and memory: 6,400 levels took about 40
  seconds, and 40,000 levels used 12 GB and ignored both `pg_cancel_backend`
  and `pg_terminate_backend`. A string `+` inside the arguments of nested
  calls, such as `strings.ReplaceAll(x, "a", "a" + strings.ReplaceAll(...))`,
  was emitted twice per level, so the time doubled with each level. A chain of
  200,000 unary signs (`- - - 1`) overflowed the stack and crashed the
  backend. plxgo now types each bracketed expression once, stops at the
  nesting limit before it does the deeper work, types unary signs without
  recursion, and checks for interrupts while it lexes, types and emits, so
  `statement_timeout` can cancel the transpile.
- **Exponential memory when plxgo subqueries are nested.** plxgo evaluates
  some values once in a subquery: the operand of `int64(x)` or `int(x)` when
  plx cannot see its type, the value of a `fmt.Sprintf` width when it is a
  call, and the value and width of a `*` width. The planner pulled each
  subquery up and copied the value into every place it is used, so the plan
  grew exponentially with nesting: 8 nested `int64()` conversions ran out of
  memory when the function ran. The subqueries now have `OFFSET 0`, which
  keeps the planner from pulling them up.
- **Memory amplification in chained ternaries.** Each `?:` level kept its own
  copy of the rest of the chain, so peak memory was depth times source length
  (568 MB for a 1 MB body). Intermediate results are now freed.
- **plxgo `fmt.Sprintf` directive without a letter verb.** A `%` followed by a
  quote consumed half of the escaped `''` pair, so text inside a Go string
  literal could end up as SQL in the generated body. Such directives now fall
  back to the `format()` lowering, which keeps the text literal.
- **Revoking plpgsql now revokes plx.** A plx function is stored and run as
  plpgsql, but a role that kept USAGE on a plx language could still create
  plpgsql functions and run DO blocks after
  `REVOKE USAGE ON LANGUAGE plpgsql FROM PUBLIC` (for example with
  `DO LANGUAGE plxplsql`, or a body that already carried the transpiled
  header). `CREATE FUNCTION` and `DO` in a plx language now also require USAGE
  on plpgsql, failing with `permission denied for language plpgsql`. As with
  plpgsql, functions that already exist keep running, and superusers are
  unaffected.
- **Generated code names plx's own objects with plx's schema.** A string
  append (`s << x`, `$s .= x`, `s += x`, `STRING-APPEND`) declared the
  variable as `plx_strbuild` and called the append function unqualified, so
  they were resolved through `search_path` every time the function ran. That
  broke the standard hardening for a `SECURITY DEFINER` function
  (`SET search_path = pg_catalog`: `type "plx_strbuild" does not exist`), broke
  functions when plx was installed in a schema not on the path, and let a
  same-named function planted earlier on the path run in their place, with the
  definer's privileges for a `SECURITY DEFINER` function. Generated code now
  writes `<plx schema>.plx_strbuild` and qualifies the append function with
  the same schema.
- **A type annotation could run code on entry.** A local variable's type is
  spliced into the plpgsql `DECLARE` section, and plpgsql reads a declaration up
  to its `;`. A type annotation could therefore carry an initializer or more
  statements. For example, `x #:: int := f()` or `let x /*:: int DEFAULT f()
  */;` ran `f()` on entry, while the source showed only a type, and in plxjs
  only a comment. Every type that reaches `DECLARE` (annotations, plxts types,
  plxgo and plxtsql declarations, the plxcobol `TYPE` clause) must now be a type
  name accepted by the SQL type-name grammar, or `name%TYPE` / `name%ROWTYPE`.
  Anything else is rejected at `CREATE FUNCTION`.

### Fixed

Fixes that change a result or reject code are listed under Breaking changes,
not here.

#### All dialects

- **`plx_strbuild` works in sorting, grouping and binary transfer.** The type
  now has binary send and receive functions and is collatable like text.
  Binary `COPY`, binary protocol results, `ORDER BY`, `DISTINCT`, `GROUP BY`
  and `COLLATE` now work on it. These used to fail, for example when a string
  built with an append operator was used in a query that sorts or removes
  duplicates. `ALTER EXTENSION plx UPDATE` sets the collation on the same
  objects a fresh install does: `plx_strbuild`, every domain and array type
  built on it at any depth (such as a domain over a domain, or an array of a
  domain), and existing columns, composite-type fields, range types, index
  keys and partition keys of these types. So sorting, the range constructors
  and operators, hash indexes, and hash, list and range partitions work on
  them after the update.

#### Several dialects

- plxjs, plxts, plxpython3, plxruby: `||` on a string built with `+=` or
  `<<`, and an interpolation of such a string into SQL, failed at run time
  with `operator is not unique: plx_strbuild || unknown`, because the append
  makes the variable a `plx_strbuild`. plx now reads such a variable as
  `text` in expressions, as it does for plxphp `.=` (see plxphp below).
- plxjs, plxts, plxphp, plxpython3, plxruby: a loop bound that used `<<`,
  `>>`, `&`, `|` or `#`, such as `i < n >> 1` or `range(n >> 1)`, gave the
  wrong number of iterations with no error. plx turns an exclusive bound HI
  into `HI - 1` (or `HI + 1` for a `range()` with a negative step), and SQL
  binds `-` and `+` tighter than these operators, so `n >> 1 - 1` was
  `n >> 0`. plx now writes `(HI) - 1`. In plxjs, plxts and plxphp, `<` binds
  tighter than `&`, `^` and `|`, so `i < n & 6` means `(i < n) & 6`, not a
  bound: such a loop is now rejected with `for-loop bound must be a single
  value`. Write the bound in brackets, `i < (n & 6)`, or use a `while` loop.
- plxjs, plxts, plxpython3, plxruby: when plx's schema name was 50 bytes or
  longer (fewer if it needs quotes), the second string append to a local
  compiled to numeric addition and failed at run time with
  `operator does not exist: ... plx_strbuild + unknown`. plxruby rejected the
  body at `CREATE FUNCTION`. Appends now work with any schema name. Version
  2.0.1 was not affected, because it did not qualify the type with the schema.
- plxjs, plxts, plxphp, plxruby, plxpython3: a `finally`/`ensure` body that
  raised an error on the normal path ran a second time from the error path.
  It now runs once.
- plxruby, plxphp, plxjs, plxts, plxpython3: `.to_s`, `.to_i` and `.to_f`
  were rewritten as casts wherever the text appeared, so a field whose name
  starts with one of them broke: `r.to_status` became `r::texttatus`. They
  are now rewritten only as whole words, and `x.to_s()` with an empty argument
  list works too.
- plxruby, plxphp, plxjs, plxts, plxpython3: `ARRAY['x']` and a subscript on a
  `jsonb`, `json` or `hstore` value (`j['a']`) are passed to SQL unchanged
  instead of being rewritten to record field access (`ARRAY.x`, `j.a`), which
  failed at run time. A record key that
  is not a plain identifier, such as `row['a b']`, is now quoted as a column
  name. In plxphp and plxruby, a record variable named `array` is no longer
  read as the SQL `ARRAY` constructor, which silently returned a one-element
  array. Such a variable still fails, because `array` is an SQL reserved word.
- plxjs, plxts: a `?` and `:` inside a template literal are now text. They used
  to be read as a ternary, which turned the literal into broken SQL and made
  the function fail when it ran.
- plxjs, plxts, plxphp: a `switch` arm may now end with `continue` or with
  `break label`. Before, valid code was rejected with
  `switch fall-through is not supported` or
  `switch body must consist of case/default labels`.
- plxjs, plxphp, plxruby, plxpython3, plxts: an expression nested too deeply,
  and an integer literal such as `0xFFFFFFFFFFFFFFFFFF` that overflows 64 bits,
  now report the source line of their statement instead of line 0. plxpython3:
  a `class` or `lambda` statement is now reported as `class is not supported`
  or `lambda is not supported` instead of `def is not supported`.

#### plxphp

- The `.` operator now works on a string built with `.=`, as `doc/plxphp.md`
  shows. `"names: " . $s` failed at run time with
  `operator is not unique: unknown || plx_strbuild`, because `.=` makes `$s` a
  `plx_strbuild`. plx now reads such a variable as `text` in expressions.

#### plxts

- `catch (e: unknown)` (a catch variable with a type annotation) now binds
  `e`, so `e.message` and `e.sqlstate` work. Before, the function failed at
  run time with `missing FROM-clause entry for table e`.

#### plxpython3

- `range(a, b, step)` with a negative literal step, such as
  `range(10, 0, -3)`, now counts down as in Python. It failed at run time with
  `BY value of FOR loop must be greater than zero`. A negative step held in a
  variable still fails that way.
- `call(f"...")` emitted an unterminated quoted identifier. An f-string
  procedure name is now rejected.

#### plxruby

- A NULL in a `raise` option no longer replaces your error. Before, if a value
  in `detail:`, `hint:`, `column:` or `constraint:` was NULL, either
  interpolated (`detail: "looked up #{u}"`) or bare (`detail: u`), the `raise`
  failed with `RAISE statement option cannot be null` (SQLSTATE 22004). Your
  message, SQLSTATE, detail and hint were lost, and a handler for your
  SQLSTATE did not run. These options now get the same fallback as the
  message: each NULL value becomes an empty string. The generated SQL changes
  for every `raise` that has options. Behaviour changes only for calls that
  used to fail with 22004. A NULL `errcode:` still fails with 22004.
- `raise message: "...", ...` now raises that message. Before, it compiled to a
  syntax error. A `raise` with both a message and `message:` is rejected at
  `CREATE FUNCTION` instead of failing when it runs.

#### plxgo

- `+` between strings now joins them (#18, #23). plx writes it as `||` when
  every operand is a string literal, a `string` local, a `text` or `varchar`
  parameter, or a call that returns a string. If any operand has a type plx
  cannot see, such as a row field, an array, `jsonb` or a SQL function call,
  `+` stays SQL `+` and fails when the function runs. Before,
  `strconv.Itoa(n) + "x"` failed when the function ran, with
  `operator does not exist: text + unknown`. A NULL operand makes the result
  NULL. `msg := "a" + s` now declares `msg` as `text`.
- `s += x` on a string appends. It used to become `s := s + (x)`, which
  failed when it ran. plx now writes `s := s || (x)` when it can see that `x`
  is a string, by the same rules as `+`. A NULL `x` makes `s` NULL. `s += 1`
  on a string is rejected at `CREATE FUNCTION`, as in Go. When plx cannot see
  the type of `x`, such as a row field or a SQL function call, it still writes
  `s := s + (x)`, which fails when the function runs.
- `make(...)` is not supported, but the error named something else: a syntax
  error at `[`, a type that could not be inferred, or a case clash between
  the type name and a variable. plx now rejects `make` at `CREATE FUNCTION`
  with `make is not supported` and suggests a slice literal or `array_fill`.

#### plxcobol

- `EXIT PERFORM CYCLE` inside a `PERFORM VARYING` with a step other than BY 1,
  or with a compound `UNTIL`, now runs the step. Before, the loop never ended.
- An `OCCURS` item with a `VALUE` clause now starts with every element set to
  that value. Before, the function was created but failed at every call with
  `malformed array literal`.
- An `EVALUATE` whose only `WHEN` phrases are stacked directly before
  `WHEN OTHER`, or that has only `WHEN OTHER` or no `WHEN` at all, no longer
  fails at every call.
- `DISPLAY` accepts `NO ADVANCING` without `WITH`. Before, `NO` and
  `ADVANCING` were read as data names and the statement failed.

#### plxtsql

- The dialect chapter and doc/LIMITATIONS.md now say that a `CATCH` block
  undoes the database changes its `TRY` block made before the error. SQL
  Server keeps them. plx cannot, because `TRY` becomes a plpgsql exception
  block, which rolls back to a savepoint. The behaviour is the same as in
  2.0.1; only the documentation is new. It shows how to keep such a write.
- A `CASE` expression works in `SET`, `RETURN`, `DECLARE` initializers, `IF`
  and `WHILE` conditions, and `SELECT` assignment lists. Before, plx cut the
  expression at the CASE's `ELSE` or `END`, and the function was rejected with
  `unexpected END` or `unexpected keyword in statement position`. An `IF`
  condition that holds `IIF(...)` now compiles too.
- `CAST` maps T-SQL type names the way `CONVERT` and `DECLARE` do. Before,
  `CAST(x AS nvarchar(5))` failed when the function ran, with
  `type "nvarchar" does not exist`. `NVARCHAR`, `NTEXT`, `VARCHAR(MAX)`,
  `NVARCHAR(MAX)`, `DATETIME`, `DATETIME2`, `UNIQUEIDENTIFIER`, `IMAGE` and
  `VARBINARY(MAX)` now work in `CAST`. `CAST` to `NVARCHAR` without a length
  gives `varchar(30)`, as in SQL Server. `TINYINT`, `SMALLMONEY`,
  `SMALLDATETIME`, `SYSNAME`, `BINARY` and `VARBINARY(n)` still fail, because
  the PostgreSQL type would not keep SQL Server's value.
- `CONVERT(varchar(MAX), x)` and `CONVERT(nvarchar(MAX), x)` give `text`.
  Before, the function was rejected with `syntax error at or near "MAX"`.
  `(MAX)` on any other type, such as `CONVERT(numeric(MAX), x)` or
  `CONVERT(char(MAX), x)`, still fails, as it does in SQL Server.
- `+` joins strings, and `SET @s += e` appends to a string variable. Before,
  both failed when the function ran, for example `'1' + '2'` with
  `operator is not unique: unknown + unknown` and `SET @s += 'b'` with
  `operator does not exist: character varying + unknown`. plx writes `+` as
  `||` only when every operand is a known string, such as a `VARCHAR`
  variable, a string literal or `ISNULL(@s, '0')`, so `'1' + '2'` gives `12`
  and `ISNULL(@s, '0') + '5'` gives `05`. `SET @s += e` becomes `||` only when
  `@s` is a string variable and `e` is a known string. Any other `+` stays
  `+`: `'1' + 2` still adds and gives 3, and a string variable next to a
  number still fails when the function runs. A NULL operand makes the result
  NULL. See doc/plxtsql.md for the full list of known strings.
- `DECLARE`, `CAST` and `CONVERT` accept SQL Server's type names of several
  words, such as `CHARACTER VARYING(n)` and `NATIONAL CHARACTER VARYING(n)`.
  Before, `DECLARE` rejected them. A value of one of the variable-length
  string types among them is now a known string, so `+` joins it with `||`.
  `CHARACTER(n)` and `NATIONAL CHARACTER(n)` are fixed length, like `CHAR(n)`,
  so `+` stays `+` for them.

#### Test suite

- The differential test records the exact result each documented divergence
  documents, and only that result is exempt. Before, a dialect could return
  any wrong value for an exempted call and the check still passed. This
  affects the exemptions for plxruby, plxphp, plxjs, plxts, plxpython3 and
  plxcobol.
- The differential test records each result as JSON. Before, a dialect that
  returned the literal text `<NULL>`, `<EMPTY>` or `ERROR <code>` was counted
  as matching a reference that returned NULL, returned an empty string or
  raised. No current case was affected.
- The fuzzer reports a statement cancelled at its timeout as a hang and an
  out-of-memory error as a finding. Before, both counted as clean. It also
  stops with an error when the plx languages are missing from the target
  database; before, it silently tested nothing.
- `make metacheck` checks `META.json` the way PGXN does at upload time:
  license identifiers, unknown keys, empty text fields, the meta-spec version,
  the prereqs structure and version ranges, `no_index`, the bugtracker and
  repository value types, and full-string version patterns. Before, it passed
  files that PGXN rejects. It reports a value of the wrong type as an error
  instead of crashing.
- `test/run_corpus.py` compares result rows and notices in order, and compares
  psql output line by line. Before, it compared unordered token sets, so
  swapped values, mispaired rows and reversed results passed. A value of `-`
  or `+`, spaces inside a value or message, and the position of an empty row
  now count too. This affects the plxruby and plxcobol corpus cases.
- A `test/run_corpus.py` case that records an expected error now passes only
  when psql prints each recorded `ERROR:` line word for word. Before, it
  passed when about half the words of the expected error showed up anywhere
  in the output, and the `CONTEXT:` line alone was enough, so a different
  error raised at the same statement passed. The runner's self-test now
  covers this check. Two reject cases recorded messages plx never printed;
  they now record the real ones.
- CI now runs the Ruby corpus on every PostgreSQL version it tests, with the
  new `make corpuscheck`. Before, nothing ran the corpus automatically, so a
  change that broke a corpus case could merge unnoticed. `test/run_corpus.py`
  now stops with a clear message when it cannot find psql, instead of a
  Python traceback.

### Changed

- `make differentialcheck` also runs the self-tests of the fuzzer and of
  `test/run_corpus.py`. Neither self-test needs a server.
- `test/run_corpus.py` uses the `psql` on `PATH` (or `PLX_PSQL`) and the libpq
  environment variables instead of a fixed `/usr/local/pgsql` path. Its
  self-test runs before it touches the server.

### Added

- **`ident()`, `literal()` and `raw()`** may wrap an interpolation in a SQL
  argument (plxphp: `{ident($t)}`): `ident(e)` splices `quote_ident(e)`,
  `literal(e)` splices `quote_nullable(e)`, and `raw(e)` splices `e` unquoted,
  for a trusted SQL fragment. Each makes the call dynamic.
- **`ROUNDED` in plxcobol.** Write `ROUNDED` after a receiving field to round
  half away from zero instead of truncating. See the PICTURE entry under
  Breaking changes.
- **`WITH NOWAIT`, `WITH SETERROR` and `WITH LOG` on plxtsql `RAISERROR`.**
  `WITH LOG` also writes the message to the server log. See the `RAISERROR`
  entry under Breaking changes.

## [2.0.1] - 2026-08-25

Packaging only. **The extension is unchanged at 2.0.0**, so there is no
`ALTER EXTENSION plx UPDATE` for this release and nothing to install if you are
already on 2.0.0. Only the distribution metadata changed, which is why the
distribution version moved and the extension version did not.

### Fixed

- `META.json` listed the tag `pl/sql`, which PGXN rejects: a Tag may not contain
  a slash. It is now `plsql`. This was found only when PGXN refused the 2.0.0
  upload, since the check happens at upload time, after a release has been
  tagged and published.

### Added

- `make metacheck` (`test/check_meta.py`) validates `META.json` against the
  PGXN Meta Specification v1 before a release rather than at upload time, and
  runs in CI. It reproduces the rejection above.

  Worth recording, because the prose and the schema disagree and the prose is
  the trap: the spec text says a Tag may contain no "slash, backslash, control,
  or space" characters, but that sentence describes a *Term*. The Tag schema is
  `^[^/\\\p{Cntrl}]{2,}$`, which permits spaces. So `sql server` is a valid
  tag and an invalid term, and only the slash in `pl/sql` was ever a problem.

## [2.0.0] - 2026-08-24

Major release for one behaviour change: interpolating a NULL now propagates it
instead of rendering an empty string. Upgrade with
`ALTER EXTENSION plx UPDATE TO '2.0.0'` after installing the new module.

### Changed

- **Interpolating a NULL propagates it.** An interpolated value is concatenated
  as-is, so the whole string becomes NULL the way SQL `||` does. Previously
  every interpolated value was wrapped in `COALESCE((x)::text, '')`, so a
  missing value silently became a plausible-looking string:

  ```sql
  -- return "#{country}-#{region}-#{code}" with a NULL region
  --   before: 'US--123'   a string that inserts, indexes and joins like data
  --   now:    NULL
  ```

  This affects `plxruby`, `plxphp`, `plxjs`, `plxts`, `plxpython3` and
  `plxgo`. `plxplsql` and `plxtsql` already propagated and are unchanged.
  `plxcobol` builds strings through the `plx_strbuild` accumulator, whose
  append treats a NULL as nothing to append by design, and is unchanged.

  A message built for `RAISE` is the exception and keeps each interpolated
  value's empty-string fallback, so one NULL cannot swallow a diagnostic and
  the literal text of the message survives.

- `plxgo`: `fmt.Sprintf` is lowered to a SQL concatenation rather than
  `format()`, which is what allows the propagation, since `format()` renders a
  NULL operand as empty and cannot be made to do otherwise. Observable output
  is otherwise unchanged: verbs render their operand as text, a `-` flag and a
  width still pad (now through `rpad`/`lpad`), other flags and the precision
  field are still dropped, and `%%` is still a literal percent.

### Upgrading

`ALTER EXTENSION plx UPDATE TO '2.0.0'` does not change any function that
already exists. plx transpiles at `CREATE FUNCTION` time and stores the result
in `pg_proc.prosrc`, so a function in the catalog keeps the plpgsql it was
created with. The new behaviour arrives the next time that function's DDL is
run, which for most installations means the next deployment.

To find the functions a redeployment will change:

```sql
SELECT p.oid::regprocedure
  FROM pg_proc p JOIN pg_language l ON l.oid = p.prolang
 WHERE l.lanname LIKE 'plx%'
   AND EXISTS (SELECT 1
                 FROM regexp_split_to_table(p.prosrc, E'\n') AS ln
                WHERE ln LIKE '%COALESCE((%)::text, ''''%'
                  AND ln NOT LIKE '%RAISE %')
 ORDER BY 1;
```

Each result interpolates a value that will become NULL when the value is NULL.
Where the old rendering was wanted, make it explicit with `coalesce(x, '')` in
the body.

The same applies to a dump taken from an earlier release and restored into
2.0.0: the restored functions keep the plpgsql stored in the dump, including
the empty-string rendering, until their DDL is run again.

## [1.3.2] - 2026-08-24

Code-only patch release (no catalog changes) carrying a plxgo fix for
`fmt.Sprintf` and the differential check that found it. Upgrade with
`ALTER EXTENSION plx UPDATE TO '1.3.2'` after installing the new module.

### Fixed

- `plxgo`: `fmt.Sprintf` in expression position passed its Go format string
  straight into SQL `format()`, so any verb other than `%s` raised
  `unrecognized format() type specifier` when the function was called. The
  transpile succeeded and the failure only appeared at run time, which made
  `fmt.Sprintf("%d", n)`, the ordinary way to format an integer in Go, produce
  a function that could not run. Go's verbs are now rewritten to the `%s` that
  `format()` understands, keeping a `-` flag and a width and dropping the flags
  and precision `format()` has no equivalent for. A `%` that starts no
  directive is escaped rather than passed through, since a lone `%` is itself
  an error to `format()`. Found by the new differential check.

  Note that the verbs which change an operand's representation rather than its
  padding render what `%s` renders, so `fmt.Sprintf("%x", 255)` yields `255`
  and not `ff`. Convert explicitly where the representation matters. This is
  recorded in `doc/plxgo.md` and `doc/LIMITATIONS.md`.

### Added

- `make differentialcheck` (`test/differential.py`): each case is one program
  written as a plpgsql reference and once per dialect, called with the same
  arguments and required to agree with the reference on both values and error
  SQLSTATEs. Intended divergences are recorded per case with a reason and
  reported separately, and the check fails if a recorded divergence stops
  happening, so a documented limitation cannot outlive its documentation.
- `doc/MIGRATION.md`, a migration-led page comparing plx against rewriting by
  hand, an embedded PL, `ora2pg`, and leaving the logic in the application,
  including how to leave plx while keeping the generated plpgsql.

### Changed

- Internal refactor of the transpiler behind a `PlxSurface.parse_body` vtable
  (#2). The single ~11k-line `src/plx_transpile.c` is split into a
  dialect-neutral engine (declared in the new `src/plx_engine.h`) plus per-dialect
  front ends in `src/plx_dialect_*.c` and the shared `src/plx_parse_brace.c`;
  the hardcoded `block_style` dispatch is replaced by a per-dialect `parse_body`
  function pointer. No functional change; generated plpgsql is byte-identical
  and all regression tests pass. Adding a dialect no longer touches shared code.
- Follow-up to the above: relocated the dialect-neutral `plx_diag_prefix()`
  helper out of `src/plx_dialect_ruby.c` and back into the engine
  (`src/plx_transpile.c`), where its other callers (Python and the brace
  front end) already live. No functional change.
- Tightened linkage and removed dead code left by the refactor: the six
  single-dialect `plx_*_parse_body` front ends are now `static` (only the
  shared `plx_brace_parse_body` keeps a prototype in `plx_engine.h`); dropped
  the write-only `Ctx.nt` field; and corrected a stale COBOL comment about a
  keyword table the surface does not carry. No functional change.

## [1.3.1] - 2026-07-16

Code-only patch release (no catalog changes) carrying the memory-safety and
robustness fixes from the full-repo transpiler audit (#1). Upgrade with
`ALTER EXTENSION plx UPDATE TO '1.3.1'` after installing the new module.

### Fixed

- Missing capacity guard on the trailing `T_EOF` token write in `lex()`: a source
  that lexed to exactly `cap-1` tokens overflowed the token array by one
  (heap overflow / SIGSEGV). All token writes are now guarded.
- `plx_strbuild` `sb_ensure()` doubled its capacity in `int32`, which could wrap
  negative near ~1GB and spin forever while passing a bogus size to `repalloc`.
  Growth is now computed in 64-bit and clamped to `MaxAllocSize`, so an
  oversized request raises the standard allocation error cleanly.
- The recursion-guarded transpiler entry points now call `check_stack_depth()`,
  so deeply nested hostile input raises a clean error honoring `max_stack_depth`
  instead of overflowing the C stack and crashing the backend.
- The Python lexer raises a clean "indentation nested too deeply" error at the
  indent-stack limit instead of emitting an unbalanced `T_INDENT` that mis-nested
  every enclosing block.
- Raw single-quoted strings in Ruby and PHP keep backslashes literal (`'\n'`,
  `'C:\temp'`); only `\\` and `\'` are special. Double-quoted and Python/JS
  single-quoted escapes are unchanged.
- PHP `${name}` curly interpolation in double-quoted strings is now recognized
  (was emitted verbatim).
- Non-decimal integer literals (`0x`/`0o`/`0b`, with `_` group separators) are
  lexed as a single token and rewritten to decimal, keeping generated plpgsql
  portable to PG13-15 (which accept `0x/0o/0b` only on PG16+). A literal that
  overflows 64 bits raises a clean "integer literal out of range" error rather
  than emitting invalid SQL.

### Other

- Declare the built-in dialect descriptors in `plx.h`, clearing
  `-Wmissing-variable-declarations` across the dialect translation units.

## [1.3.0] - 2026-07-15

Trigger row mutation across dialects, plus the cookbook and limitations
documentation set. Upgrade with `ALTER EXTENSION plx UPDATE TO '1.3.0'` after
installing the new module (no catalog changes).

### Added

- plxtsql: a trigger can now assign to `NEW` fields with `SET NEW.col = e`, which
  lowers to `NEW.col := e`, so a Transact-SQL trigger can rewrite the row and not
  only validate it. A `SET` whose target is a qualified name with a top-level `=`
  is an assignment; `SET NOCOUNT ON` and other session options are still ignored.
  Qualified names also emit without a stray space around the dot. Covered by a new
  plxtsql trigger regression test.
- A verified cookbook for each of the nine dialects (`doc/cookbook/`) and a
  consolidated gaps-and-limitations page (`doc/LIMITATIONS.md`), both on the
  documentation site.

With the plxphp arrow assignment (1.2.2) and this plxtsql `SET NEW.col`, assigning
to a trigger's `NEW` fields is supported across the dialects in the dialect's own
idiom. See each dialect's cookbook trigger recipe.

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
