# plxtsql: the Transact-SQL (SQL Server) dialect

plxtsql lets you write PostgreSQL functions with Transact-SQL syntax, the
procedural language of Microsoft SQL Server and Sybase. At `CREATE FUNCTION`
time plx transpiles the body to plpgsql and stores the plpgsql in
`pg_proc.prosrc`. The function then runs on the standard plpgsql interpreter.

Unlike plxplsql, T-SQL is not Ada-descended, so plxtsql is a restructuring front
end rather than a pass-through rewriter. It has its own tokenizer and parser and
emits plpgsql directly. It handles the parts of T-SQL that differ structurally
from plpgsql: `@`-prefixed variables, `DECLARE` anywhere in the body, `IF`/`WHILE`
bodies written as `BEGIN ... END` instead of `THEN ... END IF`, and
`TRY`/`CATCH`. This makes plxtsql a practical path for moving SQL Server stored
logic into PostgreSQL.

## Setup

```sql
CREATE EXTENSION plx;
```

## Function basics

The body is a sequence of T-SQL statements. Local variables are declared with
`DECLARE @name type` and referenced with the `@` sigil; plx hoists every
declaration into a plpgsql `DECLARE` block and drops the sigil in the generated
code.

```sql
CREATE FUNCTION grade(score int) RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @g varchar(10);
  IF @score >= 90
    SET @g = 'A';
  ELSE IF @score >= 80
    SET @g = 'B';
  ELSE
    SET @g = 'F';
  RETURN @g;
$$;
```

An outer `BEGIN ... END` wrapping the whole body is optional; if present it is
unwrapped (the declarations inside still hoist).

### Function signatures use PostgreSQL types

The **body** is T-SQL, but the function **signature** (the parameter and return
types in `CREATE FUNCTION`) is parsed by PostgreSQL before plx sees the body, so
it must use PostgreSQL type names: write `RETURNS timestamp`, not
`RETURNS DATETIME`. Parameters are referenced in the body with the `@`
sigil (`@score` above binds to the parameter `score`). Inside the body, T-SQL
type names in `DECLARE` are translated (see below).

## Transact-SQL to PostgreSQL translations

### Identifiers

| T-SQL | PostgreSQL |
| --- | --- |
| `[Order Details]`, `[user]` | `"order details"`, `"user"` |
| `"Name"` | `"Name"` |
| `[a]]b]`, `"a""b"` | `"a]b"`, `"a""b"` |

A delimited identifier keeps its delimiters, so it can hold a space or a
reserved word.

- A `[bracketed]` name is folded to lower case, then quoted. So `[Orders]`
  matches a table created as `orders` or `Orders` without quotes, as in SQL
  Server under its default case-insensitive collation.
- A `"quoted"` name keeps its case, as in PostgreSQL. Use it to reach an object
  created with a mixed-case quoted name: `"Name"` matches a column created as
  `"Name"`, not one created as `name`.

### Variables and assignment

| T-SQL | plpgsql |
| --- | --- |
| `DECLARE @x int` | hoisted to `DECLARE x integer;` |
| `DECLARE @x int = 5` | `x integer;` plus `x := 5;` at that point |
| `DECLARE @a int, @b int` | both hoisted |
| `SET @x = e` | `x := e;` |
| `SET @x += e` | `x := x + (e);` (also `-=`, `*=`, `/=`, `%=`) |
| `SET @s += e`, `@s` a string variable and `e` a known string | `s := s \|\| (e);` |
| `SELECT @x = e` (no `FROM`) | `x := e;` |
| `SELECT @x = a, @y = b` | `x := a; y := b;` |
| `SELECT @x = col FROM t ...` | `FOR r IN SELECT col AS c1 FROM t ... LOOP x := r.c1; END LOOP;` |

`SELECT @x = col FROM t ...` assigns once per row, as in SQL Server. After the
statement, `@x` holds the value from the last row. When no row matches, `@x`
keeps the value it had before. An expression in the list that reads one of the
variables the same `SELECT` assigns, such as `SELECT @t = @t + v FROM t`, is
rejected when you create the function. Use an aggregate instead, for example
`SELECT @t = SUM(v) FROM t` or `SELECT @s = string_agg(name, ',') FROM t`.

### Control flow

| T-SQL | plpgsql |
| --- | --- |
| `IF cond stmt` | `IF cond THEN stmt END IF;` |
| `IF cond BEGIN ... END` | `IF cond THEN ... END IF;` |
| `IF cond ... ELSE ...` | `IF cond THEN ... ELSE ... END IF;` |
| `ELSE IF ...` | nested `ELSE IF ... END IF;` |
| `WHILE cond BEGIN ... END` | `WHILE cond LOOP ... END LOOP;` |
| `BREAK` | `EXIT;` |
| `CONTINUE` | `CONTINUE;` |
| `RETURN e` | `RETURN e;` |

The condition after `IF`/`WHILE` runs up to the start of the body (a statement
keyword or `BEGIN`); there is no `THEN` keyword in T-SQL.

### Messages and errors

| T-SQL | plpgsql |
| --- | --- |
| `PRINT e` | `RAISE NOTICE '%', e;` |
| `RAISERROR('msg', sev, state)` with `sev` 10 or lower | `RAISE NOTICE '%', 'msg';` |
| `RAISERROR('msg', sev, state)` with `sev` 11 or higher, `-1`, or `ERROR_SEVERITY()` | `RAISE EXCEPTION '%', 'msg';` |
| `RAISERROR('msg', @sev, state)` | `IF sev <= 10 AND sev <> -1 THEN RAISE NOTICE ... ELSE RAISE EXCEPTION ... END IF;` |
| `RAISERROR(...) WITH LOG` | adds `RAISE LOG '%', 'msg';` before the message |
| `THROW n, 'msg', s` | `RAISE EXCEPTION '%', 'msg';` |
| `THROW;` (bare, in CATCH) | `RAISE;` (re-raise) |
| `BEGIN TRY ... END TRY BEGIN CATCH ... END CATCH` | `BEGIN ... EXCEPTION WHEN OTHERS THEN ... END;` |
| `ERROR_MESSAGE()` | `SQLERRM` |

RAISERROR follows SQL Server's severity rule. A severity of 10 or lower is an
informational message: the function prints it as a notice and continues, and a
CATCH block does not catch it. A severity of 11 or higher raises an error. Two
values always raise an error: -1, which in SQL Server means the severity stored
for the message (plx cannot look that up), and ERROR_SEVERITY(), because only an
error reaches a CATCH block. Any other negative severity counts as 0. When the
severity is a variable, plx checks it at run time, and a NULL severity raises an
error. After the closing parenthesis you can write WITH followed by NOWAIT,
SETERROR or LOG, separated by commas. NOWAIT needs no translation because
PostgreSQL sends every notice to the client at once. SETERROR has no effect
because plxtsql does not support @@ERROR. LOG also writes the message to the
server log. Any other option is an error when you create the function.

`RAISERROR`'s format arguments (its `%d`/`%s` printf-style substitutions) are not
applied; the message argument is emitted as-is.

### Types (in `DECLARE`)

| T-SQL | PostgreSQL |
| --- | --- |
| `INT`, `INTEGER` | `integer` |
| `BIGINT`, `SMALLINT`, `TINYINT` | `bigint`, `smallint`, `smallint` |
| `BIT` | `boolean` |
| `DECIMAL(p,s)`, `NUMERIC`, `MONEY` | `numeric(p,s)`, `numeric`, `numeric(19,4)` |
| `FLOAT`, `REAL` | `double precision`, `real` |
| `VARCHAR(n)`, `NVARCHAR(n)` | `varchar(n)` |
| `VARCHAR(MAX)`, `NVARCHAR(MAX)` | `text` |
| `CHAR(n)`, `NCHAR(n)` | `char(n)` |
| `TEXT`, `NTEXT` | `text` |
| `DATE`, `TIME` | `date`, `time` |
| `DATETIME`, `DATETIME2`, `SMALLDATETIME` | `timestamp` |
| `DATETIMEOFFSET` | `timestamptz` |
| `UNIQUEIDENTIFIER` | `uuid` |
| `VARBINARY(n)`, `VARBINARY(MAX)` | `bytea` |

An unrecognized type name is passed through, so PostgreSQL type names also work.

### Functions

| T-SQL | PostgreSQL |
| --- | --- |
| `ISNULL(a, b)` | `coalesce(a, b)` |
| `IIF(c, a, b)` | `CASE WHEN c THEN a ELSE b END` |
| `CONVERT(type, e)` | `CAST(e AS type)` (the style argument is ignored) |
| `LEN(x)` | `length(rtrim(x))` (trailing spaces are not counted, as in SQL Server) |
| `DATALENGTH(x)` | `octet_length(x)` |
| `CHARINDEX(sub, str)` | `strpos(str, sub)` (arguments swapped) |
| `GETDATE()`, `SYSDATETIME()` | `now()` |
| `NEWID()` | `gen_random_uuid()` |
| `CEILING(x)` | `ceil(x)` |
| `@@IDENTITY` | `lastval()` |

`CAST(e AS type)` and most standard functions (`COALESCE`, `SUBSTRING`, `UPPER`,
`LOWER`, `ABS`, `ROUND`, ...) are already valid in PostgreSQL and pass through.

### Operators

| T-SQL | PostgreSQL |
| --- | --- |
| `a ^ b` (bitwise exclusive OR) | `a # b` |
| `~a` (bitwise NOT) | `(~a)` |
| `a != b` | `a <> b` |
| `@s + 'x'` (every operand a known string) | `s \|\| 'x'` |

In PostgreSQL, `^` raises a number to a power, so plx always rewrites the T-SQL
`^` to `#`. Use `POWER(a, b)` for exponentiation.

The bitwise operators `^`, `&` and `|` bind differently. T-SQL gives them the
same precedence as `+` and `-` and evaluates left to right. PostgreSQL binds
`#`, `&` and `|` more loosely than `+` and `-`. plx does not add parentheses.
So `1 ^ 2 + 3` gives 6 in SQL Server, but 4 in plx, which computes
`1 # (2 + 3)`. Put parentheses around a bitwise operation that is followed by
`+` or `-`: write `(1 ^ 2) + 3`.

T-SQL binds the unary `~` more tightly than any other operator, and PostgreSQL
binds it more loosely than `+` and `*`. plx puts parentheses around `~` and its
operand, so `~1 + 2` gives 0, as in SQL Server.

### String concatenation with `+`

T-SQL uses `+` both to add numbers and to join strings. PostgreSQL has no `+`
for strings: it joins them with `||`. plx has no type information for most
expressions, so it decides from what it can see in the body.

plx writes `+` as `||` only when every operand of the chain is a known string.
A chain is a run of operands joined by `+`, such as `@a + ' ' + @b`. A known
string is one of:

- a variable or parameter declared with a variable-length string type:
  `VARCHAR`, `NVARCHAR`, `TEXT` or `NTEXT`, with or without a length or `MAX`;
- a string literal, `'...'` or `N'...'`;
- `CAST` or `CONVERT` to one of those types;
- `UPPER`, `LOWER`, `LTRIM`, `RTRIM`, `TRIM`, `SUBSTRING`, `LEFT`, `RIGHT`,
  `REPLACE`, `REPLICATE`, `CONCAT` or `STUFF`;
- `ISNULL` whose first argument is a string variable or a string literal;
- `COALESCE` whose every argument is a known string. `COALESCE('1', 2)` is not
  one: SQL Server and PostgreSQL both give it the type of the number, so
  `COALESCE('1', 2) + '3'` adds and gives 4.

`SET @s += e` becomes `s := s || (e)` only when `@s` is a string variable and
`e` is a known string or a chain of them.

In every other case plx keeps `+`. This includes a chain with a number, a
column, a parenthesized expression, a `CASE` expression, a `BIT` variable, a
`CHAR` or `NCHAR` value, or a function not in the list above, such as `LEN`. It also includes a chain followed
by `-`, `&`, `|` or `^`, because PostgreSQL would group that operator with the
last operand. What happens then depends on the operands:

- A string variable next to anything that is not a known string fails when the
  function runs: `@s + LEN(@s)`, `@s + @n` with `@n` an `INT`, and `SET @s += 1`
  raise `operator does not exist: character varying + integer`. SQL Server
  converts the string to a number and adds. Write the conversion yourself, for
  example `CAST(@s AS int) + LEN(@s)`.
- A string literal next to a number adds, as in SQL Server: `'1' + 2` is 3.

plx never guesses `||` from one string operand, because a wrong guess returns a
wrong value without an error. A failing call shows you where to add a `CAST`.

```sql
CREATE FUNCTION label(id int) RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(40) = 'item';
  SET @s += ' ';
  RETURN @s + CONVERT(varchar(10), @id) + '/' + UPPER(@s);
$$;
```

Both `+` and `||` return NULL when either side is NULL, as in SQL Server with
the default `CONCAT_NULL_YIELDS_NULL ON`.

Write `||` or `CONCAT(...)` yourself to join strings that are not all known
strings, for example two columns, or a column and a literal.

`CHAR(n)` and `NCHAR(n)` values are not known strings. This covers a variable,
a parameter, and `CAST` or `CONVERT` to `CHAR` or `NCHAR`, with or without a
length. SQL Server keeps their trailing spaces when it joins them, but
PostgreSQL's `||` drops them, so `||` would return a different value without an
error. plx keeps `+`, and the call fails:
`CAST('a' AS char(3)) + 'b'` raises
`operator does not exist: character + unknown`. Say what you want instead:

- `RTRIM(@c) + 'b'` joins without the padding and gives the same value in both
  systems.
- `rpad(@c, 3) || 'b'` keeps the padding, as SQL Server does.

Assigning a value longer than `n` to a `CHAR(n)` variable is an error
(`value too long for type character(3)`). SQL Server cuts the value to `n`.

## Dynamic SQL

`EXEC('<sql>')` (equivalently `EXECUTE('<sql>')`) becomes `EXECUTE '<sql>';`.

```sql
CREATE FUNCTION run(tbl text) RETURNS void LANGUAGE plxtsql AS $$
  EXEC('TRUNCATE ' || @tbl);
$$;
```

## Set-returning functions

A bare `SELECT` (not an assignment) in a function declared `RETURNS TABLE(...)`
or `RETURNS SETOF ...` becomes `RETURN QUERY SELECT ...`.

```sql
CREATE FUNCTION series(n int) RETURNS TABLE(k int) LANGUAGE plxtsql AS $$
  SELECT g FROM generate_series(1, @n) AS g;
$$;
```

In a scalar function a bare `SELECT` becomes `PERFORM` (evaluated for effect).

## Statement separators

End statements with `;`. T-SQL treats `;` as optional in many places, but plx
uses it (together with block boundaries such as `END`, `ELSE`, and `END TRY`) to
find statement boundaries, so a missing `;` between two statements can cause them
to be read as one. Control-flow blocks (`IF`, `WHILE`, `BEGIN ... END`,
`TRY`/`CATCH`) are self-delimiting and do not require a trailing `;`.

## Triggers

A function that returns `trigger` can back a trigger. Read the row through `NEW`
(and `OLD` on updates and deletes), and assign to a field with `SET NEW.col = e`,
which becomes `NEW.col := e`. Return `NEW`.

```sql
CREATE FUNCTION stamp() RETURNS trigger LANGUAGE plxtsql AS $$
  SET NEW.total = NEW.qty * NEW.price;
  SET NEW.tag = 'row ' || CONVERT(varchar, NEW.id);
  RETURN NEW;
$$;
```

## Session SET options

Statements such as `SET NOCOUNT ON` and `SET XACT_ABORT ON` are recognized and
ignored (they configure the SQL Server session and have no PostgreSQL
equivalent). `SET @variable = ...` and `SET NEW.col = ...` are always
assignments; a `SET` with no qualified target and no assignment is treated as a
session option.

## Not supported

- `@@ROWCOUNT`, `@@ERROR`, and other `@@` globals (except `@@IDENTITY`). Use
  `GET DIAGNOSTICS` or an exception handler in a construct plx does map, or write
  that part in another dialect.
- Table variables (`DECLARE @t TABLE (...)`) and T-SQL cursors
  (`DECLARE c CURSOR`).
- `GOTO`, `WAITFOR`, and label targets.
- Transaction control (`BEGIN TRAN`, `COMMIT`, `ROLLBACK`), in a function or a
  procedure; plxtsql rejects them everywhere. plxruby, plxphp, plxjs, plxts,
  plxpython3, plxcobol and plxplsql support `COMMIT` and `ROLLBACK` in a
  procedure; plxgo does not.
- Calling a stored procedure by name (`EXEC procname`); only `EXEC('<sql>')`
  dynamic SQL is supported.
- A `SELECT @v = ... FROM ...` assignment list that reads a variable it assigns,
  such as the accumulate idiom `SELECT @t = @t + v FROM t`. plx cannot run the
  expression once per row with the updated value, so it rejects the function
  rather than return a different result. Use an aggregate such as `SUM(v)` or
  `string_agg(name, ',')`.
- String concatenation with `+` when an operand is not a known string. See
  [String concatenation with `+`](#string-concatenation-with).

See [PARITY.md](PARITY.md) for the full per-dialect feature matrix and
[USERGUIDE.md](USERGUIDE.md) for cross-dialect examples.
