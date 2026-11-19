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
| outside quotes: `id = #{n}` | `id = n` (plpgsql binds the variable) | `id = $k`, passed with `USING` |
| a whole quoted literal: `'#{s}'` | `(s)` | `($k)` |
| inside a longer literal: `'%#{s}%'` | `('%' \|\| (s)::text \|\| '%')` | `('%' \|\| ($k)::text \|\| '%')` |
| inside a quoted identifier: `"#{t}"` | rejected | rejected |

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

- a value interpolated where SQL needs an identifier (`FROM #{t}`) becomes a
  parameter and the statement fails with `syntax error at or near "$1"`; write
  `#{ident(t)}`;
- a value interpolated to build SQL text on purpose (`ORDER BY #{col} DESC`
  where `col` holds `name`) is now bound as data; write `#{raw(col)}` (or
  `#{ident(col)}` when it is a column name);
- `'#{x}'` now compares against the value of `x`, not the string `'x'`.
