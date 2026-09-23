#!/usr/bin/env bash
# ============================================================================
# check_lfe_peak.sh — self-check for the joint true-peak LFE guard (v0.15.0),
# synthetic oracle only.  No reference corpus, no MAAT, no harness repo.
#
# WHY THIS EXISTS.  Until v0.15.0 `peak_dbfs` (joint SAMPLE peak) honoured the
# LFE scoring policy while `max_true_peak_dbtp` did not: analyze_track's
# joint_tp loop had no is_lfe/scored guard, so the joint TRUE peak was a max
# over every channel including the LFE, and --dr-lfe could not reach it.
# `plr_db` inherits it, mixing an LFE-inclusive numerator with an
# LFE-excluded denominator.
#
# It never showed up in the corpus because the LFE has to be the LOUDEST
# channel and in real material it never is -- on DSOTM 5.1 the LFE peaks at
# -11.14 against L at -1.67, so the joint peak comes from L either way and
# the defect is invisible.  That is exactly why a synthetic is needed: the
# only thing that exercises this path is material we do not have.
#
# THE FIXTURE.  5.1, every channel at -20 dBFS, LFE at -3 dBFS.  Ordinary for
# a film or concert master, and impossible to find in this corpus.
#
# THE ORACLE IS ARITHMETIC, not a reference.  True peak is >= sample peak BY
# DEFINITION, and the excess is inter-sample overshoot -- a few hundredths of
# a dB on a band-limited tone.  A 17 dB excess is not a reading anyone can
# interpret; it is a channel leaking in.  So:
#
#   1. |joint true peak - joint sample peak| <= 0.5 dB at the DEFAULT setting.
#      v0.14.0: TP -2.99 against SP -20.00, a 17.01 dB gap.  v0.15.0+: 0.01.
#   2. The same bound under `--dr-lfe include`.  Include-mode IS the old
#      unconditional behaviour, so this pins the path that must stay
#      bit-compatible with every number recorded before v0.15.0.
#   3. THE FIXTURE'S OWN PREMISE: joint sample peak must move ~17 dB between
#      the two settings.  Without this a fixture whose LFE stopped dominating
#      would satisfy 1 and 2 trivially and gate nothing -- the failure mode
#      this project has shipped twice (BoneyM at zero comparisons, DTSX at
#      1 track of 12).
#
# Usage: scripts/check_lfe_peak.sh [path-to-crete]     (default ./crete)
#        CRETE_FLAGS="--dr-blocks legacy" scripts/check_lfe_peak.sh
#
# Invariant to --dr-mean and --dr-blocks by construction: it pins peaks, which
# no averaging or block-set choice touches.  Run it after anything that moves
# analyze_track's joint loops or chan::is_lfe.
# ============================================================================
set -euo pipefail

CRETE="${1:-./crete}"
CRETE_FLAGS="${CRETE_FLAGS:-}"
[ -x "$CRETE" ] || { echo "no crete binary at $CRETE — run 'make' first"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# 5.1 in WAVE order: L R C LFE Ls Rs.  24-bit integer so the peak is exact and
# the `peak > 0 dBFS` integrity check cannot fire on it.  997 Hz mains (the
# standard non-harmonic test tone) and 55 Hz in the LFE, which is where an LFE
# actually lives.
python3 - "$TMP" <<'PY'
import wave, struct, math, sys
SR, SECS = 48000, 20
a_main, a_lfe = 10 ** (-20/20.0), 10 ** (-3/20.0)
f = wave.open(sys.argv[1] + "/lfe_dominant_5_1.wav", 'wb')
f.setnchannels(6); f.setsampwidth(3); f.setframerate(SR)
def s24(x):
    v = max(-8388608, min(8388607, int(round(x * 8388607))))
    return struct.pack('<i', v & 0xFFFFFF)[:3]
fr = bytearray()
for i in range(SR * SECS):
    t = i / SR
    m = a_main * math.sin(2*math.pi*997*t)
    l = a_lfe  * math.sin(2*math.pi*55*t)
    for ch in (m, m, m, l, m, m):
        fr += s24(ch)
f.writeframes(bytes(fr)); f.close()
PY

FIX="$TMP/lfe_dominant_5_1.wav"

# Last numeric field on a labelled row is the Joint column, whatever unit
# suffix follows it.  Keyed on the label, so a new column cannot shift it.
joint() {   # joint <row-label> [extra crete flags...]
    local lbl="$1"; shift
    "$CRETE" -f detail $CRETE_FLAGS "$@" "$FIX" 2>/dev/null | awk -v lbl="$lbl" '
        $0 ~ "^  " lbl {
            for (i = NF; i > 0; i--)
                if ($i ~ /^-?[0-9]+\.[0-9]+$/) { print $i; exit }
        }'
}

fail=0

for mode in default include; do
    if [ "$mode" = include ]; then set -- --dr-lfe include; else set --; fi
    tp=$(joint "Max True Peak"   "$@")
    sp=$(joint "Max Sample Peak" "$@")
    [ -n "$tp" ] && [ -n "$sp" ] || { echo "FAIL  could not read peaks ($mode)"; fail=1; continue; }
    gap=$(awk -v a="$tp" -v b="$sp" 'BEGIN{d=a-b; print (d<0?-d:d)}')
    if awk -v g="$gap" 'BEGIN{exit !(g <= 0.5)}'; then
        echo "PASS  $mode: joint true peak $tp vs sample peak $sp  (excess ${gap} dB <= 0.5)"
    else
        echo "FAIL  $mode: joint true peak $tp vs sample peak $sp  (excess ${gap} dB > 0.5)"
        echo "      the joint true-peak loop is not honouring the LFE scoring policy"
        fail=1
    fi
    eval "sp_$mode=\$sp"
done

# The fixture's premise.  If this fails the file stopped being a dominant-LFE
# case and the two checks above proved nothing.
spread=$(awk -v a="${sp_include:-0}" -v b="${sp_default:-0}" 'BEGIN{d=a-b; print (d<0?-d:d)}')
if awk -v s="$spread" 'BEGIN{exit !(s >= 16.0 && s <= 18.0)}'; then
    echo "PASS  fixture premise: joint sample peak moves ${spread} dB between settings (expected ~17)"
else
    echo "FAIL  fixture premise: joint sample peak moved ${spread} dB, expected ~17"
    echo "      the LFE is no longer the dominant channel — this fixture gates nothing"
    fail=1
fi

[ "$fail" -eq 0 ] && echo "ALL CHECKS PASSED" || echo "CHECKS FAILED"
exit "$fail"
