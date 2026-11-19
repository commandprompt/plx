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
CREATE TABLE "fx44 Order Details"(qty int);
INSERT INTO "fx44 Order Details" VALUES (5);
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
