# plx User Guide

This guide shows how to write functions in the plx dialects. Each example is
taken from the PL/pgSQL chapter of the PostgreSQL documentation and is shown as
the plpgsql from the manual and then the plxruby, plxphp, plxjs, plxpython3, and
plxcobol versions. Because plx transpiles to plpgsql, the plpgsql form is also
what the plx versions produce and run.

The dialects that restructure their input rather than rename keywords — plxplsql
(Oracle PL/SQL), plxts (TypeScript), plxtsql (Transact-SQL), and plxgo (Go) — are
covered in full in their own chapters (see Further reading); each produces the
same plpgsql shown here.

Install the extension first:

```sql
CREATE EXTENSION plx;
```

## A scalar function

PL/pgSQL manual, "Structure of PL/pgSQL" (`sales_tax`).

plpgsql:

```sql
CREATE FUNCTION sales_tax(subtotal real) RETURNS real AS $$
BEGIN
    RETURN subtotal * 0.06;
END;
$$ LANGUAGE plpgsql;
```

plxruby:

```sql
CREATE FUNCTION sales_tax(subtotal real) RETURNS real LANGUAGE plxruby AS $$
return subtotal * 0.06
$$;
```

plxphp:

```sql
CREATE FUNCTION sales_tax(subtotal real) RETURNS real LANGUAGE plxphp AS $$
return $subtotal * 0.06;
$$;
```

plxjs:

```sql
CREATE FUNCTION sales_tax(subtotal real) RETURNS real LANGUAGE plxjs AS $$
return subtotal * 0.06;
$$;
```

plxpython3:

```sql
CREATE FUNCTION sales_tax(subtotal real) RETURNS real LANGUAGE plxpython3 AS $$
return subtotal * 0.06
$$;
```

plxcobol:

```sql
CREATE FUNCTION sales_tax(subtotal real) RETURNS real LANGUAGE plxcobol AS $$
PROCEDURE DIVISION.
    GOBACK RETURNING SUBTOTAL * 0.06.
$$;
```

## Conditionals

PL/pgSQL manual, "IF-THEN-ELSIF".

plpgsql:

```sql
CREATE FUNCTION classify(number int) RETURNS text AS $$
DECLARE
    result text;
BEGIN
    IF number = 0 THEN
        result := 'zero';
    ELSIF number > 0 THEN
        result := 'positive';
    ELSIF number < 0 THEN
        result := 'negative';
    ELSE
        result := 'NULL';
    END IF;
    RETURN result;
END;
$$ LANGUAGE plpgsql;
```

plxruby:

```sql
CREATE FUNCTION classify(number int) RETURNS text LANGUAGE plxruby AS $$
result #:: text
if number == 0
  result = "zero"
elsif number > 0
  result = "positive"
elsif number < 0
  result = "negative"
else
  result = "NULL"
end
return result
$$;
```

plxphp:

```sql
CREATE FUNCTION classify(number int) RETURNS text LANGUAGE plxphp AS $$
$result = "" /*:: text */;
if ($number == 0) { $result = "zero"; }
elseif ($number > 0) { $result = "positive"; }
elseif ($number < 0) { $result = "negative"; }
else { $result = "NULL"; }
return $result;
$$;
```

plxjs:

```sql
CREATE FUNCTION classify(number int) RETURNS text LANGUAGE plxjs AS $$
let result /*:: text */;
if (number == 0) { result = "zero"; }
else if (number > 0) { result = "positive"; }
else if (number < 0) { result = "negative"; }
else { result = "NULL"; }
return result;
$$;
```

plxpython3:

```sql
CREATE FUNCTION classify(number int) RETURNS text LANGUAGE plxpython3 AS $$
result #:: text
if number == 0:
    result = "zero"
elif number > 0:
    result = "positive"
elif number < 0:
    result = "negative"
else:
    result = "NULL"
return result
$$;
```

plxcobol:

```sql
CREATE FUNCTION classify(number int) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-RESULT PIC X(8).
PROCEDURE DIVISION.
    EVALUATE TRUE
        WHEN NUMBER = 0
            MOVE "zero" TO WS-RESULT
        WHEN NUMBER > 0
            MOVE "positive" TO WS-RESULT
        WHEN NUMBER < 0
            MOVE "negative" TO WS-RESULT
        WHEN OTHER
            MOVE "NULL" TO WS-RESULT
    END-EVALUATE
    GOBACK RETURNING WS-RESULT.
$$;
```

Note: `==` lowers to SQL `=`. A comparison with a NULL argument is unknown, so
none of the branches match and the `else` branch runs.

## A loop over a query

PL/pgSQL manual, "Looping through query results".

plpgsql:

```sql
CREATE FUNCTION order_total(g int) RETURNS bigint AS $$
DECLARE
    total bigint := 0;
    r record;
BEGIN
    FOR r IN SELECT amount FROM orders WHERE grp = g LOOP
        total := total + r.amount;
    END LOOP;
    RETURN total;
END;
$$ LANGUAGE plpgsql;
```

plxruby:

```sql
CREATE FUNCTION order_total(g int) RETURNS bigint LANGUAGE plxruby AS $$
total = 0 #:: bigint
query("SELECT amount FROM orders WHERE grp = #{g}").each do |r|
  total = total + r.amount
end
return total
$$;
```

plxphp:

```sql
CREATE FUNCTION order_total(g int) RETURNS bigint LANGUAGE plxphp AS $$
$total = 0 /*:: bigint */;
foreach (query("SELECT amount FROM orders WHERE grp = {$g}") as $r) {
  $total = $total + $r->amount;
}
return $total;
$$;
```

plxjs:

```sql
CREATE FUNCTION order_total(g int) RETURNS bigint LANGUAGE plxjs AS $$
let total = 0 /*:: bigint */;
for (const r of query(`SELECT amount FROM orders WHERE grp = ${g}`)) {
  total = total + r.amount;
}
return total;
$$;
```

plxpython3:

```sql
CREATE FUNCTION order_total(g int) RETURNS bigint LANGUAGE plxpython3 AS $$
total = 0 #:: bigint
for r in query(f"SELECT amount FROM orders WHERE grp = {g}"):
    total = total + r.amount
return total
$$;
```

plxcobol:

```sql
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

## An integer FOR loop

PL/pgSQL manual, "FOR (integer variant)".

plpgsql:

```sql
CREATE FUNCTION sum_to(n int) RETURNS bigint AS $$
DECLARE
    total bigint := 0;
BEGIN
    FOR i IN 1..n LOOP
        total := total + i;
    END LOOP;
    RETURN total;
END;
$$ LANGUAGE plpgsql;
```

plxruby:

```sql
CREATE FUNCTION sum_to(n int) RETURNS bigint LANGUAGE plxruby AS $$
total = 0 #:: bigint
for i in 1..n
  total = total + i
end
return total
$$;
```

plxphp:

```sql
CREATE FUNCTION sum_to(n int) RETURNS bigint LANGUAGE plxphp AS $$
$total = 0 /*:: bigint */;
for ($i = 1; $i <= $n; $i++) {
  $total = $total + $i;
}
return $total;
$$;
```

plxjs:

```sql
CREATE FUNCTION sum_to(n int) RETURNS bigint LANGUAGE plxjs AS $$
let total = 0 /*:: bigint */;
for (let i = 1; i <= n; i++) {
  total = total + i;
}
return total;
$$;
```

plxpython3:

```sql
CREATE FUNCTION sum_to(n int) RETURNS bigint LANGUAGE plxpython3 AS $$
total = 0 #:: bigint
for i in range(1, n + 1):
    total = total + i
return total
$$;
```

plxcobol:

```sql
CREATE FUNCTION sum_to(n int) RETURNS bigint LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-TOTAL PIC 9(18) VALUE 0.
01 WS-I PIC 9(9).
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > N
        ADD WS-I TO WS-TOTAL
    END-PERFORM
    GOBACK RETURNING WS-TOTAL.
$$;
```

## A set-returning function

PL/pgSQL manual, "RETURN NEXT and RETURN QUERY".

plpgsql:

```sql
CREATE FUNCTION squares(n int) RETURNS SETOF int AS $$
BEGIN
    FOR i IN 1..n LOOP
        RETURN NEXT i * i;
    END LOOP;
    RETURN;
END;
$$ LANGUAGE plpgsql;
```

plxruby:

```sql
CREATE FUNCTION squares(n int) RETURNS SETOF int LANGUAGE plxruby AS $$
for i in 1..n
  return_next i * i
end
return
$$;
```

plxphp:

```sql
CREATE FUNCTION squares(n int) RETURNS SETOF int LANGUAGE plxphp AS $$
for ($i = 1; $i <= $n; $i++) {
  return_next($i * $i);
}
return;
$$;
```

plxjs:

```sql
CREATE FUNCTION squares(n int) RETURNS SETOF int LANGUAGE plxjs AS $$
for (let i = 1; i <= n; i++) {
  return_next(i * i);
}
return;
$$;
```

plxpython3:

```sql
CREATE FUNCTION squares(n int) RETURNS SETOF int LANGUAGE plxpython3 AS $$
for i in range(1, n + 1):
    return_next(i * i)
return
$$;
```

plxcobol:

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

## Trapping errors

PL/pgSQL manual, "Trapping errors".

plpgsql:

```sql
CREATE FUNCTION safe_divide(a int, b int) RETURNS int AS $$
BEGIN
    RETURN a / b;
EXCEPTION
    WHEN division_by_zero THEN
        RAISE NOTICE 'caught division_by_zero';
        RETURN -1;
END;
$$ LANGUAGE plpgsql;
```

plxruby:

```sql
CREATE FUNCTION safe_divide(a int, b int) RETURNS int LANGUAGE plxruby AS $$
begin
  return a / b
rescue => e
  raise notice: "caught: #{e.message}"
  return -1
end
$$;
```

plxphp:

```sql
CREATE FUNCTION safe_divide(a int, b int) RETURNS int LANGUAGE plxphp AS $$
try {
  return $a / $b;
} catch (\Exception $e) {
  raise('notice', "caught: " . $e->message);
  return -1;
}
$$;
```

plxjs:

```sql
CREATE FUNCTION safe_divide(a int, b int) RETURNS int LANGUAGE plxjs AS $$
try {
  return a / b;
} catch (e) {
  raise("notice", `caught: ${e.message}`);
  return -1;
}
$$;
```

plxpython3:

```sql
CREATE FUNCTION safe_divide(a int, b int) RETURNS int LANGUAGE plxpython3 AS $$
try:
    return a / b
except Exception as e:
    raise("notice", f"caught: {e.message}")
    return -1
$$;
```

plxcobol:

```sql
CREATE FUNCTION safe_divide(a int, b int) RETURNS int LANGUAGE plxcobol AS $$
PROCEDURE DIVISION.
    BEGIN-TRY
        GOBACK RETURNING A / B
    WHEN DIVISION-BY-ZERO
        DISPLAY "caught division by zero"
        GOBACK RETURNING -1
    END-TRY.
$$;
```

## Raising errors

PL/pgSQL manual, "Errors and messages".

plpgsql:

```sql
RAISE EXCEPTION 'Nonexistent ID --> %', user_id USING ERRCODE = 'no_data_found';
```

plxruby:

```sql
raise exception: "Nonexistent ID --> #{user_id}", errcode: "P0002"
```

plxphp:

```sql
throw new Exception("Nonexistent ID --> {$user_id}");
```

plxjs:

```sql
throw new Error(`Nonexistent ID --> ${user_id}`);
```

plxpython3:

```sql
raise ValueError(f"Nonexistent ID --> {user_id}")
```

plxcobol:

```sql
RAISE EXCEPTION concat("Nonexistent ID --> ", USER-ID) SQLSTATE "P0002"
```

## Anonymous code blocks

plxruby:

```sql
DO LANGUAGE plxruby $$
for i in 1..3
  raise notice: "row #{i}"
end
$$;
```

plxphp:

```sql
DO LANGUAGE plxphp $$
for ($i = 1; $i <= 3; $i++) { raise('notice', "row {$i}"); }
$$;
```

plxjs:

```sql
DO LANGUAGE plxjs $$
for (let i = 1; i <= 3; i++) { raise("notice", `row ${i}`); }
$$;
```

plxpython3:

```sql
DO LANGUAGE plxpython3 $$
for i in range(1, 4):
    raise("notice", f"row {i}")
$$;
```

plxcobol:

```sql
DO LANGUAGE plxcobol $$
WORKING-STORAGE SECTION.
01 WS-I PIC 9(9).
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > 3
        DISPLAY "row " WS-I
    END-PERFORM.
$$;
```

## Trust and security

The plx languages are declared `TRUSTED`. This is correct for the following
reasons:

- A plx function is transpiled to plpgsql and executed by plpgsql's call
  handler. It can do what a plpgsql function can do, and nothing more: no direct
  filesystem access, no network access, no arbitrary native code. All SQL runs
  with the privileges of the calling role.
- plx does not embed a language runtime. This is the difference from the native
  PL/PHP and PL/Ruby, which are untrusted because they load a full PHP or Ruby
  interpreter into the backend.

Two points to keep in mind:

- The transpiler is C code that parses the function body at `CREATE FUNCTION`
  time. It has a recursion-depth limit and has been fuzzed with mutation and
  pathological inputs across all dialects (see `test/fuzz.py`) with no crashes or
  hangs. Continued fuzzing is recommended before relying on it in a hostile
  multi-tenant deployment.
- `query`, `perform`, `execute`, and `fetch_one` with interpolated values carry
  the same SQL-injection responsibility as plpgsql `EXECUTE ... || value`. Use
  bind parameters (`execute(sql, a, b)`) for untrusted input.

## Further reading

- `README.md`: build and install.
- Per-dialect chapters: `doc/plxruby.md`, `doc/plxphp.md`, `doc/plxjs.md`,
  `doc/plxpython3.md`, `doc/plxcobol.md`, `doc/plxplsql.md`, `doc/plxts.md`,
  `doc/plxtsql.md`, `doc/plxgo.md` (full syntax, supported and rejected
  constructs, semantic differences).
- `doc/PARITY.md`: the plpgsql construct parity matrix.
- `doc/ARCHITECTURE.md`: how plx maps to the plpgsql engine.
- `doc/TRANSPILER.md`: transpiler specification.
