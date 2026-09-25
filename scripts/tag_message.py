#!/usr/bin/env python3
"""Compose a release tag message from a weekly qualification, and refuse a
release whose own claims the measurements contradict.

    scripts/tag_message.py --version 0.19.6 --title "..." --notes-file notes.md \
        --impact identical,additive --qualification q.json --qualification-text q.txt \
        --measurements m.json [--previous-measurements prev.json --previous-version 0.19.5] \
        --out message.txt

Run by packaging/Jenkinsfile.tag (Crete-Tag). The tag message it writes is the
release record every other consumer reads:

    crête 0.19.6 — <title>                 subject: Codeberg release name, OBS
                                           changelog line, the site's `title`
    <notes paragraphs>                     the site's `body`, release notes
    Qualification (...)                    the weekly's verdict, indented four
        ...                                spaces so Codeberg renders it as code
        vs 0.19.5   ...                    what moved since the last release
    Impact: identical, additive            git trailers: the site's `impact`
    Reproduce: --dr-blocks legacy          and how to get the old numbers back
    Qualified-by: Crete-Weekly #84

Impact is the crete-dr site's vocabulary (src/lib/releases.js):
  moves      recorded measurements change -- Reproduce: is then REQUIRED
  identical  measurements are provably unchanged
  additive   new surface only
and it is CHECKED, not just copied. Against the previous release's recorded
measurements (attached to its Codeberg release):
  identical + any shared value moved   -> exit 4, no tag. The claim is false.
  moves     + nothing moved            -> warning only: a change can move
                                          numbers the corpus does not contain.
With no previous measurements (the first release under this scheme, or a
release without the asset) the comparison says so and nothing is blocked.

Exit codes: 0 message written, 2 bad arguments, 3 the weekly's gate failed,
4 the declared impact is contradicted by the measurements.
"""

import argparse
import json
import sys

IMPACTS = ("moves", "identical", "additive")


def load(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def compare(cur, prev):
    """(moved, shared, new, gone) between two measurements.json files.

    Exact float equality on purpose: crete is bit-reproducible on one host,
    and "bit-identical" is the claim being checked. The weekly always runs on
    the same agent, so host arithmetic is not a variable here."""
    c, p = cur["values"], prev["values"]
    shared = c.keys() & p.keys()
    moved = sorted((abs(c[k] - p[k]), k) for k in shared if c[k] != p[k])
    return moved, len(shared), len(c.keys() - p.keys()), len(p.keys() - c.keys())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--version", required=True)
    ap.add_argument("--title", required=True)
    ap.add_argument("--notes-file", required=True)
    ap.add_argument("--impact", required=True, help="comma-separated: " + ", ".join(IMPACTS))
    ap.add_argument("--reproduce", default="", help="flag(s) that reproduce the old numbers; required with 'moves'")
    ap.add_argument("--qualification", required=True, help="qualification.json from the weekly")
    ap.add_argument("--qualification-text", required=True, help="qualification.txt from the weekly")
    ap.add_argument("--measurements", required=True, help="measurements.json from the weekly")
    ap.add_argument("--previous-measurements", help="the previous release's measurements.json, if it has one")
    ap.add_argument("--previous-version", default="the previous release")
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)

    def die(code, msg):
        sys.stderr.write(f"ERROR: {msg}\n")
        sys.exit(code)

    impact = [x.strip().lower() for x in a.impact.split(",") if x.strip()]
    bad = [x for x in impact if x not in IMPACTS]
    if not impact or bad:
        die(2, f"--impact must be a non-empty subset of {', '.join(IMPACTS)} (got {a.impact!r})")
    if "moves" in impact and "identical" in impact:
        die(2, "--impact cannot be both 'moves' and 'identical'")
    if "moves" in impact and not a.reproduce.strip():
        die(2, "'moves' needs --reproduce: the flag that gives back the previous numbers "
               "(the site's rule: where numbers moved, say how to get the old ones)")
    title = a.title.strip()
    if not title or "\n" in title:
        die(2, "--title must be one non-empty line")

    with open(a.notes_file, encoding="utf-8") as fh:
        notes = fh.read().strip()
    if not notes:
        die(2, "the release notes are empty -- say what changed")
    for line in notes.splitlines():
        if line.split(":", 1)[0] in ("Impact", "Reproduce", "Qualified-by"):
            die(2, f"the notes contain a trailer line ({line!r}); those are generated")

    q = load(a.qualification)
    if not q.get("gate", {}).get("ok"):
        die(3, "the weekly's gate failed: " + "; ".join(q.get("gate", {}).get("reasons", ["unknown"])))

    cur = load(a.measurements)
    if a.previous_measurements:
        prev = load(a.previous_measurements)
        moved, shared, new, gone = compare(cur, prev)
        extra = "".join([f", {new:,} new" if new else "", f", {gone:,} no longer measured" if gone else ""])
        if not moved:
            vs = f"{shared:,} of {shared:,} shared values bit-identical{extra}"
        else:
            d, k = moved[-1]
            vs = (f"{len(moved):,} of {shared:,} shared values moved, largest {d:.4g} "
                  f"({k.replace('|', ' / ')}){extra}")
        if prev.get("settings") != cur.get("settings"):
            vs += f"\n                (settings differ from {a.previous_version}: {prev.get('settings')})"
        if "identical" in impact and moved:
            top = "\n".join(f"  {d:.6g}  {k}" for d, k in reversed(moved[-10:]))
            die(4, f"declared 'identical' but {len(moved)} of {shared} shared values moved "
                   f"since {a.previous_version}. Largest:\n{top}")
        if "moves" in impact and not moved:
            sys.stderr.write(f"WARNING: declared 'moves' but all {shared} shared values are identical to "
                             f"{a.previous_version}; fine if what moves is outside the corpus.\n")
    else:
        vs = f"not comparable: no recorded measurements for {a.previous_version}"
        if "identical" in impact:
            vs += " -- 'identical' is unverified"

    with open(a.qualification_text, encoding="utf-8") as fh:
        qlines = fh.read().rstrip("\n").splitlines()
    at = next((i for i, l in enumerate(qlines) if l.strip().startswith("verdict")), len(qlines))
    qlines.insert(at, f"    {'vs ' + a.previous_version:<12}{vs}")

    trailers = [f"Impact: {', '.join(x for x in IMPACTS if x in impact)}"]
    if a.reproduce.strip():
        trailers.append(f"Reproduce: {a.reproduce.strip()}")
    trailers.append(f"Qualified-by: Crete-Weekly #{q.get('weekly_build')}")

    msg = (f"crête {a.version} — {title}\n\n{notes}\n\n" + "\n".join(qlines) + "\n\n"
           + "\n".join(trailers) + "\n")
    with open(a.out, "w", encoding="utf-8") as fh:
        fh.write(msg)
    sys.stdout.write(msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
