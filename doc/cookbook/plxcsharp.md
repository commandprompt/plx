# C# cookbook

Practical recipes for the plxcsharp dialect. Every recipe here was run on PostgreSQL; plx transpiles the body to plpgsql and the standard interpreter executes it. See the [plxcsharp chapter](../plxcsharp.md) for the full language reference.

## Scalar function with branching

Declare the result with its type, assign it through an `if` / `else if` / `else` chain, and return it. Single statements after `if` and `else` need no braces.

```csharp
CREATE FUNCTION grade(score int) RETURNS text LANGUAGE plxcsharp AS $$
string g;
if (score >= 90) g = "A";
else if (score >= 80) g = "B";
else if (score >= 70) g = "C";
else g = "F";
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

A counting `for` loop over an `int` becomes a plpgsql integer `FOR`. The accumulator is declared `long`, which is `bigint`.

```csharp
CREATE FUNCTION factorial(n int) RETURNS bigint LANGUAGE plxcsharp AS $$
long result = 1;
for (int i = 2; i <= n; i++)
{
    result *= i;
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

Use `+=` on a `string` local. plx lowers it to its string builder, which is amortized O(1) per append on PostgreSQL 18 instead of the O(n^2) copy that `s = s + x` would produce.

```csharp
CREATE FUNCTION csv_repeat(word text, n int) RETURNS text LANGUAGE plxcsharp AS $$
string s = "";
for (int i = 1; i <= n; i++)
{
    s += word;
    if (i < n) s += ",";
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

A `StringBuilder` lowers to the same builder, and its `Append` calls chain:

```csharp
CREATE FUNCTION squares_csv(n int) RETURNS text LANGUAGE plxcsharp AS $$
var sb = new StringBuilder();
for (int i = 1; i <= n; i++)
{
    if (i > 1) sb.Append(", ");
    sb.Append(i).Append("^2=").Append(i * i);
}
return sb.ToString();
$$;
```

```sql
SELECT squares_csv(4);
```

```
         squares_csv         
-----------------------------
 1^2=1, 2^2=4, 3^2=9, 4^2=16
(1 row)
```

## Looping over a query

`foreach (var r in query(...))` iterates a result set. The row is a record and columns are read as `r.col`. An interpolated string passed to `query` is a SQL template: the value of `{g}` is bound as a parameter, not spliced into the SQL text.

```csharp
CREATE FUNCTION order_total(g int) RETURNS bigint LANGUAGE plxcsharp AS $$
long total = 0;
foreach (var r in query($"SELECT amount FROM orders WHERE grp = {g}"))
{
    total += r.amount;
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

```csharp
CREATE FUNCTION order_report(g int) RETURNS TABLE(id int, amount bigint) LANGUAGE plxcsharp AS $$
return_query($"SELECT orders.id, orders.amount FROM orders WHERE grp = {g} ORDER BY orders.amount DESC");
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

To produce computed rows one at a time, use `yield return` with `RETURNS SETOF`, as in a C# iterator:

```csharp
CREATE FUNCTION evens(lo int, hi int) RETURNS SETOF int LANGUAGE plxcsharp AS $$
for (int i = lo; i <= hi; i++)
{
    if (i % 2 == 0) yield return i;
}
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

`try` / `catch` maps to a plpgsql exception block. The exception class picks the condition: `DivideByZeroException` is `division_by_zero`, and `Exception` catches everything. `e.Message` reads `SQLERRM`, and `Console.WriteLine` raises a `NOTICE`.

```csharp
CREATE FUNCTION safe_divide(a int, b int) RETURNS int LANGUAGE plxcsharp AS $$
try
{
    return a / b;
}
catch (DivideByZeroException e)
{
    Console.WriteLine($"caught: {e.Message}");
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

To catch a specific database error, filter on `e.SqlState` the way Npgsql code does, with a SQLSTATE string or a `PostgresErrorCodes` name. The diagnostic properties (`ConstraintName`, `ColumnName`, `TableName`, ...) are available inside the handler.

```csharp
CREATE FUNCTION open_account(i int, who text) RETURNS text LANGUAGE plxcsharp AS $$
try
{
    execute("INSERT INTO accounts VALUES ($1, $2)", i, who);
    return $"opened {i}";
}
catch (PostgresException e) when (e.SqlState == PostgresErrorCodes.UniqueViolation)
{
    return $"account {i} exists ({e.ConstraintName})";
}
catch (PostgresException e) when (e.SqlState == "23502")
{
    return $"missing {e.ColumnName}";
}
$$;
```

```sql
CREATE TABLE accounts (id int PRIMARY KEY, owner text NOT NULL);
SELECT open_account(1, 'ada') AS first, open_account(1, 'bob') AS again,
       open_account(2, NULL) AS no_owner;
```

```
  first   |              again               |   no_owner    
----------+----------------------------------+---------------
 opened 1 | account 1 exists (accounts_pkey) | missing owner
(1 row)
```

## Trigger function

A function returning `trigger` can be attached to a table. Assign to `NEW` fields and return `NEW`. This one trims and upper-cases a code and derives a label before the row is written, on both insert and update.

```csharp
CREATE FUNCTION normalize_product() RETURNS trigger LANGUAGE plxcsharp AS $$
NEW.code = NEW.code.Trim().ToUpper();
NEW.label = $"code:{NEW.code}";
return NEW;
$$;
```

```sql
CREATE TABLE products (id int, code text, label text);
CREATE TRIGGER trg_norm BEFORE INSERT OR UPDATE ON products
  FOR EACH ROW EXECUTE FUNCTION normalize_product();
INSERT INTO products(id, code) VALUES (1, 'abc'), (2, ' xy ');
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

```csharp
CREATE FUNCTION log_event(msg text) RETURNS bigint LANGUAGE plxcsharp AS $$
execute("INSERT INTO events(note) VALUES ($1)", msg);
long n = row_count();
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

When a table or column name is itself a value, interpolate it through `ident()`, which quotes it as an identifier. Plain values in the same template are still bound, so a hostile value matches nothing:

```csharp
CREATE FUNCTION count_in(tbl text, col text, val text) RETURNS bigint LANGUAGE plxcsharp AS $$
long n = fetch_one($"SELECT count(*) FROM {ident(tbl)} WHERE {ident(col)} = {val}");
return n;
$$;
```

```sql
SELECT count_in('products', 'code', 'ABC') AS hits,
       count_in('products', 'code', $x$ABC' OR 'x' = 'x$x$) AS injection_blocked;
```

```
 hits | injection_blocked 
------+-------------------
    1 |                 0
(1 row)
```

## C# idioms: interpolation, foreach, and switch

This function combines the common C# idioms plx supports: a `foreach` over a `query`, `++` and `+=`, a `switch` statement that lowers to `CASE`, and an interpolated string that puts several values into the return string.

```csharp
CREATE FUNCTION describe_grp(g int) RETURNS text LANGUAGE plxcsharp AS $$
int count = 0;
long total = 0;
foreach (var r in query($"SELECT amount FROM orders WHERE grp = {g}"))
{
    count++;
    total += r.amount;
}
string size;
switch (count)
{
    case 0: size = "empty"; break;
    case 1: size = "single"; break;
    default: size = "many"; break;
}
return $"group {g}: {count} orders ({size}), total {total}";
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

`fetch_one` returns one row, or a row with all fields NULL when nothing matches. A ternary (`c ? a : b`, which lowers to `CASE`) plus an `== null` test turns the miss into a readable message. `x == null` becomes `IS NULL`.

```csharp
CREATE FUNCTION user_greeting(uid int) RETURNS text LANGUAGE plxcsharp AS $$
var u = fetch_one($"SELECT name FROM users WHERE id = {uid}");
return u.name == null ? $"no user #{uid}" : $"hello, {u.name}";
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

## Defaults, switch expressions, and formatting

`??` supplies a default for NULL (it is `COALESCE`), a switch expression with relational patterns picks a band, and `string.Format` with an alignment pads a column. The `:F2` format in an interpolated string rounds to two decimal places.

```csharp
CREATE FUNCTION price_tag(amount numeric, discount numeric) RETURNS text LANGUAGE plxcsharp AS $$
decimal net = amount * (1 - (discount ?? 0m) / 100);
string band = net switch { < 10 => "budget", < 100 => "standard", _ => "premium" };
return string.Format("{0,-9}|{1}", band, $"{net:F2}");
$$;
```

```sql
SELECT price_tag(19.99, 25) AS a, price_tag(250, NULL) AS b, price_tag(5, 10) AS c;
```

```
        a        |        b         |       c        
-----------------+------------------+----------------
 standard |14.99 | premium  |250.00 | budget   |4.50
(1 row)
```
