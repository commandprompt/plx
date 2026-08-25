#!/usr/bin/env python3
"""Validate META.json against the PGXN Meta Specification v1.

PGXN rejects a distribution whose META.json does not satisfy the spec, and it
does so at upload time, which is after a release has been tagged and published.
That is an expensive place to discover a typo, so the constraints are checked
here instead.

The rules mirror the v1 schemas at https://github.com/pgxn/meta. Note that a
Tag and a Term are not the same: both exclude slash, backslash and control
characters, but only a Term also excludes spaces, so "sql server" is a valid
tag and an invalid term.

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
SEMVER = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
                    r"(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?"
                    r"(\+[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$")
RELEASE_STATUS = {"stable", "testing", "unstable"}


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


def main():
    try:
        with open(META, encoding="utf-8") as fh:
            m = json.load(fh)
    except (OSError, ValueError) as exc:
        sys.stderr.write("META.json could not be read as JSON: %s\n" % exc)
        return 2

    errs = []
    for field in REQUIRED:
        if field not in m:
            errs.append("required field %s is missing" % field)

    if "name" in m:
        check_term(m["name"], "name", errs)

    if "version" in m and not SEMVER.match(str(m["version"])):
        errs.append("version %r is not a semantic version" % m["version"])

    if "maintainer" in m:
        mt = m["maintainer"]
        mt = [mt] if isinstance(mt, str) else mt
        if not isinstance(mt, list) or not mt:
            errs.append("maintainer must be a string or a non-empty list")
        elif not all(isinstance(x, str) and x.strip() for x in mt):
            errs.append("every maintainer must be a non-empty string")

    if "meta-spec" in m and "version" not in (m["meta-spec"] or {}):
        errs.append("meta-spec must carry a version")

    prov = m.get("provides")
    if not isinstance(prov, dict) or not prov:
        errs.append("provides must be a non-empty object")
    else:
        for name, ext in prov.items():
            check_term(name, "provides key", errs)
            if not isinstance(ext, dict):
                errs.append("provides/%s must be an object" % name)
                continue
            if "file" not in ext:
                errs.append("provides/%s is missing file" % name)
            elif not os.path.exists(os.path.join(os.path.dirname(META),
                                                 ext["file"])):
                errs.append("provides/%s names %s, which is not in the "
                            "distribution" % (name, ext["file"]))
            if "version" not in ext:
                errs.append("provides/%s is missing version" % name)
            elif not SEMVER.match(str(ext["version"])):
                errs.append("provides/%s version %r is not a semantic version"
                            % (name, ext["version"]))

    if "tags" in m:
        tags = m["tags"]
        if not isinstance(tags, list) or not tags:
            errs.append("tags must be a non-empty list")
        else:
            if len(set(tags)) != len(tags):
                errs.append("tags must be unique")
            for i, t in enumerate(tags):
                check_tag(t, "tags[%d]" % i, errs)

    if "release_status" in m and m["release_status"] not in RELEASE_STATUS:
        errs.append("release_status %r must be one of %s"
                    % (m["release_status"], ", ".join(sorted(RELEASE_STATUS))))

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
