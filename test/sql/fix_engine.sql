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
