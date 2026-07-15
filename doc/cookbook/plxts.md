# TypeScript cookbook

Practical recipes for the plxts dialect (plxjs plus type annotations). Every recipe here was run on PostgreSQL; plx transpiles the body to plpgsql and the standard interpreter executes it. See the [plxts chapter](plxts.md) for the full language reference.

## Scalar function with branching

Typed locals carry their type after the name. Here `label` is a `string` (text) and `kelvin` a `number` (numeric). Each `if` branch assigns the result, and a template literal builds the return value.

```typescript
CREATE FUNCTION ck_water_state(celsius numeric) RETURNS text LANGUAGE plxts AS $$
let label: string;
let kelvin: number = celsius + 273.15;
if (celsius <= 0) { label = "solid"; }
else if (celsius < 100) { label = "liquid"; }
else { label = "gas"; }
return `${label} at ${kelvin} K`;
$$;
```

The argument type is a SQL type (`numeric`). The `number`/`string` annotations apply to `let` locals, not to the function signature.

```sql
SELECT ck_water_state(-5), ck_water_state(20), ck_water_state(150);
```

```
  ck_water_state   |   ck_water_state   | ck_water_state  
-------------------+--------------------+-----------------
 solid at 268.15 K | liquid at 293.15 K | gas at 423.15 K
```

## Accumulating loop

An integer `for` loop sums the range. The accumulator is annotated `bigint` so the total does not overflow `int`. The loop counter `i` needs no annotation; a counting `for` counter is always an integer.

```typescript
CREATE FUNCTION ck_sum_to(n int) RETURNS bigint LANGUAGE plxts AS $$
let total: bigint = 0;
for (let i = 1; i <= n; i++) {
  total = total + i;
}
return total;
$$;
```

```sql
SELECT ck_sum_to(100), ck_sum_to(1000000);
```

```
 ck_sum_to |  ck_sum_to   
-----------+--------------
      5050 | 500000500000
```

## Building a string in a loop

On a `string` local, `+=` lowers to the plx string builder (`plx_strbuild`), so repeated appends are amortized O(1) on PostgreSQL 18 rather than the O(n^2) of `s = s || x`. Interpolate the number with a template literal so the appended value is text.

```typescript
CREATE FUNCTION ck_csv_upto(n int) RETURNS text LANGUAGE plxts AS $$
let s: string = "";
for (let i = 1; i <= n; i++) {
  if (i > 1) { s += ","; }
  s += `${i}`;
}
return s;
$$;
```

```sql
SELECT ck_csv_upto(5);
```

```
 ck_csv_upto 
-------------
 1,2,3,4,5
```

## Looping over a query

`for (const r of query(...))` iterates the rows of a query. Values interpolated into the template literal are spliced as name references, and fields are read with `r.col`.

```typescript
CREATE FUNCTION ck_order_total(g int) RETURNS bigint LANGUAGE plxts AS $$
let total: bigint = 0;
for (const r of query(`SELECT amount FROM ck_ts_orders WHERE grp = ${g}`)) {
  total = total + r.amount;
}
return total;
$$;
```

```sql
CREATE TABLE ck_ts_orders(id int, amount int, grp int);
INSERT INTO ck_ts_orders VALUES (1, 100, 7), (2, 250, 7), (3, 999, 8);
SELECT ck_order_total(7);
```

```
 ck_order_total 
----------------
            350
```

## A set-returning function

`RETURNS SETOF int` with `return_next` emits one row per value. A bare `return` ends the function.

```typescript
CREATE FUNCTION ck_squares(n int) RETURNS SETOF int LANGUAGE plxts AS $$
for (let i = 1; i <= n; i++) {
  return_next(i * i);
}
return;
$$;
```

```sql
SELECT * FROM ck_squares(5);
```

```
 ck_squares 
------------
          1
          4
          9
         16
         25
```

## Error handling with try / catch

`try` / `catch` maps to a plpgsql exception block; `catch (e)` catches any error and `e.message` reads `SQLERRM`. Here a divide by zero is caught and a fallback value is returned.

```typescript
CREATE FUNCTION ck_safe_divide(a int, b int) RETURNS int LANGUAGE plxts AS $$
try {
  return a / b;
} catch (e) {
  raise("notice", `caught: ${e.message}`);
  return -1;
}
$$;
```

```sql
SELECT ck_safe_divide(10, 2), ck_safe_divide(10, 0);
```

```
NOTICE:  caught: division by zero
 ck_safe_divide | ck_safe_divide 
----------------+----------------
              5 |             -1
```

## A trigger function

A function returning `trigger` can assign to `NEW` fields and return `NEW`. A typed local holds the computed length before it is stored on the row.

```typescript
CREATE FUNCTION ck_stamp_len() RETURNS trigger LANGUAGE plxts AS $$
let n: number = length(NEW.name);
NEW.name_len = n;
return NEW;
$$;
```

```sql
CREATE TABLE ck_ts_people(id int primary key, name text, name_len int);
CREATE TRIGGER ck_ts_people_bi BEFORE INSERT OR UPDATE ON ck_ts_people
  FOR EACH ROW EXECUTE FUNCTION ck_stamp_len();
INSERT INTO ck_ts_people(id, name) VALUES (1, 'Ada');
UPDATE ck_ts_people SET name = 'Grace Hopper' WHERE id = 1;
SELECT * FROM ck_ts_people;
```

```
 id |     name     | name_len 
----+--------------+----------
  1 | Grace Hopper |       12
```

## Dynamic SQL with bind parameters

`execute(sql, ...args)` runs a statement with the extra arguments sent as bind parameters (`$1`, `$2`, ...) rather than interpolated. This is the safe form for untrusted input. `row_count()` reports the number of rows affected by the last statement.

```typescript
CREATE FUNCTION ck_add_note(msg text) RETURNS bigint LANGUAGE plxts AS $$
execute("INSERT INTO ck_ts_notes(msg) VALUES ($1)", msg);
let n: bigint = row_count();
return n;
$$;
```

```sql
CREATE TABLE ck_ts_notes(id serial, msg text);
SELECT ck_add_note('hello'), ck_add_note('world');
SELECT * FROM ck_ts_notes ORDER BY id;
```

```
 ck_add_note | ck_add_note 
-------------+-------------
           1 |           1

 id |  msg  
----+-------
  1 | hello
  2 | world
```

## Type-annotation showcase

The primitive mappings are `number` to `numeric`, `string` to `text`, `boolean`, and `bigint`. `T[]` becomes a SQL array of the mapped element type, and any name that is not a known TypeScript primitive (here `numeric(10,2)` and `date`) is emitted verbatim as a SQL type. Array elements are read with `arr[i]` using SQL's 1-based indexing.

```typescript
CREATE FUNCTION ck_type_demo(n int) RETURNS text LANGUAGE plxts AS $$
let amount: number = 19.99;
let label: string = "widget";
let active: boolean = true;
let big: bigint = 9000000000;
let nums: number[] = ARRAY[10, 20, 30];
let price: numeric(10,2) = amount * n;
let when_paid: date = DATE '2026-07-15';
return `${label} x${n} = ${price} (active=${active}, big=${big}, mid=${nums[2]}, on ${when_paid})`;
$$;
```

```sql
SELECT ck_type_demo(3);
```

```
                              ck_type_demo                              
------------------------------------------------------------------------
 widget x3 = 59.97 (active=true, big=9000000000, mid=20, on 2026-07-15)
```

## Fetching one row

`fetch_one` returns a single record; its fields are read with `p.col`. On no matching row every field is NULL, so a `p.col === null` test (which lowers to `IS NULL`) detects the miss.

```typescript
CREATE FUNCTION ck_price_label(pid int) RETURNS text LANGUAGE plxts AS $$
let p = fetch_one(`SELECT name, price FROM ck_ts_products WHERE id = ${pid}`);
if (p.name === null) { return "unknown"; }
return `${p.name} costs ${p.price}`;
$$;
```

```sql
CREATE TABLE ck_ts_products(id int, name text, price numeric(10,2));
INSERT INTO ck_ts_products VALUES (1, 'widget', 19.99), (2, 'gadget', 149.00);
SELECT ck_price_label(1), ck_price_label(2), ck_price_label(99);
```

```
   ck_price_label   |   ck_price_label    | ck_price_label 
--------------------+---------------------+----------------
 widget costs 19.99 | gadget costs 149.00 | unknown
```
