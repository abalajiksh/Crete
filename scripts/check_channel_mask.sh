#!/usr/bin/env bash
# ============================================================================
# check_channel_mask.sh — self-check for speaker-mask layouts (v0.21.0),
# synthetic oracle only.  No reference corpus, no MAAT, no harness repo.
#
# WHY THIS EXISTS.  Up to 0.20.0 channels.hpp described a file by its channel
# COUNT alone.  An Auro-3D 8.0 file (L R Ls Rs HL HR HLs HRs, mask 0x2D603) was
# therefore called 7.1: its Rs was taken for the LFE and left out of DR and
# loudness, and its heights were weighted as surrounds.  An Auro 9.1 (10 ch)
# fell to "10 CH": LFE scored, everything summed at unity.  crete now reads
# the WAVE_FORMAT_EXTENSIBLE dwChannelMask (and the FLAC tag, and FFmpeg's
# layout) and describes the file by its speakers -- but only where the mask
# disagrees with the count table, so no file the table already describes
# can move.
#
# THE FIXTURE.  The same 997 Hz tone at -20 dBFS in every channel of every
# file, 24-bit WAVE_FORMAT_EXTENSIBLE.  Identical channels make BS.1770
# loudness pure arithmetic: integrated = stereo + 10*log10(sum(G) / 2).
#
#   1. 8 ch, mask 0x2D603 (Auro 8.0): layout 4.0.4, all 8 scored, loudness
#      from sum(G) = L R + 2 x 1.41 (Ls Rs) + 4 heights at unity = 8.82.
#   2. 10 ch, mask 0x2D60F (Auro 9.1): layout 5.1.4, LFE excluded (9 scored),
#      sum(G) = 3 + 2.82 + 4 = 9.82.
#   3. A mask that AGREES with the count table changes nothing: 7.1 with
#      0x63F and 5.1 with 0x60F (side pair) print byte-for-byte what the same
#      audio with a plain PCM fmt chunk prints.
#   4. --channel-mask ignore on the Auro 8.0 file prints byte-for-byte what
#      the plain 8 ch file prints, i.e. reproduces 0.20.0.
#   5. THE FIXTURE'S OWN PREMISE: the Auro 8.0 file must read differently
#      with the mask than without it (scored count 8 vs 7, loudness 0.09 dB
#      apart).  Without this, a fixture both readings agreed on would pass
#      2-4 trivially and gate nothing.
#   6. Auro naming (0.23.0): the masks only Auro uses are named, the two it
#      shares with standard layouts are not.  0x2FE3F (14 ch) reads
#      "7.1.6 · ... · Auro-3D 13.1 layout"; 0x2D63F (12 ch, Auro's 7.1 + 4H
#      form) is speaker for speaker 7.1.4 and must carry no Auro name, as
#      case 2 shows for 5.1.4.  JSON names it as layout_name (when built
#      with JSON).
#
# Usage: scripts/check_channel_mask.sh [path-to-crete]     (default ./crete)
#
# Invariant to --dr-mean, --dr-blocks and --dr-lfe at the loudness checks
# (BS.1770 weights do not depend on them); the scored counts assume the
# default --dr-lfe exclude.  Run it after touching channels.hpp or any
# decoder's channel_mask.
# ============================================================================
set -euo pipefail

CRETE="${1:-./crete}"
[ -x "$CRETE" ] || { echo "no crete binary at $CRETE — run 'make' first"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

python3 - "$TMP" <<'PY'
import struct, math, sys
SR, SECS = 48000, 12
a = 10 ** (-20/20.0)
def s24(x):
    v = max(-8388608, min(8388607, int(round(x * 8388607))))
    return struct.pack('<i', v & 0xFFFFFF)[:3]
mono = [s24(a * math.sin(2*math.pi*997*i/SR)) for i in range(SR * SECS)]
GUID = bytes.fromhex('0100000000001000800000aa00389b71')   # KSDATAFORMAT_SUBTYPE_PCM
def write(name, nch, mask):
    data = b''.join(s * nch for s in mono)
    if mask is None:
        fmt = struct.pack('<HHIIHH', 1, nch, SR, SR*nch*3, nch*3, 24)
    else:
        fmt = struct.pack('<HHIIHHHHI', 0xFFFE, nch, SR, SR*nch*3, nch*3, 24,
                          22, 24, mask) + GUID
    body = (b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt +
            b'data' + struct.pack('<I', len(data)) + data)
    open(sys.argv[1] + '/' + name, 'wb').write(b'RIFF' + struct.pack('<I', len(body)) + body)
write('stereo.wav',      2,  None)
write('plain_8.wav',     8,  None)
write('plain_6.wav',     6,  None)
write('m71_63F.wav',     8,  0x63F)
write('m51_60F.wav',     6,  0x60F)
write('auro80.wav',      8,  0x2D603)
write('auro91.wav',      10, 0x2D60F)
write('auro131.wav',     14, 0x2FE3F)
write('m714.wav',        12, 0x2D63F)
PY

# The per-file detail report without the lines that name the file or the run.
report() {   # report <file> [extra crete flags...]
    local f="$1"; shift
    "$CRETE" -f detail "$@" "$TMP/$f" 2>/dev/null |
        grep -v -e '^log date:' -e '^Analyzed:' -e '^Track 1/1:'
}
field() {    # field <file> <awk-regex> [flags...]: first number on that line
    local f="$1" re="$2"; shift 2
    report "$f" "$@" | awk -v re="$re" '$0 ~ re {
        for (i = NF; i > 0; i--) if ($i ~ /^-?[0-9]+\.[0-9]+$/) { print $i; exit } }'
}
scored() {   # scored <file> [flags...]
    report "$@" | sed -n 's/.*mean of the \([0-9]*\) scored channel.*/\1/p'
}
layout() { report "$1" | sed -n 's/^  Layout: *//p'; }

fail=0
pass() { echo "PASS  $*"; }
flunk() { echo "FAIL  $*"; fail=1; }

ref=$(field stereo.wav '^  Integrated')
[ -n "$ref" ] || { echo "FAIL  could not read the stereo reference"; exit 1; }

expect_lufs() {   # expect_lufs <file> <sum-of-weights> <label>
    local got exp
    got=$(field "$1" '^  Integrated')
    exp=$(awk -v r="$ref" -v g="$2" 'BEGIN{printf "%.2f", r + 10*log(g/2)/log(10)}')
    if awk -v a="$got" -v b="$exp" 'BEGIN{d=a-b; exit !((d<0?-d:d) <= 0.015)}'; then
        pass "$3: integrated $got LUFS = stereo $ref + 10log10($2/2) = $exp"
    else
        flunk "$3: integrated $got LUFS, expected $exp (sum of BS.1770 weights $2)"
    fi
}

# 1. Auro 8.0
lay=$(layout auro80.wav)
[ "$lay" = "4.0.4 · L R Ls Rs HL HR HLs HRs · Auro-3D 8.0 layout" ] \
    && pass "Auro 8.0 layout: $lay" || flunk "Auro 8.0 layout: '$lay'"
n=$(scored auro80.wav)
[ "$n" = 8 ] && pass "Auro 8.0: all 8 channels scored (no LFE)" \
             || flunk "Auro 8.0: $n scored, expected 8 — a channel was taken for an LFE"
expect_lufs auro80.wav 8.82 "Auro 8.0"

# 2. Auro 9.1
lay=$(layout auro91.wav)
[ "$lay" = "5.1.4 · L R C LFE Ls Rs HL HR HLs HRs" ] \
    && pass "Auro 9.1 layout: $lay" || flunk "Auro 9.1 layout: '$lay'"
n=$(scored auro91.wav)
[ "$n" = 9 ] && pass "Auro 9.1: 9 scored, LFE excluded" \
             || flunk "Auro 9.1: $n scored, expected 9"
expect_lufs auro91.wav 9.82 "Auro 9.1"

# 6. Auro naming: distinctive masks only.
lay=$(layout auro131.wav)
[ "$lay" = "7.1.6 · L R C LFE Lrs Rrs Lss Rss T HL HC HR HLs HRs · Auro-3D 13.1 layout" ] \
    && pass "Auro 13.1 layout: $lay" || flunk "Auro 13.1 layout: '$lay'"
lay=$(layout m714.wav)
[ "$lay" = "7.1.4 · L R C LFE Lrs Rrs Lss Rss HL HR HLs HRs" ] \
    && pass "7.1.4 (0x2D63F) carries no Auro name: $lay" || flunk "7.1.4 (0x2D63F) layout: '$lay'"
if "$CRETE" -f json "$TMP/auro131.wav" >/dev/null 2>&1; then
    jn=$("$CRETE" -f json "$TMP/auro131.wav" 2>/dev/null | python3 -c \
        'import json,sys; d=json.load(sys.stdin); print(d.get("layout_name"), d["tracks"][0].get("layout_name"))')
    [ "$jn" = "Auro-3D 13.1 Auro-3D 13.1" ] && pass "JSON layout_name: $jn" \
                                             || flunk "JSON layout_name: '$jn'"
    jn=$("$CRETE" -f json "$TMP/m714.wav" 2>/dev/null | python3 -c \
        'import json,sys; d=json.load(sys.stdin); print(d.get("layout_name"), d["tracks"][0].get("layout_name"))')
    [ "$jn" = "None None" ] && pass "JSON: 7.1.4 has no layout_name" \
                            || flunk "JSON: 7.1.4 layout_name '$jn'"
fi

# 3. A mask that agrees with the count table moves nothing.
cmp -s <(report m71_63F.wav) <(report plain_8.wav) \
    && pass "7.1 mask 0x63F reads byte-identical to plain 8 ch" \
    || flunk "7.1 mask 0x63F differs from plain 8 ch — an agreeing mask changed the output"
cmp -s <(report m51_60F.wav) <(report plain_6.wav) \
    && pass "5.1(side) mask 0x60F reads byte-identical to plain 6 ch" \
    || flunk "5.1(side) mask 0x60F differs from plain 6 ch — an agreeing mask changed the output"

# 4. --channel-mask ignore reproduces the count-only reading.
cmp -s <(report auro80.wav --channel-mask ignore) <(report plain_8.wav) \
    && pass "--channel-mask ignore: Auro 8.0 reads byte-identical to plain 8 ch" \
    || flunk "--channel-mask ignore does not reproduce the count-only reading"

# 5. The premise: the mask must actually change this fixture's reading.
n_ign=$(scored auro80.wav --channel-mask ignore)
i_use=$(field auro80.wav '^  Integrated')
i_ign=$(field auro80.wav '^  Integrated' --channel-mask ignore)
if [ "$n_ign" = 7 ] && [ "$i_use" != "$i_ign" ]; then
    pass "fixture premise: Auro 8.0 reads 8 scored / $i_use LUFS by mask, 7 / $i_ign by count"
else
    flunk "fixture premise: by count $n_ign scored / $i_ign LUFS, by mask $i_use — the mask changed nothing, this fixture gates nothing"
fi

[ "$fail" -eq 0 ] && echo "ALL CHECKS PASSED" || echo "CHECKS FAILED"
exit "$fail"
