"""Programs for the plx differential test.

Each entry holds one small program written as a plpgsql reference plus one
body per dialect. Keep the dialect bodies idiomatic for their language but
semantically identical to the reference, and keep one behaviour per program
so that a divergence points at a single cause.
"""

# dialect language name -> short tag used in generated function names
DIALECTS = {
    "plxruby": "rb",
    "plxphp": "php",
    "plxjs": "js",
    "plxts": "ts",
    "plxpython3": "py",
    "plxgo": "go",
    "plxcobol": "cob",
    "plxplsql": "pls",
    "plxtsql": "tq",
}

# Schema the cases run against. Regular tables, not TEMP: each statement the
# runner issues is its own psql session.
SETUP = """
DROP TABLE IF EXISTS plxdiff_orders;
CREATE TABLE plxdiff_orders(grp int, amount bigint);
INSERT INTO plxdiff_orders VALUES (1, 10), (1, 20), (1, 30), (2, 5), (3, NULL);
"""

CASES = [

    # ---------------------------------------------------------------- grade
    # Branch selection, text result, and the SQL three-valued comparison:
    # a NULL score makes every comparison NULL, so the else branch wins.
    {
        "name": "grade",
        "args": "score int",
        "returns": "text",
        "calls": ["95", "90", "89", "80", "0", "-5", "NULL"],
        "reference": """
DECLARE
  g text;
BEGIN
  IF score >= 90 THEN
    g := 'A';
  ELSIF score >= 80 THEN
    g := 'B';
  ELSE
    g := 'F';
  END IF;
  RETURN g;
END;
""",
        "bodies": {
            "plxruby": """
g #:: text
if score >= 90
  g = "A"
elsif score >= 80
  g = "B"
else
  g = "F"
end
return g
""",
            "plxphp": """
$g = "F";
if ($score >= 90) { $g = "A"; }
elseif ($score >= 80) { $g = "B"; }
else { $g = "F"; }
return $g;
""",
            "plxjs": """
let g = "F";
if (score >= 90) { g = "A"; }
else if (score >= 80) { g = "B"; }
else { g = "F"; }
return g;
""",
            "plxts": """
let g: string = "F";
if (score >= 90) { g = "A"; }
else if (score >= 80) { g = "B"; }
else { g = "F"; }
return g;
""",
            "plxpython3": """
if score >= 90:
    g = "A"
elif score >= 80:
    g = "B"
else:
    g = "F"
return g
""",
            "plxgo": """
	if score >= 90 {
		return "A"
	} else if score >= 80 {
		return "B"
	} else {
		return "F"
	}
""",
            "plxcobol": """
WORKING-STORAGE SECTION.
01 WS-G PIC X(1).
PROCEDURE DIVISION.
    IF SCORE >= 90
        MOVE "A" TO WS-G
    ELSE
        IF SCORE >= 80
            MOVE "B" TO WS-G
        ELSE
            MOVE "F" TO WS-G
        END-IF
    END-IF
    GOBACK RETURNING WS-G.
""",
            "plxplsql": """
  g VARCHAR2(10);
BEGIN
  IF score >= 90 THEN
    g := 'A';
  ELSIF score >= 80 THEN
    g := 'B';
  ELSE
    g := 'F';
  END IF;
  RETURN g;
END;
""",
            "plxtsql": """
  DECLARE @g varchar(10);
  IF @score >= 90
    SET @g = 'A';
  ELSE IF @score >= 80
    SET @g = 'B';
  ELSE
    SET @g = 'F';
  RETURN @g;
""",
        },
    },

    # ---------------------------------------------------------------- accum
    # A counting loop that accumulates into a bigint. n = 0 must not execute
    # the body at all, and the widening to bigint must survive the dialect.
    {
        "name": "accum",
        "args": "n int",
        "returns": "bigint",
        "calls": ["0", "1", "10", "100", "-3"],
        "reference": """
DECLARE
  total bigint := 0;
  i int;
BEGIN
  FOR i IN 1..n LOOP
    total := total + i;
  END LOOP;
  RETURN total;
END;
""",
        "bodies": {
            "plxruby": """
total = 0 #:: bigint
for i in 1..n
  total = total + i
end
return total
""",
            "plxphp": """
$total = 0 /*:: bigint */;
for ($i = 1; $i <= $n; $i++) {
  $total = $total + $i;
}
return $total;
""",
            "plxjs": """
let total = 0 /*:: bigint */;
for (let i = 1; i <= n; i++) {
  total = total + i;
}
return total;
""",
            "plxts": """
let total: bigint = 0;
for (let i: number = 1; i <= n; i++) {
  total = total + i;
}
return total;
""",
            "plxpython3": """
total = 0 #:: bigint
for i in range(1, n + 1):
    total = total + i
return total
""",
            "plxgo": """
	var total int64 = 0
	for i := 1; i <= n; i++ {
		total = total + int64(i)
	}
	return total
""",
            "plxcobol": """
WORKING-STORAGE SECTION.
01 WS-TOTAL PIC S9(18) VALUE 0.
01 WS-I     PIC S9(9).
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > N
        ADD WS-I TO WS-TOTAL
    END-PERFORM
    GOBACK RETURNING WS-TOTAL.
""",
            "plxplsql": """
  total NUMBER := 0;
  i NUMBER;
BEGIN
  FOR i IN 1..n LOOP
    total := total + i;
  END LOOP;
  RETURN total;
END;
""",
            "plxtsql": """
  DECLARE @i int = 1;
  DECLARE @total bigint = 0;
  WHILE @i <= @n
  BEGIN
    SET @total = @total + @i;
    SET @i += 1;
  END
  RETURN @total;
""",
        },
    },

    # ----------------------------------------------------------------- divi
    # Integer division, including the negative operands where truncation
    # direction matters and the divide-by-zero that must raise 22012.
    {
        "name": "divi",
        "args": "a int, b int",
        "returns": "int",
        "calls": ["7, 2", "-7, 2", "7, -2", "-7, -2", "7, 0", "NULL, 2",
                  "7, NULL"],
        "reference": """
BEGIN
  RETURN a / b;
END;
""",
        "bodies": {
            "plxruby": """
return a / b
""",
            "plxphp": """
return $a / $b;
""",
            "plxjs": """
return a / b;
""",
            "plxts": """
return a / b;
""",
            "plxpython3": """
return a / b
""",
            "plxgo": """
	return a / b
""",
            "plxcobol": """
WORKING-STORAGE SECTION.
01 WS-Q PIC S9(9).
PROCEDURE DIVISION.
    COMPUTE WS-Q = A / B
    GOBACK RETURNING WS-Q.
""",
            "plxplsql": """
BEGIN
  RETURN a / b;
END;
""",
            "plxtsql": """
  RETURN @a / @b;
""",
        },
    },

    # ----------------------------------------------------------------- modi
    # Remainder, same operand matrix. plpgsql % truncates toward zero, so the
    # sign of the result follows the dividend.
    {
        "name": "modi",
        "args": "a int, b int",
        "returns": "int",
        "calls": ["7, 2", "-7, 2", "7, -2", "-7, -2", "7, 0", "NULL, 2"],
        "reference": """
BEGIN
  RETURN a % b;
END;
""",
        "bodies": {
            "plxruby": """
return a % b
""",
            "plxphp": """
return $a % $b;
""",
            "plxjs": """
return a % b;
""",
            "plxts": """
return a % b;
""",
            "plxpython3": """
return a % b
""",
            "plxgo": """
	return a % b
""",
            "plxcobol": """
WORKING-STORAGE SECTION.
01 WS-R PIC S9(9).
PROCEDURE DIVISION.
    COMPUTE WS-R = A % B
    GOBACK RETURNING WS-R.
""",
            "plxplsql": """
BEGIN
  RETURN a % b;
END;
""",
            "plxtsql": """
  RETURN @a % @b;
""",
        },
    },

    # --------------------------------------------------------------- interp
    # String building through each dialect's native interpolation. A NULL
    # operand must poison the whole result the way SQL concatenation does,
    # rather than being silently rendered as an empty string.
    {
        "name": "interp",
        "args": "nm text, n int",
        "returns": "text",
        "calls": ["'bob', 3", "NULL, 3", "'bob', NULL", "'', 0"],
        "reference": """
BEGIN
  RETURN 'user ' || nm || ' has ' || n || ' items';
END;
""",
        "bodies": {
            "plxruby": """
return "user #{nm} has #{n} items"
""",
            "plxphp": """
return "user $nm has {$n} items";
""",
            "plxjs": """
return `user ${nm} has ${n} items`;
""",
            "plxts": """
return `user ${nm} has ${n} items`;
""",
            "plxpython3": """
return f"user {nm} has {n} items"
""",
            "plxgo": """
	return fmt.Sprintf("user %s has %d items", nm, n)
""",
            "plxcobol": """
WORKING-STORAGE SECTION.
01 WS-S PIC X(100) VALUE "".
PROCEDURE DIVISION.
    STRING-APPEND "user " TO WS-S
    STRING-APPEND NM TO WS-S
    STRING-APPEND " has " TO WS-S
    STRING-APPEND N TO WS-S
    STRING-APPEND " items" TO WS-S
    GOBACK RETURNING WS-S.
""",
            "plxplsql": """
BEGIN
  RETURN 'user ' || nm || ' has ' || n || ' items';
END;
""",
            "plxtsql": """
  RETURN 'user ' || @nm || ' has ' || @n || ' items';
""",
        },
        # Interpolation propagates NULL in every dialect that has it, so the
        # reference holds. plxcobol builds this string through the plx_strbuild
        # accumulator, whose append is deliberately not strict, so it still
        # renders a NULL operand as empty.
        "documented": [
            {
                "dialects": ["plxcobol"],
                "calls": ["NULL, 3", "'bob', NULL"],
                "reason": "the plx_strbuild accumulator appends a NULL as "
                          "nothing by design (doc/LIMITATIONS.md)",
            },
        ],
    },

    # ---------------------------------------------------------------- bools
    # SQL three-valued logic: NULL AND TRUE is NULL, but NULL AND FALSE is
    # FALSE, so a dialect cannot treat a NULL operand as merely falsy.
    {
        "name": "bools",
        "args": "a int, b int",
        "returns": "boolean",
        "calls": ["1, 1", "1, -1", "-1, 1", "NULL, 1", "NULL, -1", "1, NULL"],
        "reference": """
BEGIN
  RETURN a > 0 AND b > 0;
END;
""",
        "bodies": {
            "plxruby": """
return a > 0 && b > 0
""",
            "plxphp": """
return $a > 0 && $b > 0;
""",
            "plxjs": """
return a > 0 && b > 0;
""",
            "plxts": """
return a > 0 && b > 0;
""",
            "plxpython3": """
return a > 0 and b > 0
""",
            "plxgo": """
	return a > 0 && b > 0
""",
            "plxplsql": """
BEGIN
  RETURN a > 0 AND b > 0;
END;
""",
            "plxcobol": """
WORKING-STORAGE SECTION.
01 WS-B TYPE boolean.
PROCEDURE DIVISION.
    COMPUTE WS-B = A > 0 AND B > 0
    GOBACK RETURNING WS-B.
""",
            "plxtsql": """
  RETURN @a > 0 AND @b > 0;
""",
        },
    },

    # -------------------------------------------------------------- loopctl
    # continue and break inside a counting loop. The skipped value must not be
    # accumulated and the break must leave the remaining iterations undone.
    {
        "name": "loopctl",
        "args": "n int",
        "returns": "int",
        "calls": ["0", "2", "3", "5", "10"],
        "reference": """
DECLARE
  total int := 0;
  i int;
BEGIN
  FOR i IN 1..n LOOP
    IF i = 3 THEN
      CONTINUE;
    END IF;
    IF i > 7 THEN
      EXIT;
    END IF;
    total := total + i;
  END LOOP;
  RETURN total;
END;
""",
        "bodies": {
            "plxruby": """
total = 0 #:: int
for i in 1..n
  next if i == 3
  break if i > 7
  total = total + i
end
return total
""",
            "plxphp": """
$total = 0;
for ($i = 1; $i <= $n; $i++) {
  if ($i == 3) { continue; }
  if ($i > 7) { break; }
  $total = $total + $i;
}
return $total;
""",
            "plxjs": """
let total = 0;
for (let i = 1; i <= n; i++) {
  if (i === 3) { continue; }
  if (i > 7) { break; }
  total = total + i;
}
return total;
""",
            "plxts": """
let total: number = 0;
for (let i: number = 1; i <= n; i++) {
  if (i === 3) { continue; }
  if (i > 7) { break; }
  total = total + i;
}
return total;
""",
            "plxpython3": """
total = 0
for i in range(1, n + 1):
    if i == 3:
        continue
    if i > 7:
        break
    total = total + i
return total
""",
            "plxgo": """
	total := 0
	for i := 1; i <= n; i++ {
		if i == 3 {
			continue
		}
		if i > 7 {
			break
		}
		total = total + i
	}
	return total
""",
            "plxcobol": """
WORKING-STORAGE SECTION.
01 WS-TOTAL PIC S9(9) VALUE 0.
01 WS-I     PIC S9(9).
PROCEDURE DIVISION.
    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > N
        IF WS-I = 3
            EXIT PERFORM CYCLE
        END-IF
        IF WS-I > 7
            EXIT PERFORM
        END-IF
        ADD WS-I TO WS-TOTAL
    END-PERFORM
    GOBACK RETURNING WS-TOTAL.
""",
            "plxplsql": """
  total int := 0;
  i int;
BEGIN
  FOR i IN 1..n LOOP
    IF i = 3 THEN
      CONTINUE;
    END IF;
    IF i > 7 THEN
      EXIT;
    END IF;
    total := total + i;
  END LOOP;
  RETURN total;
END;
""",
            "plxtsql": """
  DECLARE @i int = 0;
  DECLARE @total int = 0;
  WHILE @i < @n
  BEGIN
    SET @i += 1;
    IF @i = 3 CONTINUE;
    IF @i > 7 BREAK;
    SET @total = @total + @i;
  END
  RETURN @total;
""",
        },
    },

    # ----------------------------------------------------------------- qsum
    # Iterating a query and accumulating a column. Group 3 holds a NULL
    # amount, which must poison the running total, and group 4 selects no
    # rows at all, which must leave the initial value untouched.
    #
    # plxcobol and plxtsql are absent on purpose: neither has a row loop that
    # takes a parameter, only an aggregate form, and an aggregate disagrees
    # with a loop over an empty set for reasons that are not plx's doing.
    {
        "name": "qsum",
        "args": "g int",
        "returns": "bigint",
        "calls": ["1", "2", "3", "4"],
        "reference": """
DECLARE
  total bigint := 0;
  r record;
BEGIN
  FOR r IN SELECT amount FROM plxdiff_orders WHERE grp = g LOOP
    total := total + r.amount;
  END LOOP;
  RETURN total;
END;
""",
        "bodies": {
            "plxruby": """
total = 0 #:: bigint
query("SELECT amount FROM plxdiff_orders WHERE grp = #{g}").each do |row|
  total = total + row.amount
end
return total
""",
            "plxphp": """
$total = 0 /*:: bigint */;
foreach (query("SELECT amount FROM plxdiff_orders WHERE grp = {$g}") as $row) {
  $total = $total + $row->amount;
}
return $total;
""",
            "plxjs": """
let total = 0 /*:: bigint */;
for (const row of query(`SELECT amount FROM plxdiff_orders WHERE grp = ${g}`)) {
  total = total + row.amount;
}
return total;
""",
            "plxts": """
let total: bigint = 0;
for (const row of query(`SELECT amount FROM plxdiff_orders WHERE grp = ${g}`)) {
  total = total + row.amount;
}
return total;
""",
            "plxpython3": """
total = 0 #:: bigint
for row in query(f"SELECT amount FROM plxdiff_orders WHERE grp = {g}"):
    total = total + row.amount
return total
""",
            "plxgo": """
	var total int64 = 0
	for _, row := range query(fmt.Sprintf("SELECT amount FROM plxdiff_orders WHERE grp = %d", g)) {
		total = total + row.amount
	}
	return total
""",
            "plxplsql": """
  total bigint := 0;
  r RECORD;
BEGIN
  FOR r IN (SELECT amount FROM plxdiff_orders WHERE grp = g) LOOP
    total := total + r.amount;
  END LOOP;
  RETURN total;
END;
""",
        },
    },

    # -------------------------------------------------------------- safediv
    # Catching a raised error and returning a fallback. Division by zero is
    # the error every dialect can provoke identically, and a NULL operand must
    # produce NULL rather than entering the handler.
    #
    # plxgo and plxcobol are absent on purpose: Go's panic maps to RAISE with
    # no recover(), and COBOL has no handler construct, so neither can express
    # catching at all.
    {
        "name": "safediv",
        "args": "a int, b int",
        "returns": "int",
        "calls": ["10, 2", "10, 0", "-9, 2", "NULL, 2", "0, 0"],
        "reference": """
BEGIN
  RETURN a / b;
EXCEPTION WHEN OTHERS THEN
  RETURN -1;
END;
""",
        "bodies": {
            "plxruby": """
begin
  return a / b
rescue => e
  return -1
end
""",
            "plxphp": """
try { return $a / $b; }
catch (Exception $e) { return -1; }
""",
            "plxjs": """
try { return a / b; }
catch (e) { return -1; }
""",
            "plxts": """
try { return a / b; }
catch (e) { return -1; }
""",
            "plxpython3": """
try:
    return a / b
except Exception as e:
    return -1
""",
            "plxplsql": """
BEGIN
  RETURN a / b;
EXCEPTION WHEN OTHERS THEN
  RETURN -1;
END;
""",
            "plxtsql": """
  DECLARE @r int;
  BEGIN TRY
    SET @r = @a / @b;
  END TRY
  BEGIN CATCH
    SET @r = -1;
  END CATCH
  RETURN @r;
""",
        },
    },

    # ------------------------------------------------------------- arraysum
    # Iterating an array. An empty array must leave the total untouched, a
    # NULL element must poison it, and a NULL array itself must raise the same
    # way the reference does rather than quietly iterating nothing.
    #
    # plxtsql is absent: it has no FOREACH form over an array.
    {
        "name": "arraysum",
        "args": "a int[]",
        "returns": "bigint",
        "calls": ["ARRAY[1,2,3]", "ARRAY[5]", "ARRAY[]::int[]",
                  "ARRAY[1,NULL,3]", "NULL::int[]"],
        "reference": """
DECLARE
  total bigint := 0;
  v int;
BEGIN
  FOREACH v IN ARRAY a LOOP
    total := total + v;
  END LOOP;
  RETURN total;
END;
""",
        "bodies": {
            "plxruby": """
total = 0 #:: bigint
v #:: int
a.each do |v|
  total = total + v
end
return total
""",
            "plxphp": """
$total = 0 /*:: bigint */;
$v = 0 /*:: int */;
foreach ($a as $v) {
  $total = $total + $v;
}
return $total;
""",
            "plxjs": """
let total = 0 /*:: bigint */;
let v = 0 /*:: int */;
for (const v of a) {
  total = total + v;
}
return total;
""",
            "plxts": """
let total: bigint = 0;
let v: number = 0;
for (const v of a) {
  total = total + v;
}
return total;
""",
            "plxpython3": """
total = 0 #:: bigint
v #:: int
for v in a:
    total = total + v
return total
""",
            "plxgo": """
	var total int64 = 0
	var v int
	for _, v := range a {
		total = total + int64(v)
	}
	return total
""",
            "plxcobol": """
WORKING-STORAGE SECTION.
01 WS-TOTAL PIC S9(18) VALUE 0.
01 WS-V PIC S9(9).
PROCEDURE DIVISION.
    PERFORM WS-V OVER ARRAY A
        ADD WS-V TO WS-TOTAL
    END-PERFORM
    GOBACK RETURNING WS-TOTAL.
""",
            "plxplsql": """
  total bigint := 0;
  v int;
BEGIN
  FOREACH v IN ARRAY a LOOP
    total := total + v;
  END LOOP;
  RETURN total;
END;
""",
        },
    },

    # ------------------------------------------------------------------ cmp
    # Equality and inequality, where a dialect's == and != have to become SQL
    # comparisons rather than anything stricter. With a NULL operand both
    # comparisons are unknown, so neither branch is taken.
    {
        "name": "cmp",
        "args": "a int, b int",
        "returns": "text",
        "calls": ["3, 3", "3, 4", "-1, -1", "NULL, 3", "NULL, NULL", "0, 0"],
        "reference": """
BEGIN
  IF a = b THEN
    RETURN 'eq';
  END IF;
  IF a <> b THEN
    RETURN 'ne';
  END IF;
  RETURN 'unknown';
END;
""",
        "bodies": {
            "plxruby": """
if a == b
  return "eq"
end
if a != b
  return "ne"
end
return "unknown"
""",
            "plxphp": """
if ($a == $b) { return "eq"; }
if ($a != $b) { return "ne"; }
return "unknown";
""",
            "plxjs": """
if (a === b) { return "eq"; }
if (a !== b) { return "ne"; }
return "unknown";
""",
            "plxts": """
if (a === b) { return "eq"; }
if (a !== b) { return "ne"; }
return "unknown";
""",
            "plxpython3": """
if a == b:
    return "eq"
if a != b:
    return "ne"
return "unknown"
""",
            "plxgo": """
	if a == b {
		return "eq"
	}
	if a != b {
		return "ne"
	}
	return "unknown"
""",
            "plxcobol": """
WORKING-STORAGE SECTION.
01 WS-R PIC X(7).
PROCEDURE DIVISION.
    MOVE "unknown" TO WS-R
    IF A = B
        MOVE "eq" TO WS-R
    ELSE
        IF A NOT = B
            MOVE "ne" TO WS-R
        END-IF
    END-IF
    GOBACK RETURNING WS-R.
""",
            "plxplsql": """
BEGIN
  IF a = b THEN
    RETURN 'eq';
  END IF;
  IF a <> b THEN
    RETURN 'ne';
  END IF;
  RETURN 'unknown';
END;
""",
            "plxtsql": """
  IF @a = @b
    RETURN 'eq';
  IF @a <> @b
    RETURN 'ne';
  RETURN 'unknown';
""",
        },
    },

    # ------------------------------------------------------------- strupper
    # The per-dialect uppercase idiom has to reach SQL upper(). A NULL input
    # must stay NULL rather than becoming the empty string.
    {
        "name": "strupper",
        "args": "s text",
        "returns": "text",
        "calls": ["'hello'", "'Hello World'", "''", "NULL"],
        "reference": """
BEGIN
  RETURN upper(s);
END;
""",
        "bodies": {
            "plxruby": """
return upper(s)
""",
            "plxphp": """
return upper($s);
""",
            "plxjs": """
return upper(s);
""",
            "plxts": """
return upper(s);
""",
            "plxpython3": """
return upper(s)
""",
            "plxgo": """
	return strings.ToUpper(s)
""",
            "plxcobol": """
WORKING-STORAGE SECTION.
01 WS-S PIC X(100).
PROCEDURE DIVISION.
    COMPUTE WS-S = upper(S)
    GOBACK RETURNING WS-S.
""",
            "plxplsql": """
BEGIN
  RETURN UPPER(s);
END;
""",
            "plxtsql": """
  RETURN UPPER(@s);
""",
        },
    },

    # ------------------------------------------------------- strupper_native
    # The same program written with each language's own uppercase idiom rather
    # than a SQL call. plxgo maps a subset of the Go standard library, so
    # strings.ToUpper reaches upper() and agrees with the reference. No other
    # dialect maps its standard library: the method form is passed through and
    # fails when the function is called, which is the documented design and not
    # a defect. This case exists so that stays true on purpose. If a dialect
    # ever gains the mapping, the check reports a stale exemption and the
    # documentation gets updated with it.
    {
        "name": "strupper_native",
        "args": "s text",
        "returns": "text",
        "calls": ["'hello'", "''", "NULL"],
        "reference": """
BEGIN
  RETURN upper(s);
END;
""",
        "bodies": {
            "plxruby": """
return s.upcase
""",
            "plxphp": """
return strtoupper($s);
""",
            "plxjs": """
return s.toUpperCase();
""",
            "plxts": """
return s.toUpperCase();
""",
            "plxpython3": """
return s.upper()
""",
            "plxgo": """
	return strings.ToUpper(s)
""",
        },
        "documented": [
            {
                "dialects": ["plxruby", "plxphp", "plxjs", "plxts",
                             "plxpython3"],
                "calls": ["'hello'", "''", "NULL"],
                "reason": "only plxgo maps standard-library calls; elsewhere a "
                          "method or library call is passed through to SQL and "
                          "must name a real function (doc/LIMITATIONS.md)",
            },
        ],
    },
]
