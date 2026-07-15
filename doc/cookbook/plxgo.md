# Go cookbook

Practical recipes for the plxgo dialect. Every recipe here was run on PostgreSQL; plx transpiles the body to plpgsql and the standard interpreter executes it. See the [plxgo chapter](plxgo.md) for the full language reference.

## Scalar function with branching

Return a value from an `if` / `else if` / `else` chain. Each branch returns directly, so no result variable is needed. The signature uses PostgreSQL types; the body is Go.

```go
CREATE FUNCTION grade(score int) RETURNS text LANGUAGE plxgo AS $$
	if score >= 90 {
		return "A"
	} else if score >= 80 {
		return "B"
	} else if score >= 70 {
		return "C"
	} else {
		return "F"
	}
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

A counting `for i := 1; i <= n; i++` lowers to a plpgsql integer `FOR`. The accumulator uses the short declaration `sum := 0`, so plx infers its type as `integer` from the literal. Initialize it to `0`: a bare `var sum int` starts as `NULL`, and `NULL += i` stays `NULL`.

```go
CREATE FUNCTION sum_to(n int) RETURNS int LANGUAGE plxgo AS $$
	sum := 0
	for i := 1; i <= n; i++ {
		sum += i
	}
	return sum
$$;
```

```sql
SELECT sum_to(5), sum_to(100);
```

```
 sum_to | sum_to 
--------+--------
     15 |   5050
```

## Build a text result without `+`

plxgo does not translate `+` on strings (it cannot tell numeric addition from concatenation without type information). Build text by appending to a `[]string` slice and joining it. `append` maps to `array_append`, and `array_to_string` is a PostgreSQL function called directly. `string(i)` converts the integer to text with a `::text` cast.

```go
CREATE FUNCTION csv_range(n int) RETURNS text LANGUAGE plxgo AS $$
	var parts []string
	for i := 1; i <= n; i++ {
		parts = append(parts, string(i))
	}
	return array_to_string(parts, ",")
$$;
```

```sql
SELECT csv_range(5);
```

```
 csv_range 
-----------
 1,2,3,4,5
```

## Looping over a query result

`for _, row := range query("...") { ... }` becomes `FOR row IN EXECUTE '...' LOOP`. Read a column with `row.col`. The accumulator is initialized with `total := 0` so the first `+=` has an integer to add to.

```go
CREATE FUNCTION order_total(g int) RETURNS int LANGUAGE plxgo AS $$
	total := 0
	for _, row := range query("SELECT amount FROM ck_go_orders WHERE grp = 7") {
		total += row.amount
	}
	return total
$$;
```

```sql
CREATE TABLE ck_go_orders(id int, amount int, grp int);
INSERT INTO ck_go_orders VALUES (1, 100, 7), (2, 250, 7), (3, 999, 8);
SELECT order_total(7);
```

```
 order_total 
-------------
         350
```

## A set-returning function

`RETURNS SETOF int` with `emit(x)` produces one row per value. `emit` maps to `RETURN NEXT`. The counting `for` supplies each `i`.

```go
CREATE FUNCTION squares(n int) RETURNS SETOF int LANGUAGE plxgo AS $$
	for i := 1; i <= n; i++ {
		emit(i * i)
	}
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

## Error handling with panic

`panic(m)` maps to `RAISE EXCEPTION '%', m`. It aborts the function and the surrounding statement, so the caller sees a normal PostgreSQL error. plxgo has no `recover`; trap the error on the SQL side (or in a calling plpgsql block) if you need to continue.

```go
CREATE FUNCTION safe_div(a int, b int) RETURNS int LANGUAGE plxgo AS $$
	if b == 0 {
		panic("division by zero")
	}
	return a / b
$$;
```

```sql
SELECT safe_div(10, 2);
SELECT safe_div(10, 0);
```

```
 safe_div 
----------
        5

ERROR:  division by zero
CONTEXT:  PL/pgSQL function safe_div(integer,integer) line 4 at RAISE
```

## A trigger function

A function returning `trigger` reads and assigns `NEW` fields and returns `NEW`. Here a `BEFORE INSERT OR UPDATE` trigger normalizes `name` to upper case with the mapped `strings.ToUpper`.

```go
CREATE FUNCTION upper_name() RETURNS trigger LANGUAGE plxgo AS $$
	NEW.name = strings.ToUpper(NEW.name)
	return NEW
$$;
```

```sql
CREATE TABLE ck_go_people(id int primary key, name text);
CREATE TRIGGER ck_go_people_bi BEFORE INSERT OR UPDATE ON ck_go_people
	FOR EACH ROW EXECUTE FUNCTION upper_name();
INSERT INTO ck_go_people(id, name) VALUES (1, 'ada'), (2, 'grace');
UPDATE ck_go_people SET name = 'alan' WHERE id = 1;
SELECT * FROM ck_go_people ORDER BY id;
```

```
 id | name  
----+-------
  1 | ALAN
  2 | GRACE
```

## Dynamic SQL with execute

`execute("...")` maps to `EXECUTE '...'` for a statement that returns no rows. The function returns `void`, so each call runs the statement for its effect.

```go
CREATE FUNCTION bump() RETURNS void LANGUAGE plxgo AS $$
	execute("UPDATE ck_go_counter SET n = n + 1")
$$;
```

```sql
CREATE TABLE ck_go_counter(n int);
INSERT INTO ck_go_counter VALUES (0);
SELECT bump();
SELECT bump();
SELECT n FROM ck_go_counter;
```

```
 n 
---
 2
```

## Go idioms: switch, range, slices

A `switch` becomes an `IF`/`ELSIF`/`ELSE` chain with no fall-through. The tagged form compares the tag against each case (comma-separated values are OR-ed); the tagless form takes a boolean condition per case. `for i := range n` counts `0 .. n-1`, `for _, v := range slice` walks the elements, and a `[]int{...}` literal is a PostgreSQL array indexed from `0` in Go terms.

Tagged switch:

```go
CREATE FUNCTION weekday(n int) RETURNS text LANGUAGE plxgo AS $$
	switch n {
	case 1, 7:
		return "weekend"
	case 2, 3, 4, 5, 6:
		return "weekday"
	default:
		return "unknown"
	}
$$;
```

```sql
SELECT weekday(1), weekday(3), weekday(9);
```

```
 weekday | weekday | weekday 
---------+---------+---------
 weekend | weekday | unknown
```

Tagless switch:

```go
CREATE FUNCTION sign_of(n int) RETURNS text LANGUAGE plxgo AS $$
	switch {
	case n > 0:
		return "positive"
	case n < 0:
		return "negative"
	default:
		return "zero"
	}
$$;
```

```sql
SELECT sign_of(5), sign_of(-2), sign_of(0);
```

```
 sign_of  | sign_of  | sign_of 
----------+----------+---------
 positive | negative | zero
```

Slice literal, `range` over the slice, `range` over a count, and 0-based indexing (`nums[0]` is the first element, `10`):

```go
CREATE FUNCTION slice_demo() RETURNS int LANGUAGE plxgo AS $$
	nums := []int{10, 20, 30}
	total := 0
	for _, v := range nums {
		total += v
	}
	for i := range 3 {
		total += i
	}
	return total + nums[0]
$$;
```

```sql
SELECT slice_demo();
```

```
 slice_demo 
------------
         73
```

## Type conversions and the standard library

`float64(x)` and `int(x)` become `::double precision` and `::integer` casts, and `string(x)` becomes `::text`. A short `:=` cannot infer the type of a plain function call, so `root` is declared with `var root float64` before `math.Sqrt` assigns to it. `strings.ToUpper` and `math.Sqrt` are part of the mapped standard library subset.

```go
CREATE FUNCTION describe_num(x int) RETURNS text LANGUAGE plxgo AS $$
	f := float64(x)
	var root float64
	root = math.Sqrt(f)
	var parts []string
	parts = append(parts, strings.ToUpper("sqrt"))
	parts = append(parts, string(int(root)))
	return array_to_string(parts, "=")
$$;
```

```sql
SELECT describe_num(9), describe_num(20);
```

```
 describe_num | describe_num 
--------------+--------------
 SQRT=3       | SQRT=4
```

`strconv.Atoi` parses text into an integer. Assign it through an explicit `var n int`, since a bare `:=` cannot infer the type of the call.

```go
CREATE FUNCTION parse_double(s text) RETURNS int LANGUAGE plxgo AS $$
	var n int
	n = strconv.Atoi(s)
	return n * 2
$$;
```

```sql
SELECT parse_double('21');
```

```
 parse_double 
--------------
           42
```
