-- Regression tests for bugs found by the 2026-11-19 audit (fix_cobol).
-- Each case names its audit finding (A<n>).
-- A failing case must not hang the suite.
SET statement_timeout = '10s';
-- A35: EXIT PERFORM CYCLE in a general PERFORM VARYING still runs the step
CREATE FUNCTION fc_a35_cycle() RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-I PIC 9(9) VALUE 0.
01 WS-S PIC 9(9) VALUE 0.
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 2 UNTIL WS-I > 9
        IF WS-I = 5
            EXIT PERFORM CYCLE
        END-IF
        ADD WS-I TO WS-S
    END-PERFORM
    GOBACK RETURNING WS-S.
$$;
SELECT fc_a35_cycle() AS expect_20;
-- A35: CYCLE in an inner PERFORM must not run the outer loop's step
CREATE FUNCTION fc_a35_inner() RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-I PIC 9(9) VALUE 0.
01 WS-N PIC 9(9) VALUE 0.
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 2 UNTIL WS-I > 5
        PERFORM 2 TIMES
            ADD 1 TO WS-N
            EXIT PERFORM CYCLE
        END-PERFORM
    END-PERFORM
    GOBACK RETURNING WS-N * 100 + WS-I.
$$;
SELECT fc_a35_inner() AS expect_607;
-- A34: PERFORM VARYING BY 1 leaves the control item at the first value that satisfied UNTIL
CREATE FUNCTION fc_a34_after(n int) RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-I PIC 9(9) VALUE 0.
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > N
        CONTINUE
    END-PERFORM
    GOBACK RETURNING WS-I.
$$;
SELECT fc_a34_after(10) AS expect_11;
-- A34: EXIT PERFORM keeps the control item's current value
CREATE FUNCTION fc_a34_search(n int) RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-I PIC 9(9) VALUE 0.
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I >= 100
        IF WS-I * WS-I >= N
            EXIT PERFORM
        END-IF
    END-PERFORM
    GOBACK RETURNING WS-I.
$$;
SELECT fc_a34_search(50) AS expect_8;
-- A34: a MOVE to the control item or the bound in the body affects the next test
CREATE FUNCTION fc_a34_move() RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-I PIC 9(9) VALUE 0.
01 WS-B PIC 9(9) VALUE 10.
01 WS-C PIC 9(9) VALUE 0.
01 WS-D PIC 9(9) VALUE 0.
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > 10
        ADD 1 TO WS-C
        MOVE 100 TO WS-I
    END-PERFORM
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > WS-B
        ADD 1 TO WS-D
        MOVE 0 TO WS-B
    END-PERFORM
    GOBACK RETURNING concat(WS-C, " ", WS-D).
$$;
SELECT fc_a34_move() AS expect_1_1;
-- A34: a PIC 9(18) control item counts past the int4 range
DO LANGUAGE plxcobol $$
WORKING-STORAGE SECTION.
01 WS-I PIC 9(18) VALUE 0.
01 WS-C PIC 9(4) VALUE 0.
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 3000000000 BY 1 UNTIL WS-I > 3000000002
        ADD 1 TO WS-C
    END-PERFORM
    DISPLAY WS-C " " WS-I.
$$;
-- A34: an undeclared control variable still works
DO LANGUAGE plxcobol $$
PROCEDURE DIVISION.
    PERFORM VARYING K FROM 1 BY 1 UNTIL K > 2
        DISPLAY "k=" K
    END-PERFORM.
$$;
-- A36: the lexer rejects a character it does not recognise instead of dropping it
CREATE FUNCTION fc_a36_ne(a int, b int) RETURNS text LANGUAGE plxcobol AS $$
PROCEDURE DIVISION.
    IF A != B
        GOBACK RETURNING "different"
    END-IF
    GOBACK RETURNING "same".
$$;
CREATE FUNCTION fc_a36_pos(int) RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-X PIC 9(9).
PROCEDURE DIVISION.
    COMPUTE WS-X = $1 * 10
    GOBACK RETURNING WS-X.
$$;
CREATE FUNCTION fc_a36_abs(a int) RETURNS int LANGUAGE plxcobol AS $$
PROCEDURE DIVISION.
    GOBACK RETURNING @ A.
$$;
CREATE FUNCTION fc_a36_cat(a text) RETURNS text LANGUAGE plxcobol AS $$
PROCEDURE DIVISION.
    GOBACK RETURNING A || "x".
$$;
-- A36: the same characters inside a string literal or a comment are fine
CREATE FUNCTION fc_a36_ok() RETURNS text LANGUAGE plxcobol AS $$
PROCEDURE DIVISION.
    *> a comment may say != or $1 or @
    GOBACK RETURNING "!= $1 @ || é".
$$;
SELECT fc_a36_ok() AS expect_chars;
-- A37: stacked WHENs in EVALUATE TRUE share the following statements
CREATE FUNCTION fc_a37_searched(n int) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-R PIC X(10) VALUE "none".
PROCEDURE DIVISION.
    EVALUATE TRUE
        WHEN N = 1
        WHEN N = 2
            MOVE "small" TO WS-R
        WHEN OTHER
            MOVE "big" TO WS-R
    END-EVALUATE
    GOBACK RETURNING WS-R.
$$;
SELECT fc_a37_searched(1) AS expect_small, fc_a37_searched(2) AS small2,
       fc_a37_searched(3) AS big;
-- A37: a WHEN stacked before WHEN OTHER shares the OTHER statements
CREATE FUNCTION fc_a37_other(n int) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-R PIC X(10) VALUE "none".
PROCEDURE DIVISION.
    EVALUATE N
        WHEN 1
            MOVE "one" TO WS-R
        WHEN 2
        WHEN OTHER
            MOVE "other" TO WS-R
    END-EVALUATE
    GOBACK RETURNING WS-R.
$$;
SELECT fc_a37_other(1) AS one, fc_a37_other(2) AS expect_other,
       fc_a37_other(7) AS other7;
-- A37: the same in EVALUATE TRUE
CREATE FUNCTION fc_a37_searched_other(n int) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-R PIC X(10) VALUE "none".
PROCEDURE DIVISION.
    EVALUATE TRUE
        WHEN N = 1
            MOVE "one" TO WS-R
        WHEN N = 2
        WHEN OTHER
            MOVE "other" TO WS-R
    END-EVALUATE
    GOBACK RETURNING WS-R.
$$;
SELECT fc_a37_searched_other(2) AS expect_other;
-- A38: a store into a numeric PICTURE item truncates excess digits, as COBOL does
CREATE FUNCTION fc_a38_trunc(a numeric) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-I PIC 9(4).
01 WS-M PIC 9(3)V99.
01 WS-J PIC S9(4) VALUE 0.
01 WS-K PIC 9(4) VALUE 0.
01 WS-ARR PIC 9(3)V9 OCCURS 2 TIMES.
PROCEDURE DIVISION.
    COMPUTE WS-I = 7.9
    COMPUTE WS-M = 2.0 / 3
    MOVE A TO WS-J
    ADD 1.5 TO WS-K
    MULTIPLY 2.5 BY WS-K
    COMPUTE WS-ARR(1) = 1.99
    GOBACK RETURNING concat(WS-I, " ", WS-M, " ", WS-J, " ", WS-K, " ", WS-ARR(1)).
$$;
SELECT fc_a38_trunc(2.5) AS expect_7_066_2_2_19, fc_a38_trunc(-2.5) AS neg;
-- A38: ROUNDED rounds half away from zero, in COMPUTE and the arithmetic verbs
CREATE FUNCTION fc_a38_rounded() RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-I PIC 9(4).
01 WS-M PIC 9(3)V99.
01 WS-Q PIC 9(4) VALUE 0.
01 WS-G PIC 9(4) VALUE 0.
01 WS-D PIC 9(4) VALUE 0.
PROCEDURE DIVISION.
    COMPUTE WS-I ROUNDED = 7.5
    COMPUTE WS-M ROUNDED = 2.0 / 3
    ADD 1.5 TO WS-Q ROUNDED
    ADD 1.5 TO WS-Q GIVING WS-G ROUNDED
    DIVIDE 4 INTO 10.0 GIVING WS-D ROUNDED
    GOBACK RETURNING concat(WS-I, " ", WS-M, " ", WS-Q, " ", WS-G, " ", WS-D).
$$;
SELECT fc_a38_rounded() AS expect_8_067_2_4_3;
-- A38: TYPE and COMP-2 items keep the plain SQL assignment
CREATE FUNCTION fc_a38_other() RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-T TYPE integer.
01 WS-F PIC 9(4) USAGE COMP-2.
PROCEDURE DIVISION.
    COMPUTE WS-T = 7.9
    COMPUTE WS-F = 7.9
    GOBACK RETURNING concat(WS-T, " ", WS-F).
$$;
SELECT fc_a38_other() AS expect_8_79;
-- A39: DISPLAY of a subscripted table element shows that element
DO LANGUAGE plxcobol $$
WORKING-STORAGE SECTION.
01 WS-ARR PIC 9(9) OCCURS 3 TIMES.
01 WS-I PIC 9(9) VALUE 2.
PROCEDURE DIVISION.
    COMPUTE WS-ARR(1) = 11
    COMPUTE WS-ARR(2) = 22
    DISPLAY "elem2=" WS-ARR(2) " elemI=" WS-ARR(WS-I) UPON CONSOLE
    DISPLAY "noadv=" WS-ARR(1) WITH NO ADVANCING.
$$;
-- A39: an operand DISPLAY cannot handle is an error, not silently dropped
DO LANGUAGE plxcobol $$
PROCEDURE DIVISION.
    DISPLAY "x=" upper("a").
$$;
-- A40: DIVIDE ... BY without GIVING is rejected instead of overwriting the divisor
CREATE FUNCTION fc_a40_divby() RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-A PIC 9(4) VALUE 100.
01 WS-B PIC 9(4) VALUE 4.
PROCEDURE DIVISION.
    DIVIDE WS-A BY WS-B
    GOBACK RETURNING WS-B.
$$;
-- A40: DIVIDE ... BY ... GIVING still works
CREATE FUNCTION fc_a40_giving() RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-A PIC 9(4) VALUE 100.
01 WS-B PIC 9(4) VALUE 4.
01 WS-Q PIC 9(4) VALUE 0.
PROCEDURE DIVISION.
    DIVIDE WS-A BY WS-B GIVING WS-Q
    GOBACK RETURNING WS-Q * 1000 + WS-B.
$$;
SELECT fc_a40_giving() AS expect_25004;
-- A41: VALUE on an OCCURS item gives every element that value
DO LANGUAGE plxcobol $$
WORKING-STORAGE SECTION.
01 WS-A PIC 9(4) OCCURS 3 TIMES VALUE ZERO.
01 WS-S PIC X(2) VALUE "ab" OCCURS 2.
01 WS-N PIC 9(2) OCCURS 2 VALUE NULL.
PROCEDURE DIVISION.
    COMPUTE WS-A(2) = WS-A(2) + 5
    DISPLAY WS-A " " WS-S " " WS-N.
$$;
-- A38 review: MOVE of a whole table into a numeric OCCURS item still works
CREATE FUNCTION fc_r1_arrparam(a int[]) RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-B PIC 9(4) OCCURS 3 TIMES.
PROCEDURE DIVISION.
    MOVE A TO WS-B
    GOBACK RETURNING WS-B(2).
$$;
SELECT fc_r1_arrparam(ARRAY[1,2,3]) AS expect_2;
DO LANGUAGE plxcobol $$
WORKING-STORAGE SECTION.
01 WS-A PIC 9(4)V99 OCCURS 2 TIMES VALUE 5.
01 WS-B PIC 9(4) OCCURS 2 TIMES.
PROCEDURE DIVISION.
    COMPUTE WS-A(2) = 6
    MOVE WS-A TO WS-B
    DISPLAY "table=" WS-B.
$$;
-- A37 review: a WHEN stacked before WHEN OTHER that is the only other arm still runs
CREATE FUNCTION fc_r2_only(n int) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-R PIC X(10) VALUE "none".
PROCEDURE DIVISION.
    EVALUATE N
        WHEN 2
        WHEN OTHER
            MOVE "other" TO WS-R
    END-EVALUATE
    GOBACK RETURNING WS-R.
$$;
SELECT fc_r2_only(2) AS expect_other, fc_r2_only(5) AS other5;
CREATE FUNCTION fc_r2_only_searched(n int) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-R PIC X(10) VALUE "none".
PROCEDURE DIVISION.
    EVALUATE TRUE
        WHEN N = 2
        WHEN OTHER
            MOVE "other" TO WS-R
    END-EVALUATE
    GOBACK RETURNING WS-R.
$$;
SELECT fc_r2_only_searched(2) AS expect_other, fc_r2_only_searched(5) AS other5;
-- A37 review: an EVALUATE with only WHEN OTHER runs its statements
DO LANGUAGE plxcobol $$
WORKING-STORAGE SECTION.
01 WS-R PIC X(10) VALUE "none".
PROCEDURE DIVISION.
    EVALUATE 3
        WHEN OTHER
            MOVE "other" TO WS-R
    END-EVALUATE
    DISPLAY "r=" WS-R.
$$;
-- A39 review: NO ADVANCING without WITH is accepted, not read as two data names
DO LANGUAGE plxcobol $$
WORKING-STORAGE SECTION.
01 WS-A PIC 9(4) VALUE 1.
01 NO-X PIC 9(4) VALUE 2.
PROCEDURE DIVISION.
    DISPLAY "a=" WS-A NO ADVANCING
    DISPLAY "b=" NO-X UPON CONSOLE NO ADVANCING.
$$;
-- A38 review: VALUE on a numeric PICTURE item truncates excess digits too
DO LANGUAGE plxcobol $$
WORKING-STORAGE SECTION.
01 WS-I PIC 9(4) VALUE 2.5.
01 WS-M PIC 9V99 VALUE 1.239.
01 WS-T PIC 9(4) OCCURS 2 TIMES VALUE 7.9.
01 WS-K VALUE 3.75 PIC 9V9.
PROCEDURE DIVISION.
    DISPLAY WS-I " " WS-M " " WS-T " " WS-K.
$$;
-- A38 review: the PERFORM OVER ARRAY loop variable keeps the SQL assignment rule, as documented
CREATE FUNCTION fc_d1_over(a numeric[]) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-V PIC 9(4).
01 WS-R PIC X(40) VALUE "".
PROCEDURE DIVISION.
    PERFORM WS-V OVER ARRAY A
        STRING-APPEND WS-V TO WS-R
    END-PERFORM
    GOBACK RETURNING WS-R.
$$;
SELECT fc_d1_over(ARRAY[7.9, 2.5]) AS expect_83;
-- A38 review: DIVIDE into a numeric PICTURE item keeps the quotient's fraction for ROUNDED and for fractional receivers
DO LANGUAGE plxcobol $$
WORKING-STORAGE SECTION.
01 WS-A PIC 9(4) VALUE 3.
01 WS-B PIC 9(4) VALUE 4.
01 WS-Q PIC 9(4) VALUE 0.
01 WS-M PIC 9V99 VALUE 0.
01 WS-T PIC 9(4) VALUE 0.
01 WS-N PIC S9(4) VALUE 0.
01 WS-X TYPE integer.
01 WS-C PIC 9(4) VALUE 0.
PROCEDURE DIVISION.
    COMPUTE WS-N = -7
    DIVIDE WS-A BY WS-B GIVING WS-Q ROUNDED
    DIVIDE 2 INTO 7 GIVING WS-M
    DIVIDE 2 INTO 7 GIVING WS-T
    DIVIDE 2 INTO WS-N
    DIVIDE 2 INTO 7 GIVING WS-X
    COMPUTE WS-C ROUNDED = WS-A / WS-B
    DIVIDE 4 INTO WS-A ROUNDED
    DISPLAY WS-A " " WS-Q " " WS-M " " WS-T " " WS-N " " WS-X " " WS-C.
$$;
-- A36 review: form feed and vertical tab separate words; a non-ASCII byte outside a string is rejected
DO $d$ BEGIN
  EXECUTE format('DO LANGUAGE plxcobol %L', E'PROCEDURE DIVISION.\f    DISPLAY "ff"\x0b"vt".');
END $d$;
DO LANGUAGE plxcobol $$
PROCEDURE DIVISION.
    DISPLAY é "x".
$$;
-- A38 review: ROUNDED MODE is rejected
CREATE FUNCTION fc_g_mode() RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-I PIC 9(4).
PROCEDURE DIVISION.
    COMPUTE WS-I ROUNDED MODE NEAREST-EVEN = 2.5
    GOBACK RETURNING WS-I.
$$;
-- A38 review: ROUNDED between the receiver and GIVING is rejected
CREATE FUNCTION fc_g_giving() RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-I PIC 9(4) VALUE 1.
01 WS-G PIC 9(4).
PROCEDURE DIVISION.
    ADD 1.5 TO WS-I ROUNDED GIVING WS-G
    GOBACK RETURNING WS-G.
$$;
-- A38 review: the PERFORM VARYING start and step stores truncate into a fractional item
DO LANGUAGE plxcobol $$
WORKING-STORAGE SECTION.
01 WS-V PIC 9V9 VALUE 0.
01 WS-C PIC 9(4) VALUE 0.
PROCEDURE DIVISION.
    PERFORM VARYING WS-V FROM 0.25 BY 0.25 UNTIL WS-V > 1
        ADD 1 TO WS-C
    END-PERFORM
    DISPLAY WS-C " " WS-V.
$$;
-- A41 review: an OCCURS count that is too long or not an integer is rejected
DO LANGUAGE plxcobol $$
WORKING-STORAGE SECTION.
01 WS-A PIC 9(4) OCCURS 1234567890 TIMES VALUE ZERO.
PROCEDURE DIVISION.
    DISPLAY "unreached".
$$;
DO LANGUAGE plxcobol $$
WORKING-STORAGE SECTION.
01 WS-A PIC 9(4) OCCURS 2.5 TIMES VALUE ZERO.
PROCEDURE DIVISION.
    DISPLAY "unreached".
$$;
