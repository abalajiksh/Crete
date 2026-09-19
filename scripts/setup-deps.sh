#!/usr/bin/env bash
# ============================================================================
# setup-deps.sh — install what crête needs to build, on macOS and Linux
#
# crête's default tier is zero-dependency by design: a C++17 compiler, a C99
# compiler and make will build it. Everything this script installs beyond that
# belongs to an OPTIONAL tier, and the script says which:
#
#   cli      g++ (C++17), gcc (C99, for the vendored libdsddpcm), make
#   json     + curl            (the Makefile fetches nlohmann/json single-header)
#   ffmpeg   + pkg-config, nasm/yasm, git   (builds the bundled compact FFmpeg)
#   gui      + SDL2 dev, OpenGL dev, git    (SDL2 + Dear ImGui)
#   cross    + mingw-w64       (Windows .exe from Linux/macOS; POSIX thread model)
#
# Usage:
#   ./scripts/setup-deps.sh                 # check + install the default tiers
#   ./scripts/setup-deps.sh --check         # report only, change nothing
#   ./scripts/setup-deps.sh --dry-run       # print the exact commands, run none
#   ./scripts/setup-deps.sh --tier gui      # one tier (repeatable)
#   ./scripts/setup-deps.sh --tier all      # every tier including cross
#   ./scripts/setup-deps.sh --yes           # don't prompt before installing
#   ./scripts/setup-deps.sh --fetch         # also fetch imgui/nlohmann + build
#                                           #   the compact FFmpeg (slow, ~5 min)
#
# Default tiers: cli json ffmpeg gui.  'cross' is opt-in.
#
# This script NEVER installs anything without showing you the command first,
# and it never re-runs a package manager for something already present. The
# package installs need root on Linux; it calls sudo for those lines only, and
# prints them so you can run them yourself instead.
#
# Deliberately bash 3.2 compatible (that is what macOS ships) and clean under
# `zsh`, so the same file serves both. No associative arrays, no ${var^^}.
# ============================================================================

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ── Options ─────────────────────────────────────────────────────────────────
TIERS=""
CHECK_ONLY=0
DRY_RUN=0
ASSUME_YES=0
DO_FETCH=0

while [ $# -gt 0 ]; do
    case "$1" in
        --check)    CHECK_ONLY=1 ;;
        --dry-run)  DRY_RUN=1 ;;
        --yes|-y)   ASSUME_YES=1 ;;
        --fetch)    DO_FETCH=1 ;;
        --tier)     shift; [ $# -gt 0 ] || { echo "--tier needs a value" >&2; exit 2; }
                    TIERS="$TIERS $1" ;;
        --tier=*)   TIERS="$TIERS ${1#--tier=}" ;;
        -h|--help)  sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)          echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
    shift
done

[ -n "$TIERS" ] || TIERS="cli json ffmpeg gui"
case " $TIERS " in *" all "*) TIERS="cli json ffmpeg gui cross" ;; esac

# Validate before anything else: an unknown tier would otherwise reach
# "check_$t" as a nonexistent function and fail as a confusing 127.
for _t in $TIERS; do
    case "$_t" in
        cli|json|ffmpeg|gui|cross|all) ;;
        *) echo "unknown tier '$_t' (want: cli json ffmpeg gui cross all)" >&2; exit 2 ;;
    esac
done
TIERS="$(echo $TIERS | tr ' ' '\n' | grep -v '^all$' | awk '!seen[$0]++' | tr '\n' ' ')"

want() { case " $TIERS " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

# ── Pretty output ───────────────────────────────────────────────────────────
if [ -t 1 ]; then
    B=$(printf '\033[1m'); R=$(printf '\033[0m')
    GRN=$(printf '\033[32m'); YEL=$(printf '\033[33m'); RED=$(printf '\033[31m')
else
    B=""; R=""; GRN=""; YEL=""; RED=""
fi
say()  { printf '%s\n' "$*"; }
head2() { printf '\n%s== %s%s\n' "$B" "$*" "$R"; }
ok()   { printf '  %sok%s    %s\n'   "$GRN" "$R" "$*"; }
miss() { printf '  %sMISS%s  %s\n'   "$RED" "$R" "$*"; }
note() { printf '  %s..%s    %s\n'   "$YEL" "$R" "$*"; }

have() { command -v "$1" >/dev/null 2>&1; }

# ── Platform + package manager ──────────────────────────────────────────────
OS="$(uname -s)"
PM=""
PM_INSTALL=""
SUDO=""
DISTRO=""

case "$OS" in
    Darwin)
        PM="brew"
        PM_INSTALL="brew install"
        ;;
    Linux)
        # /etc/os-release ID_LIKE is what distinguishes the families; fall back
        # to probing for the binary, since a container may lack os-release.
        if [ -r /etc/os-release ]; then
            # shellcheck disable=SC1091
            DISTRO="$(. /etc/os-release 2>/dev/null; printf '%s' "${ID:-}")"
        fi
        if   have dnf;    then PM="dnf";    PM_INSTALL="dnf install -y"
        elif have apt-get;then PM="apt";    PM_INSTALL="apt-get install -y"
        elif have pacman; then PM="pacman"; PM_INSTALL="pacman -S --needed --noconfirm"
        elif have zypper; then PM="zypper"; PM_INSTALL="zypper install -y"
        fi
        [ "$(id -u)" = "0" ] || SUDO="sudo"
        ;;
    MINGW*|MSYS*|CYGWIN*)
        say "This looks like MSYS2/Cygwin. Use the PowerShell script instead:"
        say "    powershell -ExecutionPolicy Bypass -File scripts\\setup-deps.ps1"
        say "or, if you are already inside an MSYS2 shell, install directly:"
        say "    pacman -S --needed mingw-w64-x86_64-gcc make pkgconf nasm git mingw-w64-x86_64-SDL2"
        exit 1
        ;;
    *)
        say "Unsupported platform: $OS. crête needs only a C++17 compiler and make;"
        say "install those with whatever this system uses and run 'make'."
        exit 1
        ;;
esac

# ── Package names, per tier, per platform ───────────────────────────────────
# Bash 3.2 has no associative arrays, so this is a function returning a
# space-separated list. Empty means "nothing to install for this tier here".
pkgs_for() {
    _tier="$1"
    case "$PM:$_tier" in
        brew:cli)      echo "" ;;                       # Xcode CLT, handled below
        brew:json)     echo "" ;;                       # curl ships with macOS
        brew:ffmpeg)   echo "pkg-config nasm" ;;
        brew:gui)      echo "sdl2" ;;                   # OpenGL is in the SDK
        brew:cross)    echo "mingw-w64" ;;

        dnf:cli)       echo "gcc-c++ gcc make" ;;
        dnf:json)      echo "curl" ;;
        dnf:ffmpeg)    echo "pkgconf-pkg-config nasm git" ;;
        dnf:gui)       echo "SDL2-devel mesa-libGL-devel git" ;;
        dnf:cross)     echo "mingw64-gcc-c++" ;;

        apt:cli)       echo "build-essential" ;;
        apt:json)      echo "curl" ;;
        apt:ffmpeg)    echo "pkg-config nasm git" ;;
        apt:gui)       echo "libsdl2-dev libgl-dev git" ;;
        apt:cross)     echo "g++-mingw-w64-x86-64" ;;

        pacman:cli)    echo "base-devel" ;;
        pacman:json)   echo "curl" ;;
        pacman:ffmpeg) echo "pkgconf nasm git" ;;
        pacman:gui)    echo "sdl2 mesa git" ;;
        pacman:cross)  echo "mingw-w64-gcc" ;;

        zypper:cli)    echo "gcc-c++ gcc make" ;;
        zypper:json)   echo "curl" ;;
        zypper:ffmpeg) echo "pkgconf-pkg-config nasm git" ;;
        zypper:gui)    echo "libSDL2-devel Mesa-libGL-devel git" ;;
        zypper:cross)  echo "mingw64-cross-gcc-c++" ;;

        *)             echo "" ;;
    esac
}

# ── What is already satisfied ───────────────────────────────────────────────
# Checked by CAPABILITY, not by package name: a compiler installed from source,
# an SDL2 from a different package, a nasm already on PATH all count.
check_cli() {
    _bad=0
    if have g++ || have clang++; then
        ok "C++ compiler   $( (g++ --version 2>/dev/null || clang++ --version) | head -1 )"
    else
        miss "C++ compiler (need C++17)"; _bad=1
    fi
    # The vendored libdsddpcm is C99 and the Makefile compiles it with $(CC).
    if have gcc || have cc || have clang; then
        ok "C compiler     $( (gcc --version 2>/dev/null || cc --version) | head -1 )"
    else
        miss "C compiler (need C99, for third_party/libdsddpcm)"; _bad=1
    fi
    if have make; then ok "make           $(make --version 2>/dev/null | head -1)"
    else miss "make"; _bad=1; fi
    return $_bad
}

check_json()   { if have curl; then ok "curl           $(curl --version 2>/dev/null | head -1 | cut -d' ' -f1-2)"; return 0
                 else miss "curl (Makefile fetches nlohmann/json with it)"; return 1; fi; }

check_ffmpeg() {
    _bad=0
    if have pkg-config || have pkgconf; then ok "pkg-config     present"
    else miss "pkg-config"; _bad=1; fi
    # nasm is only needed on x86: FFmpeg's configure ERRORS OUT on x86_64
    # without an assembler, because build_ffmpeg_compact.sh does not pass
    # --disable-x86asm. On aarch64 it is irrelevant.
    case "$(uname -m)" in
        x86_64|amd64|i?86)
            if have nasm || have yasm; then ok "nasm/yasm      present (x86 needs it for FFmpeg asm)"
            else miss "nasm (FFmpeg configure FAILS on x86_64 without it)"; _bad=1; fi ;;
        *) ok "nasm/yasm      not needed on $(uname -m)" ;;
    esac
    if have git; then ok "git            present"; else miss "git (clones the FFmpeg source)"; _bad=1; fi
    return $_bad
}

check_gui() {
    _bad=0
    if (have pkg-config && pkg-config --exists sdl2 2>/dev/null); then
        ok "SDL2           $(pkg-config --modversion sdl2)"
    elif [ "$OS" = "Darwin" ] && have brew && brew --prefix sdl2 >/dev/null 2>&1; then
        ok "SDL2           $(brew --prefix sdl2)"
    else
        miss "SDL2 development files"; _bad=1
    fi
    if [ "$OS" = "Darwin" ]; then
        ok "OpenGL         macOS SDK framework"
    elif (have pkg-config && pkg-config --exists gl 2>/dev/null); then
        ok "OpenGL         $(pkg-config --modversion gl)"
    else
        miss "OpenGL development files"; _bad=1
    fi
    if have git; then ok "git            present (clones Dear ImGui)"; else miss "git"; _bad=1; fi
    return $_bad
}

check_cross() {
    if have x86_64-w64-mingw32-g++; then
        # The threading model is the ONLY thing that has ever mattered for
        # crete on Windows: the win32 model ships a libstdc++ with no
        # std::thread at all, and crete's parallel decode is the default path.
        _tm="$(x86_64-w64-mingw32-g++ -v 2>&1 | sed -n 's/^Thread model: //p')"
        if [ "$_tm" = "posix" ]; then
            ok "mingw-w64      thread model: posix (std::thread works)"
            return 0
        fi
        miss "mingw-w64 thread model is '$_tm' — need 'posix'; win32 has no std::thread"
        return 1
    fi
    miss "x86_64-w64-mingw32-g++"
    return 1
}

# ── Report ──────────────────────────────────────────────────────────────────
say "${B}crête build dependencies${R}   ($OS${DISTRO:+ / $DISTRO}, $(uname -m), pm: ${PM:-none})"
say "tiers: $TIERS"

NEED=""
for t in $TIERS; do
    head2 "tier: $t"
    if "check_$t"; then :; else NEED="$NEED $t"; fi
done

# macOS: a missing compiler is the Command Line Tools, not a brew package.
if [ "$OS" = "Darwin" ] && ! (have g++ || have clang++); then
    head2 "Xcode Command Line Tools"
    note "not installed — this provides clang/clang++/make"
    say  "  run:  xcode-select --install"
    say  "  (a GUI installer opens; re-run this script once it finishes)"
fi

if [ -z "$NEED" ]; then
    head2 "result"
    ok "every requested tier is already satisfied"
    if [ "$DO_FETCH" = "0" ]; then
        say ""
        say "Build with:  make            (zero-dep CLI)"
        want json   && say "             make cli-json"
        want ffmpeg && say "             make setup-ffmpeg && make cli-json-ffmpeg"
        want gui    && say "             make gui"
        exit 0
    fi
else
    head2 "result"
    say "  unsatisfied tiers:$NEED"
fi

# ── Install ─────────────────────────────────────────────────────────────────
if [ "$CHECK_ONLY" = "1" ]; then
    say ""
    say "--check given; nothing installed."
    exit 1
fi

if [ -n "$NEED" ]; then
    if [ -z "$PM" ]; then
        say ""
        say "No supported package manager found (dnf/apt/pacman/zypper/brew)."
        say "Install the missing items above by hand, then re-run with --check."
        exit 1
    fi
    if [ "$PM" = "brew" ] && ! have brew; then
        say ""
        say "Homebrew is not installed. Install it first:"
        say '    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'
        exit 1
    fi

    PKGS=""
    for t in $NEED; do PKGS="$PKGS $(pkgs_for "$t")"; done
    # de-duplicate, preserving order (bash 3.2: no associative array)
    UNIQ=""
    for p in $PKGS; do
        case " $UNIQ " in *" $p "*) ;; *) UNIQ="$UNIQ $p" ;; esac
    done
    UNIQ="${UNIQ# }"

    if [ -z "$UNIQ" ]; then
        say ""
        say "Nothing this script can install covers the gap above — see the notes."
    else
        CMD="$SUDO $PM_INSTALL $UNIQ"
        CMD="${CMD# }"
        say ""
        say "${B}Command to install the missing packages:${R}"
        say "    $CMD"
        if [ "$PM" = "apt" ]; then
            say "    (preceded by: ${SUDO:+$SUDO }apt-get update)"
        fi

        if [ "$DRY_RUN" = "1" ]; then
            say ""
            say "--dry-run given; nothing executed."
            exit 0
        fi

        if [ "$ASSUME_YES" != "1" ]; then
            printf '\nRun it now? [y/N] '
            read -r reply
            case "$reply" in
                y|Y|yes|YES) ;;
                *) say "Aborted. The command above is safe to run yourself."; exit 1 ;;
            esac
        fi

        if [ "$PM" = "apt" ]; then
            # shellcheck disable=SC2086
            $SUDO apt-get update || exit 1
        fi
        # shellcheck disable=SC2086
        $CMD || { say "${RED}package install failed${R}"; exit 1; }

        head2 "re-checking"
        STILL=""
        for t in $TIERS; do "check_$t" >/dev/null 2>&1 || STILL="$STILL $t"; done
        if [ -n "$STILL" ]; then
            say "  still unsatisfied:$STILL — re-run with --check for detail"
        else
            ok "all requested tiers satisfied"
        fi
    fi
fi

# ── Optional: fetch sources and build the compact FFmpeg ────────────────────
if [ "$DO_FETCH" = "1" ]; then
    if want gui; then
        head2 "Dear ImGui"
        if [ -d "$ROOT/third_party/imgui" ]; then
            ok "already present at third_party/imgui"
        elif [ "$DRY_RUN" = "1" ]; then
            say "    make -C '$ROOT' setup-imgui"
        else
            make -C "$ROOT" setup-imgui || exit 1
        fi
    fi
    if want json; then
        head2 "nlohmann/json"
        if [ -f "$ROOT/third_party/nlohmann/json.hpp" ]; then
            ok "already present"
        elif [ "$DRY_RUN" = "1" ]; then
            say "    make -C '$ROOT' setup-nlohmann"
        else
            make -C "$ROOT" setup-nlohmann || exit 1
        fi
    fi
    if want ffmpeg; then
        head2 "compact FFmpeg (LGPL-only, decode-only; several minutes)"
        if [ -f "$ROOT/third_party/ffmpeg-compact/crete-ffmpeg-build.id" ]; then
            ok "already built — delete third_party/ffmpeg-compact to force a rebuild"
        elif [ "$DRY_RUN" = "1" ]; then
            say "    '$ROOT/scripts/build_ffmpeg_compact.sh'"
        else
            "$ROOT/scripts/build_ffmpeg_compact.sh" || exit 1
        fi
    fi
fi

head2 "next"
say "  make                     zero-dependency CLI"
want json   && say "  make cli-json            + JSON output"
want ffmpeg && say "  make setup-ffmpeg        build the bundled compact FFmpeg (once)"
want ffmpeg && say "  make cli-json-ffmpeg     + JSON + FFmpeg decode (what the harness needs)"
want gui    && say "  make gui                 GUI app"
want cross  && say "  make cli CROSS=x86_64-w64-mingw32- STATIC=1    Windows .exe"
say "  ./scripts/check_fix12.sh verify the DR self-check passes"
exit 0
