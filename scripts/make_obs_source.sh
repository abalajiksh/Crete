#!/usr/bin/env bash
# ============================================================================
# make_obs_source.sh — assemble the openSUSE Build Service upload for crete
#
# OBS builds run with NO network, so everything the FFmpeg tiers fetch at
# build time is vendored into one source tarball here:
#
#   crete-<ver>/                 git archive of <ref> (crete + libdsddpcm)
#   crete-<ver>/third_party/imgui       Dear ImGui, pinned below (GUI)
#   crete-<ver>/third_party/ffmpeg-src  FFmpeg n<FFMPEG_VERSION>, whole tree
#
# The FFmpeg tree is used as-is by scripts/build_ffmpeg_compact.sh (it only
# clones when no configure script is present), so the packaged build runs the
# same script, same configure line, same component lists as CI.
#
# The tarball is DETERMINISTIC (sorted entries, fixed mtime/owner, gzip mtime
# 0): regenerating from the same ref gives the same sha256, so an OBS rebuild
# can be tied to a source by checksum.
#
# Usage:
#   scripts/make_obs_source.sh OUTDIR [REF]
#     OUTDIR  where the upload set is written (created; must be empty or new)
#     REF     git ref to package (default HEAD). The version is read from the
#             Makefile AT THAT REF, so package a tag for a release.
#
# Output (upload all of it to the OBS package):
#   crete-<ver>.tar.gz  crete.spec  crete.changes
#   crete.dsc  debian.control  debian.rules  debian.changelog  debian.copyright
#   PKGBUILD
# ============================================================================

set -euo pipefail

OUT="${1:?usage: $0 OUTDIR [REF]}"
REF="${2:-HEAD}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TPL="$ROOT/packaging/obs"

# Dear ImGui has no release discipline crete tracks; this is the commit the
# GUI was built and visually checked against. Bump deliberately.
IMGUI_COMMIT=6f41d13c7a93cf65aca4afe9bf24b610740c3d5a
IMGUI_URL=https://github.com/ocornut/imgui.git
FFMPEG_URL=https://github.com/FFmpeg/FFmpeg.git

cd "$ROOT"
git rev-parse --verify --quiet "$REF^{commit}" >/dev/null \
    || { echo "error: unknown ref '$REF'" >&2; exit 1; }
# Uncommitted edits are NOT packaged (git archive reads the ref), which is
# easy to forget when packaging HEAD -- say so rather than surprise.
if [ "$REF" = HEAD ] && ! git diff --quiet HEAD -- . ':!temp'; then
    echo "warning: working tree has uncommitted changes; they are NOT in the tarball" >&2
fi

VERSION="$(git show "$REF:Makefile" | sed -n 's/^VERSION *?= *//p')"
FFMPEG_VERSION="$(git show "$REF:Makefile" | sed -n 's/^FFMPEG_VERSION *?= *//p')"
[ -n "$VERSION" ] && [ -n "$FFMPEG_VERSION" ] \
    || { echo "error: could not read VERSION / FFMPEG_VERSION from $REF:Makefile" >&2; exit 1; }
NAME="crete-$VERSION"
echo ">> crete $VERSION (ref $REF, $(git rev-parse --short "$REF^{commit}")), FFmpeg n$FFMPEG_VERSION, ImGui ${IMGUI_COMMIT:0:12}"

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
[ -z "$(ls -A "$OUT")" ] || { echo "error: $OUT is not empty" >&2; exit 1; }
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/$NAME"

# ── crete itself ────────────────────────────────────────────────────────────
git archive --prefix="$NAME/" "$REF" | tar -x -C "$WORK"

# ── Dear ImGui: only what the GUI compiles, plus its licence ────────────────
IMGUI_SRC="$ROOT/third_party/imgui"
if ! git -C "$IMGUI_SRC" cat-file -e "$IMGUI_COMMIT^{commit}" 2>/dev/null; then
    IMGUI_SRC="$WORK/imgui-clone"
    git clone --quiet --filter=blob:none "$IMGUI_URL" "$IMGUI_SRC"
fi
mkdir -p "$STAGE/third_party/imgui"
git -C "$IMGUI_SRC" archive "$IMGUI_COMMIT" \
    LICENSE.txt imconfig.h imgui.h imgui_internal.h \
    imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp \
    imstb_rectpack.h imstb_textedit.h imstb_truetype.h \
    backends/imgui_impl_sdl2.cpp backends/imgui_impl_sdl2.h \
    backends/imgui_impl_opengl3.cpp backends/imgui_impl_opengl3.h \
    backends/imgui_impl_opengl3_loader.h \
    | tar -x -C "$STAGE/third_party/imgui"

# ── FFmpeg: the full release tree the build script configures ──────────────
FF_SRC="$ROOT/third_party/ffmpeg-src"
if ! git -C "$FF_SRC" rev-parse --verify --quiet "n$FFMPEG_VERSION^{commit}" >/dev/null 2>&1; then
    FF_SRC="$WORK/ffmpeg-clone"
    git clone --quiet --depth 1 --branch "n$FFMPEG_VERSION" "$FFMPEG_URL" "$FF_SRC"
fi
mkdir -p "$STAGE/third_party/ffmpeg-src"
git -C "$FF_SRC" archive "n$FFMPEG_VERSION" | tar -x -C "$STAGE/third_party/ffmpeg-src"

printf 'imgui   %s  %s\nffmpeg  n%s  %s\n' \
    "$IMGUI_COMMIT" "$IMGUI_URL" "$FFMPEG_VERSION" "$FFMPEG_URL" \
    > "$STAGE/third_party/VENDORED-FOR-PACKAGING.txt"

# ── Deterministic tar.gz ────────────────────────────────────────────────────
# Python rather than tar flags: macOS ships bsdtar, whose options differ from
# GNU tar's --sort/--mtime/--owner, and this must give the same bytes anywhere.
python3 - "$WORK" "$NAME" "$OUT/$NAME.tar.gz" <<'PY'
import gzip, io, os, sys, tarfile
work, name, dest = sys.argv[1:]
root = os.path.join(work, name)
paths = []
for d, dirs, files in os.walk(root):
    dirs.sort()
    for n in sorted(dirs + files):
        paths.append(os.path.join(d, n))
paths.sort(key=lambda p: os.path.relpath(p, work))
MTIME = 1_700_000_000
with open(dest, "wb") as raw:
    with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=9) as gz:
        with tarfile.open(fileobj=gz, mode="w", format=tarfile.GNU_FORMAT) as tar:
            def add(p):
                ti = tar.gettarinfo(p, arcname=os.path.relpath(p, work))
                ti.mtime, ti.uid, ti.gid, ti.uname, ti.gname = MTIME, 0, 0, "root", "root"
                if ti.isfile():
                    ti.mode = 0o755 if ti.mode & 0o111 else 0o644
                    with open(p, "rb") as f:
                        tar.addfile(ti, f)
                else:
                    if ti.isdir():
                        ti.mode = 0o755
                    tar.addfile(ti)
            add(root)
            for p in paths:
                add(p)
PY

SHA256="$( (sha256sum "$OUT/$NAME.tar.gz" 2>/dev/null || shasum -a 256 "$OUT/$NAME.tar.gz") | cut -d' ' -f1)"
CHANGES_DATE="$(LC_ALL=C TZ=UTC date '+%a %b %e %H:%M:%S UTC %Y')"
DEB_DATE="$(LC_ALL=C TZ=UTC date '+%a, %d %b %Y %H:%M:%S +0000')"

fill() {
    sed -e "s|@VERSION@|$VERSION|g" -e "s|@SHA256@|$SHA256|g" \
        -e "s|@CHANGES_DATE@|$CHANGES_DATE|g" -e "s|@DEB_DATE@|$DEB_DATE|g" \
        "$TPL/$1" > "$OUT/$2"
}
fill crete.spec.in       crete.spec
fill crete.changes.in    crete.changes
fill crete.dsc.in        crete.dsc
fill debian.changelog.in debian.changelog
fill PKGBUILD.in         PKGBUILD
cp "$TPL/debian.control" "$TPL/debian.rules" \
   "$TPL/debian.copyright" "$OUT/"

echo ">> wrote $OUT"
ls -l "$OUT"
echo ">> sha256 $SHA256  $NAME.tar.gz"
