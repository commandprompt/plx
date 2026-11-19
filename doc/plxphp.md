# plxphp: the PHP dialect

plxphp lets you write PostgreSQL functions with PHP syntax. At `CREATE FUNCTION`
time plx transpiles the body to plpgsql and stores the plpgsql in
`pg_proc.prosrc`. The function runs on the standard plpgsql interpreter.

The language name is `plxphp`, so it does not collide with the native PL/PHP.

## Setup

```sql
CREATE EXTENSION plx;
```

## Function basics

Blocks are brace-delimited and statements end with `;`. A scalar function ends
with an explicit `return`.

```sql
CREATE FUNCTION add(a int, b int) RETURNS int LANGUAGE plxphp AS $$
return $a + $b;
$$;
```

Function arguments are referenced with the `$` sigil, which is removed in the
generated plpgsql. Any SQL expression is valid, because expressions are passed
through to plpgsql and SQL unchanged.

### Local variables and types

Assignment creates a local. plx infers the type from the first value when it is
an integer, numeric, text, or boolean literal. Otherwise annotate the variable
with a `/*:: type */` comment placed before the `;`. An annotation before an initializer
(`$n /*:: bigint */ = 3;`) is rejected; write `$n = 3 /*:: bigint */;`.

```sql
$n = 42;              // inferred integer
$label = "count";     // inferred text
$amount /*:: numeric */;
$amount = $principal * $rate;
```

The annotation becomes the plpgsql type. It must be a type name and nothing
else (plx checks this at `CREATE FUNCTION` time), and `%TYPE` and `%ROWTYPE`
work:

```sql
$e /*:: emp%ROWTYPE */;
$s /*:: emp.sal%TYPE */;
```

A constant uses the `const` suffix in the annotation:

```sql
$pi = 3.14159 /*:: numeric const */;
```

A constant becomes a plpgsql `CONSTANT`, whose value is computed when the
function starts. So it can use literals, parameters and earlier constants, but
not another local variable: that would read the variable's initial value, not
the one assigned above it, and is rejected.

## Control flow

### if / elseif / else

```sql
if ($score >= 90) { $grade = "A"; }
elseif ($score >= 80) { $grade = "B"; }
else { $grade = "F"; }
```

`else if` is also accepted.

### switch

`switch` lowers to plpgsql `CASE`. Stacked labels become a value list. Each arm
must end with `break` or another statement that leaves the arm (`return`,
`throw`, `break N`, `continue N` with N of 2 or more, or `break` with a loop
label); genuine fall-through is rejected.
A `break` inside an `if` or `try` within an arm leaves the `switch`, as it
does in the source language. A `break` inside a loop within an arm leaves only
that loop.

```sql
switch ($n) {
  case 1: return "one";
  case 2: case 3: return "few";
  default: return "many";
}
```

### Loops

Counting `for` and `while`:

```sql
for ($i = 1; $i <= $n; $i++) {
  $total = $total + $i;
}

while ($n > 0) { $n = $n - 1; }
```

A counting `for` becomes a plpgsql integer `FOR`. That loop works out its
bounds once and keeps its own copy of the loop variable. So plx accepts a `for`
only when the result is the same:

- The condition compares the loop variable with a single value (`$i < $n` or
  `$i <= $n`).
- The step changes the loop variable (`$i++` or `$i += K`).
- The body does not assign the loop variable, or any variable in the bound or
  the step.
- The code after the loop does not read the loop variable before it assigns
  it again. A later `for ($i = ...)` assigns it.
- When the `for` is inside another loop, that loop does not read the loop
  variable on its next pass before the `for` runs again: not in its condition,
  and not in its body before the `for`.

`CREATE FUNCTION` rejects any other `for`. Write it as a `while` loop instead.

`continue` and `break` map to `CONTINUE` and `EXIT`.

`break N` and `continue N` leave or continue the Nth enclosing loop or
`switch`, counting from the innermost, as in PHP:

```sql
for ($i = 0; $i < $n; $i++) {
  for ($j = 0; $j < $n; $j++) {
    if ($j == 1) { break 2; }    // leaves both loops
  }
}
```

`CREATE FUNCTION` rejects a level of 0 and a level greater than the number of
enclosing loops and switches. It also rejects a `continue N` that lands on a
`switch`; write `break N` instead.

PHP treats a `switch` as a loop for `continue`. So a `continue` or
`continue 1` in a `switch` arm, and not in a loop within that arm, acts as
`break`, not as a `continue` of the enclosing loop. `CREATE FUNCTION` rejects it. Write `break`
to leave the `switch`, or `continue 2` to continue the loop.

### Loop labels

```sql
outer: for ($i = 1; $i <= $n; $i++) {
  for ($j = 1; $j <= $n; $j++) {
    if ($i + $j >= $lim) { break outer; }
  }
}
```

## Working with data

### Iterating a query

```sql
foreach (query("SELECT id, amount FROM orders WHERE grp = {$g}") as $row) {
  $total = $total + $row->amount;
}
```

The row variable is a record. Field access is `$row->col` or `$row['col']`. For
bind parameters or non-literal SQL, pass extra arguments:
`query($sql, $a) as $row`.

### Iterating an array

```sql
$v /*:: int */;
foreach ($values as $v) {
  $total = $total + $v;
}
```

The loop variable must be annotated with its element type.

### Fetching one row

```sql
$u = fetch_one("SELECT id, name FROM users WHERE id = {$uid}");
return $u->name;
```

`fetch_one` returns all-NULL on no row. `fetch_one!` raises on zero or more than
one row.

### Running SQL

```sql
perform("UPDATE counters SET n = n + 1 WHERE id = {$cid}");
execute("INSERT INTO t(msg) VALUES ($1)", $note);
return_query("SELECT id FROM vip ORDER BY id");   // set-returning function
```

Use bind arguments in `execute` for untrusted input.

### Cursors

```sql
$c = open_cursor("SELECT v FROM t ORDER BY v");
$row = fetch_from($c);
while (found()) {
  $total = $total + $row->v;
  $row = fetch_from($c);
}
close_cursor($c);
```

`move_cursor($c)` and `move_cursor($c, $n)` map to `MOVE`.

### Diagnostics

```sql
$rc = row_count();   // GET DIAGNOSTICS $rc = ROW_COUNT
if (found()) { return $rc; }   // the FOUND variable
```

### Set-returning functions

```sql
CREATE FUNCTION squares(n int) RETURNS SETOF int LANGUAGE plxphp AS $$
for ($i = 1; $i <= $n; $i++) { return_next($i * $i); }
return;
$$;
```

## Errors

### Throwing

```sql
throw new Exception("negative: {$v}");
```

`throw` takes a constructor call, such as `new Exception(message)`, and raises its first
argument as the error message. `CREATE FUNCTION` rejects any other thrown
expression, such as `throw msg;` or `throw a + b(c);`. To throw a computed
message, build it inside the call.

The call form `raise("notice", "message")` emits a `RAISE` at the named level
(`notice`, `warning`, `info`, `log`, `debug`, `exception`).

### Handling

```sql
try {
  execute("INSERT INTO uniq(id) VALUES ($1)", $k);
} catch (\Exception $e) {
  raise("notice", "dup on " . $e->constraint);
} finally {
  perform("INSERT INTO log(msg) VALUES ('done')");
}
```

`catch (\Exception $e)` maps to `WHEN OTHERS`, and so do `\Throwable` and
`\Error`. To catch a specific condition, write `PG::` and then the plpgsql
condition name in CamelCase. For example, `catch (PG::UniqueViolation $e)` maps
to `unique_violation`, and `catch (PG::SerializationFailure $e)` maps to
`serialization_failure`. `DivisionByZeroError` maps to `division_by_zero`. To
catch several conditions in one handler, join the classes with `|`, as in
`catch (PG::UniqueViolation | PG::CheckViolation $e)`.

`CREATE FUNCTION` rejects any other class name, such as `TypeError`, with
`unsupported exception class`. plpgsql checks a `PG::` condition name the first
time the function runs. An unknown name, such as `PG::NoSuchCondition`, then
fails with `unrecognized exception condition "no_such_condition"`.
Accessors: `$e->message` to `SQLERRM`, `$e->sqlstate` to `SQLSTATE`, and
`$e->detail`, `$e->hint`, `$e->constraint`, `$e->column`, `$e->table`,
`$e->schema`, `$e->datatype` to the matching `GET STACKED DIAGNOSTICS` fields.

The `finally` body runs once, whether the `try` block completes, returns,
breaks, continues or raises an error. After an error it runs and then raises
the error again, with its SQLSTATE, message, detail, hint and the names of
the objects it is about (schema, table, column, constraint, type); the
`CONTEXT` of the raised error points at the re-raise, not the original
statement. It does not run when the statement is cancelled (a query cancel
or `statement_timeout`) or an `assert` fails, because `WHEN OTHERS` does not
catch those two errors; for the same reason `catch (\Exception $e)` does not
catch them.

### Assertions

```sql
assert($n > 0, "must be positive");
```

## Expressions

- Interpolation: `"total is $amount"` and `"count {$n}"` splice the value.
  Single-quoted strings do not interpolate.
- Concatenation: `$a . $b` maps to `$a || $b`.
- Comparison: `==`, `===`, `!=`, `!==` map to `=` and `<>`. `$x == null` becomes
  `$x IS NULL`.
- Boolean: `&&` and `!` map to `AND` and `NOT`. `||` maps to `OR` only in an
  `if`/`while` condition or a ternary test. Elsewhere (assignments, `return`)
  it passes through as SQL `||` (string concatenation), so write `OR` there:
  `$ok = $a OR $b /*:: boolean */;`.
- Ternary: `$c ? $a : $b` becomes `CASE WHEN $c THEN $a ELSE $b END`.
- `null` becomes `NULL`.

## Building strings in a loop

Concatenating onto a string in a loop is slow in plpgsql: `s := s || 'x'` is
O(n^2) because text is immutable and each step copies the whole string. Use the
append assignment `.=`, which plx lowers to its string builder (`plx_strbuild`):

```sql
$s = "" /*:: text */;
foreach (query("SELECT name FROM t ORDER BY id") as $row) {
  $s .= $row->name;
  $s .= ",";
}
return $s;
```

On PostgreSQL 18 this is amortized O(1) per append. On PostgreSQL 13 to 17 it is
correct but not accelerated (the in-place optimization needs a PostgreSQL 18
feature).

## Trigger functions

A function returning `trigger` can be used as a trigger. Assign to `$NEW` fields
and return `$NEW`:

```sql
CREATE FUNCTION stamp() RETURNS trigger LANGUAGE plxphp AS $$
$NEW->tag = "row {$NEW->id}";
return $NEW;
$$;
```

`$NEW`, `$OLD`, and the `TG_` variables are available. Assigning to a record
field works with the arrow form (`$NEW->col = e`) or the array-element form
(`$NEW['col'] = e`).

## Semantic differences

These are intentional. plx pins semantics to SQL and plpgsql.

- Decimal literals infer `numeric`, not a floating-point type.
- Comparisons use SQL three-valued logic: `==`, `===`, `!=`, `!==` map to `=`
  and `<>`, so a comparison involving NULL is unknown. Only a comparison with the
  literal `null` becomes `IS NULL` / `IS NOT NULL`. A positive `if`/`while`
  condition treats NULL as false.
- `===` and `!==` do not preserve PHP strict-type semantics; they behave like
  `==` and `!=` and use SQL type resolution. `"1" === 1` is false in PHP but
  compares as equal here. Compare like-typed values.
- Interpolating a NULL propagates it, so the whole string becomes NULL rather
  than rendering the value as an empty string. In a `throw` or `raise` message,
  including the `raise("level", ...)` call form, the value falls back to an
  empty string instead, so one NULL cannot swallow the message. An `assert`
  message gets no fallback: a NULL there replaces your message with plpgsql's
  generic `assertion failed`.
- Integer division and modulo follow SQL (truncate toward zero).
- PHP truthiness is not emulated. A condition must be a boolean expression. A
  non-boolean condition is not rejected at CREATE time. When the function runs, plpgsql coerces it
  through its text form: a value whose text is a boolean literal (`0`, `1`,
  `t`, `yes`, `off`, ...) is silently accepted, and anything else raises
  `invalid input syntax for type boolean`.
- Locals are function-scoped, matching PHP function scope.

## Not supported

Rejected at `CREATE FUNCTION` time with a line number:

- Function or class definitions, namespaces, `use`, includes.
- A `for` that is not a counting loop. Only `for ($v = LO; $v < HI; $v++)` and
  `for (...; $v += K)` are supported, under the rules in [Loops](#loops).
- `switch` fall-through (end each case with `break` or `return`).

Not translated:

- Closures, anonymous functions, and array and object literals as general
  values. An unannotated assignment of one fails at `CREATE FUNCTION` time
  (`cannot infer a PostgreSQL type`); an annotated one passes through and fails
  when the function runs.
- `foreach ($a as $k => $v)`: the key is ignored; only value iteration is
  supported.

See [PARITY.md](PARITY.md) for the full plpgsql construct matrix and
[ARCHITECTURE.md](ARCHITECTURE.md) for how plx maps to the plpgsql engine.
