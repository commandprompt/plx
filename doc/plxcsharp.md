# plxcsharp: the C# dialect

plxcsharp lets you write PostgreSQL functions with C# syntax. At
`CREATE FUNCTION` time plx transpiles the body to plpgsql and stores the plpgsql
in `pg_proc.prosrc`. The function then runs on the standard plpgsql interpreter.

The language name is `plxcsharp`.

C# is a brace-family language, but it differs from plpgsql in ways a keyword map
cannot cover: declarations put the type first, strings come in several forms,
`+` joins strings, `switch` sections end with `break`, and exceptions are classes.
plxcsharp therefore reads the body with a C# parser that resolves types, then
lowers the statements through the same engine the other dialects use. The
generated plpgsql is ordinary and readable; `pg_proc.prosrc` shows exactly what
runs.

## Setup

```sql
CREATE EXTENSION IF NOT EXISTS plx;
```

## Function basics

The body is the statement list of a C# method body: no method signature, no
class, no `using` directives. Statements end with `;` and blocks use braces.
Both the K&R and the Allman brace styles work, and a single statement may follow
`if`, `else`, `while`, `for`, `foreach` and `do` without braces.

```sql
CREATE FUNCTION add(a int, b int) RETURNS int LANGUAGE plxcsharp AS $$
return a + b;
$$;

CREATE FUNCTION grade(score int) RETURNS text LANGUAGE plxcsharp AS $$
if (score >= 90)
{
    return "A";
}
else if (score >= 80)
    return "B";
return "F";
$$;
SELECT add(2, 3), grade(85);
```

### The signature uses PostgreSQL types

The parameter and return types in `CREATE FUNCTION` are parsed by PostgreSQL
before plx sees the body, so they are PostgreSQL type names: write
`RETURNS bigint`, not `RETURNS long`. Inside the body, C# type names in
declarations are translated (see Declarations and types). Parameters are
referenced by name. `OUT` and `INOUT` parameters are assigned by name.

```sql
CREATE FUNCTION split_name(full_name text, OUT first_name text, OUT last_name text)
LANGUAGE plxcsharp AS $$
int space = full_name.IndexOf(" ");
first_name = full_name.Substring(0, space);
last_name = full_name.Substring(space + 1);
$$;
SELECT * FROM split_name('Ada Lovelace');
```

## Declarations and types

A local is declared with its type first, as in C#. Every local is hoisted into
the plpgsql `DECLARE` block.

```csharp
int count = 0;
string name;                    // declared, assigned later
var total = 0L;                 // var takes the type of its value: bigint
const int MaxItems = 100;       // a plpgsql CONSTANT
int? maybe = null;              // nullable: the same as int
decimal[] prices = {9.99m, 4.50m};
numeric(10,2) amount = 0;       // a PostgreSQL type name works as written
orders row;                     // so does a table row type
```

Several declarators may share one statement (`int a = 1, b = 2;`).

### Type mapping

| C# | PostgreSQL |
|---|---|
| `int`, `Int32`, `ushort` | `integer` |
| `long`, `Int64`, `uint` | `bigint` |
| `short`, `byte`, `sbyte` | `smallint` |
| `ulong`, `decimal` | `numeric` |
| `double` | `double precision` |
| `float` | `real` |
| `bool` | `boolean` |
| `string`, `char` | `text` |
| `DateTime` | `timestamp` |
| `DateTimeOffset` | `timestamptz` |
| `DateOnly` | `date` |
| `TimeOnly` | `time` |
| `TimeSpan` | `interval` |
| `Guid` | `uuid` |
| `byte[]` | `bytea` |
| `T[]` | the mapped `T` as a PostgreSQL array, so `int[]` is `integer[]` |
| `T?` | `T` |
| `StringBuilder` | `text`, built with the plx string builder (see Building strings) |
| any other name | used as a PostgreSQL type name: `jsonb`, `numeric(10,2)`, `orders` |

A `char` is stored as one-character `text`, so it compares and concatenates like
a string. Arithmetic on a `char` is rejected; convert with `(int)c` (which is
`ascii(c)`) or `(char)n` (which is `chr(n)`).

`var` takes the type of its initializer, which must be known: a literal, a
string expression, a cast, a mapped member, an array creation, another typed
variable, or `fetch_one(...)` (a record). When the type cannot be worked out,
for example `var x = some_sql_function(y);`, declare it explicitly. An unsuffixed
real literal such as `1.5` is PostgreSQL `numeric`, as in the other dialects, so
`var x = 1.5;` declares `numeric`; write `1.5d` or `double x = 1.5;` for a double.
The literal suffixes `L`, `U`, `UL`, `F`, `D` and `M` select `bigint`, `bigint`,
`numeric`, `real`, `double precision` and `numeric`.

### Names

PostgreSQL ignores case in unquoted names, so `total` and `Total` are the same
variable. plxcsharp rejects two locals that differ only in case, and a local
whose name matches a parameter. A local cannot be named after a word that
PostgreSQL or PL/pgSQL reserves (`user`, `order`, `limit`, `end`, `left`,
`both`, `by`, ...); the error names the word.

Locals are function-scoped, as everywhere in plx. Declaring the same name again
in a sibling block is allowed when the type is the same; a different type is an
error.

## C# to PostgreSQL translations

### Statements

| C# | plpgsql |
|---|---|
| `int x = e;` | `x integer;` in `DECLARE`, then `x := e;` (folded into the declaration at the top level) |
| `x = e;` | `x := e;` |
| `x += e;` (also `-= *= /= %= &= \|= ^= <<= >>=`) | `x := x + e;` |
| `s += e;` on a `string` local | the plx string builder (see Building strings) |
| `x++;` / `++x;` / `x--;` | `x := x + 1;` / `x := x - 1;` |
| `x ??= e;` | `x := COALESCE(x, e);` |
| `if (c) { } else if (c) { } else { }` | `IF c THEN ... ELSIF c THEN ... ELSE ... END IF;` |
| `while (c) { }` | `WHILE c LOOP ... END LOOP;` |
| `do { } while (c);` | `LOOP ... EXIT WHEN (c) IS NOT TRUE; END LOOP;` |
| `for (int i = a; i < b; i++)` | `FOR i IN a..(b) - 1 LOOP` |
| `for (int i = a; i >= b; i -= 2)` | `FOR i IN REVERSE a..b BY 2 LOOP` |
| other `for (init; cond; iter)` | `init`, then a loop that tests `cond` and runs `iter` on every pass |
| `for (;;)` | `LOOP ... END LOOP;` |
| `foreach (var r in query(sql))` | `FOR r IN sql LOOP` |
| `foreach (int v in arr)` | `FOREACH v IN ARRAY arr LOOP` |
| `switch (x) { case 1: ... break; default: ... }` | `CASE x WHEN 1 THEN ... ELSE ... END CASE;` |
| `break;` / `continue;` | `EXIT;` / `CONTINUE;` |
| `return e;` | `RETURN e;` |
| `yield return e;` / `yield break;` | `RETURN NEXT e;` / `RETURN;` |
| `throw new Exception(m);` | `RAISE EXCEPTION '%', m;` |
| `throw;` | `RAISE;` |
| `try { } catch (E e) { } finally { }` | a `BEGIN ... EXCEPTION ... END` block |
| `Console.WriteLine(x);` | `RAISE NOTICE '%', x;` |
| `Debug.Assert(c, m);` | `ASSERT c, m;` |

### Expressions

| C# | PostgreSQL |
|---|---|
| `==`, `!=` | `=`, `<>` |
| `x == null`, `x is null` | `x IS NULL` |
| `x != null`, `x is not null` | `x IS NOT NULL` |
| `&&`, `\|\|`, `!` | `AND`, `OR`, `NOT` |
| `&`, `\|`, `^` on `bool` | `AND`, `OR`, `<>` |
| `&`, `\|`, `^`, `~`, `<<`, `>>` on integers | `&`, `\|`, `#`, `~`, `<<`, `>>` |
| `"a" + x` (either side a string) | `'a' \|\| x` |
| `a + b` (numbers) | `a + b` |
| `c ? a : b` | `CASE WHEN c THEN a ELSE b END` |
| `a ?? b` | `COALESCE(a, b)` |
| `x switch { 1 => "a", _ => "b" }` | `CASE x WHEN 1 THEN 'a' ELSE 'b' END` |
| `(int)x` | `(trunc(x))::integer` (C# truncates); `(x)::integer` when `x` is already an integer |
| `(double)x`, `(string)x`, ... | `(x)::double precision`, `(x)::text`, ... |
| `a[i]` | `a[i + 1]` (C# arrays are 0-based, PostgreSQL arrays 1-based) |
| `s[i]` on a string | `substr(s, i + 1, 1)` |
| `row["col"]` | `row.col` |
| `x?.Member`, `x!` | `x.Member`, `x` (a NULL already propagates through SQL functions) |
| `new[] {1, 2}`, `[1, 2]` | `ARRAY[1, 2]` |
| `new int[] {1, 2}`, `int[] a = {1, 2}` | `ARRAY[1, 2]::integer[]` |
| `new int[n]` | `array_fill(0::integer, ARRAY[n])` |
| `nameof(x)` | `'x'` |
| `null`, `true`, `false` | `NULL`, `true`, `false` |

Any other name passes through to SQL: a call to a function plx does not map is a
call to the PostgreSQL function of that name (`upper(s)`, `now()`,
`my_schema.my_func(x)`), and a bare name such as `current_date` is the SQL
value. A C# named argument `f(x: 1)` becomes SQL named notation `f(x => 1)`.

## Strings

All the C# string forms work, and each becomes an ordinary SQL string literal:

```csharp
string a = "tab\there, quote \" and \u00e9";   // regular: C# escapes
string b = @"C:\temp\file ""quoted""";           // verbatim: backslashes are literal
string c = """
    SELECT "raw" text
      keeps its relative indentation
    """;                                          // raw: indentation is stripped
char ch = 'x';
```

A regular string supports `\'`, `\"`, `\\`, `\a`, `\b`, `\e`, `\f`, `\n`, `\r`,
`\t`, `\v`, `\xH..HHHH`, `\uHHHH` and `\UHHHHHHHH`. A NUL character (`\0`) cannot
be stored in a PostgreSQL string and is rejected.

### Interpolation

An interpolated string `$"..."` (also `$@"..."` and the raw `$"""..."""`) puts
each `{expression}` into the text. `{{` and `}}` are literal braces; in a raw
string with two dollar signs (`$$"""..."""`) holes are written `{{x}}` and a
single brace is literal.

```sql
CREATE FUNCTION greet(nm text, n int) RETURNS text LANGUAGE plxcsharp AS $$
return $"{nm} has {n} item{(n == 1 ? "" : "s")}, average {10.0 / n:F2}";
$$;
SELECT greet('Ann', 3);
```

The format specifier `:F` (and `:F0` to `:F99`) rounds to that many decimal
places. Other format specifiers and alignment (`{x,10}`) are rejected; format the
value with `to_char()` inside the braces instead.

An interpolated NULL makes the whole string NULL, which is what SQL `||` does
and what the other plx dialects do. In a message for `throw` or
`Console.WriteLine` each NULL value becomes an empty string instead, so one NULL
cannot swallow the message.

### The `+` operator

`+` is string concatenation when either operand is known to be a string: a
string or character literal, an interpolated string, a `string` or `char` local
or parameter, a mapped string member such as `s.Trim()`, a cast to `string`, or
the result of a concatenation. Otherwise `+` is numeric addition. C# evaluates
`+` from left to right, and so does the translation:

```sql
CREATE FUNCTION plus_demo(a int, b int) RETURNS text LANGUAGE plxcsharp AS $$
return ("x" + a + b) + " " + (a + b + "x");
$$;
SELECT plus_demo(1, 2);
```

The result is `x12 3x`, as in C#. A NULL operand makes the result NULL (C#
treats it as an empty string); `string.Concat(a, b)` keeps the C# behaviour,
because it is SQL `concat()`, which skips NULLs. When neither operand is known to
be a string, for example two record fields, convert one: `r.first + "" + r.last`
or `$"{r.first}{r.last}"`.

### Building strings

Appending to a string in a loop with `s = s + x` copies the whole string on every
pass. Use `s += x` on a `string` local, or a `StringBuilder`: both lower to the
plx string builder (`plx_strbuild`), which appends in amortized constant time on
PostgreSQL 18.

```sql
CREATE FUNCTION csv(n int) RETURNS text LANGUAGE plxcsharp AS $$
var sb = new StringBuilder();
for (int i = 1; i <= n; i++)
{
    if (i > 1) sb.Append(",");
    sb.Append(i);
}
return sb.ToString();
$$;
SELECT csv(5);
```

`StringBuilder` supports `Append(x)`, `AppendLine()`, `AppendLine(x)`,
`AppendFormat(format, args)`, `Clear()`, `ToString()` and `Length`, and `Append`
calls may be chained.

## .NET members

plxcsharp maps a small set of .NET members to SQL. Anything else is rejected
when the function is created, except a call on a name plx does not know, which is
taken to be a schema-qualified SQL function.

| C# | PostgreSQL |
|---|---|
| `s.Length` | `length(s)` (`cardinality(a)` on an array) |
| `s.ToUpper()`, `s.ToLower()` (and the `Invariant` forms) | `upper(s)`, `lower(s)` |
| `s.Trim()`, `s.TrimStart()`, `s.TrimEnd()` (optionally with characters) | `btrim`, `ltrim`, `rtrim` |
| `s.Contains(x)` | `strpos(s, x) > 0` (`x = ANY(a)` on an array) |
| `s.StartsWith(x)`, `s.EndsWith(x)` | `starts_with(s, x)`, a suffix test |
| `s.IndexOf(x)` | `strpos(s, x) - 1` (0-based, -1 when absent) |
| `s.Substring(i)`, `s.Substring(i, n)` | `substr(s, i + 1)`, `substr(s, i + 1, n)` |
| `s.Replace(a, b)` | `replace(s, a, b)` |
| `s.PadLeft(n)`, `s.PadRight(n, c)` | `lpad`, `rpad` (never truncating, as in C#; `s` must be a variable, field or literal) |
| `s.Split(sep)` | `string_to_array(s, sep)` |
| `x.ToString()`, `x.ToString("F2")` | `(x)::text`, `round(x, 2)` |
| `x.Equals(y)` | `x = y` |
| `string.IsNullOrEmpty(s)` | `COALESCE(s, '') = ''` |
| `string.IsNullOrWhiteSpace(s)` | true for NULL, empty or whitespace only |
| `string.Join(sep, values)` | `array_to_string(values, sep, '')` |
| `string.Concat(a, b, ...)` | `concat(a, b, ...)` |
| `string.Format("{0} {1,5}", a, b)` | `format('%1$s %2$5s', a, b)` |
| `string.Empty`, `Environment.NewLine` | `''`, a newline |
| `Math.Abs`, `Max`, `Min`, `Floor`, `Ceiling`, `Truncate`, `Round`, `Sqrt`, `Pow`, `Exp`, `Log`, `Log10`, `Sign` | `abs`, `GREATEST`, `LEAST`, `floor`, `ceil`, `trunc`, `round`, `sqrt`, `power`, `exp`, `ln`, `log`, `sign` |
| `Math.PI`, `Math.E` | `pi()`, `exp(1)` |
| `Convert.ToInt32(x)` and the other `Convert.To...` | `(x)::integer`, ... |
| `int.Parse(s)`, `decimal.Parse(s)`, `DateTime.Parse(s)`, `Guid.Parse(s)`, ... | `(s)::integer`, ... |
| `int.MaxValue`, `long.MinValue`, `short.MaxValue`, ... | the limit as a literal |
| `DateTime.Now`, `DateTime.UtcNow`, `DateTime.Today` | `localtimestamp`, `timezone('UTC', now())`, `current_date::timestamp` |
| `DateTimeOffset.Now`, `DateTimeOffset.UtcNow` | `now()` |
| `d.Year`, `Month`, `Day`, `Hour`, `Minute`, `Second`, `DayOfYear`, `DayOfWeek`, `Date` | `extract(...)`, `date_trunc('day', d)` |
| `d.AddDays(n)`, `AddHours`, `AddMinutes`, `AddSeconds`, `AddMonths`, `AddYears` | `d + n * interval '1 day'`, ... |
| `Guid.NewGuid()`, `Guid.Empty` | `gen_random_uuid()`, the zero UUID |
| `char.IsDigit(c)`, `IsLetter`, `IsWhiteSpace`, `IsLetterOrDigit`, `char.ToUpper(c)`, `char.ToLower(c)` | a regular expression test, `upper`, `lower` |

`DateTime.Now` and the other clock members read the transaction's start time,
as `now()` does, not the wall clock at the moment of the call.

## Control flow

### if, while and do

`do { } while (c);` runs its body at least once. A `continue` inside it goes to
the test, as in C#.

### for

A counting loop over an `int` or `short` variable, with a `<`, `<=`, `>` or `>=`
test against a bound and a step of `++`, `--`, `+= K` or `-= K` (a positive
literal `K`), becomes a plpgsql integer `FOR`. That needs the loop body to leave
the variable and the bound unchanged, and the bound to make no function call:
then evaluating the bound once, as plpgsql does, is the same as testing it on
every pass, as C# does.

Any other `for` keeps C# semantics exactly: the initializers run once, the test
runs before every pass, and the iterators run after every pass, also after a
`continue`.

```sql
CREATE FUNCTION loops(n int) RETURNS text LANGUAGE plxcsharp AS $$
long sum = 0;
for (int i = 1; i <= n; i++) sum += i;              // integer FOR
string pairs = "";
for (int i = 0, j = n; i < j; i++, j--)             // general form
{
    if (i == 1) continue;
    pairs += $"({i},{j})";
}
return $"{sum} {pairs}";
$$;
SELECT loops(6);
```

### foreach

`foreach` iterates a query (see Working with data), an array, or the characters
of a string. Over an array the element type comes from the array's type, or from
the declared loop variable.

```sql
CREATE FUNCTION count_vowels(words text[]) RETURNS int LANGUAGE plxcsharp AS $$
int n = 0;
foreach (var w in words)
    foreach (char c in w.ToLower())
        if ("aeiou".Contains(c)) n++;
return n;
$$;
SELECT count_vowels(ARRAY['Education', 'sky']);
```

### switch

A `switch` statement becomes a plpgsql `CASE`. Stacked labels share a section,
`default` may appear anywhere, and a switch with no matching section and no
`default` does nothing. Each section must end in `break`, `return`, `throw` or
`continue`, as C# requires. A `break` inside a nested statement of a section
leaves the switch, not an enclosing loop, as in C#.

```sql
CREATE FUNCTION day_kind(d int) RETURNS text LANGUAGE plxcsharp AS $$
string kind;
switch (d)
{
    case 0:
    case 6:
        kind = "weekend";
        break;
    case 1: case 2: case 3: case 4: case 5:
        kind = "weekday";
        break;
    default:
        throw new ArgumentOutOfRangeException($"no day {d}");
}
return kind;
$$;
SELECT day_kind(0), day_kind(3);
```

A switch expression (`x switch { ... }`) takes constant patterns, `_`, `null`,
`not null`, the relational patterns `< e`, `<= e`, `> e`, `>= e`, and `when`
guards. With anything but constants it needs a variable as its subject.

```sql
CREATE FUNCTION size_of(n int) RETURNS text LANGUAGE plxcsharp AS $$
return n switch { < 0 => "negative", 0 => "zero", < 10 => "small", _ => "large" };
$$;
SELECT size_of(-1), size_of(0), size_of(5), size_of(50);
```

## Working with data

The SQL intrinsics are the same as in the other plx dialects and keep their plx
names: `query`, `fetch_one`, `fetch_one!`, `execute`, `perform`,
`return_query`, `open_cursor`, `fetch_from`, `close_cursor`, `move_cursor`,
`row_count()`, `found()`, `call`, `commit()`, `rollback()`, `raise`, `assert`,
`emit` and `return_next`.

### SQL templates

When the SQL argument is an interpolated string, it is a template: each hole is
a value bound as a parameter, never spliced into the SQL text, so it cannot
inject SQL. `ident(x)` splices a quoted identifier, `literal(x)` a quoted
literal, and `raw(x)` trusted SQL text. A plain string is static SQL. See
[DYNAMIC-SQL.md](DYNAMIC-SQL.md).

```sql
CREATE TABLE orders (id int PRIMARY KEY, grp int, amount bigint);
INSERT INTO orders VALUES (1, 1, 100), (2, 1, 250), (3, 1, 75), (4, 2, 500);

CREATE FUNCTION order_total(g int) RETURNS bigint LANGUAGE plxcsharp AS $$
long total = 0;
foreach (var row in query($"SELECT amount FROM orders WHERE grp = {g}"))
{
    total += row.amount;
}
return total;
$$;
SELECT order_total(1);
```

The loop variable is a record, and a typed loop variable takes a single column:
`foreach (long amt in query("SELECT amount FROM orders"))`. A table row type works
too: `foreach (orders o in query("SELECT * FROM orders"))`. Bind arguments after
the SQL are `$1`, `$2`, ...: `query("SELECT ... WHERE grp = $1", g)`.

Static SQL sees the function's variables, so a local or parameter with the same
name as a column the SQL uses makes that SQL ambiguous, and plpgsql reports
`column reference "amount" is ambiguous` when it runs. Give locals names that
differ from the columns, or qualify the column with its table
(`orders.amount`).

A multi-line query reads well as a raw interpolated string:

```csharp
foreach (var r in query($"""
    SELECT id, amount
    FROM orders
    WHERE grp = {g}
    ORDER BY id
    """))
{
    ...
}
```

### Single rows, statements, and dynamic SQL

```sql
CREATE FUNCTION order_info(oid int) RETURNS text LANGUAGE plxcsharp AS $$
var o = fetch_one($"SELECT id, amount FROM orders WHERE id = {oid}");
long n = fetch_one("SELECT count(*) FROM orders");
perform($"UPDATE orders SET amount = amount WHERE id = {oid}");
int changed = row_count();
string col = "amount";
long biggest = fetch_one($"SELECT max({ident(col)}) FROM orders");
return $"order {o.id}: {o.amount} of {n}, changed {changed}, max {biggest}";
$$;
SELECT order_info(2);
```

`fetch_one` returns a row with all fields NULL when nothing matches;
`fetch_one!` raises `no_data_found` or `too_many_rows`. With `var` (or a row
type) the target is a record and its columns are read as `o.col` or `o["col"]`.
With a scalar type such as `long` the variable takes the query's single column.
`execute(sql, a, b)` runs dynamic SQL with bind arguments.

### Cursors

```csharp
var c = open_cursor("SELECT amount FROM orders ORDER BY id");
var row = fetch_from(c);
while (found())
{
    total += row.amount;
    row = fetch_from(c);
}
close_cursor(c);
```

### Set-returning functions

A function declared `RETURNS SETOF` produces rows with `yield return` and ends
with `yield break` or `return;`. `return_query(sql)` appends a whole query.

```sql
CREATE FUNCTION squares(n int) RETURNS SETOF int LANGUAGE plxcsharp AS $$
for (int i = 1; i <= n; i++)
    yield return i * i;
$$;
SELECT * FROM squares(4);
```

With `RETURNS TABLE(...)` the columns are output parameters: assign them and
call `return_next()`.

```sql
CREATE FUNCTION order_lines(g int) RETURNS TABLE(id int, label text) LANGUAGE plxcsharp AS $$
foreach (orders o in query($"SELECT * FROM orders WHERE grp = {g} ORDER BY id"))
{
    id = o.id;
    label = $"#{o.id}: {o.amount}";
    return_next();
}
$$;
SELECT * FROM order_lines(1);
```

### Procedures and DO blocks

A procedure can end the transaction with `commit()` or `rollback()`. A `DO`
block takes C# too:

```sql
DO LANGUAGE plxcsharp $$
for (int i = 1; i <= 2; i++)
    Console.WriteLine($"step {i}");
$$;
```

## Errors

### Throwing

`throw new SomeException(message)` raises an error with that message. The class
picks the SQLSTATE: one of the mapped classes below, otherwise `P0001`
(`raise_exception`). Any class name ending in `Exception` may be thrown. With no
argument the message is the class name. Inside a `catch`, passing the caught
exception as a second argument (`new Exception("failed", e)`) puts its message
into the error's detail.

To raise a specific SQLSTATE with a detail and a hint, use Npgsql's
`PostgresException` constructor, `(messageText, severity, invariantSeverity,
sqlState)`, with the optional `detail` and `hint` arguments (by position or by
name):

```sql
CREATE FUNCTION check_amount(v int) RETURNS int LANGUAGE plxcsharp AS $$
if (v < 0)
    throw new PostgresException($"negative amount {v}", "ERROR", "ERROR", "22023",
                                detail: "amounts are never negative", hint: "pass zero or more");
return v;
$$;
SELECT check_amount(5);
```

`throw;` inside a `catch` rethrows the caught error unchanged; so does `throw e;`
for the caught variable `e`.

### Catching

`try`/`catch`/`finally` becomes a plpgsql exception block. The `finally` block
runs on every exit: normal completion, an error (which is raised again after
it), and a `return`, `break` or `continue` out of the `try`.

| catch clause | plpgsql condition |
|---|---|
| `catch`, `catch (Exception e)`, `catch (PostgresException e)`, `catch (NpgsqlException e)`, `catch (DbException e)`, `catch (SystemException e)` | `OTHERS` |
| `catch (DivideByZeroException e)` | `division_by_zero` |
| `catch (OverflowException e)` | `numeric_value_out_of_range` |
| `catch (FormatException e)` | `invalid_text_representation` |
| `catch (ArgumentNullException e)` | `null_value_not_allowed` |
| `catch (PostgresException e) when (e.SqlState == "23505")` | `SQLSTATE '23505'` |
| `... when (e.SqlState == PostgresErrorCodes.UniqueViolation)` | `unique_violation` |
| `... when (e.SqlState == "23502" \|\| e.SqlState is "22003" or "22P02")` | `SQLSTATE '23502' OR ...` |

A `when` filter compares `e.SqlState` with a five-character SQLSTATE string or a
`PostgresErrorCodes` name, alone or joined with `||` or the `is ... or ...`
pattern. A `PostgresErrorCodes` name becomes the plpgsql condition name in
snake case (`UniqueViolation` is `unique_violation`); a name plpgsql does not
know is reported when the function first runs. Catch clauses are tried in
order, as in C#.

The caught exception exposes these properties:

| C# | plpgsql |
|---|---|
| `e.Message`, `e.MessageText`, `e.ToString()` | `SQLERRM` |
| `e.SqlState` | `SQLSTATE` |
| `e.Detail`, `e.Hint` | `PG_EXCEPTION_DETAIL`, `PG_EXCEPTION_HINT` |
| `e.ConstraintName`, `e.ColumnName`, `e.TableName`, `e.SchemaName`, `e.DataTypeName` | the `GET STACKED DIAGNOSTICS` field of that name |

```sql
CREATE FUNCTION add_order(i int) RETURNS text LANGUAGE plxcsharp AS $$
try
{
    execute("INSERT INTO orders VALUES ($1, 9, 0)", i);
    return "inserted";
}
catch (PostgresException e) when (e.SqlState == PostgresErrorCodes.UniqueViolation)
{
    return $"duplicate: {e.ConstraintName}";
}
catch (Exception e)
{
    return $"failed ({e.SqlState}): {e.Message}";
}
$$;
SELECT add_order(1), add_order(NULL);
```

### Messages and assertions

`Console.WriteLine(x)` (and `Console.Write`) raises a `NOTICE`. With a format
string and arguments it formats the message like `string.Format`. The
`raise(level, message)` intrinsic raises at any level (`debug`, `log`, `info`,
`notice`, `warning`, `exception`). `Debug.Assert(condition, message)` and
`assert(condition, message)` become plpgsql `ASSERT`.

## Trigger functions

A function returning `trigger` reads and assigns `NEW` and `OLD` fields and
returns `NEW` (or `null` to skip the row in a `BEFORE` trigger). `TG_OP`,
`TG_TABLE_NAME` and the other `TG_` variables are available; `TG_ARGV` keeps its
plpgsql indexing, which already starts at 0.

```sql
CREATE TABLE products (id int, code text, label text, revisions int DEFAULT 0);

CREATE FUNCTION products_stamp() RETURNS trigger LANGUAGE plxcsharp AS $$
NEW.code = NEW.code.Trim().ToUpper();
NEW.label = TG_OP == "INSERT" ? $"new {NEW.code}" : $"was {OLD.code}";
NEW.revisions += 1;
return NEW;
$$;

CREATE TRIGGER products_stamp BEFORE INSERT OR UPDATE ON products
  FOR EACH ROW EXECUTE FUNCTION products_stamp();
INSERT INTO products(id, code) VALUES (1, ' ab ');
UPDATE products SET code = 'cd';
SELECT * FROM products;
```

## Debugging

`pg_proc.prosrc` holds the generated plpgsql, and [DEBUGGING.md](DEBUGGING.md)
shows how to recover the original C# from it. An error at `CREATE FUNCTION`
names the line of the C# source. With `SET client_min_messages = debug1`,
`CREATE FUNCTION` also logs the intermediate program that the C# parser hands
to the plpgsql lowering, with every expression already in SQL, which shows how
each construct was read.

## Semantic differences

These are intentional. plx pins semantics to SQL and plpgsql rather than
emulating the .NET runtime.

- A NULL takes part in SQL three-valued logic. A comparison with a NULL operand
  is unknown, so neither `a == b` nor `a != b` is true; only a comparison with
  the literal `null` becomes `IS NULL`. A positive `if`/`while` condition treats
  unknown as false.
- `+` with a NULL string operand yields NULL, where C# treats null as an empty
  string. `string.Concat` keeps the C# behaviour.
- Integer arithmetic raises `numeric_value_out_of_range` on overflow, where C#
  wraps silently (in an `unchecked` context). Integer division and `%` truncate
  toward zero in both.
- An unsuffixed real literal is exact `numeric`, not a `double`.
- A `numeric` (`decimal`) value rounds half away from zero, where .NET rounds
  half to even. That covers `Math.Round` on a `decimal`, `Math.Round(x,
  digits)`, the `Convert.To...` conversions of a `decimal`, and the `:F` format.
  A `double` rounds half to even in both. A cast `(int)x` truncates toward zero,
  as in C#.
- `bool` values print as `true` and `false`, not `True` and `False`.
- Reading an array past its end yields NULL and assigning past it extends the
  array, where C# throws `IndexOutOfRangeException`.
- A switch expression with no `_` arm yields NULL when nothing matches, where C#
  throws.
- The bound of a counting `for` loop is evaluated once, only where that cannot
  be observed (see for).
- `DateTime.Now` and the other clock members return the transaction start time.
- Locals are function-scoped, and a local is NULL until assigned (C# would
  refuse to compile the read).
- Strings compare with the database collation. Equality is exact; `<` and `>`
  between strings are not C# (which has no such operators) but pass through to
  SQL.

## Not supported

Rejected at `CREATE FUNCTION` time with a source line number:

- Methods, local functions, lambdas, delegates, classes, structs, records,
  interfaces, enums, namespaces, `using`, attributes, generics, and
  preprocessor directives.
- `object` and `dynamic`, tuples and deconstruction, `ref`/`out`/`in`
  arguments, pointers and `unsafe`, `fixed`, `stackalloc`.
- LINQ, `List<T>`, `Dictionary<K,V>` and the other collections; use arrays or
  SQL.
- Multi-dimensional and jagged arrays, ranges and indexes from the end
  (`a[1..3]`, `^1`).
- `new` other than array creation, `new StringBuilder()` in a declaration, and
  `new ...Exception(...)` after `throw`; object and collection initializers
  other than array initializers.
- `goto`, labels, `lock`, `checked`/`unchecked`, `async`/`await`, and `yield` in
  anything but a set-returning function.
- An assignment, `++` or `--` inside an expression (`x = y = 0`, `a[i++]`).
- `case` labels with patterns (`case int n:`, `case > 5:`, `case null:`, `when`
  guards, `or`/`and`); use stacked constant labels or `if`.
- `is` and switch-expression patterns other than those listed above, and `as`.
- Interpolation format specifiers other than `:F`, and alignment (`{x,10}`).
- `catch` of exception classes other than those in the table above, and `when`
  filters other than `e.SqlState` comparisons.
- `default`, `typeof`, `sizeof`, `this`, `base`.
- Arithmetic on `char` values.
- .NET members other than those listed in .NET members.
- Calling a function for its side effects as a statement (`DoWork(x);`); use
  `perform("SELECT do_work(...)")`.
- A brace inside a string literal that is itself inside an interpolation hole,
  and string literals nested more than two interpolations deep; assign the inner
  string to a variable first.
- Non-ASCII characters outside strings and comments, including in names.

See [PARITY.md](PARITY.md) for the plpgsql construct matrix and
[LIMITATIONS.md](LIMITATIONS.md) for the constraints every dialect shares.
