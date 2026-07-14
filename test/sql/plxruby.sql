-- plxruby regression tests
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;

-- scalar return, arithmetic
CREATE FUNCTION rb_add(a int, b int) RETURNS int LANGUAGE plxruby AS $$
return a + b * 2
$$;
SELECT rb_add(3, 4);

-- if / elsif / else with a type annotation
CREATE FUNCTION rb_grade(score int) RETURNS text LANGUAGE plxruby AS $$
grade #:: text
if score >= 90
  grade = "A"
elsif score >= 80
  grade = "B"
else
  grade = "F"
end
return grade
$$;
SELECT rb_grade(95), rb_grade(85), rb_grade(70);

-- unless modifier
CREATE FUNCTION rb_zcheck(n int) RETURNS text LANGUAGE plxruby AS $$
return "empty" unless n != 0
return "nonzero"
$$;
SELECT rb_zcheck(0), rb_zcheck(5);

-- while / until
CREATE FUNCTION rb_countdown(n int) RETURNS int LANGUAGE plxruby AS $$
c = 0 #:: integer
while n > 0
  c = c + n
  n = n - 1
end
return c
$$;
SELECT rb_countdown(5);

-- for loop with next / break
CREATE FUNCTION rb_sum_skip(n int) RETURNS int LANGUAGE plxruby AS $$
total = 0
for i in 1..n
  next if i == 3
  break if i > 7
  total = total + i
end
return total
$$;
SELECT rb_sum_skip(10);

-- exclusive range via .each
CREATE FUNCTION rb_exsum(n int) RETURNS int LANGUAGE plxruby AS $$
s = 0
(0...n).each do |j|
  s = s + j
end
return s
$$;
SELECT rb_exsum(5);

-- ternary
CREATE FUNCTION rb_sign(x int) RETURNS text LANGUAGE plxruby AS $$
return x >= 0 ? "nonneg" : "neg"
$$;
SELECT rb_sign(-4), rb_sign(4);

-- inference of multiple types + interpolation
CREATE FUNCTION rb_fmt() RETURNS text LANGUAGE plxruby AS $$
label = "count"
n = 42
active = true
return "#{label}=#{n} active=#{active}"
$$;
SELECT rb_fmt();

-- numeric annotation + interpolation
CREATE FUNCTION rb_interest(principal numeric) RETURNS text LANGUAGE plxruby AS $$
rate = 0.05 #:: numeric
amount #:: numeric
amount = principal * rate
return "interest: #{amount}"
$$;
SELECT rb_interest(1000);

-- SETOF via emit
CREATE FUNCTION rb_rows(n int) RETURNS TABLE(idx int, label text) LANGUAGE plxruby AS $$
for i in 1..n
  idx = i
  label = "row#{i}"
  emit
end
$$;
SELECT * FROM rb_rows(3);

-- query iteration and fetch_one against a table
CREATE TABLE rb_orders(grp int, amount bigint);
INSERT INTO rb_orders VALUES (1,10),(1,20),(1,30),(2,5);
CREATE FUNCTION rb_grp_total(g int) RETURNS bigint LANGUAGE plxruby AS $$
total = 0 #:: bigint
query("SELECT amount FROM rb_orders WHERE grp = #{g}").each do |row|
  total = total + row.amount
end
return total
$$;
SELECT rb_grp_total(1), rb_grp_total(2);

CREATE TABLE rb_users(id int, name text);
INSERT INTO rb_users VALUES (1,'Alice'),(2,'Bob');
CREATE FUNCTION rb_name(uid int) RETURNS text LANGUAGE plxruby AS $$
u = fetch_one("SELECT id, name FROM rb_users WHERE id = #{uid}")
return u.name
$$;
SELECT rb_name(2);

-- return_query
CREATE TABLE rb_vip(id int);
INSERT INTO rb_vip VALUES (3),(1),(2);
CREATE FUNCTION rb_vips() RETURNS SETOF int LANGUAGE plxruby AS $$
return_query("SELECT id FROM rb_vip ORDER BY id")
$$;
SELECT * FROM rb_vips();

-- begin / rescue
CREATE FUNCTION rb_safediv(d int) RETURNS int LANGUAGE plxruby AS $$
begin
  return 100 / d
rescue => e
  raise notice: "caught: #{e.message}"
  return -1
end
$$;
SELECT rb_safediv(4), rb_safediv(0);

-- raise with errcode
CREATE FUNCTION rb_checkpos(v int) RETURNS int LANGUAGE plxruby AS $$
if v < 0
  raise exception: "negative: #{v}", errcode: "22023"
end
return v
$$;
SELECT rb_checkpos(7);

-- DO block (body is Ruby)
DO LANGUAGE plxruby $$
raise warning: "plxruby do-block ran: #{6 * 7}"
$$;
