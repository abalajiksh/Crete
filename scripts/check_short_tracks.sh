#!/usr/bin/env bash
# ============================================================================
# check_short_tracks.sh — self-check for the short-track DR rule (after
# 0.19.5), synthetic oracle only.  No reference corpus, no MAAT, no harness.
#
# THE RULE.  A track under 10 s has NO TT DR: crete prints DR--, writes JSON
# null, and leaves it out of the album mean.  10 s is MAAT DROffline's own
# limit ("Length is less than 10 seconds"), pinned 2026-09-26 with 28 synthetic
# tracks of 3..45 s: MAAT dropped exactly the five under 10 s and accepted a
# 10.000 s file.  See dr::MIN_DR_SECONDS in analysis.hpp.
#
# WHY IT MATTERED.  Kill Bill Vol. 1 ends with three sound-effect clips, and
# crete gave "Flip Sting" a raw DR of -100.47: two 3 s blocks, the second a
# silent tail, so the 2nd-highest block peak hit the -120 dB floor.  Averaged
# in, it took the album from MAAT's DR10 to DR4, unnoticed since 0.17.0.
#
# THE FIXTURE.  An album of three dynamic 20 s tracks, a clip of exactly
# 10.000 s, one of 9.990 s, and a 4 s clip whose last second is digital
# silence -- the Flip Sting shape.
#
# THE ORACLE is an identity, not a reference: an album that contains too-short
# clips must get EXACTLY the album DR of the same album without them.
#
#   1. Default: the 4 s and 9.99 s clips read DR--, the 10.000 s clip has a DR.
#   2. Default: album DR (with the clips) == album DR (clips removed).
#   3. --dr-short include reproduces <= 0.19.5: every track has a DR.
#   4. THE FIXTURE'S OWN PREMISE: under include the 4 s clip reads below
#      DR-50 and the album DR moves by at least 2.  Without this a fixture that
#      stopped reproducing the failure would pass 1-3 and gate nothing -- the
#      failure mode this project has shipped twice.
#
# Usage: scripts/check_short_tracks.sh [path-to-crete]     (default ./crete)
#        CRETE_FLAGS="--dr-blocks legacy" scripts/check_short_tracks.sh
#
# Passes under every --dr-blocks / --dr-mean setting; under blocks=legacy the
# premise (4) is reported as SKIP, because that block set discards the
# trailing partial block and cannot produce the -111 failure at all.
# ============================================================================
set -euo pipefail

CRETE="${1:-./crete}"
CRETE_FLAGS="${CRETE_FLAGS:-}"
[ -x "$CRETE" ] || { echo "no crete binary at $CRETE — run 'make' first"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/album" "$TMP/long_only"

# 16-bit 44.1 kHz stereo.  The "music" is a quiet tone with sparse loud bursts,
# so its DR sits around 10+ (a realistic album, not a DR4 brickwall).
python3 - "$TMP" <<'PY'
import math, struct, sys, wave, random, os, shutil
T = sys.argv[1]; SR = 44100
random.seed(1)

def write(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(b"".join(struct.pack("<hh", s, int(s * 0.9)) for s in samples))

def music(n, seed):
    rnd = random.Random(seed); out = []
    for i in range(n):
        t = i / SR
        burst = 0.85 if (i // (SR // 4)) % 11 == 0 else 0.08    # 1/4 s burst every 2.75 s
        s = burst * math.sin(2 * math.pi * 440 * t) + 0.01 * (rnd.random() * 2 - 1)
        out.append(max(-32767, min(32767, int(s * 32767))))
    return out

for k in (1, 2, 3):
    write(f"{T}/album/0{k}_track.wav", music(20 * SR, k))
write(f"{T}/album/04_exactly_10s.wav", music(10 * SR, 4))                  # 441000 samples: HAS a DR
write(f"{T}/album/05_just_under_10s.wav", music(int(9.99 * SR), 5))         # no DR
write(f"{T}/album/06_flip_sting_4s.wav", music(3 * SR, 6) + [0] * SR)       # 3 s + 1 s silence: no DR
for f in ("01_track.wav", "02_track.wav", "03_track.wav", "04_exactly_10s.wav"):
    shutil.copy(f"{T}/album/{f}", f"{T}/long_only/{f}")
PY

run() { "$CRETE" $CRETE_FLAGS "$@" 2>/dev/null; }
# The track's DR cell ("DR12", "DR-100", "DR--") from the standard layout.
cell() { awk -v f="$2" '$NF == f { print $1; exit }' <<<"$1"; }
album() { sed -n 's/.*Official DR Value: \(DR[-0-9]*\).*/\1/p' <<<"$1"; }

fail=0
ok()  { echo "PASS  $*"; }
bad() { echo "FAIL  $*"; fail=1; }

def=$(run "$TMP/album")
long=$(run "$TMP/long_only")
inc=$(run --dr-short include "$TMP/album")

# 1
c4=$(cell "$def" 06_flip_sting_4s.wav); c9=$(cell "$def" 05_just_under_10s.wav); c10=$(cell "$def" 04_exactly_10s.wav)
if [ "$c4" = "DR--" ] && [ "$c9" = "DR--" ] && [[ "$c10" =~ ^DR[0-9]+$ ]]; then
    ok "default: 4 s clip $c4, 9.99 s clip $c9, exactly-10 s clip $c10"
else
    bad "default: 4 s clip '$c4', 9.99 s clip '$c9', exactly-10 s clip '$c10' (want DR--, DR--, DR<n>)"
fi

# 2
ad=$(album "$def"); al=$(album "$long")
if [ -n "$ad" ] && [ "$ad" = "$al" ]; then
    ok "album with the short clips ($ad) == album without them ($al)"
else
    bad "album with the short clips '$ad' != album without them '$al'"
fi

# 3
if grep -q "DR--" <<<"$inc"; then
    bad "--dr-short include still prints DR-- (it must reproduce <= 0.19.5)"
else
    ok "--dr-short include: every track has a DR, as before 0.19.5"
fi

# 4 -- not applicable under --dr-blocks legacy, which discards the trailing
# partial block: the silent tail never becomes a block, so the failure this
# fixture reproduces cannot occur there (the clip reads a normal DR). 1-3
# still hold. Asked of the binary, not inferred from CRETE_FLAGS.
ci=$(cell "$inc" 06_flip_sting_4s.wav); ai=$(album "$inc")
v4=${ci#DR}; vd=${ad#DR}; vi=${ai#DR}
blocks=$("$CRETE" $CRETE_FLAGS --version 2>/dev/null | sed -n 's/.*DR settings: blocks=\([a-z]*\).*/\1/p')
if [ "$blocks" = legacy ]; then
    echo "SKIP  fixture premise: blocks=legacy drops the trailing block, so the silent-tail failure cannot occur"
elif [[ "$v4" =~ ^-?[0-9]+$ ]] && [ "$v4" -le -50 ] && [[ "$vd" =~ ^[0-9]+$ ]] && [[ "$vi" =~ ^-?[0-9]+$ ]] \
   && [ $(( vd - vi )) -ge 2 ]; then
    ok "fixture premise: under include the 4 s clip reads $ci and the album drops $ad -> $ai"
else
    bad "fixture premise: 4 s clip '$ci', album '$ad' -> '$ai' under include (want <= DR-50 and a drop >= 2)"
    echo "      the fixture no longer reproduces the Flip Sting failure — it gates nothing"
fi

[ "$fail" -eq 0 ] && echo "ALL CHECKS PASSED" || echo "CHECKS FAILED"
exit "$fail"
