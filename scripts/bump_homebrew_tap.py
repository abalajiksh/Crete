#!/usr/bin/env python3
"""bump_homebrew_tap.py -- point the Homebrew formulae at a crete release tag.

    scripts/bump_homebrew_tap.py TAP_DIR TAG

Rewrites Formula/crete.rb and Formula/crete-gui.rb in a checkout of
codeberg.org/abksh/homebrew-crete so both build TAG:

  * the stable url + sha256 -> Codeberg's archive of TAG. The archive is
    downloaded TWICE and must hash identically: Codeberg generates it on
    demand, and a checksum taken from a one-off byte stream would break every
    install if the next generation differed.
  * resource "ffmpeg" -> ffmpeg-<FFMPEG_VERSION>.tar.xz when the Makefile at
    TAG pins a different FFmpeg than the formulae do.
  * resource "imgui"  -> IMGUI_COMMIT from scripts/make_obs_source.sh at TAG,
    when it differs (crete-gui only).

The resource checks are what keep a Makefile or ImGui bump from silently
shipping a formula that builds the OLD dependency.

Run from inside the crete checkout (it reads TAG's files with `git show`).
Prints what changed and exits 0; exits 0 with "unchanged" when the formulae
already build TAG, so a re-run is harmless. Nothing is committed or pushed.
"""

import hashlib
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ARCHIVE = "https://codeberg.org/abksh/Crete/archive/{tag}.tar.gz"
FFMPEG = "https://ffmpeg.org/releases/ffmpeg-{v}.tar.xz"
IMGUI = "https://github.com/ocornut/imgui/archive/{c}.tar.gz"
FORMULAE = ("crete.rb", "crete-gui.rb")


def die(msg):
    sys.exit(f"bump_homebrew_tap: {msg}")


def sha256_of(url):
    req = urllib.request.Request(url, headers={"User-Agent": "crete-release"})
    h = hashlib.sha256()
    with urllib.request.urlopen(req, timeout=300) as r:
        for chunk in iter(lambda: r.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def at_tag(tag, path):
    r = subprocess.run(["git", "show", f"{tag}:{path}"], capture_output=True, text=True)
    if r.returncode != 0:
        die(f"{tag} has no {path} (unknown tag, or it predates the packaging): "
            + r.stderr.strip())
    return r.stdout


def one(pattern, text, what):
    m = re.search(pattern, text, re.M)
    if not m:
        die(f"could not find {what}")
    return m.group(1)


def set_resource(text, name, url, sha):
    """Replace url + sha256 inside `resource "name" do ... end`."""
    block = re.compile(r'(  resource "%s" do\n    url ")[^"]+("\n    sha256 ")[0-9a-f]{64}(")'
                       % re.escape(name))
    new, n = block.subn(lambda m: f"{m.group(1)}{url}{m.group(2)}{sha}{m.group(3)}", text)
    if n != 1:
        die(f'resource "{name}" block not found exactly once')
    return new


def main():
    if len(sys.argv) != 3:
        die("usage: bump_homebrew_tap.py TAP_DIR TAG")
    tap, tag = Path(sys.argv[1]), sys.argv[2]
    if not re.fullmatch(r"v\d+\.\d+\.\d+", tag):
        die(f"tag {tag!r} is not vX.Y.Z")
    version = tag[1:]
    makefile = at_tag(tag, "Makefile")
    if one(r"^VERSION\s*\?=\s*(\S+)", makefile, "VERSION in Makefile") != version:
        die(f"Makefile at {tag} does not say VERSION ?= {version}")
    ffmpeg_v = one(r"^FFMPEG_VERSION\s*\?=\s*(\S+)", makefile, "FFMPEG_VERSION")
    imgui_c = one(r"^IMGUI_COMMIT=([0-9a-f]{40})", at_tag(tag, "scripts/make_obs_source.sh"),
                  "IMGUI_COMMIT")

    files = {f: (tap / "Formula" / f).read_text() for f in FORMULAE}
    url = ARCHIVE.format(tag=tag)
    changed = []

    if any(url not in t for t in files.values()):
        a = sha256_of(url)
        time.sleep(5)
        b = sha256_of(url)
        if a != b:
            die(f"Codeberg served two different archives for {tag} ({a} vs {b})")
        for f, t in files.items():
            t = re.sub(r'^(  url ")https://codeberg\.org/abksh/Crete/archive/[^"]+(")',
                       rf"\g<1>{url}\g<2>", t, count=1, flags=re.M)
            t = re.sub(r'^(  sha256 ")[0-9a-f]{64}(")', rf"\g<1>{a}\g<2>", t, count=1, flags=re.M)
            files[f] = t
        changed.append(f"source -> {tag} ({a})")

    ff_url = FFMPEG.format(v=ffmpeg_v)
    if any(ff_url not in t for t in files.values()):
        sha = sha256_of(ff_url)
        files = {f: set_resource(t, "ffmpeg", ff_url, sha) for f, t in files.items()}
        changed.append(f"ffmpeg -> {ffmpeg_v} ({sha})")

    im_url = IMGUI.format(c=imgui_c)
    if im_url not in files["crete-gui.rb"]:
        sha = sha256_of(im_url)
        files["crete-gui.rb"] = set_resource(files["crete-gui.rb"], "imgui", im_url, sha)
        changed.append(f"imgui -> {imgui_c[:12]} ({sha})")

    # Every formula must now name exactly this release; a regex that missed
    # would otherwise leave one formula behind without an error.
    for f, t in files.items():
        if url not in t or ff_url not in t:
            die(f"{f} was not fully updated")

    if not changed:
        print(f"unchanged: formulae already build {tag}")
        return
    for f, t in files.items():
        (tap / "Formula" / f).write_text(t)
    print("updated: " + "; ".join(changed))


if __name__ == "__main__":
    main()
