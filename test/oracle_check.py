#!/usr/bin/env python3
"""Compare plxlua and plxcsharp with real Lua and C#.

Each case in test/oracle_cases.json is a function body written in the
dialect, with its parameters, argument values and result type. The runner
creates it as a plx function, calls it with the arguments, and compares the
result with the case's "expected" value. "expected" is what the source
language prints for the same program: `--refresh` runs every case through
lua5.4 or dotnet and records that output, so expectations always come from
the real language, never from plx.

A case whose result differs on purpose (a documented divergence) sets
"diverges" to the reason and the plx result it gives in "plx". The runner
then requires that plx result, so the case fails both when plx changes and
when the documented difference goes away.

Usage: make oraclecheck                      (against contrib_regression)
       python3 test/oracle_check.py
       python3 test/oracle_check.py --refresh  (needs lua5.4 and dotnet)

The usual libpq variables choose the server and database. PLX_PSQL names
the psql binary; the default is the first psql on PATH. Run as root, the
runner connects as the postgres account. It creates and drops its own
functions in schema plx_oracle.
"""
import json, os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = os.path.join(HERE, "oracle_cases.json")
PSQL = os.environ.get("PLX_PSQL") or shutil.which("psql") or "psql"


def psql(sql):
    argv = [PSQL, "-X", "-q", "-A", "-t", "-v", "ON_ERROR_STOP=0"]
    if os.geteuid() == 0:
        argv = ["runuser", "-u", "postgres", "--"] + argv + ["-U", "postgres"]
    p = subprocess.run(argv, input=sql, capture_output=True, text=True)
    return p.stdout, p.stderr


def sql_lit(v):
    if v is None:
        return "NULL"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    return "'" + str(v).replace("'", "''") + "'"


def lua_lit(v):
    if v is None:
        return "nil"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    return '"' + str(v).replace("\\", "\\\\").replace('"', '\\"') + '"'


def cs_lit(v, cstype):
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "true" if v else "false"
    if cstype in ("decimal",):
        return repr(v) + "m"
    if cstype in ("long",):
        return repr(v) + "L"
    if isinstance(v, (int, float)):
        return repr(v)
    return '"' + str(v).replace("\\", "\\\\").replace('"', '\\"') + '"'


def run_plx(case, n):
    fn = "plx_oracle.o%d" % n
    params = ", ".join("%s %s" % (p["name"], p["sql"]) for p in case["params"])
    args = ", ".join(sql_lit(p["value"]) for p in case["params"])
    sql = ("SET client_min_messages = warning;\n"
           "CREATE FUNCTION %s(%s) RETURNS %s LANGUAGE %s AS $plx$\n%s\n$plx$;\n"
           "SELECT coalesce((%s(%s))::text, '<NULL>');\n"
           % (fn, params, case["returns"]["sql"], case["dialect"], case["body"], fn, args))
    out, err = psql(sql)
    errs = [l for l in err.splitlines() if l.startswith("ERROR")]
    if errs:
        return "ERROR: " + errs[0].split("ERROR:", 1)[1].strip()
    return out.strip()


def run_lua(case):
    names = ", ".join(p["name"] for p in case["params"])
    args = ", ".join(lua_lit(p["value"]) for p in case["params"])
    src = ("local function f(%s)\n%s\nend\n"
           "local ok, r = pcall(f%s)\n"
           "if not ok then io.write('ERROR') elseif r == nil then io.write('<NULL>')\n"
           "elseif math.type(r) == 'float' and r == math.floor(r) and r > -1e15 and r < 1e15 "
           "then io.write(string.format('%%d', r)) else io.write(tostring(r)) end\n"
           % (names, case["body"], (", " + args) if args else ""))
    with tempfile.NamedTemporaryFile("w", suffix=".lua", delete=False) as f:
        f.write(src)
    try:
        p = subprocess.run(["lua5.4", f.name], capture_output=True, text=True, timeout=30)
    finally:
        os.unlink(f.name)
    if p.returncode != 0:
        sys.exit("lua5.4 failed on %s:\n%s" % (case["id"], p.stderr))
    return p.stdout.strip()


def run_cs(cases):
    """Run every C# case in one program, so dotnet builds once."""
    parts = ["using System; using System.Text; using System.Globalization;",
             "CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;",
             "CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;"]
    for i, c in cases:
        params = ", ".join("%s %s" % (p["cs"], p["name"]) for p in c["params"])
        args = ", ".join(cs_lit(p["value"], p["cs"]) for p in c["params"])
        parts.append("%s F%d(%s) {\n%s\n}" % (c["returns"]["cs"], i, params, c["body"]))
        parts.append('try { var r%d = F%d(%s); Console.WriteLine("@@%d " + '
                     '(r%d == null ? "<NULL>" : Convert.ToString(r%d, CultureInfo.InvariantCulture)'
                     '.Replace("True", "true").Replace("False", "false"))); } '
                     'catch (Exception) { Console.WriteLine("@@%d ERROR"); }'
                     % (i, i, args, i, i, i, i))
    d = tempfile.mkdtemp()
    with open(os.path.join(d, "o.cs"), "w") as f:
        f.write("\n".join(parts) + "\n")
    p = subprocess.run(["/root/bin/csrun", os.path.join(d, "o.cs")],
                       capture_output=True, text=True, timeout=600)
    shutil.rmtree(d)
    res = {}
    for line in p.stdout.splitlines():
        if line.startswith("@@"):
            k, _, v = line[2:].partition(" ")
            res[int(k)] = v
    if not res:
        sys.exit("dotnet run failed:\n" + p.stdout[-3000:] + p.stderr[-2000:])
    return res


def main():
    data = json.load(open(CASES))
    cases = data["cases"]
    if "--refresh" in sys.argv:
        cs = run_cs([(i, c) for i, c in enumerate(cases) if c["dialect"] == "plxcsharp"])
        for i, c in enumerate(cases):
            c["expected"] = run_lua(c) if c["dialect"] == "plxlua" else cs.get(i, "MISSING")
        with open(CASES, "w") as f:
            json.dump(data, f, indent=1, ensure_ascii=False)
            f.write("\n")
        print("refreshed %d expectations" % len(cases))
        return 0
    out, err = psql("DROP SCHEMA IF EXISTS plx_oracle CASCADE; CREATE SCHEMA plx_oracle;")
    if "ERROR" in err:
        sys.exit(err)
    bad = 0
    for i, c in enumerate(cases):
        got = run_plx(c, i)
        want = c["plx"] if c.get("diverges") else c["expected"]
        # an error in the source language matches any error in plx
        ok = got == want or (want == "ERROR" and got.startswith("ERROR"))
        if c.get("diverges") and c["plx"] == c["expected"]:
            ok = False      # a documented divergence that is not one
        print("%s %-8s %-28s %s" % ("ok  " if ok else "FAIL", c["dialect"][3:], c["id"],
                                    "" if ok else "got %r, want %r (source language: %r)"
                                    % (got, want, c["expected"])))
        bad += not ok
    psql("DROP SCHEMA IF EXISTS plx_oracle CASCADE;")
    print("PASS %d/%d  FAIL %d" % (len(cases) - bad, len(cases), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
