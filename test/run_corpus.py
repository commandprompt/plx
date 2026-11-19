#!/usr/bin/env python3
"""plx corpus test runner. Each case runs in its own fresh psql session
(so TEMP tables don't collide), output normalized and compared to expected.
A case that should succeed must produce the expected result rows in the same
order, with the same values and row counts, and the same NOTICE lines in the
same order. Only alignment padding and blank lines are ignored.
Reject-* categories PASS when transpile/exec raises an ERROR.
Run as: runuser -u postgres -- python3 <thisdir>/run_corpus.py
"""
import json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
PSQL = "/usr/local/pgsql/bin/psql"
cases = json.load(open(os.path.join(HERE, "corpus.json")))["cases"]

def psql(sql):
    p = subprocess.run(["runuser", "-u", "postgres", "--", PSQL, "-U", "postgres",
                        "-X", "-q", "-v", "ON_ERROR_STOP=0"],
                       input=sql, capture_output=True, text=True)
    return p.stdout + p.stderr

# fresh extension
psql("DROP EXTENSION IF EXISTS plx CASCADE; CREATE EXTENSION plx;")

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
    """normalize() must keep order, pairing and messages. Returns failures."""
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
    return failures

_selftest_failures = selftest()
if _selftest_failures:
    sys.exit("run_corpus.py self-test failed:\n  " +
             "\n  ".join(_selftest_failures))

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

npass = nfail = 0
fails = []
for c in cases:
    reject = c["category"].startswith("reject")
    exp_err = "ERROR:" in c["expected_output"]
    # Keep the runner's own setup quiet, so only the case's messages count.
    sql = ("SET client_min_messages = warning;\n"
           "CREATE EXTENSION IF NOT EXISTS plx;\n"
           "RESET client_min_messages;\n" + c["ruby_create"] + "\n")
    sql += c["test_sql"] + "\n"
    out = psql(sql)
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
        npass += 1
    else:
        nfail += 1
        detail = out.strip().splitlines()[-3:] if out.strip() else []
        if not (reject or exp_err) and not had_error:
            detail = ["expected rows: %s" % " / ".join(want[0]),
                      "got rows     : %s" % " / ".join(got[0])]
            if want[1] != got[1]:
                detail += ["expected messages: %s" % " / ".join(want[1]),
                           "got messages     : %s" % " / ".join(got[1])]
        fails.append((c["id"], c["category"], detail))

print(f"PASS {npass}/{len(cases)}  FAIL {nfail}")
for fid, cat, tail in fails:
    print(f"  FAIL [{cat}] {fid}")
    for t in tail:
        print(f"        {t}")
sys.exit(1 if nfail else 0)
