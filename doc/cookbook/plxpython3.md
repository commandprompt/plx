# Python cookbook

Practical recipes for the plxpython3 dialect. Every recipe here was run on PostgreSQL; plx transpiles the body to plpgsql and the standard interpreter executes it. See the [plxpython3 chapter](plxpython3.md) for the full language reference.

## Scalar function with branching

An `if` / `elif` / `else` chain selects a value. Locals are declared by
annotating them with `#:: type` and assigned with `=`.

```python
CREATE FUNCTION letter_grade(score int) RETURNS text LANGUAGE plxpython3 AS $$
result #:: text
if score >= 90:
    result = "A"
elif score >= 80:
    result = "B"
elif score >= 70:
    result = "C"
else:
    result = "F"
return result
$$;
```

```sql
SELECT letter_grade(95) AS a, letter_grade(83) AS b, letter_grade(72) AS c, letter_grade(50) AS f;
```

```
 a | b | c | f
---+---+---+---
 A | B | C | F
(1 row)
```

## Accumulating loop with range

`for i in range(a, b)` counts from `a` to `b - 1`. Give the accumulator an
explicit type so the arithmetic does not overflow `int`.

```python
CREATE FUNCTION factorial(n int) RETURNS bigint LANGUAGE plxpython3 AS $$
result = 1 #:: bigint
for i in range(1, n + 1):
    result = result * i
return result
$$;
```

```sql
SELECT factorial(5) AS f5, factorial(10) AS f10;
```

```
 f5  |   f10
-----+---------
 120 | 3628800
(1 row)
```

## Building a string in a loop

Use `+=` on a text variable. plx lowers this to its string builder, so each
append is amortized O(1) on PostgreSQL 18 instead of the O(n^2) copy that
`s = s || x` would cost. The `f"{i}"` converts the integer to text before it is
appended.

```python
CREATE FUNCTION int_csv(n int) RETURNS text LANGUAGE plxpython3 AS $$
s = "" #:: text
for i in range(1, n + 1):
    if i > 1:
        s += ", "
    s += f"{i}"
return s
$$;
```

```sql
SELECT int_csv(5) AS csv;
```

```
      csv
---------------
 1, 2, 3, 4, 5
(1 row)
```

## Looping over a query

`for row in query(...)` iterates the rows of a result set. Each `row` is a
record; access columns with `row.col`. The f-string interpolates the argument
`g` into the SQL text.

```python
CREATE FUNCTION order_total(g int) RETURNS bigint LANGUAGE plxpython3 AS $$
total = 0 #:: bigint
for row in query(f"SELECT amount FROM orders WHERE grp = {g}"):
    total = total + row.amount
return total
$$;
```

```sql
CREATE TABLE orders(id int, grp int, amount int);
INSERT INTO orders VALUES (1,1,100),(2,1,250),(3,2,40),(4,1,10);
SELECT order_total(1) AS grp1, order_total(2) AS grp2;
```

```
 grp1 | grp2
------+------
  360 |   40
(1 row)
```

## Set-returning function

A function returning `SETOF` emits rows with `return_next(...)`. A bare `return`
ends the function.

```python
CREATE FUNCTION divisors(n int) RETURNS SETOF int LANGUAGE plxpython3 AS $$
for i in range(1, n + 1):
    if n % i == 0:
        return_next(i)
return
$$;
```

```sql
SELECT array_agg(d) AS divisors_of_12 FROM divisors(12) d;
```

```
 divisors_of_12
----------------
 {1,2,3,4,6,12}
(1 row)
```

## Error handling with try / except

`try` / `except` maps to a plpgsql exception block. Name a specific condition
with the `PG::` spelling; `PG::DivisionByZero` catches `division_by_zero`. The
`e.message` accessor reads `SQLERRM`.

```python
CREATE FUNCTION safe_divide(a int, b int) RETURNS int LANGUAGE plxpython3 AS $$
try:
    return a / b
except PG::DivisionByZero as e:
    raise("notice", f"caught: {e.message}")
    return -1
$$;
```

```sql
SELECT safe_divide(7, 2) AS ok, safe_divide(1, 0) AS zero;
```

```
NOTICE:  caught: division by zero
 ok | zero
----+------
  3 |   -1
(1 row)
```

## Trigger function

A function returning `trigger` can back a trigger. Assign to `NEW` fields and
return `NEW`. Here a `BEFORE INSERT` trigger fills in a default label and
normalizes the code to upper case.

```python
CREATE FUNCTION stamp_widget() RETURNS trigger LANGUAGE plxpython3 AS $$
if NEW.label is None:
    NEW.label = f"row {NEW.id}"
NEW.code = upper(NEW.code)
return NEW
$$;
```

```sql
CREATE TABLE widget(id int primary key, label text, code text);
CREATE TRIGGER widget_stamp BEFORE INSERT ON widget
    FOR EACH ROW EXECUTE FUNCTION stamp_widget();
INSERT INTO widget(id, label, code) VALUES (1, NULL, 'ab'), (2, 'named', 'cd');
SELECT id, label, code FROM widget ORDER BY id;
```

```
 id | label | code
----+-------+------
  1 | row 1 | AB
  2 | named | CD
(2 rows)
```

## Dynamic SQL with bind parameters

Interpolate identifiers you control (a table name) with an f-string, but pass
untrusted values as bind parameters: `$1` in the SQL text and an extra argument
to `fetch_one`. This keeps the value out of the SQL string and avoids injection.

```python
CREATE FUNCTION count_at_least(tbl text, threshold int) RETURNS bigint LANGUAGE plxpython3 AS $$
r = fetch_one(f"SELECT count(*) AS c FROM {tbl} WHERE amount >= $1", threshold)
return r.c
$$;
```

```sql
SELECT count_at_least('orders', 100) AS big_orders;
```

```
 big_orders
------------
          2
(1 row)
```

## Python idioms: f-strings, range, while

f-strings interpolate expressions with `f"{expr}"`, `for i in range(a, b)` is the
integer loop, and `while` runs until its condition is false. This function lists
the integers up to `n` and then uses a `while` loop to find the largest power of
two that does not exceed `n`.

```python
CREATE FUNCTION number_report(n int) RETURNS text LANGUAGE plxpython3 AS $$
s = "" #:: text
for i in range(2, n + 1):
    s += f"{i} "
p = 1 #:: int
while p * 2 <= n:
    p = p * 2
s += f"| largest power of two <= {n} is {p}"
return s
$$;
```

```sql
SELECT number_report(10) AS report;
```

```
                        report
------------------------------------------------------
 2 3 4 5 6 7 8 9 10 | largest power of two <= 10 is 8
(1 row)
```

## Iterating an array

`for v in values` iterates an array argument. The loop variable is annotated
with the element type. The annotation `v #:: int` comes before the loop.

```python
CREATE FUNCTION array_sum(vals int[]) RETURNS bigint LANGUAGE plxpython3 AS $$
total = 0 #:: bigint
v #:: int
for v in vals:
    total = total + v
return total
$$;
```

```sql
SELECT array_sum(ARRAY[3, 1, 4, 1, 5, 9]) AS total;
```

```
 total
-------
    23
(1 row)
```
