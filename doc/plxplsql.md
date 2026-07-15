# plxplsql: the Oracle PL/SQL dialect

plxplsql lets you write PostgreSQL functions with Oracle PL/SQL syntax. At
`CREATE FUNCTION` time plx transpiles the body to plpgsql and stores the plpgsql
in `pg_proc.prosrc`. The function runs on the standard plpgsql interpreter.

PL/SQL and plpgsql are both descended from Ada, so most of the language is the
same in both: `DECLARE`/`BEGIN`/`EXCEPTION`/`END`, `IF`/`ELSIF`/`ELSE`,
`LOOP`/`WHILE`/`FOR`, `CASE`, assignment with `:=`, `||` concatenation, cursors,
`%TYPE`, and `%ROWTYPE` all pass through unchanged. plxplsql is a
layout-preserving rewriter: it keeps your formatting and comments and only
translates the Oracle-specific spellings. This makes it a practical path for
moving Oracle PL/SQL into PostgreSQL.

## Setup

```sql
CREATE EXTENSION plx;
```

## Function basics

The body is a PL/SQL block: optional declarations, then `BEGIN ... END`, with an
optional `EXCEPTION` section. A `DECLARE` keyword is added automatically when the
body opens with declarations.

```sql
CREATE FUNCTION grade(score numeric) RETURNS text LANGUAGE plxplsql AS $$
  result VARCHAR2(10);
BEGIN
  IF score >= 90 THEN
    result := 'A';
  ELSIF score >= 80 THEN
    result := 'B';
  ELSE
    result := 'F';
  END IF;
  RETURN result;
END;
$$;
```

### Function signatures use PostgreSQL types

The **body** is PL/SQL, but the function **signature** (the parameter and return
types in `CREATE FUNCTION`) is parsed by PostgreSQL before plx sees the body, so
it must use PostgreSQL type names: write `RETURNS numeric`, not `RETURN NUMBER`.
Inside the body, Oracle type names are translated (see below).

## Oracle to PostgreSQL translations

Everything not listed here is emitted verbatim (it is already valid plpgsql).

### Types (in the body)

| Oracle | PostgreSQL |
|---|---|
| `NUMBER`, `NUMBER(p,s)` | `numeric`, `numeric(p,s)` |
| `VARCHAR2(n)`, `NVARCHAR2(n)` | `varchar(n)` |
| `PLS_INTEGER`, `BINARY_INTEGER`, `SIMPLE_INTEGER` | `integer` |
| `BINARY_FLOAT` / `BINARY_DOUBLE` | `real` / `double precision` |
| `CLOB`, `NCLOB`, `LONG` | `text` |
| `BLOB`, `RAW` | `bytea` |

### Statements and functions

| Oracle | PostgreSQL |
|---|---|
| `DBMS_OUTPUT.PUT_LINE(x)` | `RAISE NOTICE '%', (x)` |
| `RAISE_APPLICATION_ERROR(num, msg)` | `RAISE EXCEPTION '%', (msg)` |
| `EXECUTE IMMEDIATE sql [INTO v] [USING a]` | `EXECUTE sql [INTO v] [USING a]` |
| `... FROM DUAL` | `...` (removed) |
| `NVL(a, b)` | `coalesce(a, b)` |
| `seq.NEXTVAL` / `seq.CURRVAL` | `nextval('seq')` / `currval('seq')` |
| `SYSDATE` | `LOCALTIMESTAMP` |
| `SYSTIMESTAMP` | `clock_timestamp()` |
| `CURSOR c IS query` | `c CURSOR FOR query` |

## Cursors

Explicit cursors work as in PL/SQL:

```sql
CURSOR c IS SELECT v FROM t ORDER BY v;
...
OPEN c;
LOOP
  FETCH c INTO r;
  EXIT WHEN NOT FOUND;   -- or c%NOTFOUND, see notes
  ...
END LOOP;
CLOSE c;
```

## Errors

```sql
IF b = 0 THEN
  RAISE_APPLICATION_ERROR(-20001, 'cannot divide by zero');
END IF;
```

Handle exceptions with the PL/SQL (and plpgsql) `EXCEPTION` section:

```sql
EXCEPTION
  WHEN NO_DATA_FOUND THEN
    RETURN NULL;
  WHEN OTHERS THEN
    RETURN -1;
```

Condition names such as `NO_DATA_FOUND`, `TOO_MANY_ROWS`, `DUP_VAL_ON_INDEX`,
and `ZERO_DIVIDE` are recognized by plpgsql as well; where a name differs, use
the SQLSTATE condition name.

## Dynamic SQL

```sql
EXECUTE IMMEDIATE 'UPDATE t SET n = n + 1 WHERE id = $1' USING v_id;
EXECUTE IMMEDIATE 'SELECT count(*) FROM t WHERE g = $1' INTO v_count USING v_g;
```

## Semantic differences

These are intentional. plx pins semantics to SQL and plpgsql.

- The signature uses PostgreSQL types (see above); the body uses PL/SQL.
- Oracle type names are translated only in the **declaration section** (before the
  first `BEGIN`), so a body reference to a column or alias named like an Oracle
  type (e.g. a column `number`) is left untouched. A cast to an Oracle type in the
  body should use the PostgreSQL type (`x::numeric`, not `x::NUMBER`).
- `NUMBER` maps to `numeric` (arbitrary precision), not Oracle's `NUMBER`
  internal representation; arithmetic is PostgreSQL `numeric`.
- Empty string and NULL are distinct in PostgreSQL, unlike Oracle where `''` is
  NULL. Code that relies on Oracle's empty-string-is-NULL behavior needs review.
- Date and time follow PostgreSQL types and functions. `SYSDATE` maps to
  `LOCALTIMESTAMP`.

## Not supported

Rejected or unchanged (and therefore likely to error) at `CREATE FUNCTION` time:

- Packages (`CREATE PACKAGE`), package-qualified calls other than the
  `DBMS_OUTPUT` forms above, and the wider `DBMS_*` / `UTL_*` library.
- PL/SQL collections and associative arrays, records beyond `%ROWTYPE`, `BULK
  COLLECT`, `FORALL`, and pipelined functions.
- Autonomous transactions (`PRAGMA AUTONOMOUS_TRANSACTION`) and other pragmas.
- Oracle-only SQL (outer-join `(+)`, `CONNECT BY`, `DECODE`, `MERGE` specifics,
  Oracle hint comments) inside statements. Use the PostgreSQL equivalent.

See [PARITY.md](PARITY.md) for the plpgsql construct matrix and
[ARCHITECTURE.md](ARCHITECTURE.md) for how plx maps to the plpgsql engine.
