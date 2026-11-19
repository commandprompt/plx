#!/usr/bin/env python3
"""Validate META.json against the PGXN Meta Specification v1.

PGXN rejects a distribution whose META.json does not satisfy the spec, and it
does so at upload time, which is after a release has been tagged and published.
That is an expensive place to discover a typo, so the constraints are checked
here instead.

The checks cover required keys, unknown keys (only x_ custom keys may be
added), value types, the license identifiers, non-empty text fields, the
meta-spec version and url, the prereqs structure and its version ranges,
resources, provides, no_index, release_status and tags. A self-test runs
first: it breaks a valid document in known ways and requires each break to be
reported.

The rules mirror the v1 schemas at https://github.com/pgxn/meta. Each pattern
must match the whole value, as a JSON Schema pattern does. Note that a Tag and
a Term are not the same: both exclude slash, backslash and control characters,
but only a Term also excludes spaces, so "sql server" is a valid tag and an
invalid term.

Not checked: the "uri" and "email" formats (a URL or address only has to be a
non-empty string), and that a path uses Unix conventions. A provides file must
exist as a regular file, but a docfile or a no_index path is not looked up.

Run with `make metacheck`.
"""
import json
import os
import re
import sys
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
META = os.environ.get("PLX_META", os.path.join(HERE, os.pardir, "META.json"))

REQUIRED = ["name", "version", "abstract", "maintainer", "license", "provides",
            "meta-spec"]
# version.schema.json. Use fullmatch() with these patterns: a JSON Schema
# pattern does not match before a trailing newline, and Python's $ does.
SEMVER = re.compile(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
                    r"(?:-((?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)"
                    r"(?:\.(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?"
                    r"(?:\+([0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*))?", re.ASCII)
RELEASE_STATUS = {"stable", "testing", "unstable"}

# Keys a distribution may carry at the top level. Any other key must be a
# custom key starting with x_ or X_ (distribution.schema.json sets
# unevaluatedProperties to false).
TOP_LEVEL = set(REQUIRED) | {"description", "generated_by", "tags", "no_index",
                             "prereqs", "release_status", "resources"}
CUSTOM = re.compile(r"^[xX]_.")
LICENSES = {
    "agpl_3", "apache_1_1", "apache_2_0", "artistic_1", "artistic_2", "bsd",
    "freebsd", "gfdl_1_2", "gfdl_1_3", "gpl_1", "gpl_2", "gpl_3", "lgpl_2_1",
    "lgpl_3_0", "mit", "mozilla_1_0", "mozilla_1_1", "openssl", "perl_5",
    "postgresql", "qpl_1_0", "ssleay", "sun", "zlib", "open_source",
    "restricted", "unrestricted", "unknown",
}
META_SPEC_VERSION = re.compile(r"1\.0\.[0-9]+")
META_SPEC_URLS = {"https://pgxn.org/meta/spec.txt", "http://pgxn.org/meta/spec.txt"}
PREREQ_PHASES = {"configure", "build", "test", "runtime", "develop"}
PREREQ_RELATIONS = {"requires", "recommends", "suggests", "conflicts"}
# version_range.schema.json: "0", or one or more comma-separated versions,
# each with an optional comparison operator.
_V = (r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"
      r"(?:-(?:0|[1-9][0-9]*|[0-9]*[a-zA-Z-][0-9a-zA-Z-]*)"
      r"(?:\.(?:0|[1-9][0-9]*|[0-9]*[a-zA-Z-][0-9a-zA-Z-]*))*)?"
      r"(?:\+[0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*)?")
_OP = r"(?:(?:[=!]=|[<>]=?)\s*)?"
VERSION_RANGE = re.compile(r"(?:" + _OP + "(?:" + _V + "|0))"
                           r"(?:,\s*" + _OP + _V + ")*", re.ASCII)
EXTENSION_KEYS = {"file", "version", "abstract", "docfile", "docpath"}
NO_INDEX_KEYS = {"file", "directory"}
RESOURCE_KEYS = {"homepage": None,
                 "bugtracker": {"web", "mailto"},
                 "repository": {"url", "web", "type"}}


def has_control(s):
    return any(unicodedata.category(c) == "Cc" for c in s)


def check_term(value, label, errs):
    """A Term: 2 or more characters, no slash, backslash, space or control."""
    if not isinstance(value, str) or len(value) < 2:
        errs.append("%s must be a string of at least two characters" % label)
        return
    for ch, name in (("/", "slash"), ("\\", "backslash")):
        if ch in value:
            errs.append("%s %r must not contain a %s" % (label, value, name))
    if any(c.isspace() for c in value):
        errs.append("%s %r must not contain whitespace" % (label, value))
    if has_control(value):
        errs.append("%s %r must not contain control characters" % (label, value))


def check_tag(value, label, errs):
    """A Tag: 2 to 255 characters, no slash, backslash or control. Spaces are
    allowed, which is the one way a Tag is looser than a Term."""
    if not isinstance(value, str):
        errs.append("%s must be a string" % label)
        return
    if not 2 <= len(value) <= 255:
        errs.append("%s %r must be between 2 and 255 characters" % (label, value))
    for ch, name in (("/", "slash"), ("\\", "backslash")):
        if ch in value:
            errs.append("%s %r must not contain a %s" % (label, value, name))
    if has_control(value):
        errs.append("%s %r must not contain control characters" % (label, value))


def unknown_keys(obj, allowed, label, errs):
    """Report keys of obj outside allowed that are not x_ custom keys."""
    for key in obj:
        if key not in allowed and not CUSTOM.match(key):
            errs.append("%s has unknown key %r (custom keys must start "
                        "with x_)" % (label, key))


def unique(values):
    """True when no two values are equal. Works for unhashable values."""
    return len(set(json.dumps(v, sort_keys=True) for v in values)) == len(values)


def is_semver(v):
    return isinstance(v, str) and SEMVER.fullmatch(v) is not None


def check_path(value, label, errs):
    """A Path (path.schema.json): a string of at least two characters."""
    if not (isinstance(value, str) and len(value) >= 2):
        errs.append("%s must be a path of at least two characters" % label)


def check_no_index(ni, errs):
    """no_index: file and/or directory, each a path or a non-empty list of
    unique paths."""
    if not isinstance(ni, dict):
        errs.append("no_index must be an object")
        return
    unknown_keys(ni, NO_INDEX_KEYS, "no_index", errs)
    if not NO_INDEX_KEYS & set(ni):
        errs.append("no_index must name file or directory")
    for key in sorted(NO_INDEX_KEYS & set(ni)):
        label = "no_index/%s" % key
        paths = ni[key]
        if isinstance(paths, list):
            if not paths:
                errs.append("%s must not be an empty list" % label)
            elif not unique(paths):
                errs.append("%s must not repeat a path" % label)
            for i, p in enumerate(paths):
                check_path(p, "%s[%d]" % (label, i), errs)
        else:
            check_path(paths, label, errs)


def check_text(m, field, errs):
    """An optional field that, when present, must be a non-empty string."""
    if field in m and not (isinstance(m[field], str) and m[field]):
        errs.append("%s must be a non-empty string" % field)


def check_license(lic, errs):
    """A license: a known identifier, a list of them, or an object whose
    values are links to license texts."""
    if isinstance(lic, str):
        if lic not in LICENSES:
            errs.append("license %r is not a PGXN license identifier "
                        "(for example mit or postgresql)" % lic)
    elif isinstance(lic, list):
        if not lic:
            errs.append("license list must not be empty")
        if not unique(lic):
            errs.append("license list must not repeat an entry")
        for x in lic:
            if not isinstance(x, str) or x not in LICENSES:
                errs.append("license %r is not a PGXN license identifier"
                            % (x,))
    elif isinstance(lic, dict):
        if not lic:
            errs.append("license object must not be empty")
        for name, url in lic.items():
            if not isinstance(url, str) or not url:
                errs.append("license %r must map to the URL of its text"
                            % name)
    else:
        errs.append("license must be a string, a list or an object")


def check_prereqs(pre, errs):
    """prereqs: phase -> relationship -> extension name -> version range."""
    if not isinstance(pre, dict):
        errs.append("prereqs must be an object")
        return
    unknown_keys(pre, PREREQ_PHASES, "prereqs", errs)
    if not PREREQ_PHASES & set(pre):
        errs.append("prereqs must name at least one phase (%s)"
                    % ", ".join(sorted(PREREQ_PHASES)))
    for phase in sorted(PREREQ_PHASES & set(pre)):
        rels = pre[phase]
        label = "prereqs/%s" % phase
        if not isinstance(rels, dict):
            errs.append("%s must be an object" % label)
            continue
        unknown_keys(rels, PREREQ_RELATIONS, label, errs)
        if not PREREQ_RELATIONS & set(rels):
            errs.append("%s must name at least one relationship (%s)"
                        % (label, ", ".join(sorted(PREREQ_RELATIONS))))
        for rel in sorted(PREREQ_RELATIONS & set(rels)):
            exts = rels[rel]
            rlabel = "%s/%s" % (label, rel)
            if not isinstance(exts, dict) or not exts:
                errs.append("%s must be a non-empty object" % rlabel)
                continue
            for name, rng in exts.items():
                check_term(name, "%s key" % rlabel, errs)
                ok = (rng == 0 and not isinstance(rng, bool)) or \
                    (isinstance(rng, str) and VERSION_RANGE.fullmatch(rng))
                if not ok:
                    errs.append("%s/%s version %r is not a version range "
                                "(for example 13.0.0 or >= 13.0.0, or 0)"
                                % (rlabel, name, rng))


def check_resources(res, errs):
    if not isinstance(res, dict):
        errs.append("resources must be an object")
        return
    unknown_keys(res, set(RESOURCE_KEYS), "resources", errs)
    if not set(RESOURCE_KEYS) & set(res):
        errs.append("resources must name homepage, bugtracker or repository")
    if "homepage" in res and not (isinstance(res["homepage"], str)
                                  and res["homepage"]):
        errs.append("resources/homepage must be a URL")
    for key in ("bugtracker", "repository"):
        if key not in res:
            continue
        if not isinstance(res[key], dict):
            errs.append("resources/%s must be an object" % key)
            continue
        unknown_keys(res[key], RESOURCE_KEYS[key], "resources/%s" % key, errs)
        for field in sorted(RESOURCE_KEYS[key] & set(res[key])):
            if not (isinstance(res[key][field], str) and res[key][field]):
                errs.append("resources/%s/%s must be a non-empty string"
                            % (key, field))
    if isinstance(res.get("bugtracker"), dict) and \
            not {"web", "mailto"} & set(res["bugtracker"]):
        errs.append("resources/bugtracker must have web or mailto")
    if isinstance(res.get("repository"), dict):
        rep = res["repository"]
        if "web" not in rep and not {"url", "type"} <= set(rep):
            errs.append("resources/repository must have web, or both url "
                        "and type")
        # repository.schema.json: type matches ^\p{lower}+$
        if isinstance(rep.get("type"), str) and \
                not (rep["type"] and all(c.islower() for c in rep["type"])):
            errs.append("resources/repository/type %r must be lower-case "
                        "letters only (for example git)" % rep["type"])


def validate(m, base):
    """Return the list of ways the META.json document m breaks the spec.

    base is the distribution directory that provides/*/file is relative to.
    """
    errs = []
    for field in REQUIRED:
        if field not in m:
            errs.append("required field %s is missing" % field)

    if "name" in m:
        check_term(m["name"], "name", errs)

    unknown_keys(m, TOP_LEVEL, "the distribution", errs)
    for field in ("abstract", "description", "generated_by"):
        check_text(m, field, errs)
    if "license" in m:
        check_license(m["license"], errs)
    if "prereqs" in m:
        check_prereqs(m["prereqs"], errs)
    if "resources" in m:
        check_resources(m["resources"], errs)
    if "no_index" in m:
        check_no_index(m["no_index"], errs)

    if "version" in m and not is_semver(m["version"]):
        errs.append("version %r is not a semantic version" % m["version"])

    if "maintainer" in m:
        mt = m["maintainer"]
        mt = [mt] if isinstance(mt, str) else mt
        if not isinstance(mt, list) or not mt:
            errs.append("maintainer must be a string or a non-empty list")
        elif not all(isinstance(x, str) and x.strip() for x in mt):
            errs.append("every maintainer must be a non-empty string")
        elif len(set(mt)) != len(mt):
            errs.append("maintainer list must not repeat an entry")

    if "meta-spec" in m:
        ms = m["meta-spec"]
        if not isinstance(ms, dict):
            errs.append("meta-spec must be an object")
        elif "version" not in ms:
            errs.append("meta-spec must carry a version")
        else:
            if not (isinstance(ms["version"], str)
                    and META_SPEC_VERSION.fullmatch(ms["version"])):
                errs.append("meta-spec version %r must be 1.0.x"
                            % (ms["version"],))
            if "url" in ms and not (isinstance(ms["url"], str)
                                    and ms["url"] in META_SPEC_URLS):
                errs.append("meta-spec url %r must be %s"
                            % (ms["url"], " or ".join(sorted(META_SPEC_URLS))))
            unknown_keys(ms, {"version", "url"}, "meta-spec", errs)

    prov = m.get("provides")
    if not isinstance(prov, dict) or not prov:
        errs.append("provides must be a non-empty object")
    else:
        for name, ext in prov.items():
            check_term(name, "provides key", errs)
            if not isinstance(ext, dict):
                errs.append("provides/%s must be an object" % name)
                continue
            unknown_keys(ext, EXTENSION_KEYS, "provides/%s" % name, errs)
            for field in ("abstract", "docpath"):
                if field in ext and not (isinstance(ext[field], str)
                                         and ext[field]):
                    errs.append("provides/%s/%s must be a non-empty string"
                                % (name, field))
            if "docfile" in ext:
                check_path(ext["docfile"], "provides/%s/docfile" % name, errs)
            if "file" not in ext:
                errs.append("provides/%s is missing file" % name)
            elif not (isinstance(ext["file"], str) and len(ext["file"]) >= 2):
                check_path(ext["file"], "provides/%s/file" % name, errs)
            elif not os.path.isfile(os.path.join(base, ext["file"])):
                errs.append("provides/%s names %s, which is not a file in the "
                            "distribution" % (name, ext["file"]))
            if "version" not in ext:
                errs.append("provides/%s is missing version" % name)
            elif not is_semver(ext["version"]):
                errs.append("provides/%s version %r is not a semantic version"
                            % (name, ext["version"]))

    if "tags" in m:
        tags = m["tags"]
        if not isinstance(tags, list) or not tags:
            errs.append("tags must be a non-empty list")
        else:
            if not unique(tags):
                errs.append("tags must be unique")
            for i, t in enumerate(tags):
                check_tag(t, "tags[%d]" % i, errs)

    if "release_status" in m and not (isinstance(m["release_status"], str)
                                      and m["release_status"] in RELEASE_STATUS):
        errs.append("release_status %r must be one of %s"
                    % (m["release_status"], ", ".join(sorted(RELEASE_STATUS))))

    return errs


# A document that satisfies the spec. The self-test breaks it one way at a
# time and requires validate() to notice. Its provides file is this script,
# relative to the distribution directory, so it always exists.
GOOD = {
    "name": "plx", "version": "1.0.0", "abstract": "a test distribution",
    "maintainer": ["someone <someone@example.org>"], "license": "mit",
    "provides": {"plx": {"file": "test/check_meta.py", "version": "1.0.0"}},
    "meta-spec": {"version": "1.0.0", "url": "https://pgxn.org/meta/spec.txt"},
    "x_custom": "custom keys are allowed",
}

# (description, change to GOOD, text the error must contain, or None when the
# changed document is still valid)
SELFTEST = [
    ("license not in the list", {"license": "MIT License"}, "license"),
    ("license empty list", {"license": []}, "license"),
    ("license object with a non-string", {"license": {"MIT": 5}}, "license"),
    ("license list", {"license": ["mit", "postgresql"]}, None),
    ("license list repeats an entry", {"license": ["mit", "mit"]}, "repeat"),
    ("license object", {"license": {"MIT": "https://opensource.org/license/mit"}}, None),
    ("empty abstract", {"abstract": ""}, "abstract"),
    ("empty description", {"description": ""}, "description"),
    ("empty generated_by", {"generated_by": ""}, "generated_by"),
    ("unknown top-level key", {"homepage": "https://example.org"}, "homepage"),
    ("meta-spec version not 1.0.x", {"meta-spec": {"version": "banana"}}, "meta-spec"),
    ("meta-spec unknown key", {"meta-spec": {"version": "1.0.0", "spec": "x"}}, "meta-spec"),
    ("meta-spec url not the spec", {"meta-spec": {"version": "1.0.0",
        "url": "https://example.org/spec.txt"}}, "meta-spec url"),
    ("prereq version not a version", {"prereqs": {"runtime": {"requires":
        {"PostgreSQL": "thirteen"}}}}, "thirteen"),
    ("prereq unknown phase", {"prereqs": {"install": {"requires":
        {"PostgreSQL": "13.0.0"}}}}, "install"),
    ("prereq unknown relationship", {"prereqs": {"runtime": {"needs":
        {"PostgreSQL": "13.0.0"}}}}, "needs"),
    ("prereq version range", {"prereqs": {"runtime": {"requires":
        {"PostgreSQL": ">= 13.0.0, < 19.0.0", "plpgsql": 0}}}}, None),
    # homepage is present, so only the unknown key can cause the error.
    ("resources unknown key", {"resources": {"homepage": "https://example.org",
        "home": "https://example.org"}}, "'home'"),
    ("bugtracker unknown key", {"resources": {"bugtracker": {
        "web": "https://example.org/issues", "email": "a@example.org"}}},
        "'email'"),
    ("repository unknown key", {"resources": {"repository": {
        "web": "https://example.org", "vcs": "git"}}}, "'vcs'"),
    ("resources with x_ keys", {"resources": {"homepage": "https://example.org",
        "x_a": 1, "bugtracker": {"web": "https://example.org/i", "x_b": 2},
        "repository": {"web": "https://example.org", "x_c": 3}}}, None),
    ("provides unknown key", {"provides": {"plx": {"file": "test/check_meta.py",
        "version": "1.0.0", "docs": "README.md"}}}, "docs"),
    ("duplicate maintainer", {"maintainer": ["a <a@example.org>",
        "a <a@example.org>"]}, "maintainer"),
    # no_index.schema.json: file and directory only, plus x_ keys.
    ("no_index", {"no_index": {"file": "src/a.sql",
        "directory": ["doc", "test"], "x_note": 1}}, None),
    ("no_index unknown key", {"no_index": {"files": ["src/a.sql"]}}, "'files'"),
    ("no_index not an object", {"no_index": "x"}, "no_index"),
    ("no_index without file or directory", {"no_index": {"x_note": 1}},
        "no_index"),
    ("no_index empty list", {"no_index": {"file": []}}, "no_index/file"),
    ("no_index repeated path", {"no_index": {"directory": ["doc", "doc"]}},
        "no_index/directory"),
    ("no_index one-character path", {"no_index": {"file": "a"}},
        "no_index/file"),
    # bugtracker and repository values are strings; type is lower case.
    ("repository url not a string", {"resources": {"repository": {
        "url": 5, "type": "git"}}}, "resources/repository/url"),
    ("repository type not lower case", {"resources": {"repository": {
        "url": "https://example.org/a.git", "type": "Git"}}},
        "resources/repository/type"),
    ("bugtracker web not a string", {"resources": {"bugtracker": {
        "web": 7}}}, "resources/bugtracker/web"),
    # A JSON Schema pattern does not match before a trailing newline, and
    # Python's $ does, so each pattern must match the whole string.
    ("meta-spec version with a newline", {"meta-spec": {
        "version": "1.0.0\n"}}, "meta-spec version"),
    ("prereq version with a newline", {"prereqs": {"runtime": {"requires":
        {"PostgreSQL": "13.0.0\n"}}}}, "not a version range"),
    ("version with a newline", {"version": "1.0.0\n"}, "semantic version"),
    ("version not a string", {"version": 1}, "semantic version"),
    # path.schema.json: at least two characters; file must be a file.
    ("provides empty docfile", {"provides": {"plx": {
        "file": "test/check_meta.py", "version": "1.0.0", "docfile": ""}}},
        "provides/plx/docfile"),
    ("provides empty file", {"provides": {"plx": {"file": "",
        "version": "1.0.0"}}}, "provides/plx/file"),
    ("provides file is a directory", {"provides": {"plx": {"file": "test",
        "version": "1.0.0"}}}, "not a file"),
    # A value of the wrong type is reported, not a Python traceback.
    ("license list holding an object", {"license": [{"mit": "x"}]},
        "license"),
    ("tags holding an object", {"tags": [{"a": 1}]}, "tags[0]"),
    ("release_status a list", {"release_status": ["stable"]},
        "release_status"),
]


def selftest():
    """Break a valid document in known ways. Returns a list of failures."""
    base = os.path.join(HERE, os.pardir)
    failures = []
    errs = validate(dict(GOOD), base)
    if errs:
        failures.append("the valid sample document was rejected: %s"
                        % "; ".join(errs))
    for what, change, needle in SELFTEST:
        doc = dict(GOOD)
        doc.update(change)
        try:
            errs = validate(doc, base)
        except Exception as exc:  # noqa: BLE001 - report, do not crash
            failures.append("%s: validate() raised %s: %s"
                            % (what, type(exc).__name__, exc))
            continue
        if needle is None and errs:
            failures.append("%s: rejected a valid document: %s"
                            % (what, "; ".join(errs)))
        elif needle is not None and not any(needle in e for e in errs):
            failures.append("%s: not reported (errors: %s)"
                            % (what, "; ".join(errs) or "none"))
    return failures


def main():
    failures = selftest()
    if failures:
        sys.stderr.write("check_meta.py self-test failed:\n  %s\n"
                         % "\n  ".join(failures))
        return 2

    try:
        with open(META, encoding="utf-8") as fh:
            m = json.load(fh)
    except (OSError, ValueError) as exc:
        sys.stderr.write("META.json could not be read as JSON: %s\n" % exc)
        return 2

    errs = validate(m, os.path.dirname(META))
    if errs:
        print("META.json does not satisfy the PGXN Meta Spec v1:")
        for e in errs:
            print("  %s" % e)
        return 1

    print("META.json satisfies the PGXN Meta Spec v1")
    print("  name         %s" % m["name"])
    print("  version      %s (distribution)" % m["version"])
    for name, ext in m["provides"].items():
        print("  provides     %s %s -> %s"
              % (name, ext["version"], ext["file"]))
    print("  tags         %d, all valid" % len(m.get("tags", [])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
