#!/usr/bin/env python3
"""obs_wait_release.py -- wait until an OBS package has built AND published a version.

    scripts/obs_wait_release.py [--osc-config FILE] [--timeout MIN] PROJECT PACKAGE VERSION

Polls OBS through `osc api` (so it uses osc's authentication) until every
enabled repository/arch of PACKAGE is:

  * status "succeeded" and the repository not dirty,
  * carrying binaries whose file names contain VERSION -- the proof that the
    success belongs to THIS release and not the previous revision, which OBS
    still reports as "succeeded" for the seconds before it notices the new
    sources,
  * in repository state "published", i.e. actually downloadable by users.

Exits 0 when all are, 1 as soon as any build fails / is unresolvable / broken
(printing the repositories concerned and the build-log command), 2 on timeout.
"disabled" and "excluded" repositories are ignored.
"""

import argparse
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

API = "https://api.opensuse.org"
FAILED = {"failed", "unresolvable", "broken"}
IGNORED = {"disabled", "excluded"}


def osc_api(cfg, path):
    cmd = ["osc"] + (["--config", cfg] if cfg else []) + ["-A", API, "api", path]
    return ET.fromstring(subprocess.run(cmd, check=True, capture_output=True, text=True).stdout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--osc-config")
    ap.add_argument("--timeout", type=int, default=150, help="minutes (default 150)")
    ap.add_argument("--interval", type=int, default=30, help="seconds between polls")
    ap.add_argument("project")
    ap.add_argument("package")
    ap.add_argument("version")
    a = ap.parse_args()

    deadline = time.time() + a.timeout * 60
    last = None
    while True:
        try:
            res = osc_api(a.osc_config, f"/build/{a.project}/_result?package={a.package}")
        except subprocess.CalledProcessError as e:  # transient API error: keep waiting
            print(f"osc api failed ({e.stderr.strip()[:200]}); retrying", flush=True)
            res = None

        if res is not None:
            pending, failed, done = [], [], []
            for r in res.findall("result"):
                where = f"{r.get('repository')}/{r.get('arch')}"
                st = r.find("status")
                code = st.get("code") if st is not None else "unknown"
                if code in IGNORED:
                    continue
                if code in FAILED:
                    failed.append(f"{where}: {code}")
                elif code != "succeeded" or r.get("dirty") == "true":
                    pending.append(f"{where}: {code}{' (dirty)' if r.get('dirty') else ''}")
                else:
                    bins = osc_api(a.osc_config, f"/build/{a.project}/{r.get('repository')}"
                                                 f"/{r.get('arch')}/{a.package}")
                    names = [b.get("filename") for b in bins.findall("binary")]
                    if not any(a.version in n for n in names if not n.startswith("_")):
                        pending.append(f"{where}: succeeded, but not yet as {a.version}")
                    elif r.get("state") != "published":
                        pending.append(f"{where}: built, repository {r.get('state')}")
                    else:
                        done.append(where)

            if failed:
                print("FAILED:\n  " + "\n  ".join(failed))
                print(f"Logs: osc -A {API} remotebuildlog {a.project} {a.package} <repo> <arch>")
                return 1
            summary = f"{len(done)} published, {len(pending)} pending"
            if summary != last:
                print(summary + ("".join(f"\n  {p}" for p in pending) if pending else ""), flush=True)
                last = summary
            if done and not pending:
                print(f"All {len(done)} repositories published {a.package} {a.version}.")
                return 0

        if time.time() > deadline:
            print(f"TIMEOUT after {a.timeout} min; still pending as listed above.")
            return 2
        time.sleep(a.interval)


if __name__ == "__main__":
    sys.exit(main())
