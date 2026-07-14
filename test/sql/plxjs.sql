-- plxjs regression tests
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;

-- scalar return
CREATE FUNCTION js_add(a int, b int) RETURNS int LANGUAGE plxjs AS $$
return a + b * 2;
$$;
SELECT js_add(3, 4);

-- let, if / else if / else
CREATE FUNCTION js_grade(score int) RETURNS text LANGUAGE plxjs AS $$
let grade = "F";
if (score >= 90) { grade = "A"; }
else if (score >= 80) { grade = "B"; }
else { grade = "F"; }
return grade;
$$;
SELECT js_grade(95), js_grade(85), js_grade(70);

-- C-style for with let
CREATE FUNCTION js_sum(n int) RETURNS int LANGUAGE plxjs AS $$
let total = 0;
for (let i = 1; i <= n; i++) {
  total = total + i;
}
return total;
$$;
SELECT js_sum(10);

-- template literal interpolation
CREATE FUNCTION js_greet(nm text) RETURNS text LANGUAGE plxjs AS $$
return `Hello, ${nm}!`;
$$;
SELECT js_greet('World');

CREATE FUNCTION js_interp(nm text, n int) RETURNS text LANGUAGE plxjs AS $$
return `user ${nm} has ${n} items`;
$$;
SELECT js_interp('bob', 3);

-- while
CREATE FUNCTION js_countdown(n int) RETURNS int LANGUAGE plxjs AS $$
let c = 0;
while (n > 0) { c = c + n; n = n - 1; }
return c;
$$;
SELECT js_countdown(5);

-- for-of over a query
CREATE TABLE js_orders(grp int, amount bigint);
INSERT INTO js_orders VALUES (1,10),(1,20),(1,30),(2,5);
CREATE FUNCTION js_grp_total(g int) RETURNS bigint LANGUAGE plxjs AS $$
let total = 0 /*:: bigint */;
for (const row of query(`SELECT amount FROM js_orders WHERE grp = ${g}`)) {
  total = total + row.amount;
}
return total;
$$;
SELECT js_grp_total(1), js_grp_total(2);

-- fetch_one
CREATE TABLE js_users(id int, name text);
INSERT INTO js_users VALUES (1,'Alice'),(2,'Bob');
CREATE FUNCTION js_name(uid int) RETURNS text LANGUAGE plxjs AS $$
let u = fetch_one(`SELECT id, name FROM js_users WHERE id = ${uid}`);
return u.name;
$$;
SELECT js_name(2);

-- SETOF via return_next
CREATE FUNCTION js_squares(n int) RETURNS SETOF int LANGUAGE plxjs AS $$
for (let i = 1; i <= n; i++) { return_next(i * i); }
return;
$$;
SELECT * FROM js_squares(3);

-- try / catch
CREATE FUNCTION js_safediv(d int) RETURNS int LANGUAGE plxjs AS $$
try {
  return 100 / d;
} catch (e) {
  raise("notice", `caught: ${e.message}`);
  return -1;
}
$$;
SELECT js_safediv(4), js_safediv(0);

-- throw
CREATE FUNCTION js_checkpos(v int) RETURNS int LANGUAGE plxjs AS $$
if (v < 0) { throw new Error(`negative: ${v}`); }
return v;
$$;
SELECT js_checkpos(7);

-- DO block (body is JavaScript)
DO LANGUAGE plxjs $$
for (let i = 1; i <= 2; i++) { raise("warning", `row ${i}`); }
$$;

-- switch (with stacked cases and default)
CREATE FUNCTION js_switch(n int) RETURNS text LANGUAGE plxjs AS $$
switch (n) {
  case 1: return "one";
  case 2: case 3: return "few";
  default: return "many";
}
$$;
SELECT js_switch(1), js_switch(2), js_switch(9);

-- assert
CREATE FUNCTION js_assert(n int) RETURNS int LANGUAGE plxjs AS $$
assert(n > 0, "must be positive");
return n;
$$;
SELECT js_assert(5);

-- FOREACH over an array (for..of)
CREATE FUNCTION js_sumarr(a int[]) RETURNS int LANGUAGE plxjs AS $$
let total = 0 /*:: int */;
let v /*:: int */;
for (const v of a) { total = total + v; }
return total;
$$;
SELECT js_sumarr(ARRAY[1,2,3,4]);

-- cursors
CREATE TABLE js_nums(v int);
INSERT INTO js_nums SELECT generate_series(1,4);
CREATE FUNCTION js_cursor_sum() RETURNS int LANGUAGE plxjs AS $$
let total = 0 /*:: int */;
let c = open_cursor(`SELECT v FROM js_nums ORDER BY v`);
let row = fetch_from(c);
while (found()) {
  total = total + row.v;
  row = fetch_from(c);
}
close_cursor(c);
return total;
$$;
SELECT js_cursor_sum();

-- labeled loop
CREATE FUNCTION js_labeled(n int) RETURNS int LANGUAGE plxjs AS $$
let count = 0 /*:: int */;
outer: for (let i = 1; i <= n; i++) {
  for (let j = 1; j <= n; j++) {
    count = count + 1;
    if (i + j >= 4) { break outer; }
  }
}
return count;
$$;
SELECT js_labeled(5);
