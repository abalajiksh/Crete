#!/usr/bin/env bash
# ============================================================================
# check_cue_multifile.sh — self-check for multi-FILE cue sheets and dropped
# cue tracks.  Synthetic oracle only: no corpus, no harness repo.
#
# WHY THIS EXISTS.  Until 0.19.1 cue.hpp kept ONE referenced file per sheet,
# overwritten by every FILE line, so a vinyl rip with one file per side
# (Bronski Beat, The Age Of Consent: 4 FILEs, 10 tracks) sliced all ten
# INDEX times -- which restart at 0:00 on each side -- out of the LAST file.
# Result: wrong audio under every title, three tracks dropped as "empty cue
# range", the other three sides measured whole as standalone files, and
# exit 0.
#
# THE FIXTURE.  Two WAV "sides", each two 10 s tones at distinct levels, so a
# track's sample peak names the exact file and half it was cut from:
#     side A: -20 dBFS then -10 dBFS      side B: -6 dBFS then -3 dBFS
#
#   1. The two-FILE cue yields EXACTLY 4 tracks (not 4 + standalone sides),
#      with peaks -20, -10, -6, -3 in cue order, and exits 0.
#   2. A cue whose second INDEX precedes its first drops a track, and the run
#      exits 2 -- an album missing a track is not a complete measurement.
#
# Usage: scripts/check_cue_multifile.sh [path-to-crete]     (default ./crete)
# ============================================================================
set -euo pipefail

CRETE="${1:-./crete}"
[ -x "$CRETE" ] || { echo "no crete binary at $CRETE — run 'make' first"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir "$TMP/multi" "$TMP/bad"

python3 - "$TMP" <<'PY'
import wave, struct, math, sys
SR = 44100
def side(path, levels):
    frames = bytearray()
    for db in levels:
        amp = 32767 * 10 ** (db / 20)
        for n in range(SR * 10):
            v = int(round(amp * math.sin(2 * math.pi * 997 * n / SR)))
            frames += struct.pack('<hh', v, v)
    with wave.open(path, 'wb') as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(bytes(frames))
side(sys.argv[1] + '/multi/side a.wav', [-20, -10])
side(sys.argv[1] + '/multi/side b.wav', [-6, -3])
side(sys.argv[1] + '/bad/one.wav', [-20, -10])
PY

cat > "$TMP/multi/album.cue" <<'CUE'
FILE "side a.wav" WAVE
  TRACK 01 AUDIO
    TITLE "One"
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    TITLE "Two"
    INDEX 01 00:10:00
FILE "side b.wav" WAVE
  TRACK 03 AUDIO
    TITLE "Three"
    INDEX 01 00:00:00
  TRACK 04 AUDIO
    TITLE "Four"
    INDEX 01 00:10:00
CUE

cat > "$TMP/bad/album.cue" <<'CUE'
FILE "one.wav" WAVE
  TRACK 01 AUDIO
    TITLE "One"
    INDEX 01 00:10:00
  TRACK 02 AUDIO
    TITLE "Two"
    INDEX 01 00:05:00
CUE

fail=0

set +e
"$CRETE" -f detail "$TMP/multi" > "$TMP/multi.out" 2> "$TMP/multi.err"
rc=$?
"$CRETE" -f detail "$TMP/bad" > "$TMP/bad.out" 2> "$TMP/bad.err"
rc_bad=$?
set -e

tracks=$(grep -c '^Track ' "$TMP/multi.out" || true)
# joint sample peak = last numeric column of the "Max Sample Peak" row
peaks=$(awk '/Max Sample Peak/ {printf "%.0f ", $(NF-1)}' "$TMP/multi.out")

if [ "$tracks" = 4 ] && [ "$peaks" = "-20 -10 -6 -3 " ] && [ "$rc" = 0 ]; then
    echo "PASS  multi-FILE cue: 4 tracks, peaks $peaks, exit 0"
else
    echo "FAIL  multi-FILE cue: tracks=$tracks peaks='$peaks' exit=$rc (want 4, '-20 -10 -6 -3 ', 0)"
    fail=1
fi

if [ "$rc_bad" = 2 ] && grep -q 'track dropped' "$TMP/bad.err"; then
    echo "PASS  dropped cue track: exit 2 with a 'track dropped' warning"
else
    echo "FAIL  dropped cue track: exit=$rc_bad (want 2)"; cat "$TMP/bad.err"
    fail=1
fi

exit $fail
