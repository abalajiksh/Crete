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
pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,\
pcm_f32le,pcm_f32be,pcm_u8,pcm_s8"

# wav/aiff/flac demuxers are redundant with crête's native paths (which always
# win in decode_file) but are cheap to keep and make this FFmpeg usable solo.
DEMUXERS="mov,mp3,ogg,matroska,webm_dash_manifest,aac,asf,flac,wav,aiff,\
ape,wv,mpc,mpc8,tak"

PARSERS="aac,aac_latm,mpegaudio,flac,vorbis,opus,tak"

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

echo
echo ">> done."
echo ">>   libs + .pc files: $PREFIX/lib"
echo ">>   headers:          $PREFIX/include"
echo ">> now build crête:    make cli-ffmpeg"
