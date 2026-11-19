-- Regression tests for bugs found by the 2026-11-19 audit (fix_engine).
-- Each case names its audit finding (A<n>).
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;

-- A5: names that differ only by case are rejected, not merged into one variable
CREATE FUNCTION fe_a5_param_rb(n int) RETURNS int LANGUAGE plxruby AS $$
N = 10
return n
$$;
CREATE FUNCTION fe_a5_local_js() RETURNS int LANGUAGE plxjs AS $$
let a = 1;
let A = 2;
return a;
$$;
CREATE FUNCTION fe_a5_local_py() RETURNS int LANGUAGE plxpython3 AS $$
a = 1
A = 2
return a
$$;
CREATE FUNCTION fe_a5_param_php(x int) RETURNS int LANGUAGE plxphp AS $$
$X = 5;
return $x;
$$;
-- A5: one consistent spelling of a parameter that SQL folded to lower case still works
CREATE FUNCTION fe_a5_camel_js(userId int) RETURNS int LANGUAGE plxjs AS $$
userId = userId + 1;
return userId;
$$;
SELECT fe_a5_camel_js(1);

-- A5: a variable read with a second spelling is rejected, not read as the first
CREATE FUNCTION fe_a5_ref_js() RETURNS int LANGUAGE plxjs AS $$
let a = 1;
return A + 1;
$$;
-- A5: names that differ only after byte 63 are the same name to plpgsql
CREATE FUNCTION fe_a5_long_js() RETURNS int LANGUAGE plxjs AS $$
let abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcX = 1;
let abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcY = 2;
return abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcX;
$$;

-- A5: a counting-loop variable spelled like an earlier local or parameter is rejected
CREATE FUNCTION fe_a5_loop_js() RETURNS int LANGUAGE plxjs AS $$
let i = 100;
let s = 0;
for (let I = 1; I <= 3; I++) { s += i; }
return s;
$$;
CREATE FUNCTION fe_a5_loop_py() RETURNS int LANGUAGE plxpython3 AS $$
i = 100
s = 0
for I in range(3):
    s += i
return s
$$;
CREATE FUNCTION fe_a5_loop_rb(n int) RETURNS int LANGUAGE plxruby AS $$
s = 0
for N in 1..3
  s += n
end
return s
$$;
CREATE FUNCTION fe_a5_loop_go() RETURNS int LANGUAGE plxgo AS $$
i := 100
s := 0
for I := 1; I <= 3; I++ {
	s += i
}
return s
$$;
-- A5: a TypeScript type after "as" is not a variable
CREATE FUNCTION fe_a5_as_ts(x int) RETURNS int LANGUAGE plxts AS $$
let num: number = x as Num;
return num;
$$;
SELECT fe_a5_as_ts(4);
-- A6: an integer literal with a leading zero is octal in Ruby, PHP and JS
CREATE FUNCTION fe_a6_rb() RETURNS int LANGUAGE plxruby AS $$
x = 0755
return x + 0_17
$$;
SELECT fe_a6_rb();
CREATE FUNCTION fe_a6_php() RETURNS int LANGUAGE plxphp AS $$
$x = 0755;
return $x;
$$;
SELECT fe_a6_php();
CREATE FUNCTION fe_a6_js() RETURNS int LANGUAGE plxjs AS $$
return 0755;
$$;
SELECT fe_a6_js();
-- A6: a leading zero does not make a decimal fraction octal
CREATE FUNCTION fe_a6_float_rb() RETURNS numeric LANGUAGE plxruby AS $$
return 00.5 + 0
$$;
SELECT fe_a6_float_rb();
-- A6: COBOL numbers are decimal, leading zeros included
CREATE FUNCTION fe_a6_cob() RETURNS int LANGUAGE plxcobol AS $$
       PROCEDURE DIVISION.
           RETURN 0755.
$$;
SELECT fe_a6_cob();
-- A6: Python and TypeScript reject a leading zero; 8 and 9 are not octal digits
CREATE FUNCTION fe_a6_py() RETURNS int LANGUAGE plxpython3 AS $$
return 0755
$$;
CREATE FUNCTION fe_a6_ts() RETURNS int LANGUAGE plxts AS $$
return 0755;
$$;
CREATE FUNCTION fe_a6_bad_rb() RETURNS int LANGUAGE plxruby AS $$
return 0789
$$;

-- A6: Go integer literals keep their base: 0755 and 0o755 are octal
CREATE FUNCTION fe_a6_go() RETURNS text LANGUAGE plxgo AS $$
a := 0755
var b int = 0o17 + 0x1F + 0b101 + 1_000
var c float64 = 00.5 + 0.5
return fmt.Sprintf("%d %d %v", a, b, c)
$$;
SELECT fe_a6_go();
CREATE FUNCTION fe_a6_go_bad() RETURNS int LANGUAGE plxgo AS $$
return 0789
$$;
-- A7: ^ is bitwise XOR in Ruby, PHP, JS, TS and Python, not SQL exponent
CREATE FUNCTION fe_a7_xor_rb(a int, b int) RETURNS int LANGUAGE plxruby AS $$
return a ^ b
$$;
SELECT fe_a7_xor_rb(6, 3);
CREATE FUNCTION fe_a7_xor_js() RETURNS int LANGUAGE plxjs AS $$
return 5 ^ 3;
$$;
SELECT fe_a7_xor_js();
-- A7: Go ^ is XOR or NOT, not a power: plxgo rejects it
CREATE FUNCTION fe_a7_xor_go(a int, b int) RETURNS int LANGUAGE plxgo AS $$
return a ^ b
$$;
CREATE FUNCTION fe_a7_andnot_go(a int, b int) RETURNS int LANGUAGE plxgo AS $$
a &^= b
return a
$$;
-- A7: SQL gives ^, &, | and the shifts one precedence, so a mix that
-- the dialect would group differently is rejected instead of computed wrong
CREATE FUNCTION fe_a7_mix_py(a int, b int, c int) RETURNS int LANGUAGE plxpython3 AS $$
return a ^ b & c
$$;
CREATE FUNCTION fe_a7_mix_js(a int, b int) RETURNS int LANGUAGE plxjs AS $$
return a ^ b << 2;
$$;
CREATE FUNCTION fe_a7_mix_php(a int, b int, c int) RETURNS int LANGUAGE plxphp AS $$
return ($a | $b & $c) + 1;
$$;
-- A7: a mix that SQL already groups the same way still works
CREATE FUNCTION fe_a7_mixok_js(a int, b int, c int) RETURNS int LANGUAGE plxjs AS $$
return (a & b | c) * 100 + ((a ^ b) & c) * 10 + (a ^ (b << 1));
$$;
SELECT fe_a7_mixok_js(1, 3, 2);
CREATE FUNCTION fe_a7_mixok_rb(a int, b int, c int) RETURNS int LANGUAGE plxruby AS $$
return a | b ^ c
$$;
SELECT fe_a7_mixok_rb(1, 2, 3);
-- A7: ** is right-associative and binds tighter than unary minus
CREATE FUNCTION fe_a7_pow_py() RETURNS numeric LANGUAGE plxpython3 AS $$
return 2 ** 3 ** 2
$$;
SELECT fe_a7_pow_py();
CREATE FUNCTION fe_a7_powneg_rb() RETURNS numeric LANGUAGE plxruby AS $$
return -2 ** 2
$$;
SELECT fe_a7_powneg_rb();
CREATE FUNCTION fe_a7_pow_js(x int) RETURNS numeric LANGUAGE plxjs AS $$
return (x + 1) ** 2 * 2 + abs(-x) ** 2 ** 1;
$$;
SELECT fe_a7_pow_js(2);
CREATE FUNCTION fe_a7_pow_php() RETURNS numeric LANGUAGE plxphp AS $$
return 2 ** -1;
$$;
SELECT fe_a7_pow_php();

-- A7: a power inside a call or a parenthesized receiver stays inside it
CREATE FUNCTION fe_a7_powcall_py(x int) RETURNS text LANGUAGE plxpython3 AS $$
return f"{int(x ** 0.5)} {str(x ** 2)} {float(x ** 2)}"
$$;
SELECT fe_a7_powcall_py(17);
CREATE FUNCTION fe_a7_powrecv_rb(x int) RETURNS int LANGUAGE plxruby AS $$
return (x ** 2).to_i + (x + 1) * (2 ** x)
$$;
SELECT fe_a7_powrecv_rb(3);
CREATE FUNCTION fe_a7_powarg_js(x int) RETURNS numeric LANGUAGE plxjs AS $$
return abs(-(x ** 2)) + greatest(1, x ** 3);
$$;
SELECT fe_a7_powarg_js(2);
-- A8: chained assignment a = b = v is rejected, not compiled to a := (b = v)
CREATE FUNCTION fe_a8_js() RETURNS text LANGUAGE plxjs AS $$
let a = "x";
let b = "y";
a = b = "z";
return `${a},${b}`;
$$;
CREATE FUNCTION fe_a8_py() RETURNS int LANGUAGE plxpython3 AS $$
a = 0
b = 0
a = b = 1
return a + b
$$;
CREATE FUNCTION fe_a8_rb() RETURNS int LANGUAGE plxruby AS $$
a = 0
b = 0
a += b = 1
return a + b
$$;
-- A8: a chained assignment to a field or element target is rejected too
CREATE FUNCTION fe_a8_field_js() RETURNS trigger LANGUAGE plxjs AS $$
let b = 0;
NEW.x = b = 1;
return NEW;
$$;
-- A8: a compound assignment such as <<=, &= or ??= inside a right-hand side is rejected too
CREATE FUNCTION fe_a8_shift_js() RETURNS int LANGUAGE plxjs AS $$
let a = 0;
let b = 1;
a = b <<= 2;
return a;
$$;
CREATE FUNCTION fe_a8_and_js() RETURNS int LANGUAGE plxjs AS $$
let a = 0;
let b = 6;
a = b &= 3;
return a;
$$;
CREATE FUNCTION fe_a8_nullish_js() RETURNS int LANGUAGE plxjs AS $$
let a = 0;
let b = 1;
a = b ??= 4;
return a;
$$;
-- A8: == in a right-hand side is a comparison, not an assignment
CREATE FUNCTION fe_a8_ok_rb(x int) RETURNS boolean LANGUAGE plxruby AS $$
ok = x == 1 #:: boolean
return ok
$$;
SELECT fe_a8_ok_rb(1);

-- A11: escape sequences keep their meaning: \x41 is A, \v is a vertical tab
CREATE FUNCTION fe_a11_js() RETURNS text LANGUAGE plxjs AS $$
return "[\x41][\v][\b][\f][\u0042][\u{e9}]";
$$;
SELECT encode(convert_to(fe_a11_js(), 'UTF8'), 'hex') AS js;
CREATE FUNCTION fe_a11_py() RETURNS text LANGUAGE plxpython3 AS $$
return "[\x41][\a][\101][\u00e9][\q]"
$$;
SELECT encode(convert_to(fe_a11_py(), 'UTF8'), 'hex') AS py;
CREATE FUNCTION fe_a11_rb() RETURNS text LANGUAGE plxruby AS $$
return "[\x41][\e][\s][\101][\u{41 42}][\q]"
$$;
SELECT encode(convert_to(fe_a11_rb(), 'UTF8'), 'hex') AS rb;
CREATE FUNCTION fe_a11_php() RETURNS text LANGUAGE plxphp AS $$
return "[\x41][\e][\101][\u{41}][\q][\a][\$x]";
$$;
SELECT encode(convert_to(fe_a11_php(), 'UTF8'), 'hex') AS php;
-- A11: a NUL character cannot be stored in text, so \0 is rejected
CREATE FUNCTION fe_a11_nul_js() RETURNS text LANGUAGE plxjs AS $$
return "a\0b";
$$;
CREATE FUNCTION fe_a11_nul_rb() RETURNS text LANGUAGE plxruby AS $$
return "a\0b"
$$;

-- A11: Go escapes keep their meaning too
CREATE FUNCTION fe_a11_go() RETURNS text LANGUAGE plxgo AS $$
return "[\x41][\101][\v][\u00e9][\U0001F600][\a]"
$$;
SELECT encode(convert_to(fe_a11_go(), 'UTF8'), 'hex') AS go;
CREATE FUNCTION fe_a11_nul_go() RETURNS text LANGUAGE plxgo AS $$
return "a\x00b"
$$;
CREATE FUNCTION fe_a11_bad_go() RETURNS text LANGUAGE plxgo AS $$
return "a\xffb"
$$;
-- A12: ? and : inside a template literal are text, not a ternary
CREATE FUNCTION fe_a12_js(x int) RETURNS text LANGUAGE plxjs AS $$
let s = `is it ${x}? yes: no`;
return s;
$$;
SELECT fe_a12_js(1);
CREATE FUNCTION fe_a12_ts() RETURNS text LANGUAGE plxts AS $$
return `a? b: c`;
$$;
SELECT fe_a12_ts();
-- A12: a real ternary with template-literal branches still works
CREATE FUNCTION fe_a12_tern_js(x int) RETURNS text LANGUAGE plxjs AS $$
return x > 0 ? `pos? ${x}` : `neg: ${x}`;
$$;
SELECT fe_a12_tern_js(2), fe_a12_tern_js(-2);

-- A22: adjacent string literals concatenate, as in Ruby and Python
CREATE FUNCTION fe_a22_rb() RETURNS text LANGUAGE plxruby AS $$
return "a""b"
$$;
SELECT fe_a22_rb();
CREATE FUNCTION fe_a22_py() RETURNS text LANGUAGE plxpython3 AS $$
return 'a''b' "c"
$$;
SELECT fe_a22_py();
-- A22: an escape string next to a raw string ending in a backslash stays closed
CREATE FUNCTION fe_a22_esc_rb() RETURNS text LANGUAGE plxruby AS $$
x = "\n"'x\\' #:: text
return x
$$;
SELECT replace(fe_a22_esc_rb(), E'\n', '|');
-- A22: a Python triple-quoted string is one literal
CREATE FUNCTION fe_a22_triple_py() RETURNS text LANGUAGE plxpython3 AS $$
return """hello"""
$$;
SELECT fe_a22_triple_py();
CREATE FUNCTION fe_a22_triple2_py(n int) RETURNS text LANGUAGE plxpython3 AS $$
s = f"""say "hi" {n}
it's '''here'''""" #:: text
return s
$$;
SELECT replace(fe_a22_triple2_py(1), E'\n', '|');
-- A22: line numbers after a multi-line triple-quoted string stay right
CREATE FUNCTION fe_a22_line_py() RETURNS int LANGUAGE plxpython3 AS $$
s = """a
b
c"""
break
return 1
$$;

-- A1: a typed array, varchar(n) or char(n) local keeps its type when appended to
CREATE FUNCTION fe_a1_push_rb(a text[]) RETURNS text[] LANGUAGE plxruby AS $$
b = a #:: text[]
b << 'y'
return b
$$;
SELECT fe_a1_push_rb('{x}');
CREATE FUNCTION fe_a1_vc_js() RETURNS text LANGUAGE plxjs AS $$
let s = "" /*:: varchar(3) */;
for (let i = 1; i <= 5; i++) { s += "z"; }
return s;
$$;
CREATE FUNCTION fe_a1_vc_php() RETURNS text LANGUAGE plxphp AS $$
$s = "" /*:: varchar(3) */;
for ($i = 1; $i <= 5; $i++) { $s .= "z"; }
return $s;
$$;
CREATE FUNCTION fe_a1_vc_rb() RETURNS text LANGUAGE plxruby AS $$
s = "" #:: character varying(3)
s << "ab"
s << "cd"
return s
$$;
CREATE FUNCTION fe_a1_ch_js() RETURNS text LANGUAGE plxjs AS $$
let s = "a" /*:: char(2) */;
s += "bc";
return s;
$$;
\set VERBOSITY terse
SELECT fe_a1_vc_js();
SELECT fe_a1_vc_php();
SELECT fe_a1_vc_rb();
SELECT fe_a1_ch_js();
\set VERBOSITY default
-- A1: plain text and varchar locals still use the string builder
CREATE FUNCTION fe_a1_vc_ok_js() RETURNS text LANGUAGE plxjs AS $$
let s = "" /*:: varchar */;
let t = "" /*:: text */;
for (let i = 1; i <= 5; i++) { s += "z"; t += i; }
return `${s}${t}`;
$$;
SELECT fe_a1_vc_ok_js();
SELECT proname, prosrc LIKE '%s public.plx_strbuild%' AS s_builder,
       prosrc LIKE '%t public.plx_strbuild%' AS t_builder
FROM pg_proc WHERE proname IN ('fe_a1_vc_ok_js', 'fe_a1_vc_js', 'fe_a1_ch_js')
ORDER BY proname;
-- A1: PHP .= on an array local is rejected instead of pushing an element
CREATE FUNCTION fe_a1_arr_php(a text[]) RETURNS text LANGUAGE plxphp AS $$
$b = $a /*:: text[] */;
$b .= "y";
return $b;
$$;
-- A2: ARRAY['x'] and jsonb subscripts are not rewritten to record field access
CREATE FUNCTION fe_a2_arr_js() RETURNS text[] LANGUAGE plxjs AS $$
return ARRAY['x'];
$$;
SELECT fe_a2_arr_js();
CREATE FUNCTION fe_a2_arr_rb() RETURNS text[] LANGUAGE plxruby AS $$
return array["x"]
$$;
SELECT fe_a2_arr_rb();
CREATE FUNCTION fe_a2_jsub_js(j jsonb) RETURNS jsonb LANGUAGE plxjs AS $$
return j['a'];
$$;
CREATE FUNCTION fe_a2_jsub_py(j jsonb) RETURNS jsonb LANGUAGE plxpython3 AS $$
d = j #:: jsonb
return d["a"]
$$;
SELECT proname, prosrc LIKE '%RETURN j[''a''];%' AS j_kept,
       prosrc LIKE '%RETURN d[''a''];%' AS d_kept
FROM pg_proc WHERE proname IN ('fe_a2_jsub_js', 'fe_a2_jsub_py') ORDER BY proname;
-- A2: a composite parameter and a query row still take field subscripts
CREATE TYPE fe_a2_pt AS (name text, "a b" int);
CREATE FUNCTION fe_a2_comp_js(r fe_a2_pt) RETURNS text LANGUAGE plxjs AS $$
return r['name'];
$$;
SELECT fe_a2_comp_js(ROW('bob', 1)::fe_a2_pt);
CREATE FUNCTION fe_a2_row_rb() RETURNS int LANGUAGE plxruby AS $$
query('SELECT 7 AS "a b"').each do |row|
  return row["a b"]
end
return 0
$$;
SELECT fe_a2_row_rb();
-- A2: a variable named array is not the ARRAY constructor
CREATE FUNCTION fe_a2_var_php() RETURNS text LANGUAGE plxphp AS $$
$array = fetch_one("SELECT 'bob'::text AS name");
return $array['name'];
$$;
CREATE FUNCTION fe_a2_var_rb() RETURNS text LANGUAGE plxruby AS $$
array = fetch_one("SELECT 'bob'::text AS name")
return array['name']
$$;
SELECT proname, prosrc LIKE '%RETURN array.name;%' AS field_read
FROM pg_proc WHERE proname IN ('fe_a2_var_php', 'fe_a2_var_rb') ORDER BY proname;
-- A10: PHP "$r->prop" and "$a[key]" interpolate the property or element
CREATE FUNCTION fe_a10_php(i int) RETURNS text LANGUAGE plxphp AS $$
$r = fetch_one("SELECT 'bob'::text AS name");
$a = ARRAY['x', 'y'] /*:: text[] */;
return "Hello $r->name! $r[name] $a[1] $a[$i] $r->name->x $r-> $a[";
$$;
SELECT fe_a10_php(2);
-- A18: a comment inside a multi-line expression is not copied into the SQL
CREATE FUNCTION fe_a18_php(a int) RETURNS int LANGUAGE plxphp AS $$
return $a # 3
  + 1;
$$;
SELECT fe_a18_php(10);
CREATE FUNCTION fe_a18_js(a int) RETURNS int LANGUAGE plxjs AS $$
return a + // first -- x
  1;
$$;
SELECT fe_a18_js(1);
CREATE FUNCTION fe_a18_ts(a int) RETURNS int LANGUAGE plxts AS $$
return a /* it's */ + 1;
$$;
SELECT fe_a18_ts(1);
CREATE FUNCTION fe_a18_py(a int) RETURNS int LANGUAGE plxpython3 AS $$
return (a  # it's
        + 1)
$$;
SELECT fe_a18_py(1);
CREATE FUNCTION fe_a18_rb(a int) RETURNS int LANGUAGE plxruby AS $$
return a +   # don't
  2
$$;
SELECT fe_a18_rb(1);
-- A18: comment markers inside strings are kept
CREATE FUNCTION fe_a18_str_js() RETURNS text LANGUAGE plxjs AS $$
return `a // b ${
  1}`;
$$;
SELECT fe_a18_str_js();
CREATE FUNCTION fe_a18_str_php() RETURNS text LANGUAGE plxphp AS $$
return "a # b /* c */";
$$;
SELECT fe_a18_str_php();
-- A23: int(x) and x.to_i truncate toward zero, as Python and Ruby do
CREATE FUNCTION fe_a23_py(x numeric) RETURNS int LANGUAGE plxpython3 AS $$
return int(x)
$$;
SELECT fe_a23_py(2.7) AS p27, fe_a23_py(-2.5) AS m25, fe_a23_py(-2.7) AS m27;
CREATE FUNCTION fe_a23_py_f(x float8, s text, b bool) RETURNS text LANGUAGE plxpython3 AS $$
return f"{int(x * 2)} {int(s)} {int(b)} {int(x > 1)} {int(True)} {int(x) + 1}"
$$;
SELECT fe_a23_py_f(1.4, '42', true);
CREATE FUNCTION fe_a23_rb(x numeric) RETURNS int LANGUAGE plxruby AS $$
return x.to_i
$$;
SELECT fe_a23_rb(2.7) AS p27, fe_a23_rb(-2.5) AS m25;
CREATE FUNCTION fe_a23_rb_f(x numeric, s text) RETURNS text LANGUAGE plxruby AS $$
return "#{(x * 2).to_i} #{s.to_i} #{'4.7'.to_i} #{x.to_s.to_i} #{1 + x.to_i} #{x.to_f.to_i} #{abs(x).to_i}"
$$;
SELECT fe_a23_rb_f(-1.4, '12');
-- A23: Python int() of a string does not accept a fraction, as in Python
CREATE FUNCTION fe_a23_str_py(s text) RETURNS text LANGUAGE plxpython3 AS $$
v = s #:: text
return f"{int(s)} {int(v)} {int('12')} {int(' 7 ')}"
$$;
SELECT fe_a23_str_py('42');
SELECT fe_a23_str_py('4.7');
CREATE FUNCTION fe_a23_lit_py() RETURNS int LANGUAGE plxpython3 AS $$
return int("4.7")
$$;
SELECT fe_a23_lit_py();
-- A23: .to_i and .to_f on an interpolated string convert the whole string
CREATE FUNCTION fe_a23_interp_rb(a int) RETURNS text LANGUAGE plxruby AS $$
return "#{"#{a}5".to_i * 2} #{"#{a}.5".to_f * 2}"
$$;
SELECT fe_a23_interp_rb(1);
-- A23: int() of a boolean plx cannot see fails; a comparison makes it visible
CREATE FUNCTION fe_a23_hb_py(s text) RETURNS int LANGUAGE plxpython3 AS $$
return int(starts_with(s, 'a'))
$$;
CREATE FUNCTION fe_a23_hb2_py(s text) RETURNS int LANGUAGE plxpython3 AS $$
return int(starts_with(s, 'a') == True)
$$;
\set VERBOSITY sqlstate
SELECT fe_a23_hb_py('abc');
\set VERBOSITY default
SELECT fe_a23_hb2_py('abc');
-- A25: a quote or brace inside an interpolated expression stays in the expression
CREATE FUNCTION fe_a25_py(x text) RETURNS text LANGUAGE plxpython3 AS $$
return f"{concat('}', x)}|{x == '{'}"
$$;
SELECT fe_a25_py('a');
CREATE FUNCTION fe_a25_rb(s text) RETURNS text LANGUAGE plxruby AS $$
return "#{s} is #{s == "a" ? "A" : "other"}, #{concat(s, "}")}"
$$;
SELECT fe_a25_rb('a'), fe_a25_rb('b');
CREATE FUNCTION fe_a25_rb_nest(s text) RETURNS text LANGUAGE plxruby AS $$
return "<#{concat("[#{s}]", '}')}>"
$$;
SELECT fe_a25_rb_nest('a');
CREATE FUNCTION fe_a25_js(x text) RETURNS text LANGUAGE plxjs AS $$
return `${concat(x, "}")}`;
$$;
SELECT fe_a25_js('a');
CREATE FUNCTION fe_a25_php() RETURNS text LANGUAGE plxphp AS $$
$r = fetch_one("SELECT 'bob'::text AS name");
return "{$r["name"]}!";
$$;
SELECT fe_a25_php();
-- A25: ? : and ** inside a string nested in an interpolation are text
CREATE FUNCTION fe_a25_text_rb(s text) RETURNS text LANGUAGE plxruby AS $$
return "[#{"a?b:c"}] #{s} #{"x**y"}"
$$;
SELECT fe_a25_text_rb('q');
CREATE FUNCTION fe_a25_tern_rb(n int) RETURNS text LANGUAGE plxruby AS $$
return n > 0 ? "#{"p:#{n}"}" : "neg"
$$;
SELECT fe_a25_tern_rb(2), fe_a25_tern_rb(-1);
-- A25: deeply nested interpolations stop with an error, not a crash
\set VERBOSITY terse
DO $d$ BEGIN
EXECUTE 'CREATE FUNCTION fe_a25_deep() RETURNS text LANGUAGE plxruby AS $b$' || E'\n'
  || 'return ' || repeat('"#{', 200000) || '1' || repeat('}"', 200000) || E'\n$b$';
END $d$;
\set VERBOSITY default
SELECT 1 AS still_up;
-- A9: a NULL in a raise option no longer replaces the error with 22004
CREATE FUNCTION fe_a9_raise_opt(u text) RETURNS void LANGUAGE plxruby AS $$
raise "user #{u} missing", errcode: "P0042", detail: "looked up #{u}", hint: "check #{u}"
$$;
CREATE FUNCTION fe_a9_raise_bare(u text) RETURNS void LANGUAGE plxruby AS $$
raise "user missing", errcode: "P0042", detail: u, hint: u
$$;
CREATE FUNCTION fe_a9_raise_col(u text) RETURNS void LANGUAGE plxruby AS $$
raise "bad", errcode: "P0042", column: u, constraint: u
$$;
CREATE FUNCTION fe_a9_raise_code(c text) RETURNS void LANGUAGE plxruby AS $$
raise "bad", errcode: c
$$;
CREATE FUNCTION fe_a9_catch(f text, a text) RETURNS text LANGUAGE plpgsql AS $f$
DECLARE s text; m text; d text; h text;
BEGIN
  EXECUTE format('SELECT %I($1)', f) USING a;
  RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
  GET STACKED DIAGNOSTICS s = RETURNED_SQLSTATE, m = MESSAGE_TEXT, d = PG_EXCEPTION_DETAIL, h = PG_EXCEPTION_HINT;
  RETURN format('[%s|%s|%s|%s]', s, m, d, h);
END $f$;
SELECT fe_a9_catch('fe_a9_raise_opt', 'bob');
SELECT fe_a9_catch('fe_a9_raise_opt', NULL);
SELECT fe_a9_catch('fe_a9_raise_bare', NULL);
SELECT fe_a9_catch('fe_a9_raise_col', NULL);
-- A9: a NULL errcode still fails with 22004: plx does not make up a SQLSTATE
SELECT fe_a9_catch('fe_a9_raise_code', NULL);
SELECT fe_a9_catch('fe_a9_raise_code', '22023');
-- A9: message: as the first argument is the message, with the same NULL fallback
CREATE FUNCTION fe_a9_raise_msgkey(u text) RETURNS void LANGUAGE plxruby AS $$
raise message: "m #{u}", errcode: "P0042", detail: u
$$;
SELECT fe_a9_catch('fe_a9_raise_msgkey', 'bob');
SELECT fe_a9_catch('fe_a9_raise_msgkey', NULL);
-- A9: a message and message: together are rejected
CREATE FUNCTION fe_a9_raise_twice() RETURNS void LANGUAGE plxruby AS $$
raise "x", message: "y"
$$;
\pset tuples_only on
\pset format unaligned
SELECT (regexp_match(prosrc, 'RAISE[^\n]*'))[1] FROM pg_proc WHERE proname = 'fe_a9_raise_opt';
\pset format aligned
\pset tuples_only off
