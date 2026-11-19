#!/usr/bin/env python3
"""plx differential test runner.

Every case is one small program written once as a plpgsql reference and once
per dialect. The reference is the plpgsql a PostgreSQL developer would write
for the same logic, so it acts as the oracle: plx claims a dialect body
transpiles to plpgsql that behaves the same way, and any disagreement between
a dialect variant and the reference is a transpiler defect (or a limitation
worth recording).

Each variant is called with the same argument lists. A call either produces a
value or raises, and both outcomes are compared: values by their text form,
encoded as JSON so that NULL, the empty string and an error can never be
confused with a returned value, errors by SQLSTATE, so that a dialect is
required to fail the same way the reference fails. A self-test of the
encoding runs before the cases.

A case may document a known divergence for some dialects and calls. Each such
entry records the exact outcome it documents ("outcome"), and only that
outcome is exempt: a dialect that diverges in any other way is still reported
as unexplained.

Run it with `make differentialcheck`, which points PLX_PSQL at the psql
belonging to PG_CONFIG. It needs the extension installed and a running server,
the same preconditions as installcheck.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from differential_cases import CASES, DIALECTS, SETUP  # noqa: E402

PSQL = os.environ.get("PLX_PSQL", "psql")
DB = os.environ.get("PLX_DB", "contrib_regression")


def psql(sql, stop_on_error=False):
    """Run SQL, returning (stdout, stderr, rc).

    Two environments have to work. In the development container this runs as
    root, which the server will not accept a connection from, so it drops to
    the postgres account. In CI it runs as an unprivileged user who already
    owns a superuser role, and dropping privileges would be wrong. PLX_PGUSER
    overrides the role either way.
    """
    argv = [PSQL, "-d", DB, "-X", "-q", "-A", "-t", "-v",
            "ON_ERROR_STOP=" + ("1" if stop_on_error else "0")]
    user = os.environ.get("PLX_PGUSER")
    if os.geteuid() == 0:
        argv = (["runuser", "-u", "postgres", "--"] + argv +
                ["-U", user or "postgres"])
    elif user:
        argv += ["-U", user]
    p = subprocess.run(argv, input=sql, capture_output=True, text=True)
    return p.stdout, p.stderr, p.returncode


PROBE = r"""
CREATE EXTENSION IF NOT EXISTS plx;
SET client_min_messages = warning;
CREATE OR REPLACE FUNCTION plxdiff_probe(q text) RETURNS text
LANGUAGE plpgsql AS $probe$
DECLARE r text;
BEGIN
  EXECUTE 'SELECT (' || q || ')::text' INTO r;
  /* One probe is one JSON value on one line: null for SQL NULL, a JSON
   * string for a value, {"error": sqlstate} for an error. JSON escapes
   * quotes, backslashes and control characters, so no value can look
   * like NULL or an error, and no value can span two lines. */
  IF r IS NULL THEN
    RETURN 'null';
  END IF;
  RETURN to_json(r)::text;
EXCEPTION WHEN OTHERS THEN
  RETURN json_build_object('error', SQLSTATE)::text;
END;
$probe$;
"""


# Probe encodings the self-test checks. Each pair below must probe to two
# different results: on the left a NULL, an error, an empty string or a
# newline, on the right a value whose text could be mistaken for it.
PROBE_PAIRS = [
    ("NULL::text", "'<NULL>'::text"),
    ("(1/0)::text", "'ERROR 22012'::text"),
    ("''::text", "'<EMPTY>'::text"),
    ("E'a\\nb'", "'a\\\\nb'"),
]
# Exact results the probe must give.
PROBE_EXACT = [
    ("NULL::text", 'null'),
    ("(1/0)::text", '{"error" : "22012"}'),
    ("''::text", '""'),
    ("'<NULL>'::text", '"<NULL>"'),
    ("E'a\\nb'", '"a\\nb"'),
    ("42", '"42"'),
]


def split_probe_output(out):
    """Split psql output into one string per probe.

    Do not use str.splitlines(): it also breaks at U+0085, U+2028 and U+2029,
    which to_json leaves unescaped. psql ends each row with a plain newline,
    and no JSON result is empty, so the rstrip cannot drop a real result.
    """
    return out.rstrip("\n").split("\n")


def probe_selftest():
    """Check the probe encoding against the server. Returns a list of failures.

    This uses the same psql() call and split_probe_output() as probe_all(),
    so it covers the real path.
    """
    failures = []
    exprs = []
    for a, b in PROBE_PAIRS:
        exprs += [a, b]
    exprs += [e for e, _ in PROBE_EXACT]
    out, err, _ = psql("".join("SELECT plxdiff_probe($plxq$%s$plxq$);\n" % e
                               for e in exprs))
    lines = split_probe_output(out)
    if len(lines) != len(exprs):
        return ["probe self-test: %d results for %d probes\n%s"
                % (len(lines), len(exprs), err)]
    got = dict(zip(exprs, lines))
    for a, b in PROBE_PAIRS:
        if got[a] == got[b]:
            failures.append("probe self-test: %s and %s both probe as %r"
                            % (a, b, got[a]))
    for e, want in PROBE_EXACT:
        if got[e] != want:
            failures.append("probe self-test: %s probes as %r, want %r"
                            % (e, got[e], want))
    # Pure Python, so it does not depend on the server encoding: only a
    # newline may end a probe, not U+2028 or U+0085.
    sample = '"a b"\n"c\x85d"\n"e"\n'
    want = ['"a b"', '"c\x85d"', '"e"']
    if split_probe_output(sample) != want:
        failures.append("probe self-test: split_probe_output(%r) gave %r, "
                        "want %r" % (sample, split_probe_output(sample), want))
    return failures


def fname(case, variant):
    return "plxdiff_%s_%s" % (variant, case["name"])


def create_variants():
    """Create every reference and dialect function. Returns {(case,variant): err}."""
    errors = {}
    for case in CASES:
        variants = [("ref", "plpgsql", case["reference"])]
        for dialect, body in case["bodies"].items():
            variants.append((DIALECTS[dialect], dialect, body))
        for tag, lang, body in variants:
            ddl = ("DROP FUNCTION IF EXISTS %s(%s);\n"
                   "CREATE FUNCTION %s(%s) RETURNS %s LANGUAGE %s AS $plxdiff$%s$plxdiff$;"
                   % (fname(case, tag), case["args"], fname(case, tag),
                      case["args"], case["returns"], lang, body))
            _, err, rc = psql(ddl, stop_on_error=True)
            if rc != 0:
                first = [ln for ln in err.splitlines() if ln.startswith("ERROR")]
                errors[(case["name"], tag)] = first[0] if first else err.strip()
    return errors


def probe_all(created_ok):
    """Call every created variant with every argument list. Returns nested dict."""
    selects = []
    keys = []
    for case in CASES:
        tags = ["ref"] + [DIALECTS[d] for d in case["bodies"]]
        for tag in tags:
            if (case["name"], tag) not in created_ok:
                continue
            for call in case["calls"]:
                expr = "%s(%s)" % (fname(case, tag), call)
                keys.append((case["name"], tag, call))
                selects.append("SELECT plxdiff_probe($plxq$%s$plxq$)" % expr)
    if not selects:
        return {}
    out, err, _ = psql("\n".join(s + ";" for s in selects))
    lines = split_probe_output(out)
    if len(lines) != len(keys):
        sys.stderr.write("probe count mismatch: %d results for %d probes\n%s\n"
                         % (len(lines), len(keys), err))
        sys.exit(2)
    results = {}
    for (name, tag, call), value in zip(keys, lines):
        results.setdefault((name, call), {})[tag] = value
    return results


def find_exemption(known, dialect, call):
    """Return the documented-divergence entry covering (dialect, call), or None."""
    for entry in known:
        if dialect in entry["dialects"] and call in entry["calls"]:
            return entry
    return None


def documented_outcome(entry, call):
    """The exact result a documented divergence records for one call.

    An entry's "outcome" is either one result for every call it lists, or a
    dict from call to result.
    """
    outcome = entry["outcome"]
    if isinstance(outcome, dict):
        return outcome[call]
    return outcome


def classify(got, expected, entry, call):
    """Classify one dialect result against the reference result.

    Returns "match", "stale", "documented" or "unexplained". A documented
    divergence exempts only the exact outcome it records: any other result
    that differs from the reference is unexplained.
    """
    agrees = got == expected
    if agrees and entry is None:
        return "match"
    if agrees:
        return "stale"
    if entry is not None and got == documented_outcome(entry, call):
        return "documented"
    return "unexplained"


def check_exemptions(cases):
    """Every documented divergence must record its outcome for every call."""
    problems = []
    for case in cases:
        for entry in case.get("documented", []):
            if "outcome" not in entry:
                problems.append("%s: documented entry for %s has no outcome"
                                % (case["name"], ", ".join(entry["dialects"])))
                continue
            if isinstance(entry["outcome"], dict):
                for call in entry["calls"]:
                    if call not in entry["outcome"]:
                        problems.append("%s: documented entry for %s has no "
                                        "outcome for call (%s)"
                                        % (case["name"],
                                           ", ".join(entry["dialects"]), call))
    return problems


def selftest():
    """Check the comparison logic on made-up results. Returns a list of failures."""
    failures = []
    entry = {"dialects": ["plxruby"], "calls": ["1"], "reason": "r",
             "outcome": '{"error" : "42883"}'}
    checks = [
        # (got, expected, entry, want)
        ('"5"', '"5"', None, "match"),
        ('"6"', '"5"', None, "unexplained"),
        ('"5"', '"5"', entry, "stale"),
        ('{"error" : "42883"}', '"5"', entry, "documented"),
        # A documented divergence exempts only the outcome it documents.
        ('"wrong"', '"5"', entry, "unexplained"),
        ('{"error" : "42P01"}', '"5"', entry, "unexplained"),
    ]
    per_call = dict(entry, outcome={"1": '"x"'})
    checks += [('"x"', '"5"', per_call, "documented"),
               ('"y"', '"5"', per_call, "unexplained")]
    for got, expected, ent, want in checks:
        verdict = classify(got, expected, ent, "1")
        if verdict != want:
            failures.append("classify(%r, %r, %s) gave %s, want %s"
                            % (got, expected, "exemption" if ent else "None",
                               verdict, want))
    return failures


def main():
    failures = selftest() + check_exemptions(CASES)
    if failures:
        sys.stderr.write("differential.py self-test failed:\n  %s\n"
                         % "\n  ".join(failures))
        return 2

    _, err, rc = psql(PROBE + SETUP, stop_on_error=True)
    if rc != 0:
        sys.stderr.write("setup failed:\n%s\n" % err)
        return 2

    failures = probe_selftest()
    if failures:
        sys.stderr.write("differential.py self-test failed:\n  %s\n"
                         % "\n  ".join(failures))
        return 2

    create_errors = create_variants()
    created_ok = set()
    for case in CASES:
        for tag in ["ref"] + [DIALECTS[d] for d in case["bodies"]]:
            if (case["name"], tag) not in create_errors:
                created_ok.add((case["name"], tag))

    results = probe_all(created_ok)

    ncmp = nmatch = 0
    mismatches = []       # diverged, and nothing says it should
    documented = []       # diverged exactly where a documented limitation says
    stale = []            # a documented divergence that no longer happens
    for case in CASES:
        known = case.get("documented", [])
        for call in case["calls"]:
            row = results.get((case["name"], call), {})
            expected = row.get("ref")
            if expected is None:
                continue
            for dialect in case["bodies"]:
                tag = DIALECTS[dialect]
                if tag not in row:
                    continue
                ncmp += 1
                entry = find_exemption(known, dialect, call)
                verdict = classify(row[tag], expected, entry, call)
                if verdict == "match":
                    nmatch += 1
                elif verdict == "stale":
                    stale.append((case["name"], dialect, call, entry["reason"]))
                elif verdict == "documented":
                    documented.append((case["name"], dialect, call,
                                       entry["reason"]))
                else:
                    mismatches.append((case["name"], dialect, call,
                                       expected, row[tag], entry))

    print("plx differential test")
    print("  cases                : %d" % len(CASES))
    print("  comparisons          : %d" % ncmp)
    print("  matching reference   : %d" % nmatch)
    print("  documented divergence: %d" % len(documented))
    print("  unexplained          : %d" % len(mismatches))
    print("  stale exemptions     : %d" % len(stale))
    print("  build errors         : %d" % len(create_errors))

    gaps = []
    for case in CASES:
        missing = [d for d in DIALECTS if d not in case["bodies"]]
        if missing:
            gaps.append((case["name"], missing))
    if gaps:
        print("\ndialects a case does not exercise:")
        for name, missing in gaps:
            print("  %-10s %s" % (name, " ".join(sorted(missing))))

    if create_errors:
        print("\nfunctions that failed to build:")
        for (name, tag) in sorted(create_errors):
            print("  %-18s %-6s %s" % (name, tag, create_errors[(name, tag)]))

    if mismatches:
        print("\nunexplained divergences from the plpgsql reference:")
        for name, dialect, call, expected, got, entry in mismatches:
            print("  %s(%s) %s" % (name, call, dialect))
            print("      reference: %s" % expected)
            if entry is not None:
                print("      documented: %s" % documented_outcome(entry, call))
            print("      dialect  : %s" % got)

    if stale:
        print("\nexemptions that no longer apply (the dialect now agrees, so"
              "\nboth the exemption and the documentation should be removed):")
        for name, dialect, call, reason in stale:
            print("  %s(%s) %s: %s" % (name, call, dialect, reason))

    return 1 if (mismatches or stale or create_errors) else 0


if __name__ == "__main__":
    sys.exit(main())
