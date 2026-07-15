#!/usr/bin/env python3
"""plx transpiler fuzzer.

Feeds mutated and pathological function bodies to CREATE FUNCTION in each plx
dialect and watches for backend crashes (lost connection) or hangs (transpiler
infinite loop / non-interruptible). A clean run means every input either
transpiled or raised a normal error, and the server stayed up.

Run in the container: PGHOST=/tmp python3 test/fuzz.py [iterations]
"""
import os, random, subprocess, sys

PSQL = "/usr/local/pgsql/bin/psql"
ENV = dict(os.environ, PGHOST=os.environ.get("PGHOST", "/tmp"), PGUSER="postgres")
DIALECTS = ["plxruby", "plxphp", "plxjs", "plxpython3", "plxcobol"]
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
}
SPECIALS = [b'"', b"'", b"`", b"#{", b"${", b"{$", b"*/", b"/*", b"\\", b"(", b")",
            b"{", b"}", b"[", b"]", b"end", b"..", b"...", b"do |", b"|", b"::",
            b"->", b"=>", b";", b"raise", b"query(", b"fetch_one(", b"\n",
            b'\n    ', b'\n\t', b':', b'    ', b'if ', b'for ', b'range(',
            b'f"', b'except ', b'pass', b'\n        ', b'elif ',
            b'END-IF', b'END-PERFORM', b'PERFORM ', b'PIC ', b'MOVE ',
            b'GOBACK', b'UNTIL', b'STRING-APPEND ', b'WS-', b'.', b'%', b',']

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

def run_one(dialect, body):
    sql = ("SET statement_timeout='4s'; DROP FUNCTION IF EXISTS fz(); "
           "CREATE FUNCTION fz() RETURNS text LANGUAGE %s AS %s;" % (dialect, quote(body)))
    try:
        p = subprocess.run([PSQL, "-U", "postgres", "-X", "-q", "-v", "ON_ERROR_STOP=0"],
                           env=ENV, input=sql, capture_output=True, text=True, timeout=12)
    except subprocess.TimeoutExpired:
        return "HANG"
    out = p.stdout + p.stderr
    if "closed the connection" in out or "terminating connection" in out or \
       "server process" in out or "Perhaps out of memory" in out:
        return "CRASH"
    return "ok"

def check(dialect, body, cases):
    r = run_one(dialect, body)
    if r != "ok":
        cases.append((r, dialect, body[:80]))
        if r == "CRASH":
            for _ in range(30):
                if server_up():
                    break
                import time; time.sleep(1)

def main():
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
        print("no crashes or hangs.")
        sys.exit(0)
    print("%d finding(s):" % len(bad))
    for kind, d, snippet in bad[:40]:
        print("  [%s] %s : %r" % (kind, d, snippet))
    sys.exit(1)

main()
