-- Regression tests for bugs found by the 2026-11-19 audit (fix_brace).
-- Each case names its audit finding (A<n>).
-- A13: a for-of / foreach over query() with no arguments is rejected, not a crash
DO LANGUAGE plxjs $$ for (const r of query()) { } $$;
DO LANGUAGE plxts $$ for (const r of query()) { } $$;
DO LANGUAGE plxphp $$ foreach (query() as $r) { } $$;
SELECT 'A13 backend still alive' AS ok;
-- N-pyquery: a Python for over query() with no arguments is rejected, not a crash
DO LANGUAGE plxpython3 $$
for r in query():
    pass
$$;
SELECT 'N-pyquery backend still alive' AS ok;
-- N-tscatch: a TypeScript catch with a type annotation binds the variable, not the type
CREATE FUNCTION fb_tscatch(d int) RETURNS text LANGUAGE plxts AS $$
let r: number = 0;
try {
  r = 10 / d;
  return "ok";
} catch (e: unknown) {
  return `caught ${e.sqlstate}`;
}
$$;
SELECT fb_tscatch(0);
-- A14: a catch class maps to its own condition; an unknown class is rejected
\set VERBOSITY terse
CREATE FUNCTION fb_ct1(d int) RETURNS text LANGUAGE plxphp AS $$
$r = 0;
try {
  $r = 10 / $d;
  return "ok $r";
} catch (PG::SerializationFailure $e) {
  return "retry";
}
$$;
SELECT fb_ct1(0);
CREATE FUNCTION fb_ct2(d int) RETURNS text LANGUAGE plxphp AS $$
$r = 0;
try {
  $r = 10 / $d;
  return "ok $r";
} catch (PG::DivisionByZero $e) {
  return "div " . $e->sqlstate;
}
$$;
SELECT fb_ct2(0);
CREATE FUNCTION fb_ct3(d int) RETURNS text LANGUAGE plxphp AS $$
$r = 0;
try {
  $r = 10 / $d;
  return "ok $r";
} catch (PG::UniqueViolation | DivisionByZeroError $e) {
  return "union";
}
$$;
SELECT fb_ct3(0);
CREATE FUNCTION fb_ct4(d int) RETURNS text LANGUAGE plxphp AS $$
$r = 0;
try {
  $r = 10 / $d;
  return "ok $r";
} catch (TypeError $e) {
  return "type";
}
$$;
CREATE FUNCTION fb_ct5(d int) RETURNS text LANGUAGE plxphp AS $$
$r = 0;
try {
  $r = 10 / $d;
  return "ok $r";
} catch (PG::NoSuchCondition $e) {
  return "none";
}
$$;
SELECT fb_ct5(0);
CREATE FUNCTION fb_ct6(d int) RETURNS text LANGUAGE plxphp AS $$
$r = 0;
try {
  $r = 10 / $d;
  return "ok $r";
} catch (\Throwable $e) {
  return "any";
}
$$;
SELECT fb_ct6(0);
\set VERBOSITY default
