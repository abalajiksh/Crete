#!/usr/bin/env python3
"""
make_sacd_test_image.py — build a REDUCED SACD image for the test corpus.

A real SACD image is ~4 GB, which is too much to pull on every weekly for a
suite that only needs to prove the Scarlet Book walk, the two frame formats
and the area selector still work. This takes a full image and emits a small
one holding the FIRST N TRACKS OF EACH AREA, with the multichannel area
relocated down so the file does not have to span the ~1.85 GB gap before it.

What is real in the output, and what is not:

  REAL   every audio sector, byte for byte -- the DSD packets, the DST frames,
         the sector headers with their DSD-vs-DST frame-info sizing, the
         byte-interleaved channel layout. The decoders see exactly what they
         see on the disc.
  EDITED the TOC fields that describe EXTENT: the master TOC's area pointers,
         each area's audio_start/audio_end/track_count/playing time, and the
         track list. Nothing else.

So the suite gates crete against real disc bytes; the edits only shorten the
disc. The output is NOT a playable SACD image and is not meant to be -- other
structures on the disc still reference the original LSNs.

The reduction is self-checking: pass --verify and every kept track is measured
in the reduced image and in the source image and the two JSON results must be
identical. That is what stops a mistake in this script from becoming a test
that gates the mistake.

Usage:
  scripts/make_sacd_test_image.py <source.iso> <out.iso>
        [--stereo-tracks N] [--mc-tracks N] [--verify <crete-binary>]
"""

import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path

SECTOR = 2048
MASTER_LSN = 510
# Master TOC area pointers, big-endian u32 at these byte offsets.
OFF_2CH, OFF_2CH_MIRROR, OFF_MC, OFF_MC_MIRROR = 64, 68, 72, 76
# Area TOC fields (see sacd.hpp parse_area — same offsets, same meanings).
OFF_PLAY_MIN, OFF_PLAY_SEC, OFF_PLAY_FRM = 64, 65, 66
OFF_TRACK_COUNT = 69
OFF_AUDIO_START, OFF_AUDIO_END = 72, 76
TRACK_SLOTS = 255


def rd32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def wr32(b, o, v):
    struct.pack_into(">I", b, o, v)


class Iso:
    def __init__(self, path):
        self.f = open(path, "rb")

    def read(self, lsn, count=1):
        self.f.seek(lsn * SECTOR)
        d = self.f.read(SECTOR * count)
        if len(d) != SECTOR * count:
            raise SystemExit(f"short read at LSN {lsn} (+{count})")
        return d


def parse_area(iso, toc_lsn, magic):
    """Mirror of sacd.hpp's parse_area, reading only what this tool rewrites."""
    b = iso.read(toc_lsn)
    if b[:8] != magic:
        raise SystemExit(f"no {magic.decode()} at LSN {toc_lsn}")
    blk = iso.read(toc_lsn, 16)
    trl = None
    for s in range(16):
        if blk[s * SECTOR:s * SECTOR + 8] == b"SACDTRL1":
            trl = s
            break
    if trl is None:
        raise SystemExit(f"{magic.decode()}: no SACDTRL1 within 16 sectors")
    count = b[OFF_TRACK_COUNT]
    at = trl * SECTOR
    tracks = []
    for i in range(count):
        st = rd32(blk, at + 8 + 4 * i)
        ln = rd32(blk, at + 8 + 4 * TRACK_SLOTS + 4 * i)
        if ln:
            tracks.append((st, ln))
    return {
        "toc_lsn": toc_lsn,
        "trl_sector": trl,
        "channels": b[32],
        "frame_format": b[21] & 0x0F,
        "audio_start": rd32(b, OFF_AUDIO_START),
        "audio_end": rd32(b, OFF_AUDIO_END),
        "tracks": tracks,
    }


def set_playtime(buf, seconds):
    """Playing time is stored as minutes / seconds / 1-75 frames."""
    total = max(0.0, seconds)
    m = int(total // 60)
    s = int(total - m * 60)
    fr = int(round((total - m * 60 - s) * 75))
    if fr > 74:
        fr, s = 0, s + 1
    if s > 59:
        s, m = 0, m + 1
    buf[OFF_PLAY_MIN], buf[OFF_PLAY_SEC], buf[OFF_PLAY_FRM] = m & 0xFF, s & 0xFF, fr & 0xFF


def build(src, dst, n_stereo, n_mc):
    iso = Iso(src)
    master = bytearray(iso.read(MASTER_LSN))
    if master[:8] != b"SACDMTOC":
        raise SystemExit("source has no SACDMTOC at LSN 510 — not an SACD image")

    two_lsn, mc_lsn = rd32(master, OFF_2CH), rd32(master, OFF_MC)
    if not two_lsn:
        raise SystemExit("source declares no 2-channel area")
    stereo = parse_area(iso, two_lsn, b"TWOCHTOC")
    mc = parse_area(iso, mc_lsn, b"MULCHTOC") if mc_lsn else None

    keep_st = stereo["tracks"][:n_stereo]
    if not keep_st:
        raise SystemExit("no stereo tracks to keep")
    # The stereo area keeps its ORIGINAL LSNs: it already sits near the start
    # of the disc, so truncating after its last kept track is enough.
    st_end = keep_st[-1][0] + keep_st[-1][1] - 1
    out_len = st_end + 1

    keep_mc = mc["tracks"][:n_mc] if (mc and n_mc) else []
    mc_new_toc = mc_src_first = mc_src_last = 0
    if keep_mc:
        # Relocate the multichannel area as ONE contiguous block: its TOC
        # through the end of the last kept track. Copying the block whole
        # keeps every intra-block offset valid, so only absolute LSNs move.
        mc_src_first = mc["toc_lsn"]
        mc_src_last = keep_mc[-1][0] + keep_mc[-1][1] - 1
        mc_new_toc = out_len
        out_len += mc_src_last - mc_src_first + 1
    delta = mc_src_first - mc_new_toc if keep_mc else 0

    print(f"source stereo: {len(stereo['tracks'])} tracks, keeping {len(keep_st)}")
    if mc:
        print(f"source mc    : {len(mc['tracks'])} tracks, keeping {len(keep_mc)}"
              f" (frame_format {mc['frame_format']}"
              f"{' = DST' if mc['frame_format'] == 0 else ''})")
    print(f"output       : {out_len} sectors = {out_len * SECTOR / 1e6:.1f} MB")

    # ── Copy, then patch ───────────────────────────────────────────────────
    with open(dst, "wb") as o:
        o.write(iso.read(0, st_end + 1))
        if keep_mc:
            # Copy in chunks: the block can be hundreds of MB.
            todo = mc_src_last - mc_src_first + 1
            at = mc_src_first
            while todo:
                n = min(todo, 4096)
                o.write(iso.read(at, n))
                at += n
                todo -= n

    with open(dst, "r+b") as o:
        def put(lsn, buf):
            o.seek(lsn * SECTOR)
            o.write(bytes(buf))

        # Master TOC: point at the relocated multichannel area, and drop both
        # MIRROR pointers — the mirrors live at the far end of a full disc and
        # are gone. crete reads only the two primaries, so this is hygiene
        # rather than a requirement.
        wr32(master, OFF_2CH_MIRROR, 0)
        wr32(master, OFF_MC, mc_new_toc if keep_mc else 0)
        wr32(master, OFF_MC_MIRROR, 0)
        put(MASTER_LSN, master)

        # Stereo area TOC: shorten the extent and the track list in place.
        b = bytearray(iso.read(stereo["toc_lsn"]))
        wr32(b, OFF_AUDIO_END, st_end)
        b[OFF_TRACK_COUNT] = len(keep_st)
        kept_sectors = st_end - keep_st[0][0] + 1
        set_playtime(b, kept_sectors * SECTOR / (352800.0 * stereo["channels"]))
        put(stereo["toc_lsn"], b)

        trl = bytearray(iso.read(stereo["toc_lsn"] + stereo["trl_sector"]))
        for i in range(len(keep_st), TRACK_SLOTS):
            wr32(trl, 8 + 4 * i, 0)
            wr32(trl, 8 + 4 * TRACK_SLOTS + 4 * i, 0)   # length 0 = unused slot
        put(stereo["toc_lsn"] + stereo["trl_sector"], trl)

        if keep_mc:
            b = bytearray(iso.read(mc["toc_lsn"]))
            wr32(b, OFF_AUDIO_START, mc["audio_start"] - delta)
            wr32(b, OFF_AUDIO_END, mc_src_last - delta)
            b[OFF_TRACK_COUNT] = len(keep_mc)
            # DST is compressed, so playing time cannot be derived from the
            # byte count the way the stereo area's can. Sum the kept tracks'
            # own declared durations instead, taken as a proportion of the
            # source area's playing time is not available here either -- use
            # the uncompressed-equivalent of each track's frame count, which
            # is what the disc's own per-track timing amounts to.
            secs = sum(t[1] for t in keep_mc) * SECTOR / (352800.0 * mc["channels"])
            set_playtime(b, secs)
            put(mc_new_toc, b)

            trl = bytearray(iso.read(mc["toc_lsn"] + mc["trl_sector"]))
            for i, (st, ln) in enumerate(keep_mc):
                wr32(trl, 8 + 4 * i, st - delta)
                wr32(trl, 8 + 4 * TRACK_SLOTS + 4 * i, ln)
            for i in range(len(keep_mc), TRACK_SLOTS):
                wr32(trl, 8 + 4 * i, 0)
                wr32(trl, 8 + 4 * TRACK_SLOTS + 4 * i, 0)
            put(mc_new_toc + mc["trl_sector"], trl)

    return keep_st, keep_mc


def verify(crete, src, dst, n_stereo, n_mc):
    """Measure both images and require identical JSON for the kept tracks.

    This is the whole reason the script can be trusted: if a rewritten LSN,
    a mis-sized TOC field or a bad copy offset changed what the decoder sees,
    the numbers move and this fails. A reduced image that measures identically
    to the disc it came from IS the disc, as far as crete is concerned.
    """
    ok = True
    for area, n in (("stereo", n_stereo), ("multichannel", n_mc)):
        if not n:
            continue
        runs = []
        for img in (src, dst):
            r = subprocess.run(
                [str(crete), "-q", "-f", "json", "--sacd-area", area, str(img)],
                capture_output=True, text=True)
            if r.returncode not in (0, 2) or not r.stdout.strip():
                print(f"  {area}: crete failed on {Path(img).name}: "
                      f"rc={r.returncode} {r.stderr.strip()[:200]}")
                ok = False
                break
            runs.append(json.loads(r.stdout))
        if len(runs) != 2:
            continue
        full, red = runs
        # The reduced image holds the first n tracks; compare against the
        # first n of the source, field by field.
        a = [dict(t) for t in full["tracks"][:n]]
        b = [dict(t) for t in red["tracks"][:n]]
        if len(b) != n:
            print(f"  {area}: reduced image yielded {len(b)} tracks, expected {n}")
            ok = False
            continue
        for x, y in zip(a, b):
            x.pop("filename", None)
            y.pop("filename", None)
        if a == b:
            print(f"  {area}: {n} track(s) IDENTICAL to the source image")
        else:
            ok = False
            for i, (x, y) in enumerate(zip(a, b)):
                for k in x:
                    if x[k] != y.get(k):
                        print(f"  {area}: track {i+1} {k}: {x[k]} -> {y.get(k)}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("out")
    ap.add_argument("--stereo-tracks", type=int, default=2)
    ap.add_argument("--mc-tracks", type=int, default=1)
    ap.add_argument("--verify", metavar="CRETE",
                    help="measure both images and require identical results")
    a = ap.parse_args()

    keep_st, keep_mc = build(a.source, a.out, a.stereo_tracks, a.mc_tracks)
    print(f"wrote {a.out} ({Path(a.out).stat().st_size / 1e6:.1f} MB)")

    if a.verify:
        print("verifying against the source image (this decodes both) ...")
        if not verify(a.verify, a.source, a.out, len(keep_st), len(keep_mc)):
            sys.exit("VERIFY FAILED — the reduced image does not measure like its source")
        print("verify OK")


if __name__ == "__main__":
    main()
