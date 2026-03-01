# crête

Zero-dependency C++17 dynamic range meter. The name is French for *peak* / *crest* — the origin of the audio term *crest factor*.

Measures **TT Dynamic Range** (Pleasurize Music Foundation algorithm), **EBU R128 integrated loudness**, and **crest factor** for lossless audio files. Output is compatible with [dr.loudness-war.info](http://dr.loudness-war.info/) submission format.

## Build

```bash
make                    # optimized build
make VERSION=0.2.0      # custom version stamp
make debug              # debug build with sanitizers
make install            # install to /usr/local/bin
```

No external libraries required. Single compilation unit, ~1800 lines.

## Usage

```bash
# Check version
crete --version

# Analyze a folder (dr.loudness-war.info format)
crete /path/to/album/

# foobar2000 DR Meter style output
crete -f foobar /path/to/album/

# Extended output (includes LUFS, PLR, crest factor)
crete -f ext /path/to/album/

# Save to file for submission
crete -o dr_log.txt /path/to/album/
```

## Supported Formats

| Format | Extension | Notes |
|--------|-----------|-------|
| WAV    | `.wav`    | PCM 8/16/24/32-bit, IEEE float 32/64-bit, WAVE_FORMAT_EXTENSIBLE |
| AIFF   | `.aif`, `.aiff` | Standard AIFF and AIFF-C (uncompressed + `sowt`) |
| FLAC   | `.flac`   | Built-in decoder — no libFLAC dependency |
| DSD    | `.dsf`    | Sony DSF format. Decimated to PCM (DSD64→88.2k, DSD128→176.4k) |
| DSD    | `.dff`    | Philips DSDIFF format. Same decimation as DSF |

### Not Yet Supported

**ALAC** (`.m4a`): Requires MP4/ISO BMFF container parsing. Convert first: `ffmpeg -i input.m4a -c:a flac output.flac`

## Output Formats

### Standard (`-f std`, default)
Compatible with dr.loudness-war.info submission:
```
DR         Peak       RMS        Filename
DR9        over       -10.30 dB  01 - Give Life Back to Music.flac
```

### foobar2000 (`-f foobar`)
Mimics foobar2000 DR Meter 1.1.1 output with duration and technical info.

### Extended (`-f ext`)
All metrics in one table — DR, peak, RMS, integrated loudness (LUFS), peak-to-loudness ratio, crest factor, duration.

## Algorithm

The TT Dynamic Range algorithm:

1. Split each channel into **3-second non-overlapping blocks**
2. Compute **RMS** and **peak** per block
3. Discard silent blocks (RMS < −60 dBFS)
4. Sort blocks by RMS, descending
5. Select the **top 20%** loudest blocks
6. **DR = peak_dBFS − RMS_dBFS** of those top-20% blocks
7. Average across channels, round to integer

Additionally computed: EBU R128 integrated loudness with K-weighting and dual gating (ITU-R BS.1770-4), crest factor, and PLR.

## Interpretation

| DR Score | Meaning |
|----------|---------|
| < 5      | Brickwalled — loudness war casualty |
| 5–7      | Heavily compressed |
| 8–13     | Typical commercial release |
| 14–20    | Audiophile / well-mastered |
| > 20     | Exceptional dynamics |

## License

Public domain / CC0.
