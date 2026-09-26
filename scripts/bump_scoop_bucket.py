#!/usr/bin/env python3
"""bump_scoop_bucket.py -- point the Scoop manifest at a crete release.

    scripts/bump_scoop_bucket.py BUCKET_DIR TAG ZIP [--verify-url]

Writes bucket/crete.json in a checkout of codeberg.org/abksh/scoop-crete so it
installs TAG from the Windows zip the release job attached to TAG's Codeberg
release (packaging/Jenkinsfile.release, Windows branch).

The script owns the WHOLE manifest -- it is rendered, not patched -- so the
first release creates it and a later change of shape is one edit here.

ZIP is the local copy of the file that was uploaded. It must be named
crete-X.Y.Z-windows-x86_64.zip (a tag build; untagged Crete-Windows builds
carry -g<sha> and are refused), and must hold that same directory with
crete.exe and crete-gui.exe in it, because the manifest's extract_dir and bin
entries name them: a zip that stopped matching would install nothing usable.

--verify-url downloads the published asset and requires its sha256 to equal
the local zip's. The release job passes it on a real run, after the upload.

Prints what changed; exits 0 with "unchanged" when the manifest already says
this, so a re-run is harmless. Nothing is committed or pushed.
"""

import argparse
import hashlib
import json
import re
import sys
import urllib.request
import zipfile
from pathlib import Path

REPO = "https://codeberg.org/abksh/Crete"
ASSET = REPO + "/releases/download/v{v}/crete-{v}-windows-x86_64.zip"


def die(msg):
    sys.exit(f"bump_scoop_bucket: {msg}")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_url(url):
    req = urllib.request.Request(url, headers={"User-Agent": "crete-release"})
    h = hashlib.sha256()
    with urllib.request.urlopen(req, timeout=300) as r:
        for chunk in iter(lambda: r.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def manifest(version, sha):
    top = f"crete-{version}-windows-x86_64"
    auto_top = "crete-$version-windows-x86_64"
    return {
        "version": version,
        "description": "Dynamic-range and loudness meter (TT DR, EBU R128, true peak)",
        "homepage": REPO,
        "license": "MIT,BSD-2-Clause,LGPL-2.1-or-later,Zlib,OFL-1.1",
        "architecture": {
            "64bit": {
                "url": ASSET.format(v=version),
                "hash": sha,
                "extract_dir": top,
            }
        },
        "bin": ["crete.exe", "crete-gui.exe"],
        "shortcuts": [["crete-gui.exe", "crête"]],
        "checkver": {
            "url": "https://codeberg.org/api/v1/repos/abksh/Crete/releases/latest",
            "jsonpath": "$.tag_name",
            "regex": r"v([\d.]+)",
        },
        "autoupdate": {
            "architecture": {
                "64bit": {
                    "url": ASSET.replace("{v}", "$version"),
                    "hash": {"url": "$url.sha256"},
                    "extract_dir": auto_top,
                }
            }
        },
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("bucket")
    ap.add_argument("tag")
    ap.add_argument("zip")
    ap.add_argument("--verify-url", action="store_true",
                    help="require the published asset to hash like ZIP")
    a = ap.parse_args(argv)

    if not re.fullmatch(r"v\d+\.\d+\.\d+", a.tag):
        die(f"tag {a.tag!r} is not vX.Y.Z")
    version = a.tag[1:]
    top = f"crete-{version}-windows-x86_64"
    zp = Path(a.zip)
    if zp.name != top + ".zip":
        die(f"{zp.name} is not {top}.zip -- only a tag build of Crete-Windows can be released")

    with zipfile.ZipFile(zp) as z:
        names = set(z.namelist())
    for need in ("crete.exe", "crete-gui.exe", "fonts/"):
        if f"{top}/{need}" not in names:
            die(f"{zp.name} has no {top}/{need}; the manifest's extract_dir/bin would not match")

    sha = sha256_file(zp)
    if a.verify_url:
        url = ASSET.format(v=version)
        try:
            got = sha256_url(url)
        except OSError as e:          # URLError/HTTPError: not published (yet)
            die(f"cannot fetch {url}: {e}")
        if got != sha:
            die(f"published {url} hashes {got}, local zip {sha}")
        print(f"verified: {url} == local zip")

    path = Path(a.bucket) / "bucket" / "crete.json"
    text = json.dumps(manifest(version, sha), indent=4, ensure_ascii=False) + "\n"
    if path.exists() and path.read_text(encoding="utf-8") == text:
        print(f"unchanged: bucket/crete.json already installs {a.tag}")
        return 0
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    print(f"updated: crete {version} ({sha})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
