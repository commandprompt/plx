# plxpython3: the Python dialect

plxpython3 lets you write PostgreSQL functions with Python syntax. At
`CREATE FUNCTION` time plx transpiles the body to plpgsql and stores the plpgsql
in `pg_proc.prosrc`. The function runs on the standard plpgsql interpreter.

The language name is `plxpython3`, so it does not collide with the native
PL/Python (`plpython3u`). plxpython3 is a trusted language, unlike `plpython3u`.

## Setup

```sql
CREATE EXTENSION plx;
```

## Function basics

Blocks are defined by indentation, as in Python. A compound-statement header ends
with `:` and its body is indented. A scalar function ends with an explicit
`return`.

```sql
CREATE FUNCTION add(a int, b int) RETURNS int LANGUAGE plxpython3 AS $$
return a + b
$$;
```

Function arguments are referenced by name. Any SQL expression is valid, because
expressions are passed through to plpgsql and SQL unchanged.

### Local variables and types

Assignment creates a local. plx infers the type from the first value when it is
an integer, numeric, text, or boolean literal. Otherwise annotate the variable
with `#:: type`.

```sql
n = 42            # inferred integer
label = "count"   # inferred text
amount #:: numeric
amount = principal * rate
```

The annotation text is emitted verbatim, so `%TYPE` and `%ROWTYPE` work:

```sql
e #:: emp%ROWTYPE
s #:: emp.sal%TYPE
```

A constant uses the `const` suffix on the annotation:

```sql
pi = 3.14159 #:: numeric const
```

## Control flow

### if / elif / else

```sql
if score >= 90:
    grade = "A"
elif score >= 80:
    grade = "B"
else:
    grade = "F"
```

### Loops

`for ... in range(...)` is an integer loop. `range(n)` is 0 to n-1, `range(a, b)`
is a to b-1, `range(a, b, step)` adds a step.

```sql
for i in range(1, n + 1):
    total = total + i

while n > 0:
    n = n - 1
```

`continue` and `break` map to `CONTINUE` and `EXIT`. `pass` is a no-op.

### Loop labels

```sql
outer: for i in range(n):
    for j in range(n):
        if i + j >= limit:
            break outer
```

## Working with data

### Iterating a query

```sql
for row in query(f"SELECT id, amount FROM orders WHERE grp = {g}"):
    total = total + row.amount
```

The row variable is a record. Field access is `row.col`. For bind parameters or
non-literal SQL, pass extra arguments: `query(sql, a)`.

### Iterating an array

```sql
v #:: int
for v in values:
    total = total + v
```

The loop variable must be annotated with its element type.

### Fetching one row

```sql
u = fetch_one(f"SELECT id, name FROM users WHERE id = {uid}")
return u.name
```

`fetch_one` returns all-NULL on no row. `fetch_one!` raises on zero or more than
one row.

### Running SQL

```sql
perform(f"UPDATE counters SET n = n + 1 WHERE id = {cid}")
execute("INSERT INTO t(msg) VALUES ($1)", note)
return_query("SELECT id FROM vip ORDER BY id")   # set-returning function
```

Use bind arguments in `execute` for untrusted input.

### Cursors

```sql
c = open_cursor("SELECT v FROM t ORDER BY v")
row = fetch_from(c)
while found():
    total = total + row.v
    row = fetch_from(c)
close_cursor(c)
```

`move_cursor(c)` and `move_cursor(c, n)` map to `MOVE`.

### Diagnostics

```sql
rc = row_count()   # GET DIAGNOSTICS rc = ROW_COUNT
found()            # the FOUND variable
```

### Set-returning functions

```sql
CREATE FUNCTION squares(n int) RETURNS SETOF int LANGUAGE plxpython3 AS $$
for i in range(1, n + 1):
    return_next(i * i)
return
$$;
```

## Errors

### Raising

```sql
raise ValueError(f"negative: {v}")
```

The call form `raise("notice", "message")` emits a `RAISE` at the named level
(`notice`, `warning`, `info`, `log`, `debug`, `exception`). A bare `raise` inside
an `except` handler re-raises.

### Handling

```sql
try:
    execute("INSERT INTO uniq(id) VALUES ($1)", k)
except PG::UniqueViolation as e:
    raise("notice", f"dup on {e.constraint}")
finally:
    perform("INSERT INTO log(msg) VALUES ('done')")
```

`except Exception as e` maps to `WHEN OTHERS`. To catch a specific condition,
name it with the `PG::` spelling (`except PG::UniqueViolation as e`), which maps
to `unique_violation`. Accessors: `e.message` to `SQLERRM`, `e.sqlstate` to
`SQLSTATE`, and `e.detail`, `e.hint`, `e.constraint`, `e.column`, `e.table`,
`e.schema`, `e.datatype` to the matching `GET STACKED DIAGNOSTICS` fields.

### Assertions

```sql
assert n > 0, "must be positive"
```

## Expressions

- Interpolation: f-strings, `f"total is {amount}"`. Plain strings do not
  interpolate. `{{` and `}}` are literal braces.
- Comparison: `==` and `!=` map to `=` and `<>`. `x is None` becomes `x IS NULL`
  and `x is not None` becomes `x IS NOT NULL`.
- Boolean: `and`, `or`, `not` map to `AND`, `OR`, `NOT`.
- Ternary: Python's `a if c else b` is not supported; use an explicit `if`.
- `None` becomes `NULL`.

## Trigger functions

A function returning `trigger` can be used as a trigger. Assign to `NEW` fields
and return `NEW`:

```sql
CREATE FUNCTION stamp() RETURNS trigger LANGUAGE plxpython3 AS $$
NEW.tag = f"row {NEW.id}"
return NEW
$$;
```

`NEW`, `OLD`, and the `TG_` variables are available. Assigning to a record field
(`NEW.col = e`) or an array element is supported.

## Semantic differences

These are intentional. plx pins semantics to SQL and plpgsql.

- Decimal literals infer `numeric`, not a floating-point type.
- Comparisons use SQL three-valued logic. `==` and `!=` are null-aware; a
  positive `if`/`while` condition treats NULL as false.
- Integer division follows SQL rules, not Python's `/` (float) and `//` (floor).
- Python truthiness is not emulated. A condition must be a boolean expression. A
  non-boolean condition is an error reported by plpgsql when the function runs.
- Locals are function-scoped, matching Python function scope.

## Not supported

Rejected at `CREATE FUNCTION` time with a line number:

- `def`, `class`, `lambda`, decorators, comprehensions, generators.
- `import`, modules, and the Python standard library.
- Tuples, lists, dicts, and sets as general values.
- `match`/`case`.
- The conditional expression `a if c else b`.
- Per-block local scope (locals are function-scoped by design).

See [PARITY.md](PARITY.md) for the full plpgsql construct matrix and
[ARCHITECTURE.md](ARCHITECTURE.md) for how plx maps to the plpgsql engine.
