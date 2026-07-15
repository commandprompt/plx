# PHP cookbook

Practical recipes for the plxphp dialect. Every recipe here was run on PostgreSQL; plx transpiles the body to plpgsql and the standard interpreter executes it. See the [plxphp chapter](plxphp.md) for the full language reference.

## Scalar function with branching

Return a text value chosen by an `if` / `elseif` / `else` chain. The result is built in a local annotated `text` and returned at the end.

```php
CREATE FUNCTION grade(score int) RETURNS text LANGUAGE plxphp AS $$
$g = "" /*:: text */;
if ($score >= 90) { $g = "A"; }
elseif ($score >= 80) { $g = "B"; }
elseif ($score >= 70) { $g = "C"; }
else { $g = "F"; }
return $g;
$$;
```

```sql
SELECT grade(95) AS a, grade(83) AS b, grade(72) AS c, grade(50) AS d;
```

```
 a | b | c | d 
---+---+---+---
 A | B | C | F
```

## Accumulating loop

A counting `for` loop sums the integers from 1 to `n`. The accumulator is annotated `bigint` so it does not overflow `int`.

```php
CREATE FUNCTION sum_to(n int) RETURNS bigint LANGUAGE plxphp AS $$
$total = 0 /*:: bigint */;
for ($i = 1; $i <= $n; $i++) {
  $total = $total + $i;
}
return $total;
$$;
```

```sql
SELECT sum_to(100);
```

```
 sum_to 
--------
   5050
```

## Building a string in a loop

The append assignment `.=` lowers to the plx string builder (`plx_strbuild`), which is amortized O(1) per append on PostgreSQL 18 instead of the O(n^2) of repeated `||`. Interpolating `{$i}` makes each appended value text.

```php
CREATE FUNCTION csv_ints(n int) RETURNS text LANGUAGE plxphp AS $$
$s = "" /*:: text */;
for ($i = 1; $i <= $n; $i++) {
  if ($i > 1) { $s .= ","; }
  $s .= "{$i}";
}
return $s;
$$;
```

```sql
SELECT csv_ints(5);
```

```
 csv_ints  
-----------
 1,2,3,4,5
```

## Looping over a query result

`foreach (query("...") as $r)` iterates a query. Each row is a record; read a column with `$r->col`. Values spliced with `{$g}` are interpolated into the SQL text.

```php
CREATE TABLE orders(id int, grp int, amount int);
INSERT INTO orders VALUES (1,10,100),(2,10,250),(3,10,75),(4,20,999);

CREATE FUNCTION order_total(g int) RETURNS bigint LANGUAGE plxphp AS $$
$total = 0 /*:: bigint */;
foreach (query("SELECT amount FROM orders WHERE grp = {$g}") as $r) {
  $total = $total + $r->amount;
}
return $total;
$$;
```

```sql
SELECT order_total(10);
```

```
 order_total 
-------------
         425
```

## Set-returning function

A function that returns `SETOF int` emits rows with `return_next(...)` and ends with a bare `return`. Call it in the `FROM` clause.

```php
CREATE FUNCTION squares(n int) RETURNS SETOF int LANGUAGE plxphp AS $$
for ($i = 1; $i <= $n; $i++) {
  return_next($i * $i);
}
return;
$$;
```

```sql
SELECT * FROM squares(5);
```

```
 squares 
---------
       1
       4
       9
      16
      25
```

## Error handling with try / catch

`try` / `catch (\Exception $e)` maps to a plpgsql exception block with `WHEN OTHERS`. Accessors such as `$e->message` expose the standard diagnostics.

```php
CREATE FUNCTION safe_divide(a int, b int) RETURNS int LANGUAGE plxphp AS $$
try {
  return $a / $b;
} catch (\Exception $e) {
  raise('notice', "caught: " . $e->message);
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
```

## Trigger function

A `BEFORE INSERT` trigger fills in a derived column. Assign to a field with the arrow form `$NEW->col = ...` (or the array-element form `$NEW['col'] = ...`) and return `$NEW`. Read a field with `$NEW->col`, here inside an interpolated string.

```php
CREATE TABLE audit_item(id int, name text, tag text);

CREATE FUNCTION stamp() RETURNS trigger LANGUAGE plxphp AS $$
$NEW->tag = "row {$NEW->id}";
return $NEW;
$$;

CREATE TRIGGER t_stamp BEFORE INSERT ON audit_item
  FOR EACH ROW EXECUTE FUNCTION stamp();
```

```sql
INSERT INTO audit_item(id, name) VALUES (1, 'widget'), (2, 'gadget');
SELECT id, name, tag FROM audit_item ORDER BY id;
```

```
 id |  name  |  tag  
----+--------+-------
  1 | widget | row 1
  2 | gadget | row 2
```

## Dynamic SQL with bind parameters

Interpolation (`{$tbl}`) builds the SQL text, while the untrusted value is passed as a bind parameter to `execute`, so it is never spliced into the statement. `row_count()` reports how many rows the last statement affected.

```php
CREATE TABLE note(id serial, msg text);

CREATE FUNCTION add_note(tbl text, val text) RETURNS bigint LANGUAGE plxphp AS $$
execute("INSERT INTO {$tbl}(msg) VALUES ($1)", $val);
$n = row_count();
return $n;
$$;
```

```sql
SELECT add_note('note', 'hello') AS rows;
SELECT id, msg FROM note ORDER BY id;
```

```
 rows 
------
    1

 id |  msg  
----+-------
  1 | hello
```

## PHP idioms: interpolation, foreach, switch

`foreach ($vals as $v)` iterates an array (the loop variable is annotated with its element type), `switch` lowers to plpgsql `CASE`, and `"{$v}:{$label} "` interpolates both locals. Each `switch` arm ends in `break`.

```php
CREATE FUNCTION size_labels(vals int[]) RETURNS text LANGUAGE plxphp AS $$
$s = "" /*:: text */;
$v /*:: int */;
foreach ($vals as $v) {
  $label = "" /*:: text */;
  switch ($v) {
    case 0: $label = "zero"; break;
    case 1: $label = "one"; break;
    default: $label = "many"; break;
  }
  $s .= "{$v}:{$label} ";
}
return $s;
$$;
```

```sql
SELECT size_labels(ARRAY[0,1,5,1]);
```

```
        size_labels         
----------------------------
 0:zero 1:one 5:many 1:one 
```

## Fetching one row with a ternary fallback

`fetch_one` returns a record whose fields are all NULL when no row matches. A ternary supplies a default; `$who == null` lowers to `IS NULL`. The result local is declared with a `text` annotation because the ternary type cannot be inferred.

```php
CREATE TABLE users(id int, name text);
INSERT INTO users VALUES (1, 'Ada');

CREATE FUNCTION greet(uid int) RETURNS text LANGUAGE plxphp AS $$
$u = fetch_one("SELECT name FROM users WHERE id = {$uid}");
$who /*:: text */;
$who = $u->name == null ? "stranger" : $u->name;
return "Hello, {$who}!";
$$;
```

```sql
SELECT greet(1) AS known, greet(99) AS unknown;
```

```
    known    |     unknown      
-------------+------------------
 Hello, Ada! | Hello, stranger!
```
