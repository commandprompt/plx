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
