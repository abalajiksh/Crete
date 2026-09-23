#!/usr/bin/env bash
# make_dvd_corpus.sh -- build the DVD-Video / DVD-Audio test corpus.
#
#   scripts/make_dvd_corpus.sh <DVDAUDIO_MOUNT> <DVDVIDEO_MOUNT> <DEST>
#
# Needs the two discs mounted read-only, e.g.
#   hdiutil attach -readonly -nobrowse -mountpoint /tmp/rum   RUMOURS.iso
#   hdiutil attach -readonly -nobrowse -mountpoint /tmp/dsd50 ..._50TH_DVD.iso
#
# WHY SLICES, NOT WHOLE TITLES. The two source titles are 4.8 GB and 2.7 GB.
# Every property the suite gates is present in the first minute, so the corpus
# carries byte ranges instead -- and a byte range of an MPEG program stream is
# itself a valid program stream, because DVD packs are exactly 2048 bytes and
# every cut here is pack-aligned. The audio inside is the disc's own bytes; no
# transcode, no re-mux.
#
# WHY THE FRAGMENTS ARE SPLIT THE WAY THEY ARE. A real DVD splits a title at
# whatever pack the 1 GB file limit fell on, so the split is arbitrary and lands
# mid-frame. Cutting one contiguous range into two files reproduces exactly that
# -- and the split is the thing under test, since measuring the pieces
# separately loses the frame that straddles it.
#
# The .flac references are the SAME audio decoded through a different path, so
# the suite has a crete-vs-crete oracle needing no external reference: MLP and
# DVD LPCM are lossless, so crete-on-AOB and crete-on-FLAC must agree exactly.
set -euo pipefail

DVDA="${1:?usage: $0 <dvdaudio-mount> <dvdvideo-mount> <dest>}"
DVDV="${2:?usage: $0 <dvdaudio-mount> <dvdvideo-mount> <dest>}"
DEST="${3:?usage: $0 <dvdaudio-mount> <dvdvideo-mount> <dest>}"

PACK=2048
have() { command -v "$1" >/dev/null 2>&1; }
have ffmpeg || { echo "ERROR: ffmpeg is required to build the references"; exit 1; }

src_aob="$DVDA/AUDIO_TS/ATS_01_1.AOB"
src_grp="$DVDA/AUDIO_TS/ATS_01_3.AOB"
src_vob="$DVDV/VIDEO_TS/VTS_02_1.VOB"
for f in "$src_aob" "$src_grp" "$src_vob"; do
    [ -f "$f" ] || { echo "ERROR: missing $f"; exit 1; }
done

rm -rf "$DEST"
mkdir -p "$DEST/Rumours_DVDA" "$DEST/Rumours_DVDA_GroupChange" \
         "$DEST/DSOTM_DVDV" "$DEST/reference"

# ── DVD-Audio: MLP 96 kHz 24-bit 5.1, one group, split in two ──────────────
echo ">> Rumours_DVDA (MLP 5.1)"
dd if="$src_aob" bs=$PACK count=8192            of="$DEST/Rumours_DVDA/ATS_01_1.AOB" 2>/dev/null
dd if="$src_aob" bs=$PACK skip=8192 count=8192  of="$DEST/Rumours_DVDA/ATS_01_2.AOB" 2>/dev/null

# ── DVD-Audio: a range spanning the 6-channel -> 2-channel GROUP change ────
# Located by bisection on the source disc: ATS_01_3.AOB is 5.1 up to about pack
# 277113 and stereo from about 277616. The range below starts inside the 5.1
# group -- so the container probe and the first frame agree -- and ends past
# the boundary, which is what makes crete warn instead of metering two
# different programs as one.
echo ">> Rumours_DVDA_GroupChange (5.1 -> stereo)"
dd if="$src_grp" bs=$PACK skip=274000 count=3000           of="$DEST/Rumours_DVDA_GroupChange/ATS_01_1.AOB" 2>/dev/null
dd if="$src_grp" bs=$PACK skip=277000 count=3000           of="$DEST/Rumours_DVDA_GroupChange/ATS_01_2.AOB" 2>/dev/null

# ── DVD-Video: LPCM 2.0 + two AC-3 5.1 beside MPEG-2 video ─────────────────
echo ">> DSOTM_DVDV (pcm_dvd + 2x ac3)"
dd if="$src_vob" bs=$PACK count=12288           of="$DEST/DSOTM_DVDV/VTS_02_1.VOB" 2>/dev/null
dd if="$src_vob" bs=$PACK skip=12288 count=12288 of="$DEST/DSOTM_DVDV/VTS_02_2.VOB" 2>/dev/null

# ── References: the same audio through a different decode path ─────────────
# `cat` of the fragments is byte-for-byte what crete's own ConcatIO presents,
# so the reference is built from identical input rather than from a re-read of
# the disc. Both carriers are lossless, so the FLAC holds the same samples.
# The stream is located BY CODEC, never by a hardcoded index: the index a VOB
# reports is not stable across fragments of the same title (see test_dvd.py),
# so writing "4" here would be writing down an accident of this cut.
mk_ref() {   # mk_ref <dir> <glob> <codec-to-find> <out.flac>
    local tmp idx
    tmp="$(mktemp)"
    cat "$1"/$2 > "$tmp"
    idx="$(ffprobe -v error -probesize 100M -analyzeduration 100M \
             -select_streams a -show_entries stream=index,codec_name \
             -of csv=p=0 "$tmp" | awk -F, -v c="$3" '$2==c {print $1; exit}')"
    [ -n "$idx" ] || { echo "ERROR: no $3 stream in $1"; rm -f "$tmp"; exit 1; }
    echo "   $3 is stream $idx"
    # +genpts because a DVD program stream carries repeating DTS across a pack
    # boundary and the FLAC muxer refuses them otherwise. It changes no sample.
    ffmpeg -v error -y -probesize 100M -analyzeduration 100M -fflags +genpts \
           -i "$tmp" -map "0:$idx" -c:a flac "$4"
    rm -f "$tmp"
}
echo ">> references"
mk_ref "$DEST/Rumours_DVDA" '*.AOB' mlp     "$DEST/reference/Rumours_DVDA.flac"
# The LPCM stream, not the AC-3 ones: AC-3 is lossy, so it can only be
# snapshotted, while LPCM gives the suite an exact equality to assert.
mk_ref "$DEST/DSOTM_DVDV" '*.VOB'  pcm_dvd "$DEST/reference/DSOTM_DVDV_lpcm.flac"

echo
find "$DEST" -type f | sort | while read -r f; do
    printf '  %8s  %s\n' "$(du -h "$f" | cut -f1)" "${f#"$DEST"/}"
done
echo ">> total: $(du -sh "$DEST" | cut -f1)"
