# plxjs: the JavaScript dialect

plxjs lets you write PostgreSQL functions with JavaScript syntax (current
ECMAScript). At `CREATE FUNCTION` time plx transpiles the body to plpgsql and
stores the plpgsql in `pg_proc.prosrc`. The function runs on the standard plpgsql
interpreter.

The language name is `plxjs`.

## Setup

```sql
CREATE EXTENSION plx;
```

## Function basics

Blocks are brace-delimited and statements end with `;`. A scalar function ends
with an explicit `return`.

```sql
CREATE FUNCTION add(a int, b int) RETURNS int LANGUAGE plxjs AS $$
return a + b;
$$;
```

Function arguments are referenced by name. Any SQL expression is valid, because
expressions are passed through to plpgsql and SQL unchanged.

### Local variables and types

Declare locals with `let`, `const`, or `var`; the keyword is removed in the
generated plpgsql. plx infers the type from the first value when it is an
integer, numeric, text, or boolean literal. Otherwise annotate the variable with
a `/*:: type */` comment before the `;`.

```sql
let n = 42;             // inferred integer
let label = "count";    // inferred text
let amount /*:: numeric */;
amount = principal * rate;
```

The annotation text is emitted verbatim, so `%TYPE` and `%ROWTYPE` work:

```sql
let e /*:: emp%ROWTYPE */;
let s /*:: emp.sal%TYPE */;
```

A constant uses the `const` suffix in the annotation:

```sql
let pi = 3.14159 /*:: numeric const */;
```

## Control flow

### if / else if / else

```sql
if (score >= 90) { grade = "A"; }
else if (score >= 80) { grade = "B"; }
else { grade = "F"; }
```

### switch

`switch` lowers to plpgsql `CASE`. Stacked labels become a value list. Each arm
must end with `break` or a terminating statement (`return`/`throw`); genuine
fall-through is rejected.

```sql
switch (n) {
  case 1: return "one";
  case 2: case 3: return "few";
  default: return "many";
}
```

### Loops

Counting `for` and `while`:

```sql
for (let i = 1; i <= n; i++) {
  total = total + i;
}

while (n > 0) { n = n - 1; }
```

`continue` and `break` map to `CONTINUE` and `EXIT`.

### Loop labels

```sql
outer: for (let i = 1; i <= n; i++) {
  for (let j = 1; j <= n; j++) {
    if (i + j >= limit) { break outer; }
  }
}
```

## Working with data

### Iterating a query

```sql
for (const row of query(`SELECT id, amount FROM orders WHERE grp = ${g}`)) {
  total = total + row.amount;
}
```

The row variable is a record. Field access is `row.col`. For bind parameters or
non-literal SQL, pass extra arguments: `query(sql, a) of ...`.

### Iterating an array

```sql
let v /*:: int */;
for (const v of values) {
  total = total + v;
}
```

The loop variable must be annotated with its element type.

### Fetching one row

```sql
let u = fetch_one(`SELECT id, name FROM users WHERE id = ${uid}`);
return u.name;
```

`fetch_one` returns all-NULL on no row. `fetch_one!` raises on zero or more than
one row.

### Running SQL

```sql
perform(`UPDATE counters SET n = n + 1 WHERE id = ${cid}`);
execute("INSERT INTO t(msg) VALUES ($1)", note);
return_query(`SELECT id FROM vip ORDER BY id`);   // set-returning function
```

Use bind arguments in `execute` for untrusted input.

### Cursors

```sql
let c = open_cursor(`SELECT v FROM t ORDER BY v`);
let row = fetch_from(c);
while (found()) {
  total = total + row.v;
  row = fetch_from(c);
}
close_cursor(c);
```

`move_cursor(c)` and `move_cursor(c, n)` map to `MOVE`.

### Diagnostics

```sql
let rc = row_count();   // GET DIAGNOSTICS rc = ROW_COUNT
found()                 // the FOUND variable
```

### Set-returning functions

```sql
CREATE FUNCTION squares(n int) RETURNS SETOF int LANGUAGE plxjs AS $$
for (let i = 1; i <= n; i++) { return_next(i * i); }
return;
$$;
```

## Errors

### Throwing

```sql
throw new Error(`negative: ${v}`);
```

The call form `raise("notice", "message")` emits a `RAISE` at the named level
(`notice`, `warning`, `info`, `log`, `debug`, `exception`).

### Handling

```sql
try {
  return 100 / d;
} catch (e) {
  raise("notice", `caught: ${e.message}`);
  return -1;
} finally {
  perform("INSERT INTO log(msg) VALUES ('done')");
}
```

`catch (e)` maps to `WHEN OTHERS`. Accessors: `e.message` to `SQLERRM`,
`e.sqlstate` to `SQLSTATE`, and `e.detail`, `e.hint`, `e.constraint`, `e.column`,
`e.table`, `e.schema`, `e.datatype` to the matching `GET STACKED DIAGNOSTICS`
fields.

### Assertions

```sql
assert(n > 0, "must be positive");
```

## Expressions

- Interpolation: template literals, `` `total is ${amount}` ``. Plain `"..."` and
  `'...'` strings do not interpolate. Use template literals for concatenation.
- Comparison: `==`, `===`, `!=`, `!==` map to `=` and `<>`. `x === null` becomes
  `x IS NULL`.
- Boolean: `&&`, `||`, `!` map to `AND`, `OR`, `NOT`.
- Ternary: `c ? a : b` becomes `CASE WHEN c THEN a ELSE b END`.
- `null` and `undefined` become `NULL`.

## Trigger functions

A function returning `trigger` can be used as a trigger. Assign to `NEW` fields
and return `NEW`:

```sql
CREATE FUNCTION stamp() RETURNS trigger LANGUAGE plxjs AS $$
NEW.tag = `row ${NEW.id}`;
return NEW;
$$;
```

`NEW`, `OLD`, and the `TG_` variables are available. Assigning to a record field
(`NEW.col = e`) or an array element (`arr[i] = e`) is supported.

## Semantic differences

These are intentional. plx pins semantics to SQL and plpgsql.

- Decimal literals infer `numeric`, not a floating-point type. Arithmetic is
  exact, not IEEE 754.
- Comparisons use SQL three-valued logic. `==` and `===` are null-aware; a
  positive `if`/`while` condition treats NULL as false.
- `+` is numeric addition; use template literals for string building.
- JavaScript truthiness is not emulated. A condition must be a boolean
  expression. A non-boolean condition is an error reported by plpgsql when the
  function runs.
- Locals are function-scoped, matching JavaScript `var` scope.

## Not supported

Rejected at `CREATE FUNCTION` time with a line number:

- Function definitions, arrow functions, classes, `import`.
- Object and array literals as general values.
- `for...in`, and `for...of` over anything other than `query(...)` or an array.
- `switch` fall-through (end each case with `break` or `return`).
- Per-block local `DECLARE` (locals are function-scoped by design).

See [PARITY.md](PARITY.md) for the full plpgsql construct matrix and
[ARCHITECTURE.md](ARCHITECTURE.md) for how plx maps to the plpgsql engine.
