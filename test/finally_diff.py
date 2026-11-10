#!/usr/bin/env python3
"""Differential check of try/except/finally control flow.

Generates random Python programs that mix nested try/except/finally, for
loops, return/break/continue (also inside finally) and raised errors, runs
each in real Python and as a plxpython3 function, and requires the results to
agree. `acc` folds in every executed statement, so a result encodes the order
of execution, not just the last value.

Needs the extension installed and a running server, like installcheck:
  PGDATABASE=contrib_regression python3 test/finally_diff.py [cases] [seed]
"""
import os, random, subprocess, sys, warnings

warnings.simplefilter("ignore")		# Python warns about return in finally
random.seed(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
N = int(sys.argv[1]) if len(sys.argv) > 1 else 200
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
    body = prog()
    cases.append((n, body, run_py(body)))
sql = ["SET client_min_messages = warning;"]
for n, body, _ in cases:
    b = "\n".join(body)
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
