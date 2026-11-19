-- Regression tests for bugs found by the 2026-11-19 audit (fix_tsqlplsql).
-- Each case names its audit finding (A<n>).
\set VERBOSITY terse
-- A42: PL/SQL SELECT INTO and EXECUTE IMMEDIATE INTO raise NO_DATA_FOUND and TOO_MANY_ROWS
CREATE TABLE fx42_amt(v int);
INSERT INTO fx42_amt VALUES (1), (2), (3);
CREATE FUNCTION fx42_sel(lim int) RETURNS int LANGUAGE plxplsql AS $$
  x NUMBER;
BEGIN
  SELECT v INTO x FROM fx42_amt WHERE v > lim;
  RETURN x;
EXCEPTION
  WHEN NO_DATA_FOUND THEN
    RETURN -1;
  WHEN TOO_MANY_ROWS THEN
    RETURN -2;
END;
$$;
SELECT fx42_sel(2) AS one, fx42_sel(100) AS none, fx42_sel(0) AS many;
CREATE FUNCTION fx42_dyn(lim int) RETURNS int LANGUAGE plxplsql AS $$
  x NUMBER;
BEGIN
  EXECUTE IMMEDIATE 'SELECT v FROM fx42_amt WHERE v > $1' INTO x USING lim;
  RETURN x;
EXCEPTION
  WHEN NO_DATA_FOUND THEN
    RETURN -1;
  WHEN TOO_MANY_ROWS THEN
    RETURN -2;
END;
$$;
SELECT fx42_dyn(2) AS one, fx42_dyn(100) AS none, fx42_dyn(0) AS many;
CREATE FUNCTION fx42_case(lim int) RETURNS text LANGUAGE plxplsql AS $$
  s VARCHAR2(10);
BEGIN
  IF lim >= 0 THEN
    SELECT CASE WHEN v > 2 THEN 'big' ELSE 'small' END INTO s FROM fx42_amt WHERE v > lim;
  END IF;
  RETURN s;
EXCEPTION
  WHEN TOO_MANY_ROWS THEN
    RETURN 'many';
END;
$$;
SELECT fx42_case(2) AS one, fx42_case(0) AS many;
-- A43: SELECT @v = e FROM keeps @v when no row matches, takes the last row, and never splits a long list
CREATE TABLE fx43_amt(v int);
INSERT INTO fx43_amt VALUES (1), (2), (3);
CREATE FUNCTION fx43_keep(lim int) RETURNS int LANGUAGE plxtsql AS $$
  DECLARE @a int = 7;
  SELECT @a = v FROM fx43_amt WHERE v > @lim ORDER BY v;
  RETURN @a;
$$;
SELECT fx43_keep(100) AS keeps, fx43_keep(0) AS last_row;
CREATE FUNCTION fx43_two() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @a int = 0, @b varchar(10) = 'none';
  SELECT @a = v * 10, @b = CAST(v AS varchar(10)) FROM fx43_amt WHERE v < 3 ORDER BY v;
  RETURN CAST(@a AS varchar(10)) || '/' || @b;
$$;
SELECT fx43_two();
CREATE FUNCTION fx43_acc() RETURNS int LANGUAGE plxtsql AS $$
  DECLARE @t int = 0;
  SELECT @t = @t + v FROM fx43_amt;
  RETURN @t;
$$;
CREATE FUNCTION fx43_trail() RETURNS int LANGUAGE plxtsql AS $$ DECLARE @a int, @b int; SELECT @a = 1, @b$$;
CREATE FUNCTION fx43_65() RETURNS int LANGUAGE plxtsql AS $$ DECLARE @v0 int, @v1 int, @v2 int, @v3 int, @v4 int, @v5 int, @v6 int, @v7 int, @v8 int, @v9 int, @v10 int, @v11 int, @v12 int, @v13 int, @v14 int, @v15 int, @v16 int, @v17 int, @v18 int, @v19 int, @v20 int, @v21 int, @v22 int, @v23 int, @v24 int, @v25 int, @v26 int, @v27 int, @v28 int, @v29 int, @v30 int, @v31 int, @v32 int, @v33 int, @v34 int, @v35 int, @v36 int, @v37 int, @v38 int, @v39 int, @v40 int, @v41 int, @v42 int, @v43 int, @v44 int, @v45 int, @v46 int, @v47 int, @v48 int, @v49 int, @v50 int, @v51 int, @v52 int, @v53 int, @v54 int, @v55 int, @v56 int, @v57 int, @v58 int, @v59 int, @v60 int, @v61 int, @v62 int, @v63 int, @v64 int; SET @v0 = -1; SELECT @v0 = 0, @v1 = 1, @v2 = 2, @v3 = 3, @v4 = 4, @v5 = 5, @v6 = 6, @v7 = 7, @v8 = 8, @v9 = 9, @v10 = 10, @v11 = 11, @v12 = 12, @v13 = 13, @v14 = 14, @v15 = 15, @v16 = 16, @v17 = 17, @v18 = 18, @v19 = 19, @v20 = 20, @v21 = 21, @v22 = 22, @v23 = 23, @v24 = 24, @v25 = 25, @v26 = 26, @v27 = 27, @v28 = 28, @v29 = 29, @v30 = 30, @v31 = 31, @v32 = 32, @v33 = 33, @v34 = 34, @v35 = 35, @v36 = 36, @v37 = 37, @v38 = 38, @v39 = 39, @v40 = 40, @v41 = 41, @v42 = 42, @v43 = 43, @v44 = 44, @v45 = 45, @v46 = 46, @v47 = 47, @v48 = 48, @v49 = 49, @v50 = 50, @v51 = 51, @v52 = 52, @v53 = 53, @v54 = 54, @v55 = 55, @v56 = 56, @v57 = 57, @v58 = 58, @v59 = 59, @v60 = 60, @v61 = 61, @v62 = 62, @v63 = 63, @v64 = 64 FROM fx43_amt WHERE v > 100; RETURN @v0; $$;
SELECT fx43_65();
-- A44: [bracketed] and "quoted" identifiers keep their delimiters
CREATE TABLE fx44_acct("user" text, "Name" text, name text, "odd ]""col" text);
INSERT INTO fx44_acct VALUES ('alice', 'UPPER', 'lower', 'odd');
CREATE TABLE "fx44 order details"(qty int);
INSERT INTO "fx44 order details" VALUES (5);
CREATE FUNCTION fx44_q() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @u varchar(50), @n varchar(50), @o varchar(50), @q [int];
  SELECT @u = [user], @n = "Name", @o = [odd ]]"col] FROM fx44_acct;
  SELECT @q = qty FROM [fx44 Order Details];
  RETURN @u || '/' || @n || '/' || @o || '/' || CONVERT([varchar](10), @q);
$$;
SELECT fx44_q();
CREATE FUNCTION fx44_bad() RETURNS int LANGUAGE plxtsql AS $$ RETURN [abc $$;
-- A45: ^ is bitwise XOR and LEN() ignores trailing spaces
CREATE FUNCTION fx45_xor(a int, b int) RETURNS int LANGUAGE plxtsql AS $$
  RETURN @a ^ @b;
$$;
CREATE FUNCTION fx45_len(s varchar) RETURNS int LANGUAGE plxtsql AS $$
  RETURN LEN(@s);
$$;
SELECT fx45_xor(5, 3) AS xor_6, fx45_len('abc   ') AS len_3, fx45_len('  ab') AS len_4;
-- A46: RAISERROR severity 10 or lower prints a notice, and WITH options are consumed
CREATE FUNCTION fx46_progress(n int) RETURNS int LANGUAGE plxtsql AS $$
  DECLARE @i int = 0;
  WHILE @i < @n
  BEGIN
    RAISERROR('step', 0, 1) WITH NOWAIT;
    SET @i += 1;
  END
  RETURN @i;
$$;
SELECT fx46_progress(3);
CREATE FUNCTION fx46_info_try() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @r varchar(20) = 'no catch';
  BEGIN TRY
    RAISERROR('progress', 10, 1);
  END TRY
  BEGIN CATCH
    SET @r = 'caught';
  END CATCH
  RETURN @r;
$$;
SELECT fx46_info_try();
CREATE FUNCTION fx46_raise_nowait(n int) RETURNS int LANGUAGE plxtsql AS $$
  IF @n = 0
    RAISERROR('cannot be zero', 16, 1) WITH NOWAIT;
  RETURN 100 / @n;
$$;
SELECT fx46_raise_nowait(4);
SELECT fx46_raise_nowait(0);
CREATE FUNCTION fx46_sev(s int) RETURNS text LANGUAGE plxtsql AS $$
  RAISERROR('sev', @s, 1);
  RETURN 'continued';
$$;
SELECT fx46_sev(0);
SELECT fx46_sev(16);
SELECT fx46_sev(-1);
SELECT fx46_sev(NULL);
CREATE FUNCTION fx46_neg() RETURNS int LANGUAGE plxtsql AS $$
  RAISERROR('neg', -5, 1);
  RETURN 1;
$$;
SELECT fx46_neg();
CREATE FUNCTION fx46_neg1() RETURNS int LANGUAGE plxtsql AS $$
  RAISERROR('neg1', -1, 1);
  RETURN 1;
$$;
SELECT fx46_neg1();
CREATE FUNCTION fx46_log() RETURNS int LANGUAGE plxtsql AS $$
  RAISERROR('logged', 0, 1) WITH LOG, SETERROR;
  RETURN 1;
$$;
SELECT position('RAISE LOG' in prosrc) > 0 AS has_log FROM pg_proc WHERE proname = 'fx46_log';
SELECT fx46_log();
CREATE FUNCTION fx46_foo() RETURNS int LANGUAGE plxtsql AS $$
  RAISERROR('x', 0, 1) WITH FOO;
  RETURN 1;
$$;
CREATE FUNCTION fx46_rethrow() RETURNS int LANGUAGE plxtsql AS $$
  BEGIN TRY
    RAISERROR('inner', 16, 1);
  END TRY
  BEGIN CATCH
    RAISERROR(ERROR_MESSAGE(), ERROR_SEVERITY(), ERROR_STATE());
  END CATCH
  RETURN 1;
$$;
SELECT fx46_rethrow();
CREATE FUNCTION fx46_with() RETURNS int LANGUAGE plxtsql AS $$
  RAISERROR('x', 0, 1) WITH;
  RETURN 1;
$$;
-- A47: CASE ... END inside an expression no longer ends the statement or block
CREATE FUNCTION fx47_case(a int) RETURNS int LANGUAGE plxtsql AS $$
  DECLARE @x int;
  DECLARE @y int = CASE WHEN @a > 10 THEN 100 ELSE 0 END;
  SET @x = CASE WHEN @a > 0 THEN 1 ELSE 0 END;
  IF CASE WHEN @a > 5 THEN 1 ELSE 0 END = 1
  BEGIN
    SET @x = @x + CASE @a WHEN 6 THEN 10 WHEN 7 THEN 20 ELSE 30 END;
  END
  ELSE
    SET @x = @x + 1000;
  RETURN @x + @y;
$$;
CREATE FUNCTION fx47_ret(a int) RETURNS int LANGUAGE plxtsql AS $$
  RETURN CASE WHEN @a > 0 THEN 1 END;
$$;
SELECT fx47_case(5) AS c5, fx47_case(6) AS c6, fx47_case(20) AS c20, fx47_case(-1) AS cm1, fx47_ret(-3) AS rm3, fx47_ret(3) AS r3;
CREATE FUNCTION fx47_sel() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(10);
  SELECT @s = CASE WHEN v > 2 THEN 'big' ELSE 'small' END FROM fx43_amt WHERE v = 3;
  RETURN @s;
$$;
SELECT fx47_sel();
-- A48: unbalanced call parentheses transpile in linear time
SET statement_timeout = '5s';
SET check_function_bodies = off;
DO $d$ BEGIN EXECUTE 'CREATE FUNCTION fx48_quad() RETURNS void LANGUAGE plxtsql AS $plx$DECLARE @x int = 1; SET @x = ' || repeat('f(', 160000) || ';$plx$'; END $d$;
RESET check_function_bodies;
RESET statement_timeout;
-- N-plsql-quadratic-paren: unbalanced DBMS_OUTPUT.PUT_LINE( calls transpile in linear time
SET statement_timeout = '5s';
SET check_function_bodies = off;
DO $d$ BEGIN EXECUTE 'CREATE FUNCTION fxn_plquad() RETURNS void LANGUAGE plxplsql AS $plx$BEGIN ' || repeat('DBMS_OUTPUT.PUT_LINE(', 80000) || '$plx$'; END $d$;
DO $d$ BEGIN EXECUTE 'CREATE FUNCTION fxn_plquad2() RETURNS void LANGUAGE plxplsql AS $plx$BEGIN ' || repeat('RAISE_APPLICATION_ERROR(', 80000) || '$plx$'; END $d$;
RESET check_function_bodies;
RESET statement_timeout;
-- A43: the self-read error names the entry that reads the variable
CREATE FUNCTION fx43_names() RETURNS int LANGUAGE plxtsql AS $$
  DECLARE @a int, @b int;
  SELECT @a = v, @b = @a * 2 FROM fx43_amt;
  RETURN @b;
$$;
-- A47: an IF condition holding IIF, which becomes CASE ... THEN, still works
CREATE FUNCTION fx47_iif(a int) RETURNS int LANGUAGE plxtsql AS $$
  IF IIF(@a > 0, 1, 0) = 1 RETURN 1;
  RETURN 0;
$$;
SELECT fx47_iif(5) AS pos, fx47_iif(-5) AS neg;
-- A42: STRICT goes only on a SELECT INTO, never on INSERT INTO, RETURNING INTO or an INTO that has STRICT
CREATE TABLE fx42_t2(v int);
CREATE FUNCTION fx42_wins() RETURNS int LANGUAGE plxplsql AS $$
  n NUMBER;
BEGIN
  WITH s AS (SELECT 5 AS v) INSERT INTO fx42_t2 SELECT v FROM s;
  SELECT count(*) INTO n FROM fx42_t2;
  RETURN n;
END;
$$;
SELECT fx42_wins();
CREATE FUNCTION fx42_ret() RETURNS text LANGUAGE plxplsql AS $$
  x NUMBER := 7;
  y NUMBER := 7;
BEGIN
  WITH s AS (SELECT 1 AS v) UPDATE fx42_amt SET v = v WHERE v > 100 RETURNING v INTO x;
  UPDATE fx42_amt SET v = v WHERE v > 100 RETURNING v INTO y;
  RETURN NVL(x, -1) || '/' || NVL(y, -1);
END;
$$;
SELECT fx42_ret();
CREATE FUNCTION fx42_has() RETURNS int LANGUAGE plxplsql AS $$
  x NUMBER;
BEGIN
  SELECT v INTO STRICT x FROM fx42_amt WHERE v = 1;
  EXECUTE IMMEDIATE 'SELECT 2' INTO STRICT x;
  RETURN x;
END;
$$;
SELECT fx42_has();
-- A44: [bracketed] names fold to lower case like unquoted names; "quoted" names keep their case and undo a doubled ""
CREATE TABLE fx44_orders(orderid int, "Say ""Hi""" text);
INSERT INTO fx44_orders VALUES (9, 'hello');
CREATE FUNCTION fx44_fold() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @o int, @s varchar(10);
  SELECT @o = [OrderID], @s = "Say ""Hi""" FROM [FX44_Orders];
  RETURN CONVERT(varchar(10), @o) || '/' || @s;
$$;
SELECT fx44_fold();
-- A48: nested balanced calls whose argument lists have no comma transpile in linear time
SET statement_timeout = '5s';
SET check_function_bodies = off;
DO $d$ BEGIN EXECUTE 'CREATE FUNCTION fx48_iif() RETURNS void LANGUAGE plxtsql AS $plx$DECLARE @x int = 1; SET @x = ' || repeat('IIF(', 160000) || '1' || repeat(')', 160000) || ';$plx$'; END $d$;
DO $d$ BEGIN EXECUTE 'CREATE FUNCTION fx48_conv() RETURNS void LANGUAGE plxtsql AS $plx$DECLARE @x int = 1; SET @x = ' || repeat('CONVERT(', 160000) || '1' || repeat(')', 160000) || ';$plx$'; END $d$;
DO $d$ BEGIN EXECUTE 'CREATE FUNCTION fx48_cidx() RETURNS void LANGUAGE plxtsql AS $plx$DECLARE @x int = 1; SET @x = ' || repeat('CHARINDEX(', 160000) || '1' || repeat(')', 160000) || ';$plx$'; END $d$;
-- N-plsql-quadratic-paren: nested balanced RAISE_APPLICATION_ERROR( with no comma transpiles in linear time
DO $d$ BEGIN EXECUTE 'CREATE FUNCTION fxn_plquad3() RETURNS void LANGUAGE plxplsql AS $plx$BEGIN ' || repeat('RAISE_APPLICATION_ERROR(', 160000) || '1' || repeat(')', 160000) || '; END;$plx$'; END $d$;
RESET check_function_bodies;
RESET statement_timeout;
-- A45: a bitwise operation in parentheses, as doc/plxtsql.md advises, keeps the T-SQL result
CREATE FUNCTION fx45_prec() RETURNS int LANGUAGE plxtsql AS $$
  RETURN (1 ^ 2) + 3;
$$;
SELECT fx45_prec();
-- tsql-plus-eq-string: += and + on a string-typed local concatenate, as in T-SQL; numeric locals still add
-- CHAR and NCHAR follow PostgreSQL: || drops the padding first, so @c ends as cy (SQL Server keeps it and truncates to c)
CREATE FUNCTION fxps_pluseq() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @v varchar(20) = 'a', @nv NVARCHAR(MAX) = 'b', @c char(3) = 'c', @nc nchar(2) = 'd', @t text = 'e', @nt ntext = 'f', @i int = 1, @m decimal(5,2) = 1.5;
  SET @v += 'x';
  SET @nv += @v + '!';
  SET @c += 'y';
  SET @nc += 'z';
  SET @t += 'w';
  SET @nt += 'v';
  SET @i += 2;
  SET @m += 1;
  RETURN @v + '|' + @nv + '|' + @c + '|' + @nc + '|' + @t + '|' + @nt + '|' + CONVERT(varchar(10), @i) + '|' + CONVERT(varchar(10), @m);
$$;
SELECT fxps_pluseq();
CREATE FUNCTION fxps_plus() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(20) = 'ab', @u varchar(20) = 'cd', @n int = 3;
  SET @s = @s + 'x';
  SET @s = @s + @u;
  SET @n = @n + 4;
  SET @s = @s + CAST(@n AS varchar(10));
  RETURN @s;
$$;
SELECT fxps_plus();
-- NULL propagates through += and + on a string local, as with CONCAT_NULL_YIELDS_NULL ON
CREATE FUNCTION fxps_null() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(20) = 'a', @z varchar(20);
  SET @s += @z;
  RETURN ISNULL(@s, 'null') + '/' + ISNULL(@z + 'q', 'null');
$$;
SELECT fxps_null();
-- a string parameter concatenates too; a number next to + keeps it an addition
CREATE FUNCTION fxps_mixed(p varchar) RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @d varchar(5) = '40', @n int = 1;
  SET @p += '!';
  SET @n = @n + LEN(@p) + CAST(@d AS int) * 2;
  RETURN @p + '-' + UPPER(@p) + '-' + CONVERT(varchar(10), '1' + 2) + '-' + CONVERT(varchar(10), @n);
$$;
SELECT fxps_mixed('hi');
-- review of 5a1465d: + becomes || only when every operand of the chain is a known string, and SET @s += e
-- only when e is one. Anything else stays +, so a string next to a number fails at call time ("operator
-- does not exist") instead of returning the concatenated value; SQL Server's answer is in each comment.
CREATE FUNCTION fxps_pe_num() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(10) = '5';
  SET @s += 1;
  RETURN @s;
$$;
SELECT fxps_pe_num();  -- SQL Server: 6
CREATE FUNCTION fxps_pe_var() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(10) = '5', @n int = 2;
  SET @s += @n;
  RETURN @s;
$$;
SELECT fxps_pe_var();  -- SQL Server: 7
CREATE FUNCTION fxps_un_len() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(10) = '5';
  RETURN @s + LEN(@s);
$$;
SELECT fxps_un_len();  -- SQL Server: 6
CREATE FUNCTION fxps_un_paren() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(10) = '5', @n int = 1;
  RETURN (@n + 1) + @s;
$$;
SELECT fxps_un_paren();  -- SQL Server: 7
CREATE TABLE fxps_t(qty int);
INSERT INTO fxps_t VALUES (2);
CREATE FUNCTION fxps_un_col() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(10) = '5', @r varchar(10);
  SELECT @r = qty + @s FROM fxps_t;
  RETURN @r;
$$;
SELECT fxps_un_col();  -- SQL Server: 7
CREATE FUNCTION fxps_un_bit() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(10) = '5', @b bit = 1;
  RETURN @s + @b;
$$;
SELECT fxps_un_bit();  -- SQL Server: 6
-- a number later in the chain: T-SQL groups (@n + '2') + @s and adds; || there would give 35
CREATE FUNCTION fxps_un_mid() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(10) = '5', @n int = 1;
  RETURN @n + '2' + @s;
$$;
SELECT fxps_un_mid();  -- SQL Server: 8
-- a - after the chain: T-SQL computes (@s + '0') - 1; || binds looser than - in PostgreSQL and would give 1-1
CREATE FUNCTION fxps_un_minus() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(10) = '1';
  RETURN @s + '0' - 1;
$$;
SELECT fxps_un_minus();  -- SQL Server: 9
DROP TABLE fxps_t;
