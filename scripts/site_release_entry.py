#!/usr/bin/env python3
"""Add a release tag's entry to the crete-dr site's changelog (src/lib/releases.js).

    scripts/site_release_entry.py --tag v0.19.6 --releases /path/to/crete-dr/src/lib/releases.js

Run by packaging/Jenkinsfile.site (Crete-Site) inside a crete checkout that has
the tags. Reads the ANNOTATED TAG, which Crete-Tag wrote as the release record
(see scripts/tag_message.py):

    subject     "crête X.Y.Z — <title>"          -> title
    date        the tagger date                   -> date
    paragraphs  up to the "Qualification (" block -> body[]
    Impact:     trailer                           -> impact[]
    Reproduce:  trailer                           -> a body paragraph, when the
                                                     notes do not already name it
    the qualification block                       -> a body paragraph saying what
                                                     CI verified

and inserts one entry into `releases`, newest first, in the file's own style
(tabs, single-quoted strings, typographic apostrophes outside `code`). The first
entry drives the site's version everywhere (src/lib/version.js), so adding the
entry IS the site's version bump.

Idempotent: a version already in the file is left alone (exit 0, "unchanged").
A tag with no Impact trailer was not cut by Crete-Tag and is refused (exit 3):
the site's rule is that no entry is added without deciding its impact.

Exit codes: 0 added or unchanged, 2 bad input, 3 the tag is not a release record.
"""

import argparse
import re
import subprocess
import sys

IMPACT_ORDER = ("moves", "identical", "additive")


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def tag_record(tag):
    fmt = "%(objecttype)%00%(taggerdate:short)%00%(contents:subject)%00%(contents:body)"
    out = git("for-each-ref", f"refs/tags/{tag}", f"--format={fmt}")
    if not out.strip():
        sys.exit(f"ERROR: no tag {tag} in this repository (fetch tags first)")
    kind, date, subject, body = out.rstrip("\n").split("\0", 3)
    if kind != "tag":
        print(f"ERROR: {tag} is a lightweight tag -- not a release record")
        sys.exit(3)
    trailers = {}
    for line in git("for-each-ref", f"refs/tags/{tag}",
                    "--format=%(trailers:only,unfold)").splitlines():
        k, _, v = line.partition(":")
        if k.strip():
            trailers[k.strip()] = v.strip()
    return date, subject, body, trailers


def paragraphs(body):
    """Notes paragraphs (before the qualification block) and that block."""
    paras = [p.strip() for p in re.split(r"\n\s*\n", body.strip()) if p.strip()]
    notes, qual = [], []
    for p in paras:
        if p.startswith("Qualification ("):
            qual = p.splitlines()
            break
        notes.append(" ".join(l.strip() for l in p.splitlines()))
    return notes, qual


def qual_paragraph(qual):
    """'**Qualified by Crete-Weekly #84** on `abc1234`: ...' from the block."""
    if not qual:
        return None
    m = re.match(r"Qualification \(Crete-Weekly #(\S+) on (\w+)", qual[0])
    fields = {}
    for line in qual[1:]:
        s = line.strip()
        mm = re.match(r"(tests|compared|vs \S+)\s{2,}(.*)$", s)
        if mm:
            fields[mm.group(1)] = mm.group(2)
    parts = []
    if "tests" in fields:
        parts.append("tests: " + fields["tests"])
    if "compared" in fields:
        parts.append("compared " + fields["compared"])
    vs = next((k for k in fields if k.startswith("vs ")), None)
    if vs:
        parts.append(f"against {vs[3:]}, {fields[vs]}")
    head = f"**Qualified by Crete-Weekly #{m.group(1)}** on `{m.group(2)}`" if m else "**Qualified by the weekly**"
    return (head + ": " + "; ".join(parts) + ".") if parts else None


def typographic(text):
    """Straight apostrophes -> ’, except inside `code` (flags must stay typeable)."""
    return "".join(seg if i % 2 else seg.replace("'", "’")
                   for i, seg in enumerate(re.split(r"(`[^`]*`)", text)))


def js(s):
    return "'" + s.replace("\\", "\\\\").replace("'", "\\'") + "'"


def vkey(v):
    return tuple(int(x) for x in v.split("."))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--tag", required=True)
    ap.add_argument("--releases", required=True, help="path to crete-dr's src/lib/releases.js")
    a = ap.parse_args(argv)

    if not re.fullmatch(r"v\d+\.\d+\.\d+", a.tag):
        print(f"ERROR: {a.tag!r} is not a vX.Y.Z tag")
        return 2
    version = a.tag[1:]
    date, subject, body, trailers = tag_record(a.tag)

    impact = [x.strip() for x in trailers.get("Impact", "").split(",") if x.strip()]
    if not impact:
        print(f"ERROR: {a.tag} has no Impact: trailer, so it was not cut by Crete-Tag. "
              "Add its site entry by hand -- the site never publishes an entry without an impact.")
        return 3
    if any(x not in IMPACT_ORDER for x in impact):
        print(f"ERROR: unknown impact in {a.tag}: {impact}")
        return 2
    impact = [x for x in IMPACT_ORDER if x in impact]

    m = re.match(r"crête\s+" + re.escape(version) + r"\s+—\s+(.+)$", subject)
    if not m:
        print(f"ERROR: subject {subject!r} is not 'crête {version} — <title>'")
        return 2
    title = m.group(1).strip()

    notes, qual = paragraphs(body)
    body_out = list(notes)
    rep = trailers.get("Reproduce")
    if rep and rep not in " ".join(notes):
        body_out.append(f"To get the previous numbers back: `{rep}`.")
    qp = qual_paragraph(qual)
    if qp:
        body_out.append(qp)

    with open(a.releases, encoding="utf-8") as fh:
        src = fh.read()
    if re.search(r"^\t\tversion: '" + re.escape(version) + "',$", src, re.M):
        print(f"unchanged: {version} is already in {a.releases}")
        return 0

    entry = ("\t{\n"
             f"\t\tversion: {js(version)},\n"
             f"\t\tdate: {js(date)},\n"
             f"\t\timpact: [{', '.join(js(x) for x in impact)}],\n"
             f"\t\ttitle: {js(typographic(title))},\n"
             "\t\tbody: [\n"
             + ",\n".join(f"\t\t\t{js(typographic(p))}" for p in body_out)
             + "\n\t\t]\n\t},\n")

    anchor = "export const releases = [\n"
    if src.count(anchor) != 1:
        print(f"ERROR: {a.releases} does not contain exactly one '{anchor.strip()}' line")
        return 2
    start = src.index(anchor) + len(anchor)
    # Newest first: insert before the first existing entry with a lower version.
    pos = None
    for mm in re.finditer(r"^\t\{\n\t\tversion: '(\d+\.\d+\.\d+)',", src[start:], re.M):
        if vkey(mm.group(1)) < vkey(version):
            pos = start + mm.start()
            break
    if pos is None:
        close = src.index("\n];", start)
        pos = close + 1          # an older backfill: after the last entry
        if not src[:pos].rstrip().endswith(","):
            src = src[:pos - 1].rstrip() + ",\n" + src[pos:]
            pos = src.index("\n];", start) + 1
    src = src[:pos] + entry + src[pos:]
    with open(a.releases, "w", encoding="utf-8") as fh:
        fh.write(src)
    print(f"added: {version} ({', '.join(impact)}) -- {title}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
