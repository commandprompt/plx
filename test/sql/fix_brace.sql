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
-- A15: a break nested inside a switch arm leaves the switch, not the enclosing loop
CREATE FUNCTION fb_sw1(n int) RETURNS int LANGUAGE plxjs AS $$
let total = 0;
for (let i = 0; i < n; i++) {
  switch (i) {
    case 1:
      if (n > 2) { break; }
      total = total + 100;
      break;
    default:
      total = total + 1;
      break;
  }
}
return total;
$$;
SELECT fb_sw1(5);
CREATE FUNCTION fb_sw2(n int) RETURNS int LANGUAGE plxjs AS $$
let r = 0;
switch (n) {
  case 1: if (n > 0) { break; } r = 5; break;
  default: r = 9; break;
}
return r;
$$;
SELECT fb_sw2(1), fb_sw2(2);
CREATE FUNCTION fb_sw3(n int) RETURNS int LANGUAGE plxjs AS $$
let r = 0;
switch (n) {
  case 1:
    try {
      if (n > 0) { break; }
      r = 5;
    } finally {
      r = r + 10;
    }
    r = r + 100;
    break;
  default:
    r = 9;
    break;
}
return r;
$$;
SELECT fb_sw3(1);
CREATE FUNCTION fb_sw4(n int) RETURNS int LANGUAGE plxjs AS $$
let c = 0;
switch (n) {
  case 1:
    for (let i = 0; i < 5; i++) {
      if (i == 2) { break; }
      c = c + 1;
    }
    c = c + 100;
    break;
  default:
    c = -1;
    break;
}
return c;
$$;
SELECT fb_sw4(1);
CREATE FUNCTION fb_sw5(n int) RETURNS int LANGUAGE plxphp AS $$
$t = 0;
for ($i = 0; $i < $n; $i++) {
  switch ($i) {
    case 1:
      if ($n > 2) { break; }
      $t = $t + 100;
      break;
    default:
      $t = $t + 1;
      break;
  }
}
return $t;
$$;
SELECT fb_sw5(5);
-- N-swlabel: a switch arm may end with a labelled break or with continue
CREATE FUNCTION fb_sw6(n int) RETURNS int LANGUAGE plxjs AS $$
let c = 0;
outer: for (let i = 0; i < n; i++) {
  switch (i) {
    case 2:
      break outer;
    default:
      c = c + 1;
      break;
  }
}
return c;
$$;
SELECT fb_sw6(5);
CREATE FUNCTION fb_sw7(n int) RETURNS int LANGUAGE plxjs AS $$
let c = 0;
for (let i = 0; i < n; i++) {
  switch (i) {
    case 2:
      continue;
    default:
      c = c + 1;
      break;
  }
  c = c + 10;
}
return c;
$$;
SELECT fb_sw7(5);
-- A16: PHP break N and continue N leave or continue the N-th enclosing loop or switch
CREATE FUNCTION fb_bn1(n int) RETURNS int LANGUAGE plxphp AS $$
$c = 0;
for ($i = 0; $i < $n; $i++) {
  for ($j = 0; $j < $n; $j++) {
    $c = $c + 1;
    if ($j == 1) { break 2; }
  }
}
return $c;
$$;
SELECT fb_bn1(5);
CREATE FUNCTION fb_bn2(n int) RETURNS int LANGUAGE plxphp AS $$
$c = 0;
for ($i = 0; $i < $n; $i++) {
  switch ($i % 2) {
    case 0:
      continue 2;
    default:
      $c = $c + 1;
      break;
  }
  $c = $c + 10;
}
return $c;
$$;
SELECT fb_bn2(4);
CREATE FUNCTION fb_bn3(n int) RETURNS int LANGUAGE plxphp AS $$
$c = 0;
for ($i = 0; $i < $n; $i++) {
  switch ($i) {
    case 3:
      break 2;
    default:
      $c = $c + 1;
      break;
  }
}
return $c;
$$;
SELECT fb_bn3(10);
CREATE FUNCTION fb_bn4(n int) RETURNS int LANGUAGE plxphp AS $$
$c = 0;
for ($i = 0; $i < $n; $i++) {
  for ($j = 0; $j < $n; $j++) {
    try {
      if ($j == 1) { break 2; }
      $c = $c + 1;
    } finally {
      $c = $c + 100;
    }
  }
}
return $c;
$$;
SELECT fb_bn4(5);
CREATE FUNCTION fb_bn5(n int) RETURNS int LANGUAGE plxphp AS $$
$c = 0;
for ($i = 0; $i < $n; $i++) {
  for ($j = 0; $j < $n; $j++) {
    if ($j == 2) { continue 2; }
    $c = $c + 1;
  }
}
return $c;
$$;
SELECT fb_bn5(4);
\set VERBOSITY terse
CREATE FUNCTION fb_bn6(n int) RETURNS int LANGUAGE plxphp AS $$
for ($i = 0; $i < $n; $i++) {
  for ($j = 0; $j < $n; $j++) {
    break 3;
  }
}
return 0;
$$;
CREATE FUNCTION fb_bn7(n int) RETURNS int LANGUAGE plxphp AS $$
for ($i = 0; $i < $n; $i++) {
  break 0;
}
return 0;
$$;
CREATE FUNCTION fb_bn8(n int) RETURNS int LANGUAGE plxphp AS $$
switch ($n) {
  case 1:
    switch ($n) {
      case 1:
        continue 2;
      default:
        break;
    }
    break;
  default:
    break;
}
return 0;
$$;
CREATE FUNCTION fb_bn9(n int) RETURNS int LANGUAGE plxjs AS $$
for (let i = 0; i < n; i++) {
  for (let j = 0; j < n; j++) {
    break 2;
  }
}
return 0;
$$;
\set VERBOSITY default
-- A17: a for that is not a plain counting loop is rejected, not lowered to a wrong FOR
\set VERBOSITY terse
CREATE FUNCTION fb_f1(n int) RETURNS int LANGUAGE plxjs AS $$
let c = 0;
for (let i = 0; c < n; i++) { c = c + 2; }
return c;
$$;
CREATE FUNCTION fb_f2(n int) RETURNS int LANGUAGE plxjs AS $$
let c = 0; let lim = n;
for (let i = 0; i < lim; i++) { lim = 2; c = c + 1; }
return c;
$$;
CREATE FUNCTION fb_f3() RETURNS int LANGUAGE plxjs AS $$
let i = 0; let c = 0;
for (i = 0; i < 10; i++) { i = i + 1; c = c + 1; }
return c;
$$;
CREATE FUNCTION fb_f4(n int) RETURNS int LANGUAGE plxjs AS $$
let i = 0;
for (i = 0; i < n; i++) { }
return i;
$$;
CREATE FUNCTION fb_f5(n int) RETURNS int LANGUAGE plxjs AS $$
let c = 0;
for (let i = 0; i < n && c < 2; i++) { c = c + 1; }
return c;
$$;
CREATE FUNCTION fb_f6(n int) RETURNS int LANGUAGE plxjs AS $$
let c = 0; let j = 0;
for (let i = 0; i < n; i++, j++) { c = c + 1; }
return c;
$$;
CREATE FUNCTION fb_f7(n int) RETURNS int LANGUAGE plxphp AS $$
for ($i = 0; $i < $n; $i++) { }
return $i;
$$;
CREATE FUNCTION fb_f8(n int) RETURNS int LANGUAGE plxjs AS $$
let c = 0;
for (let i = 0; i < n; j++) { c = c + 1; }
return c;
$$;
\set VERBOSITY default
-- a counting loop, reused after the loop only by another for, still compiles
CREATE FUNCTION fb_f9(n int) RETURNS int LANGUAGE plxphp AS $$
$t = 0;
for ($i = 0; $i < $n; $i++) { $t = $t + $i; }
for ($i = 0; $i < $n; $i += 2) { $t = $t + 1; }
return $t;
$$;
SELECT fb_f9(4);
-- a loop variable assigned again after the loop is not read after it
CREATE FUNCTION fb_f10(n int) RETURNS int LANGUAGE plxphp AS $$
$t = 0;
for ($i = 1; $i <= $n; $i++) {
  for ($j = 1; $j <= $i; $j++) { $t = $t + 1; }
}
$i = 100;
return $t + $i;
$$;
SELECT fb_f10(3);
