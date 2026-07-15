# Transact-SQL cookbook

Practical recipes for the plxtsql dialect (SQL Server / Sybase T-SQL). Every recipe here was run on PostgreSQL; plx transpiles the body to plpgsql and the standard interpreter executes it. See the [plxtsql chapter](plxtsql.md) for the full language reference.

## Scalar function with branching

Declare a local with `DECLARE @g varchar` and pick a value through an `IF` / `ELSE IF` / `ELSE` chain. There is no `THEN` keyword in T-SQL: the condition runs up to the start of the body. A single-statement branch needs no `BEGIN ... END`.

```sql
CREATE FUNCTION grade(score int) RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @g varchar(10);
  IF @score >= 90
    SET @g = 'A';
  ELSE IF @score >= 80
    SET @g = 'B';
  ELSE IF @score >= 70
    SET @g = 'C';
  ELSE
    SET @g = 'F';
  RETURN @g;
$$;
```

```sql
SELECT grade(95), grade(83), grade(72), grade(40);
```

```
 grade | grade | grade | grade 
-------+-------+-------+-------
 A     | B     | C     | F
```

## Accumulating loop

A `WHILE` loop with a `BEGIN ... END` body. `DECLARE @x int = 0` both declares and initializes; `SET @total += @i` is the compound-assignment form, which expands to `total := total + (i)`.

```sql
CREATE FUNCTION sum_to(n int) RETURNS bigint LANGUAGE plxtsql AS $$
  DECLARE @i int = 1;
  DECLARE @total bigint = 0;
  WHILE @i <= @n
  BEGIN
    SET @total += @i;
    SET @i += 1;
  END
  RETURN @total;
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

## Building a text result in a loop

T-SQL overloads `+` for both numeric addition and string concatenation, so plx leaves `+` untranslated. Use `||` or `CONCAT(...)` to join strings. `VARCHAR(MAX)` maps to `text`.

```sql
CREATE FUNCTION repeat_word(word text, times int) RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @out varchar(max) = '';
  DECLARE @i int = 1;
  WHILE @i <= @times
  BEGIN
    IF @i > 1
      SET @out = @out || ', ';
    SET @out = CONCAT(@out, @word);
    SET @i += 1;
  END
  RETURN @out;
$$;
```

```sql
SELECT repeat_word('ping', 3);
```

```
   repeat_word    
------------------
 ping, ping, ping
```

## Reading a value from a query

`SELECT @x = col FROM t ...` becomes `SELECT col INTO x FROM t ...`. This is the T-SQL idiom for pulling a computed or aggregated value out of a table into a local variable. `ISNULL(@total, 0)` guards against a group with no rows.

```sql
CREATE TABLE orders (id int, grp int, amount int);
INSERT INTO orders VALUES (1,1,10),(2,1,25),(3,1,5),(4,2,100);

CREATE FUNCTION order_total(g int) RETURNS bigint LANGUAGE plxtsql AS $$
  DECLARE @total bigint;
  SELECT @total = SUM(amount) FROM orders WHERE grp = @g;
  RETURN ISNULL(@total, 0);
$$;
```

```sql
SELECT order_total(1), order_total(2), order_total(9);
```

```
 order_total | order_total | order_total 
-------------+-------------+-------------
          40 |         100 |           0
```

Name a local so it does not collide with a column referenced in the same query. If a variable and a column share a name, PostgreSQL reports an ambiguous reference.

## Set-returning function

In a function declared `RETURNS TABLE(...)` or `RETURNS SETOF ...`, a bare `SELECT` (one that is not an assignment) becomes `RETURN QUERY SELECT ...`.

```sql
CREATE FUNCTION squares(n int) RETURNS TABLE(k int) LANGUAGE plxtsql AS $$
  SELECT g * g FROM generate_series(1, @n) AS g;
$$;
```

```sql
SELECT * FROM squares(5);
```

```
 k  
----
  1
  4
  9
 16
 25
```

## Error handling with TRY / CATCH

`BEGIN TRY ... END TRY BEGIN CATCH ... END CATCH` becomes a plpgsql block with an `EXCEPTION WHEN OTHERS` handler. `ERROR_MESSAGE()` maps to `SQLERRM`, so the caught message is available in the handler.

```sql
CREATE FUNCTION safe_divide(a int, b int) RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @result varchar(100);
  BEGIN TRY
    SET @result = CAST(@a / @b AS text);
  END TRY
  BEGIN CATCH
    SET @result = 'error: ' || ERROR_MESSAGE();
  END CATCH
  RETURN @result;
$$;
```

```sql
SELECT safe_divide(10, 2), safe_divide(10, 0);
```

```
 safe_divide |       safe_divide       
-------------+-------------------------
 5           | error: division by zero
```

## Trigger function

A trigger function returns `trigger` and reads the row through `NEW` (and `OLD` on updates and deletes). This `BEFORE INSERT` trigger validates the incoming row and raises with `THROW` when the amount is negative; `RETURN NEW` lets a valid row through unchanged.

```sql
CREATE TABLE deposits (id int, amount numeric);

CREATE FUNCTION deposits_check() RETURNS trigger LANGUAGE plxtsql AS $$
  IF NEW.amount < 0
    THROW 50001, 'amount cannot be negative', 1;
  RETURN NEW;
$$;

CREATE TRIGGER trg_deposits_check BEFORE INSERT ON deposits
  FOR EACH ROW EXECUTE FUNCTION deposits_check();
```

```sql
INSERT INTO deposits (id, amount) VALUES (1, 100), (2, 50);
SELECT * FROM deposits ORDER BY id;
```

```
 id | amount 
----+--------
  1 |    100
  2 |     50
```

A row that fails the check is rejected:

```sql
INSERT INTO deposits (id, amount) VALUES (3, -5);
```

```
ERROR:  amount cannot be negative
```

## Dynamic SQL

`EXEC('<sql>')` (or `EXECUTE('<sql>')`) becomes plpgsql `EXECUTE '<sql>';`. Build the statement text with `||`.

```sql
CREATE FUNCTION make_seed(tbl text) RETURNS void LANGUAGE plxtsql AS $$
  EXEC('CREATE TABLE ' || @tbl || ' (id int)');
  EXEC('INSERT INTO ' || @tbl || ' VALUES (1), (2), (3)');
$$;
```

```sql
SELECT make_seed('dyn_demo');
SELECT count(*) AS rows_in_dyn_demo FROM dyn_demo;
```

```
 rows_in_dyn_demo 
------------------
                3
```

## T-SQL scalar functions

`LEN`, `CHARINDEX`, `IIF`, `CONVERT`, and `ISNULL` are rewritten to their PostgreSQL equivalents: `length`, `strpos` (with arguments swapped), a `CASE` expression, `CAST`, and `coalesce`. `CHARINDEX` returns 0 when the substring is not found.

```sql
CREATE FUNCTION describe(s text) RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @n int = LEN(@s);
  DECLARE @pos int = CHARINDEX('@', @s);
  DECLARE @kind varchar(20) = IIF(@pos > 0, 'email', 'plain');
  RETURN CONCAT('len=', CONVERT(varchar, @n),
                ' at=', CONVERT(varchar, @pos),
                ' kind=', @kind,
                ' safe=', ISNULL(@s, 'null'));
$$;
```

```sql
SELECT describe('bob@x.io'), describe('plain');
```

```
              describe               |             describe             
-------------------------------------+----------------------------------
 len=8 at=4 kind=email safe=bob@x.io | len=5 at=0 kind=plain safe=plain
```

## Last inserted identity and multi-target assignment

`@@IDENTITY` maps to `lastval()`, so it reports the value the surrounding `INSERT` generated for a serial column. `SELECT @a = ..., @b = ... FROM t` assigns several targets from one row in a single statement.

```sql
CREATE TABLE people (id serial primary key, name text, age int);

CREATE FUNCTION add_person(pname text, page int) RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @new_id int;
  DECLARE @who varchar(50);
  DECLARE @yrs varchar(10);
  INSERT INTO people(name, age) VALUES (@pname, @page);
  SET @new_id = @@IDENTITY;
  SELECT @who = name, @yrs = CAST(age AS text) FROM people WHERE id = @new_id;
  RETURN CONCAT('id=', CAST(@new_id AS text), ' ', @who, ' age ', @yrs);
$$;
```

```sql
SELECT add_person('Ada', 36);
SELECT add_person('Grace', 45);
```

```
    add_person     
-------------------
 id=1 Ada age 36
 id=2 Grace age 45
```
