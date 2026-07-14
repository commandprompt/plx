-- plx transpiler-output tests: assert the exact plpgsql each dialect construct
-- lowers to, independent of execution. Catches a wrong lowering that happens to
-- produce the right runtime result for a sample input.
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;

-- normalize prosrc: drop the embedded original and the body hash, leaving the
-- sentinel and the generated plpgsql.
CREATE FUNCTION _src(nm text) RETURNS text LANGUAGE sql AS $$
  SELECT regexp_replace(
           regexp_replace(prosrc, '/\*plx-orig:[^*]*\*/\s*', '', 'g'),
           'plx:v1:([a-z0-9]+):[0-9a-f]+', 'plx:v1:\1')
  FROM pg_proc WHERE proname = nm;
$$;

-- ruby: if/elsif/else + type annotation + constant fold
CREATE FUNCTION o_rb_if(score int) RETURNS text LANGUAGE plxruby AS $$
grade #:: text
if score >= 90
  grade = "A"
elsif score >= 80
  grade = "B"
else
  grade = "F"
end
return grade
$$;
SELECT _src('o_rb_if');

-- ruby: inclusive vs exclusive range, next/break -> WHEN
CREATE FUNCTION o_rb_for(n int) RETURNS int LANGUAGE plxruby AS $$
t = 0
for i in 1...n
  next if i == 2
  t = t + i
end
return t
$$;
SELECT _src('o_rb_for');

-- ruby: interpolation (E-string, apostrophe, numeric cast), ternary
CREATE FUNCTION o_rb_expr(n int) RETURNS text LANGUAGE plxruby AS $$
return n >= 0 ? "it's #{n}\n" : "neg"
$$;
SELECT _src('o_rb_expr');

-- ruby: == maps to plain SQL = (three-valued, not null-aware)
CREATE FUNCTION o_rb_null(a int, b int) RETURNS boolean LANGUAGE plxruby AS $$
return a == b
$$;
SELECT _src('o_rb_null');

-- ruby: case/when (simple) -> CASE
CREATE FUNCTION o_rb_case(n int) RETURNS text LANGUAGE plxruby AS $$
case n
when 1, 2
  return "low"
else
  return "hi"
end
$$;
SELECT _src('o_rb_case');

-- ruby: query iteration + fetch + raise with options
CREATE FUNCTION o_rb_data(g int) RETURNS bigint LANGUAGE plxruby AS $$
t = 0 #:: bigint
query("SELECT amt FROM x WHERE g = #{g}").each do |row|
  t = t + row.amt
end
raise exception: "done #{t}", errcode: "22023"
return t
$$;
SELECT _src('o_rb_data');

-- php: switch -> CASE, concat -> ||, interpolation
CREATE FUNCTION o_php_switch(n int) RETURNS text LANGUAGE plxphp AS $$
switch ($n) {
  case 1: return "a" . "b";
  default: return "z {$n}";
}
$$;
SELECT _src('o_php_switch');

-- php: counting for -> integer FOR
CREATE FUNCTION o_php_for(n int) RETURNS int LANGUAGE plxphp AS $$
$t = 0;
for ($i = 1; $i < $n; $i++) { $t = $t + $i; }
return $t;
$$;
SELECT _src('o_php_for');

-- js: template literal, for-of, ===
CREATE FUNCTION o_js(n int) RETURNS int LANGUAGE plxjs AS $$
let t = 0;
for (const r of query(`SELECT v FROM x WHERE g = ${n}`)) {
  if (r.v === 0) { continue; }
  t = t + r.v;
}
return t;
$$;
SELECT _src('o_js');

-- python: if/elif, range, f-string
CREATE FUNCTION o_py(n int) RETURNS text LANGUAGE plxpython3 AS $$
t = 0
for i in range(n):
    t = t + i
if t == 0:
    return "zero"
return f"sum {t}"
$$;
SELECT _src('o_py');

-- python: try/except with diagnostics, is None
CREATE FUNCTION o_py_try(x int) RETURNS text LANGUAGE plxpython3 AS $$
if x is None:
    return "null"
try:
    perform("INSERT INTO x VALUES (1)")
except Exception as e:
    return f"err {e.message}"
return "ok"
$$;
SELECT _src('o_py_try');

-- interpolation of a NULL value must render as empty string, not annihilate the
-- whole string (COALESCE guard)
CREATE FUNCTION o_interp_null(x int) RETURNS text LANGUAGE plxruby AS $$
return "val=#{x} end"
$$;
SELECT _src('o_interp_null');

-- literal nil/null/None comparison becomes IS NULL; a value comparison stays =
CREATE FUNCTION o_cmp_null(x int) RETURNS text LANGUAGE plxruby AS $$
if x == nil
  return "a"
end
if x != nil
  return "b"
end
return "c"
$$;
SELECT _src('o_cmp_null');

-- python: both `is None` and `== None` lower to IS NULL
CREATE FUNCTION o_py_none(x int) RETURNS boolean LANGUAGE plxpython3 AS $$
return x == None
$$;
SELECT _src('o_py_none');

-- js: === and !== map to plain = / <> (not type-strict)
CREATE FUNCTION o_js_strict(a int, b int) RETURNS boolean LANGUAGE plxjs AS $$
return a === b;
$$;
SELECT _src('o_js_strict');

-- integer division and modulo pass through to SQL operators
CREATE FUNCTION o_intdiv(a int, b int) RETURNS text LANGUAGE plxruby AS $$
return "#{a / b}:#{a % b}"
$$;
SELECT _src('o_intdiv');

-- a SQL escape-string literal passed through is not re-quoted
CREATE FUNCTION o_estring() RETURNS text LANGUAGE plxruby AS $$
return E'tab\there'
$$;
SELECT _src('o_estring');
