#!/usr/bin/env bash
# ============================================================================
# build_windows_release.sh -- build, check and package crete for Windows
#
# Runs in an MSYS2 MINGW64 shell (the Windows Jenkins job and a human at the
# console run exactly this script):
#
#   scripts/build_windows_release.sh <src-dir> <out-dir> [stage ...]
#
# Stages, in order (default: all four):
#   preflight  toolchain present, and g++ uses the POSIX thread model
#   harness    make cli-json-ffmpeg STATIC=1, then the zero-corpus self-checks
#              (check_fix12.sh, check_lfe_peak.sh) and a JSON round trip.
#              Kept as <out-dir>/harness/crete-json-ffmpeg.exe for the test
#              harness -- it is NOT part of the release zip.
#   ship       make cli-ffmpeg STATIC=1 and make gui-ffmpeg STATIC=1
#   package    <out-dir>/crete-<ver>-windows-x86_64.zip + .sha256
#
# <src-dir> is a crete checkout of the ref being built. It may be an OLDER tag
# than this script: the job runs the script from its own checkout (main) and
# builds the ref in a subdirectory, so the script must only rely on things
# every supported tag has (Makefile targets, scripts/check_*.sh,
# IMGUI_COMMIT in scripts/make_obs_source.sh -- all present since v0.19.2).
#
# Why these choices, briefly (details in the job file, packaging/Jenkinsfile.windows):
#  - STATIC=1 on every binary. The CLI imports only KERNEL32/msvcrt/bcrypt and
#    the GUI links SDL2 statically, so the zip needs no DLLs. Checked below: a
#    MinGW runtime or SDL2/FFmpeg DLL import fails the build.
#  - Dear ImGui pinned to the SAME commit the OBS/Homebrew source tarball uses,
#    read from scripts/make_obs_source.sh, so all platforms ship one ImGui.
#    `make setup-imgui` would clone ImGui's moving HEAD.
#  - cli-json-ffmpeg and cli-ffmpeg write the SAME file (crete-ffmpeg.exe), so
#    the harness binary is moved aside before the shipped one is linked.
# ============================================================================
set -euo pipefail
# MINGW64 python writes stdout in the Windows code page when piped, which
# garbles "crête" in the Jenkins log. Force UTF-8.
export PYTHONIOENCODING=utf-8 PYTHONUTF8=1

usage() { sed -n '5,17p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
[ $# -ge 2 ] || usage
SRC=$1; OUT=$2; shift 2
STAGES=${*:-preflight harness ship package}

[ -f "$SRC/Makefile" ] || { echo "ERROR: $SRC is not a crete checkout"; exit 1; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
cd "$SRC"

VERSION=$(sed -n 's/^VERSION *?= *//p' Makefile)
FFV=$(sed -n 's/^FFMPEG_VERSION *?= *//p' Makefile)
SHA=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
# A tagged build is named by its version; anything else also carries the
# commit, so an artifact from main can never be mistaken for a release.
if [ "$(git tag --points-at HEAD 2>/dev/null | grep -Fx "v$VERSION" || true)" ]; then
    NAME="crete-$VERSION-windows-x86_64"
else
    NAME="crete-$VERSION-g$SHA-windows-x86_64"
fi

say() { printf '\n== %s\n' "$*"; }

# Windows system DLLs never start with "lib"; MinGW's runtime (libstdc++,
# libgcc_s, libwinpthread) and any dynamically-linked FFmpeg/SDL2 do.
check_imports() {
    local exe=$1 dlls bad
    dlls=$(objdump -p "$exe" | sed -n 's/^[[:space:]]*DLL Name: //p')
    echo "$exe imports: $(echo $dlls)"
    bad=$(printf '%s\n' "$dlls" | grep -iE '^lib|^sdl|^av(codec|format|util)|^swresample' || true)
    if [ -n "$bad" ]; then
        echo "ERROR: $exe is not self-contained -- it imports: $(echo $bad)"
        echo "       Was it built with STATIC=1?"
        exit 1
    fi
}

check_version() {
    local line
    line=$("$1" --version | head -n 1)
    echo "$line"
    printf '%s\n' "$line" | grep -qF " $VERSION" \
        || { echo "ERROR: $1 does not report version $VERSION"; exit 1; }
    if [ "${2:-}" = ffmpeg ]; then
        # "n7.1" from a git clone of FFmpeg (git describe), "7.1" from a
        # release tarball (OBS). Both are the pinned version.
        printf '%s\n' "$line" | grep -qE "ffmpeg-compact n?${FFV//./\\.}\]" \
            || { echo "ERROR: $1 was not built against the compact FFmpeg $FFV"; exit 1; }
    fi
}

pin_imgui() {
    local pin
    pin=$(sed -n 's/^IMGUI_COMMIT=//p' scripts/make_obs_source.sh 2>/dev/null || true)
    [ -n "$pin" ] || { echo "ERROR: no IMGUI_COMMIT in scripts/make_obs_source.sh"; exit 1; }
    # rev-parse inside a directory with no .git of its own answers with
    # crete's HEAD, which never equals the pin -- so a stray tree is replaced.
    if [ "$(git -C third_party/imgui rev-parse HEAD 2>/dev/null || true)" != "$pin" ]; then
        echo "fetching Dear ImGui ${pin:0:12}"
        rm -rf third_party/imgui
        git init -q third_party/imgui
        git -C third_party/imgui fetch -q --depth 1 https://github.com/ocornut/imgui.git "$pin"
        git -C third_party/imgui checkout -q FETCH_HEAD
    fi
    echo "Dear ImGui $(git -C third_party/imgui rev-parse --short=12 HEAD)"
}

stage_preflight() {
    say "preflight: crete $VERSION ($SHA), FFmpeg $FFV, artifact $NAME"
    local missing="" t tm
    for t in g++ gcc make pkg-config nasm objdump python3 git curl sha256sum; do
        command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
    done
    [ -z "$missing" ] || {
        echo "ERROR: missing in MSYS2 MINGW64:$missing"
        echo "       powershell -ExecutionPolicy Bypass -File scripts\\setup-deps.ps1"
        echo "       (and: pacman -S mingw-w64-x86_64-python for the self-checks)"
        exit 1
    }
    [ -x /c/Windows/System32/tar.exe ] || { echo "ERROR: Windows tar.exe (bsdtar) not found"; exit 1; }
    g++ --version | head -n 1
    # win32-model MinGW has no std::thread; crete's parallel decode would
    # link clean and throw on the first album.
    tm=$(g++ -v 2>&1 | sed -n 's/^Thread model: //p')
    [ "$tm" = posix ] || { echo "ERROR: g++ thread model is '$tm', need posix"; exit 1; }
    echo "thread model: posix"
}

stage_harness() {
    say "harness: make cli-json-ffmpeg STATIC=1"
    make clean >/dev/null
    make cli-json-ffmpeg STATIC=1
    check_version ./crete-ffmpeg.exe ffmpeg
    check_imports ./crete-ffmpeg.exe
    ./scripts/check_fix12.sh ./crete-ffmpeg.exe
    ./scripts/check_lfe_peak.sh ./crete-ffmpeg.exe
    # JSON round trip. The WAV path is passed as an ARGUMENT, which MSYS2
    # converts for the native python/crete; a path written INSIDE python
    # source would not be converted.
    local tmp; tmp=$(mktemp -d)
    python3 - "$tmp/t.wav" <<'PY'
import math, struct, sys, wave
w = wave.open(sys.argv[1], "wb")
w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
w.writeframes(b"".join(struct.pack("<hh", int(16000 * math.sin(i / 10)), int(8000 * math.sin(i / 10)))
                       for i in range(44100 * 12)))
w.close()
PY
    ./crete-ffmpeg.exe -f json "$tmp/t.wav" > "$tmp/t.json"
    python3 - "$tmp/t.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8"))
t = d["tracks"][0]
# Expected values measured on macOS and Windows (2026-09-25), identical.
assert round(t["peak_dbfs"], 4) == -6.2271, t["peak_dbfs"]
assert not d.get("warnings"), d.get("warnings")
print("JSON round trip ok:", d["version"], "peak", round(t["peak_dbfs"], 4))
PY
    rm -rf "$tmp"
    mkdir -p "$OUT/harness"
    mv -f crete-ffmpeg.exe "$OUT/harness/crete-json-ffmpeg.exe"
}

stage_ship() {
    say "ship: make cli-ffmpeg STATIC=1"
    rm -f crete-ffmpeg.exe crete-gui-ffmpeg.exe
    make cli-ffmpeg STATIC=1
    check_version ./crete-ffmpeg.exe ffmpeg
    check_imports ./crete-ffmpeg.exe

    say "ship: make gui-ffmpeg STATIC=1"
    pin_imgui
    make gui-ffmpeg STATIC=1
    check_imports ./crete-gui-ffmpeg.exe
}

stage_package() {
    say "package: $NAME.zip"
    local stage="$OUT/$NAME" f
    for f in crete-ffmpeg.exe crete-gui-ffmpeg.exe; do
        [ -f "$f" ] || { echo "ERROR: $f missing -- run the ship stage first"; exit 1; }
    done
    rm -rf "$stage" "$OUT/$NAME.zip" "$OUT/$NAME.zip.sha256"
    mkdir -p "$stage/licenses"
    # Named like the Linux/macOS packages: the FFmpeg tier IS `crete`.
    cp crete-ffmpeg.exe "$stage/crete.exe"
    cp crete-gui-ffmpeg.exe "$stage/crete-gui.exe"
    # The GUI loads its fonts from fonts\ beside the exe (crete_theme.hpp).
    cp -r fonts "$stage/fonts"
    cp LICENSE "$stage/LICENSE.txt"

    # Third-party notices. Missing ones are reported, not fatal: an absent
    # notice is a packaging bug to fix, not a reason to have no binaries.
    lic() {
        if [ -f "$1" ]; then cp "$1" "$stage/licenses/$2"
        else echo "WARNING: license not found: $1 (for licenses/$2)"; fi
    }
    lic third_party/libdsddpcm/LICENSE             libdsddpcm-BSD-2-Clause.txt
    lic third_party/ffmpeg-src/COPYING.LGPLv2.1    FFmpeg-LGPL-2.1.txt
    lic third_party/imgui/LICENSE.txt              DearImGui-MIT.txt
    lic /mingw64/share/licenses/SDL2/LICENSE.txt   SDL2-zlib.txt

    cat > "$stage/README.txt" <<EOF
crete $VERSION for Windows x86_64 ($SHA)

  crete.exe       command-line meter (FFmpeg tier: FLAC, WAV, AIFF, DSD, SACD ISO,
                  ALAC, AAC, MP3, TrueHD, DTS-HD, AC-3/E-AC-3, DVD title sets, ...)
  crete-gui.exe   the GUI. Keep the fonts folder next to it.

Both are single self-contained executables: no DLLs, no installer.
Run "crete.exe --help" for usage.

Source: https://codeberg.org/abksh/Crete (MIT). Third-party licenses are in
licenses\\. FFmpeg n$FFV (LGPL-2.1) is linked statically, configured by
scripts/build_ffmpeg_compact.sh in the crete source at this version; its
source is https://ffmpeg.org/releases/ffmpeg-$FFV.tar.xz.
EOF

    # Windows' own bsdtar writes a standard zip (forward-slash entry names,
    # unlike Windows PowerShell 5.1's Compress-Archive).
    (cd "$OUT" && /c/Windows/System32/tar.exe -a -c -f "$NAME.zip" "$NAME")
    (cd "$OUT" && sha256sum "$NAME.zip" > "$NAME.zip.sha256")
    rm -rf "$stage"
    /c/Windows/System32/tar.exe -t -f "$OUT/$NAME.zip"
    cat "$OUT/$NAME.zip.sha256"
    ls -la "$OUT"
}

for s in $STAGES; do
    case "$s" in
        preflight|harness|ship|package) "stage_$s" ;;
        *) echo "ERROR: unknown stage '$s'"; usage ;;
    esac
done
