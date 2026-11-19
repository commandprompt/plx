-- Regression tests for bugs found by the 2026-11-19 audit (fix_rubypy).
-- Each case names its audit finding (A<n>).
-- A20: an integer for-loop variable keeps its last value after the loop
SET client_min_messages = warning;
CREATE FUNCTION fr_a20_py_break() RETURNS int LANGUAGE plxpython3 AS $$
i = 100
for i in range(10):
    if i * i > 20:
        break
return i
$$;
SELECT fr_a20_py_break() AS py_expect_5;
CREATE FUNCTION fr_a20_py_done() RETURNS int LANGUAGE plxpython3 AS $$
i = 0
for i in range(5):
    pass
return i
$$;
SELECT fr_a20_py_done() AS py_expect_4;
CREATE FUNCTION fr_a20_py_after() RETURNS int LANGUAGE plxpython3 AS $$
for k in range(2, 12, 3):
    pass
return k
$$;
SELECT fr_a20_py_after() AS py_expect_11;
CREATE FUNCTION fr_a20_py_empty() RETURNS int LANGUAGE plxpython3 AS $$
i = 7
for i in range(0):
    pass
return i
$$;
SELECT fr_a20_py_empty() AS py_expect_7;
CREATE FUNCTION fr_a20_py_param(n int) RETURNS int LANGUAGE plxpython3 AS $$
for n in range(n):
    pass
return n
$$;
SELECT fr_a20_py_param(3) AS py_expect_2;
CREATE FUNCTION fr_a20_rb_done() RETURNS int LANGUAGE plxruby AS $$
i = 0
for i in 1..5
  x = 1
end
return i
$$;
SELECT fr_a20_rb_done() AS rb_expect_5;
CREATE FUNCTION fr_a20_rb_break() RETURNS int LANGUAGE plxruby AS $$
i = 100
for i in 0...10 do
  break if i * i > 20
end
return i
$$;
SELECT fr_a20_rb_break() AS rb_expect_5;
-- a loop variable used only inside the loop still compiles to a plain FOR
CREATE FUNCTION fr_a20_py_local() RETURNS int LANGUAGE plxpython3 AS $$
t = 0
for j in range(4):
    t = t + j
return t
$$;
SELECT fr_a20_py_local() AS py_expect_6;
SELECT prosrc ~ 'FOR j IN 0' AS plain_for FROM pg_proc WHERE proname = 'fr_a20_py_local';
-- A21: plxpython3 except with an unknown class is rejected, not turned into WHEN OTHERS
\set VERBOSITY terse
CREATE FUNCTION fr_a21_unknown() RETURNS text LANGUAGE plxpython3 AS $$
try:
    perform("SELECT 1/0")
    return "ok"
except KeyError:
    return "caught by KeyError handler"
$$;
CREATE FUNCTION fr_a21_typo() RETURNS text LANGUAGE plxpython3 AS $$
try:
    perform("SELECT 1/0")
    return "ok"
except PG::UniqueVoilation as e:
    return "typo caught"
$$;
CREATE FUNCTION fr_a21_tuple_miss() RETURNS text LANGUAGE plxpython3 AS $$
try:
    perform("SELECT 1/0")
    return "ok"
except (PG::UniqueViolation, PG::CheckViolation) as e:
    return f"caught: {e.message}"
$$;
SELECT fr_a21_tuple_miss();
CREATE FUNCTION fr_a21_tuple_hit() RETURNS text LANGUAGE plxpython3 AS $$
try:
    perform("SELECT 1/0")
    return "ok"
except (PG::UniqueViolation, ZeroDivisionError) as e:
    return f"caught: {e.message}"
$$;
SELECT fr_a21_tuple_hit();
CREATE FUNCTION fr_a21_tuple_bad() RETURNS text LANGUAGE plxpython3 AS $$
try:
    return "ok"
except (PG::UniqueViolation, KeyError):
    return "caught"
$$;
CREATE FUNCTION fr_a21_exception() RETURNS text LANGUAGE plxpython3 AS $$
try:
    perform("SELECT 1/0")
    return "ok"
except Exception as e:
    return f"caught: {e.message}"
$$;
SELECT fr_a21_exception();
CREATE FUNCTION fr_a21_bare() RETURNS text LANGUAGE plxpython3 AS $$
try:
    perform("SELECT 1/0")
    return "ok"
except:
    return "caught"
$$;
SELECT fr_a21_bare();
\set VERBOSITY default
-- A24: plxruby each_with_index over an array or a range sets the index
CREATE FUNCTION fr_a24_array() RETURNS int LANGUAGE plxruby AS $$
i = 42
total = 0
v #:: int
[10, 20, 30].each_with_index do |v, i|
  total = total + i
end
return total
$$;
SELECT fr_a24_array() AS rb_expect_3;
CREATE FUNCTION fr_a24_range() RETURNS int LANGUAGE plxruby AS $$
total = 0
(5..7).each_with_index do |v, i|
  total = total + v * 10 + i
end
return total
$$;
SELECT fr_a24_range() AS rb_expect_183;
\set VERBOSITY terse
CREATE FUNCTION fr_a24_noidx() RETURNS int LANGUAGE plxruby AS $$
total = 0
(5..7).each_with_index do |v|
  total = total + v
end
return total
$$;
CREATE FUNCTION fr_a24_each2() RETURNS int LANGUAGE plxruby AS $$
total = 0
(5..7).each do |v, i|
  total = total + i
end
return total
$$;
\set VERBOSITY default
