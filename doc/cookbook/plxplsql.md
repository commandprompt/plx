# Oracle PL/SQL cookbook

Practical recipes for the plxplsql dialect. Every recipe here was run on PostgreSQL; plx transpiles the body to plpgsql and the standard interpreter executes it. The function signature uses PostgreSQL types; the body is PL/SQL. See the [plxplsql chapter](plxplsql.md) for the full language reference.

## Scalar function with branching

Declare a local with an Oracle type and pick a value through an `IF` / `ELSIF` / `ELSE` chain. `VARCHAR2(10)` in the declaration section is translated to `varchar(10)`.

```sql
CREATE FUNCTION grade(score numeric) RETURNS text LANGUAGE plxplsql AS $$
  result VARCHAR2(10);
BEGIN
  IF score >= 90 THEN
    result := 'A';
  ELSIF score >= 80 THEN
    result := 'B';
  ELSIF score >= 70 THEN
    result := 'C';
  ELSE
    result := 'F';
  END IF;
  RETURN result;
END;
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

A numeric accumulator over an inclusive `FOR i IN 1..n LOOP` range. `NUMBER` maps to PostgreSQL `numeric`, which is arbitrary precision, so the factorial does not overflow.

```sql
CREATE FUNCTION factorial(n int) RETURNS numeric LANGUAGE plxplsql AS $$
  result NUMBER := 1;
BEGIN
  FOR i IN 1..n LOOP
    result := result * i;
  END LOOP;
  RETURN result;
END;
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

The `||` operator concatenates text. This body is already valid plpgsql, so plx passes it through and only translates the `VARCHAR2` declaration.

```sql
CREATE FUNCTION csv_row(n int) RETURNS text LANGUAGE plxplsql AS $$
  s VARCHAR2(200) := '';
BEGIN
  FOR i IN 1..n LOOP
    IF i > 1 THEN
      s := s || ',';
    END IF;
    s := s || i;
  END LOOP;
  RETURN s;
END;
$$;
```

```sql
SELECT csv_row(5);
```

```
  csv_row  
-----------
 1,2,3,4,5
```

## Cursor loop

An explicit `CURSOR c IS ...` declaration, iterated with `FOR r IN c LOOP`. plx rewrites the declaration to the plpgsql `c CURSOR FOR ...` form. The recipe needs a table, so create and populate one first.

```sql
CREATE TABLE product (id int, name text, price numeric);
INSERT INTO product VALUES (1, 'pen', 2.50), (2, 'notebook', 5.00), (3, 'stapler', 8.75);

CREATE FUNCTION price_list() RETURNS text LANGUAGE plxplsql AS $$
  CURSOR c IS SELECT name, price FROM product ORDER BY id;
  out VARCHAR2(400) := '';
BEGIN
  FOR r IN c LOOP
    out := out || r.name || '=' || r.price || ';';
  END LOOP;
  RETURN out;
END;
$$;
```

```sql
SELECT price_list();
```

```
              price_list              
--------------------------------------
 pen=2.50;notebook=5.00;stapler=8.75;
```

## Set-returning function

`RETURNS TABLE(...)` with `RETURN QUERY` streams rows back to the caller. The body is standard plpgsql; nothing Oracle-specific is needed. This uses the `product` table from the previous recipe.

```sql
CREATE FUNCTION affordable(max_price numeric)
  RETURNS TABLE(name text, price numeric) LANGUAGE plxplsql AS $$
BEGIN
  RETURN QUERY
    SELECT p.name, p.price FROM product p
    WHERE p.price <= max_price
    ORDER BY p.price;
END;
$$;
```

```sql
SELECT * FROM affordable(6.00);
```

```
   name   | price 
----------+-------
 pen      |  2.50
 notebook |  5.00
```

## Exception handling

Raise an application error with `RAISE_APPLICATION_ERROR`, which plx translates to `RAISE EXCEPTION`, and catch it in an `EXCEPTION` section. The error code (the first argument) is required; a single-argument form is not supported.

```sql
CREATE FUNCTION safe_divide(a numeric, b numeric) RETURNS numeric LANGUAGE plxplsql AS $$
BEGIN
  IF b = 0 THEN
    RAISE_APPLICATION_ERROR(-20001, 'cannot divide by zero');
  END IF;
  RETURN a / b;
EXCEPTION
  WHEN OTHERS THEN
    RETURN -1;
END;
$$;
```

```sql
SELECT safe_divide(10, 2), safe_divide(10, 0);
```

```
    safe_divide     | safe_divide 
--------------------+-------------
 5.0000000000000000 |          -1
```

## Trigger function

A `BEFORE INSERT OR UPDATE` trigger validates the row and stamps a timestamp. `NEW` and `OLD` are available as in plpgsql, and `SYSDATE` is translated to `LOCALTIMESTAMP`.

```sql
CREATE TABLE account (id int, name text, balance numeric, updated_at timestamp);

CREATE FUNCTION stamp_account() RETURNS trigger LANGUAGE plxplsql AS $$
BEGIN
  IF NEW.balance < 0 THEN
    RAISE_APPLICATION_ERROR(-20002, 'balance may not be negative');
  END IF;
  NEW.updated_at := SYSDATE;
  RETURN NEW;
END;
$$;

CREATE TRIGGER trg_stamp BEFORE INSERT OR UPDATE ON account
  FOR EACH ROW EXECUTE FUNCTION stamp_account();
```

```sql
INSERT INTO account (id, name, balance) VALUES (1, 'checking', 100.00);
SELECT id, name, balance, (updated_at IS NOT NULL) AS stamped FROM account;
```

```
 id |   name   | balance | stamped 
----+----------+---------+---------
  1 | checking |  100.00 | t
```

## Dynamic SQL

`EXECUTE IMMEDIATE` runs a query built at runtime. plx rewrites it to plpgsql `EXECUTE`; `INTO` captures a scalar and `USING` binds parameters by position (`$1`, `$2`, ...). This uses the `product` table from the cursor recipe.

```sql
CREATE FUNCTION count_where(min_price numeric) RETURNS bigint LANGUAGE plxplsql AS $$
  n bigint;
BEGIN
  EXECUTE IMMEDIATE 'SELECT count(*) FROM product WHERE price >= $1'
    INTO n USING min_price;
  RETURN n;
END;
$$;
```

```sql
SELECT count_where(5.00);
```

```
 count_where 
-------------
           2
```

## Oracle idioms: NVL, SYSDATE, %TYPE, NEXTVAL

Four Oracle spellings in one function. `NVL` becomes `coalesce`, `SYSDATE` becomes `LOCALTIMESTAMP`, `%TYPE` passes through, and `seq.NEXTVAL` becomes `nextval('seq')`. `SYSDATE` and `NVL` are not translated inside a `RAISE_APPLICATION_ERROR` or `DBMS_OUTPUT.PUT_LINE` argument, so assign them to a variable first as shown here. This uses the `product` table from the cursor recipe.

```sql
CREATE SEQUENCE ticket_seq;

CREATE FUNCTION next_ticket(label text) RETURNS text LANGUAGE plxplsql AS $$
  v_label product.name%TYPE;
  v_id    NUMBER;
  v_year  VARCHAR2(4);
BEGIN
  v_label := NVL(label, 'untitled');
  v_id    := ticket_seq.NEXTVAL;
  v_year  := to_char(SYSDATE, 'YYYY');
  RETURN v_year || '-' || v_id || ':' || v_label;
END;
$$;
```

```sql
SELECT next_ticket('bug'), next_ticket(NULL);
```

```
 next_ticket |   next_ticket   
-------------+-----------------
 2026-1:bug  | 2026-2:untitled
```

## WHILE loop over a numeric

A `WHILE` loop that counts the decimal digits of a value. It mixes `PLS_INTEGER` (translated to `integer`) and `NUMBER` (translated to `numeric`) locals.

```sql
CREATE FUNCTION digit_count(n numeric) RETURNS integer LANGUAGE plxplsql AS $$
  digits PLS_INTEGER := 0;
  v      NUMBER := abs(n);
BEGIN
  IF v < 1 THEN
    RETURN 1;
  END IF;
  WHILE v >= 1 LOOP
    v := floor(v / 10);
    digits := digits + 1;
  END LOOP;
  RETURN digits;
END;
$$;
```

```sql
SELECT digit_count(7), digit_count(42), digit_count(1000000);
```

```
 digit_count | digit_count | digit_count 
-------------+-------------+-------------
           1 |           2 |           7
```
