# Debugging plx functions

A plx function is transpiled to plpgsql at `CREATE FUNCTION` time and executed by
the plpgsql interpreter. Runtime errors are therefore reported by plpgsql, with a
line number and `CONTEXT` that refer to the **generated plpgsql** stored in
`pg_proc.prosrc`, not to your dialect source. This page explains how to correlate
the two.

## See the generated plpgsql

The generated body is ordinary, readable plpgsql, one construct per source
construct. View it with:

```sql
\sf my_func                      -- in psql
-- or
SELECT pg_get_functiondef('my_func(int)'::regprocedure);
```

The first line is a plx sentinel comment (`/*plx:v1:<dialect>:<hash>*/`), then the
`DECLARE` block, `BEGIN`, the body, and a trailing comment that carries your
original source (base64-encoded). When an error says `line 8 at RAISE`, line 8 is
counted from that sentinel line.

## Recover your original source

plx embeds the original dialect source in the function, so you can always get it
back. Define this helper once:

```sql
CREATE FUNCTION plx_source(f regprocedure) RETURNS text
LANGUAGE sql STABLE AS $$
  SELECT convert_from(
           decode((regexp_match(prosrc, 'plx-orig:b64[$]([^$]*)[$]plx-orig'))[1],
                  'base64'),
           'UTF8')
  FROM pg_proc WHERE oid = f;
$$;
```

Then:

```sql
SELECT plx_source('my_func(int)');
```

returns exactly the Ruby / PHP / JavaScript / Python / COBOL body you wrote.

## Why the error line differs from your source line

plpgsql requires every local variable to be declared in a `DECLARE` block before
`BEGIN`. plx therefore **hoists** your declarations to the top of the generated
function. That, plus the one-line sentinel and the `BEGIN`, shifts the body down
relative to your source, so the reported line does not equal your source line.

To locate a failing statement:

1. Read the reported line and construct (`line N at RAISE`, `at assignment`, …).
2. Print the generated body numbered and look at line N:

   ```sql
   SELECT string_agg(n || ': ' || l, E'\n')
   FROM regexp_split_to_table(
          (SELECT prosrc FROM pg_proc WHERE oid = 'my_func(int)'::regprocedure),
          E'\n') WITH ORDINALITY AS x(l, n)
   WHERE l NOT LIKE '%plx-orig%';
   ```

3. The plpgsql at line N maps one-to-one to the statement you wrote; compare it to
   `plx_source('my_func(int)')`.

Because the generated plpgsql mirrors your logic statement for statement, the
construct name in the `CONTEXT` (`RAISE`, `assignment`, `FOR over SELECT`, …)
plus the surrounding lines is usually enough to pinpoint the source statement.
Keeping functions small keeps this trivial.

## Compile-time errors

Errors raised at `CREATE FUNCTION` time (a construct outside the supported subset,
a type that cannot be inferred, a missing `END-IF`, …) come from the plx
transpiler and carry your dialect's name and the **source** line, for example:

```
ERROR:  plxruby: unsupported operator in statement
DETAIL:  at plxruby source line 3
```

These already point at your source, so no correlation is needed.
