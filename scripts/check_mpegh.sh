#!/usr/bin/env bash
# ============================================================================
# check_mpegh.sh — self-check for MPEG-H 3D Audio identification (mpegh.hpp),
# synthetic oracle only.  No reference corpus, no harness repo.
#
# WHY THIS EXISTS.  No crete build can decode MPEG-H, so crete refuses it --
# but it refuses by NAME, from the container's own declaration, instead of
# passing FFmpeg's "unspecified number of channels" through.  This checks the
# naming and, as importantly, that nothing else is refused by mistake.
#
# THE FIXTURES are containers built here byte by byte, carrying no audio
# payload at all (the detector never reads one):
#
#   1. MP4, one 'mhm1' track with mhaC (Baseline L3, CICP 19)  -> refused,
#      named "Baseline profile, level 3 ... CICP 19 = 7.1.4, 12 channels".
#   2. The same with moov AFTER a 64-bit-size mdat                -> same.
#   3. MP4, an 'enca' entry whose sinf/frma is 'mha1' (LC L2, CICP 6)
#                                                                 -> refused, "encrypted".
#   4. MPEG-2 TS, PMT stream_type 0x2D with the MPEG-H descriptor
#      (Baseline L4, CICP 13)                                     -> refused, "22.2".
#   5. The same as 192-byte BDAV packets (.m2ts)                  -> same.
#   6. The same with the PMT padded so it SPANS TWO packets       -> same
#      (section reassembly).
#   7. MP4 with an 'mp4a' track only; TS with stream_type 0x0F only
#                                                                 -> not MPEG-H.
#   8. MP4 with mhm1 AND mp4a; TS with 0x2D AND 0x81 (AC-3)       -> not refused:
#      the other track is left to the decoder.
#
# For 7 and 8 the check is only that no MPEG-H warning appears; whether the
# file then decodes depends on the build tier and is not this script's concern.
#
# Usage: scripts/check_mpegh.sh [path-to-crete]     (default ./crete)
# Run it after touching mpegh.hpp or the input collector in groups.hpp.
# ============================================================================
set -euo pipefail

CRETE="${1:-./crete}"
[ -x "$CRETE" ] || { echo "no crete binary at $CRETE — run 'make' first"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

python3 - "$TMP" <<'PY'
import struct, sys
out = sys.argv[1]

def box(t, body): return struct.pack('>I', 8 + len(body)) + t + body
def full(t, body, v=0, f=0): return box(t, struct.pack('>I', (v << 24) | f) + body)

def audio_entry(fourcc, children=b''):
    # SampleEntry(6 reserved + data_reference_index) + AudioSampleEntry v0
    fields = b'\0' * 6 + struct.pack('>H', 1) + b'\0' * 8 + \
             struct.pack('>HHHHI', 0, 16, 0, 0, 48000 << 16)
    return box(fourcc, fields + children)

def mhac(pli, cicp):
    cfg = b'\0' * 4
    return box(b'mhaC', bytes([1, pli, cicp]) + struct.pack('>H', len(cfg)) + cfg)

def trak(entry, handler=b'soun'):
    stsd = full(b'stsd', struct.pack('>I', 1) + entry)
    hdlr = full(b'hdlr', b'\0' * 4 + handler + b'\0' * 12 + b'\0')
    return box(b'trak', box(b'mdia', hdlr + box(b'minf', box(b'stbl', stsd))))

FTYP = box(b'ftyp', b'mp42' + b'\0\0\0\0' + b'mp42isom')
def mp4(name, traks, moov_last=False):
    moov = box(b'moov', b''.join(traks))
    # mdat with a 64-bit largesize, so the top-level walk must honour it
    payload = b'\0' * 64
    mdat = struct.pack('>I', 1) + b'mdat' + struct.pack('>Q', 16 + len(payload)) + payload
    data = FTYP + (mdat + moov if moov_last else moov + mdat)
    open(f'{out}/{name}', 'wb').write(data)

mhm1 = trak(audio_entry(b'mhm1', mhac(0x12, 19)))     # Baseline L3, CICP 19
mp4('mpegh_mhm1.mp4', [mhm1])
mp4('mpegh_moov_last.mp4', [mhm1], moov_last=True)
sinf = box(b'sinf', box(b'frma', b'mha1') + full(b'schm', b'cenc' + struct.pack('>I', 0x10000)))
mp4('mpegh_enca.mp4', [trak(audio_entry(b'enca', sinf + mhac(0x0C, 6)))])   # LC L2, CICP 6
mp4a = trak(audio_entry(b'mp4a'))
mp4('plain_aac.mp4', [mp4a])
mp4('mixed_mpegh_aac.mp4', [mhm1, mp4a])

# ── MPEG-2 TS ───────────────────────────────────────────────────────────────
def crc32_mpeg(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) if crc & 0x80000000 else (crc << 1)
            crc &= 0xFFFFFFFF
    return crc

def section(table_id, ext, body):
    # syntax 1, '0', reserved 11, length; ext id; version/current; sec#, last#
    head = struct.pack('>HBBB', ext, 0xC1, 0, 0) + body
    length = len(head) + 4
    s = bytes([table_id]) + struct.pack('>H', 0xB000 | length) + head
    return s + struct.pack('>I', crc32_mpeg(s))

def packets(pid, sec):
    # pointer_field 0 at the start; continuation packets after; 0xFF stuffing
    data = b'\0' + sec
    pk, cc = [], 0
    for i in range(0, len(data), 184):
        chunk = data[i:i + 184]
        pusi = 0x40 if i == 0 else 0
        hdr = bytes([0x47, pusi | (pid >> 8), pid & 0xFF, 0x10 | (cc & 0xF)])
        pk.append(hdr + chunk + b'\xff' * (184 - len(chunk)))
        cc += 1
    return pk

PMT_PID = 0x100
PAT = section(0x00, 1, struct.pack('>HH', 1, 0xE000 | PMT_PID))

def es(stype, pid, desc=b''):
    return bytes([stype]) + struct.pack('>HH', 0xE000 | pid, 0xF000 | len(desc)) + desc

# extension descriptor 0x3F: ext tag 0x08, profile-level, interactivity(1)
# reserved(7), reserved(2) reference_channel_layout(6)
def mpegh_desc(pli, cicp):
    return bytes([0x3F, 4, 0x08, pli, 0x7F, 0xC0 | cicp])

def pmt(streams, pad=0):
    # program_info descriptors padded with a private descriptor (tag 0xFF
    # range is user private) so the section can be forced past one packet.
    pinfo = b''
    while pad > 0:
        n = min(pad, 255); pinfo += bytes([0xFE, n]) + b'\xAA' * n; pad -= n + 2
    body = struct.pack('>HH', 0xE000 | 0x1FFF, 0xF000 | len(pinfo)) + pinfo + b''.join(streams)
    return section(0x02, 1, body)

def null_packet():
    return bytes([0x47, 0x1F, 0xFF, 0x10]) + b'\xff' * 184

def ts(name, streams, pad=0, bdav=False):
    pk = packets(0, PAT) + packets(PMT_PID, pmt(streams, pad))
    pk += [null_packet()] * 8
    if bdav: pk = [b'\0\0\0\0' + p for p in pk]
    open(f'{out}/{name}', 'wb').write(b''.join(pk))

m13 = es(0x2D, 0x20, mpegh_desc(0x13, 13))             # Baseline L4, CICP 13
ts('mpegh.ts', [m13])
ts('mpegh.m2ts', [m13], bdav=True)
ts('mpegh_long_pmt.ts', [m13], pad=300)
ts('plain_aac.ts', [es(0x0F, 0x21)])
ts('mixed_mpegh_ac3.ts', [m13, es(0x81, 0x22)])
PY

fail=0
pass() { echo "  ok    $1"; }
bad()  { echo "  FAIL  $1"; fail=1; }

# All stderr for one file argument.
run() { "$CRETE" "$TMP/$1" 2>&1 >/dev/null || true; }

expect_refused() {   # file  substring...
    local f="$1"; shift
    local out; out="$(run "$f")"
    if ! grep -q "^Warning: $f is MPEG-H 3D Audio" <<<"$out"; then
        bad "$f: not refused as MPEG-H"; printf '%s\n' "$out" | sed 's/^/        /'; return
    fi
    for s in "$@"; do
        grep -qF -- "$s" <<<"$out" || { bad "$f: missing '$s'"; printf '%s\n' "$out" | sed 's/^/        /'; return; }
    done
    pass "$f refused: $*"
}

expect_not_refused() {
    local f="$1"; local out; out="$(run "$f")"
    if grep -q "MPEG-H" <<<"$out"; then
        bad "$f: refused as MPEG-H"; printf '%s\n' "$out" | sed 's/^/        /'
    else
        pass "$f not refused"
    fi
}

echo "check_mpegh: $CRETE"
expect_refused mpegh_mhm1.mp4      "in MP4 (mhm1," "Baseline profile, level 3" "CICP 19 = 7.1.4, 12 channels"
expect_refused mpegh_moov_last.mp4 "in MP4 (mhm1," "Baseline profile, level 3" "CICP 19 = 7.1.4, 12 channels"
expect_refused mpegh_enca.mp4      "(mha1, encrypted," "Low Complexity profile, level 2" "CICP 6 = 5.1, 6 channels"
expect_refused mpegh.ts            "in MPEG-2 TS (" "Baseline profile, level 4" "CICP 13 = 22.2, 24 channels"
expect_refused mpegh.m2ts          "in MPEG-2 TS (" "Baseline profile, level 4" "CICP 13 = 22.2, 24 channels"
expect_refused mpegh_long_pmt.ts   "in MPEG-2 TS (" "Baseline profile, level 4" "CICP 13 = 22.2, 24 channels"
expect_not_refused plain_aac.mp4
expect_not_refused plain_aac.ts
expect_not_refused mixed_mpegh_aac.mp4
expect_not_refused mixed_mpegh_ac3.ts

# The long-PMT fixture must really span two packets, or case 6 tests nothing.
python3 - "$TMP/mpegh_long_pmt.ts" <<'PY' || fail=1
import sys
b = open(sys.argv[1], 'rb').read()
pmt = [b[i:i+188] for i in range(0, len(b), 188) if ((b[i+1] & 0x1F) << 8 | b[i+2]) == 0x100]
print("  ok    long-PMT fixture spans %d packets" % len(pmt) if len(pmt) >= 2
      else "  FAIL  long-PMT fixture fits one packet")
sys.exit(0 if len(pmt) >= 2 else 1)
PY

[ "$fail" -eq 0 ] && echo "check_mpegh: PASS" || { echo "check_mpegh: FAIL"; exit 1; }
