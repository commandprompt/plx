# Lua cookbook

Practical recipes for the plxlua dialect. Every recipe here was run on PostgreSQL; plx transpiles the body to plpgsql and the standard interpreter executes it. See the [plxlua chapter](../plxlua.md) for the full language reference.

## Scalar function with branching

Return a value from an `if` / `elseif` / `else` chain. Each branch returns directly, so no result variable is needed.

```lua
CREATE FUNCTION letter_grade(score int) RETURNS text LANGUAGE plxlua AS $$
if score >= 90 then
  return "A"
elseif score >= 80 then
  return "B"
elseif score >= 70 then
  return "C"
elseif score >= 60 then
  return "D"
else
  return "F"
end
$$;
```

```sql
SELECT letter_grade(95), letter_grade(83), letter_grade(50);
```

```
 letter_grade | letter_grade | letter_grade 
--------------+--------------+--------------
 A            | B            | F
```

## Accumulating loop

A numeric `for` loop. The accumulator is annotated `bigint` with a `--::` comment so the product does not overflow `int`.

```lua
CREATE FUNCTION factorial(n int) RETURNS bigint LANGUAGE plxlua AS $$
local result = 1 --:: bigint
for i = 1, n do
  result = result * i
end
return result
$$;
```

```sql
SELECT factorial(5), factorial(10);
```

```
 factorial | factorial 
-----------+-----------
       120 |   3628800
```

## Building a string in a loop

`s = s .. x` on a string local is lowered to plx's string builder, so repeated appends are amortized O(1) on PostgreSQL 18 rather than the O(n^2) of `s := s || x`.

```lua
CREATE FUNCTION repeat_stars(n int) RETURNS text LANGUAGE plxlua AS $$
local s = ""
for i = 1, n do
  s = s .. "*"
end
return s
$$;
```

```sql
SELECT repeat_stars(5);
```

```
 repeat_stars 
--------------
 *****
```

## Looping over a query result

`for r in query(...) do` iterates the rows. Lua has no string interpolation, so the group is passed as a bind argument (`$1`), and fields are read with `r.col`.

```lua
CREATE FUNCTION order_total(g int) RETURNS bigint LANGUAGE plxlua AS $$
local total = 0 --:: bigint
for r in query("SELECT amount FROM ck_orders WHERE grp = $1", g) do
  total = total + r.amount
end
return total
$$;
```

```sql
CREATE TABLE ck_orders(id int, amount int, grp int);
INSERT INTO ck_orders VALUES (1, 100, 7), (2, 250, 7), (3, 999, 8);
SELECT order_total(7);
```

```
 order_total 
-------------
         350
```

## A set-returning function

`RETURNS SETOF int` with `emit(v)` produces one row per value. A bare `return` ends the function.

```lua
CREATE FUNCTION squares(n int) RETURNS SETOF int LANGUAGE plxlua AS $$
for i = 1, n do
  emit(i * i)
end
return
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

## Rows with several columns

In a `RETURNS TABLE` function, `emit(a, b)` assigns the output columns in order and produces the row.

```lua
CREATE FUNCTION countdown(n int) RETURNS TABLE(step int, label text) LANGUAGE plxlua AS $$
for i = n, 1, -1 do
  emit(i, i == 1 and "liftoff" or "t-minus " .. i)
end
$$;
```

```sql
SELECT * FROM countdown(3);
```

```
 step |   label   
------+-----------
    3 | t-minus 3
    2 | t-minus 2
    1 | liftoff
```

## Trapping an error

`pcall(function() ... end)` maps to a plpgsql exception block. `ok` is false when the protected function raised, and `err` holds the error message.

```lua
CREATE FUNCTION safe_divide(a int, b int) RETURNS int LANGUAGE plxlua AS $$
local result --:: int
local ok, err = pcall(function()
  result = a // b
end)
if not ok then
  print("caught: " .. err)
  return -1
end
return result
$$;
```

```sql
SELECT safe_divide(10, 2), safe_divide(10, 0);
```

```
NOTICE:  caught: division by zero
 safe_divide | safe_divide 
-------------+-------------
           5 |          -1
```

## Handling one error condition

Lua has no exception classes. Test `err.sqlstate` to handle one condition, and raise anything else again with `error(err)`, which keeps its SQLSTATE, message, detail and hint.

```lua
CREATE FUNCTION add_tag(t text) RETURNS text LANGUAGE plxlua AS $$
local ok, err = pcall(function()
  execute("INSERT INTO ck_tags VALUES ($1)", t)
end)
if ok then return "added " .. t end
if err.sqlstate == "23505" then
  return "already have " .. t .. " (" .. err.constraint .. ")"
end
error(err)
$$;
```

```sql
CREATE TABLE ck_tags(tag text PRIMARY KEY);
SELECT add_tag('red'), add_tag('red');
```

```
  add_tag  |             add_tag             
-----------+---------------------------------
 added red | already have red (ck_tags_pkey)
```

## Raising an error with a SQLSTATE

`error{...}` sets the message, SQLSTATE, detail and hint of the error, where `error(msg)` sets only the message.

```lua
CREATE FUNCTION check_qty(q int) RETURNS int LANGUAGE plxlua AS $$
if q < 0 then
  error{message = "quantity cannot be negative", sqlstate = "22023",
        detail = "got " .. q, hint = "pass zero or more"}
end
return q
$$;
```

```sql
SELECT check_qty(-3);
```

```
ERROR:  quantity cannot be negative
DETAIL:  got -3
HINT:  pass zero or more
CONTEXT:  PL/pgSQL function check_qty(integer) line 4 at RAISE
```

## A trigger function

A function returning `trigger` can assign to `NEW` fields and return `NEW`. `TG_OP` tells an insert from an update.

```lua
CREATE FUNCTION stamp_change() RETURNS trigger LANGUAGE plxlua AS $$
if TG_OP == "INSERT" then
  NEW.changed_at = "row " .. NEW.id .. " created"
else
  NEW.changed_at = "row " .. NEW.id .. " touched"
end
return NEW
$$;
```

```sql
CREATE TABLE ck_audit(id int primary key, note text, changed_at text);
CREATE TRIGGER ck_audit_bi BEFORE INSERT OR UPDATE ON ck_audit
  FOR EACH ROW EXECUTE FUNCTION stamp_change();
INSERT INTO ck_audit(id, note) VALUES (1, 'first'), (2, 'second');
UPDATE ck_audit SET note = 'edited' WHERE id = 1;
SELECT * FROM ck_audit ORDER BY id;
```

```
 id |  note  |  changed_at   
----+--------+---------------
  1 | edited | row 1 touched
  2 | second | row 2 created
```

## Dynamic SQL with bind parameters

Pass values as extra arguments so they are sent as bind parameters (`$1`, `$2`, ...). Build identifiers into the SQL text with `..` and `quote_ident()`. `fetch_one` returns a single record.

```lua
CREATE FUNCTION count_where(tbl text, minid int) RETURNS bigint LANGUAGE plxlua AS $$
local r = fetch_one("SELECT count(*) AS c FROM " .. quote_ident(tbl) .. " WHERE id >= $1", minid)
return r.c
$$;
```

```sql
CREATE TABLE ck_users(id int, name text);
INSERT INTO ck_users VALUES (1, 'Ada'), (2, 'Alan'), (3, 'Grace');
SELECT count_where('ck_users', 2);
```

```
 count_where 
-------------
           2
```

## Lua idioms: or, and/or, continue

`x or default` lowers to `COALESCE`, `c and a or b` to `CASE`, and `goto continue` with `::continue::` at the end of the loop body to `CONTINUE`. A string parameter used as a condition is a nil test.

```lua
CREATE FUNCTION describe(name text, n int) RETURNS text LANGUAGE plxlua AS $$
local who = name or "someone"
local evens = 0
for i = 1, n do
  if i % 2 == 1 then goto continue end
  evens = evens + 1
  ::continue::
end
local size = n > 10 and "many" or "few"
if not name then who = who .. " (anonymous)" end
return who .. " counted " .. size .. " numbers, " .. evens .. " even"
$$;
```

```sql
SELECT describe('Ada', 4), describe(NULL, 20);
```

```
            describe             |                     describe                      
---------------------------------+---------------------------------------------------
 Ada counted few numbers, 2 even | someone (anonymous) counted many numbers, 10 even
```

## Iterating an array

`ipairs` over an array becomes `FOREACH`; the element type comes from the parameter's type. `table.concat` is `array_to_string`, and `string.format` formats a value.

```lua
CREATE FUNCTION summarize(prices numeric[]) RETURNS text LANGUAGE plxlua AS $$
local total = 0.0
local parts = {} --:: text[]
for i, p in ipairs(prices) do
  total = total + p
  table.insert(parts, string.format("#%d=%.2f", i, p))
end
return table.concat(parts, " ") .. " total " .. string.format("%.2f", total)
$$;
```

```sql
SELECT summarize(ARRAY[1.5, 2.25, 10]);
```

```
              summarize               
--------------------------------------
 #1=1.50 #2=2.25 #3=10.00 total 13.75
```

## Reducing a query into a string builder

Combine a query loop with the builder to fold rows into one value. Each name part is split with a SQL expression and its first letter is accumulated into initials.

```lua
CREATE FUNCTION initials(full_name text) RETURNS text LANGUAGE plxlua AS $$
local out = ""
for r in query("SELECT unnest(string_to_array($1, ' ')) AS part", full_name) do
  out = out .. r.part:sub(1, 1):upper()
end
return out
$$;
```

```sql
SELECT initials('grace brewster hopper');
```

```
 initials 
----------
 GBH
```
