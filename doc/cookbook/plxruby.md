# Ruby cookbook

Practical recipes for the plxruby dialect. Every recipe here was run on PostgreSQL; plx transpiles the body to plpgsql and the standard interpreter executes it. See the [plxruby chapter](plxruby.md) for the full language reference.

## Scalar function with branching

Return a value from an `if` / `elsif` / `else` chain. Each branch returns directly, so no result variable is needed.

```ruby
CREATE FUNCTION letter_grade(score int) RETURNS text LANGUAGE plxruby AS $$
if score >= 90
  return "A"
elsif score >= 80
  return "B"
elsif score >= 70
  return "C"
elsif score >= 60
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

An integer `for` loop over an inclusive range. The accumulator is annotated `bigint` so the product does not overflow `int`.

```ruby
CREATE FUNCTION factorial(n int) RETURNS bigint LANGUAGE plxruby AS $$
result = 1 #:: bigint
for i in 1..n
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

The `<<` operator appends to a text local. plx lowers it to its string builder, so repeated appends are amortized O(1) on PostgreSQL 18 rather than the O(n^2) of `s = s || x`.

```ruby
CREATE FUNCTION repeat_stars(n int) RETURNS text LANGUAGE plxruby AS $$
s = "" #:: text
for i in 1..n
  s << "*"
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

`query(...).each do |r| ... end` iterates the rows. Interpolated values in the SQL string are spliced as name references, and fields are read with `r.col`.

```ruby
CREATE FUNCTION order_total(g int) RETURNS bigint LANGUAGE plxruby AS $$
total = 0 #:: bigint
query("SELECT amount FROM ck_orders WHERE grp = #{g}").each do |r|
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

`RETURNS SETOF int` with `emit` (the alias for a bare `return_next`) produces one row per value. A bare `return` ends the function.

```ruby
CREATE FUNCTION squares(n int) RETURNS SETOF int LANGUAGE plxruby AS $$
for i in 1..n
  emit i * i
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

## Trapping an error

`begin` / `rescue` maps to a plpgsql exception block. A bare `rescue => e` catches any error, and `e.message` reads `SQLERRM`.

```ruby
CREATE FUNCTION safe_divide(a int, b int) RETURNS int LANGUAGE plxruby AS $$
begin
  return a / b
rescue => e
  raise notice: "caught: #{e.message}"
  return -1
end
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

## A trigger function

A function returning `trigger` can assign to `NEW` fields and return `NEW`. Interpolation builds the stamped value from `NEW.id`.

```ruby
CREATE FUNCTION stamp_change() RETURNS trigger LANGUAGE plxruby AS $$
NEW.changed_at = "row #{NEW.id} touched"
return NEW
$$;
```

```sql
CREATE TABLE ck_audit(id int primary key, note text, changed_at text);
CREATE TRIGGER ck_audit_bi BEFORE INSERT OR UPDATE ON ck_audit
  FOR EACH ROW EXECUTE FUNCTION stamp_change();
INSERT INTO ck_audit(id, note) VALUES (1, 'first');
UPDATE ck_audit SET note = 'edited' WHERE id = 1;
SELECT * FROM ck_audit;
```

```
 id |  note  |  changed_at   
----+--------+---------------
  1 | edited | row 1 touched
```

## Dynamic SQL with bind parameters

Pass values as extra arguments so they are sent as bind parameters (`$1`, `$2`, ...) rather than interpolated. This is the safe form for untrusted input. `fetch_one` returns a single record.

```ruby
CREATE FUNCTION user_name(uid int) RETURNS text LANGUAGE plxruby AS $$
u = fetch_one("SELECT name FROM ck_users WHERE id = $1", uid)
return u.name
$$;
```

```sql
CREATE TABLE ck_users(id int, name text);
INSERT INTO ck_users VALUES (1, 'Ada'), (2, 'Alan');
SELECT user_name(1), user_name(2);
```

```
 user_name | user_name 
-----------+-----------
 Ada       | Alan
```

## Ruby idioms: interpolation, guard, ternary

The ternary `cond ? a : b` lowers to a `CASE` expression, `"#{expr}"` builds text by interpolation, and a modifier `if` guards a statement. Locals whose first value is not a plain literal are annotated with `#:: text`.

```ruby
CREATE FUNCTION describe(n int) RETURNS text LANGUAGE plxruby AS $$
kind #:: text
label #:: text
kind = n % 2 == 0 ? "even" : "odd"
label = "#{n} is #{kind}"
return "#{label} and big" if n >= 100
return label
$$;
```

```sql
SELECT describe(4), describe(7), describe(100);
```

```
 describe  | describe |      describe       
-----------+----------+---------------------
 4 is even | 7 is odd | 100 is even and big
```

## Reducing a query into a string builder

Combine a query loop with the `<<` builder to fold rows into one value. Here each name part is split with a SQL expression and its first letter is accumulated into initials.

```ruby
CREATE FUNCTION initials(full_name text) RETURNS text LANGUAGE plxruby AS $$
out = "" #:: text
query("SELECT unnest(string_to_array(#{full_name}, ' ')) AS part").each do |r|
  out << upper(left(r.part, 1))
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
