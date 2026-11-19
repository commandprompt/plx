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
-- A19: throw of anything but a constructor call is rejected, not cut to its first argument
\set VERBOSITY terse
DO LANGUAGE plxjs $$ throw upper("abc") + lower("XYZ"); $$;
CREATE FUNCTION fb_th1(x int) RETURNS int LANGUAGE plxjs AS $$
throw `code ` + abs(x);
$$;
DO LANGUAGE plxjs $$ throw new Error("x").foo(1); $$;
DO LANGUAGE plxphp $$ throw new \RuntimeException("php " . upper("ns")); $$;
DO LANGUAGE plxjs $$ throw new Error(`code ${abs(-7)}`); $$;
DO LANGUAGE plxjs $$ throw Error("plain"); $$;
\set VERBOSITY default
-- A16: break 2 out of a switch inside two try/finally blocks runs both finally bodies
CREATE FUNCTION fb_rv1(n int) RETURNS int LANGUAGE plxphp AS $$
$c = 0; $fb = 0; $fc = 0;
for ($i = 0; $i < $n; $i++) {
  try {
    try {
      switch ($i) {
        case 1: break 2;
        default: $c = $c + 1; break;
      }
    } finally { $fb = $fb + 1; }
  } finally { $fc = $fc + 1; }
}
return $c * 100 + $fb * 10 + $fc;
$$;
SELECT fb_rv1(3);
CREATE FUNCTION fb_rv2(n int) RETURNS int LANGUAGE plxphp AS $$
$c = 0; $f = 0; $g = 0;
for ($i = 0; $i < $n; $i++) {
  try {
    try {
      lbl: for ($j = 0; $j < $n; $j++) {
        if ($j == 1) { break 2; }
        $c = $c + 1;
      }
    } finally { $f = $f + 1; }
  } finally { $g = $g + 1; }
}
return $c * 100 + $f * 10 + $g;
$$;
SELECT fb_rv2(3);
-- A16: PHP continue or continue 1 whose target is a switch is rejected, since PHP runs it as break
\set VERBOSITY terse
CREATE FUNCTION fb_rv3(n int) RETURNS int LANGUAGE plxphp AS $$
$c = 0;
for ($i = 0; $i < $n; $i++) {
  switch ($i) { case 1: continue; default: break; }
  $c = $c + 1;
}
return $c;
$$;
CREATE FUNCTION fb_rv4(n int) RETURNS int LANGUAGE plxphp AS $$
$c = 0;
for ($i = 0; $i < $n; $i++) {
  switch ($i) { case 1: continue 1; default: break; }
  $c = $c + 1;
}
return $c;
$$;
CREATE FUNCTION fb_rv5(n int) RETURNS int LANGUAGE plxphp AS $$
$c = 0;
for ($i = 0; $i < $n; $i++) {
  switch ($i) {
    case 1:
      if ($n > 2) { continue; }
      $c = $c + 100;
      break;
    default:
      break;
  }
  $c = $c + 1;
}
return $c;
$$;
\set VERBOSITY default
-- A17: a for whose variable an enclosing loop reads before the for is rejected
\set VERBOSITY terse
CREATE FUNCTION fb_rv6() RETURNS int LANGUAGE plxjs AS $$
let i = 0; let x = 0; let k = 0;
while (k < 2) { x = x + i; for (i = 0; i < 3; i++) { } k = k + 1; }
return x;
$$;
CREATE FUNCTION fb_rv7() RETURNS int LANGUAGE plxphp AS $$
$i = 0; $x = 0; $k = 0;
while ($k < 2) { $x = $x + $i; for ($i = 0; $i < 3; $i++) { } $k = $k + 1; }
return $x;
$$;
CREATE FUNCTION fb_rv8() RETURNS int LANGUAGE plxjs AS $$
let i = 0; let k = 0;
while (i < 3 && k < 5) { for (i = 0; i < 3; i++) { } k = k + 1; }
return k;
$$;
\set VERBOSITY default
-- an enclosing loop that reads the variable only inside another counting for still compiles
CREATE FUNCTION fb_rv9() RETURNS int LANGUAGE plxjs AS $$
let x = 0; let k = 0; let i = 0;
while (k < 2) {
  if (k > 0) { for (i = 0; i < 3; i++) { x = x + i; } }
  for (i = 0; i < 2; i++) { x = x + 10; }
  k = k + 1;
}
return x;
$$;
SELECT fb_rv9();
-- A17: for (var i ...) keeps i after the loop in JavaScript, so a read after the loop is rejected
\set VERBOSITY terse
CREATE FUNCTION fb_rv10(n int) RETURNS int LANGUAGE plxjs AS $$
var i = 7;
for (var i = 0; i < n; i++) { }
return i;
$$;
CREATE FUNCTION fb_rv11(n int) RETURNS int LANGUAGE plxts AS $$
for (var i = 0; i < n; i++) { }
return i;
$$;
\set VERBOSITY default
-- a for (var i ...) whose variable is not read after the loop still compiles
CREATE FUNCTION fb_rv12(n int) RETURNS int LANGUAGE plxjs AS $$
let c = 0;
for (var i = 0; i < n; i++) { c = c + i; }
return c;
$$;
SELECT fb_rv12(4);
-- A14: a PHP catch with no variable maps its class, not every error
\set VERBOSITY terse
CREATE FUNCTION fb_rv13(d int) RETURNS text LANGUAGE plxphp AS $$
try { $r = 0; $r = 10 / $d; return "ok"; } catch (TypeError) { return "type"; }
$$;
CREATE FUNCTION fb_rv14(d int) RETURNS text LANGUAGE plxphp AS $$
try { $r = 0; $r = 10 / $d; return "ok"; } catch (PG::SerializationFailure) { return "retry"; }
$$;
SELECT fb_rv14(0);
\set VERBOSITY default
CREATE FUNCTION fb_rv15(d int) RETURNS text LANGUAGE plxphp AS $$
try { $r = 0; $r = 10 / $d; return "ok"; } catch (PG::UniqueViolation | DivisionByZeroError) { return "union"; }
$$;
SELECT fb_rv15(0);
-- A19: throw new Error(...) with no semicolon, last in its block, is accepted
\set VERBOSITY terse
CREATE FUNCTION fb_rv16(x int) RETURNS int LANGUAGE plxts AS $$
if (x < 0) {
  throw new Error("neg")
}
return x
$$;
SELECT fb_rv16(-1);
CREATE FUNCTION fb_rv17(x int) RETURNS int LANGUAGE plxjs AS $$
if (x < 0) {
  throw new Error(`neg ${x}`)
}
return x
$$;
SELECT fb_rv17(-2);
\set VERBOSITY default
-- A14: a union of classes that does not match the error lets it through
\set VERBOSITY terse
CREATE FUNCTION fb_rv18(d int) RETURNS text LANGUAGE plxphp AS $$
$r = 0;
try {
  $r = 10 / $d;
  return "ok $r";
} catch (PG::UniqueViolation | PG::SerializationFailure $e) {
  return "union";
}
$$;
SELECT fb_rv18(0);
\set VERBOSITY default
-- A14: a comment inside a catch class name is not part of the class
CREATE FUNCTION fb_rv19(d int) RETURNS text LANGUAGE plxphp AS $$
$r = 0;
try {
  $r = 10 / $d;
  return "ok $r";
} catch (PG:: /* SQLSTATE 22012 */ DivisionByZero $e) {
  return "div";
}
$$;
SELECT fb_rv19(0);
-- A14: a union with a catch-all member catches every error
CREATE FUNCTION fb_rv20(d int) RETURNS text LANGUAGE plxphp AS $$
$r = 0;
try {
  $r = 10 / $d;
  return "ok $r";
} catch (PG::UniqueViolation | Exception $e) {
  return "any";
}
$$;
SELECT fb_rv20(0);
SELECT prosrc LIKE '%WHEN OTHERS THEN%' AS plain_others FROM pg_proc WHERE proname = 'fb_rv20';
