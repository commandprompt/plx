# JavaScript cookbook

Practical recipes for the plxjs dialect. Every recipe here was run on PostgreSQL; plx transpiles the body to plpgsql and the standard interpreter executes it. See the [plxjs chapter](plxjs.md) for the full language reference.

## Scalar function with branching

Assign to a result variable through an `if` / `else if` / `else` chain and return it. The variable is annotated with its type because it has no initializing literal.

```javascript
CREATE FUNCTION grade(score int) RETURNS text LANGUAGE plxjs AS $$
let g /*:: text */;
if (score >= 90) { g = "A"; }
else if (score >= 80) { g = "B"; }
else if (score >= 70) { g = "C"; }
else { g = "F"; }
return g;
$$;
```

```sql
SELECT grade(95) AS a, grade(85) AS b, grade(72) AS c, grade(40) AS f;
```

```
 a | b | c | f 
---+---+---+---
 A | B | C | F
(1 row)
```

## Accumulating loop

A C-style `for` loop accumulates into a variable. The accumulator is typed `bigint` from its literal initializer plus the annotation.

```javascript
CREATE FUNCTION factorial(n int) RETURNS bigint LANGUAGE plxjs AS $$
let result = 1 /*:: bigint */;
for (let i = 2; i <= n; i++) {
  result = result * i;
}
return result;
$$;
```

```sql
SELECT factorial(10);
```

```
 factorial 
-----------
   3628800
(1 row)
```

## Building a string in a loop

Use `+=` on a string variable. plx lowers this to its string builder, which is amortized O(1) per append on PostgreSQL 18 instead of the O(n^2) copy that `s = s || x` would produce.

```javascript
CREATE FUNCTION csv_repeat(word text, n int) RETURNS text LANGUAGE plxjs AS $$
let s = "" /*:: text */;
for (let i = 1; i <= n; i++) {
  s += word;
  if (i < n) { s += ","; }
}
return s;
$$;
```

```sql
SELECT csv_repeat('ab', 4);
```

```
 csv_repeat  
-------------
 ab,ab,ab,ab
(1 row)
```

## Looping over a query

`for (const r of query(...))` iterates a result set. The row is a record and columns are read as `r.col`. Use a template literal to interpolate the argument into the SQL text.

```javascript
CREATE FUNCTION order_total(g int) RETURNS bigint LANGUAGE plxjs AS $$
let total = 0 /*:: bigint */;
for (const r of query(`SELECT amount FROM orders WHERE grp = ${g}`)) {
  total = total + r.amount;
}
return total;
$$;
```

Setup and call:

```sql
CREATE TABLE orders (id int, grp int, amount bigint);
INSERT INTO orders VALUES (1,1,100),(2,1,250),(3,1,75),(4,2,500);
SELECT order_total(1);
```

```
 order_total 
-------------
         425
(1 row)
```

## Set-returning function

A function declared `RETURNS TABLE(...)` streams rows with `return_query`. Qualify the column names with the table name so they are not read as the output parameters of the same name.

```javascript
CREATE FUNCTION order_report(g int) RETURNS TABLE(id int, amount bigint) LANGUAGE plxjs AS $$
return_query(`SELECT orders.id, orders.amount FROM orders WHERE grp = ${g} ORDER BY orders.amount DESC`);
return;
$$;
```

```sql
SELECT * FROM order_report(1);
```

```
 id | amount 
----+--------
  2 |    250
  1 |    100
  3 |     75
(3 rows)
```

To emit computed rows one at a time instead, use `return_next` with `RETURNS SETOF`:

```javascript
CREATE FUNCTION evens(lo int, hi int) RETURNS SETOF int LANGUAGE plxjs AS $$
for (let i = lo; i <= hi; i++) {
  if (i % 2 == 0) { return_next(i); }
}
return;
$$;
```

```sql
SELECT array_agg(x) FROM evens(1, 10) AS x;
```

```
  array_agg   
--------------
 {2,4,6,8,10}
(1 row)
```

## Error handling with try / catch

`try` / `catch (e)` maps to a plpgsql exception block (`WHEN OTHERS`). `e.message` reads `SQLERRM`. The `raise` call form emits a `RAISE` at the named level.

```javascript
CREATE FUNCTION safe_divide(a int, b int) RETURNS int LANGUAGE plxjs AS $$
try {
  return a / b;
} catch (e) {
  raise("notice", `caught: ${e.message}`);
  return -1;
}
$$;
```

```sql
SELECT safe_divide(10, 2) AS ok, safe_divide(10, 0) AS zero;
```

```
NOTICE:  caught: division by zero
 ok | zero 
----+------
  5 |   -1
(1 row)
```

## Trigger function

A function returning `trigger` can be attached to a table. Assign to `NEW` fields and return `NEW`. This one normalizes a code and derives a label before the row is written, on both insert and update.

```javascript
CREATE FUNCTION normalize_product() RETURNS trigger LANGUAGE plxjs AS $$
NEW.code = upper(NEW.code);
NEW.label = `code:${NEW.code}`;
return NEW;
$$;
```

```sql
CREATE TABLE products (id int, code text, label text);
CREATE TRIGGER trg_norm BEFORE INSERT OR UPDATE ON products
  FOR EACH ROW EXECUTE FUNCTION normalize_product();
INSERT INTO products(id, code) VALUES (1, 'abc'), (2, 'xy');
SELECT id, code, label FROM products ORDER BY id;
```

```
 id | code |  label   
----+------+----------
  1 | ABC  | code:ABC
  2 | XY   | code:XY
(2 rows)
```

## Dynamic SQL with bind parameters

Use `execute(sql, arg, ...)` and reference the arguments as `$1`, `$2`, and so on. Bind parameters keep untrusted values out of the SQL text, which avoids injection. `row_count()` reads how many rows the statement affected.

```javascript
CREATE FUNCTION log_event(msg text) RETURNS bigint LANGUAGE plxjs AS $$
execute("INSERT INTO events(note) VALUES ($1)", msg);
let n = row_count() /*:: bigint */;
return n;
$$;
```

```sql
CREATE TABLE events (id serial PRIMARY KEY, note text);
SELECT log_event('deploy started');
SELECT note FROM events ORDER BY id;
```

```
 log_event 
-----------
         1
(1 row)

      note      
----------------
 deploy started
(1 row)
```

## JS idioms: template literals, for...of, and switch

This function combines the common JavaScript idioms plx supports: a `for...of` loop over a `query`, a `switch` that lowers to `CASE`, and a template literal that interpolates several values into the return string.

```javascript
CREATE FUNCTION describe_grp(g int) RETURNS text LANGUAGE plxjs AS $$
let count = 0 /*:: int */;
let total = 0 /*:: bigint */;
for (const r of query(`SELECT amount FROM orders WHERE grp = ${g}`)) {
  count = count + 1;
  total = total + r.amount;
}
let size /*:: text */;
switch (count) {
  case 0: size = "empty"; break;
  case 1: size = "single"; break;
  default: size = "many"; break;
}
return `group ${g}: ${count} orders (${size}), total ${total}`;
$$;
```

```sql
SELECT describe_grp(1) AS g1, describe_grp(2) AS g2;
```

```
                 g1                  |                  g2                   
-------------------------------------+---------------------------------------
 group 1: 3 orders (many), total 425 | group 2: 1 orders (single), total 500
(1 row)
```

## Single-row lookup with a ternary

`fetch_one` returns one row, or an all-NULL record when nothing matches. A ternary (`c ? a : b`, which lowers to `CASE`) plus a `=== null` test turns the miss into a readable message. `x === null` becomes `IS NULL`.

```javascript
CREATE FUNCTION user_greeting(uid int) RETURNS text LANGUAGE plxjs AS $$
let u = fetch_one(`SELECT name FROM users WHERE id = ${uid}`);
return u.name === null ? `no user #${uid}` : `hello, ${u.name}`;
$$;
```

```sql
CREATE TABLE users (id int, name text);
INSERT INTO users VALUES (1, 'Ada'), (2, 'Grace');
SELECT user_greeting(1) AS found, user_greeting(99) AS missing;
```

```
   found    |   missing   
------------+-------------
 hello, Ada | no user #99
(1 row)
```
