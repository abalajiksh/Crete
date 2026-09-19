#!/usr/bin/env bash
# ============================================================================
# build_ffmpeg_compact.sh — build a minimal, static, decode-only FFmpeg
#
# Produces static libs + headers + pkg-config files under
# third_party/ffmpeg-compact/, which `make cli-ffmpeg` links into a single
# self-contained `crete` binary (no FFmpeg shared-lib dependency).
#
# Scope, deliberately constrained:
#   - LGPL-2.1 only: no --enable-gpl, no --enable-nonfree
#   - native decoders only: no external codec libs (keeps it self-contained)
#   - no encoders / muxers / filters / network / programs / devices
#   - decoders for the formats crête can't decode natively (ALAC, AAC, MP3,
#     Opus, Vorbis, WMA, WavPack, Monkey's Audio, Musepack, TAK, plus LPCM)
#   - Blu-ray / disc audio beds: TrueHD (+MLP), E-AC-3, AC-3, DTS (incl. the
#     DTS-HD MA lossless extension). All native LGPL decoders, so the
#     LGPL-only constraint above still holds.
#
# NOTE ON OBJECT AUDIO: FFmpeg has no Atmos/DTS:X renderer. It decodes the
# channel BED only (TrueHD 7.1, E-AC-3 5.1 for JOC) and discards the object
# metadata. It does report the carrier in AVCodecContext::profile as an
# integer, so the caller can label what was actually measured. Never present a
# bed measurement as an Atmos measurement.
#
# BUT: --enable-small below makes NULL_IF_CONFIG_SMALL(ff_truehd_profiles) and
# its siblings expand to NULL, so av_get_profile_name() returns NULL for EVERY
# codec in THIS build -- including DTS-HD MA, which a full ffprobe names
# without trouble. Measured 2026-09-19 on all six DSOTM Blu-ray carriers: 6/6
# empty. The profile NUMBER survives; only the name TABLE is compiled out. So
# crete carries its own AV_PROFILE_* -> string table in audio_ffmpeg.hpp rather
# than dropping --enable-small (which would grow every agent's cached build and
# invalidate the crete-ffmpeg-build.id stamp for no measurement benefit).
# If you ever remove --enable-small, that table becomes dead but harmless:
# av_get_profile_name() is tried first and wins when it works.
#
# Usage:
#   ./scripts/build_ffmpeg_compact.sh [VERSION] [PREFIX]
#     VERSION  FFmpeg release tag without the leading 'n' (default: 7.1)
#     PREFIX   install dir (default: <repo>/third_party/ffmpeg-compact)
#
# Windows (MinGW cross) is intentionally not baked in; add the usual
#   --cross-prefix=x86_64-w64-mingw32- --target-os=mingw32 --arch=x86_64
#   --enable-cross-compile
# to the configure call below when cross-building.
# ============================================================================

set -euo pipefail

VERSION="${1:-7.1}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${2:-$ROOT/third_party/ffmpeg-compact}"
SRC="$ROOT/third_party/ffmpeg-src"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

# ── Components to include (trim or extend to taste) ─────────────────────────
# Decoder names are FFmpeg's internal names (mpc7/mpc8 for Musepack, etc.).
DECODERS="alac,aac,aac_latm,mp3,mp3float,opus,vorbis,flac,\
wmav1,wmav2,wmalossless,wmapro,wavpack,ape,mpc7,mpc8,tak,\
truehd,mlp,eac3,ac3,dca,\
pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,\
pcm_f32le,pcm_f32be,pcm_u8,pcm_s8"

# wav/aiff/flac demuxers are redundant with crête's native paths (which always
# win in decode_file) but are cheap to keep and make this FFmpeg usable solo.
# truehd/dts/eac3/ac3 here are the RAW elementary-stream demuxers - matroska and
# mov already cover the disc rips, these just make bare .thd/.dts files work too.
DEMUXERS="mov,mp3,ogg,matroska,webm_dash_manifest,aac,asf,flac,wav,aiff,\
ape,wv,mpc,mpc8,tak,truehd,dts,eac3,ac3"

# mlp parser handles both MLP and TrueHD framing.
PARSERS="aac,aac_latm,mpegaudio,flac,vorbis,opus,tak,mlp,ac3,dca"

# ── Fetch ───────────────────────────────────────────────────────────────────
mkdir -p "$ROOT/third_party"
if [ ! -d "$SRC/.git" ]; then
    echo ">> cloning FFmpeg n$VERSION"
    git clone --depth 1 --branch "n$VERSION" \
        https://github.com/FFmpeg/FFmpeg.git "$SRC"
fi
cd "$SRC"

# ── Configure ────────────────────────────────────────────────────────────────
echo ">> configuring (prefix: $PREFIX)"
./configure \
    --prefix="$PREFIX" \
    --enable-static --disable-shared --enable-pic \
    --enable-small \
    --disable-programs --disable-doc \
    --disable-network --disable-autodetect \
    --disable-avdevice --disable-avfilter --disable-swscale --disable-postproc \
    --disable-everything \
    --disable-encoders --disable-muxers --disable-bsfs \
    --disable-devices --disable-hwaccels \
    --disable-zlib --disable-bzlib --disable-lzma --disable-iconv \
    --enable-protocol=file \
    --enable-decoder="$DECODERS" \
    --enable-demuxer="$DEMUXERS" \
    --enable-parser="$PARSERS"

# ── Build + install ──────────────────────────────────────────────────────────
echo ">> building with $JOBS jobs"
make -j"$JOBS"
make install

# ── Build fingerprint ────────────────────────────────────────────────────────
# A cached prefix is normally reused, and callers have gated that on
# lib/pkgconfig/libavformat.pc merely EXISTING. That sentinel says "an FFmpeg
# was built here", not "an FFmpeg with the decoders you need was built here" --
# so after e754d71 added truehd/mlp/eac3/ac3/dca, every agent with a warm cache
# silently kept a build that could not decode disc audio, and the DISCAUDIO
# suite failed on four of six carriers with no hint as to why. Record what this
# build actually contains so a caller can invalidate on a CHANGE rather than on
# absence.
# The hash is of THIS SCRIPT, not just the component lists: the configure flags
# define the build every bit as much as the decoder list does, and hashing the
# whole file means a future edit of any kind invalidates a stale cache without
# anyone having to remember to extend a fingerprint.
script_hash() {
    if command -v md5sum >/dev/null 2>&1; then md5sum  < "$1" | cut -d' ' -f1
    else                                       md5 -q  "$1"
    fi
}
cat > "$PREFIX/crete-ffmpeg-build.id" <<EOF
script_md5=$(script_hash "$0")
version=$VERSION
decoders=$DECODERS
demuxers=$DEMUXERS
parsers=$PARSERS
EOF
echo ">> wrote build id: $PREFIX/crete-ffmpeg-build.id"

echo
echo ">> done."
echo ">>   libs + .pc files: $PREFIX/lib"
echo ">>   headers:          $PREFIX/include"
echo ">> now build crête:    make cli-ffmpeg"
