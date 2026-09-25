#!/usr/bin/env python3
"""Create (if needed) the Codeberg release for a tag and attach files to it.

    CODEBERG_TOKEN=... scripts/codeberg_release.py --repo abksh/Crete \
        --tag v0.19.6 --notes-file notes.txt dist/*.zip dist/*.sha256

Used by packaging/Jenkinsfile.release. Standard library only (runs on any
agent with python3).

Idempotent, like the rest of the release job:
  - the release is created only if the tag has none; an existing release's
    title and notes are left alone (a human may have edited them);
  - an asset already attached under the same name is downloaded and compared
    byte for byte: identical -> left alone, different -> replaced.

--dry-run makes only unauthenticated GET requests and prints what it would
do; it needs no token. The token is read from the environment, never from
the command line (which would put it in the process list and the job log),
and is never printed.

Forgejo API (Codeberg): GET  /repos/{repo}/releases/tags/{tag}
                        POST /repos/{repo}/releases
                        POST /repos/{repo}/releases/{id}/assets?name=...
                        DELETE /repos/{repo}/releases/{id}/assets/{asset_id}
"""

import argparse
import hashlib
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid

API = "https://codeberg.org/api/v1"


def request(method, url, token=None, data=None, headers=None, expect=(200,)):
    hdrs = {"Accept": "application/json", "User-Agent": "crete-release"}
    if token:
        hdrs["Authorization"] = "token " + token
    hdrs.update(headers or {})
    req = urllib.request.Request(url, data=data, method=method, headers=hdrs)
    try:
        with urllib.request.urlopen(req, timeout=300) as r:
            body = r.read()
            status = r.status
    except urllib.error.HTTPError as e:
        body, status = e.read(), e.code
    if status not in expect:
        detail = body[:500].decode("utf-8", "replace")
        raise SystemExit(f"ERROR: {method} {url} -> HTTP {status}: {detail}")
    return status, body


def get_json(url, token=None, expect=(200,)):
    status, body = request("GET", url, token, expect=expect)
    return status, (json.loads(body) if body else None)


def multipart(field, filename, payload):
    boundary = "----crete-" + uuid.uuid4().hex
    head = (f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="{field}"; filename="{filename}"\r\n'
            f"Content-Type: application/octet-stream\r\n\r\n").encode()
    tail = f"\r\n--{boundary}--\r\n".encode()
    return boundary, head + payload + tail


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--repo", required=True, help="owner/name, e.g. abksh/Crete")
    ap.add_argument("--tag", required=True)
    ap.add_argument("--notes-file", help="release notes; first line is the title")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--api", default=API, help="API base URL (default: Codeberg; override for testing)")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()

    token = os.environ.get("CODEBERG_TOKEN", "")
    if not a.dry_run and not token:
        raise SystemExit("ERROR: CODEBERG_TOKEN is not set")
    auth = None if a.dry_run else token
    base = f"{a.api.rstrip('/')}/repos/{a.repo}"
    q = urllib.parse.quote

    for f in a.files:
        if not os.path.isfile(f):
            raise SystemExit(f"ERROR: no such file: {f}")

    notes = ""
    if a.notes_file:
        with open(a.notes_file, encoding="utf-8") as fh:
            notes = fh.read().strip()
    title, _, body = notes.partition("\n")
    title = title.strip() or a.tag
    body = body.strip()

    # The tag must already be on Codeberg: a release for a missing tag would
    # make Forgejo create the tag itself, from the default branch.
    status, _ = get_json(f"{base}/tags/{q(a.tag, safe='')}", auth, expect=(200, 404))
    if status == 404:
        raise SystemExit(f"ERROR: tag {a.tag} is not on {a.repo} -- push it first")

    status, rel = get_json(f"{base}/releases/tags/{q(a.tag, safe='')}", auth, expect=(200, 404))
    if status == 404:
        print(f">> creating release {a.tag}: {title!r}")
        if a.dry_run:
            rel = {"id": None, "assets": []}
        else:
            payload = json.dumps({"tag_name": a.tag, "name": title, "body": body,
                                  "draft": False, "prerelease": False}).encode()
            code, raw = request("POST", f"{base}/releases", token, payload,
                                {"Content-Type": "application/json"}, expect=(201, 409))
            if code == 409:
                # Crete-Tag and Crete-Release both attach files to a new tag's
                # release and can race to create it. The loser uses the winner's.
                print(">> created concurrently by another job -- using that release")
                _, rel = get_json(f"{base}/releases/tags/{q(a.tag, safe='')}", auth)
            else:
                rel = json.loads(raw)
    else:
        print(f">> release {a.tag} exists (id {rel['id']}): {rel.get('name')!r} -- title/notes left as they are")

    existing = {x["name"]: x for x in rel.get("assets") or []}
    for path in a.files:
        name = os.path.basename(path)
        with open(path, "rb") as fh:
            data = fh.read()
        old = existing.get(name)
        if old:
            _, cur = request("GET", old["browser_download_url"], None, expect=(200,))
            if cur == data:
                print(f"   unchanged  {name}  ({len(data)} bytes, sha256 {sha256(data)[:16]})")
                continue
            print(f"   replace    {name}  ({old.get('size')} -> {len(data)} bytes)")
            if not a.dry_run:
                request("DELETE", f"{base}/releases/{rel['id']}/assets/{old['id']}", token,
                        expect=(204,))
        else:
            print(f"   upload     {name}  ({len(data)} bytes, sha256 {sha256(data)[:16]})")
        if a.dry_run:
            continue
        boundary, form = multipart("attachment", name, data)
        request("POST", f"{base}/releases/{rel['id']}/assets?name={q(name)}", token, form,
                {"Content-Type": f"multipart/form-data; boundary={boundary}"}, expect=(201,))

    if a.dry_run:
        print(">> DRY RUN: nothing was created or uploaded")
    else:
        print(f">> https://codeberg.org/{a.repo}/releases/tag/{q(a.tag)}")


if __name__ == "__main__":
    sys.exit(main())
