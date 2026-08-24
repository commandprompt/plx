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
        # doc/LIMITATIONS.md: "Interpolating a NULL yields an empty string, and
        # never turns the whole string NULL." The dialects that build the
        # string through interpolation or a builder therefore keep going where
        # SQL concatenation would have produced NULL. plxplsql and plxtsql use
        # || directly and so are held to the reference.
        "documented": [
            {
                "dialects": ["plxruby", "plxphp", "plxjs", "plxts",
                             "plxpython3", "plxgo", "plxcobol"],
                "calls": ["NULL, 3", "'bob', NULL"],
                "reason": "interpolating NULL yields an empty string "
                          "(doc/LIMITATIONS.md)",
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
]
