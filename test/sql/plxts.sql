-- plxts regression tests (TypeScript dialect)
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;

-- type annotations (string->text, number->numeric) + a ternary whose colon
-- must NOT be treated as a type annotation
CREATE FUNCTION ts_grade(score int) RETURNS text LANGUAGE plxts AS $$
let result: string = "F";
let bonus: number = score >= 95 ? 1 : 0;
if (score >= 90) { result = "A"; }
else if (score >= 80) { result = "B"; }
return result;
$$;
SELECT ts_grade(95), ts_grade(85), ts_grade(50);

-- bigint annotation + counting for loop (the loop-var annotation is dropped)
CREATE FUNCTION ts_sum(n int) RETURNS bigint LANGUAGE plxts AS $$
let total: bigint = 0;
for (let i: number = 1; i <= n; i++) {
  total = total + i;
}
return total;
$$;
SELECT ts_sum(100);

-- declaration with no value, boolean, union type "number | null"
CREATE FUNCTION ts_flags(a int, b int) RETURNS text LANGUAGE plxts AS $$
let m: number | null;
let ok: boolean = a < b;
m = a + b;
if (ok) { return "lt=" || m; }
return "ge=" || m;
$$;
SELECT ts_flags(2, 5), ts_flags(9, 1);

-- array type + FOREACH over an array (element type from the declaration)
CREATE FUNCTION ts_arraysum(a int[]) RETURNS bigint LANGUAGE plxts AS $$
let total: bigint = 0;
let v: number = 0;
for (const v of a) {
  total = total + v;
}
return total;
$$;
SELECT ts_arraysum(ARRAY[5, 10, 15, 20]);

-- query iteration + template literal
CREATE FUNCTION ts_query(g int) RETURNS bigint LANGUAGE plxts AS $$
let total: bigint = 0;
for (const r of query(`SELECT a FROM (VALUES (1),(2),(3)) t(a) WHERE a <= ${g}`)) {
  total = total + r.a;
}
return total;
$$;
SELECT ts_query(2);

-- switch/case
CREATE FUNCTION ts_name(n int) RETURNS text LANGUAGE plxts AS $$
let r: string = "many";
switch (n) {
  case 1: r = "one"; break;
  case 2: case 3: r = "few"; break;
  default: r = "many"; break;
}
return r;
$$;
SELECT ts_name(1), ts_name(3), ts_name(9);

-- try/catch, throw
CREATE FUNCTION ts_div(a int, b int) RETURNS int LANGUAGE plxts AS $$
try {
  return a / b;
} catch (e) {
  return -1;
}
$$;
SELECT ts_div(10, 2), ts_div(10, 0);

-- set-returning function with return_next
CREATE FUNCTION ts_squares(n int) RETURNS SETOF int LANGUAGE plxts AS $$
for (let i: number = 1; i <= n; i++) {
  return_next(i * i);
}
return;
$$;
SELECT array_agg(x) FROM ts_squares(4) x;

-- numeric(p,s) SQL type passed through in an annotation
CREATE FUNCTION ts_money(x numeric) RETURNS numeric LANGUAGE plxts AS $$
let amount: numeric(10,2) = x * 1.5;
return amount;
$$;
SELECT ts_money(10);

-- the annotation is rewritten to a trailing block comment
SELECT prosrc FROM pg_proc WHERE proname = 'ts_sum';
