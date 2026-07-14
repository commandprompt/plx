# plxruby: the Ruby dialect

plxruby lets you write PostgreSQL functions with Ruby syntax. At
`CREATE FUNCTION` time plx transpiles the body to plpgsql and stores the plpgsql
in `pg_proc.prosrc`. The function runs on the standard plpgsql interpreter.

The language name is `plxruby`, so it does not collide with the native PL/Ruby.

## Setup

```sql
CREATE EXTENSION plx;
```

## Function basics

The body is a sequence of statements. A scalar function ends with an explicit
`return`.

```sql
CREATE FUNCTION add(a int, b int) RETURNS int LANGUAGE plxruby AS $$
return a + b
$$;
```

Function arguments are referenced by name. Any SQL expression is valid in a
statement, because expressions are passed through to plpgsql and SQL unchanged.

### Local variables and types

Assignment creates a local. plpgsql requires every local to have a type, so plx
infers it from the first value when it is an integer, numeric, text, or boolean
literal. Otherwise annotate the variable with `#:: type`.

```sql
n = 42            -- inferred integer
label = "count"   -- inferred text
amount #:: numeric
amount = principal * rate
```

A first assignment of a constant literal at the top level is folded into the
declaration, so `n = 42` becomes `DECLARE n integer := 42;`.

The annotation text is emitted verbatim as the plpgsql type, so `%TYPE` and
`%ROWTYPE` work:

```sql
e #:: emp%ROWTYPE
s #:: emp.sal%TYPE
```

A constant uses the `const` suffix on the annotation:

```sql
pi = 3.14159 #:: numeric const
```

## Control flow

### if / elsif / else, unless

```sql
if score >= 90
  grade = "A"
elsif score >= 80
  grade = "B"
else
  grade = "F"
end
```

`unless c` is `if not c`. Modifier forms are supported: `return x unless n != 0`.

### case / when

With a subject it is a simple `CASE`; without one it is a searched `CASE`.

```sql
case n
when 1
  return "one"
when 2, 3
  return "few"
else
  return "many"
end
```

### Loops

Integer range, `while`, `until`, and `loop`:

```sql
for i in 1..n     # inclusive; use 1...n for exclusive
  total = total + i
end

while n > 0
  n = n - 1
end

loop do
  break if done
end
```

`next` and `break` map to `CONTINUE` and `EXIT`, and accept a condition modifier
(`next if i == 3`).

### Loop labels

```sql
outer: for i in 1..n
  for j in 1..n
    break outer if i + j >= limit
  end
end
```

## Working with data

### Iterating a query

```sql
query("SELECT id, amount FROM orders WHERE grp = #{g}").each do |row|
  total = total + row.amount
end
```

The row variable is a record. Field access is `row.col`, `row[:col]`, or
`row['col']`. Interpolated values in the SQL string are spliced as name
references. For bind parameters or non-literal SQL, pass extra arguments:
`query(sql, a, b).each`. `each_with_index do |row, i|` provides a zero-based
index.

### Iterating an array

```sql
v #:: int
values.each do |v|
  total = total + v
end
```

The loop variable must be annotated with its element type.

### Fetching one row

```sql
u = fetch_one("SELECT id, name FROM users WHERE id = #{uid}")
return u.name
```

`fetch_one` returns all-NULL on no row. `fetch_one!` raises on zero or more than
one row.

### Running SQL

```sql
perform("UPDATE counters SET n = n + 1 WHERE id = #{cid}")
execute("INSERT INTO t(msg) VALUES ($1)", note)
return_query("SELECT id FROM vip ORDER BY id")   -- in a set-returning function
```

`perform` runs a literal statement. `execute` runs dynamic SQL with optional bind
arguments. Use bind arguments for untrusted input.

### Cursors

```sql
c = open_cursor("SELECT v FROM t ORDER BY v")
row = fetch_from(c)
while found?
  total = total + row.v
  row = fetch_from(c)
end
close_cursor(c)
```

`move_cursor(c)` and `move_cursor(c, n)` map to `MOVE`.

### Diagnostics

```sql
rc = row_count()   -- GET DIAGNOSTICS rc = ROW_COUNT
found?             -- the FOUND variable
```

### Set-returning functions

```sql
CREATE FUNCTION squares(n int) RETURNS SETOF int LANGUAGE plxruby AS $$
for i in 1..n
  return_next i * i
end
return
$$;
```

`emit` is an alias for a bare `return_next`.

## Errors

### Raising

```sql
raise "bad value"                                -- EXCEPTION
raise notice: "processed #{n} rows"              -- NOTICE
raise exception: "negative: #{v}", errcode: "22023"
```

Levels are `notice`, `warning`, `info`, `log`, `debug`, `exception`. Options are
`errcode`, `detail`, `hint`, `column`, `constraint`, `message`. The call form
`raise("notice", "message")` is also accepted.

### Handling

```sql
begin
  return 100 / d
rescue PG::UniqueViolation => e
  raise notice: "dup: #{e.message} on #{e.constraint}"
  return -1
ensure
  perform("INSERT INTO log(msg) VALUES ('done')")
end
```

`rescue => e` (or bare `rescue`) is `WHEN OTHERS`. Exception classes map to
plpgsql conditions (`PG::UniqueViolation` to `unique_violation`, and similar).
Accessors: `e.message` to `SQLERRM`, `e.sqlstate` to `SQLSTATE`, and `e.detail`,
`e.hint`, `e.constraint`, `e.column`, `e.table`, `e.schema`, `e.datatype` to the
matching `GET STACKED DIAGNOSTICS` fields.

### Assertions

```sql
assert(n > 0, "must be positive")
```

## Expressions

- Interpolation: `"total is #{amount}"` becomes `'total is ' || (amount)::text`.
  Single-quoted strings do not interpolate.
- Comparison: `==` and `!=` map to `=` and `<>`. `x == nil` becomes `x IS NULL`.
- Boolean: `&&`, `||`, `!`, and `and`, `or`, `not` map to `AND`, `OR`, `NOT`.
- Ternary: `c ? a : b` becomes `CASE WHEN c THEN a ELSE b END`.
- Casts: `x.to_i`, `x.to_s`, `x.to_f` become `::integer`, `::text`,
  `::double precision`. `nil` becomes `NULL`.

## Trigger functions

A function returning `trigger` can be used as a trigger. Assign to `NEW` fields
and return `NEW` (or `OLD`, or `nil`):

```sql
CREATE FUNCTION stamp() RETURNS trigger LANGUAGE plxruby AS $$
NEW.tag = "row #{NEW.id}"
return NEW
$$;
```

`NEW`, `OLD`, and the `TG_` variables are available. Assigning to a record field
(`NEW.col = e`) or an array element (`arr[i] = e`) is supported.

## Semantic differences

These are intentional. plx pins semantics to SQL and plpgsql.

- Decimal literals infer `numeric`, not a floating-point type.
- Comparisons use SQL three-valued logic. `==` and `!=` are null-aware; a
  positive `if`/`while` condition treats NULL as false.
- `String#+` remains SQL numeric `+`. Use interpolation for concatenation.
- Locals are function-scoped, matching Ruby method scope.
- Ruby truthiness is not emulated. A condition must be a boolean expression;
  write `x != 0` or `!x.nil?` rather than a bare `x`. A non-boolean condition is
  an error reported by plpgsql when the function runs, not at CREATE time.

## Not supported

Rejected at `CREATE FUNCTION` time with a line number:

- Method or class definitions (`def`, classes, modules), gems.
- Blocks and lambdas beyond the recognized `.each` forms.
- Hash and array literals as general values.
- `||=`, `&&=`, and `and`/`or` in value position.
- `redo`, `retry`.
- Per-block local `DECLARE` (locals are function-scoped by design).

See [PARITY.md](PARITY.md) for the full plpgsql construct matrix and
[ARCHITECTURE.md](ARCHITECTURE.md) for how plx maps to the plpgsql engine.
