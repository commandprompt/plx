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
