#!/usr/bin/env bash
# ============================================================================
# check_mpegps.sh — self-check for MPEG program-stream identification
# (mpegps.hpp, mpegps-info), synthetic oracle only.  No corpus, no FFmpeg.
#
# WHY THIS EXISTS.  mpegps.hpp names every elementary stream of a .vob /
# .aob / .mpg by the ID the disc gives it, in every build.  It was checked
# against ffprobe on 36 real VOB/AOB files (three DVD-Video discs, one
# DVD-Audio disc) -- IDs, kinds, codecs, video size and frame rate all agree
# -- but that corpus is commercial and lives on one machine.  These fixtures
# pin the same behaviour anywhere:
#
#   1. MPEG-2 PS, DVD-Video shape: system header (audio_bound 3), nav pack
#      (PCI 980 + DSI 1018 bytes), MPEG-2 video 720x576 25 fps (sequence
#      header + sequence_extension), AC-3 0x80, LPCM 0xA0, subpicture 0x20,
#      MPEG-1 Layer II 0xC0, padding.  -> exactly those streams.
#   2. MLP vs LPCM is decided by the MLP major sync, not the sub-stream ID:
#      0xA1 with F8 72 6F BB is MLP, 0xA2 without it is LPCM.
#   3. A private_stream_1 packet whose sub-stream ID is in no DVD range
#      (0xFF filler, as on a Last Waltz VOB) is counted, never made a stream.
#   4. A program_end_code followed by 0xFF stuffing to the sector's end (as
#      on a DVD-Audio still-video object) is not reported as lost sync.
#   5. A private_stream_2 that is NOT a PCI/DSI pair (745-byte packets) is a
#      data stream, not "navigation".
#   6. The same stream split into VTS_01_1.VOB / VTS_01_2.VOB at an
#      arbitrary byte -- mid-packet -- reads as one title set with the same
#      streams as the unsplit file.
#   7. MPEG-1 system stream: 12-byte pack header, MPEG-1 packet headers
#      (stuffing, STD buffer, PTS), MPEG-1 video 352x288 25 fps (no
#      sequence_extension) and MPEG-1 Layer II audio.
#
# Usage: scripts/check_mpegps.sh [path-to-mpegps-info]   (default ./mpegps-info)
# Run it after touching mpegps.hpp or dvd.hpp.
# ============================================================================
set -euo pipefail

INFO="${1:-./mpegps-info}"
[ -x "$INFO" ] || { echo "no mpegps-info at $INFO — run 'make mpegps-info' first"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

python3 - "$TMP" <<'PY'
import os, struct, sys
out = sys.argv[1]
SECTOR = 2048

def pack2():   # MPEG-2 pack header, 14 bytes, no stuffing
    return b'\x00\x00\x01\xba' + bytes([0x44, 0, 4, 0, 4, 1, 0x01, 0x89, 0xC3, 0xF8])

def pack1():   # MPEG-1 pack header, 12 bytes
    return b'\x00\x00\x01\xba' + bytes([0x21, 0, 1, 0, 1, 0x80, 0x1B, 0x83])

def system_header(audio_bound, video_bound):
    body = bytes([0x80, 0x00, 0x01, (audio_bound << 2), 0xE0 | video_bound, 0x7F])
    return b'\x00\x00\x01\xbb' + struct.pack('>H', len(body)) + body

PTS = bytes([0x21, 0, 1, 0, 1])
def pes2(sid, payload):   # MPEG-2 PES with a PTS
    hdr = bytes([0x81, 0x80, 5]) + PTS
    return b'\x00\x00\x01' + bytes([sid]) + struct.pack('>H', len(hdr) + len(payload)) + hdr + payload

def pes1(sid, payload):   # MPEG-1 packet: 2 stuffing, STD buffer, PTS
    hdr = b'\xff\xff' + bytes([0x60, 0x2E]) + PTS
    return b'\x00\x00\x01' + bytes([sid]) + struct.pack('>H', len(hdr) + len(payload)) + hdr + payload

def raw(sid, payload):    # private_stream_2 / padding: no header
    return b'\x00\x00\x01' + bytes([sid]) + struct.pack('>H', len(payload)) + payload

def sector(header, *packets):
    s = header + b''.join(packets)
    room = SECTOR - len(s)
    assert room == 0 or room >= 6, room
    if room: s += raw(0xBE, b'\xff' * (room - 6))
    return s

def seq_header(w, h, frc, mpeg2):
    s = b'\x00\x00\x01\xb3' + bytes([w >> 4, ((w & 0xF) << 4) | (h >> 8), h & 0xFF,
                                     0x20 | frc, 0xFF, 0xFF, 0xE0, 0x18])
    if mpeg2: s += b'\x00\x00\x01\xb5' + bytes([0x14, 0x8A, 0x00, 0x01, 0x00, 0x00])
    return s + b'\x00\x00\x01\xb8' + b'\x00' * 8   # then a GOP header

# A picture start code and a slice: what makes an elementary stream look
# like video to a content prober, as every real one does.
PIC = b'\x00\x00\x01\x00' + bytes([0x00, 0x0F, 0xFF, 0xF8]) + b'\x00\x00\x01\x01' + b'\x44' * 400

MP2 = b'\xff\xfd\x94\x00' + b'\x55' * 60            # MPEG-1 Layer II frame header
AC3 = b'\x80\x01\x00\x01' + b'\x0b\x77' + b'\x11' * 60
# Private headers in the shape the discs use: DVD-Video LPCM (frames,
# first-access pointer, then 24-bit / 48 kHz / 2 ch), and the 13-byte
# DVD-Audio header Rumours' MLP stream carries before its major sync.
LPCM = lambda sub: bytes([sub, 0x05, 0x00, 0x04, 0x00, 0x81, 0x80]) + b'\x22' * 64
MLP  = lambda sub: bytes([sub, 0x20, 0xC0, 0x06, 0x00, 0x05, 0x5F, 0xFF, 0x80, 0x00, 0x50,
                          0xA0, 0xFF, 0xB0]) + b'\xf8\x72\x6f\xbb' + b'\x33' * 60
NAV = raw(0xBF, b'\x00' + b'\x00' * 979) + raw(0xBF, b'\x01' + b'\x00' * 1017)

# 1-4: a DVD-Video-shaped title, 40 sectors
secs = []
secs.append(sector(pack2(), system_header(3, 1), NAV))
for k in range(12):
    secs.append(sector(pack2(), pes2(0xE0, seq_header(720, 576, 3, True) + PIC)))
    secs.append(sector(pack2(), pes2(0xBD, AC3), pes2(0xBD, LPCM(0xA0)), pes2(0xBD, MLP(0xA1)),
                       pes2(0xBD, LPCM(0xA2)), pes2(0xC0, MP2)))
    secs.append(sector(pack2(), pes2(0xBD, b'\x20' + b'\x00' * 100)))
secs.append(sector(pack2(), pes2(0xBD, b'\xff' * 400)))                  # 3
secs.append(b'\x00\x00\x01\xb9' + b'\xff' * (SECTOR - 4))              # 4
secs.append(sector(pack2(), pes2(0xE0, b'\x44' * 500)))
dvdv = b''.join(secs)
assert len(dvdv) % SECTOR == 0
open(f'{out}/dvdv.vob', 'wb').write(dvdv)

# 5: private_stream_2 that is not a PCI/DSI pair
open(f'{out}/ps2.vob', 'wb').write(
    sector(pack2(), raw(0xBF, b'\x02' + b'\x00' * 744)) + sector(pack2(), pes2(0xE0, b'\x44' * 64)))

# 6: the same title as VTS_01_1 / VTS_01_2, cut mid-packet
os.makedirs(f'{out}/VIDEO_TS')
cut = 7 * SECTOR + 333
open(f'{out}/VIDEO_TS/VTS_01_1.VOB', 'wb').write(dvdv[:cut])
open(f'{out}/VIDEO_TS/VTS_01_2.VOB', 'wb').write(dvdv[cut:])

# 7: MPEG-1 system stream
m1 = b''
for k in range(8):
    m1 += sector(pack1(), *( [system_header(1, 1)] if k == 0 else []),
                 pes1(0xE0, seq_header(352, 288, 3, False) + PIC), pes1(0xC0, MP2))
m1 += b'\x00\x00\x01\xb9'
open(f'{out}/mpeg1.mpg', 'wb').write(m1)
PY

fail=0
pass()  { echo "  ok    $1"; }
flunk() { echo "  FAIL  $1"; fail=1; }

expect_ids() {   # expect_ids <name> <file> <expected lines...>   (--single --ids)
    local name="$1" f="$2"; shift 2
    local got want
    got="$("$INFO" --single --ids "$TMP/$f")"
    want="$(printf '%s\n' "$@")"
    if [ "$got" = "$want" ]; then pass "$name"; else
        flunk "$name"; diff <(echo "$want") <(echo "$got") | sed 's/^/        /'; fi
}

echo "check_mpegps: $INFO"
expect_ids "DVD-Video shape: streams, kinds, codecs (1, 2)" dvdv.vob \
    "0x20 subpicture subpicture" \
    "0x80 audio AC-3" \
    "0xa0 audio LPCM" \
    "0xa1 audio MLP" \
    "0xa2 audio LPCM" \
    "0x1bf navigation DVD navigation (PCI/DSI)" \
    "0x1c0 audio MPEG-1 Layer II" \
    "0x1e0 video MPEG-2 video 720x576 25 fps"

full="$("$INFO" --single "$TMP/dvdv.vob")"
grep -q "System header    : audio_bound 3, video_bound 1" <<<"$full" \
    && pass "system header read" || flunk "system header: $(grep 'System' <<<"$full")"
grep -q "Unrecognised     : 1 private_stream_1 packets" <<<"$full" \
    && pass "0xFF filler counted, not a stream (3)" || flunk "unknown sub-stream not counted"
grep -q "Resync" <<<"$full" \
    && flunk "program_end_code + stuffing reported as lost sync (4)" \
    || pass "program_end_code + stuffing is not lost sync (4)"
grep -q "every pack on a 2048-byte boundary" <<<"$full" \
    && pass "2048-byte pack alignment seen" || flunk "pack alignment not reported"

expect_ids "private_stream_2 without PCI/DSI shape is data (5)" ps2.vob \
    "0x1bf data private_stream_2" \
    "0x1e0 video video"

got="$("$INFO" --ids "$TMP/VIDEO_TS/VTS_01_2.VOB")"
want="$("$INFO" --single --ids "$TMP/dvdv.vob")"
[ "$got" = "$want" ] && pass "title set split mid-packet reads as the whole stream (6)" \
                     || { flunk "split title set differs from the unsplit stream (6)"; diff <(echo "$want") <(echo "$got") | sed 's/^/        /'; }
"$INFO" "$TMP/VIDEO_TS/VTS_01_1.VOB" | grep -q "^Input            : DVD-Video title set VTS_01" \
    && pass "named as DVD-Video title set VTS_01" || flunk "title set not named"

expect_ids "MPEG-1 system stream (7)" mpeg1.mpg \
    "0x1c0 audio MPEG-1 Layer II" \
    "0x1e0 video MPEG-1 video 352x288 25 fps"
"$INFO" "$TMP/mpeg1.mpg" | grep -q "^Format           : MPEG-1 system stream" \
    && pass "MPEG-1 pack header recognised" || flunk "MPEG-1 pack header not recognised"

[ "$fail" -eq 0 ] && echo "check_mpegps: PASS" || { echo "check_mpegps: FAIL"; exit 1; }
