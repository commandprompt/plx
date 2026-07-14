-- plxpython3 regression tests
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;

-- scalar return
CREATE FUNCTION py_add(a int, b int) RETURNS int LANGUAGE plxpython3 AS $$
return a + b * 2
$$;
SELECT py_add(3, 4);

-- if / elif / else
CREATE FUNCTION py_grade(score int) RETURNS text LANGUAGE plxpython3 AS $$
if score >= 90:
    grade = "A"
elif score >= 80:
    grade = "B"
else:
    grade = "F"
return grade
$$;
SELECT py_grade(95), py_grade(85), py_grade(70);

-- for over range
CREATE FUNCTION py_sum(n int) RETURNS int LANGUAGE plxpython3 AS $$
total = 0
for i in range(1, n + 1):
    total = total + i
return total
$$;
SELECT py_sum(10);

-- range with a single argument (0-based)
CREATE FUNCTION py_sum0(n int) RETURNS int LANGUAGE plxpython3 AS $$
total = 0
for i in range(n):
    total = total + i
return total
$$;
SELECT py_sum0(5);

-- while, nested if, break
CREATE FUNCTION py_countdown(n int) RETURNS int LANGUAGE plxpython3 AS $$
c = 0
while n > 0:
    c = c + n
    n = n - 1
return c
$$;
SELECT py_countdown(5);

-- f-string interpolation
CREATE FUNCTION py_greet(nm text, n int) RETURNS text LANGUAGE plxpython3 AS $$
return f"user {nm} has {n} items"
$$;
SELECT py_greet('bob', 3);

-- foreach over an array
CREATE FUNCTION py_arr(a int[]) RETURNS int LANGUAGE plxpython3 AS $$
total = 0
v #:: int
for v in a:
    total = total + v
return total
$$;
SELECT py_arr(ARRAY[3,4,5]);

-- query iteration with an f-string
CREATE TABLE py_orders(grp int, amount bigint);
INSERT INTO py_orders VALUES (1,10),(1,20),(1,30),(2,5);
CREATE FUNCTION py_grp_total(want int) RETURNS bigint LANGUAGE plxpython3 AS $$
total = 0 #:: bigint
for row in query(f"SELECT amount FROM py_orders WHERE grp = {want}"):
    total = total + row.amount
return total
$$;
SELECT py_grp_total(1);

-- fetch_one
CREATE TABLE py_users(id int, name text);
INSERT INTO py_users VALUES (1,'Alice'),(2,'Bob');
CREATE FUNCTION py_name(uid int) RETURNS text LANGUAGE plxpython3 AS $$
u = fetch_one(f"SELECT id, name FROM py_users WHERE id = {uid}")
return u.name
$$;
SELECT py_name(2);

-- set-returning
CREATE FUNCTION py_squares(n int) RETURNS SETOF int LANGUAGE plxpython3 AS $$
for i in range(1, n + 1):
    return_next(i * i)
return
$$;
SELECT * FROM py_squares(3);

-- assert (statement form)
CREATE FUNCTION py_assert(n int) RETURNS int LANGUAGE plxpython3 AS $$
assert n > 0, "must be positive"
return n
$$;
SELECT py_assert(5);

-- try / except / finally with a raise
CREATE FUNCTION py_safediv(d int) RETURNS int LANGUAGE plxpython3 AS $$
try:
    return 100 / d
except Exception as e:
    return -1
$$;
SELECT py_safediv(4), py_safediv(0);

-- pass
CREATE FUNCTION py_pass(n int) RETURNS int LANGUAGE plxpython3 AS $$
if n > 0:
    pass
else:
    n = 0
return n
$$;
SELECT py_pass(5), py_pass(-3);

-- labeled loop
CREATE FUNCTION py_label(n int) RETURNS int LANGUAGE plxpython3 AS $$
count = 0
outer: for i in range(1, n + 1):
    for j in range(1, n + 1):
        count = count + 1
        break outer
return count
$$;
SELECT py_label(5);
