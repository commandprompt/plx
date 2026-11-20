-- Regression tests for the PR 24 review findings owned by the engine3 group.
-- Each case names its finding as R<n>.
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS plx;
-- e3_err(q) runs q and returns 'SQLSTATE: message' if it fails. The message
-- leaves out the error position, which differs between PostgreSQL 13
-- (plpgsql prefixes each expression with SELECT) and later versions.
CREATE FUNCTION e3_err(q text) RETURNS text LANGUAGE plpgsql AS $h$
BEGIN
  EXECUTE q;
  RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
  RETURN SQLSTATE || ': ' || SQLERRM;
END $h$;

-- R1: deeply nested brackets transpile in linear time (the ** rewriter used
-- to scan each group to its end at every bracket, which took seconds here)
SET statement_timeout = '1s';
SET check_function_bodies = off;
DO $d$ BEGIN EXECUTE format('CREATE FUNCTION e3_r1_js() RETURNS text LANGUAGE plxjs AS %L', 'let a = 1; return ' || repeat('("x" + ', 16000) || 'a' || repeat(')', 16000) || ';'); END $d$;
DO $d$ BEGIN EXECUTE format('CREATE FUNCTION e3_r1_ts() RETURNS int LANGUAGE plxts AS %L', 'let a: number = 1; return ' || repeat('(', 32000) || 'a' || repeat(')', 32000) || ';'); END $d$;
DO $d$ BEGIN EXECUTE format('CREATE FUNCTION e3_r1_php() RETURNS int LANGUAGE plxphp AS %L', '$a = 1; return ' || repeat('[', 32000) || '$a' || repeat(']', 32000) || ';'); END $d$;
DO $d$ BEGIN EXECUTE format('CREATE FUNCTION e3_r1_py() RETURNS int LANGUAGE plxpython3 AS %L', 'a = 1' || chr(10) || 'return ' || repeat('abs(', 24000) || 'a' || repeat(')', 24000)); END $d$;
DO $d$ BEGIN EXECUTE format('CREATE FUNCTION e3_r1_rb() RETURNS int LANGUAGE plxruby AS %L', 'a = 1' || chr(10) || 'return ' || repeat('abs(', 24000) || 'a' || repeat(')', 24000)); END $d$;
RESET check_function_bodies;
RESET statement_timeout;
-- the ** inside a group is still rewritten
CREATE FUNCTION e3_r1_pow() RETURNS int LANGUAGE plxjs AS $$
return abs(((2 ** 3 ** 2)) - 0) + (1 + (2 ** 2));
$$;
SELECT e3_r1_pow();

-- R6: nested loop labels that differ only in case are rejected (plpgsql
-- would read both as one label, so continue Outer went to the inner loop)
CREATE FUNCTION e3_r6_js() RETURNS int LANGUAGE plxjs AS $$
let s = 0;
Outer: for (let i = 0; i < 3; i++) { outer: for (let j = 0; j < 3; j++) { if (j == 1) { continue Outer; } s += 1; } }
return s;
$$;
CREATE FUNCTION e3_r6_py() RETURNS int LANGUAGE plxpython3 AS $$
s = 0
Outer: for i in range(3):
    outer: for j in range(3):
        if j == 1:
            break Outer
        s += 1
return s
$$;
CREATE FUNCTION e3_r6_rb() RETURNS int LANGUAGE plxruby AS $$
s = 0
Outer: for i in 0..2
  outer: for j in 0..2
    break Outer if j == 1
    s += 1
  end
end
return s
$$;
-- labels that differ only in case on loops that are not nested still work
CREATE FUNCTION e3_r6_seq() RETURNS int LANGUAGE plxjs AS $$
let s = 0;
Outer: for (let i = 0; i < 3; i++) { if (i == 1) { break Outer; } s += 1; }
outer: for (let j = 0; j < 3; j++) { if (j == 2) { break outer; } s += 10; }
return s;
$$;
SELECT e3_r6_seq();

-- R7: plxphp '.' works on a string built with '.=' (the builder local was
-- passed to || as it is, which PostgreSQL could not resolve)
CREATE TABLE e3_names(id int, name text);
INSERT INTO e3_names VALUES (1, 'ann'), (2, 'bob');
CREATE FUNCTION e3_r7_sb2() RETURNS text LANGUAGE plxphp AS $body$
$s = "" /*:: text */;
foreach (query("SELECT name FROM e3_names ORDER BY id") as $row) {
  $s .= $row->name;
  $s .= ",";
}
return "names: " . $s;
$body$;
SELECT e3_r7_sb2();
CREATE FUNCTION e3_r7_g10(n int) RETURNS text LANGUAGE plxphp AS $body$
$s = ""; $t = "";
for ($i = 0; $i < $n; $i++) { $s .= "x"; }
$t = $s . "!";
return $t;
$body$;
SELECT e3_r7_g10(3);
-- a read that comes before the first .= in the source
CREATE FUNCTION e3_r7_early() RETURNS text LANGUAGE plxphp AS $body$
$s = ""; $out = "";
for ($k = 0; $k < 3; $k++) { $out = $out . "[" . $s . "]"; $s .= "x"; }
return $out . $s;
$body$;
SELECT e3_r7_early();

-- R15: the second string append works whatever plx's schema is called. A
-- schema name of 50 bytes or more overflowed the buffer is_builder_type
-- copied the qualified type name into, so the local stopped counting as a
-- string and s += 'b' became numeric addition. This case re-creates the
-- extension in such schemas and puts it back in public at the end.
DROP EXTENSION plx CASCADE;
CREATE SCHEMA sssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssss;
CREATE EXTENSION plx SCHEMA sssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssss;
CREATE FUNCTION e3_r15_js_long() RETURNS text LANGUAGE plxjs AS $$
let s = '';
s += 'a';
s += 'b';
return s;
$$;
CREATE FUNCTION e3_r15_ts_long() RETURNS text LANGUAGE plxts AS $$
let s: string = '';
s += 'a';
s += 'b';
return s;
$$;
CREATE FUNCTION e3_r15_py_long() RETURNS text LANGUAGE plxpython3 AS $$
s = ''
s += 'a'
s += 'b'
return s
$$;
CREATE FUNCTION e3_r15_rb_long() RETURNS text LANGUAGE plxruby AS $$
s = ''
s << 'a'
s << 'b'
return s
$$;
SELECT e3_r15_js_long(), e3_r15_ts_long(), e3_r15_py_long(), e3_r15_rb_long();
DROP EXTENSION plx CASCADE;
CREATE SCHEMA "Plx ""Q"" Schema With Mixed Case And Quotes """"x"""" 0123456789";
CREATE EXTENSION plx SCHEMA "Plx ""Q"" Schema With Mixed Case And Quotes """"x"""" 0123456789";
CREATE FUNCTION e3_r15_js_quoted() RETURNS text LANGUAGE plxjs AS $$
let s = '';
s += 'a';
s += 'b';
return s;
$$;
CREATE FUNCTION e3_r15_ts_quoted() RETURNS text LANGUAGE plxts AS $$
let s: string = '';
s += 'a';
s += 'b';
return s;
$$;
CREATE FUNCTION e3_r15_py_quoted() RETURNS text LANGUAGE plxpython3 AS $$
s = ''
s += 'a'
s += 'b'
return s
$$;
CREATE FUNCTION e3_r15_rb_quoted() RETURNS text LANGUAGE plxruby AS $$
s = ''
s << 'a'
s << 'b'
return s
$$;
SELECT e3_r15_js_quoted(), e3_r15_ts_quoted(), e3_r15_py_quoted(), e3_r15_rb_quoted();
DROP EXTENSION plx CASCADE;
DROP SCHEMA sssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssss;
DROP SCHEMA "Plx ""Q"" Schema With Mixed Case And Quotes """"x"""" 0123456789";
CREATE EXTENSION plx SCHEMA public;

-- R5: a counting for whose variable is read after the loop, or again by an
-- enclosing loop, is rejected when the read is inside a string
-- interpolation too (it used to see the value from before the loop)
CREATE FUNCTION e3_r5_rl1(n int) RETURNS text LANGUAGE plxjs AS $body$
let i = 0;
for (i = 0; i < n; i++) {
  if (i * i > 10) { break; }
}
let r = fetch_one(`SELECT ${i} * 100 AS v`);
return `first i with i*i > 10: ${i}, query says ${r.v}`;
$body$;
CREATE FUNCTION e3_r5_rl2(n int) RETURNS text LANGUAGE plxjs AS $body$
let s = ""; let j = 0; let k = 0;
while (k < 2) {
  s += `[j=${j}]`;
  for (j = 0; j < n; j++) { }
  k += 1;
}
return s;
$body$;
CREATE FUNCTION e3_r5_rl3(n int) RETURNS text LANGUAGE plxphp AS $body$
$i = 0;
for ($i = 0; $i < $n; $i++) {
  if ($i * $i > 10) { break; }
}
return "stopped at $i";
$body$;
CREATE FUNCTION e3_r5_ts(n int) RETURNS text LANGUAGE plxts AS $body$
let i: number = 0;
for (i = 0; i < n; i++) { }
return `${i}`;
$body$;
-- a read in the header of an enclosing loop
CREATE FUNCTION e3_r5_hdr(n int) RETURNS int LANGUAGE plxjs AS $body$
let j = 0; let k = 0;
while (`${j}` != "3") {
  for (j = 0; j < n; j++) { }
  k += 1;
}
return k;
$body$;
-- the name in plain text, or in a string that does not interpolate, is not a read
CREATE FUNCTION e3_r5_plain(n int) RETURNS text LANGUAGE plxjs AS $body$
let i = 0; let t = 0;
for (i = 0; i < n; i++) { t += i; }
let name = "i";
return `${name} = ${t}`;
$body$;
SELECT e3_r5_plain(4);
-- plxpython3 and plxruby already keep the final value, interpolation included
CREATE FUNCTION e3_r5_py(n int) RETURNS text LANGUAGE plxpython3 AS $body$
i = 0
for i in range(n):
    if i * i > 10:
        break
return f"stopped at {i}"
$body$;
SELECT e3_r5_py(100);
CREATE FUNCTION e3_r5_rb(n int) RETURNS text LANGUAGE plxruby AS $body$
i = 0
for i in 0...n
  break if i * i > 10
end
return "stopped at #{i}"
$body$;
SELECT e3_r5_rb(100);
