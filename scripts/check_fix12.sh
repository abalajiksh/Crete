#!/usr/bin/env bash
# ============================================================================
# check_fix12.sh — self-check for FIX 12 (v0.11.3), synthetic oracle only.
#
# Two assertions, both with analytically known truth — no reference corpus, no
# MAAT, no harness repo.  Run after touching compute_tt_dr() or album_dr().
#
#   1. Per-channel DR uses each channel's OWN top-20% RMS.
#      Signal: identical peak in both channels, R gated to a 25% duty cycle so
#      its RMS is exactly half of L's.  Ground truth: DR_R - DR_L = 6.02 dB.
#      Pre-FIX-12 (shared joint RMS) this collapsed to DR_L == DR_R exactly —
#      the RAM_CD2 "18. GL" 8.64/8.64 signature from Weekly #53.
#
#   2. album_dr rounds ONCE from the raw per-track DR.
#      Tracks at raw 7.6 / 7.6 / 7.1.  Double rounding gives
#      round(mean(8,8,7)) = 8; correct single rounding gives round(7.433) = 7.
#
# Usage: scripts/check_fix12.sh [path-to-crete]     (default ./crete)
#        CRETE_FLAGS="--dr-blocks reference" scripts/check_fix12.sh
#
# Both assertions are invariant to --dr-mean and --dr-blocks by construction --
# one pins a DR *difference* between two channels of the same file, the other
# pins the album rounding rule -- so they PASS in every mode. That is the point:
# they must keep passing when the measurement axes move.
# ============================================================================
set -euo pipefail

CRETE="${1:-./crete}"
CRETE_FLAGS="${CRETE_FLAGS:-}"
[ -x "$CRETE" ] || { echo "no crete binary at $CRETE — run 'make' first"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

python3 - "$TMP" <<'PY'
import wave, struct, math, sys, os
SR, SECS, AMP = 44100, 30, 0.9
out = sys.argv[1]

def write(path, gen):
    f = wave.open(path, 'wb'); f.setnchannels(2); f.setsampwidth(2); f.setframerate(SR)
    fr = bytearray()
    for i in range(SR * SECS):
        l, r = gen(i)
        fr += struct.pack('<hh', max(-32768, min(32767, int(l * 32767))),
                                 max(-32768, min(32767, int(r * 32767))))
    f.writeframes(bytes(fr)); f.close()

# 1. equal peaks, R's RMS exactly halved by a 25% duty gate
def split(i):
    s = AMP * math.sin(2 * math.pi * 997.0 * i / SR)
    return s, (s if (i % (SR // 10)) < (SR // 40) else 0.0)
write(os.path.join(out, 'split.wav'), split)

# 2. three tracks at exact raw DR values; 0.98 marker fixes peak2 in every block
os.makedirs(os.path.join(out, 'alb'), exist_ok=True)
for name, d in (('t1', 7.6), ('t2', 7.6), ('t3', 7.1)):
    A = 0.98 / (10 ** (d / 20.0))
    def tone(i, A=A):
        s = A * math.sin(2 * math.pi * 997.0 * i / SR)
        if i % (SR * 3) == 1000: s = 0.98
        return s, s
    write(os.path.join(out, 'alb', name + '.wav'), tone)
PY

fail=0

# ── 1. per-channel RMS ──────────────────────────────────────────────────────
read -r L R <<<"$("$CRETE" $CRETE_FLAGS -f detail "$TMP/split.wav" 2>/dev/null \
                  | awk '/DR \(PMF\)/{print $3, $4}')"
delta=$(python3 -c "print(f'{float('$R')-float('$L'):.2f}')")
if [ "$delta" = "6.02" ]; then
    echo "PASS  per-channel DR split: L=$L R=$R  (delta $delta dB, expected 6.02)"
else
    echo "FAIL  per-channel DR split: L=$L R=$R  (delta $delta dB, expected 6.02)"
    [ "$delta" = "0.00" ] && echo "      delta 0.00 == the pre-FIX-12 shared-joint-RMS collapse"
    fail=1
fi

# ── 2. album_dr single rounding ─────────────────────────────────────────────
adr=$("$CRETE" $CRETE_FLAGS "$TMP/alb" 2>/dev/null | grep -oiE 'Official DR Value: DR[0-9]+' | grep -oE '[0-9]+$')
if [ "$adr" = "7" ]; then
    echo "PASS  album_dr single rounding: DR$adr  (raw mean 7.433)"
else
    echo "FAIL  album_dr: got DR$adr, expected DR7"
    [ "$adr" = "8" ] && echo "      DR8 == round(mean(round(dr))), the double-rounding bug"
    fail=1
fi

exit $fail
