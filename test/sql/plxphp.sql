-- plxphp regression tests
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = WARNING;

CREATE FUNCTION php_add(a int, b int) RETURNS int LANGUAGE plxphp AS $$
  return $a + $b * 2;
$$;
SELECT php_add(3, 4) AS should_be_11;

CREATE FUNCTION php_grade(score int) RETURNS text LANGUAGE plxphp AS $$
  $grade = "F";
  if ($score >= 90) { $grade = "A"; }
  elseif ($score >= 80) { $grade = "B"; }
  else { $grade = "F"; }
  return $grade;
$$;
SELECT php_grade(85) AS should_be_B;

CREATE FUNCTION php_sum(n int) RETURNS int LANGUAGE plxphp AS $$
  $total = 0;
  for ($i = 1; $i <= $n; $i++) {
    $total = $total + $i;
  }
  return $total;
$$;
SELECT php_sum(10) AS should_be_55;

CREATE FUNCTION php_greet(nm text) RETURNS text LANGUAGE plxphp AS $$
  return "Hello, " . $nm . "!";
$$;
SELECT php_greet('World') AS should_be_hello_world;

CREATE FUNCTION php_interp(nm text, n int) RETURNS text LANGUAGE plxphp AS $$
  return "user $nm has {$n} items";
$$;
SELECT php_interp('bob', 3) AS interp;

CREATE FUNCTION php_countdown(n int) RETURNS int LANGUAGE plxphp AS $$
  $c = 0;
  while ($n > 0) { $c = $c + $n; $n = $n - 1; }
  return $c;
$$;
SELECT php_countdown(5) AS should_be_15;

CREATE TEMP TABLE orders(grp int, amount bigint);
INSERT INTO orders VALUES (1,10),(1,20),(1,30),(2,5);
CREATE FUNCTION php_sumq(g int) RETURNS bigint LANGUAGE plxphp AS $$
  $total = 0 /*:: bigint */;
  foreach (query("SELECT amount FROM orders WHERE grp = {$g}") as $row) {
    $total = $total + $row->amount;
  }
  return $total;
$$;
SELECT php_sumq(1) AS should_be_60;

CREATE TEMP TABLE users(id int, name text);
INSERT INTO users VALUES (1,'Alice'),(2,'Bob');
CREATE FUNCTION php_name(uid int) RETURNS text LANGUAGE plxphp AS $$
  $u = fetch_one("SELECT id, name FROM users WHERE id = {$uid}");
  return $u->name;
$$;
SELECT php_name(2) AS should_be_Bob;

CREATE FUNCTION php_squares(n int) RETURNS SETOF int LANGUAGE plxphp AS $$
  for ($i = 1; $i <= $n; $i++) { return_next($i * $i); }
  return;
$$;
SELECT string_agg(s::text, ',') AS should_be_1_4_9 FROM php_squares(3) s;

CREATE FUNCTION php_safediv(d int) RETURNS int LANGUAGE plxphp AS $$
  try {
    return 100 / $d;
  } catch (\Exception $e) {
    return -1;
  }
$$;
SELECT php_safediv(0) AS should_be_minus1;

CREATE FUNCTION php_checkpos(v int) RETURNS int LANGUAGE plxphp AS $$
  if ($v < 0) { throw new Exception("negative: {$v}"); }
  return $v;
$$;
SELECT php_checkpos(-3) AS should_error;

-- switch (with stacked cases and default)
CREATE FUNCTION php_switch(n int) RETURNS text LANGUAGE plxphp AS $$
switch ($n) {
  case 1: return "one";
  case 2: case 3: return "few";
  default: return "many";
}
$$;
SELECT php_switch(1), php_switch(2), php_switch(9);

-- assert
CREATE FUNCTION php_assert(n int) RETURNS int LANGUAGE plxphp AS $$
assert($n > 0, "must be positive");
return $n;
$$;
SELECT php_assert(5);

-- FOREACH over an array
CREATE FUNCTION php_sumarr(a int[]) RETURNS int LANGUAGE plxphp AS $$
$total = 0 /*:: int */;
$v /*:: int */;
foreach ($a as $v) { $total = $total + $v; }
return $total;
$$;
SELECT php_sumarr(ARRAY[5,5,5]);

-- trigger: read and assign NEW fields via the -> arrow form
CREATE TABLE php_trg(id int, qty int, price numeric, total numeric, tag text);
CREATE FUNCTION php_stamp() RETURNS trigger LANGUAGE plxphp AS $$
$NEW->total = $NEW->qty * $NEW->price;
$NEW->tag = "row {$NEW->id}";
return $NEW;
$$;
CREATE TRIGGER php_trg_ins BEFORE INSERT ON php_trg
  FOR EACH ROW EXECUTE FUNCTION php_stamp();
INSERT INTO php_trg(id, qty, price) VALUES (7, 3, 10);
SELECT id, total, tag FROM php_trg;
DROP TABLE php_trg CASCADE;

-- ${name} curly interpolation in double-quoted strings
CREATE FUNCTION php_interp_curly(name text) RETURNS text LANGUAGE plxphp AS $$
return "Hello ${name}!";
$$;
SELECT php_interp_curly('world');

-- single-quoted strings are raw: backslash escapes stay literal ('\n' -> \ n)
CREATE FUNCTION php_raw_squote() RETURNS text LANGUAGE plxphp AS $$
return 'a\nb';
$$;
SELECT php_raw_squote() AS val,
       length(php_raw_squote()) AS len,
       php_raw_squote() = 'a' || chr(92) || 'nb' AS backslash_kept;

-- double-quoted strings still process escapes ('\n' -> newline)
CREATE FUNCTION php_dquote_esc() RETURNS int LANGUAGE plxphp AS $$
return length("a\nb");
$$;
SELECT php_dquote_esc() AS newline_len;
