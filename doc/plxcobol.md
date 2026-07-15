# plxcobol: the COBOL dialect

plxcobol lets you write PostgreSQL functions with COBOL syntax (ISO/IEC
1989:2023, COBOL 2023, free format). At `CREATE FUNCTION` time plx transpiles the
body to plpgsql and stores the plpgsql in `pg_proc.prosrc`. The function runs on
the standard plpgsql interpreter.

The language name is `plxcobol`. It is a trusted language.

COBOL is verb-driven and unlike the other plx dialects, so it has its own front
end: data names are mapped to plpgsql identifiers (lower-cased, hyphens become
underscores), and compound statements use the COBOL 2023 explicit scope
terminators (`END-IF`, `END-PERFORM`, `END-EVALUATE`).

## Setup

```sql
CREATE EXTENSION plx;
```

## Function basics

A function body is a free-format COBOL program fragment: an optional
`DATA DIVISION` with a `WORKING-STORAGE SECTION` for locals, then an optional
`PROCEDURE DIVISION` header, then statements. A scalar function returns with
`GOBACK RETURNING`.

```sql
CREATE FUNCTION add(a int, b int) RETURNS int LANGUAGE plxcobol AS $$
PROCEDURE DIVISION.
    COMPUTE RESULT = A + B
    GOBACK RETURNING RESULT.
$$;
```

Function arguments are referenced by name (case-insensitively). Any SQL
expression is valid inside `COMPUTE` and conditions, because expressions are
passed through to plpgsql and SQL.

### Local variables and types

Locals are declared in `WORKING-STORAGE SECTION` as level entries. A `PICTURE`
clause is mapped to a SQL type:

| PICTURE / clause | SQL type |
|---|---|
| `PIC 9(n)`, `n` up to 9 | `integer` |
| `PIC 9(n)`, `n` up to 18 | `bigint` |
| `PIC 9(n)`, larger | `numeric(n)` |
| `PIC 9(i)V9(s)` (implied decimal) | `numeric(i+s, s)` |
| `PIC X(n)`, `PIC A(n)` | `varchar(n)` |
| `USAGE COMP-1` / `COMP-2` | `real` / `double precision` |

A leading `S` (sign) is accepted and ignored for typing. A `VALUE` clause becomes
the declaration's initializer:

```sql
WORKING-STORAGE SECTION.
01 WS-COUNT PIC 9(9) VALUE 0.
01 WS-NAME  PIC X(40).
01 WS-RATE  PIC 9(3)V9(4).
```

For types a PICTURE cannot express (`%TYPE`, `%ROWTYPE`, `RECORD`, `refcursor`),
use a `TYPE` clause, whose text is emitted verbatim as the plpgsql type:

```sql
01 WS-EMP  TYPE emp%ROWTYPE.
01 WS-ROW  TYPE RECORD.
01 WS-CUR  TYPE refcursor.
```

A COBOL 2023 constant uses `CONSTANT AS`:

```sql
01 PI CONSTANT AS 3.14159.
```

### Tables (arrays)

`OCCURS n [TIMES]` makes an item a table, mapped to a PostgreSQL array of the
element type (the fixed bound is not enforced). A subscript `WS-ARR(i)` reads or
writes element `i` (1-based):

```sql
01 WS-ARR PIC 9(9) OCCURS 5 TIMES.
...
COMPUTE WS-ARR(WS-I) = WS-I * WS-I     -- write element
COMPUTE WS-SUM = WS-SUM + WS-ARR(WS-I) -- read element
```

Iterate the whole table with `PERFORM v OVER ARRAY WS-ARR`. A subscript is
recognized in `MOVE`/`COMPUTE` targets and in expressions; use `COMPUTE` (not
`ADD`/`SUBTRACT`) when an operand is a subscripted element.

## Control flow

### IF / ELSE

```sql
IF SCORE >= 90
    MOVE "A" TO WS-GRADE
ELSE
    MOVE "F" TO WS-GRADE
END-IF
```

Conditions may use the relational symbols (`=`, `<>`, `<`, `>`, `<=`, `>=`) or
the COBOL relational words (`IS EQUAL TO`, `IS GREATER THAN`,
`IS GREATER THAN OR EQUAL TO`, `IS NOT EQUAL TO`, and the `LESS` forms). `AND`,
`OR`, and `NOT` combine conditions. `IS NULL` and `IS NOT NULL` test for null.

### EVALUATE

A subject makes a simple `CASE`; `EVALUATE TRUE` makes a searched `CASE`. Stacked
`WHEN` values share the following statements, and `WHEN OTHER` is the default.

```sql
EVALUATE N
    WHEN 1
        MOVE "one" TO WS-R
    WHEN 2
    WHEN 3
        MOVE "few" TO WS-R
    WHEN OTHER
        MOVE "many" TO WS-R
END-EVALUATE
```

```sql
EVALUATE TRUE
    WHEN N > 0   MOVE "pos" TO WS-R
    WHEN N < 0   MOVE "neg" TO WS-R
    WHEN OTHER   MOVE "zero" TO WS-R
END-EVALUATE
```

### Loops

`PERFORM` has several inline forms, each closed by `END-PERFORM`:

```sql
PERFORM UNTIL WS-I > N
    ADD 1 TO WS-I
END-PERFORM

PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > N
    ADD WS-I TO WS-TOTAL
END-PERFORM

PERFORM N TIMES
    ADD 1 TO WS-COUNT
END-PERFORM
```

`EXIT PERFORM` leaves the loop and `EXIT PERFORM CYCLE` starts the next
iteration. `CONTINUE` is a no-op.

## Working with data

### Assignment and arithmetic

`MOVE` assigns; `COMPUTE` evaluates an expression (with `**` for exponent). The
arithmetic verbs `ADD`, `SUBTRACT`, `MULTIPLY`, and `DIVIDE` support the basic
forms, including `GIVING`:

```sql
MOVE 0 TO WS-TOTAL
COMPUTE WS-A = PI * R ** 2
ADD WS-I TO WS-TOTAL
SUBTRACT B FROM A GIVING WS-D
MULTIPLY A BY B GIVING WS-P
DIVIDE B INTO A GIVING WS-Q
```

### Iterating a query

`PERFORM <record> OVER "<sql>"` runs the loop body once per row. Field access on
the record is `<record>.<column>`. Add `USING <args>` for bind parameters or
non-literal SQL.

```sql
01 WS-ROW TYPE RECORD.
...
PERFORM WS-ROW OVER "SELECT id, amount FROM orders WHERE grp = $1" USING G
    ADD WS-ROW.AMOUNT TO WS-TOTAL
END-PERFORM
```

### Iterating an array

```sql
01 WS-V PIC 9(9).
...
PERFORM WS-V OVER ARRAY WS-VALUES
    ADD WS-V TO WS-TOTAL
END-PERFORM
```

### Dynamic SQL

`EXECUTE` runs dynamic SQL, with optional `USING` binds and `INTO` targets (in
either order):

```sql
EXECUTE "SELECT count(*) FROM t WHERE amount >= $1" USING WS-MIN INTO WS-COUNT
EXECUTE "INSERT INTO t(msg) VALUES ($1)" USING WS-NOTE
```

### Cursors

```sql
01 WS-C   TYPE refcursor.
01 WS-ROW TYPE RECORD.
...
OPEN-CURSOR WS-C FOR "SELECT v FROM t ORDER BY v"
FETCH-CURSOR WS-C INTO WS-ROW
PERFORM UNTIL WS-ROW IS NULL
    ADD WS-ROW.V TO WS-TOTAL
    FETCH-CURSOR WS-C INTO WS-ROW
END-PERFORM
CLOSE-CURSOR WS-C
```

`MOVE-CURSOR WS-C` and `MOVE-CURSOR WS-C 3` map to plpgsql `MOVE`.

### Diagnostics

`GET ROW-COUNT INTO <var>` maps to `GET DIAGNOSTICS <var> = ROW_COUNT`. `FOUND`
is available in conditions.

### Set-returning functions

```sql
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

`RETURN-QUERY "<sql>"` returns the rows of a query (add `USING` for binds or a
non-literal command).

### Calling a procedure

```sql
CALL "my_proc" USING WS-A WS-B
```

`COMMIT` and `ROLLBACK` are available in a procedure context.

### Building strings in a loop

Concatenating onto a string in a loop is slow in plpgsql (O(n^2), because text is
immutable and each step copies the whole string). `STRING-APPEND <expr> TO <var>`
lowers to the plx string builder (`plx_strbuild`), whose append is amortized
O(1):

```sql
01 WS-OUT PIC X(1) VALUE "".
...
PERFORM WS-ROW OVER "SELECT name FROM t ORDER BY id"
    STRING-APPEND WS-ROW.NAME TO WS-OUT
    STRING-APPEND "," TO WS-OUT
END-PERFORM
GOBACK RETURNING WS-OUT.
```

On PostgreSQL 18 this is amortized O(1) per append; on PostgreSQL 13 to 17 it is
correct but not accelerated (the in-place optimization needs a PostgreSQL 18
feature).

## Errors

### Raising

```sql
RAISE EXCEPTION "negative value" SQLSTATE "22023"
RAISE NOTICE "processed rows"
```

Levels are `EXCEPTION` (or `ERROR`), `NOTICE`, `WARNING`, `INFO`, `LOG`, and
`DEBUG`. `DISPLAY` emits a `NOTICE`, concatenating its operands.

### Handling

```sql
BEGIN-TRY
    COMPUTE WS-Q = A / B
WHEN DIVISION-BY-ZERO
    MOVE "cannot divide by zero" TO WS-MSG
WHEN OTHER
    GET MESSAGE INTO WS-MSG
END-TRY
```

`WHEN <condition>` names a SQL condition (`DIVISION-BY-ZERO`,
`UNIQUE-VIOLATION`, and similar; hyphens become underscores). `WHEN OTHER`
catches everything. Inside a handler, `GET` retrieves stacked diagnostics:
`GET MESSAGE`, `GET DETAIL`, `GET HINT`, `GET SQLSTATE`, and `GET CONTEXT`, each
`INTO <var>`.

### Assertions

```sql
ASSERT N > 0
```

## Expressions

- String literals use `"..."` or `'...'`; a doubled quote is a literal quote.
- Concatenation and formatting: `DISPLAY` concatenates its operands.
- Figurative constants: `ZERO` / `ZEROS` map to `0`, `SPACE` / `SPACES` to the
  empty string, `NULL` to SQL `NULL`.
- Arithmetic: `+`, `-`, `*`, `/` are as in SQL; `**` maps to SQL `^` (exponent)
  and `%` is modulo. `COMPUTE` evaluates the expression as SQL.
- Comparisons use SQL three-valued logic. Use `IS NULL` / `IS NOT NULL` to test
  for null.

## Trigger functions

A function returning `trigger` can be used as a trigger. Assign to `NEW` fields
with `MOVE` (or `COMPUTE`) and return `NEW`:

```sql
CREATE FUNCTION stamp() RETURNS trigger LANGUAGE plxcobol AS $$
PROCEDURE DIVISION.
    MOVE "row" TO NEW.TAG
    GOBACK RETURNING NEW.
$$;
```

`NEW`, `OLD`, and the `TG_` variables are available. Assigning to a record field
(`MOVE e TO NEW.COL`) works because a qualified name maps to `new.col`; a bare
`MOVE e TO NEW` is not supported.

## Semantic differences

These are intentional. plx pins semantics to SQL and plpgsql.

- Decimal literals infer `numeric`, not a floating-point type.
- Integer division and modulo follow SQL (truncate toward zero).
- A condition must be a boolean expression; there is no COBOL 88-level or
  class-condition abbreviation.
- Data names are function-scoped after mapping to plpgsql identifiers, so two
  names that differ only by `-` versus `_` collide.

## Not supported

Rejected at `CREATE FUNCTION` time with a line number:

- Full program structure beyond a single procedure body: `IDENTIFICATION`,
  `ENVIRONMENT`, and `CONFIGURATION` divisions, and named paragraphs or sections
  with out-of-line `PERFORM <paragraph>`.
- Group items (subordinate level entries under an `01`) in `WORKING-STORAGE`;
  declare elementary items, or use a `TYPE` clause for a composite plpgsql type.
- Fixed-format source (columns, sequence area, indicator area). Use free format.
- Report Writer, screen sections, object orientation, and the standard COBOL
  intrinsic function library. Use SQL functions inside expressions and SQL text.

See [PARITY.md](PARITY.md) for the full plpgsql construct matrix and
[ARCHITECTURE.md](ARCHITECTURE.md) for how plx maps to the plpgsql engine.
