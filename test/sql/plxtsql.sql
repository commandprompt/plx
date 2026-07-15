-- plxtsql regression tests (Transact-SQL / SQL Server dialect)
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;

-- inline DECLARE hoist, SET, SET +=, WHILE BEGIN/END
CREATE FUNCTION tq_fact(n int) RETURNS bigint LANGUAGE plxtsql AS $$
  DECLARE @i int = 1;
  DECLARE @acc bigint = 1;
  WHILE @i <= @n
  BEGIN
    SET @acc = @acc * @i;
    SET @i += 1;
  END
  RETURN @acc;
$$;
SELECT tq_fact(0) AS f0, tq_fact(5) AS f120, tq_fact(10) AS f3628800;

-- IF / ELSE IF / ELSE, || concat, CONVERT(type, x) -> CAST(x AS type)
CREATE FUNCTION tq_grade(score int) RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @g varchar(10);
  IF @score >= 90
    SET @g = 'A';
  ELSE IF @score >= 80
    SET @g = 'B';
  ELSE
    SET @g = 'F';
  RETURN @g || ' (' || CONVERT(varchar, @score) || ')';
$$;
SELECT tq_grade(95) AS a, tq_grade(85) AS b, tq_grade(50) AS f;

-- multiple DECLAREs on one line, IIF -> CASE, ISNULL -> coalesce, LEN, CAST
CREATE FUNCTION tq_parity(n int) RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @r varchar(20) = IIF(@n % 2 = 0, 'even', 'odd');
  RETURN ISNULL(@r, '?') || ':' || CAST(LEN(@r) AS varchar);
$$;
SELECT tq_parity(4) AS even4, tq_parity(7) AS odd3;

-- SET @x = <query> via SELECT-assignment with FROM -> SELECT ... INTO
CREATE FUNCTION tq_count() RETURNS int LANGUAGE plxtsql AS $$
  DECLARE @c int;
  SELECT @c = count(*) FROM (VALUES (1),(2),(3),(4)) AS v(x);
  RETURN @c;
$$;
SELECT tq_count() AS should_be_4;

-- SELECT-assignment with NO from clause is a plain assignment list
CREATE FUNCTION tq_multi() RETURNS int LANGUAGE plxtsql AS $$
  DECLARE @a int;
  DECLARE @b int;
  SELECT @a = 10, @b = 32;
  RETURN @a + @b;
$$;
SELECT tq_multi() AS should_be_42;

-- WHILE with BREAK and CONTINUE
CREATE FUNCTION tq_sum_evens(n int) RETURNS int LANGUAGE plxtsql AS $$
  DECLARE @i int = 0;
  DECLARE @s int = 0;
  WHILE 1 = 1
  BEGIN
    SET @i += 1;
    IF @i > @n
      BREAK;
    IF @i % 2 = 1
      CONTINUE;
    SET @s += @i;
  END
  RETURN @s;
$$;
SELECT tq_sum_evens(10) AS should_be_30;

-- BEGIN TRY / END TRY / BEGIN CATCH / END CATCH -> BEGIN..EXCEPTION..END,
-- THROW num, msg, state -> RAISE EXCEPTION, ERROR_MESSAGE() -> SQLERRM
CREATE FUNCTION tq_try(n int) RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @msg varchar(100);
  BEGIN TRY
    IF @n < 0
      THROW 50000, 'negative input', 1;
    SET @msg = 'ok';
  END TRY
  BEGIN CATCH
    SET @msg = 'caught: ' || ERROR_MESSAGE();
  END CATCH
  RETURN @msg;
$$;
SELECT tq_try(1) AS ok, tq_try(-1) AS caught;

-- RAISERROR (message is the first argument)
CREATE FUNCTION tq_raise(n int) RETURNS int LANGUAGE plxtsql AS $$
  IF @n = 0
    RAISERROR('cannot be zero', 16, 1);
  RETURN 100 / @n;
$$;
SELECT tq_raise(4) AS should_be_25;
SELECT tq_raise(0) AS boom;

-- CHARINDEX(sub, str) -> strpos(str, sub) (argument order swap)
CREATE FUNCTION tq_find(hay text, needle text) RETURNS int LANGUAGE plxtsql AS $$
  RETURN CHARINDEX(@needle, @hay);
$$;
SELECT tq_find('hello world', 'world') AS should_be_7;

-- an outer BEGIN..END wrapper around the whole body is unwrapped
CREATE FUNCTION tq_wrapped(n int) RETURNS int LANGUAGE plxtsql AS $$
BEGIN
  DECLARE @x int = @n * 2;
  RETURN @x + 1;
END
$$;
SELECT tq_wrapped(20) AS should_be_41;

-- set-returning: a bare SELECT becomes RETURN QUERY
CREATE FUNCTION tq_series(n int) RETURNS TABLE(k int) LANGUAGE plxtsql AS $$
  SELECT g FROM generate_series(1, @n) AS g;
$$;
SELECT string_agg(k::text, ',') AS series FROM tq_series(4);

-- SET session options (SET NOCOUNT ON) are accepted and ignored
CREATE FUNCTION tq_nocount() RETURNS int LANGUAGE plxtsql AS $$
  SET NOCOUNT ON;
  DECLARE @x int = 7;
  RETURN @x;
$$;
SELECT tq_nocount() AS should_be_7;

-- a raw DML statement passes through (with @vars stripped)
CREATE TABLE tq_log(id serial, note text);
CREATE FUNCTION tq_insert(msg text) RETURNS int LANGUAGE plxtsql AS $$
  DECLARE @n text = @msg;
  INSERT INTO tq_log(note) VALUES (@n);
  RETURN (SELECT count(*)::int FROM tq_log);
$$;
SELECT tq_insert('first') AS should_be_1, tq_insert('second') AS should_be_2;

DROP TABLE tq_log CASCADE;

-- trigger: mutate NEW fields with SET NEW.col = e (session options are ignored)
CREATE TABLE tq_trg(id int, qty int, price numeric, total numeric, tag text);
CREATE FUNCTION tq_stamp() RETURNS trigger LANGUAGE plxtsql AS $$
  SET NOCOUNT ON;
  SET NEW.total = NEW.qty * NEW.price;
  SET NEW.tag = 'row ' || CONVERT(varchar, NEW.id);
  RETURN NEW;
$$;
CREATE TRIGGER tq_trg_ins BEFORE INSERT ON tq_trg
  FOR EACH ROW EXECUTE FUNCTION tq_stamp();
INSERT INTO tq_trg(id, qty, price) VALUES (7, 3, 10);
SELECT id, total, tag FROM tq_trg;
DROP TABLE tq_trg CASCADE;
