#!/usr/bin/env bash
# ============================================================================
# check_dsd_rates.sh — self-check for which DSD input rates crete accepts,
# synthetic oracle only.  No reference corpus, no MAAT, no harness.
#
# THE RULE.  libdsddpcm decodes the 44.1 kHz family only: DSD64/128/256/512 =
# 2.8224 MHz x 1/2/4/8, exactly.  Everything else is refused with a non-zero
# exit -- including the 48 kHz family (3.072 MHz x 1/2/4/8), which some ADCs
# and recorders really do write, and which the error must name as such.
#
# The trap this guards: the rate used to be mapped by integer division
# (rate / 44100), so a header claiming 2.85 MHz -- or 2822401 Hz -- was taken
# as DSD64 and metered at the wrong rate, exit 0, with a duration wrong by the
# ratio.
#
# THE FIXTURE.  1 s mono files of the DSD idle pattern (0x69), written here,
# as both DSF and DFF (the two readers parse the rate separately).
#
#   1. DSD64/128/256/512 at their exact rates: accepted (DSF and DFF).
#   2. 48 kHz-family DSD64/128/256/512: refused, and the message says
#      "48 kHz-family".
#   3. Near misses (2822401, 2850000, 2822399 Hz): refused.
#
# Usage: scripts/check_dsd_rates.sh [path-to-crete]     (default ./crete)
# ============================================================================
set -euo pipefail

CRETE="${1:-./crete}"
[ -x "$CRETE" ] || { echo "no crete binary at $CRETE — run 'make' first"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

RATES_OK="2822400 5644800 11289600 22579200"
RATES_48K="3072000 6144000 12288000 24576000"
RATES_NEAR="2822401 2850000 2822399"

python3 - "$TMP" $RATES_OK $RATES_48K $RATES_NEAR <<'PY'
import struct, sys
out, rates = sys.argv[1], [int(r) for r in sys.argv[2:]]
BLK = 4096
def dsf(path, fs):
    n = fs                                   # 1 s, mono
    nbytes = (n + 7) // 8
    data = b"\x69" * nbytes + b"\x00" * ((-nbytes) % BLK)
    fmt = struct.pack("<4sQIIIIIIQII", b"fmt ", 52, 1, 0, 1, 1, fs, 1, n, BLK, 0)
    body = fmt + struct.pack("<4sQ", b"data", 12 + len(data)) + data
    open(path, "wb").write(struct.pack("<4sQQQ", b"DSD ", 28, 28 + len(body), 0) + body)
def dff(path, fs):
    def ck(i, b): return i + struct.pack(">Q", len(b)) + b + (b"\0" if len(b) % 2 else b"")
    prop = b"SND " + ck(b"FS  ", struct.pack(">I", fs)) \
         + ck(b"CHNL", struct.pack(">H", 1) + b"C   ") \
         + ck(b"CMPR", b"DSD " + b"\x0eNot Compressed\0")
    body = b"DSD " + ck(b"FVER", struct.pack(">I", 0x01050000)) + ck(b"PROP", prop) \
         + ck(b"DSD ", b"\x69" * (fs // 8))
    open(path, "wb").write(b"FRM8" + struct.pack(">Q", len(body)) + body)
for fs in rates:
    dsf(f"{out}/r{fs}.dsf", fs)
    dff(f"{out}/r{fs}.dff", fs)
PY

fail=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fail=1; }

# run <file> -> sets RC and ERR
run() { ERR="$("$CRETE" -f detail "$1" 2>&1 >/dev/null)" && RC=0 || RC=$?; }

echo "check_dsd_rates: $CRETE"

for fs in $RATES_OK; do
  for ext in dsf dff; do
    run "$TMP/r$fs.$ext"
    [ "$RC" -eq 0 ] && ok "1. $fs Hz .$ext accepted" \
                    || bad "1. $fs Hz .$ext refused (exit $RC): ${ERR##*$'\n'}"
  done
done

for fs in $RATES_48K; do
  for ext in dsf dff; do
    run "$TMP/r$fs.$ext"
    if [ "$RC" -eq 0 ]; then bad "2. $fs Hz .$ext ACCEPTED (48 kHz family must be refused)"
    elif printf '%s' "$ERR" | grep -q "48 kHz-family"; then ok "2. $fs Hz .$ext refused as 48 kHz-family"
    else bad "2. $fs Hz .$ext refused, but the message does not name the 48 kHz family: ${ERR##*$'\n'}"
    fi
  done
done

for fs in $RATES_NEAR; do
  for ext in dsf dff; do
    run "$TMP/r$fs.$ext"
    [ "$RC" -ne 0 ] && ok "3. $fs Hz .$ext refused" \
                    || bad "3. $fs Hz .$ext ACCEPTED (not an exact 44.1 kHz multiple)"
  done
done

[ "$fail" -eq 0 ] && echo "check_dsd_rates: PASS" || { echo "check_dsd_rates: FAIL"; exit 1; }
