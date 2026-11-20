# Interpolation in SQL

Every dialect with string interpolation (plxruby `#{x}`, plxphp `{$x}`/`$x`,
plxjs and plxts `${x}`, plxpython3 f-string `{x}`) can interpolate values into
the SQL argument of `query`, `fetch_one`, `fetch_one!`, `execute`,
`open_cursor`, `return_query` and `perform`. Since 3.0, an interpolated value
never becomes SQL text. It is bound as a parameter, or quoted as a literal, and
the author asks explicitly for anything else.

## Values

```ruby
u = fetch_one("SELECT id, name FROM users WHERE name = '#{uname}' LIMIT $1", lim)
```

becomes

```plpgsql
EXECUTE 'SELECT id, name FROM users WHERE name = ($2) LIMIT $1' INTO u USING lim, uname;
```

| Where the interpolation sits | Static SQL | Dynamic SQL |
|---|---|---|
| outside quotes: `id = #{n}` | `id = (n)` (plpgsql binds the variable) | `id = $k`, passed with `USING` |
| where SQL needs a name: `FROM #{t}` | `FROM (t)`: `CREATE FUNCTION` fails with a syntax error (not in `UPDATE ... SET #{c} =`; see below) | `FROM $k`: the statement fails when it runs |
| a whole quoted literal: `'#{s}'` | `(s)` | `($k)` |
| inside a longer literal: `'%#{s}%'` | `('%' \|\| (s)::text \|\| '%')` | `('%' \|\| ($k)::text \|\| '%')` |
| a whole literal after a type name: `interval '#{s}'` | `CAST((s)::text AS interval)` | `CAST(($k)::text AS interval)` |
| inside a literal after a type name: `interval '#{n} days'` | `CAST(('' \|\| (n)::text \|\| ' days') AS interval)` | `CAST(('' \|\| ($k)::text \|\| ' days') AS interval)` |
| before an interval field: `interval '#{n}' hour` | rejected | rejected |
| inside a quoted identifier: `"#{t}"` | rejected | rejected |
| inside a SQL comment: `-- #{x}`, `/* #{x} */` | rejected | rejected |
| inside a dollar-quoted string: `$q$#{s}$q$` | rejected | rejected |
| inside a prefixed literal: `U&'#{s}'`, `N'#{s}'`, `text'#{s}'` | rejected | rejected |
| joined to a name, number or `$`: `v#{x}`, `#{x}abc` | rejected | rejected |
| right after an unpaired backslash in an `E'...'` string: `E'a\\#{x}'` in a Ruby string | rejected | rejected |

plx reads the SQL string as PostgreSQL does. A quote inside a `--` or
`/* */` comment (block comments nest) or inside a dollar-quoted string does
not open a string, and `E'...'` strings, `U&'...'` strings, doubled quotes
(`'it''s'`) and quoted identifiers are each read as one token. Each rejected
form fails when the function is created. In dynamic SQL, plx writes a literal
that contains a backslash as `E'...'`, as it does for the function body, so
the SQL reads the same whatever `standard_conforming_strings` the caller uses.
To end the text of an `E'...'` string with a backslash right before an
interpolation, write the backslash twice in the SQL text. The dialect's own
string escapes come first: in a double-quoted string (plxruby, plxphp), a
template string (plxjs, plxts) or an f-string (plxpython3), `\\` is one
backslash, so the SQL text `E'a\\#{x}'` is written `"SELECT E'a\\\\#{x}'"` in plxruby. Written
with two backslashes in the source, the SQL text has one, and plx rejects it.
With one backslash in the source, `\#{x}` is an escaped interpolation: the
SQL gets the text `#{x}`.

A whole quoted literal, `'#{s}'`, becomes the value itself, with the value's
type. Where SQL needs another type, cast it: with a `text` variable `since`,
`ts > '#{since}'` fails with `operator does not exist: date > text`, and
`ts > '#{since}'::date` works. A type name before a literal that holds an
interpolation makes plx write a `CAST` to that type. plx knows the built-in
type names, including `double precision`, `character varying` and
`timestamp with time zone`; after any other word the literal stays text. A
`char` or `character` literal becomes a `CAST` to `bpchar`, so the value
keeps its length, as `char 'abc'` does. A type with a modifier, such as
`varchar(2) '#{s}'`, is not recognised and fails with a syntax error; write
`CAST('#{s}' AS varchar(2))`. An interval field after the literal,
`interval '#{n}' hour`, is rejected, because a `CAST` cannot give it the
literal's meaning: write the unit inside the literal, `interval '#{n} hours'`,
or call `make_interval(hours => #{n})`.

A call is static when its SQL argument is a single string literal and it has
no bind arguments; otherwise it runs as dynamic SQL (`EXECUTE`). `execute` is
always dynamic. Interpolated values are numbered after the call's own bind
arguments, so `$1` in the example above is still `lim`.

`'#{s}'` used to put the *text* of the expression into the SQL (a string literal
reading `uname`), and in a dynamic call a value was concatenated into the SQL
unquoted. Both are gone: a value is data wherever it appears.

## Statements that take no parameters

PostgreSQL accepts `$n` parameters only in planned statements: `SELECT`,
`INSERT`, `UPDATE`, `DELETE`, `MERGE`, `WITH`, `VALUES`, `TABLE`, `CALL` and
`EXPLAIN`. In any other dynamic statement (`CREATE`, `ALTER`, `SET`, `COMMENT`,
`GRANT`, and so on), a value is spliced through `quote_nullable()` instead:

```ruby
execute("COMMENT ON TABLE #{ident(t)} IS '#{c}'")
execute("SET application_name = #{appname}")
```

```plpgsql
EXECUTE 'COMMENT ON TABLE ' || quote_ident((t)::text) || ' IS ' || quote_nullable((c)::text);
EXECUTE 'SET application_name = ' || quote_nullable((appname)::text);
```

A value inside a longer quoted literal (`IS 'note: #{c}'`) cannot be quoted in
such a statement and is rejected; build the whole value first and interpolate
it in place of the literal.

## Identifiers and SQL fragments

Three helpers, recognized only as the whole of an interpolation in a SQL
argument (plxphp spells them `{ident($t)}`, `{literal($v)}`, `{raw($s)}`), make
the call dynamic and splice their result into the SQL text:

| Helper | Lowers to | Use for |
|---|---|---|
| `ident(e)` | `quote_ident(e)` | a table, column or other identifier |
| `literal(e)` | `quote_nullable(e)` | a value as a quoted literal, in any statement |
| `raw(e)` | `e` as text, unquoted | a trusted SQL fragment (for example a sort order) |

```js
let r = fetch_one(`SELECT count(*) AS c FROM ${ident(tbl)} WHERE id >= ${minid}`);
for (const row of query(`SELECT name FROM users ORDER BY ${raw(order)}`)) { ... }
```

`ident()` and `literal()` cannot appear inside quotes; `raw()` is the only way
to put caller-supplied text into SQL unquoted, and it is the author's
responsibility to make that text safe.

## Escaped interpolations

A backslash before an interpolation makes it plain text, as it does in a
string value: plxphp `\$x` and `{\$x}`, plxjs and plxts `\${x}`, plxruby
`\#{x}`. So `fetch_one("SELECT '\$x' AS v")` in plxphp returns the text `$x`.
In a plxpython3 f-string, `\{x}` keeps the backslash and still interpolates,
as in Python; write `{{x}}` for the text `{x}`.
In plxphp, `\{` is no escape, as in PHP: `"\{$x}"` keeps the backslash and
the braces and interpolates `$x`, giving `\{5}` when `$x` is 5.

## What is not covered

Only interpolation written directly in the SQL argument is seen. A string
assembled earlier and passed in a variable is ordinary string concatenation:

```js
let sql = `SELECT * FROM t WHERE name = '${name}'`;   // concatenation
execute(sql);                                         // runs whatever sql holds
```

Write the interpolation in the call, or use bind arguments
(`execute("SELECT * FROM t WHERE name = $1", name)`).

## Upgrading from 2.x

Functions already in the catalog keep the plpgsql they were generated with.
When a function is created or replaced under 3.0:

- a value interpolated where SQL needs an identifier (`FROM #{t}`) is an
  error; write `#{ident(t)}`. In a static call `CREATE FUNCTION` fails with
  a syntax error at the parenthesis plx writes around the value. In a dynamic
  call the statement fails when it runs, with `syntax error at or near "$k"`,
  where `$k` is the value's parameter number;
  one static name position is not caught: `UPDATE t SET #{c} = ...` becomes
  `SET (c) = ...`, which PostgreSQL reads as a one-column list. With a plain
  value the statement fails when it runs; with a sub-`SELECT` or `ROW()` it
  updates the column named `c` whatever the variable holds, as 2.x did. Write
  `#{ident(c)}`;
- a value interpolated to build SQL text on purpose (`ORDER BY #{col} DESC`
  where `col` holds `name`) is now bound as data; write `#{raw(col)}` (or
  `#{ident(col)}` when it is a column name);
- `'#{x}'` now compares against the value of `x`, not the string `'x'`;
- an expression interpolated outside quotes in a static call is one value:
  `SELECT #{a + b} * 10` runs as `SELECT (a + b) * 10`. 2.x wrote
  `SELECT a + b * 10`, so a function that relied on the SQL splitting the
  expression computes a different result once re-created;
- an escaped interpolation (`\$x`, `\${x}`, `\#{x}`) is plain text in the
  SQL; 2.x put a backslash and the value of `x` there.
