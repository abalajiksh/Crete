#!/usr/bin/env bash
# make_sacd_corpus.sh — package the SACD test corpus (sacd.zip).
#
# Unlike the integrity corpus, this one is NOT synthetic: it is cut from a real
# commercial disc, so it is built once and uploaded rather than regenerated on
# each machine. What makes it trustworthy is not reproducibility but the
# builder's --verify mode: every kept track is measured in the reduced image
# and in the 4 GB source, and the two JSON results must be identical.
#
# Usage: scripts/make_sacd_corpus.sh <source.iso> [dest] [--no-verify]
set -euo pipefail

SRC="${1:?usage: make_sacd_corpus.sh <source.iso> [dest] [--no-verify]}"
DEST="${2:-build/corpus}"
VERIFY=1
for a in "$@"; do [ "$a" = "--no-verify" ] && VERIFY=0; done

ROOT="$DEST/sacd"
rm -rf "$ROOT" "$DEST/sacd.zip"
mkdir -p "$ROOT"

CRETE="${CRETE_BIN:-./crete}"
[ -x "$CRETE" ] || { echo "need a crete binary with JSON (make cli-json), or set CRETE_BIN" >&2; exit 1; }

# ── 1. The reduced image ───────────────────────────────────────────────────
# 2 stereo tracks (uncompressed DSD) + 1 multichannel track (DST). Two stereo
# tracks so a track BOUNDARY and an album aggregate are exercised; one
# multichannel track because DST is ~2 GB per area on the source and the
# decoder is what needs covering, not the track count.
ARGS=(--stereo-tracks 2 --mc-tracks 1)
[ "$VERIFY" = 1 ] && ARGS+=(--verify "$CRETE")
python3 scripts/make_sacd_test_image.py "$SRC" "$ROOT/dsotm_reduced.iso" "${ARGS[@]}"

# ── 2. Negative control ────────────────────────────────────────────────────
# crete dispatches on CONTENT (SACDMTOC at LSN 510), never on the .iso
# extension -- that extension names every disc image ever made. This is what
# proves it: an ISO9660 data disc that must be refused, not metered.
python3 - "$ROOT/not_an_sacd.iso" <<'PY'
import random, sys
random.seed(42)
S = 2048
with open(sys.argv[1], 'wb') as f:
    for lsn in range(700):                      # past LSN 510, deliberately
        if lsn == 16:                           # ISO9660 Primary Volume Descriptor
            b = bytearray(S); b[0] = 1; b[1:6] = b'CD001'; b[6] = 1
            b[40:72] = b'CRETE NEGATIVE CONTROL          '
            f.write(bytes(b))
        else:
            f.write(bytes(random.getrandbits(8) for _ in range(64)) + bytes(S - 64))
PY

# ── 3. Manifest ────────────────────────────────────────────────────────────
{
  printf 'file\tarea\tcontents\texpected\n'
  printf 'dsotm_reduced.iso\tstereo\t2 tracks, uncompressed DSD64\texit 0, STEREO, 68.4s + 169.0s\n'
  printf 'dsotm_reduced.iso\tmultichannel\t1 track, DST-compressed\texit 0, 5.1, 6 ch, 5 scored\n'
  printf 'not_an_sacd.iso\t-\tISO9660 data disc\texit 1, refused by content\n'
} > "$ROOT/MANIFEST.tsv"

# ── 4. Deterministic zip ───────────────────────────────────────────────────
find "$ROOT" -exec touch -t 202601010000.00 {} +
( cd "$DEST" && find sacd -type f | LC_ALL=C sort | zip -qX sacd.zip -@ )

echo
echo "zip:    $DEST/sacd.zip  ($(du -h "$DEST/sacd.zip" | cut -f1))"
echo "sha256: $(shasum -a 256 "$DEST/sacd.zip" | cut -d' ' -f1)"
echo
echo "After uploading, drop 'optional: true' from the SACD entry in Jenkinsfile.weeklyRuns."
