-- plxcobol regression tests (ISO/IEC 1989:2023)
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;

-- scalar return: WORKING-STORAGE + COMPUTE + GOBACK RETURNING
CREATE FUNCTION cob_add(a int, b int) RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-R PIC S9(9).
PROCEDURE DIVISION.
    COMPUTE WS-R = A + B * 2
    GOBACK RETURNING WS-R.
$$;
SELECT cob_add(3, 4);

-- nested IF / ELSE / END-IF, MOVE, PIC X
CREATE FUNCTION cob_grade(score int) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-GRADE PIC X(1).
PROCEDURE DIVISION.
    IF SCORE >= 90
        MOVE "A" TO WS-GRADE
    ELSE
        IF SCORE >= 80
            MOVE "B" TO WS-GRADE
        ELSE
            MOVE "F" TO WS-GRADE
        END-IF
    END-IF
    GOBACK RETURNING WS-GRADE.
$$;
SELECT cob_grade(95), cob_grade(85), cob_grade(70);

-- PERFORM VARYING + ADD .. TO, numeric PIC + VALUE
CREATE FUNCTION cob_sum(n int) RETURNS bigint LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-TOTAL PIC 9(18) VALUE 0.
01 WS-I     PIC 9(9).
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > N
        ADD WS-I TO WS-TOTAL
    END-PERFORM
    GOBACK RETURNING WS-TOTAL.
$$;
SELECT cob_sum(100);

-- arithmetic verbs with GIVING
CREATE FUNCTION cob_calc(a int, b int) RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-S PIC S9(9).
01 WS-D PIC S9(9).
01 WS-P PIC S9(9).
01 WS-Q PIC S9(9).
PROCEDURE DIVISION.
    ADD A B GIVING WS-S
    SUBTRACT B FROM A GIVING WS-D
    MULTIPLY A BY B GIVING WS-P
    DIVIDE B INTO A GIVING WS-Q
    COMPUTE WS-S = WS-S + WS-D + WS-P + WS-Q
    GOBACK RETURNING WS-S.
$$;
SELECT cob_calc(12, 3);

-- arithmetic: multi-addend ADD ... TO (in place) and SUBTRACT ... FROM
CREATE FUNCTION cob_addto() RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-T PIC S9(9) VALUE 100.
PROCEDURE DIVISION.
    ADD 1 2 3 TO WS-T
    SUBTRACT 6 FROM WS-T
    GOBACK RETURNING WS-T.
$$;
SELECT cob_addto();

-- relational word forms and figurative constants
CREATE FUNCTION cob_relwords(a int, b int) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-R PIC X(4).
PROCEDURE DIVISION.
    IF A IS GREATER THAN OR EQUAL TO B
        MOVE "ge" TO WS-R
    ELSE
        IF A IS NOT EQUAL TO B
            MOVE "lt" TO WS-R
        END-IF
    END-IF
    GOBACK RETURNING WS-R.
$$;
SELECT cob_relwords(5, 5) AS ge, cob_relwords(2, 9) AS lt;

CREATE FUNCTION cob_figs(n int) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-C PIC 9(9) VALUE ZERO.
01 WS-S PIC X(8) VALUE SPACE.
PROCEDURE DIVISION.
    IF N IS EQUAL TO ZERO
        MOVE "zero" TO WS-S
    END-IF
    GOBACK RETURNING WS-S.
$$;
SELECT cob_figs(0) AS zero;

-- MOVE to multiple receivers, PERFORM n TIMES
CREATE FUNCTION cob_multimove(n int) RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-A PIC 9(9).
01 WS-B PIC 9(9).
01 WS-I PIC 9(9) VALUE 0.
PROCEDURE DIVISION.
    MOVE N TO WS-A WS-B
    PERFORM WS-A TIMES
        ADD 1 TO WS-I
    END-PERFORM
    GOBACK RETURNING WS-I + WS-B.
$$;
SELECT cob_multimove(5) AS should_be_10;

-- EVALUATE (simple) with stacked WHEN and WHEN OTHER
CREATE FUNCTION cob_classify(n int) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-R PIC X(8).
PROCEDURE DIVISION.
    EVALUATE N
        WHEN 1
            MOVE "one" TO WS-R
        WHEN 2
        WHEN 3
            MOVE "few" TO WS-R
        WHEN OTHER
            MOVE "many" TO WS-R
    END-EVALUATE
    GOBACK RETURNING WS-R.
$$;
SELECT cob_classify(1), cob_classify(3), cob_classify(9);

-- EVALUATE TRUE (searched CASE), PERFORM TIMES, EXIT PERFORM
CREATE FUNCTION cob_sign(n int) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-R PIC X(8).
PROCEDURE DIVISION.
    EVALUATE TRUE
        WHEN N > 0
            MOVE "pos" TO WS-R
        WHEN N < 0
            MOVE "neg" TO WS-R
        WHEN OTHER
            MOVE "zero" TO WS-R
    END-EVALUATE
    GOBACK RETURNING WS-R.
$$;
SELECT cob_sign(5), cob_sign(-2), cob_sign(0);

-- PERFORM UNTIL + inline PERFORM with EXIT PERFORM
CREATE FUNCTION cob_firstmult(n int, d int) RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-I PIC 9(9) VALUE 1.
PROCEDURE DIVISION.
    PERFORM UNTIL WS-I > N
        IF WS-I / D * D = WS-I
            GOBACK RETURNING WS-I
        END-IF
        ADD 1 TO WS-I
    END-PERFORM
    GOBACK RETURNING 0.
$$;
SELECT cob_firstmult(20, 7);

-- CONSTANT AS + COMPUTE with ** exponent
CREATE FUNCTION cob_area(r numeric) RETURNS numeric LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 PI CONSTANT AS 3.14159.
01 WS-A PIC 9(9)V9(5).
PROCEDURE DIVISION.
    COMPUTE WS-A = PI * R ** 2
    GOBACK RETURNING WS-A.
$$;
SELECT cob_area(2);

-- OCCURS: a table (array). Subscript WS-ARR(i) reads/writes the element.
CREATE FUNCTION cob_occurs() RETURNS bigint LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-ARR PIC 9(9) OCCURS 5 TIMES.
01 WS-I PIC 9(9).
01 WS-SUM PIC 9(18) VALUE 0.
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > 5
        COMPUTE WS-ARR(WS-I) = WS-I * WS-I
    END-PERFORM
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > 5
        COMPUTE WS-SUM = WS-SUM + WS-ARR(WS-I)
    END-PERFORM
    GOBACK RETURNING WS-SUM.
$$;
SELECT cob_occurs() AS should_be_55;

-- MOVE into an array element (varchar element type), and FOREACH over the array
CREATE FUNCTION cob_occurs2() RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-A PIC X(4) OCCURS 3 TIMES.
01 WS-V PIC X(4).
01 WS-S PIC X(1) VALUE "".
PROCEDURE DIVISION.
    MOVE "ab" TO WS-A(1)
    MOVE "cd" TO WS-A(2)
    MOVE "ef" TO WS-A(3)
    PERFORM WS-V OVER ARRAY WS-A
        STRING-APPEND WS-V TO WS-S
    END-PERFORM
    GOBACK RETURNING WS-S.
$$;
SELECT cob_occurs2() AS should_be_abcdef;

-- multi-argument SQL function calls in expressions (comma survives inside parens)
CREATE FUNCTION cob_funcs(a int, b int, c int) RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-R PIC S9(9).
PROCEDURE DIVISION.
    COMPUTE WS-R = mod(A, B) + greatest(A, B, C)
    GOBACK RETURNING WS-R.
$$;
SELECT cob_funcs(17, 5, 20);

-- STRING-APPEND lowers to the plx_strbuild string builder; % is modulo
CREATE FUNCTION cob_build(n int) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-S PIC X(1) VALUE "".
01 WS-I PIC 9(9).
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > N
        IF WS-I % 2 = 0
            STRING-APPEND "ab" TO WS-S
        END-IF
    END-PERFORM
    GOBACK RETURNING WS-S.
$$;
SELECT cob_build(6);

-- data set-up for the SQL constructs
CREATE TEMP TABLE cob_items(id int, amount int);
INSERT INTO cob_items VALUES (1, 10), (2, 20), (3, 30);

-- query FOR-loop over a record, qualified field access
CREATE FUNCTION cob_total() RETURNS bigint LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-TOTAL PIC 9(18) VALUE 0.
01 WS-ROW TYPE RECORD.
PROCEDURE DIVISION.
    PERFORM WS-ROW OVER "SELECT amount FROM cob_items ORDER BY id"
        ADD WS-ROW.AMOUNT TO WS-TOTAL
    END-PERFORM
    GOBACK RETURNING WS-TOTAL.
$$;
SELECT cob_total();

-- FOREACH over an array
CREATE FUNCTION cob_arraysum(a int[]) RETURNS bigint LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-TOTAL PIC 9(18) VALUE 0.
01 WS-V PIC 9(9).
PROCEDURE DIVISION.
    PERFORM WS-V OVER ARRAY A
        ADD WS-V TO WS-TOTAL
    END-PERFORM
    GOBACK RETURNING WS-TOTAL.
$$;
SELECT cob_arraysum(ARRAY[5, 10, 15, 20]);

-- SETOF via RETURN-NEXT
CREATE FUNCTION cob_squares(n int) RETURNS SETOF int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-I PIC 9(9).
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > N
        RETURN-NEXT WS-I * WS-I
    END-PERFORM
    GOBACK.
$$;
SELECT array_agg(x) FROM cob_squares(4) x;

-- RETURN-QUERY
CREATE FUNCTION cob_ids() RETURNS SETOF int LANGUAGE plxcobol AS $$
PROCEDURE DIVISION.
    RETURN-QUERY "SELECT id FROM cob_items ORDER BY id".
$$;
SELECT array_agg(x) FROM cob_ids() x;

-- EXECUTE dynamic with USING and INTO, plus GET ROW-COUNT
CREATE FUNCTION cob_dyn(minamt int) RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-CNT PIC 9(9).
01 WS-RC  PIC 9(9).
PROCEDURE DIVISION.
    EXECUTE "SELECT count(*) FROM cob_items WHERE amount >= $1" USING MINAMT INTO WS-CNT
    EXECUTE "UPDATE cob_items SET amount = amount WHERE amount >= $1" USING MINAMT
    GET ROW-COUNT INTO WS-RC
    COMPUTE WS-CNT = WS-CNT * 10 + WS-RC
    GOBACK RETURNING WS-CNT.
$$;
SELECT cob_dyn(20);

-- cursors: OPEN / FETCH / CLOSE + IS NULL loop test
CREATE FUNCTION cob_cursor() RETURNS bigint LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-C   TYPE refcursor.
01 WS-ROW TYPE RECORD.
01 WS-SUM PIC 9(18) VALUE 0.
PROCEDURE DIVISION.
    OPEN-CURSOR WS-C FOR "SELECT amount FROM cob_items ORDER BY id"
    FETCH-CURSOR WS-C INTO WS-ROW
    PERFORM UNTIL WS-ROW IS NULL
        ADD WS-ROW.AMOUNT TO WS-SUM
        FETCH-CURSOR WS-C INTO WS-ROW
    END-PERFORM
    CLOSE-CURSOR WS-C
    GOBACK RETURNING WS-SUM.
$$;
SELECT cob_cursor();

-- exception handling: BEGIN-TRY / WHEN <cond> / WHEN OTHER + GET STACKED
CREATE FUNCTION cob_safediv(a int, b int) RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-Q PIC 9(9).
01 WS-MSG PIC X(80).
PROCEDURE DIVISION.
    BEGIN-TRY
        COMPUTE WS-Q = A / B
        MOVE "ok" TO WS-MSG
    WHEN DIVISION-BY-ZERO
        MOVE "divide by zero" TO WS-MSG
    WHEN OTHER
        GET MESSAGE INTO WS-MSG
    END-TRY
    GOBACK RETURNING WS-MSG.
$$;
SELECT cob_safediv(10, 2), cob_safediv(10, 0);

-- RAISE with SQLSTATE, caught, GET STACKED SQLSTATE
CREATE FUNCTION cob_raise() RETURNS text LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-S PIC X(20).
PROCEDURE DIVISION.
    BEGIN-TRY
        RAISE EXCEPTION "nope" SQLSTATE "22012"
    WHEN OTHER
        GET SQLSTATE INTO WS-S
    END-TRY
    GOBACK RETURNING WS-S.
$$;
SELECT cob_raise();

-- ASSERT and CONTINUE (no-op)
CREATE FUNCTION cob_checked(n int) RETURNS int LANGUAGE plxcobol AS $$
PROCEDURE DIVISION.
    ASSERT N > 0
    CONTINUE
    GOBACK RETURNING N * N.
$$;
SELECT cob_checked(6);

-- DISPLAY -> RAISE NOTICE (observe the notice)
CREATE FUNCTION cob_greet(who text) RETURNS void LANGUAGE plxcobol AS $$
PROCEDURE DIVISION.
    DISPLAY "hello, " WHO
    GOBACK.
$$;
SET client_min_messages = notice;
SELECT cob_greet('world');
SET client_min_messages = warning;

-- CALL a procedure
CREATE TEMP TABLE cob_log(msg text);
CREATE PROCEDURE cob_note(m text) LANGUAGE sql AS $$ INSERT INTO cob_log VALUES (m) $$;
CREATE FUNCTION cob_donote() RETURNS void LANGUAGE plxcobol AS $$
PROCEDURE DIVISION.
    CALL "cob_note" USING "logged"
    GOBACK.
$$;
SELECT cob_donote();
SELECT msg FROM cob_log;
