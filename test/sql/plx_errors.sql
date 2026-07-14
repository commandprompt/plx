-- plx rejection tests: each CREATE FUNCTION must fail at CREATE time with a
-- precise error and source line number.
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;

-- plxruby: nested def
CREATE FUNCTION e_rb_def() RETURNS int LANGUAGE plxruby AS $$
def helper
  1
end
return helper
$$;

-- plxruby: case/when
CREATE FUNCTION e_rb_case(n int) RETURNS text LANGUAGE plxruby AS $$
case n
when 1
  return "one"
end
return "other"
$$;

-- plxruby: unresolvable local type
CREATE FUNCTION e_rb_type() RETURNS int LANGUAGE plxruby AS $$
x = some_call()
return x
$$;

-- plxruby: next outside a loop
CREATE FUNCTION e_rb_next() RETURNS int LANGUAGE plxruby AS $$
next
return 1
$$;

-- plxphp: nested function definition
CREATE FUNCTION e_php_fn() RETURNS int LANGUAGE plxphp AS $$
function helper() { return 1; }
return 1;
$$;

-- plxphp: switch/case
CREATE FUNCTION e_php_switch(n int) RETURNS text LANGUAGE plxphp AS $$
switch ($n) { case 1: return "one"; }
return "other";
$$;
