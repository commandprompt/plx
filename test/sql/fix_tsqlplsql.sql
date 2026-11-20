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
CREATE FUNCTION fxps_pluseq() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @v varchar(20) = 'a', @nv NVARCHAR(MAX) = 'b', @t text = 'e', @nt ntext = 'f', @i int = 1, @m decimal(5,2) = 1.5;
  SET @v += 'x';
  SET @nv += @v + '!';
  SET @t += 'w';
  SET @nt += 'v';
  SET @i += 2;
  SET @m += 1;
  RETURN @v + '|' + @nv + '|' + @t + '|' + @nt + '|' + CONVERT(varchar(10), @i) + '|' + CONVERT(varchar(10), @m);
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
-- ft_err(q) runs q and returns 'SQLSTATE: message' if it fails. The message
-- leaves out the error position, which differs between PostgreSQL 13
-- (plpgsql prefixes each expression with SELECT) and later versions.
CREATE FUNCTION ft_err(q text) RETURNS text LANGUAGE plpgsql AS $h$
BEGIN
  EXECUTE q;
  RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
  RETURN SQLSTATE || ': ' || SQLERRM;
END $h$;
SELECT ft_err($q$SELECT fxps_pe_num()$q$);  -- SQL Server: 6
CREATE FUNCTION fxps_pe_var() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(10) = '5', @n int = 2;
  SET @s += @n;
  RETURN @s;
$$;
SELECT ft_err($q$SELECT fxps_pe_var()$q$);  -- SQL Server: 7
CREATE FUNCTION fxps_un_len() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(10) = '5';
  RETURN @s + LEN(@s);
$$;
SELECT ft_err($q$SELECT fxps_un_len()$q$);  -- SQL Server: 6
CREATE FUNCTION fxps_un_paren() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(10) = '5', @n int = 1;
  RETURN (@n + 1) + @s;
$$;
SELECT ft_err($q$SELECT fxps_un_paren()$q$);  -- SQL Server: 7
CREATE TABLE fxps_t(qty int);
INSERT INTO fxps_t VALUES (2);
CREATE FUNCTION fxps_un_col() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(10) = '5', @r varchar(10);
  SELECT @r = qty + @s FROM fxps_t;
  RETURN @r;
$$;
SELECT ft_err($q$SELECT fxps_un_col()$q$);  -- SQL Server: 7
CREATE FUNCTION fxps_un_bit() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(10) = '5', @b bit = 1;
  RETURN @s + @b;
$$;
SELECT ft_err($q$SELECT fxps_un_bit()$q$);  -- SQL Server: 6
-- a number later in the chain: T-SQL groups (@n + '2') + @s and adds; || there would give 35
CREATE FUNCTION fxps_un_mid() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(10) = '5', @n int = 1;
  RETURN @n + '2' + @s;
$$;
SELECT ft_err($q$SELECT fxps_un_mid()$q$);  -- SQL Server: 8
-- a - after the chain: T-SQL computes (@s + '0') - 1; || binds looser than - in PostgreSQL and would give 1-1
CREATE FUNCTION fxps_un_minus() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(10) = '1';
  RETURN @s + '0' - 1;
$$;
SELECT ft_err($q$SELECT fxps_un_minus()$q$);  -- SQL Server: 9
DROP TABLE fxps_t;
-- review of 641976f: COALESCE is a known string only when every argument is one. SQL Server's COALESCE
-- takes the type of its highest-precedence argument, and PostgreSQL resolves COALESCE('1', 2) to integer,
-- so these add or fail; || would give 13 (51 for +=). SQL Server's answer is in each comment.
CREATE FUNCTION fxco_int() RETURNS text LANGUAGE plxtsql AS $$
  RETURN COALESCE('1', 2) + '3';
$$;
SELECT fxco_int() AS r;  -- SQL Server: 4
CREATE FUNCTION fxco_var() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @n int = 2;
  RETURN COALESCE('1', @n) + '3';
$$;
SELECT fxco_var() AS r;  -- SQL Server: 4
CREATE FUNCTION fxco_num() RETURNS text LANGUAGE plxtsql AS $$
  RETURN COALESCE('1', 2.5) + '3';
$$;
SELECT fxco_num() AS r;  -- SQL Server: 4.0
CREATE FUNCTION fxco_n() RETURNS text LANGUAGE plxtsql AS $$
  RETURN COALESCE(N'1', 2) + N'3';
$$;
SELECT fxco_n() AS r;  -- SQL Server: 4
CREATE TABLE fxco_t(qty int);
INSERT INTO fxco_t VALUES (2);
CREATE FUNCTION fxco_col() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @r varchar(10);
  SELECT @r = COALESCE('1', qty) + '3' FROM fxco_t;
  RETURN @r;
$$;
SELECT fxco_col() AS r;  -- SQL Server: 4
DROP TABLE fxco_t;
CREATE FUNCTION fxco_pe() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s varchar(10) = '5';
  SET @s += COALESCE('1', 2);
  RETURN @s;
$$;
SELECT ft_err($q$SELECT fxco_pe()$q$);  -- SQL Server: 6
-- COALESCE whose every argument is a known string still concatenates
CREATE FUNCTION fxco_str() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @z varchar(10), @s varchar(10) = 'b';
  RETURN COALESCE(@z, 'a', @s) + COALESCE(@s, @z) + COALESCE(@z, UPPER(@s) + 'c');
$$;
SELECT fxco_str() AS r;  -- SQL Server: abBc
-- review of 641976f: a fixed-length CHAR(n) or NCHAR(n) value is not a known string. SQL Server keeps its
-- trailing spaces when it concatenates, but PostgreSQL's || drops them, so || would give a different value
-- without an error. + stays +, and the call fails ("operator does not exist"). SQL Server's answer is in each comment.
CREATE FUNCTION fxch_cast() RETURNS text LANGUAGE plxtsql AS $$
  RETURN CAST('a' AS char(3)) + 'b';
$$;
SELECT ft_err($q$SELECT fxch_cast()$q$);  -- SQL Server: 'a  b'
CREATE FUNCTION fxch_conv() RETURNS text LANGUAGE plxtsql AS $$
  RETURN CONVERT(char(3), 'a') + 'b';
$$;
SELECT ft_err($q$SELECT fxch_conv()$q$);  -- SQL Server: 'a  b'
CREATE FUNCTION fxch_castn() RETURNS text LANGUAGE plxtsql AS $$
  RETURN CAST('' AS nchar) + 'b';
$$;
SELECT ft_err($q$SELECT fxch_castn()$q$);  -- SQL Server: ' b' (NCHAR without a length is NCHAR(1))
CREATE FUNCTION fxch_var() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s nchar(2) = 'a';
  RETURN @s + 'b';
$$;
SELECT ft_err($q$SELECT fxch_var()$q$);  -- SQL Server: 'a b'
CREATE FUNCTION fxch_par(p char(3)) RETURNS text LANGUAGE plxtsql AS $$
  RETURN @p + 'b';
$$;
SELECT ft_err($q$SELECT fxch_par('a')$q$);  -- SQL Server: 'a  b'
CREATE FUNCTION fxch_pe() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @c char(3) = 'c';
  SET @c += 'y';
  RETURN @c;
$$;
SELECT ft_err($q$SELECT fxch_pe()$q$);  -- SQL Server: 'c  ' (the result cut to 3 characters)
-- T-SQL binds unary ~ tighter than any other operator, so ~a + b is (~a) + b. PostgreSQL binds prefix ~
-- looser than + and *, so ~ must apply to its operand only. SQL Server's answer is in each comment.
CREATE FUNCTION fxtl_lit() RETURNS int LANGUAGE plxtsql AS $$
  RETURN ~1 + '2';
$$;
SELECT fxtl_lit() AS r;  -- SQL Server: 0
CREATE FUNCTION fxtl_var() RETURNS int LANGUAGE plxtsql AS $$
  DECLARE @n int = 1;
  RETURN ~@n + 1;
$$;
SELECT fxtl_var() AS r;  -- SQL Server: -1
CREATE FUNCTION fxtl_call() RETURNS int LANGUAGE plxtsql AS $$
  RETURN ~ABS(-1) * 2;
$$;
SELECT fxtl_call() AS r;  -- SQL Server: -4
CREATE FUNCTION fxtl_paren() RETURNS int LANGUAGE plxtsql AS $$
  RETURN ~(1 + 1) + 1;
$$;
SELECT fxtl_paren() AS r;  -- SQL Server: -2
CREATE TABLE fxtl_t(qty int);
INSERT INTO fxtl_t VALUES (2);
CREATE FUNCTION fxtl_col() RETURNS int LANGUAGE plxtsql AS $$
  DECLARE @r int;
  SELECT @r = ~qty + 1 FROM fxtl_t;
  RETURN @r;
$$;
SELECT fxtl_col() AS r;  -- SQL Server: -2
DROP TABLE fxtl_t;
-- CAST maps a T-SQL type name the way CONVERT and DECLARE do. Before, CAST passed the name through, so
-- CAST(x AS nvarchar(5)) failed with 'type "nvarchar" does not exist'. SQL Server's answer is in each comment.
CREATE FUNCTION fxct_nvar() RETURNS text LANGUAGE plxtsql AS $$
  RETURN CAST('abcdefg' AS nvarchar(5));
$$;
SELECT fxct_nvar() AS r;  -- SQL Server: abcde
CREATE FUNCTION fxct_nvar30() RETURNS int LANGUAGE plxtsql AS $$
  RETURN LEN(CAST('12345678901234567890123456789012345' AS nvarchar));
$$;
SELECT fxct_nvar30() AS r;  -- SQL Server: 30 (CAST without a length gives 30 characters)
CREATE FUNCTION fxct_max() RETURNS text LANGUAGE plxtsql AS $$
  RETURN CAST('ab' AS varchar(MAX)) + CAST('cd' AS nvarchar(max));
$$;
SELECT fxct_max() AS r;  -- SQL Server: abcd
CREATE FUNCTION fxct_ntext() RETURNS text LANGUAGE plxtsql AS $$
  RETURN CAST('e' AS ntext);
$$;
SELECT fxct_ntext() AS r;  -- SQL Server: e
CREATE FUNCTION fxct_conv_max() RETURNS text LANGUAGE plxtsql AS $$
  RETURN CONVERT(varchar(MAX), 'ab') + CONVERT(nvarchar(MAX), 'cd');
$$;
SELECT fxct_conv_max() AS r;  -- SQL Server: abcd
CREATE FUNCTION fxct_dt() RETURNS timestamp LANGUAGE plxtsql AS $$
  RETURN CAST('2024-01-02 03:04:05.5' AS datetime);
$$;
SELECT fxct_dt() AS r;  -- SQL Server: 2024-01-02 03:04:05.500
CREATE FUNCTION fxct_dt2() RETURNS timestamp LANGUAGE plxtsql AS $$
  RETURN CAST('2024-01-02 03:04:05.5' AS datetime2(0));
$$;
SELECT fxct_dt2() AS r;  -- SQL Server: 2024-01-02 03:04:06
CREATE FUNCTION fxct_bit() RETURNS boolean LANGUAGE plxtsql AS $$
  RETURN CAST(2 AS bit);
$$;
SELECT fxct_bit() AS r;  -- SQL Server: 1 (any non-zero number becomes 1)
CREATE FUNCTION fxct_money() RETURNS numeric LANGUAGE plxtsql AS $$
  RETURN CAST(2.5 AS money) + 1;
$$;
SELECT fxct_money() AS r;  -- SQL Server: 3.5000
CREATE FUNCTION fxct_uuid() RETURNS uuid LANGUAGE plxtsql AS $$
  RETURN CAST('A0EEBC99-9C0B-4EF8-BB6D-6BB9BD380A11' AS uniqueidentifier);
$$;
SELECT fxct_uuid() AS r;  -- SQL Server: A0EEBC99-9C0B-4EF8-BB6D-6BB9BD380A11
CREATE FUNCTION fxct_bin() RETURNS int LANGUAGE plxtsql AS $$
  RETURN DATALENGTH(CAST('ab' AS varbinary(MAX))) + 10 * DATALENGTH(CAST('cde' AS image));
$$;
SELECT fxct_bin() AS r;  -- SQL Server: 32
CREATE FUNCTION fxct_nest(n int) RETURNS text LANGUAGE plxtsql AS $$
  RETURN CAST(CAST(@n AS nvarchar(10)) AS nvarchar(2)) + 'x';
$$;
SELECT fxct_nest(12345) AS r;  -- SQL Server: 12x
CREATE TABLE fxct_t(v int);
INSERT INTO fxct_t VALUES (42);
CREATE FUNCTION fxct_col() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @r nvarchar(10);
  SELECT @r = CAST(v AS nvarchar(10)) + '!' FROM fxct_t;
  RETURN @r;
$$;
SELECT fxct_col() AS r;  -- SQL Server: 42!
DROP TABLE fxct_t;
-- a PostgreSQL type name passes through unchanged
CREATE FUNCTION fxct_pg() RETURNS text LANGUAGE plxtsql AS $$
  RETURN CAST(CAST(1.5 AS double precision) AS varchar(10));
$$;
SELECT fxct_pg() AS r;
-- a T-SQL type whose PostgreSQL type would not keep SQL Server's value still fails when it runs:
-- tinyint and smallmoney have a smaller range, binary(n) and varbinary(n) cut to n bytes, and
-- smalldatetime rounds to the minute. SQL Server's answer is in each comment.
CREATE FUNCTION fxct_tiny() RETURNS int LANGUAGE plxtsql AS $$
  RETURN CAST(300 AS tinyint);
$$;
SELECT ft_err($q$SELECT fxct_tiny()$q$) AS r;  -- SQL Server: arithmetic overflow error
CREATE FUNCTION fxct_smoney() RETURNS numeric LANGUAGE plxtsql AS $$
  RETURN CAST(300000 AS smallmoney);
$$;
SELECT ft_err($q$SELECT fxct_smoney()$q$) AS r;  -- SQL Server: arithmetic overflow error
CREATE FUNCTION fxct_vbin() RETURNS int LANGUAGE plxtsql AS $$
  RETURN DATALENGTH(CAST('abc' AS varbinary(1)));
$$;
SELECT ft_err($q$SELECT fxct_vbin()$q$) AS r;  -- SQL Server: 1
CREATE FUNCTION fxct_sdt() RETURNS timestamp LANGUAGE plxtsql AS $$
  RETURN CAST('2024-01-02 03:04:35' AS smalldatetime);
$$;
SELECT ft_err($q$SELECT fxct_sdt()$q$) AS r;  -- SQL Server: 2024-01-02 03:05:00
-- CHARACTER VARYING, CHAR VARYING and NATIONAL CHARACTER VARYING are SQL Server's names for VARCHAR and
-- NVARCHAR, so a value of one of these types is a known string for +. Before, DECLARE rejected the
-- names and + stayed + after a CAST or CONVERT to them. SQL Server's answer is in each comment.
CREATE FUNCTION fxcv_local() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s character varying(10) = 'a', @t char varying(5) = 'c', @u national character varying(5) = 'd';
  RETURN @s + 'b' + @t + @u;
$$;
SELECT fxcv_local() AS r;  -- SQL Server: abcd
CREATE FUNCTION fxcv_nat() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @s national char varying(5) = 'a', @m character varying(MAX) = 'c';
  RETURN @s + 'b' + @m;
$$;
SELECT fxcv_nat() AS r;  -- SQL Server: abc
CREATE FUNCTION fxcv_par(p character varying, q character varying(5)) RETURNS text LANGUAGE plxtsql AS $$
  RETURN @p + 'b' + @q;
$$;
SELECT fxcv_par('a', 'c') AS r;  -- SQL Server: abc
CREATE FUNCTION fxcv_cast() RETURNS text LANGUAGE plxtsql AS $$
  RETURN CAST('a' AS character varying(5)) + 'b' + CAST('c' AS national char varying(5));
$$;
SELECT fxcv_cast() AS r;  -- SQL Server: abc
CREATE FUNCTION fxcv_cast30() RETURNS int LANGUAGE plxtsql AS $$
  RETURN LEN(CAST('12345678901234567890123456789012345' AS national character varying));
$$;
SELECT fxcv_cast30() AS r;  -- SQL Server: 30 (CAST without a length gives 30 characters)
CREATE FUNCTION fxcv_conv() RETURNS text LANGUAGE plxtsql AS $$
  RETURN CONVERT(character varying(5), 'a') + 'b' + CONVERT(char varying(5), 'c');
$$;
SELECT fxcv_conv() AS r;  -- SQL Server: abc
-- CHARACTER(n) and NATIONAL CHARACTER(n) are fixed length, like CHAR(n): + stays +, and the call fails
CREATE FUNCTION fxcv_fix() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @c character(3) = 'a';
  RETURN @c + 'b';
$$;
SELECT ft_err($q$SELECT fxcv_fix()$q$) AS r;  -- SQL Server: 'a  b'
CREATE FUNCTION fxcv_fixn() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @c national character(3) = 'a';
  RETURN @c + 'b';
$$;
SELECT ft_err($q$SELECT fxcv_fixn()$q$) AS r;  -- SQL Server: 'a  b'
CREATE FUNCTION fxcv_fixcast() RETURNS text LANGUAGE plxtsql AS $$
  RETURN CAST('a' AS national char(3)) + 'b';
$$;
SELECT ft_err($q$SELECT fxcv_fixcast()$q$) AS r;  -- SQL Server: 'a  b'
-- (MAX) is only for VARCHAR, NVARCHAR and VARBINARY: on another type CONVERT must still fail
CREATE FUNCTION fxmx_num() RETURNS text LANGUAGE plxtsql AS $$
  RETURN CONVERT(numeric(MAX), '1.5');
$$;
SELECT ft_err($q$SELECT fxmx_num()$q$) AS r;  -- SQL Server: error, numeric(max) is not a type
-- SQL Server: error, char(max) is not a type; plx fails at CREATE
CREATE FUNCTION fxmx_char() RETURNS text LANGUAGE plxtsql AS $$
  RETURN CONVERT(char(MAX), 'a  ');
$$;
CREATE FUNCTION fxmx_nv() RETURNS int LANGUAGE plxtsql AS $$
  RETURN LEN(CONVERT(nvarchar(MAX), 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa')) + DATALENGTH(CONVERT(varbinary(MAX), 'ab'));
$$;
SELECT fxmx_nv() AS r;  -- SQL Server: 42
-- R10 (PR 24 review [10]): a conversion to an integer type truncates toward zero, as in SQL Server,
-- in CAST, CONVERT and assignment to an integer variable; a DECIMAL or NUMERIC with no precision is decimal(18,0)
CREATE FUNCTION fxr10_s2(d numeric) RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @i int = 2.7;
  DECLARE @j int;
  SET @j = @d;
  RETURN CONCAT(CAST(1.5 AS int), ' ', CONVERT(int, -2.5), ' ', @i, ' ', @j);
$$;
SELECT fxr10_s2(3.9) AS r;  -- SQL Server: 1 -2 2 3
CREATE FUNCTION fxr10_v1(x numeric) RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @d DECIMAL = 2.7;
  RETURN CONCAT(CAST(@x AS numeric), '|', @d);
$$;
SELECT fxr10_v1(12.345) AS r;  -- SQL Server: 12|3
CREATE FUNCTION fxr10_more(x numeric) RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @f float = 2.9;
  DECLARE @b bit = 1;
  DECLARE @t tinyint = 2.9;
  DECLARE @s smallint = -2.9;
  DECLARE @n int = 1;
  DECLARE @e dec = -2.5;
  SET @n += 1.5;
  RETURN CONCAT(CAST(@f AS int), ' ', CAST(@f * -1 AS int), ' ', CAST(@b AS int), ' ',
                CAST('12' AS int), ' ', CONVERT(bigint, 9007199254740993.9), ' ', CONVERT(bigint, @x * -1), ' ',
                @t, ' ', @s, ' ', @n, ' ', CONVERT(decimal, 2.5), ' ', @e, ' ', CAST(@x AS numeric(5,2)));
$$;
SELECT fxr10_more(12.345) AS r;  -- SQL Server: 2 -2 1 12 9007199254740993 -12 2 -2 2 3 -3 12.35
CREATE TABLE fxr10_t(v numeric);
INSERT INTO fxr10_t VALUES (1.2), (2.9);
CREATE FUNCTION fxr10_sel() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @a int;
  DECLARE @b int;
  SELECT @a = v FROM fxr10_t WHERE v > 2;
  SELECT @b = -7.9;
  RETURN CONCAT(@a, ' ', @b);
$$;
SELECT fxr10_sel() AS r;  -- SQL Server: 2 -7
CREATE FUNCTION fxr10_q() RETURNS SETOF int LANGUAGE plxtsql AS $$
  SELECT CAST(v AS int) FROM fxr10_t ORDER BY v;
$$;
SELECT * FROM fxr10_q();  -- SQL Server: 1, 2
-- a string that is not an integer still fails, as in SQL Server
CREATE FUNCTION fxr10_str() RETURNS int LANGUAGE plxtsql AS $$
  RETURN CAST('1.5' AS int);
$$;
SELECT ft_err($q$SELECT fxr10_str()$q$) AS r;  -- SQL Server: error, conversion failed
-- aggregates, a window function and GROUP BY
CREATE FUNCTION fxr10_agg() RETURNS SETOF text LANGUAGE plxtsql AS $$
  SELECT CONCAT(CAST(SUM(v) AS int), ' ', CAST(AVG(v) OVER () AS int), ' ', CONVERT(int, ABS(v) * -1))
    FROM fxr10_t GROUP BY v ORDER BY v;
$$;
SELECT * FROM fxr10_agg();  -- SQL Server: 1 2 -1, 2 2 -2
-- nested conversions evaluate each value once: the text and the plan grow linearly
CREATE FUNCTION fxr10_nest(x numeric) RETURNS int LANGUAGE plxtsql AS $$
  RETURN CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(CAST(@x AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int) AS int);
$$;
SET statement_timeout = '60s';
SELECT fxr10_nest(-2.7) AS r;  -- SQL Server: -2
RESET statement_timeout;
-- R10b (review of the PR 24 fixes): a conversion whose value holds a function
-- call goes through one helper function call, so the value is evaluated once,
-- an aggregate or window function in it stays in the outer query, it works in
-- DDL, and the plan grows linearly with nesting
CREATE TABLE fxr10b_t(g int, x numeric);
INSERT INTO fxr10b_t VALUES (1, 1.5), (1, 1.2), (2, 1.9);
CREATE FUNCTION fxr10b_agg() RETURNS SETOF text LANGUAGE plxtsql AS $$
  SELECT CONCAT(g, ':', CAST(COUNT(*) AS int)) FROM fxr10b_t GROUP BY g ORDER BY g;
$$;
SELECT * FROM fxr10b_agg();  -- SQL Server: 1:2, 2:1
CREATE FUNCTION fxr10b_agg2() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @n int;
  DECLARE @m int;
  SELECT @n = CAST(COUNT(*) AS int) FROM fxr10b_t;
  SET @m = (SELECT CONVERT(int, COUNT(*)) FROM fxr10b_t);
  RETURN CONCAT(@n, ' ', @m, ' ', (SELECT CAST(SUM(1) * 1.5 AS int) FROM fxr10b_t));
$$;
SELECT fxr10b_agg2() AS r;  -- SQL Server: 3 3 4
CREATE FUNCTION fxr10b_win() RETURNS SETOF int LANGUAGE plxtsql AS $$
  SELECT CAST(SUM(CAST(x AS int)) OVER () AS int) FROM fxr10b_t ORDER BY g;
$$;
SELECT * FROM fxr10b_win();  -- SQL Server: 3, 3, 3
-- a call inside a converted window expression runs once per row
CREATE SEQUENCE fxr10b_seq;
CREATE FUNCTION fxr10b_once() RETURNS SETOF int LANGUAGE plxtsql AS $$
  SELECT CAST(nextval('fxr10b_seq') * 0 + ROW_NUMBER() OVER (ORDER BY x) * 1.5 AS int) FROM fxr10b_t ORDER BY x;
$$;
SELECT * FROM fxr10b_once();  -- SQL Server: 1, 3, 4
SELECT last_value AS r FROM fxr10b_seq;  -- one call per row: 3
-- a conversion in a DEFAULT or a CHECK of a table the function creates
CREATE FUNCTION fxr10b_ddl() RETURNS text LANGUAGE plxtsql AS $$
  CREATE TABLE fxr10b_c (a numeric, b int DEFAULT CAST(abs(-2.5) AS int), CHECK (CAST(abs(a) AS int) < 5));
  INSERT INTO fxr10b_c (a) VALUES (-4.9);
  RETURN (SELECT CONCAT(a, ' ', b) FROM fxr10b_c);
$$;
SELECT fxr10b_ddl() AS r;  -- SQL Server: -4.9 2
-- a literal of unknown type in parentheses converts as before
CREATE FUNCTION fxr10b_unk() RETURNS text LANGUAGE plxtsql AS $$
  RETURN CONCAT(CAST((NULL) AS int), '|', CAST(('12') AS int), '|', CONVERT(bigint, (NULL)));
$$;
SELECT fxr10b_unk() AS r;  -- SQL Server: |12|
-- 300 nested conversions plan in linear time (before the fix: over 2 GB of memory and over a second)
DO $d$
BEGIN
  EXECUTE format('CREATE FUNCTION fxr10b_deep(d numeric) RETURNS int LANGUAGE plxtsql AS %L',
                 'RETURN ' || repeat('CAST(abs(', 300) || '@d' || repeat(') AS int)', 300) || ';');
END $d$;
SET statement_timeout = '1s';
SELECT fxr10b_deep(-2.7) AS r;  -- SQL Server: 2
RESET statement_timeout;
-- R11 (PR 24 review [11]): a CATCH block undoes the writes its TRY block made
-- before the error, because TRY becomes a plpgsql block with an EXCEPTION
-- clause, which rolls back to a savepoint. SQL Server keeps them. plx cannot
-- emulate this; the case pins the behaviour so that a change is noticed.
CREATE TABLE fxr11_log(msg text);
CREATE FUNCTION fxr11_try() RETURNS text LANGUAGE plxtsql AS $$
  DECLARE @n int = 0;
  BEGIN TRY
    INSERT INTO fxr11_log VALUES ('attempt');
    SET @n = 1;
    RAISERROR('fail', 16, 1);
  END TRY
  BEGIN CATCH
    INSERT INTO fxr11_log VALUES ('caught');
  END CATCH
  RETURN CONCAT('n=', @n);
$$;
SELECT fxr11_try() AS r;  -- SQL Server: n=1 (a variable keeps its value in both)
SELECT string_agg(msg, ',' ORDER BY msg) AS r FROM fxr11_log;  -- SQL Server: attempt,caught; plx: caught
-- the documented workaround: make a write that must survive before BEGIN TRY
TRUNCATE fxr11_log;
CREATE FUNCTION fxr11_fix() RETURNS text LANGUAGE plxtsql AS $$
  INSERT INTO fxr11_log VALUES ('attempt');
  BEGIN TRY
    RAISERROR('fail', 16, 1);
  END TRY
  BEGIN CATCH
    INSERT INTO fxr11_log VALUES ('caught');
  END CATCH
  RETURN 'done';
$$;
SELECT fxr11_fix() AS r;
SELECT string_agg(msg, ',' ORDER BY msg) AS r FROM fxr11_log;  -- SQL Server: attempt,caught
