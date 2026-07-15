-- plxplsql regression tests (Oracle PL/SQL dialect)
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;

-- types (VARCHAR2), IF/ELSIF/ELSE, assignment, || , RETURN.
-- Note: the function signature uses PostgreSQL types; the body is PL/SQL.
CREATE FUNCTION pls_grade(score numeric) RETURNS text LANGUAGE plxplsql AS $$
  result VARCHAR2(10);
BEGIN
  IF score >= 90 THEN
    result := 'A';
  ELSIF score >= 80 THEN
    result := 'B';
  ELSE
    result := 'F';
  END IF;
  RETURN result || '!';
END;
$$;
SELECT pls_grade(95), pls_grade(85), pls_grade(50);

-- integer FOR loop, PLS_INTEGER, NUMBER
CREATE FUNCTION pls_sum(n int) RETURNS bigint LANGUAGE plxplsql AS $$
  total PLS_INTEGER := 0;
  i NUMBER;
BEGIN
  FOR i IN 1..n LOOP
    total := total + i;
  END LOOP;
  RETURN total;
END;
$$;
SELECT pls_sum(100);

-- WHILE loop and CASE
CREATE FUNCTION pls_while(n int) RETURNS int LANGUAGE plxplsql AS $$
  i int := 0;
  s int := 0;
BEGIN
  WHILE i < n LOOP
    i := i + 1;
    s := s + CASE WHEN mod(i, 2) = 0 THEN 2 ELSE 1 END;
  END LOOP;
  RETURN s;
END;
$$;
SELECT pls_while(4);

-- NVL -> coalesce, DBMS_OUTPUT.PUT_LINE -> RAISE NOTICE, FROM DUAL removed
CREATE FUNCTION pls_misc(x numeric) RETURNS text LANGUAGE plxplsql AS $$
  v numeric;
  d text;
BEGIN
  v := NVL(x, -1);
  SELECT 'ok' INTO d FROM DUAL;
  RETURN d || ':' || v;
END;
$$;
SELECT pls_misc(NULL), pls_misc(7);

-- DBMS_OUTPUT.PUT_LINE emits a NOTICE
CREATE FUNCTION pls_say(msg text) RETURNS void LANGUAGE plxplsql AS $$
BEGIN
  DBMS_OUTPUT.PUT_LINE('say: ' || msg);
END;
$$;
SET client_min_messages = notice;
SELECT pls_say('hi');
SET client_min_messages = warning;

-- RAISE_APPLICATION_ERROR + EXCEPTION handler
CREATE FUNCTION pls_div(a numeric, b numeric) RETURNS numeric LANGUAGE plxplsql AS $$
BEGIN
  IF b = 0 THEN
    RAISE_APPLICATION_ERROR(-20001, 'cannot divide by zero');
  END IF;
  RETURN a / b;
EXCEPTION
  WHEN OTHERS THEN
    RETURN -1;
END;
$$;
SELECT pls_div(10, 2), pls_div(10, 0);

-- EXECUTE IMMEDIATE with USING and INTO
CREATE FUNCTION pls_dyn(minv int) RETURNS bigint LANGUAGE plxplsql AS $$
  c bigint;
BEGIN
  EXECUTE IMMEDIATE
    'SELECT count(*) FROM (VALUES (10),(20),(30)) t(a) WHERE a >= $1'
    INTO c USING minv;
  RETURN c;
END;
$$;
SELECT pls_dyn(20);

-- sequence NEXTVAL / CURRVAL
CREATE SEQUENCE pls_seq START 100;
CREATE FUNCTION pls_seqnext() RETURNS bigint LANGUAGE plxplsql AS $$
BEGIN
  RETURN pls_seq.NEXTVAL;
END;
$$;
SELECT pls_seqnext() AS n100, pls_seqnext() AS n101;

-- explicit cursor: CURSOR c IS ...; OPEN/FETCH/CLOSE; EXIT WHEN NOT FOUND
CREATE FUNCTION pls_cursor() RETURNS bigint LANGUAGE plxplsql AS $$
  CURSOR c IS SELECT a FROM (VALUES (1),(2),(3)) t(a) ORDER BY a;
  v bigint;
  total bigint := 0;
BEGIN
  OPEN c;
  LOOP
    FETCH c INTO v;
    EXIT WHEN NOT FOUND;
    total := total + v;
  END LOOP;
  CLOSE c;
  RETURN total;
END;
$$;
SELECT pls_cursor();

-- body that opens directly with BEGIN (no declarations)
CREATE FUNCTION pls_nodecl(a int, b int) RETURNS int LANGUAGE plxplsql AS $$
BEGIN
  RETURN a + b;
END;
$$;
SELECT pls_nodecl(2, 3);

-- SYSDATE transpiles to LOCALTIMESTAMP (value not asserted, just that it runs)
CREATE FUNCTION pls_today() RETURNS boolean LANGUAGE plxplsql AS $$
  d timestamp;
BEGIN
  d := SYSDATE;
  RETURN d IS NOT NULL;
END;
$$;
SELECT pls_today();

-- set-returning function with RETURN QUERY (plpgsql, passes through)
CREATE FUNCTION pls_squares(n int) RETURNS SETOF int LANGUAGE plxplsql AS $$
BEGIN
  RETURN QUERY SELECT (i * i)::int FROM generate_series(1, n) i;
END;
$$;
SELECT array_agg(x) FROM pls_squares(4) x;

-- type names are only translated in the declaration section, so a body
-- reference to a column named like an Oracle type is left alone
CREATE FUNCTION pls_colname() RETURNS int LANGUAGE plxplsql AS $$
  n int;
BEGIN
  SELECT number INTO n FROM (VALUES (42)) t(number);
  RETURN n;
END;
$$;
SELECT pls_colname() AS should_be_42;

-- the generated plpgsql preserves the source layout
SELECT prosrc FROM pg_proc WHERE proname = 'pls_grade';
