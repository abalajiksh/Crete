#!/usr/bin/env bash
# make_integrity_corpus.sh — build the INTEGRITY test corpus.
#
# Every other crete corpus is commercial music: it cannot be redistributed,
# which is why an external contributor can run the synthetic tier and nothing
# else. This one is SYNTHESISED from a fixed seed, so it is freely
# distributable, reproducible byte-for-byte on any machine, and needs no
# reference oracle -- the assertions are about crete's own integrity
# reporting, not about a DR value anyone has to agree with.
#
# Output: <dest>/integrity/{clean,damaged,album_mixed}/ + MANIFEST.tsv, and
# integrity.zip beside it.
#
# Usage: scripts/make_integrity_corpus.sh [dest]   (default: ./build/corpus)
set -euo pipefail

DEST="${1:-build/corpus}"
ROOT="$DEST/integrity"
rm -rf "$ROOT" "$DEST/integrity.zip"
mkdir -p "$ROOT/clean" "$ROOT/damaged" "$ROOT/album_mixed"

command -v ffmpeg >/dev/null || { echo "need ffmpeg" >&2; exit 1; }

# ── 1. The source signal ───────────────────────────────────────────────────
# Deterministic, stdlib-only, and deliberately NOT a steady tone: a meter test
# needs block-to-block RMS variation or truncation changes nothing and DR is
# meaningless. Harmonics + a slow envelope + seeded noise gives a signal that
# behaves like music to the 3 s block machinery and still compresses.
python3 - "$ROOT/clean/clean.wav" <<'PY'
import math, random, struct, sys, wave
SR, SECS = 44100, 20
random.seed(20260921)
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
frames = bytearray()
for n in range(SR * SECS):
    t = n / SR
    # Slow envelope with a quiet passage, so the top-20% block set is a real
    # subset rather than every block.
    env = 0.16 + 0.55 * (0.5 - 0.5 * math.cos(2 * math.pi * t / 7.0)) ** 2
    tone = (math.sin(2*math.pi*220*t) + 0.5*math.sin(2*math.pi*441*t)
            + 0.25*math.sin(2*math.pi*663*t))
    l = env * (tone / 1.75) + random.uniform(-1, 1) * 0.0008
    r = env * (math.sin(2*math.pi*331*t) * 0.6) + random.uniform(-1, 1) * 0.0008
    frames += struct.pack('<hh', max(-32768, min(32767, int(l*30000))),
                                 max(-32768, min(32767, int(r*30000))))
w.writeframes(bytes(frames)); w.close()
PY

ffmpeg -nostdin -v error -i "$ROOT/clean/clean.wav" -c:a flac     "$ROOT/clean/clean.flac" -y
ffmpeg -nostdin -v error -i "$ROOT/clean/clean.wav" -c:a pcm_s16be "$ROOT/clean/clean.aiff" -y

# ── 2. The damage ──────────────────────────────────────────────────────────
# Each case targets ONE detector. Offsets are fractions of the file so the
# corpus regenerates identically anywhere.
python3 - "$ROOT" <<'PY'
import os, random, shutil, sys
root = sys.argv[1]
clean_f = os.path.join(root, 'clean', 'clean.flac')
clean_w = os.path.join(root, 'clean', 'clean.wav')
clean_a = os.path.join(root, 'clean', 'clean.aiff')
dmg     = os.path.join(root, 'damaged')
random.seed(7)

def patch(src, name, off_frac, payload):
    dst = os.path.join(dmg, name)
    shutil.copy(src, dst)
    with open(dst, 'r+b') as f:
        f.seek(int(os.path.getsize(dst) * off_frac))
        f.write(payload)
    return dst

def truncate(src, name, frac):
    dst = os.path.join(dmg, name)
    shutil.copy(src, dst)
    os.truncate(dst, int(os.path.getsize(src) * frac))
    return dst

# FLAC — the only format here that can prove itself.
patch(clean_f, 'flac_crc_single_byte.flac', 0.50, bytes([0xFF]))
patch(clean_f, 'flac_crc_burst.flac',       0.50,
      bytes(random.getrandbits(8) for _ in range(64)))
# 0.10, not 0.50: swept across the file, only some offsets leave the
# decoder emitting OUT-OF-RANGE samples, and that is the one detector
# with no checksum behind it. This offset trips all three (CRC-16,
# sample-count shortfall, peak > 0 dBFS) and is pinned for that reason.
patch(clean_f, 'flac_bad_sector.flac',      0.10, b'\x00' * 4096)
truncate(clean_f, 'flac_truncated.flac', 0.60)
patch(clean_f, 'flac_bad_magic.flac', 0.0, b'XXXX')

# WAV / AIFF — no checksum exists, so only LENGTH and RANGE are checkable.
truncate(clean_w, 'wav_truncated.wav', 0.60)
truncate(clean_w, 'wav_header_only.wav', 0.0000083)      # ~44 bytes
truncate(clean_a, 'aiff_truncated.aiff', 0.60)
with open(os.path.join(dmg, 'wav_garbage.wav'), 'wb') as f:
    f.write(bytes(random.getrandbits(8) for _ in range(65536)))
open(os.path.join(dmg, 'wav_empty.wav'), 'wb').close()

# The documented BLIND SPOT, kept in the corpus on purpose: altering sample
# values inside a WAV is indistinguishable from the music being like that.
# No meter can catch it. The suite asserts crete reports it CLEAN, so the
# limitation stays visible instead of being quietly assumed away.
patch(clean_w, 'wav_altered_samples.wav', 0.50, b'\x00' * 4096)

# An album with one silently-wrong track and one that cannot be read at all.
# This is the case that matters in practice: before 0.17.0 it scored the
# damaged track, averaged it into the album DR and exited 0.
alb = os.path.join(root, 'album_mixed')
for i in (1, 2, 3):
    shutil.copy(clean_f, os.path.join(alb, f'0{i} - good.flac'))
shutil.copy(os.path.join(dmg, 'flac_bad_sector.flac'),
            os.path.join(alb, '04 - damaged.flac'))
shutil.copy(os.path.join(dmg, 'flac_bad_magic.flac'),
            os.path.join(alb, '05 - unreadable.flac'))
print('corpus written')
PY

# ── 3. Manifest ────────────────────────────────────────────────────────────
{
  printf 'file\tclass\texpected\n'
  printf 'clean/clean.wav\tclean\texit 0, no warnings\n'
  printf 'clean/clean.flac\tclean\texit 0, no warnings\n'
  printf 'clean/clean.aiff\tclean\texit 0, no warnings\n'
  printf 'damaged/flac_crc_single_byte.flac\tdegraded\texit 2, CRC-16 mismatch\n'
  printf 'damaged/flac_crc_burst.flac\tdegraded\texit 2, CRC and/or sample-count\n'
  printf 'damaged/flac_bad_sector.flac\tdegraded\texit 2, CRC + shortfall + peak>0 dBFS\n'
  printf 'damaged/flac_truncated.flac\tdegraded\texit 2, STREAMINFO shortfall\n'
  printf 'damaged/flac_bad_magic.flac\tfatal\texit 1, no JSON\n'
  printf 'damaged/wav_truncated.wav\tdegraded\texit 2, data-chunk shortfall\n'
  printf 'damaged/wav_header_only.wav\tfatal\texit 1, no JSON\n'
  printf 'damaged/wav_garbage.wav\tfatal\texit 1, no JSON\n'
  printf 'damaged/wav_empty.wav\tfatal\texit 1, no JSON\n'
  printf 'damaged/aiff_truncated.aiff\tdegraded\texit 2, COMM/SSND shortfall\n'
  printf 'damaged/wav_altered_samples.wav\tblind-spot\texit 0 -- WAV has no checksum\n'
  printf 'album_mixed/\tmixed\texit 2, 4 tracks measured, 1 named as unreadable\n'
} > "$ROOT/MANIFEST.tsv"

# ── 4. Deterministic zip ───────────────────────────────────────────────────
# The FILES are reproducible from the seed, but a zip embeds mtimes and walks
# the directory in filesystem order, so two runs otherwise produce two
# different sha256s. CI pins corpora by checksum, and "reproducible except for
# the checksum you actually compare" is not reproducible. Fixed timestamp,
# -X to drop platform extra fields, and an explicit sorted file list.
find "$ROOT" -exec touch -t 202601010000.00 {} +
( cd "$DEST" && find integrity -type f | LC_ALL=C sort | zip -qX integrity.zip -@ )

echo "corpus: $ROOT"
echo "zip:    $DEST/integrity.zip  ($(du -h "$DEST/integrity.zip" | cut -f1))"
echo "sha256: $(shasum -a 256 "$DEST/integrity.zip" | cut -d' ' -f1)"
echo
echo "NOTE: the default destination lives under build/, so 'make clean' removes it."
echo "      Re-run this script, or pass a destination outside the build tree." 
