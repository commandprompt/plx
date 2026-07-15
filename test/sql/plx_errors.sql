-- plx rejection tests: each CREATE FUNCTION must fail at CREATE time with a
-- precise error and source line number.
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;

-- plxruby: nested def
CREATE FUNCTION e_rb_def() RETURNS int LANGUAGE plxruby AS $$
def helper
  1
end
return helper
$$;

-- plxruby: unresolvable local type
CREATE FUNCTION e_rb_type() RETURNS int LANGUAGE plxruby AS $$
x = some_call()
return x
$$;

-- plxruby: next outside a loop
CREATE FUNCTION e_rb_next() RETURNS int LANGUAGE plxruby AS $$
next
return 1
$$;

-- plxruby: assignment with an empty right-hand side
CREATE FUNCTION e_rb_emptyrhs() RETURNS int LANGUAGE plxruby AS $$
x =#:: int
return 1
$$;

-- plxphp: nested function definition
CREATE FUNCTION e_php_fn() RETURNS int LANGUAGE plxphp AS $$
function helper() { return 1; }
return 1;
$$;

-- plxphp: switch fall-through (non-terminated case body)
CREATE FUNCTION e_php_fall(n int) RETURNS text LANGUAGE plxphp AS $$
switch ($n) {
  case 1: $x = "a";
  case 2: return "b";
}
return "c";
$$;

-- plxjs: switch fall-through
CREATE FUNCTION e_js_fall(n int) RETURNS text LANGUAGE plxjs AS $$
switch (n) {
  case 1: let x = "a";
  default: return "b";
}
$$;

-- plxruby: ||= has no faithful lowering
CREATE FUNCTION e_rb_orassign() RETURNS int LANGUAGE plxruby AS $$
x = 0
x ||= 5
return x
$$;

-- plxruby: emit outside a set-returning function
CREATE FUNCTION e_rb_emit() RETURNS int LANGUAGE plxruby AS $$
emit
return 1
$$;

-- plxruby: bare raise outside a handler
CREATE FUNCTION e_rb_reraise() RETURNS int LANGUAGE plxruby AS $$
raise
$$;

-- plxpython3: def is not supported
CREATE FUNCTION e_py_def() RETURNS int LANGUAGE plxpython3 AS $$
def helper():
    return 1
return helper()
$$;

-- plxpython3: class is not supported
CREATE FUNCTION e_py_class() RETURNS int LANGUAGE plxpython3 AS $$
class C:
    pass
return 1
$$;

-- plxpython3: match/case is not supported
CREATE FUNCTION e_py_match(n int) RETURNS int LANGUAGE plxpython3 AS $$
match n:
    case 1:
        return 1
return 0
$$;

-- plxpython3: conditional expression a if c else b
CREATE FUNCTION e_py_ternary(x int) RETURNS int LANGUAGE plxpython3 AS $$
return 1 if x > 0 else 2
$$;

-- plxjs: nested function definition
CREATE FUNCTION e_js_fn() RETURNS int LANGUAGE plxjs AS $$
function helper() { return 1; }
return 1;
$$;

-- plxruby: << on a numeric variable is not a string append; rejected
CREATE FUNCTION e_rb_numshift() RETURNS int LANGUAGE plxruby AS $$
x = 0
x << 2
return x
$$;

-- plxcobol: PERFORM VARYING truncated at UNTIL (must error cleanly, not crash)
CREATE FUNCTION e_cob_varying() RETURNS int LANGUAGE plxcobol AS $$
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL
$$;

-- plxcobol: OCCURS without an element type (PIC/TYPE) is rejected
CREATE FUNCTION e_cob_occurs() RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-T OCCURS 5 TIMES.
PROCEDURE DIVISION.
    GOBACK RETURNING 1.
$$;

-- plxcobol: data item with no PIC/TYPE/CONSTANT
CREATE FUNCTION e_cob_notype() RETURNS int LANGUAGE plxcobol AS $$
WORKING-STORAGE SECTION.
01 WS-X.
PROCEDURE DIVISION.
    GOBACK RETURNING 1.
$$;

-- plxcobol: out-of-line PERFORM of a paragraph is not supported
CREATE FUNCTION e_cob_para() RETURNS int LANGUAGE plxcobol AS $$
PROCEDURE DIVISION.
    PERFORM SOME-PARAGRAPH
    GOBACK RETURNING 1.
$$;

-- plxcobol: EXIT without PERFORM
CREATE FUNCTION e_cob_exit() RETURNS int LANGUAGE plxcobol AS $$
PROCEDURE DIVISION.
    EXIT
    GOBACK RETURNING 1.
$$;

-- plxcobol: missing END-IF scope terminator
CREATE FUNCTION e_cob_endif() RETURNS int LANGUAGE plxcobol AS $$
PROCEDURE DIVISION.
    IF 1 = 1
        MOVE 2 TO WS-X
    GOBACK RETURNING 1.
$$;

-- plxcobol: unterminated string literal
CREATE FUNCTION e_cob_str() RETURNS void LANGUAGE plxcobol AS $$
PROCEDURE DIVISION.
    DISPLAY "unterminated
$$;

-- plxtsql: the @@ROWCOUNT global variable is not supported
CREATE FUNCTION e_tq_rowcount() RETURNS int LANGUAGE plxtsql AS $$
  DECLARE @n int;
  SET @n = @@ROWCOUNT;
  RETURN @n;
$$;

-- plxtsql: DECLARE of a TABLE variable is not supported
CREATE FUNCTION e_tq_tablevar() RETURNS int LANGUAGE plxtsql AS $$
  DECLARE @t TABLE (id int);
  RETURN 1;
$$;

-- plxtsql: transaction control is not allowed in a function
CREATE FUNCTION e_tq_commit() RETURNS int LANGUAGE plxtsql AS $$
  DECLARE @x int = 1;
  COMMIT;
  RETURN @x;
$$;

-- plxtsql: EXEC of a stored procedure (not dynamic SQL) is not supported
CREATE FUNCTION e_tq_execproc() RETURNS int LANGUAGE plxtsql AS $$
  EXEC some_procedure;
  RETURN 1;
$$;
