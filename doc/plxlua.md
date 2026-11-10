# plxlua: the Lua dialect

plxlua lets you write PostgreSQL functions with Lua syntax. At
`CREATE FUNCTION` time plx transpiles the body to plpgsql and stores the plpgsql
in `pg_proc.prosrc`. The function runs on the standard plpgsql interpreter.

The language name is `plxlua`, so it does not collide with PL/Lua (`pllua`).
plxlua is a trusted language and loads no Lua interpreter: what runs is
plpgsql.

## Setup

```sql
CREATE EXTENSION plx;
```

## Function basics

The body is a sequence of Lua statements. As in Lua, statements are free form:
line breaks are not significant, a `;` between statements is optional, and
several statements can share a line. A scalar function ends with an explicit
`return`.

```sql
CREATE FUNCTION add(a int, b int) RETURNS int LANGUAGE plxlua AS $$
return a + b
$$;

CREATE FUNCTION fact(n int) RETURNS bigint LANGUAGE plxlua AS $$
if n <= 1 then return 1 end
local r = 1 --:: bigint
for i = 2, n do r = r * i end
return r
$$;
```

Function arguments are referenced by name. A call to a function that is not a
Lua construct is a call to the SQL function of that name, so `upper(s)`,
`coalesce(a, b)`, `now()` and `pg_catalog.lower(s)` all work. A call used as a
statement runs with `PERFORM`.

### Local variables and types

`local x = e` declares a local. plpgsql requires every variable to have a
type, so plx infers it from the first value:

| First value | Type |
|---|---|
| integer literal (`42`) | `integer`, or `bigint` beyond 32 bits |
| decimal literal (`0.5`, `1e3`) | `numeric` |
| string literal | `text` |
| `true`, `false` | `boolean` |
| table constructor of literals (`{1, 2}`) | an array (`integer[]`) |
| a parameter or a typed local | that type |
| `..`, `tostring(x)`, a string function | `text` |
| a comparison, `not`, `and`/`or` on conditions | `boolean` |
| `#x` | `integer` |
| arithmetic on known numeric types | the wider type |
| `fetch_one(...)`, `fetch_from(...)` | `record` |

Otherwise annotate the variable with a `--::` comment, which Lua itself reads
as a comment:

```lua
local n = 42                -- integer
local label = "count"       -- text
local amount --:: numeric
amount = principal * rate
local total = 0 --:: bigint
local names = {} --:: text[]
```

The annotation text is emitted verbatim as the plpgsql type, so `%TYPE` and
`%ROWTYPE` work:

```lua
local e --:: emp%ROWTYPE
local s --:: emp.sal%TYPE
```

A local declared without a value and without an annotation takes the type of
the first value assigned to it. A local whose type is never known is an error
at `CREATE FUNCTION` time that names the variable.

A first literal value at the top level of the function is folded into the
declaration, so `local n = 42` becomes `DECLARE n integer := 42;`.

Lua 5.4's `<const>` attribute makes a local read-only: assigning to it is an
error at `CREATE FUNCTION` time. When its value is a literal at the top level
it becomes a plpgsql `CONSTANT`:

```lua
local rate <const> = 0.25           -- rate CONSTANT numeric := 0.25
```

An assignment to a name that was never declared creates a local of the whole
function, where Lua would create a global.

### Scope

Locals are block scoped, as in Lua. plpgsql has one namespace for a whole
function and folds the case of names, so plx gives a Lua local its own plpgsql
variable (`x_2`, `x_3`, ...) when it shadows a local of an enclosing block or
a parameter, when it is declared again in the same block with a different
type, when its name differs from another visible name only in case, or when
its name is a word plpgsql reserves (`user`, `order`, `limit`, `left`, `to`,
`loop`, ...).

```sql
CREATE FUNCTION scopes(n int) RETURNS text LANGUAGE plxlua AS $$
local x = 1
do
  local x = "inner"          -- a new variable, x_2
end
local n = n * 100            -- shadows the parameter: n_2
local user = "u"             -- user_2
return x .. " " .. n .. " " .. user
$$;
SELECT scopes(3);   -- 1 300 u
```

A local declared inside a loop body is nil again at the start of every
iteration.

## Lua to PostgreSQL

| Lua | plpgsql |
|---|---|
| `local x = e`, `x = e` | `x := e;` (the declaration is hoisted) |
| `a, b = b, a` | assignments through temporaries when a value reads a target |
| `if c then ... elseif c then ... else ... end` | `IF ... ELSIF ... ELSE ... END IF;` |
| `while c do ... end` | `WHILE c LOOP ... END LOOP;` |
| `repeat ... until c` | `LOOP ... EXIT WHEN c; END LOOP;` |
| `for i = a, b do` | `FOR i IN a..b LOOP` |
| `for i = a, b, 2 do`, `for i = b, a, -1 do` | `FOR i IN a..b BY 2`, `FOR i IN REVERSE b..a` |
| `for i = a, b, step do` (variable step) | a counted `FOR` that computes `i` |
| `for r in query(sql, ...) do` | `FOR r IN sql LOOP`, or `FOR r IN EXECUTE sql USING ...` |
| `for i, v in ipairs(arr) do` | `FOREACH v IN ARRAY arr LOOP` (plus a counter for `i`) |
| `break` | `EXIT;` |
| `goto continue` ... `::continue::` (end of the body) | `CONTINUE;` |
| `goto done` ... `::done::` (right after the loop) | `EXIT label;` |
| `return e` | `RETURN e;` |
| `return a, b` (OUT parameters) | assigns the OUT parameters, then `RETURN;` |
| `emit(v)`, `return_next(v)` | `RETURN NEXT v;` |
| `local ok, err = pcall(function() ... end)` | `BEGIN ... EXCEPTION WHEN OTHERS OR assert_failure THEN ... END;` |
| `error("m")`, `error{message = ..., sqlstate = ...}` | `RAISE EXCEPTION ...` |
| `assert(c, m)` | `ASSERT c, m;` |
| `print(a, b)` | `RAISE NOTICE` |
| `a .. b` | `a \|\| b` |
| `a ~= b` | `a <> b` |
| `x == nil`, `x ~= nil` | `x IS NULL`, `x IS NOT NULL` |
| `nil`, `true`, `false` | `NULL`, `true`, `false` |
| `not`, `and`, `or` | `NOT`, `AND`, `OR` (see the value forms below) |
| `x or d` | `COALESCE(x, d)` |
| `c and a or b` | `CASE WHEN c THEN a ELSE b END` |
| `#s`, `#t` | `length(s)`, `cardinality(t)` |
| `{1, 2, 3}`, `{}` | `ARRAY[1, 2, 3]`, `'{}'` |
| `t[i]`, `r.f`, `r["f"]` | `t[i]`, `r.f`, `r.f` |
| `a // b` | `floor(a::numeric / b)` |
| `a & b`, `a \| b`, `a ~ b`, `~a`, `a << n`, `a >> n` | `&`, `\|`, `#`, `~`, `<<`, `>>` |
| `tostring(x)`, `tonumber(s)` | `x::text`, `s::numeric` |

## Control flow

### if / elseif / else

```sql
CREATE FUNCTION grade(score int) RETURNS text LANGUAGE plxlua AS $$
if score >= 90 then
  return "A"
elseif score >= 80 then
  return "B"
else
  return "F"
end
$$;
```

A condition follows Lua's truthiness where the type is known: a parameter or
local of a type other than `boolean` used as a condition (`if name then`,
`not name`) tests for nil, so it becomes `name IS NOT NULL` (`name IS NULL`).
A value whose type plx cannot see, such as a record field or a function
result, must be a boolean.

### Loops

`while`, `repeat`, and the numeric `for`:

```lua
while n > 0 do
  n = n - 1
end

repeat
  local k = j * 2
  j = j + 1
until k >= 6        -- the condition sees the body's local k

for i = 1, n do total = total + i end
for i = 10, 1, -2 do down[#down + 1] = i end
```

A literal step becomes `BY`, and a negative literal step becomes `REVERSE`. A
step held in a variable (or a fractional one, or a fractional start) becomes a
counted loop, `FOR k IN 0..floor((limit - start) / step)`, that computes the
loop variable from the counter, so a step of either sign is right and the
start and step are evaluated once, as in Lua. A literal step of `0` is
rejected, and a variable step of `0` raises `division_by_zero` when the loop
starts.
Assigning to the loop variable inside the body does not change the iteration,
as in Lua.

The generic `for` takes `query(...)` and `ipairs(...)`:

```lua
for row in query("SELECT id, amount FROM orders WHERE grp = $1", g) do
  total = total + row.amount
end

for i, v in ipairs(arr) do
  total = total + i * v
end

for _, v in ipairs(arr) do total = total + v end
for i in ipairs(arr) do count = count + 1 end
```

`ipairs` over an array becomes `FOREACH`. The element type comes from the
array's type (a parameter's, a local's, or a table constructor's). When it
cannot be seen, annotate it after `do`:

```lua
for _, w in ipairs(words) do --:: text
  s = s .. w
end
```

`pairs` is rejected: it iterates a table as a map.

### break and continue

`break` is `EXIT`. Lua has no `continue`; its idiom, a `goto continue` with
`::continue::` as the last statement of the loop body, becomes `CONTINUE`:

```lua
for i = 1, n do
  if i == 3 then goto continue end
  if i > 7 then break end
  total = total + i
  ::continue::
end
```

In a `repeat` loop the jump still runs the `until` test, as in Lua. A label
right after a loop is a break target, which leaves nested loops at once:

```lua
for i = 1, n do
  for j = 1, n do
    if i * j > limit then goto done end
  end
end
::done::
```

A `goto` from a nested loop to an outer loop's `::continue::` continues the
outer loop. Other uses of `goto` and labels are rejected.

### Blocks

`do ... end` is a block with its own scope.

## Expressions

- Concatenation: `a .. b` is `a || b`. A non-text operand is cast to text, so
  `"n=" .. 5` is `'n=' || 5::text`. A NULL operand makes the result NULL.
- Comparison: `==` and `~=` are `=` and `<>`. `x == nil` is `x IS NULL`.
- Length: `#s` is `length(s)` for a string and `#t` is `cardinality(t)` for an
  array. plx tells them apart by type; when the type cannot be seen (a record
  field, a function result) it is `length()`, so use `cardinality()` for such
  an array.
- Tables: a constructor `{a, b, c}` is `ARRAY[a, b, c]`, indexed from 1 like
  Lua and PostgreSQL arrays. `{}` is the empty array `'{}'`, which needs an
  annotation (`local t = {} --:: int[]`). `t[#t + 1] = v` appends.
- Arithmetic: `+ - * / %` are SQL's (see Semantic differences), `//` is floor
  division, `^` is exponentiation and right associative.
- `tostring(x)` is `x::text`, `tonumber(s)` is `s::numeric`.
- `and`, `or` and `not` in a condition are `AND`, `OR` and `NOT` with the
  truthiness rule above. In a value they give a boolean, except for three
  Lua idioms that are recognized exactly:

```lua
local who = name or "anon"                     -- COALESCE(name, 'anon')
local sign = n > 0 and "pos" or "neg"          -- CASE WHEN n > 0 THEN 'pos' ELSE 'neg' END
local size = n == 1 and "one" or n == 2 and "two" or "many"
                                               -- CASE WHEN ... WHEN ... ELSE 'many' END
local dbl = n and n * 2 or 0                   -- COALESCE(CASE WHEN n IS NOT NULL THEN n * 2 END, 0)
```

### Library functions

A small part of Lua's library is mapped. A string function can be called as
`string.f(s, ...)` or as the method `s:f(...)`.

| Lua | plpgsql |
|---|---|
| `string.upper(s)`, `string.lower(s)`, `string.reverse(s)` | `upper(s)`, `lower(s)`, `reverse(s)` |
| `string.len(s)` | `length(s)` |
| `string.sub(s, i [, j])` | `substr`, with Lua's rules for negative and out-of-range indexes |
| `string.rep(s, n)` | `repeat(s, n)` |
| `string.byte(s [, i])`, `string.char(n, ...)` | `ascii(...)`, `chr(n) \|\| ...` |
| `string.format(fmt, ...)` | a concatenation (below) |
| `math.floor`, `math.ceil`, `math.abs`, `math.sqrt`, `math.exp` | `floor`, `ceil`, `abs`, `sqrt`, `exp` |
| `math.max(...)`, `math.min(...)` | `GREATEST(...)`, `LEAST(...)` |
| `math.fmod(a, b)`, `math.log(x [, base])` | `mod(a, b)`, `ln(x)` or `log(base, x)` |
| `math.pi`, `math.huge`, `math.maxinteger`, `math.mininteger` | `pi()`, `'Infinity'`, the bigint limits |
| `table.concat(t [, sep [, i, j]])` | `array_to_string(t, sep)` |
| `table.insert(t, [pos,] v)`, `table.remove(t [, pos])` (statements) | `array_append` and array slices |

`string.format` needs a literal format and is lowered to a concatenation, so
a NULL operand makes the result NULL (PostgreSQL's `format()` would print it
as an empty string). The directives are `%s`, `%d`, `%i`, `%f` (with a
precision, `%.2f`), `%x`, `%X`, `%c` and `%%`, with a width and the `-` flag.
A width pads without truncating.

```sql
CREATE FUNCTION fmt(n int, s text) RETURNS text LANGUAGE plxlua AS $$
return string.format("[%5d][%-6s][%.2f][%x]", n, s, 3.14159, 255)
$$;
SELECT fmt(42, 'hi');   -- [   42][hi    ][3.14][ff]
```

### Strings

Double- and single-quoted strings take Lua's escapes (`\n`, `\t`, `\\`, `\"`,
`\ddd`, `\xXX`, `\u{XXXX}`, `\z`). Long strings `[[...]]` and `[==[...]==]`
are raw and may span lines; a line break right after the opening bracket is
not part of the string. Comments are `--` to the end of the line and
`--[[ ... ]]` (or `--[==[ ... ]==]`). A string cannot contain a NUL character.

## Building strings in a loop

Concatenating onto a string in a loop is slow in plpgsql: `s := s || 'x'` is
O(n^2) because text is immutable and each step copies the whole string. plx
recognizes exactly `s = s .. e` on a string local and lowers it to its string
builder (`plx_strbuild`):

```sql
CREATE FUNCTION csv(n int) RETURNS text LANGUAGE plxlua AS $$
local s = ""
for i = 1, n do
  s = s .. i .. ","
end
return s
$$;
```

```plpgsql
DECLARE
  s public.plx_strbuild := '';
BEGIN
  FOR i IN 1..n LOOP
    s := public.plx_sb_append(s, i::text || ',');
  END LOOP;
  RETURN s;
END;
```

On PostgreSQL 18 this is amortized O(1) per append. On PostgreSQL 13 to 17 it
is correct but not accelerated. The builder appends a NULL as nothing and
starts a NULL string as empty, where `..` elsewhere gives NULL.

## Working with data

Lua has no string interpolation, so values reach SQL as bind arguments
(`$1`, `$2`, ...) passed after the SQL text. A call with bind arguments runs
as dynamic SQL (`EXECUTE ... USING`). A call without them is static SQL, in
which a plain name refers to a plpgsql variable or parameter. See
[DYNAMIC-SQL.md](DYNAMIC-SQL.md).

### Iterating a query

```lua
local total = 0 --:: bigint
for row in query("SELECT amount FROM orders WHERE grp = $1", g) do
  total = total + row.amount
end
```

The row variable is a record. Field access is `row.col` or `row["col"]`.

### Fetching one row

```lua
local u = fetch_one("SELECT id, name FROM users WHERE id = $1", uid)
return u.name
```

`fetch_one` gives an all-NULL row when nothing matches. `fetch_one!` raises
when the query returns no row or more than one.

### Running SQL

```lua
perform("UPDATE counters SET hits = hits + 1 WHERE id = cid")
execute("INSERT INTO t(msg) VALUES ($1)", note)
return_query("SELECT id FROM vip WHERE level >= $1 ORDER BY id", lvl)   -- set-returning
```

`perform` runs a literal statement. `execute` always runs dynamic SQL. To build
SQL text, concatenate with `..` and quote identifiers with `quote_ident()`:

```lua
local sql = "SELECT count(*) AS c FROM " .. quote_ident(tbl)
local r = fetch_one(sql)
```

### Cursors

```lua
local c = open_cursor("SELECT v FROM t ORDER BY v")
local row = fetch_from(c)
while found() do
  total = total + row.v
  row = fetch_from(c)
end
close_cursor(c)
```

`move_cursor(c)` and `move_cursor(c, n)` map to `MOVE`.

### Diagnostics

```lua
local n = row_count()           -- GET DIAGNOSTICS n = ROW_COUNT
if found() then seen = seen + 1 end   -- the FOUND variable
```

`found()` follows static SQL. PL/pgSQL's `EXECUTE` does not set `FOUND`, so
after a call with bind arguments test `row_count()` instead.

### Procedures

`call("proc", a, b)` is `CALL proc(a, b)`. In a procedure, `commit()` and
`rollback()` end the transaction.

## Set-returning functions

```sql
CREATE FUNCTION squares(n int) RETURNS SETOF int LANGUAGE plxlua AS $$
for i = 1, n do
  emit(i * i)
end
return
$$;

CREATE FUNCTION items(n int) RETURNS TABLE(k int, label text) LANGUAGE plxlua AS $$
for i = 1, n do
  emit(i, "item " .. i)
end
$$;
```

`emit(v)` (or `return_next(v)`) produces a row. In a function with OUT columns
(`RETURNS TABLE`), `emit(a, b)` assigns the columns in order and produces the
row, and a bare `emit()` produces the current column values. A bare `return`
ends the function; `return v` is rejected in a set-returning function.

## OUT parameters

`return a, b` in a function with OUT parameters assigns them in order:

```sql
CREATE FUNCTION divmod(a int, b int, OUT q int, OUT r int) LANGUAGE plxlua AS $$
return a // b, a - (a // b) * b
$$;
SELECT * FROM divmod(17, 5);   -- 3 | 2
```

## Errors

### Raising

```lua
error("negative: " .. n)                -- RAISE EXCEPTION
error("bad value", 2)                   -- the level is accepted and ignored
error{message = "too big", sqlstate = "22023",
      detail = "n is " .. n, hint = "use 100 or less"}
raise("notice", "processed " .. n)     -- RAISE at a level
print("n is", n)                        -- RAISE NOTICE, values separated by tabs
assert(n > 0, "n must be positive")     -- ASSERT
```

`error{...}` takes the fields `message`, `sqlstate` (or `errcode`), `detail`,
`hint`, `column`, `constraint`, `datatype`, `table` and `schema`. `raise`
levels are `notice`, `warning`, `info`, `log`, `debug` and `exception`. In a
message built for `error`, `raise`, `assert` or `print`, a NULL operand of
`..` prints as an empty string, so one NULL cannot swallow the message.

### Handling

Lua handles errors with `pcall`. plx recognizes exactly
`pcall(function() ... end)` and lowers it to a plpgsql exception block:

```sql
CREATE FUNCTION safediv(a int, b int) RETURNS int LANGUAGE plxlua AS $$
local r --:: int
local ok, err = pcall(function()
  r = a // b
end)
if not ok then
  print("failed: " .. err)
  return -1
end
return r
$$;
```

```plpgsql
DECLARE
  r int;
  ok boolean;
  err text;
BEGIN
  BEGIN
    r := floor(a::numeric / b)::integer;
    ok := true;
    err := NULL;
  EXCEPTION WHEN OTHERS OR assert_failure THEN
    ok := false;
    err := SQLERRM;
  END;
  IF NOT ok THEN
    RAISE NOTICE '%', 'failed: ' || COALESCE(err, '');
    RETURN -1;
  END IF;
  RETURN r;
END;
```

The forms are `local ok, err = pcall(...)` (or `local ok = ...`, or an
assignment to existing variables), a bare `pcall(...)` statement that ignores
the error, and `if pcall(...) then` / `if not pcall(...) then`. The function
takes no parameters. As in Lua, `pcall` catches every error raised inside it,
a failed `assert` included (plpgsql's `OTHERS` alone leaves that one out); a
cancelled statement (a query cancel or `statement_timeout`) is not caught. A
bare `return` inside it leaves the protected block successfully; returning a value is rejected, and so are `break` and `goto` out
of it.

`err` is the error message (`SQLERRM`), so it can be printed and concatenated.
It also has the fields of the error:

| Field | Source |
|---|---|
| `err.message` | `SQLERRM` (the same as `err`) |
| `err.sqlstate` | `RETURNED_SQLSTATE` |
| `err.detail`, `err.hint`, `err.context` | `PG_EXCEPTION_DETAIL`, `PG_EXCEPTION_HINT`, `PG_EXCEPTION_CONTEXT` |
| `err.constraint`, `err.column`, `err.table`, `err.schema`, `err.datatype` | the matching `GET STACKED DIAGNOSTICS` items |

Only the fields that are read are captured. After a successful call `err` and
its fields are nil.

Lua has no exception classes. To handle one condition, test `err.sqlstate`
and raise anything else again with `error(err)`, which raises it with its
SQLSTATE, message, detail and hint:

```lua
local ok, err = pcall(function()
  execute("INSERT INTO uniq VALUES ($1)", k)
end)
if ok then return "inserted" end
if err.sqlstate == "23505" then
  return "duplicate on " .. err.constraint
end
error(err)
```

As in plpgsql, when the protected function raises an error its database
changes are rolled back (the exception block is a subtransaction) and its
variable assignments are kept.

## Trigger functions

A function returning `trigger` can be used as a trigger. Assign to `NEW`
fields and return `NEW` (or `OLD`, or `nil`):

```sql
CREATE FUNCTION stamp() RETURNS trigger LANGUAGE plxlua AS $$
if TG_OP == "INSERT" then
  NEW.tag = "row " .. NEW.id
elseif NEW.qty < 0 then
  error("negative quantity for " .. NEW.id)
end
return NEW
$$;
```

`NEW`, `OLD`, and the `TG_` variables are available. Assigning to a record
field (`NEW.col = e`) or an array element (`t[i] = e`) is supported.

## Semantic differences

These are intentional. plx pins semantics to SQL and plpgsql.

- Arrays are 1-based in both Lua and PostgreSQL. A nested table constructor is
  a multidimensional array: index it with both subscripts (`m[2][1]`), since
  `m[2]` alone is not a row, and `#m` counts every element.
- `nil` is SQL NULL, and comparisons use three-valued logic: `x == y` with a
  NULL operand is unknown, not false, and only a comparison with the literal
  `nil` becomes `IS NULL`. A condition that is NULL counts as false.
- `and` and `or` in a value give a boolean, except for the idioms `x or d`,
  `c and a or b` (with a literal `a`) and `c and v or d`. `x or d` is
  `COALESCE`, which differs from Lua only when `x` is `false`.
- `..` with a nil operand is an error in Lua; here the result is NULL. In a
  message for `error`, `raise`, `assert` or `print` the operand prints as an
  empty string instead, and in the string-builder form `s = s .. e` a NULL
  appends nothing.
- Numbers are SQL types, not Lua's integer and float subtypes. Decimal
  literals are exact `numeric`. `/` is SQL division: two integers divide as
  integers and truncate (`7 / 2` is `3`; write `7 / 2.0` for `3.5`). `%`
  truncates toward zero as in SQL (`-7 % 2` is `-1`, where Lua gives `1`).
  `//` is floor division, as in Lua. Integer overflow raises an error instead
  of wrapping. `math.floor` and `math.ceil` return the SQL type of their
  argument rather than an integer.
- A number concatenated with `..` or passed to `tostring` renders in SQL's
  text form (`numeric` keeps its scale, so `2.50` stays `2.50`).
- Strings are character strings: `#s`, `string.len` and `string.sub` count
  characters, not bytes, and comparisons use the database collation.
- `tostring(nil)` is NULL, not `"nil"`, and `print` shows NULL as `<NULL>`.
  `tonumber` of text that is not a number raises an error instead of
  returning nil.
- `ipairs` visits every element of the array, NULLs included; Lua stops at the
  first nil.
- A numeric `for` with an integer start and a fractional limit floors the
  limit (ceils it for a negative step), as Lua does.
- `assert` is plpgsql's `ASSERT`, which `plpgsql.check_asserts = off` turns
  off. `error`'s level is ignored, and its message has no position prefix.
- A local that shadows another is renamed in the plpgsql (`x_2`). SQL text
  refers to plpgsql names, so pass such a local to SQL as a bind argument
  rather than by name.
- As in plpgsql, a local with the same name as a column used in static SQL
  makes the column reference ambiguous; use a bind argument or another name.

## Not supported

Rejected at `CREATE FUNCTION` time with a line number:

- Function definitions: `function f()`, `local function f()`, and anonymous
  functions other than the one given to `pcall`; closures.
- Metatables (`setmetatable`, `getmetatable`) and object orientation.
- Coroutines.
- Tables as maps: constructors with keys (`{a = 1}`, `{[k] = v}`), `pairs`,
  `next`.
- Multiple return values (except into OUT parameters), varargs (`...`),
  `select`, `table.unpack`.
- `goto` and labels other than the two loop idioms.
- The `os`, `io`, `debug`, `package`, `coroutine` and `utf8` libraries,
  `require`, `load` and `dofile`.
- Lua patterns (`string.find`, `string.match`, `string.gmatch`,
  `string.gsub`); use `strpos()`, `regexp_match()` or `regexp_replace()`.
- `string.format` directives other than those listed, and flags other than
  `-`; `string.rep` with a separator; `tonumber` with a base.
- `xpcall`, and error values other than a string, `error{...}` and a caught
  `err`.
- The `<close>` attribute.
- A multiple assignment to fields or elements that reads one of its own
  targets (it needs typed temporaries); assign the values one at a time.

See [PARITY.md](PARITY.md) for the full plpgsql construct matrix and
[ARCHITECTURE.md](ARCHITECTURE.md) for how plx maps to the plpgsql engine.
