#!/usr/bin/env python3
"""plx transpiler fuzzer.

Feeds mutated and pathological function bodies to CREATE FUNCTION in each plx
dialect and watches for backend crashes (lost connection) or hangs (transpiler
infinite loop / non-interruptible). A clean run means every input either
transpiled or raised a normal error, and the server stayed up.

A statement the server cancels at statement_timeout counts as a hang, and an
out-of-memory error counts as a finding too. Before it starts, the fuzzer
checks that every plx language exists in the target database, because a body
in a missing language raises an ordinary error and would test nothing.

Usage: python3 test/fuzz.py [iterations]
       python3 test/fuzz.py --selftest   (check the classifier only, no server)

The usual libpq variables (PGHOST, PGPORT, PGDATABASE) choose the server and
database. PLX_PSQL names the psql binary; the default is the first psql on PATH.
"""
import os, random, re, shutil, subprocess, sys

# PLX_PSQL names the psql to use; otherwise the first psql on PATH.
PSQL = os.environ.get("PLX_PSQL") or shutil.which("psql") or "/usr/local/pgsql/bin/psql"
ENV = dict(os.environ, PGHOST=os.environ.get("PGHOST", "/tmp"), PGUSER="postgres")
DIALECTS = ["plxruby", "plxphp", "plxjs", "plxpython3", "plxcobol", "plxplsql",
            "plxts", "plxtsql", "plxgo", "plxlua", "plxcsharp"]
random.seed(20260714)

SEEDS = {
 "plxruby": [
  "return a + b * 2",
  "x = 0\nfor i in 1..n\n  x = x + i\nend\nreturn x",
  'grade #:: text\nif a\n  grade = "A"\nelsif b\n  grade = "B"\nelse\n  grade = "C"\nend\nreturn grade',
  'raise notice: "hi #{x} there"',
  "begin\n  return 1\nrescue => e\n  raise notice: e.message\n  return -1\nend",
  'query("select a from t").each do |r|\n  x = r.a\nend\nreturn x',
  'u = fetch_one("select 1")\nreturn u.a',
  "return x >= 0 ? 1 : 2",
 ],
 "plxphp": [
  "return $a + $b;",
  "$x = 0; for ($i = 1; $i <= $n; $i++) { $x = $x + $i; } return $x;",
  'if ($a) { return 1; } elseif ($b) { return 2; } else { return 3; }',
  'try { return 1; } catch (\\Exception $e) { raise("notice", $e->message); return -1; }',
  'foreach (query("select a from t") as $r) { $x = $r->a; } return $x;',
  '$s = "hi $x there {$y}"; return $s;',
  'throw new Exception("bad {$x}");',
 ],
 "plxjs": [
  "return a + b * 2;",
  "let x = 0; for (let i = 1; i <= n; i++) { x = x + i; } return x;",
  'if (a) { return 1; } else if (b) { return 2; } else { return 3; }',
  'let s = `hi ${x} there`; return s;',
  'for (const r of query(`select a from t`)) { x = r.a; } return x;',
  'try { return 1; } catch (e) { raise("notice", `${e.message}`); return -1; }',
  'throw new Error(`bad ${x}`);',
 ],
 "plxpython3": [
  "return a + b * 2\n",
  "total = 0\nfor i in range(1, n + 1):\n    total = total + i\nreturn total\n",
  'if a:\n    return 1\nelif b:\n    return 2\nelse:\n    return 3\n',
  'return f"hi {x} there"\n',
  'for row in query("select a from t"):\n    x = row.a\nreturn x\n',
  'try:\n    return 1\nexcept Exception as e:\n    return -1\n',
  'if x > 0:\n    pass\nelse:\n    x = 0\nreturn x\n',
  'assert n > 0, "bad"\nraise ValueError(f"no {x}")\n',
 ],
 "plxcobol": [
  "PROCEDURE DIVISION.\n    GOBACK RETURNING 1.",
  "WORKING-STORAGE SECTION.\n01 WS-I PIC 9(9).\nPROCEDURE DIVISION.\n    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > 10\n        CONTINUE\n    END-PERFORM\n    GOBACK RETURNING WS-I.",
  'WORKING-STORAGE SECTION.\n01 WS-R PIC X(4).\nPROCEDURE DIVISION.\n    IF 1 IS GREATER THAN OR EQUAL TO 0\n        MOVE "yes" TO WS-R\n    ELSE\n        MOVE "no" TO WS-R\n    END-IF\n    GOBACK RETURNING WS-R.',
  'WORKING-STORAGE SECTION.\n01 WS-R PIC X(8).\nPROCEDURE DIVISION.\n    EVALUATE 2\n        WHEN 1\n            MOVE "one" TO WS-R\n        WHEN OTHER\n            MOVE "many" TO WS-R\n    END-EVALUATE\n    GOBACK RETURNING WS-R.',
  'WORKING-STORAGE SECTION.\n01 WS-S PIC X(1) VALUE "".\n01 WS-I PIC 9(9).\nPROCEDURE DIVISION.\n    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL WS-I > 5\n        STRING-APPEND "x" TO WS-S\n    END-PERFORM\n    GOBACK RETURNING WS-S.',
  'WORKING-STORAGE SECTION.\n01 WS-M PIC X(80).\nPROCEDURE DIVISION.\n    BEGIN-TRY\n        COMPUTE WS-M = mod(1, 0)\n    WHEN OTHER\n        MOVE "err" TO WS-M\n    END-TRY\n    GOBACK RETURNING WS-M.',
  'PROCEDURE DIVISION.\n    DISPLAY "hi" 1 "there"\n    GOBACK RETURNING 1.',
  "WORKING-STORAGE SECTION.\n01 WS-T PIC S9(9) VALUE 0.\nPROCEDURE DIVISION.\n    ADD 1 2 3 TO WS-T\n    GOBACK RETURNING WS-T.",
 ],
 "plxplsql": [
  "BEGIN RETURN 1 + 2; END;",
  "  v NUMBER := 0;\nBEGIN\n  FOR i IN 1..10 LOOP\n    v := v + i;\n  END LOOP;\n  RETURN v;\nEND;",
  "  r VARCHAR2(10);\nBEGIN\n  IF 1 > 0 THEN r := 'p'; ELSE r := 'n'; END IF;\n  RETURN r;\nEND;",
  "BEGIN\n  DBMS_OUTPUT.PUT_LINE('hi ' || 42);\n  RETURN 'x';\nEND;",
  "BEGIN\n  RAISE_APPLICATION_ERROR(-20001, 'bad');\nEXCEPTION WHEN OTHERS THEN RETURN -1;\nEND;",
  "  c bigint;\nBEGIN\n  EXECUTE IMMEDIATE 'SELECT 1' INTO c;\n  RETURN c;\nEND;",
  "  CURSOR c IS SELECT 1;\n  v int;\nBEGIN\n  OPEN c; FETCH c INTO v; CLOSE c;\n  RETURN v;\nEND;",
  "BEGIN\n  RETURN NVL(NULL, SYSDATE);\nEND;",
 ],
 "plxts": [
  "let x: number = 1; return x + 2;",
  "let total: bigint = 0;\nfor (let i: number = 1; i <= 10; i++) { total = total + i; }\nreturn total;",
  'let r: string = "F";\nif (1 > 0) { r = "A"; }\nreturn r;',
  "let m: number | null;\nm = 5;\nreturn m;",
  "let a: numeric(10,2) = 1.5;\nreturn a;",
  "let v: number[] = null;\nreturn 1;",
  "let s: boolean = true;\nreturn s;",
 ],
 "plxtsql": [
  "DECLARE @x int = 1;\nRETURN @x + 2;",
  "DECLARE @i int = 0;\nDECLARE @s int = 0;\nWHILE @i < 10\nBEGIN\n  SET @s = @s + @i;\n  SET @i += 1;\nEND\nRETURN @s;",
  "DECLARE @g varchar(10);\nIF 1 > 0\n  SET @g = 'p';\nELSE\n  SET @g = 'n';\nRETURN @g;",
  "DECLARE @c int;\nSELECT @c = count(*) FROM (VALUES (1)) AS v(x);\nRETURN @c;",
  "BEGIN TRY\n  THROW 50000, 'bad', 1;\nEND TRY\nBEGIN CATCH\n  RETURN 'e' + ERROR_MESSAGE();\nEND CATCH\nRETURN 'ok';",
  "PRINT 'hi';\nRAISERROR('bad', 16, 1);\nRETURN 1;",
  "DECLARE @n int = 4;\nRETURN IIF(@n > 0, CONVERT(varchar, @n), ISNULL(NULL, 'x'));",
  "DECLARE @a int, @b int;\nSELECT @a = 10, @b = 32;\nRETURN @a + @b;",
 ],
 "plxlua": [
  "return a + b * 2",
  "local x = 0\nfor i = 1, n do\n  x = x + i\nend\nreturn x",
  'local g\nif a then\n  g = "A"\nelseif b then\n  g = "B"\nelse\n  g = "C"\nend\nreturn g',
  'local s = ""\nfor i = 10, 1, -2 do s = s .. i .. "," end\nreturn s:sub(2, -2) .. #s',
  'local ok, err = pcall(function()\n  execute("select 1/0")\nend)\nif not ok then print(err, err.sqlstate) end\nreturn err',
  'for r in query("select a from t where b = $1", x) do\n  y = r.a\nend\nreturn y',
  'local u = fetch_one("select 1 as a")\nreturn u.a',
  'local t = {1, 2, 3}\nfor i, v in ipairs(t) do\n  if v == 2 then goto continue end\n  ::continue::\nend\nreturn n and n * 2 or "none"',
  'repeat\n  local k = 1\n  if k then break end\nuntil k > 1 --[[ c ]] return [==[x]==] .. string.format("%5d|%s", 1, "a")',
  'local a, b = 1, 2\na, b = b, a\nerror{message = "m", sqlstate = "22023"}',
 ],
 "plxcsharp": [
  "return a + b * 2;",
  "long total = 0;\nfor (int i = 1; i <= n; i++)\n{\n    total += i;\n}\nreturn total;",
  'string g;\nif (a > 0) g = "A";\nelse if (a < 0) { g = "B"; }\nelse g = "C";\nreturn g;',
  'string s = $"hi {x} there {(y > 1 ? "a" : "b")} {z:F2}";\nreturn s + @"C:\\p" + """\n    raw\n    """;',
  'foreach (var r in query($"SELECT a FROM t WHERE b = {x}")) { y += r.a; }\nreturn y;',
  'try { return 1 / x; }\ncatch (DivideByZeroException e) { Console.WriteLine(e.Message); throw; }\ncatch (PostgresException e) when (e.SqlState == "23505") { return -1; }\nfinally { perform("SELECT 1"); }',
  'switch (n) { case 1: case 2: r = "x"; break; default: { r = "y"; break; } }\nreturn r;',
  'var sb = new StringBuilder();\ndo { sb.Append(i).AppendLine(); i++; } while (i < 3);\nint[] a = {1, 2};\nreturn sb.ToString() + a[0] + s.Substring(1).ToUpper() + (x switch { 1 => "a", _ => "b" });',
  'throw new PostgresException("m", "ERROR", "ERROR", "P0001", detail: $"d {x}");',
 ],
 "plxgo": [
  "return 1 + 2 * 3",
  "acc := 1\nfor i := 1; i <= 10; i++ {\n\tacc *= i\n}\nreturn acc",
  "if 1 > 0 {\n\treturn 1\n} else if 1 < 0 {\n\treturn -1\n}\nreturn 0",
  "switch 2 {\ncase 1:\n\treturn 10\ndefault:\n\treturn 0\n}",
  "var s []int\nfor i := range 5 {\n\ts = append(s, i*i)\n}\nreturn len(s)",
  "a := []int{1, 2, 3}\nreturn a[0] + a[2]",
  "fmt.Println(\"hi\", 1)\npanic(\"bad\")",
  "a, b := 1, 2\na, b = b, a\nreturn a",
  "s := \"a\" + (\"b\" + \"c\")\ns += strconv.Itoa(3)\nreturn len(s + fmt.Sprintf(\"%d\", 1))",
 ],
}
SPECIALS = [b'"', b"'", b"`", b"#{", b"${", b"{$", b"*/", b"/*", b"\\", b"(", b")",
            b"{", b"}", b"[", b"]", b"end", b"..", b"...", b"do |", b"|", b"::",
            b"->", b"=>", b";", b"raise", b"query(", b"fetch_one(", b"\n",
            b'\n    ', b'\n\t', b':', b'    ', b'if ', b'for ', b'range(',
            b'f"', b'except ', b'pass', b'\n        ', b'elif ',
            b'END-IF', b'END-PERFORM', b'PERFORM ', b'PIC ', b'MOVE ',
            b'GOBACK', b'UNTIL', b'STRING-APPEND ', b'WS-', b'.', b'%', b',',
            b'@', b'@@', b':=', b'++', b'--', b'&&', b'||', b'range ', b'func ',
            b'DECLARE ', b'BEGIN TRY', b'END TRY', b'BEGIN CATCH', b'THROW ',
            b'SET ', b'PRINT ', b'switch ', b'case ', b'default:', b'[]int{',
            b'else if ', b'defer ', b'go ', b'RAISERROR(',
            b'--', b'--[[', b']]', b'[[', b'[==[', b']==]', b'~=', b'#',
            b'local ', b'then ', b'elseif ', b'repeat ', b'until ', b'goto ',
            b'::continue::', b'pcall(function() ', b'ipairs(', b'function ',
            b' and ', b' or ', b'nil', b'//', b'\\z', b'\\u{', b'<const>',
            b'$"', b'@"', b'$@"',
            b'"""', b'$$"""', b"'\\u", b'catch (', b'when (',
            b'foreach (', b'yield return ', b'?? ', b'?.', b'=>', b' switch {',
            b'new[] {', b'{{']

def mutate(s):
    b = bytearray(s.encode("utf-8", "ignore"))
    if not b:
        b = bytearray(b"x")
    for _ in range(random.randint(1, 4)):
        op = random.randint(0, 6)
        if op == 0:
            del b[:random.randint(0, len(b))]
        elif op == 1 and b:
            del b[random.randrange(len(b))]
        elif op == 2:
            b[random.randrange(len(b) + 1):0] = bytes([random.randint(1, 126)])
        elif op == 3 and b:
            b[random.randrange(len(b))] = random.randint(1, 126)
        elif op == 4 and b:
            i = random.randrange(len(b)); b[i:i] = bytes(b[i:i + 1]) * random.randint(1, 40)
        elif op == 5:
            sp = random.choice(SPECIALS); i = random.randrange(len(b) + 1); b[i:i] = sp * random.randint(1, 15)
        else:
            i = random.randrange(len(b) + 1); b[i:i] = b"end " * random.randint(1, 15)
    return b.decode("utf-8", "ignore")

def pathological():
    return [
     ("plxruby", "if x\n" * 4000 + "y = 1\n" + "end\n" * 4000),
     ("plxjs", "if (x) {" * 4000 + "}" * 4000),
     ("plxphp", "if ($x) {" * 4000 + "}" * 4000),
     ("plxruby", "begin\n" * 3000 + "x = 1\n" + "end\n" * 3000),
     ("plxruby", "return " + "a ? b : " * 4000 + "c"),
     ("plxjs", "return `" + "${" * 3000 + "x" + "}" * 3000 + "`;"),
     ("plxruby", 'return "' + "#{" * 3000 + "x" + "}" * 3000 + '"'),
     ("plxphp", 'return "' + "{$" * 3000 + "x" + "}" * 3000 + '";'),
     ("plxruby", "x" * 200000 + " = 1\nreturn x"),
     ("plxphp", '$s = "' + "a" * 300000),          # unterminated string
     ("plxruby", "(" * 8000),
     ("plxjs", "for (let i = " + "(" * 5000),
     ("plxruby", "query(" * 3000),
     ("plxjs", "/*" + "a" * 200000),               # unterminated block comment
     ("plxphp", "for (" + ";" * 100000 + ")"),
     ("plxpython3", "if x:\n" * 4000 + "    return 1\n"),
     ("plxpython3", "return f\"" + "{" * 3000 + "x" + "}" * 3000 + "\"\n"),
     ("plxpython3", "\n".join("    " * i + "if x:" for i in range(2000))),
     ("plxpython3", "for i in range(" + "(" * 5000),
     # plxcobol
     ("plxcobol", "PROCEDURE DIVISION.\n" + "    IF 1 = 1\n" * 4000 +
      "        CONTINUE\n" + "    END-IF\n" * 4000 + "    GOBACK RETURNING 1."),
     ("plxcobol", "PROCEDURE DIVISION.\n    PERFORM VARYING WS-I FROM 1 BY 1 UNTIL"),
     ("plxcobol", "WORKING-STORAGE SECTION.\n01 WS-X PIC 9(999999999999).\n"
      "PROCEDURE DIVISION.\n    GOBACK RETURNING 1."),
     ("plxcobol", 'PROCEDURE DIVISION.\n    DISPLAY "' + "a" * 200000),
     ("plxcobol", "PROCEDURE DIVISION.\n    COMPUTE X = " + "(" * 8000),
     ("plxcobol", "PROCEDURE DIVISION.\n" + "    PERFORM UNTIL 1 = 1\n" * 3000 +
      "        CONTINUE\n" + "    END-PERFORM\n" * 3000 + "    GOBACK RETURNING 1."),
     ("plxcobol", "*> " + "a" * 200000 + "\nPROCEDURE DIVISION.\n    GOBACK RETURNING 1."),
     ("plxcobol", "WORKING-STORAGE SECTION.\n" + "01 WS-A PIC X(1).\n" * 5000 +
      "PROCEDURE DIVISION.\n    GOBACK RETURNING 1."),
     ("plxcobol", "PROCEDURE DIVISION.\n    MOVE " + "mod(1," * 3000 + "2" + ")" * 3000 + " TO X."),
     # plxplsql
     ("plxplsql", "BEGIN\n" + "IF 1=1 THEN\n" * 4000 + "NULL;\n" + "END IF;\n" * 4000 + "END;"),
     ("plxplsql", "BEGIN " * 3000 + "NULL; " + "END; " * 3000),
     ("plxplsql", "BEGIN RETURN " + "(" * 8000 + "1"),
     ("plxplsql", "BEGIN DBMS_OUTPUT.PUT_LINE(" + "(" * 5000),
     ("plxplsql", "BEGIN RAISE_APPLICATION_ERROR(-20001," + "'a'" * 5000),
     ("plxplsql", "  v VARCHAR2(" + "9" * 100000 + ");\nBEGIN NULL; END;"),
     ("plxplsql", "BEGIN\n  v := 'unterminated"),
     ("plxplsql", "/*" + "a" * 200000),
     # plxts
     ("plxts", "let x: " + "number | " * 5000 + "null = 0; return x;"),
     ("plxts", "let x: numeric(" + "(" * 8000),
     ("plxts", "let x: number = " + "(" * 8000),
     ("plxts", "for (let i: number = 1; " * 3000),
     ("plxts", "let s: string = `" + "a" * 200000),
     ("plxts", "let x: number[" + "[" * 5000),
     # plxtsql
     ("plxtsql", "WHILE 1=1 BEGIN\n" * 3000 + "SET @x = 1;\n" + "END\n" * 3000 + "RETURN 1;"),
     ("plxtsql", "BEGIN TRY " * 3000),
     ("plxtsql", "DECLARE @x int = " + "(" * 8000),
     ("plxtsql", "IF 1=1 " * 4000 + "PRINT 'x'"),
     ("plxtsql", "SELECT " + "@a = 1, " * 3000 + "@b = 2"),
     ("plxtsql", "DECLARE @s varchar = 'unterminated"),
     ("plxtsql", "RAISERROR(" + "'a'," * 5000),
     ("plxtsql", "PRINT " + "'a' + " * 5000 + "'b'"),
     # plxgo
     ("plxgo", "for {\n" * 4000 + "x := 1\n" + "}\n" * 4000 + "return 1"),
     ("plxgo", "if 1==1 {} else " * 4000 + "{}"),   # deep else-if chain
     ("plxgo", "a" + ", a" * 3000 + " := 1" + ", 1" * 3000),  # many-target assign
     ("plxgo", "x := " + "(" * 8000),
     ("plxgo", "fmt." + "\n" * 200),                # dot as final token
     ("plxgo", "return \"" + "a" * 200000),         # unterminated string
     ("plxgo", "return `" + "a" * 200000),          # unterminated raw string
     ("plxgo", "switch {\n" + "case 1==1:\n\tx := 1\n" * 3000 + "}"),
     ("plxgo", "s := []int{" + "1, " * 50000 + "2}"),
     ("plxgo", "for i := 0; i < 3; " + "i++" * 3000 + " {}"),
     # nested string + must type in linear time (PR 24 review [0])
     ("plxgo", "return len(" + '("x" + ' * 40000 + '"a"' + ")" * 40000 + ")"),
     ("plxgo", "x := 1 + " + "- " * 200000 + "1\nreturn x"),
     ("plxgo", "return " + 'strings.ReplaceAll(x, "a", "a" + ' * 40 + "x" +
      ")" * 40),
     # plxlua
     ("plxlua", "if x then\n" * 4000 + "y = 1\n" + "end\n" * 4000),
     ("plxlua", "repeat\n" * 3000 + "until x\n" * 3000),
     ("plxlua", "while x do\n" * 3000 + "end\n" * 3000),
     ("plxlua", "return " + "(" * 8000 + "1" + ")" * 8000),
     ("plxlua", "return " + "{" * 8000 + "1" + "}" * 8000),
     ("plxlua", "return " + "not " * 8000 + "x"),
     ("plxlua", "local s = 'a'" + " .. 'b'" * 50000 + " return s"),
     ("plxlua", "return x" + " and y" * 50000),
     ("plxlua", "return 1" + " == 1" * 50000),
     ("plxlua", "local n = 1 return " + " or ".join("n == %d and %d" % (i, i) for i in range(20000))),
     ("plxlua", "return r" + ".f" * 50000),
     ("plxlua", "return [==[" + "a" * 200000),          # unterminated long string
     ("plxlua", "--[[" + "a" * 200000),                  # unterminated long comment
     ("plxlua", "return [" + "=" * 100000 + "[" + "]" + "=" * 99999 + "]" * 2000),
     ("plxlua", 'return "' + "a" * 200000),             # unterminated string
     ("plxlua", 'return "' + "\\z" * 50000 + '"'),
     ("plxlua", "local ok = pcall(function()\n" * 600 + "x = 1\n" + "end)\n" * 600),
     ("plxlua", "local ok = pcall(" * 5000),
     ("plxlua", "for i = 1, 2 do\n" * 600 + "goto continue\n" + "::continue::\nend\n" * 600),
     ("plxlua", "".join("local x = %s\n" % ("1" if i % 2 else "'a'") for i in range(5000)) + "return x"),
     ("plxlua", "local x = 1\n" + "do local x = x + 1\n" * 450 + "end\n" * 450 + "return x"),
     ("plxlua", "local a" + ", a" * 70 + " = 1"),
     ("plxlua", "x = 1 " + "x = x + 1 " * 50000 + "return x"),
     ("plxlua", "end " * 50000),
     ("plxlua", "::" * 50000),
     ("plxlua", "return string.format('" + "%5d" * 3000 + "'" + ", 1" * 3000 + ")"),
     ("plxlua", "local " + "a" * 100000 + " = 1"),
     # plxcsharp
     ("plxcsharp", "if (x) {" * 4000 + "}" * 4000),
     ("plxcsharp", "return " + "(" * 8000 + "1"),
     ("plxcsharp", 'return $"' + "{" * 3000 + "x" + "}" * 3000 + '";'),
     ("plxcsharp", 'return ' + '$"{' * 3000 + "1" + '}"' * 3000 + ";"),
     ("plxcsharp", 'return $$' + '"' * 3 + "{{" * 2000 + "x" + "}}" * 2000 + '"' * 3 + ';'),
     ("plxcsharp", 'return ' + '"' * 3 + '\n' + "    line\n" * 20000),   # no closing line
     ("plxcsharp", 'return @"' + "a" * 200000),                     # unterminated verbatim
     ("plxcsharp", 'return "' + "\\u12" * 50000),                   # bad escapes
     ("plxcsharp", "/*" + "a" * 200000),
     ("plxcsharp", "return " + "a ? b : " * 4000 + "c;"),
     ("plxcsharp", "return " + "x ?? " * 4000 + "y;"),
     ("plxcsharp", "return " + "!" * 8000 + "x;"),
     ("plxcsharp", "return " + "- " * 8000 + "x;"),
     ("plxcsharp", "return (" + "1 + " * 50000 + "1);"),
     ("plxcsharp", "return " + "a == " * 20000 + "b;"),
     ("plxcsharp", 'return "a"' + ".PadLeft(1)" * 60 + ";"),
     ("plxcsharp", "return s" + ".Trim()" * 5000 + ";"),
     ("plxcsharp", "return a" + "[0]" * 5000 + ";"),
     ("plxcsharp", "int x = 0;" + " x++;" * 20000 + " return x;"),
     ("plxcsharp", "for (int i = 0; i < n; i++) {" * 3000 + "}" * 3000),
     ("plxcsharp", "switch (x) {" + " case 1: x = 2; break;" * 5000 + " }"),
     ("plxcsharp", "try { x = 1; }" + " catch (Exception e) { x = 2; }" * 3000),
     ("plxcsharp", "int[] a = {" + "1, " * 50000 + "2}; return a[0];"),
     ("plxcsharp", "execute(" + "x, " * 40 + "y);"),
     ("plxcsharp", "foreach (var r in query(" + "x, " * 40 + "y)) { }"),
     ("plxcsharp", "return string.Format(\"" + "{0}" * 3000 + "\", 1);"),
     ("plxcsharp", "else " * 5000),
     ("plxcsharp", "$" * 5000),
     ("plxcsharp", "@" * 5000),
     ("plxcsharp", '"' * 5001),
     ("plxcsharp", "'" * 5001),
     # crash regressions (all must error cleanly, never crash the backend)
     ("plxjs", "return call(`p`, " + ", ".join(str(i) for i in range(50)) + ");"),
     ("plxphp", "execute($s, " + ", ".join("$b" for _ in range(40)) + ");"),
     ("plxruby", 'query("s", ' + ", ".join("b" for _ in range(40)) +
      ").each do |r|\n x = r.a\nend\nreturn x"),
     ("plxpython3", "\n".join(" " * i + "if x:" for i in range(4000)) +
      "\n" + " " * 4000 + "x = 1"),
     ("plxcobol", "WORKING-STORAGE SECTION.\n01 WS-X USAGE"),
     ("plxcobol", "WORKING-STORAGE SECTION.\n01 WS-X USAGE IS"),
    ]

def quote(body):
    return "'" + body.replace("'", "''") + "'"

def server_up():
    try:
        p = subprocess.run([PSQL, "-U", "postgres", "-X", "-tAc", "SELECT 1"],
                           env=ENV, capture_output=True, text=True, timeout=10)
        return p.stdout.strip() == "1"
    except Exception:
        return False

# psql prefixes a server error with "psql:<file>:<line>: " when it reads a
# script, and prints it bare when it reads standard input. Match either, and
# only at the start of a line, so a fuzz body echoed inside an error message
# cannot fake a finding.
_ERR = r"^(?:psql:[^\n]*: )?ERROR:  "
# Nothing else cancels the fuzzer's statements, so either cancel message means
# the statement ran into statement_timeout. PostgreSQL reports a timeout that
# fires very early as "user request".
_TIMEOUT = re.compile(_ERR + r"canceling statement due to "
                      r"(?:statement timeout|user request)$", re.M)
_OOM = re.compile(_ERR + r"out of memory$", re.M)
_NOLANG = re.compile(_ERR + r'language "[^"\n]*" does not exist$', re.M)

def classify(out):
    """Classify what psql printed for one CREATE FUNCTION.

    CRASH: the backend died. HANG: the server cancelled the statement at
    statement_timeout, so the transpiler ran for too long. RESOURCE: the
    transpiler ran out of memory. SETUP: the language does not exist, so
    nothing was transpiled. Any other output, including an ordinary error,
    is "ok".
    """
    if "closed the connection" in out or "terminating connection" in out or \
       "server process" in out or "Perhaps out of memory" in out:
        return "CRASH"
    if _TIMEOUT.search(out):
        return "HANG"
    if _OOM.search(out):
        return "RESOURCE"
    if _NOLANG.search(out):
        return "SETUP"
    return "ok"

SELFTEST = [
    # (psql output, expected classification)
    ("CREATE FUNCTION\n", "ok"),
    ("ERROR:  plxruby: unexpected token\nDETAIL:  at plxruby source line 1\n", "ok"),
    ("ERROR:  stack depth limit exceeded\n", "ok"),
    ("server closed the connection unexpectedly\n", "CRASH"),
    # The server cancelled the transpile at statement_timeout: a hang.
    ("ERROR:  canceling statement due to statement timeout\n", "HANG"),
    ("ERROR:  out of memory\nDETAIL:  Failed on request of size 8192.\n", "RESOURCE"),
    ("psql:<stdin>:1: ERROR:  canceling statement due to statement timeout\n", "HANG"),
    ("ERROR:  canceling statement due to user request\n", "HANG"),
    # A transpile error that merely quotes the text is not a finding: not
    # when the quote ends the message (the $ anchor), and not when it is
    # somewhere other than the start of a line (the ^ anchor).
    ("ERROR:  plxruby: unexpected 'ERROR:  out of memory'\n", "ok"),
    ("ERROR:  plxruby: unexpected ERROR:  out of memory\n", "ok"),
    ("ERROR:  plxjs: bad x ERROR:  canceling statement due to statement "
     "timeout\n", "ok"),
    ('ERROR:  plxphp: near ERROR:  language "plxruby" does not exist\n', "ok"),
    # Nothing was transpiled at all: the run tests nothing.
    ('ERROR:  language "plxruby" does not exist\n', "SETUP"),
]

def selftest():
    """Check classify() on known outputs. Returns a list of failures."""
    failures = []
    for out, want in SELFTEST:
        got = classify(out)
        if got != want:
            failures.append("classify(%r) gave %s, want %s" % (out, got, want))
    return failures

def run_one(dialect, body):
    sql = ("SET statement_timeout='4s'; DROP FUNCTION IF EXISTS fz(); "
           "CREATE FUNCTION fz() RETURNS text LANGUAGE %s AS %s;" % (dialect, quote(body)))
    try:
        p = subprocess.run([PSQL, "-U", "postgres", "-X", "-q", "-v", "ON_ERROR_STOP=0"],
                           env=ENV, input=sql, capture_output=True, text=True, timeout=12)
    except subprocess.TimeoutExpired:
        return "HANG"
    return classify(p.stdout + p.stderr)

def check(dialect, body, cases):
    r = run_one(dialect, body)
    if r != "ok":
        cases.append((r, dialect, body[:80]))
        if r == "CRASH":
            for _ in range(30):
                if server_up():
                    break
                import time; time.sleep(1)

def languages_present():
    """Return the plx dialects missing from the target database."""
    p = subprocess.run([PSQL, "-U", "postgres", "-X", "-tA", "-c",
                        "SELECT lanname FROM pg_language"],
                       env=ENV, capture_output=True, text=True, timeout=30)
    if p.returncode != 0:
        sys.exit("fuzz.py: cannot query the server:\n" + p.stderr)
    have = set(p.stdout.split())
    return [d for d in DIALECTS if d not in have]

def main():
    if sys.argv[1:2] in (["-h"], ["--help"]):
        print(__doc__.strip())
        sys.exit(0)
    failures = selftest()
    for f in failures:
        print("self-test FAILED: " + f)
    if failures or sys.argv[1:] == ["--selftest"]:
        sys.exit(1 if failures else 0)
    missing = languages_present()
    if missing:
        sys.exit("fuzz.py: these languages do not exist in the target database, "
                 "so the fuzzer would test nothing: %s\n"
                 "Run CREATE EXTENSION plx there, or set PGDATABASE."
                 % " ".join(missing))
    iters = int(sys.argv[1]) if len(sys.argv) > 1 else 2500
    bad = []
    print("pathological cases (%d)..." % len(pathological()))
    for d, body in pathological():
        check(d, body, bad)
    print("mutation fuzz (%d)..." % iters)
    for k in range(iters):
        d = random.choice(DIALECTS)
        body = mutate(random.choice(SEEDS[d]))
        check(d, body, bad)
        if k % 500 == 499:
            print("  %d/%d, up=%s, findings=%d" % (k + 1, iters, server_up(), len(bad)))
    print("\n=== fuzz complete. server up: %s ===" % server_up())
    if not bad:
        print("no crashes, hangs, or resource findings.")
        sys.exit(0)
    print("%d finding(s):" % len(bad))
    for kind, d, snippet in bad[:40]:
        print("  [%s] %s : %r" % (kind, d, snippet))
    sys.exit(1)

if __name__ == "__main__":
    main()
