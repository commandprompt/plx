# COBOL cookbook

Practical recipes for the plxcobol dialect (ISO/IEC 1989:2023). Every recipe here was run on PostgreSQL; plx transpiles the body to plpgsql and the standard interpreter executes it. See the [plxcobol chapter](plxcobol.md) for the full language reference.

## Absolute value with IF / ELSE

A scalar function branches with `IF ... ELSE ... END-IF` and returns with `GOBACK RETURNING`. The result variable is declared with a signed picture.

```cobol
CREATE FUNCTION abs_val(n int) RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-R PIC S9(9).
PROCEDURE DIVISION.
    IF N < 0
        COMPUTE WS-R = 0 - N
    ELSE
        MOVE N TO WS-R
    END-IF
    GOBACK RETURNING WS-R.
$$;
```

```sql
SELECT abs_val(-42) AS abs_neg, abs_val(7) AS abs_pos;
```

```
 abs_neg | abs_pos 
---------+---------
      42 |       7
(1 row)
```

## Factorial with an accumulating loop

`PERFORM VARYING ... UNTIL ... END-PERFORM` is the counted loop. Here it multiplies a running product across the range. The accumulator starts at 1 with a `VALUE` clause.

```cobol
CREATE FUNCTION factorial(n int) RETURNS bigint LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-RESULT PIC 9(18) VALUE 1.
01 WS-I PIC 9(9).
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > N
        COMPUTE WS-RESULT = WS-RESULT * WS-I
    END-PERFORM
    GOBACK RETURNING WS-RESULT.
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

## Classifying with EVALUATE

`EVALUATE` over a subject maps to a `CASE`. Stacked `WHEN` values share the statements that follow them, and `WHEN OTHER` is the default.

```cobol
CREATE FUNCTION weekday_name(n int) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-R PIC X(9).
PROCEDURE DIVISION.
    EVALUATE N
        WHEN 1
            MOVE "Monday" TO WS-R
        WHEN 6
        WHEN 7
            MOVE "weekend" TO WS-R
        WHEN OTHER
            MOVE "weekday" TO WS-R
    END-EVALUATE
    GOBACK RETURNING WS-R.
$$;
```

```sql
SELECT weekday_name(1) AS d1, weekday_name(3) AS d3, weekday_name(7) AS d7;
```

```
   d1   |   d3    |   d7    
--------+---------+---------
 Monday | weekday | weekend
(1 row)
```

## Building a string in a loop

`STRING-APPEND <expr> TO <var>` lowers to the plx string builder, whose append is amortized O(1) on PostgreSQL 18. Non-text operands are coerced, so a numeric counter can be appended directly.

```cobol
CREATE FUNCTION number_list(n int) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-OUT PIC X(1) VALUE "".
01 WS-I PIC 9(9).
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > N
        STRING-APPEND WS-I TO WS-OUT
        STRING-APPEND "," TO WS-OUT
    END-PERFORM
    GOBACK RETURNING WS-OUT.
$$;
```

```sql
SELECT number_list(5);
```

```
 number_list 
-------------
 1,2,3,4,5,
(1 row)
```

## A table with OCCURS

`OCCURS n TIMES` makes an item a table, mapped to a PostgreSQL array. A subscript `WS-ARR(i)` is 1-based and is recognized in `COMPUTE` targets and expressions. This fills a table of squares, then reads one element back.

```cobol
CREATE FUNCTION nth_square(n int) RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-ARR PIC 9(9) OCCURS 10 TIMES.
01 WS-I PIC 9(9).
01 WS-OUT PIC 9(9).
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > 10
        COMPUTE WS-ARR(WS-I) = WS-I * WS-I
    END-PERFORM
    COMPUTE WS-OUT = WS-ARR(N)
    GOBACK RETURNING WS-OUT.
$$;
```

```sql
SELECT nth_square(1) AS s1, nth_square(7) AS s7;
```

```
 s1 | s7 
----+----
  1 | 49
(1 row)
```

## Looping over a query result

`PERFORM <record> OVER "<sql>"` runs the loop body once per row. Field access is `<record>.<column>`, and `USING` supplies bind parameters. This example assumes a table:

```sql
CREATE TABLE orders (id int, grp int, amount int);
INSERT INTO orders VALUES (1,1,100),(2,1,250),(3,1,75),(4,2,500);
```

```cobol
CREATE FUNCTION order_total(g int) RETURNS bigint LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-TOTAL PIC 9(18) VALUE 0.
01 WS-R TYPE RECORD.
PROCEDURE DIVISION.
    PERFORM WS-R OVER "SELECT amount FROM orders WHERE grp = $1" USING G
        ADD WS-R.AMOUNT TO WS-TOTAL
    END-PERFORM
    GOBACK RETURNING WS-TOTAL.
$$;
```

```sql
SELECT order_total(1);
```

```
 order_total 
-------------
         425
(1 row)
```

## A set-returning function

A function that returns `SETOF` emits rows with `RETURN-NEXT`. Each `RETURN-NEXT` adds one value to the result set; the function ends with a plain `GOBACK`.

```cobol
CREATE FUNCTION squares(n int) RETURNS SETOF int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-I PIC 9(9).
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > N
        RETURN-NEXT WS-I * WS-I
    END-PERFORM
    GOBACK.
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
(5 rows)
```

## Dynamic SQL with EXECUTE

`EXECUTE "<sql>"` runs a command built at run time, with `USING` binds and an `INTO` target for a scalar result. This counts the rows meeting a threshold on the `orders` table from the query recipe above.

```cobol
CREATE FUNCTION count_big(threshold int) RETURNS bigint LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-COUNT PIC 9(18).
PROCEDURE DIVISION.
    EXECUTE "SELECT count(*) FROM orders WHERE amount >= $1" USING THRESHOLD INTO WS-COUNT
    GOBACK RETURNING WS-COUNT.
$$;
```

```sql
SELECT count_big(100);
```

```
 count_big 
-----------
         3
(1 row)
```

## PICTURE clauses and their SQL types

A `PICTURE` clause chooses the SQL type of a local. `PIC 9(9)` is `integer`, a signed implied-decimal `PIC S9(5)V9(4)` is `numeric(9,4)`, and `PIC X(20)` is `varchar(20)`. This function uses one of each and returns a formatted line.

```cobol
CREATE FUNCTION picture_demo(n int, rate numeric, name text) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-COUNT PIC 9(9).
01 WS-RATE  PIC S9(5)V9(4).
01 WS-NAME  PIC X(20).
01 WS-OUT   PIC X(40) VALUE "".
PROCEDURE DIVISION.
    MOVE N TO WS-COUNT
    MOVE RATE TO WS-RATE
    MOVE NAME TO WS-NAME
    COMPUTE WS-RATE = WS-RATE * WS-COUNT
    STRING-APPEND WS-NAME TO WS-OUT
    STRING-APPEND " total=" TO WS-OUT
    STRING-APPEND WS-RATE TO WS-OUT
    GOBACK RETURNING WS-OUT.
$$;
```

```sql
SELECT picture_demo(3, 1.5, 'widgets');
```

```
     picture_demo     
----------------------
 widgets total=4.5000
(1 row)
```

The signed four-place scale keeps `WS-RATE` as `numeric(9,4)`, so `1.5 * 3` prints as `4.5000`.

## COMPUTE with an arithmetic expression

`COMPUTE` evaluates a full expression, with `**` for exponent and a `CONSTANT AS` value for a named constant. This computes the area of a circle.

```cobol
CREATE FUNCTION circle_area(r numeric) RETURNS numeric LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 PI CONSTANT AS 3.14159.
01 WS-A PIC 9(9)V9(4).
PROCEDURE DIVISION.
    COMPUTE WS-A = PI * R ** 2
    GOBACK RETURNING WS-A.
$$;
```

```sql
SELECT circle_area(2);
```

```
 circle_area 
-------------
     12.5664
(1 row)
```
