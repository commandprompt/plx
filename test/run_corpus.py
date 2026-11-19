#!/usr/bin/env python3
"""plx corpus test runner. Each case runs in its own fresh psql session
(so TEMP tables don't collide), output normalized and compared to expected.
A case that should succeed must produce the expected result rows in the same
order, with the same values and row counts, and the same NOTICE lines in the
same order. Only alignment padding and blank lines are ignored.
Reject-* categories PASS when transpile/exec raises an ERROR.

Usage: python3 test/run_corpus.py
       python3 test/run_corpus.py --selftest   (check the comparison only, no server)

A self-test of the comparison runs first, before the runner touches the
server. The runner then drops and re-creates the plx extension in the target
database, so point it at a test database. The usual libpq variables (PGHOST,
PGPORT, PGDATABASE) choose the server and database. PLX_PSQL names the psql
binary; the default is the first psql on PATH. Run as root, the runner
connects as the postgres account; otherwise as the current user, or as
PLX_PGUSER when it is set.
"""
import json, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
PSQL = os.environ.get("PLX_PSQL") or shutil.which("psql") or "psql"

def psql(sql):
    argv = [PSQL, "-X", "-q", "-v", "ON_ERROR_STOP=0"]
    user = os.environ.get("PLX_PGUSER")
    if os.geteuid() == 0:
        argv = ["runuser", "-u", "postgres", "--"] + argv + ["-U", user or "postgres"]
    elif user:
        argv += ["-U", user]
    p = subprocess.run(argv, input=sql, capture_output=True, text=True)
    return p.stdout + p.stderr

CHROME = {"CREATE", "EXTENSION", "FUNCTION", "DROP", "TABLE", "TEMP", "INSERT",
          "SET", "PL/pgSQL", "CONTEXT:", "line", "at"}

MESSAGE = ("NOTICE:", "WARNING:", "INFO:")
ERRORISH = ("ERROR:", "CONTEXT:", "DETAIL:", "HINT:", "LINE ", "QUERY:")

def normalize(s):
    """Split psql output into (rows, messages), each an ordered list.

    rows holds every result line (headers, data rows and row counts) with the
    alignment padding removed, so "  1 |    20" becomes "1|20". Blank lines and
    the dashed separator lines are dropped. messages holds the NOTICE,
    WARNING and INFO lines. psql writes those to stderr and the results to
    stdout, so their order relative to the rows is not comparable, but the
    order within each list is. Error lines are left out; the caller checks
    for an error separately.
    """
    rows, messages = [], []
    for ln in s.splitlines():
        ln = re.sub(r"^psql:[^:]*:\d+: ", "", ln.strip())
        if ln.startswith(MESSAGE):
            messages.append(" ".join(ln.split()))
        elif ln.startswith(ERRORISH):
            continue
        elif not ln or set(ln) <= set("-+"):
            continue
        else:
            rows.append(re.sub(r"\s*\|\s*", "|", ln))
    return rows, messages

def selftest():
    """normalize() must keep order, pairing and messages, and judge() must
    use it. Returns failures."""
    t = " a | b | c \n---+---+---\n A | B | F\n(1 row)"
    pairs = [
        # (expected, got, should they compare equal?)
        (t, "  a|b|c\n-+-+-\n  A |  B | F \n(1 row)\n\n", True),
        (t, " a | b | c \n---+---+---\n F | B | A\n(1 row)", False),
        (" k | v \n---+---\n 1 | 20\n 2 | 60\n(2 rows)",
         " k | v \n---+---\n 1 | 60\n 2 | 20\n(2 rows)", False),
        (" x \n---\n f\n(1 row)\n\n x \n---\n t\n(1 row)",
         " x \n---\n t\n(1 row)\n\n x \n---\n f\n(1 row)", False),
        ("NOTICE:  a\nNOTICE:  b\n x \n---\n 1\n(1 row)",
         " x \n---\n 1\n(1 row)\nNOTICE:  a\nNOTICE:  b\n", True),
        ("NOTICE:  a\nNOTICE:  b\n x \n---\n 1\n(1 row)",
         " x \n---\n 1\n(1 row)\nNOTICE:  b\nNOTICE:  a\n", False),
        (" x \n---\n 1\n(1 row)", " x \n---\n 1\n 1\n(2 rows)", False),
    ]
    failures = []
    for want, got, same in pairs:
        if (normalize(want) == normalize(got)) != same:
            failures.append("normalize: %r vs %r should %scompare equal"
                            % (want, got, "" if same else "not "))
    # judge() decides PASS or FAIL for a whole case, so test it too: a case
    # that should succeed passes only on the same rows, in the same order.
    two = " x \n---\n f\n(1 row)\n\n x \n---\n t\n(1 row)"
    swapped = " x \n---\n t\n(1 row)\n\n x \n---\n f\n(1 row)"
    ok_case = {"category": "values", "expected_output": two}
    reject_case = {"category": "reject-x", "expected_output": "anything"}
    verdicts = [
        # (case, psql output, should it pass?)
        (ok_case, two + "\n", True),
        (ok_case, swapped + "\n", False),
        (ok_case, two + "\nERROR:  boom\n", False),
        (reject_case, "ERROR:  plxruby: not supported\n", True),
        (reject_case, " x \n---\n 1\n(1 row)\n", False),
    ]
    for case, out, want in verdicts:
        got = judge(case, out)[0]
        if got != want:
            failures.append("judge(%s case, %r) gave %s, want %s"
                            % (case["category"], out,
                               "PASS" if got else "FAIL",
                               "PASS" if want else "FAIL"))
    return failures

def datatokens(s):
    toks = set()
    for ln in s.splitlines():
        ln = ln.strip()
        if not ln or set(ln) <= set("-+| "):
            continue
        if re.match(r"^\(\d+ rows?\)$", ln):
            continue
        for t in re.split(r"[\s|]+", ln):
            t = t.strip().strip(",;\"")
            if t and t not in CHROME:
                toks.add(t)
    return toks

def judge(c, out):
    """Decide whether case c passed, given what psql printed for it.

    Returns (ok, detail), where detail is a list of lines explaining a FAIL.
    """
    reject = c["category"].startswith("reject")
    exp_err = "ERROR:" in c["expected_output"]
    had_error = "ERROR:" in out
    if reject or exp_err:
        # expected to raise: PASS if it errored (and a keyword from expected shows)
        kw = datatokens(c["expected_output"].split("ERROR:")[-1]) if exp_err else set()
        got = datatokens(out)
        ok = had_error and (not kw or len(kw & got) >= max(1, len(kw) // 2))
    else:
        # Compare in order: values, row pairing, row count and messages
        # must all match the expected output, and nothing may be extra.
        want = normalize(c["expected_output"])
        got = normalize(out)
        ok = (not had_error) and want == got
    if ok:
        return True, []
    detail = out.strip().splitlines()[-3:] if out.strip() else []
    if not (reject or exp_err) and not had_error:
        detail = ["expected rows: %s" % " / ".join(want[0]),
                  "got rows     : %s" % " / ".join(got[0])]
        if want[1] != got[1]:
            detail += ["expected messages: %s" % " / ".join(want[1]),
                       "got messages     : %s" % " / ".join(got[1])]
    return False, detail

def main():
    failures = selftest()
    if failures:
        sys.exit("run_corpus.py self-test failed:\n  " + "\n  ".join(failures))
    if sys.argv[1:] == ["--selftest"]:
        return 0
    cases = json.load(open(os.path.join(HERE, "corpus.json")))["cases"]
    # fresh extension
    psql("DROP EXTENSION IF EXISTS plx CASCADE; CREATE EXTENSION plx;")
    npass = nfail = 0
    fails = []
    for c in cases:
        # Keep the runner's own setup quiet, so only the case's messages count.
        sql = ("SET client_min_messages = warning;\n"
               "CREATE EXTENSION IF NOT EXISTS plx;\n"
               "RESET client_min_messages;\n" + c["ruby_create"] + "\n")
        sql += c["test_sql"] + "\n"
        ok, detail = judge(c, psql(sql))
        if ok:
            npass += 1
        else:
            nfail += 1
            fails.append((c["id"], c["category"], detail))

    print(f"PASS {npass}/{len(cases)}  FAIL {nfail}")
    for fid, cat, tail in fails:
        print(f"  FAIL [{cat}] {fid}")
        for t in tail:
            print(f"        {t}")
    return 1 if nfail else 0

if __name__ == "__main__":
    sys.exit(main())
