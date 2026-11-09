#!/usr/bin/env python3
"""Differential check of try/except/finally control flow.

Generates random Python programs that mix nested try/except/finally, for
loops, return/break/continue (also inside finally) and raised errors, runs
each in real Python and as a plxpython3 function, and requires the results to
agree. `acc` folds in every executed statement, so a result encodes the order
of execution, not just the last value.

With a third argument of plxcsharp the same kind of program is generated as
C# as well (Python stays the reference), using counting and general for
loops, do/while and switch, with no return, break or continue leaving a
finally block (C# forbids that).

Needs the extension installed and a running server, like installcheck:
  PGDATABASE=contrib_regression python3 test/finally_diff.py [cases] [seed] [dialect]
"""
import os, random, subprocess, sys, warnings

warnings.simplefilter("ignore")		# Python warns about return in finally
random.seed(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
N = int(sys.argv[1]) if len(sys.argv) > 1 else 200
DIALECT = sys.argv[3] if len(sys.argv) > 3 else "plxpython3"
K = [0]
def k():
    K[0] += 1; return K[0] % 7 + 1
def block(d, inloop, ind):
    out = []
    for _ in range(random.randint(1, 3)):
        out += stmt(d, inloop, ind)
    return out
def stmt(d, inloop, ind):
    p = " " * ind
    r = random.random()
    if d >= 4 or r < 0.30:
        return [f"{p}acc = (acc * 3 + {k()}) % 1000003"]
    if r < 0.40:
        return [f"{p}if acc % 2 == 0:"] + [f"{p}    return acc"]
    if r < 0.48 and inloop:
        return [f"{p}if acc % 3 == 0:", f"{p}    break"]
    if r < 0.56 and inloop:
        return [f"{p}if acc % 5 == 1:", f"{p}    continue"]
    if r < 0.62:
        return [f"{p}if acc % 4 == 1:", f"{p}    raise ValueError(\"x\")"]
    if r < 0.75:
        v = f"i{d}"
        return [f"{p}for {v} in range(0, 3):"] + block(d + 1, True, ind + 4)
    # try
    lines = [f"{p}try:"] + block(d + 1, inloop, ind + 4)
    form = random.choice(["exc", "fin", "both"])
    if form in ("exc", "both"):
        lines += [f"{p}except Exception as e:"] + block(d + 1, inloop, ind + 4)
    if form in ("fin", "both"):
        lines += [f"{p}finally:"] + block(d + 1, inloop, ind + 4)
    return lines
def prog():
    return ["acc = 1"] + block(0, False, 0) + ["return acc"]

# C#: each generator returns (python lines, C# lines) for the same program
def cs_block(d, inloop, infin, ind):
    py, cs = [], []
    for _ in range(random.randint(1, 3)):
        a, b = cs_stmt(d, inloop, infin, ind)
        py += a
        cs += b
    return py, cs
def cs_stmt(d, inloop, infin, ind):
    p = " " * ind
    r = random.random()
    if d >= 4 or r < 0.28:
        v = k()
        return ([f"{p}acc = (acc * 3 + {v}) % 1000003"],
                [f"{p}acc = (acc * 3 + {v}) % 1000003;"])
    if r < 0.38 and not infin:
        return ([f"{p}if acc % 2 == 0:", f"{p}    return acc"],
                [f"{p}if (acc % 2 == 0) return acc;"])
    if r < 0.46 and inloop is True:
        return ([f"{p}if acc % 3 == 0:", f"{p}    break"], [f"{p}if (acc % 3 == 0) break;"])
    if r < 0.54 and inloop:
        return ([f"{p}if acc % 5 == 1:", f"{p}    continue"],
                [f"{p}if (acc % 5 == 1) continue;"])
    if r < 0.60:
        return ([f"{p}if acc % 4 == 1:", f"{p}    raise ValueError(\"x\")"],
                [f"{p}if (acc % 4 == 1) throw new Exception(\"x\");"])
    if r < 0.72:
        v = f"i{d}"
        bpy, bcs = cs_block(d + 1, True, infin, ind + 4)
        py = [f"{p}for {v} in range(0, 3):"] + bpy
        form = random.choice(["for", "gfor", "do"])
        if form == "for":
            cs = [f"{p}for (int {v} = 0; {v} < 3; {v}++)", f"{p}{{"] + bcs + [f"{p}}}"]
        elif form == "gfor":
            cs = ([f"{p}for (int {v} = 0, w{d} = 0; {v} < 3; {v}++, w{d}++)", f"{p}{{"] +
                  bcs + [f"{p}}}"])
        else:
            cs = ([f"{p}int {v} = 0;", f"{p}do", f"{p}{{", f"{p}    {v}++;"] + bcs +
                  [f"{p}}} while ({v} < 3);"])
        return py, cs
    if r < 0.80:
        # switch: each section ends in break; a nested break would leave the
        # switch, so none is generated inside it
        t = f"t{d}"
        py = [f"{p}{t} = acc % 3"]
        cs = [f"{p}switch (acc % 3)", f"{p}{{"]
        for n, lab in enumerate(["case 0:", "case 1:", "default:"]):
            bpy, bcs = cs_block(d + 1, "nobreak" if inloop else False, infin, ind + 4)
            py += [f"{p}{['if', 'elif', 'else'][n]}" + ("" if n == 2 else f" {t} == {n}") + ":"] + bpy
            cs += [f"{p}{lab}"] + bcs + [f"{p}    break;"]
        cs += [f"{p}}}"]
        return py, cs
    lpy, lcs = cs_block(d + 1, inloop, infin, ind + 4)
    py = [f"{p}try:"] + lpy
    cs = [f"{p}try", f"{p}{{"] + lcs + [f"{p}}}"]
    form = random.choice(["exc", "fin", "both"])
    if form in ("exc", "both"):
        epy, ecs = cs_block(d + 1, inloop, infin, ind + 4)
        py += [f"{p}except Exception as e:"] + epy
        cs += [f"{p}catch (Exception e)", f"{p}{{"] + ecs + [f"{p}}}"]
    if form in ("fin", "both"):
        fpy, fcs = cs_block(d + 1, False, True, ind + 4)
        py += [f"{p}finally:"] + fpy
        cs += [f"{p}finally", f"{p}{{"] + fcs + [f"{p}}}"]
    return py, cs
def cs_prog():
    py, cs = cs_block(0, False, False, 0)
    return ["acc = 1"] + py + ["return acc"], ["long acc = 1;"] + cs + ["return acc;"]
def run_py(body):
    src = "def f():\n" + "\n".join("    " + l for l in body) + "\n"
    ns = {}
    exec(src, ns)
    try:
        return str(ns["f"]())
    except ValueError as e:
        # plx raises it as P0001 with the same message
        return f"ERROR P0001:{e}"
cases = []
for n in range(N):
    if DIALECT == "plxcsharp":
        body, src = cs_prog()
        cases.append((n, src, run_py(body)))
    else:
        body = prog()
        cases.append((n, body, run_py(body)))
sql = ["SET client_min_messages = warning;"]
for n, body, _ in cases:
    b = "\n".join(body)
    if DIALECT == "plxcsharp":
        sql.append(f"CREATE OR REPLACE FUNCTION fd{n}() RETURNS bigint LANGUAGE plxcsharp AS $plx$\n{b}\n$plx$;")
    else:
        sql.append(f"CREATE OR REPLACE FUNCTION fd{n}() RETURNS bigint LANGUAGE plxpython3 AS $plx$\nacc #:: bigint\n{b}\n$plx$;")
    sql.append(f"DO $d$ BEGIN RAISE WARNING 'R{n}=%', fd{n}(); EXCEPTION WHEN OTHERS THEN RAISE WARNING 'R{n}=ERROR %:%', SQLSTATE, SQLERRM; END $d$;")
r = subprocess.run([os.environ.get("PLX_PSQL", "psql"), "-q", "-X"], input="\n".join(sql),
                   capture_output=True, text=True)
got = {}
for line in r.stderr.splitlines():
    if "R" in line and "=" in line and "WARNING" in line:
        tag = line.split("WARNING:")[1].strip()
        key, val = tag.split("=", 1)
        got[int(key[1:])] = val
other = [l for l in r.stderr.splitlines() if "WARNING" not in l]
bad = 0
for n, body, exp in cases:
    g = got.get(n, "MISSING")
    if g != exp:
        bad += 1
        if bad <= 3:
            print(f"--- case {n}: python={exp} plx={g}\n" + "\n".join(body))
print(f"{N} cases, {bad} mismatches; other stderr lines: {len(other)}")
for l in other[:5]:
    print("  ", l)
sys.exit(1 if bad or other else 0)
