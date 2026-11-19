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

-- A7: ^ is bitwise XOR in Ruby, PHP, JS, TS and Python, not SQL exponent
CREATE FUNCTION fe_a7_xor_rb(a int, b int) RETURNS int LANGUAGE plxruby AS $$
return a ^ b
$$;
SELECT fe_a7_xor_rb(6, 3);
CREATE FUNCTION fe_a7_xor_js() RETURNS int LANGUAGE plxjs AS $$
return 5 ^ 3;
$$;
SELECT fe_a7_xor_js();
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
