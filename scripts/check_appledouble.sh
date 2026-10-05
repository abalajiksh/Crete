#!/usr/bin/env bash
# ============================================================================
# check_appledouble.sh — self-check: folder scans skip macOS AppleDouble
# sidecars ("._<name>").  Synthetic oracle only; no corpus, no harness.
#
# THE RULE.  A Mac writing to a volume without native metadata support
# (exFAT/FAT USB sticks, SMB shares) puts a "._<name>" file next to every file.
# It is never audio, even when its name ends in .dsf or .wav.  Scanning a
# folder must skip it; a sidecar named explicitly is still attempted, so the
# user who asks for it gets the real error.
#
# WHY IT MATTERED.  Re-recording the DSD references from a USB stick
# (2026-10-05): crete reported each of an album's 14 sidecars as a damaged
# track ("DSF: invalid header") and exited 2, and dsd2wav failed the album
# outright, although every real track decoded fine.
#
# THE FIXTURE.  A folder holding one real 12 s WAV plus sidecars named like a
# WAV, a DSF and a cue sheet (the three things the collector looks at).
#
#   1. crete on the folder: exit 0, exactly 1 file, no "Error:" on stderr.
#   2. THE FIXTURE'S OWN PREMISE: the sidecar named explicitly is rejected
#      (non-zero exit, "invalid header").  Without this a sidecar crete happened
#      to tolerate would let check 1 pass while gating nothing.
#   3. (with a dsd2wav path) dsd2wav on a folder holding a real 1 s DSF and its
#      sidecar: exit 0, "1 written, 0 failed".
#
# Usage: scripts/check_appledouble.sh [path-to-crete] [path-to-dsd2wav]
#        (defaults ./crete, and ./dsd2wav when it exists)
# ============================================================================
set -euo pipefail

CRETE="${1:-./crete}"
DSD2WAV="${2:-}"
[ -z "$DSD2WAV" ] && [ -x ./dsd2wav ] && DSD2WAV=./dsd2wav
[ -x "$CRETE" ] || { echo "no crete binary at $CRETE — run 'make' first"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/album" "$TMP/dsd"

python3 - "$TMP" <<'PY'
import math, struct, sys, wave
T = sys.argv[1]
SR = 44100
with wave.open(f"{T}/album/tone.wav", "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
    w.writeframes(b"".join(struct.pack("<hh", s, s) for s in
                  (int(8000 * math.sin(2 * math.pi * 997 * i / SR)) for i in range(12 * SR))))
# AppleDouble header (magic 0x00051607, version 2, "Mac OS X") + padding
sidecar = bytes.fromhex("00051607000200004d6163204f532058202020200000") + bytes(4074)
for name in ("._tone.wav", "._tone.dsf", "._album.cue"):
    open(f"{T}/album/{name}", "wb").write(sidecar)
open(f"{T}/dsd/._tone.dsf", "wb").write(sidecar)

# 1 s mono DSD64 from a 2nd-order sigma-delta modulator, LSB-first DSF
FS = 2822400; N = FS
i1 = i2 = 0.0; v = 1.0
bits = bytearray(N // 8)
for n in range(N):
    i1 += 0.5 * math.sin(2 * math.pi * 1000 * n / FS) - v
    i2 += i1 - v
    v = 1.0 if i2 >= 0.0 else -1.0
    if v > 0:
        bits[n >> 3] |= 1 << (n & 7)
data = bytes(bits) + bytes((-len(bits)) % 4096)
body = struct.pack("<4sQIIIIIIQII", b"fmt ", 52, 1, 0, 1, 1, FS, 1, N, 4096, 0) \
     + struct.pack("<4sQ", b"data", 12 + len(data)) + data
with open(f"{T}/dsd/tone.dsf", "wb") as f:
    f.write(struct.pack("<4sQQQ", b"DSD ", 28, 28 + len(body), 0) + body)
PY

fail=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fail=1; }

echo "check_appledouble: $CRETE"

set +e
"$CRETE" "$TMP/album" > "$TMP/out" 2> "$TMP/err"; rc=$?
set -e
files=$(awk -F: '/Number of Files/ { gsub(/ /, "", $2); print $2 }' "$TMP/out")
[ "$rc" -eq 0 ] && ok "folder scan exits 0" || bad "folder scan exited $rc"
[ "$files" = "1" ] && ok "folder scan measures exactly 1 file" || bad "folder scan measured '${files}' files"
grep -q "Error:" "$TMP/err" && bad "folder scan printed errors: $(grep Error: "$TMP/err" | head -1)" \
                            || ok "folder scan printed no errors"

set +e
"$CRETE" "$TMP/album/._tone.wav" > /dev/null 2> "$TMP/err2"; rc2=$?
set -e
if [ "$rc2" -ne 0 ] && grep -q "invalid header" "$TMP/err2"; then
    ok "premise: the sidecar named explicitly is rejected (exit $rc2)"
else
    bad "premise: the sidecar named explicitly was accepted (exit $rc2) — the fixture tests nothing"
fi

if [ -n "$DSD2WAV" ]; then
    set +e
    "$DSD2WAV" -o "$TMP/pcm" "$TMP/dsd" > /dev/null 2> "$TMP/err3"; rc3=$?
    set -e
    if [ "$rc3" -eq 0 ] && grep -q "1 written, 0 failed" "$TMP/err3"; then
        ok "dsd2wav skips the sidecar (1 written, 0 failed)"
    else
        bad "dsd2wav: exit $rc3, $(grep -E 'written|FAIL' "$TMP/err3" | head -2 | tr '\n' ' ')"
    fi
else
    echo "  skip  dsd2wav (no binary; pass it as the second argument)"
fi

[ "$fail" -eq 0 ] && echo "PASS" || { echo "FAILED"; exit 1; }
