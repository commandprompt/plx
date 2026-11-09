-- plxlua regression tests
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;

SELECT lanname, lanpltrusted FROM pg_language WHERE lanname = 'plxlua';

-- the generated plpgsql, without the sentinel hash and the embedded original
CREATE FUNCTION lua_src(nm text) RETURNS text LANGUAGE sql AS $$
  SELECT regexp_replace(regexp_replace(prosrc, '/\*plx-orig:[^*]*\*/\s*', '', 'g'),
                        '^/\*plx:v1:plxlua:[0-9a-f]+\*/\n', '')
  FROM pg_proc WHERE proname = nm $$;

CREATE TABLE lua_orders(grp int, amount bigint);
INSERT INTO lua_orders VALUES (1, 10), (1, 20), (1, 30), (2, 5), (3, NULL);
CREATE TABLE lua_emp(id int PRIMARY KEY, name text, sal numeric);
INSERT INTO lua_emp VALUES (1, 'ann', 100), (2, 'bob', 200);
CREATE TABLE lua_log(id serial, msg text);

-- the SQLSTATE, message, detail and hint a call raises
CREATE FUNCTION lua_caught(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE st text; msg text; det text; hnt text;
BEGIN
  EXECUTE q;
  RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
  GET STACKED DIAGNOSTICS st = RETURNED_SQLSTATE, msg = MESSAGE_TEXT,
                          det = PG_EXCEPTION_DETAIL, hnt = PG_EXCEPTION_HINT;
  RETURN st || ' ' || msg || ' / ' || det || ' / ' || hnt;
END $$;

-- ================================================================ basics

CREATE FUNCTION lua_add(a int, b int) RETURNS int LANGUAGE plxlua AS $$
return a + b * 2
$$;
SELECT lua_add(3, 4);

-- locals: literal inference, top-level folding, annotations, <const>
CREATE FUNCTION lua_locals(x numeric) RETURNS text LANGUAGE plxlua AS $$
local n = 42
local label = "count"
local ratio = 0.5
local on = true
local big = 3000000000
local amount --:: numeric
amount = x * ratio
local rate <const> = 0.25
local base <const> = 100 --:: numeric
local copy = n
return label .. " " .. n .. " " .. amount .. " " .. tostring(on) .. " " .. big .. " " .. base * rate .. " " .. copy
$$;
SELECT lua_locals(10);
SELECT lua_src('lua_locals');

-- an untyped local takes the type of its first value
CREATE FUNCTION lua_late() RETURNS text LANGUAGE plxlua AS $$
local g
if true then g = "set later" end
return g
$$;
SELECT lua_late();

-- ================================================================ control flow

CREATE FUNCTION lua_grade(score int) RETURNS text LANGUAGE plxlua AS $$
if score >= 90 then
  return "A"
elseif score >= 80 then
  return "B"
else
  return "F"
end
$$;
SELECT lua_grade(95), lua_grade(85), lua_grade(10), lua_grade(NULL);
SELECT lua_src('lua_grade');

-- one statement per line is not required
CREATE FUNCTION lua_fact(n int) RETURNS bigint LANGUAGE plxlua AS $$ if n <= 1 then return 1 end local r = 1 --:: bigint
for i = 2, n do r = r * i end return r $$;
SELECT lua_fact(1), lua_fact(10);

-- while, and repeat ... until whose condition sees the body's locals
CREATE FUNCTION lua_loops(n int) RETURNS text LANGUAGE plxlua AS $$
local i, total = 0, 0
while i < n do
  i = i + 1
  total = total + i
end
local j = 0
repeat
  local k = j * 2
  j = j + 1
until k >= 6
return total .. " " .. j
$$;
SELECT lua_loops(4);
SELECT lua_src('lua_loops');

-- numeric for: literal steps become FOR [REVERSE] ... BY; a variable step
-- counts iterations so a step of either sign is right
CREATE FUNCTION lua_steps(a int, b int, st int) RETURNS text LANGUAGE plxlua AS $$
local up = {} --:: int[]
for i = 1, 10, 3 do up[#up + 1] = i end
local down = {} --:: int[]
for i = 10, 1, -3 do down[#down + 1] = i end
local var = {} --:: int[]
for i = a, b, st do table.insert(var, i) end
local fl = {} --:: numeric[]
for x = 0, 1, 0.25 do table.insert(fl, x) end
return table.concat(up, ",") .. " | " .. table.concat(down, ",") .. " | " ..
       table.concat(var, ",") .. " | " .. table.concat(fl, ",")
$$;
SELECT lua_steps(1, 10, 4), lua_steps(10, 1, -4), lua_steps(1, 0, 1);
SELECT lua_src('lua_steps');

-- a non-integer limit is floored, as Lua does
CREATE FUNCTION lua_flimit(lim numeric) RETURNS int LANGUAGE plxlua AS $$
local n = 0
for i = 1, lim do n = n + 1 end
return n
$$;
SELECT lua_flimit(2.9);

-- break, and the continue idiom (goto continue with ::continue:: at the end)
CREATE FUNCTION lua_loopctl(n int) RETURNS int LANGUAGE plxlua AS $$
local total = 0
for i = 1, n do
  if i == 3 then goto continue end
  if i > 7 then break end
  total = total + i
  ::continue::
end
return total
$$;
SELECT lua_loopctl(0), lua_loopctl(5), lua_loopctl(10);

-- continue in a repeat loop still runs the until test
CREATE FUNCTION lua_repcont() RETURNS text LANGUAGE plxlua AS $$
local out, k = "", 0
repeat
  k = k + 1
  if k == 2 then goto continue end
  out = out .. k
  ::continue::
until k >= 4
return out
$$;
SELECT lua_repcont();
SELECT lua_src('lua_repcont');

-- break out of an outer loop (a label right after it), continue an outer loop
CREATE FUNCTION lua_nested(n int) RETURNS text LANGUAGE plxlua AS $$
local out = ""
for i = 1, n do
  for j = 1, n do
    if j > i then goto next_i end
    if i * j > 6 then goto done end
    out = out .. i .. j .. " "
  end
  ::next_i::
end
::done::
local w = 0
while w < 3 do
  w = w + 1
  for q = 1, 3 do
    if w == 2 and q == 2 then goto skip end
  end
  out = out .. "w" .. w
  ::skip::
end
return out
$$;
SELECT lua_nested(4);
SELECT lua_src('lua_nested');

-- do ... end is a block
CREATE FUNCTION lua_doblock() RETURNS int LANGUAGE plxlua AS $$
local x = 1
do
  local x = 2
  x = x + 1
end
return x
$$;
SELECT lua_doblock();

-- ================================================================ expressions

-- .. concatenates (and propagates NULL); ~= is <>; nil is NULL
CREATE FUNCTION lua_greet(nm text, n int) RETURNS text LANGUAGE plxlua AS $$
return "user " .. nm .. " has " .. n .. " items"
$$;
SELECT lua_greet('bob', 3), lua_greet(NULL, 3) IS NULL AS null_propagates;

CREATE FUNCTION lua_cmp(a int, b int) RETURNS text LANGUAGE plxlua AS $$
if a == nil or b == nil then return "missing" end
if a ~= b then return "differ" end
return "same"
$$;
SELECT lua_cmp(1, 1), lua_cmp(1, 2), lua_cmp(NULL, 2);

-- # is length() for a string and cardinality() for an array
CREATE FUNCTION lua_len(s text, a int[]) RETURNS text LANGUAGE plxlua AS $$
local t = {1, 2, 3}
return #s .. " " .. #a .. " " .. #t .. " " .. #"literal"
$$;
SELECT lua_len('hello', ARRAY[4, 5]);

-- table constructors are arrays (1-based, like PostgreSQL's); nesting is
-- a multidimensional array; t["f"] on a record is the field f
CREATE FUNCTION lua_tables() RETURNS text LANGUAGE plxlua AS $$
local t = {10, 20, 30}
local m = {{1, 2}, {3, 4}}
local r = fetch_one("SELECT 7 AS f")
t[2] = 25
return t[1] .. " " .. t[2] .. " " .. m[2][1] .. " " .. r["f"] .. " " .. #m
$$;
SELECT lua_tables();

-- arithmetic: // is floor division, % and / are SQL's, ^ is right associative
CREATE FUNCTION lua_arith(a int, b int) RETURNS text LANGUAGE plxlua AS $$
return a // b .. " " .. a / b .. " " .. a % b .. " " .. 2 ^ 3 ^ 2 .. " " .. -a ^ 2 .. " " .. 7 / 2.0
$$;
SELECT lua_arith(7, 2), lua_arith(-7, 2);
SELECT lua_src('lua_arith');

CREATE FUNCTION lua_bits(a int, b int) RETURNS text LANGUAGE plxlua AS $$
return (a & b) .. " " .. (a | b) .. " " .. (a ~ b) .. " " .. (a << 2) .. " " .. (a >> 1) .. " " .. ~a
$$;
SELECT lua_bits(12, 10);

-- conversions
CREATE FUNCTION lua_conv(s text, n int) RETURNS text LANGUAGE plxlua AS $$
return tostring(n) .. "|" .. tonumber(s) + 1 .. "|" .. tostring(n > 0)
$$;
SELECT lua_conv('2.5', 3);

-- string library and methods
CREATE FUNCTION lua_strs(s text) RETURNS text LANGUAGE plxlua AS $$
local a = s:upper() .. "|" .. string.lower(s) .. "|" .. s:len() .. "|" .. s:reverse()
local b = s:sub(2, 4) .. "|" .. s:sub(-3) .. "|" .. string.sub(s, 2) .. "|" .. s:sub(3, 2) .. "|" .. ("ab"):rep(3)
local c = string.byte("A") .. "|" .. string.char(72, 105) .. "|" .. s:byte(-1)
return a .. " / " .. b .. " / " .. c
$$;
SELECT lua_strs('Hello');
SELECT lua_src('lua_strs');

-- string.sub with variable indexes follows Lua's rules
CREATE FUNCTION lua_sub(s text, i int, j int) RETURNS text LANGUAGE plxlua AS $$
return s:sub(i, j)
$$;
SELECT lua_sub('hello', 2, 3), lua_sub('hello', -3, -2), lua_sub('hello', 0, 10),
       lua_sub('hello', -100, 2), lua_sub('hello', 4, 2), lua_sub('hello', 6, 9);

-- string.format lowers to concatenation, so a NULL operand propagates
CREATE FUNCTION lua_fmt(n int, s text) RETURNS text LANGUAGE plxlua AS $$
return string.format("[%5d][%-6s][%.2f][%x][%X][%s][%3s][%c]%%", n, s, 3.14159, 255, 255, s, "toolong", 65)
$$;
SELECT lua_fmt(42, 'hi'), lua_fmt(NULL, 'hi') IS NULL AS null_propagates;
SELECT lua_src('lua_fmt');

CREATE FUNCTION lua_math(x numeric, a int, b int) RETURNS text LANGUAGE plxlua AS $$
return math.floor(x) .. " " .. math.ceil(x) .. " " .. math.abs(-a) .. " " .. math.max(a, b, 3) .. " " ..
       math.min(a, b) .. " " .. math.fmod(a, b) .. " " .. math.sqrt(16) .. " " .. string.format("%.4f", math.pi) ..
       " " .. tostring(math.huge) .. " " .. math.maxinteger
$$;
SELECT lua_math(3.7, 7, 2), lua_math(-3.7, -7, 2);

-- and/or: in a condition they are AND/OR with Lua truthiness (a variable of
-- a non-boolean type is true when it is not nil); in a value, "x or d" is
-- COALESCE, "c and a or b" is CASE, and "c and v or d" picks v or d
CREATE FUNCTION lua_idioms(name text, n int) RETURNS text LANGUAGE plxlua AS $$
local who = name or "anon"
local sign = n > 0 and "pos" or "nonpos"
local size = n == 1 and "one" or n == 2 and "two" or "many"
local dbl = n and n * 2 or 0
local both = name ~= nil and n ~= nil
if name then who = who .. "!" end
if not name then who = who .. "?" end
return who .. " " .. sign .. " " .. size .. " " .. dbl .. " " .. tostring(both)
$$;
SELECT lua_idioms('bob', 2), lua_idioms(NULL, 1), lua_idioms(NULL, NULL);
SELECT lua_src('lua_idioms');

-- SQL functions are called by name; a qualified name works too
CREATE FUNCTION lua_sqlfn(s text) RETURNS text LANGUAGE plxlua AS $$
return pg_catalog.upper(coalesce(s, "none")) .. " " .. strpos(s, "l")
$$;
SELECT lua_sqlfn('hello');

-- ================================================================ strings

CREATE FUNCTION lua_esc() RETURNS text LANGUAGE plxlua AS $$
local a = "tab\tnl\\n q\"' \65\x42\u{263A} \z
      joined"
local b = 'it\'s'
local c = [==[back\slash ]] stays]==]
local d = [[
first newline dropped]]
-- a comment
--[[ a long
     comment ]]
return a .. "|" .. b .. "|" .. c .. "|" .. d
$$;
SELECT lua_esc();
SELECT lua_src('lua_esc');

-- backslashes survive standard_conforming_strings = off
SET standard_conforming_strings = off;
CREATE FUNCTION lua_bs() RETURNS text LANGUAGE plxlua AS $$ return [[C:\dir\]] $$;
RESET standard_conforming_strings;
SELECT lua_bs();

CREATE FUNCTION lua_nums() RETURNS text LANGUAGE plxlua AS $$
return 0xFF .. " " .. 1e3 .. " " .. .5 .. " " .. 3. .. " " .. 3000000000
$$;
SELECT lua_nums();

-- ================================================================ scoping

-- locals are block scoped: a shadowing local, a same-scope redeclaration of
-- another type, and a name that differs only in case get their own variable
CREATE FUNCTION lua_scopes(n int) RETURNS text LANGUAGE plxlua AS $$
local x = 1
local out = ""
do
  local x = "inner"
  out = out .. x
end
for i = 1, 2 do
  local x = i * 10
  out = out .. "," .. x
end
local X = 5
local n = n * 100
local y = 1
local y = "now text"
return out .. "," .. x .. "," .. X .. "," .. n .. "," .. y
$$;
SELECT lua_scopes(3);
SELECT lua_src('lua_scopes');

-- a local in a loop body is nil again on every iteration
CREATE FUNCTION lua_fresh() RETURNS text LANGUAGE plxlua AS $$
local out = ""
for i = 1, 3 do
  local seen --:: int
  if i == 2 then seen = i end
  out = out .. tostring(seen ~= nil)
end
return out
$$;
SELECT lua_fresh();

-- names plpgsql cannot take as variables are renamed
CREATE FUNCTION lua_reserved() RETURNS text LANGUAGE plxlua AS $$
local user = "u"
local left, right = 1, 2
local limit = 10
for loop = 1, 2 do limit = limit + loop end
return user .. left .. right .. limit
$$;
SELECT lua_reserved();
SELECT lua_src('lua_reserved');

-- an assignment to an undeclared name makes a function-wide local
CREATE FUNCTION lua_global(n int) RETURNS int LANGUAGE plxlua AS $$
if n > 0 then
  result = n * 2
else
  result = 0
end
return result
$$;
SELECT lua_global(4);

-- multiple assignment evaluates every value first
CREATE FUNCTION lua_swap(a int, b int) RETURNS text LANGUAGE plxlua AS $$
local x, y = a, b
x, y = y, x
local p, q = 1 --:: int
local t = {1, 2}
t[1], t[2] = 10, 20
return x .. "," .. y .. "," .. p .. "," .. tostring(q == nil) .. "," .. t[1] + t[2]
$$;
SELECT lua_swap(1, 2);
SELECT lua_src('lua_swap');

-- ================================================================ string building

-- s = s .. e on a string local appends through plx_strbuild
CREATE FUNCTION lua_build(n int) RETURNS text LANGUAGE plxlua AS $$
local s = ""
for i = 1, n do
  s = s .. i .. ","
end
return s
$$;
SELECT lua_build(5);
SELECT lua_src('lua_build');

-- ================================================================ SQL access

CREATE FUNCTION lua_qsum(g int) RETURNS bigint LANGUAGE plxlua AS $$
local total = 0 --:: bigint
for row in query("SELECT amount FROM lua_orders WHERE grp = $1", g) do
  total = total + row.amount
end
return total
$$;
SELECT lua_qsum(1), lua_qsum(2), lua_qsum(3), lua_qsum(4);
SELECT lua_src('lua_qsum');

-- static SQL (no bind arguments) refers to variables by name
CREATE FUNCTION lua_qstatic(g int) RETURNS bigint LANGUAGE plxlua AS $$
local total = 0 --:: bigint
for row in query("SELECT amount FROM lua_orders WHERE grp = g AND amount IS NOT NULL") do
  total = total + row.amount
end
return total
$$;
SELECT lua_qstatic(1);
SELECT lua_src('lua_qstatic');

-- dynamic SQL built with .. and quote_ident
CREATE FUNCTION lua_dyn(tbl text) RETURNS bigint LANGUAGE plxlua AS $$
local sql = "SELECT count(*) AS c FROM " .. quote_ident(tbl)
local n = 0 --:: bigint
for r in query(sql) do n = r.c end
local r2 = fetch_one(sql)
return n + r2.c
$$;
SELECT lua_dyn('lua_emp');

-- fetch_one returns an all-NULL row when nothing matches; fetch_one! raises.
-- found() follows static SQL; after a call with bind arguments (EXECUTE),
-- row_count() tells
CREATE FUNCTION lua_fetch(k int) RETURNS text LANGUAGE plxlua AS $$
local e = fetch_one("SELECT name FROM lua_emp WHERE id = k")
if not found() then return "none" end
local e2 = fetch_one("SELECT name FROM lua_emp WHERE id = $1", k)
local n = row_count()
return e.name .. " " .. e2.name .. " " .. n
$$;
SELECT lua_fetch(1), lua_fetch(9);
CREATE FUNCTION lua_fetchx(k int) RETURNS text LANGUAGE plxlua AS $$
local e --:: lua_emp%ROWTYPE
e = fetch_one!("SELECT * FROM lua_emp WHERE id = $1", k)
local s --:: lua_emp.sal%TYPE
s = e.sal * 2
return e.name .. ":" .. s
$$;
SELECT lua_fetchx(2);
SELECT lua_fetchx(9);
SELECT lua_src('lua_fetchx');

-- execute, perform, row_count
CREATE FUNCTION lua_dml(note text) RETURNS bigint LANGUAGE plxlua AS $$
execute("INSERT INTO lua_log(msg) VALUES ($1)", note .. "!")
perform([[INSERT INTO lua_log(msg) VALUES ('second')]])
perform("SELECT 1")
execute("UPDATE lua_log SET msg = upper(msg) WHERE msg = $1", note .. "!")
local n = row_count()
return n
$$;
SELECT lua_dml('first');
SELECT msg FROM lua_log ORDER BY id;
SELECT lua_src('lua_dml');

-- cursors
CREATE FUNCTION lua_cursor() RETURNS bigint LANGUAGE plxlua AS $$
local total = 0 --:: bigint
local c = open_cursor("SELECT sal FROM lua_emp ORDER BY id")
local row = fetch_from(c)
while found() do
  total = total + row.sal
  row = fetch_from(c)
end
close_cursor(c)
local c2 = open_cursor("SELECT id FROM lua_emp ORDER BY id")
move_cursor(c2)
local r = fetch_from(c2)
close_cursor(c2)
return total + r.id
$$;
SELECT lua_cursor();
SELECT lua_src('lua_cursor');

-- any other function call is run with PERFORM
CREATE FUNCTION lua_perfcall() RETURNS text LANGUAGE plxlua AS $$
set_config("application_name", "plxlua_test", true)
return current_setting("application_name")
$$;
SELECT lua_perfcall();

-- procedures: call(), commit()
CREATE PROCEDURE lua_proc_inner(m text) LANGUAGE plxlua AS $$
execute("INSERT INTO lua_log(msg) VALUES ($1)", m)
$$;
CREATE PROCEDURE lua_proc(m text) LANGUAGE plxlua AS $$
call("lua_proc_inner", m)
commit()
$$;
CALL lua_proc('from a procedure');
SELECT count(*) FROM lua_log WHERE msg = 'from a procedure';

-- ================================================================ results

CREATE FUNCTION lua_squares(n int) RETURNS SETOF int LANGUAGE plxlua AS $$
for i = 1, n do
  emit(i * i)
end
return
$$;
SELECT * FROM lua_squares(4);

CREATE FUNCTION lua_table_fn(n int) RETURNS TABLE(k int, label text) LANGUAGE plxlua AS $$
for i = 1, n do
  emit(i, "item " .. i)
end
$$;
SELECT * FROM lua_table_fn(3);
SELECT lua_src('lua_table_fn');

CREATE FUNCTION lua_rows() RETURNS SETOF lua_emp LANGUAGE plxlua AS $$
for r in query("SELECT * FROM lua_emp ORDER BY id") do
  return_next(r)
end
$$;
SELECT * FROM lua_rows();

CREATE FUNCTION lua_rq(minsal numeric) RETURNS SETOF lua_emp LANGUAGE plxlua AS $$
return_query("SELECT * FROM lua_emp WHERE sal >= $1 ORDER BY id", minsal)
$$;
SELECT * FROM lua_rq(150);

-- return with OUT parameters assigns them
CREATE FUNCTION lua_divmod(a int, b int, OUT q int, OUT r int) LANGUAGE plxlua AS $$
return a // b, a - (a // b) * b
$$;
SELECT * FROM lua_divmod(17, 5);
SELECT * FROM lua_divmod(-7, 2);
CREATE FUNCTION lua_outswap(INOUT a int, INOUT b int) LANGUAGE plxlua AS $$
return b, a
$$;
SELECT * FROM lua_outswap(1, 2);

-- ================================================================ errors

CREATE FUNCTION lua_err(n int) RETURNS int LANGUAGE plxlua AS $$
if n < 0 then error("negative: " .. n) end
if n == 0 then error("zero", 2) end
if n > 100 then
  error{message = "too big", sqlstate = "22023", detail = "n is " .. n, hint = "use 100 or less"}
end
return n
$$;
SELECT lua_err(5);
SELECT lua_err(-1);
SELECT lua_err(0);
SELECT lua_caught('SELECT lua_err(500)');

CREATE FUNCTION lua_msgs(n int) RETURNS int LANGUAGE plxlua AS $$
print("n is", n, nil)
print()
raise("warning", "careful: " .. n)
assert(n > 0, "n must be positive, got " .. n)
return n
$$;
SET client_min_messages = notice;
SELECT lua_msgs(3);
SELECT lua_msgs(-1);
SELECT lua_msgs(NULL);
SET client_min_messages = warning;
SELECT lua_src('lua_msgs');

-- pcall(function() ... end): ok, err; err.sqlstate and the other fields
CREATE FUNCTION lua_safediv(a int, b int) RETURNS int LANGUAGE plxlua AS $$
local r --:: int
local ok, err = pcall(function()
  r = a // b
end)
if not ok then
  return -1
end
return r
$$;
SELECT lua_safediv(10, 3), lua_safediv(1, 0), lua_safediv(NULL, 2);
SELECT lua_src('lua_safediv');

CREATE TABLE lua_uniq(id int PRIMARY KEY);
CREATE FUNCTION lua_ins(k int) RETURNS text LANGUAGE plxlua AS $$
local ok, err = pcall(function()
  execute("INSERT INTO lua_uniq VALUES ($1)", k)
end)
if ok then return "inserted" end
if err.sqlstate == "23505" then
  return "duplicate on " .. err.constraint .. ": " .. err
end
error(err)
$$;
SELECT lua_ins(1), lua_ins(1);
SELECT lua_src('lua_ins');

-- error(err) raises the caught error again with its SQLSTATE, detail and hint
CREATE FUNCTION lua_rethrow() RETURNS text LANGUAGE plxlua AS $$
local ok, err = pcall(function()
  error{message = "bad input", sqlstate = "22023", hint = "try again"}
end)
if err.sqlstate ~= "22023" then return "other" end
error(err)
$$;
SELECT lua_caught('SELECT lua_rethrow()');
SELECT lua_caught('SELECT lua_ins(NULL)');

-- if not pcall(...) then, a pcall statement, return inside the function,
-- and the database work of a failed call is rolled back
CREATE FUNCTION lua_pcforms(n int) RETURNS text LANGUAGE plxlua AS $$
local out = "start"
if not pcall(function() perform("SELECT 1/0") end) then
  out = out .. ",caught"
end
pcall(function() execute("INSERT INTO lua_uniq VALUES ($1)", 1) end)
local ok = pcall(function()
  if n > 0 then return end
  execute("INSERT INTO lua_uniq VALUES ($1)", 100)
  error("negative")
end)
return out .. "," .. tostring(ok)
$$;
SELECT lua_pcforms(1), lua_pcforms(-1);
SELECT count(*) AS rolled_back FROM lua_uniq WHERE id = 100;
SELECT lua_src('lua_pcforms');

-- ================================================================ triggers

CREATE TABLE lua_items(id int, tag text, qty int);
CREATE FUNCTION lua_stamp() RETURNS trigger LANGUAGE plxlua AS $$
if TG_OP == "INSERT" then
  NEW.tag = "row " .. NEW.id
elseif NEW.qty < 0 then
  error("negative quantity for " .. NEW.id)
end
return NEW
$$;
CREATE TRIGGER lua_items_stamp BEFORE INSERT OR UPDATE ON lua_items
  FOR EACH ROW EXECUTE FUNCTION lua_stamp();
INSERT INTO lua_items VALUES (1, NULL, 5), (2, 'x', 3);
SELECT * FROM lua_items ORDER BY id;
UPDATE lua_items SET qty = -1 WHERE id = 1;
SELECT lua_src('lua_stamp');

-- ================================================================ DO blocks

SET client_min_messages = notice;
DO LANGUAGE plxlua $$
local s = 0
for _, v in ipairs({1, 2, 3}) do s = s + v end
print("sum", s)
$$;
SET client_min_messages = warning;

-- ================================================================ ipairs

CREATE FUNCTION lua_ipairs(a int[]) RETURNS text LANGUAGE plxlua AS $$
local total, idx = 0, 0
for i, v in ipairs(a) do
  total = total + v
  idx = idx + i
end
local names = {"x", "y"}
local s = ""
for _, nm in ipairs(names) do s = s .. nm end
local cnt = 0
for i in ipairs(a) do cnt = cnt + 1 end
local u = {} --:: text[]
for _, w in ipairs(u) do --:: text
  s = s .. w
end
return total .. "/" .. idx .. "/" .. s .. "/" .. cnt
$$;
SELECT lua_ipairs(ARRAY[1, 2, 3]);
SELECT lua_src('lua_ipairs');

-- ================================================================ rejected

\set VERBOSITY terse
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ local function f() return 1 end return f() $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ local f = function() return 1 end return 1 $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ local t = {a = 1} return 1 $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ for k, v in pairs(x) do end return 1 $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ goto skip return 1 ::skip:: return 2 $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ ::top:: return 1 $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ return ("abc"):find("b") $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ return 1, 2 $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ local x = setmetatable({}, {}) return 1 $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ local co = coroutine.create(1) return 1 $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ local x = ... return 1 $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ break $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ while true do local ok = pcall(function() break end) end $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ local x = pcall(f) return 1 $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ local ok = pcall(function() return 1 end) return 1 $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ local x return x $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ local r = fetch_one("select 1 as a") if r then return 1 end return 0 $$;
CREATE FUNCTION lua_bad() RETURNS SETOF int LANGUAGE plxlua AS $$ return 5 $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ emit(1) $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ return string.format("%q", "a") $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ local k <const> = 1 k = 2 return k $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ local a <close> = 1 return 1 $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ for i = 1, 10, 0 do end return 1 $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ return query("select 1") $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ return os.time() $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ return "a\q" $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ return "a\0b" $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ return x != y $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ local __plx_x = 1 return 1 $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ if x then return 1 $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ end $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ x, t.y = 1, x $$;
CREATE PROCEDURE lua_badp() LANGUAGE plxlua AS $$ return 1 $$;
-- hostile input: unterminated strings and comments, deep nesting, long chains
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ return "unterminated $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ return [==[ never closed ]=] $$;
CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS $$ --[[ never closed
return 1 $$;
DO $$ BEGIN EXECUTE 'CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS '
  || quote_literal(repeat('if x then ', 3000) || 'y = 1 ' || repeat('end ', 3000)); END $$;
DO $$ BEGIN EXECUTE 'CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS '
  || quote_literal('return ' || repeat('(', 5000) || '1' || repeat(')', 5000)); END $$;
DO $$ BEGIN EXECUTE 'CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS '
  || quote_literal('return 1' || repeat(' == 1', 5000)); END $$;
DO $$ BEGIN EXECUTE 'CREATE FUNCTION lua_bad() RETURNS int LANGUAGE plxlua AS '
  || quote_literal('return [' || repeat('=', 50000) || '[x]' || repeat('=', 49999) || ']'); END $$;
\set VERBOSITY default
-- a long left-associative chain is fine, and its text stays linear
DO $$ BEGIN EXECUTE 'CREATE FUNCTION lua_longchain() RETURNS bigint LANGUAGE plxlua AS '
  || quote_literal('return 0' || repeat(' + 1', 20000)); END $$;
SELECT length(prosrc) < 300000 AS linear FROM pg_proc WHERE proname = 'lua_longchain';
