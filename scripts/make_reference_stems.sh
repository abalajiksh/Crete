#!/usr/bin/env bash
# make_reference_stems.sh — split multichannel / dual-mono albums into stems that
# MAAT DROffline can actually read, for reference-log generation.
#
# MAAT analyses at most two channels (its log has LEFT / RIGHT columns and
# nothing else), so a 5.1 master cannot be referenced directly.  This splits each
# 5.1 track into three stereo stems that map onto the 5.1 channel order, so one
# MAAT folder run per stem group covers every channel:
#
#     [LR]     c0,c1  ->  MAAT LEFT = L,  RIGHT = R
#     [C-LFE]  c2,c3  ->  MAAT LEFT = C,  RIGHT = LFE   (LFE column ignored)
#     [Ls-Rs]  c4,c5  ->  MAAT LEFT = Ls, RIGHT = Rs
#
# A 2-channel album is instead split to a single 1-channel stem group [M] (c0),
# which is how a dual-mono master gets a genuine nch==1 reference.
#
# Extraction is `pan` with explicit channel indices — layout-name independent and
# bit-exact (verified by --verify, which round-trips each stem against its source
# channel through raw s32le and cmp).  Encoding is FLAC at the source word length.
#
# Usage:
#   scripts/make_reference_stems.sh <src-dir> <dest-dir> [--verify]
#
#   <src-dir>   directory of album directories (e.g. ~/SurroundMix), or a single
#               album directory containing the audio files
#   <dest-dir>  stems are written to <dest-dir>/<album> [<group>]/<same filename>
#
# Each stem group directory is a self-contained MAAT target: point DROffline at
# it, run a folder analysis, and drop the resulting _log.tsv beside the stems.

set -euo pipefail

SRC=""; DEST=""; VERIFY=0
for a in "$@"; do
    case "$a" in
        --verify) VERIFY=1 ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) if [ -z "$SRC" ]; then SRC="$a"; elif [ -z "$DEST" ]; then DEST="$a";
           else echo "unexpected argument: $a" >&2; exit 2; fi ;;
    esac
done
[ -n "$SRC" ] && [ -n "$DEST" ] || { echo "usage: $0 <src-dir> <dest-dir> [--verify]" >&2; exit 2; }
command -v ffmpeg  >/dev/null || { echo "ffmpeg not found" >&2; exit 1; }
command -v ffprobe >/dev/null || { echo "ffprobe not found" >&2; exit 1; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
MANIFEST="$DEST/STEMS.tsv"
mkdir -p "$DEST"
printf 'album\tgroup\tstem_channels\tsource_file\tstem_file\tsrc_channels\tsample_rate\tbits\n' > "$MANIFEST"

probe() { ffprobe -v error -select_streams a:0 -show_entries stream="$2" -of csv=p=0 "$1"; }

# extract <src> <out> <pan-expr>
extract() {
    ffmpeg -nostdin -hide_banner -v error -y -i "$1" -af "$3" -c:a flac -sample_fmt s32 "$2"
}

# verify_stem <src> <stem> <src-ch-index> <stem-ch-index>
verify_stem() {
    ffmpeg -nostdin -v error -y -i "$1" -af "pan=mono|c0=c$3" -f s32le "$TMP/a.raw"
    ffmpeg -nostdin -v error -y -i "$2" -af "pan=mono|c0=c$4" -f s32le "$TMP/b.raw"
    cmp -s "$TMP/a.raw" "$TMP/b.raw"
}

process_album() {
    local dir="$1" album; album="$(basename "$dir")"
    local first; first="$(find "$dir" -maxdepth 1 -type f \( -iname '*.flac' -o -iname '*.wav' -o -iname '*.aiff' -o -iname '*.aif' \) | sort | head -1)"
    [ -n "$first" ] || return 0
    local nch sr bits
    nch="$(probe "$first" channels)"
    sr="$(probe "$first" sample_rate)"
    bits="$(probe "$first" bits_per_raw_sample)"; [ -n "$bits" ] || bits="?"

    # group name -> "pan expr|src channel indices|stem channel count"
    local groups=()
    case "$nch" in
        6) groups=( "LR|pan=stereo|c0=c0|c1=c1#0,1"
                    "C-LFE|pan=stereo|c0=c2|c1=c3#2,3"
                    "Ls-Rs|pan=stereo|c0=c4|c1=c5#4,5" ) ;;
        8) groups=( "LR|pan=stereo|c0=c0|c1=c1#0,1"
                    "C-LFE|pan=stereo|c0=c2|c1=c3#2,3"
                    "Lss-Rss|pan=stereo|c0=c4|c1=c5#4,5"
                    "Lrs-Rrs|pan=stereo|c0=c6|c1=c7#6,7" ) ;;
        2) groups=( "M|pan=mono|c0=c0#0" ) ;;
        1) echo "  $album: already 1 channel — nothing to split"; return 0 ;;
        *) echo "  $album: unsupported channel count $nch — skipped" >&2; return 0 ;;
    esac

    echo "  $album: ${nch}ch ${sr}Hz ${bits}-bit -> ${#groups[@]} stem group(s)"
    local spec gname pan idx
    for spec in "${groups[@]}"; do
        gname="${spec%%|*}"; local rest="${spec#*|}"
        pan="${rest%%#*}"; local tail="${rest#*#}"
        idx="${tail%%#*}"
        local out="$DEST/$album [$gname]"
        mkdir -p "$out"
        local f base n=0
        while IFS= read -r f; do
            base="$(basename "$f")"; base="${base%.*}.flac"
            extract "$f" "$out/$base" "$pan"
            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                   "$album" "$gname" "$idx" "$f" "$out/$base" "$nch" "$sr" "$bits" >> "$MANIFEST"
            if [ "$VERIFY" -eq 1 ]; then
                local si=0 sc
                for sc in ${idx//,/ }; do
                    verify_stem "$f" "$out/$base" "$sc" "$si" \
                        || { echo "    VERIFY FAILED: $base c$sc" >&2; exit 1; }
                    si=$((si+1))
                done
            fi
            n=$((n+1))
        done < <(find "$dir" -maxdepth 1 -type f \( -iname '*.flac' -o -iname '*.wav' -o -iname '*.aiff' -o -iname '*.aif' \) | sort)
        echo "    [$gname] $n file(s)$([ "$VERIFY" -eq 1 ] && echo ' — bit-exact verified')"
    done
}

# A directory holding audio files directly is one album; otherwise recurse one level.
if find "$SRC" -maxdepth 1 -type f \( -iname '*.flac' -o -iname '*.wav' -o -iname '*.aiff' -o -iname '*.aif' \) | grep -q .; then
    process_album "$SRC"
else
    while IFS= read -r d; do process_album "$d"; done < <(find "$SRC" -maxdepth 1 -mindepth 1 -type d | sort)
fi

echo
echo "Stems written under: $DEST"
echo "Manifest:            $MANIFEST"
echo "Next: run MAAT DROffline folder analysis on each '<album> [<group>]' directory"
echo "      and leave the _log.tsv inside that directory."
