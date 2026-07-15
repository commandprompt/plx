# plxtsql: the Transact-SQL (SQL Server) dialect

plxtsql lets you write PostgreSQL functions with Transact-SQL syntax, the
procedural language of Microsoft SQL Server and Sybase. At `CREATE FUNCTION`
time plx transpiles the body to plpgsql and stores the plpgsql in
`pg_proc.prosrc`. The function then runs on the standard plpgsql interpreter.

Unlike plxplsql, T-SQL is not Ada-descended, so plxtsql is a restructuring front
end rather than a pass-through rewriter. It has its own tokenizer and parser and
emits plpgsql directly. It handles the parts of T-SQL that differ structurally
from plpgsql: `@`-prefixed variables, `DECLARE` anywhere in the body, `IF`/`WHILE`
bodies written as `BEGIN ... END` instead of `THEN ... END IF`, and
`TRY`/`CATCH`. This makes plxtsql a practical path for moving SQL Server stored
logic into PostgreSQL.

## Setup

```sql
CREATE EXTENSION plx;
```

## Function basics

The body is a sequence of T-SQL statements. Local variables are declared with
`DECLARE @name type` and referenced with the `@` sigil; plx hoists every
declaration into a plpgsql `DECLARE` block and drops the sigil in the generated
code.

```sql
CREATE FUNCTION grade(score int) RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @g varchar(10);
  IF @score >= 90
    SET @g = 'A';
  ELSE IF @score >= 80
    SET @g = 'B';
  ELSE
    SET @g = 'F';
  RETURN @g;
$$;
```

An outer `BEGIN ... END` wrapping the whole body is optional; if present it is
unwrapped (the declarations inside still hoist).

### Function signatures use PostgreSQL types

The **body** is T-SQL, but the function **signature** (the parameter and return
types in `CREATE FUNCTION`) is parsed by PostgreSQL before plx sees the body, so
it must use PostgreSQL type names: write `RETURNS int`, not `RETURNS INT` inside
a T-SQL `CREATE PROCEDURE`. Parameters are referenced in the body with the `@`
sigil (`@score` above binds to the parameter `score`). Inside the body, T-SQL
type names in `DECLARE` are translated (see below).

## Transact-SQL to PostgreSQL translations

### Variables and assignment

| T-SQL | plpgsql |
| --- | --- |
| `DECLARE @x int` | hoisted to `DECLARE x integer;` |
| `DECLARE @x int = 5` | `x integer;` plus `x := 5;` at that point |
| `DECLARE @a int, @b int` | both hoisted |
| `SET @x = e` | `x := e;` |
| `SET @x += e` | `x := x + (e);` (also `-=`, `*=`, `/=`, `%=`) |
| `SELECT @x = e` (no `FROM`) | `x := e;` |
| `SELECT @x = a, @y = b` | `x := a; y := b;` |
| `SELECT @x = col FROM t ...` | `SELECT col INTO x FROM t ...;` |

### Control flow

| T-SQL | plpgsql |
| --- | --- |
| `IF cond stmt` | `IF cond THEN stmt END IF;` |
| `IF cond BEGIN ... END` | `IF cond THEN ... END IF;` |
| `IF cond ... ELSE ...` | `IF cond THEN ... ELSE ... END IF;` |
| `ELSE IF ...` | nested `ELSE IF ... END IF;` |
| `WHILE cond BEGIN ... END` | `WHILE cond LOOP ... END LOOP;` |
| `BREAK` | `EXIT;` |
| `CONTINUE` | `CONTINUE;` |
| `RETURN e` | `RETURN e;` |

The condition after `IF`/`WHILE` runs up to the start of the body (a statement
keyword or `BEGIN`); there is no `THEN` keyword in T-SQL.

### Messages and errors

| T-SQL | plpgsql |
| --- | --- |
| `PRINT e` | `RAISE NOTICE '%', e;` |
| `RAISERROR('msg', sev, state)` | `RAISE EXCEPTION '%', 'msg';` |
| `THROW n, 'msg', s` | `RAISE EXCEPTION '%', 'msg';` |
| `THROW;` (bare, in CATCH) | `RAISE;` (re-raise) |
| `BEGIN TRY ... END TRY BEGIN CATCH ... END CATCH` | `BEGIN ... EXCEPTION WHEN OTHERS THEN ... END;` |
| `ERROR_MESSAGE()` | `SQLERRM` |

`RAISERROR`'s format arguments (its `%d`/`%s` printf-style substitutions) are not
applied; the message argument is emitted as-is.

### Types (in `DECLARE`)

| T-SQL | PostgreSQL |
| --- | --- |
| `INT`, `INTEGER` | `integer` |
| `BIGINT`, `SMALLINT`, `TINYINT` | `bigint`, `smallint`, `smallint` |
| `BIT` | `boolean` |
| `DECIMAL(p,s)`, `NUMERIC`, `MONEY` | `numeric(p,s)`, `numeric`, `numeric(19,4)` |
| `FLOAT`, `REAL` | `double precision`, `real` |
| `VARCHAR(n)`, `NVARCHAR(n)` | `varchar(n)` |
| `VARCHAR(MAX)`, `NVARCHAR(MAX)` | `text` |
| `CHAR(n)`, `NCHAR(n)` | `char(n)` |
| `TEXT`, `NTEXT` | `text` |
| `DATE`, `TIME` | `date`, `time` |
| `DATETIME`, `DATETIME2`, `SMALLDATETIME` | `timestamp` |
| `DATETIMEOFFSET` | `timestamptz` |
| `UNIQUEIDENTIFIER` | `uuid` |
| `VARBINARY(n)`, `VARBINARY(MAX)` | `bytea` |

An unrecognized type name is passed through, so PostgreSQL type names also work.

### Functions

| T-SQL | PostgreSQL |
| --- | --- |
| `ISNULL(a, b)` | `coalesce(a, b)` |
| `IIF(c, a, b)` | `CASE WHEN c THEN a ELSE b END` |
| `CONVERT(type, e)` | `CAST(e AS type)` (the style argument is ignored) |
| `LEN(x)` | `length(x)` |
| `DATALENGTH(x)` | `octet_length(x)` |
| `CHARINDEX(sub, str)` | `strpos(str, sub)` (arguments swapped) |
| `GETDATE()`, `SYSDATETIME()` | `now()` |
| `NEWID()` | `gen_random_uuid()` |
| `CEILING(x)` | `ceil(x)` |
| `@@IDENTITY` | `lastval()` |

`CAST(e AS type)` and most standard functions (`COALESCE`, `SUBSTRING`, `UPPER`,
`LOWER`, `ABS`, `ROUND`, ...) are already valid in PostgreSQL and pass through.

## Dynamic SQL

`EXEC('<sql>')` (equivalently `EXECUTE('<sql>')`) becomes `EXECUTE '<sql>';`.

```sql
CREATE FUNCTION run(tbl text) RETURNS void LANGUAGE plxtsql AS $$
  EXEC('TRUNCATE ' || @tbl);
$$;
```

## Set-returning functions

A bare `SELECT` (not an assignment) in a function declared `RETURNS TABLE(...)`
or `RETURNS SETOF ...` becomes `RETURN QUERY SELECT ...`.

```sql
CREATE FUNCTION series(n int) RETURNS TABLE(k int) LANGUAGE plxtsql AS $$
  SELECT g FROM generate_series(1, @n) AS g;
$$;
```

In a scalar function a bare `SELECT` becomes `PERFORM` (evaluated for effect).

## Statement separators

End statements with `;`. T-SQL treats `;` as optional in many places, but plx
uses it (together with block boundaries such as `END`, `ELSE`, and `END TRY`) to
find statement boundaries, so a missing `;` between two statements can cause them
to be read as one. Control-flow blocks (`IF`, `WHILE`, `BEGIN ... END`,
`TRY`/`CATCH`) are self-delimiting and do not require a trailing `;`.

## Session SET options

Statements such as `SET NOCOUNT ON` and `SET XACT_ABORT ON` are recognized and
ignored (they configure the SQL Server session and have no PostgreSQL
equivalent). `SET @variable = ...` is always an assignment.

## Not supported

- `@@ROWCOUNT`, `@@ERROR`, and other `@@` globals (except `@@IDENTITY`). Use
  `GET DIAGNOSTICS` or an exception handler in a construct plx does map, or write
  that part in another dialect.
- Table variables (`DECLARE @t TABLE (...)`) and T-SQL cursors
  (`DECLARE c CURSOR`).
- `GOTO`, `WAITFOR`, and label targets.
- Transaction control (`BEGIN TRAN`, `COMMIT`, `ROLLBACK`): a plpgsql function
  runs inside the caller's transaction and cannot manage transactions.
- Calling a stored procedure by name (`EXEC procname`); only `EXEC('<sql>')`
  dynamic SQL is supported.
- String concatenation with `+`. T-SQL overloads `+` for both numeric addition
  and string concatenation; plx cannot tell which is meant without type
  information, so it leaves `+` as `+`. Use `||` or `CONCAT(...)` for string
  concatenation.

See [PARITY.md](PARITY.md) for the full per-dialect feature matrix and
[USERGUIDE.md](USERGUIDE.md) for cross-dialect examples.
