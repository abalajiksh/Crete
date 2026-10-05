#!/usr/bin/env bash
# ============================================================================
# check_dsd_bw.sh — self-check for --dsd-bandwidth (libdsddpcm 0.2.2's output
# band limit), synthetic oracle only.  No reference corpus, no MAAT, no harness.
#
# THE RULE.  Above 48 kHz output the halving cascade alone keeps the DSD
# modulator's shaped ultrasonic noise, which inflates RMS, peak and DR.  The
# default (50 kHz, Scarlet Book Annex D.1) removes it; `full` keeps it, which is
# exactly what crete <= 0.21 / libdsddpcm 0.2.1 measured.  The filter can only
# act at 176.4 and 352.8 kHz: at 44.1 kHz the final stage already brickwalls,
# and at 88.2 kHz the 50 kHz corner is above Nyquist.
#
# THE FIXTURE.  A DSD64 .dsf written here by a plain 2nd-order sigma-delta
# modulator: a 1 kHz tone at -12 dBFS plus a 100 kHz tone at -9 dBFS, mono,
# 2 s.  The 100 kHz tone stands in for the shaped ultrasonic noise real DSD
# carries (a 2nd-order modulator's own noise is far too low below ~150 kHz to
# test anything -- the premise check below caught exactly that).  It sits
# above the default filter's 70 kHz stopband, so full should read ~4.7 dB
# more RMS than default at 352.8 kHz.
#
#   1. 44.1 kHz: default and full are byte-identical (output text compared).
#   2. 88.2 kHz: the same.
#   3. 352.8 kHz: default reads at least 1 dB lower RMS than full.
#   4. Default is rate-independent: 176.4 and 352.8 kHz RMS agree within
#      0.05 dB (with full they do not -- that is what the band limit is for).
#   5. Bad values are rejected (19999 Hz, junk) with a non-zero exit.
#   6. THE FIXTURE'S OWN PREMISE: under full, 352.8 kHz reads at least 3 dB
#      more RMS than 44.1 kHz.  Without that the fixture carries no ultrasonic
#      noise and checks 1-4 would pass while gating nothing.
#
# Usage: scripts/check_dsd_bw.sh [path-to-crete]     (default ./crete)
# ============================================================================
set -euo pipefail

CRETE="${1:-./crete}"
[ -x "$CRETE" ] || { echo "no crete binary at $CRETE — run 'make' first"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
DSF="$TMP/sdm2_1k.dsf"

python3 - "$DSF" <<'PY'
import math, struct, sys
path = sys.argv[1]
FS = 2822400                       # DSD64
N = 2 * FS                         # 2 s, mono
A1, F1 = 0.25, 1000.0              # -12 dBFS
A2, F2 = 0.355, 100000.0           # -9 dBFS, ultrasonic
w1, w2 = 2 * math.pi * F1 / FS, 2 * math.pi * F2 / FS
i1 = i2 = 0.0
v = 1.0
bits = bytearray((N + 7) // 8)
for n in range(N):                 # 2nd-order CIFB modulator
    x = A1 * math.sin(w1 * n) + A2 * math.sin(w2 * n)
    i1 += x - v
    i2 += i1 - v
    v = 1.0 if i2 >= 0.0 else -1.0
    if v > 0:
        bits[n >> 3] |= 1 << (n & 7)   # DSF is LSB-first
BLK = 4096
data = bytes(bits) + bytes((-len(bits)) % BLK)
fmt = struct.pack("<4sQIIIIIIQII", b"fmt ", 52, 1, 0, 1, 1, FS, 1, N, BLK, 0)
body = fmt + struct.pack("<4sQ", b"data", 12 + len(data)) + data
with open(path, "wb") as f:
    f.write(struct.pack("<4sQQQ", b"DSD ", 28, 28 + len(body), 0) + body)
PY

fail=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fail=1; }

# Detail output without the timestamp line, for byte comparisons.
detail() { "$CRETE" -f detail "$@" "$DSF" 2>/dev/null | grep -v '^log date:'; }
# Joint RMS (dB): the last value before the unit on the detail table's RMS
# row (one value column for mono, three for stereo).
rms() { detail "$@" | awk '$1 == "RMS" { print $(NF-1); exit }'; }
# Numeric comparison that FAILS when either value is missing or not a number.
cmp() { awk -v a="$1" -v b="$2" "BEGIN { if (a !~ /^-?[0-9.]+\$/ || b !~ /^-?[0-9.]+\$/) exit 1; $3 }"; }

echo "check_dsd_bw: $CRETE"

for rate in 44100 88200; do
    if [ "$(detail --dsd-out-rate $rate)" = "$(detail --dsd-out-rate $rate --dsd-bandwidth full)" ]; then
        ok "$rate Hz: default == full (filter cannot act)"
    else
        bad "$rate Hz: default and full differ"
    fi
done

r44_full=$(rms --dsd-out-rate 44100 --dsd-bandwidth full)
r176_def=$(rms --dsd-out-rate 176400)
r176_full=$(rms --dsd-out-rate 176400 --dsd-bandwidth full)
r352_def=$(rms --dsd-out-rate 352800)
r352_full=$(rms --dsd-out-rate 352800 --dsd-bandwidth full)
echo "  RMS  44.1k full $r44_full | 176.4k default $r176_def full $r176_full | 352.8k default $r352_def full $r352_full"

cmp "$r352_full" "$r44_full" 'exit !(a - b >= 3.0)' \
    && ok "premise: full 352.8k reads >= 3 dB above 44.1k (fixture carries ultrasonic noise)" \
    || bad "premise: full 352.8k is not >= 3 dB above 44.1k — the fixture tests nothing"
cmp "$r352_full" "$r352_def" 'exit !(a - b >= 1.0)' \
    && ok "352.8k: default reads >= 1 dB below full" \
    || bad "352.8k: default is not >= 1 dB below full"
cmp "$r352_def" "$r176_def" 'd = a - b; if (d < 0) d = -d; exit !(d <= 0.05)' \
    && ok "default is rate-independent: 176.4k vs 352.8k within 0.05 dB" \
    || bad "default 176.4k vs 352.8k differ by more than 0.05 dB"

for v in 19999 junk; do
    if "$CRETE" --dsd-bandwidth "$v" "$DSF" >/dev/null 2>&1; then
        bad "--dsd-bandwidth $v was accepted"
    else
        ok "--dsd-bandwidth $v rejected"
    fi
done

[ "$fail" -eq 0 ] && echo "PASS" || { echo "FAILED"; exit 1; }
