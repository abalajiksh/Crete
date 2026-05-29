# crête

Zero-dependency C++17 dynamic range meter. The name is French for *peak* / *crest* — the origin of the audio term *crest factor*.

Measures **TT Dynamic Range** (Pleasurize Music Foundation algorithm), **EBU R128 integrated loudness**, **true peak** (4× polyphase FIR), **PLR**, **PSR**, **LRA**, **crest factor**, and per-channel variants for lossless audio files. PCM output is compatible with [dr.loudness-war.info](http://dr.loudness-war.info/) submission format; DSD output uses the same algorithmic conventions as foobar2000's SACD plugin in Direct mode.

---

## Build

### CLI (zero dependencies)

```bash
make                    # optimized build, no JSON output
make VERSION=0.6.3      # custom version stamp
make debug              # debug build with sanitizers
make install            # install to /usr/local/bin
```

Single translation unit (`main.cpp`), no external libraries.

### CLI with JSON output

```bash
make cli-json           # auto-fetches nlohmann/json header into third_party/
```

Adds the `-f json` output mode for machine-readable analysis. The header is downloaded on first build; subsequent builds are offline. The CLI remains a single binary.

### GUI (Dear ImGui + SDL2)

```bash
# Install dependencies
sudo dnf install SDL2-devel mesa-libGL-devel    # Fedora
sudo apt install libsdl2-dev libgl-dev          # Ubuntu
brew install sdl2                               # macOS

# Get Dear ImGui (one-time)
make setup-imgui

# Build
make gui                # GUI app
make all                # cli-json + gui together
make debug-gui          # GUI with sanitizers
```

The GUI binary is `crete-gui`. It supports drag-and-drop, native file/folder dialogs, sortable results, color-coded DR values, and saves a log next to the analysed folder.

---

## Usage

### CLI

```bash
crete --version
crete /path/to/album/
crete -f foobar /path/to/album/
crete -f ext /path/to/album/
crete -f detail /path/to/album/
crete -f json /path/to/album/    # cli-json builds only
crete -o dr_log.txt /path/to/album/
crete -q /path/to/album/         # suppress progress, useful in CI
```

### GUI

```bash
crete-gui
```

Browse for a file or folder (or drag-and-drop), pick an output format, click Analyze.

---

## Supported Formats

| Format        | Extension          | Notes                                                                  |
|---------------|--------------------|------------------------------------------------------------------------|
| WAV           | `.wav`             | PCM 8/16/24/32-bit, IEEE float 32/64-bit, `WAVE_FORMAT_EXTENSIBLE`     |
| AIFF / AIFF-C | `.aif`, `.aiff`    | Standard big-endian; AIFF-C uncompressed (`NONE`) and `sowt` little-endian |
| FLAC          | `.flac`            | Built-in decoder — no libFLAC dependency                                |
| DSD (DSF)     | `.dsf`             | Sony format. Decimated to 352.8 kHz PCM via foobar's `fir1_8` / `fir1_16` |
| DSD (DFF)     | `.dff`             | Philips DSDIFF. Same decimation path as DSF, MSB-first deinterleaver    |
| Cue sheet     | `.cue`             | Splits a monolithic file into per-track results — see below             |

### Cue sheet support

When a folder contains a `.cue` file, crête:

1. Parses the cue (`FILE`, `TRACK`, `TITLE`, `PERFORMER`, `INDEX 01`).
2. Locates the referenced audio file (case-insensitive fallback).
3. Decodes that file once, then slices its PCM by track boundaries and runs the full analysis pipeline on each slice.

Each track is reported as `NN - Title.ext` (e.g. `01 - What A Shame.dff`), matching the per-track naming convention produced by `sacd_extract`, `flac --split`, and most other splitters — so a cue-sliced monolithic album pairs cleanly against the equivalent per-track album. Audio files in the same folder *not* referenced by any cue are still analysed individually.

This makes SACD rips (one big `.dff` + cue), CD images (one big `.flac` + cue), and vinyl rips work without any preprocessing.

Track times are stored internally as CDDA frames (1/75 sec) and converted to PCM samples at the actual decoded rate, so the same cue works for 44.1 kHz WAV and 352.8 kHz DSD-decoded PCM without rounding loss (every standard rate is divisible by 75).

> **Memory note.** A monolithic 60-minute DSD64 album decodes to ~21 GB of float64 PCM at 352.8 kHz before slicing. Cue-sliced analysis adds one slice (~1.7 GB) and the analyser's working set (~2× the slice) on top. Plan accordingly on memory-constrained hosts.

### Not yet supported

**ALAC** (`.m4a`): requires MP4/ISO BMFF container parsing. Convert first: `ffmpeg -i input.m4a -c:a flac output.flac`.

---

## Output Formats

**Official Album Data reference:** https://dr.loudness-war.info/album/view/200279

### Standard (`-f std`, default)

dr.loudness-war.info–compatible:

```
----------------------------------------------------------------------------------------------
 Analyzed Folder: /home/abalaji/Music/Random Access Memories/Disc 2
----------------------------------------------------------------------------------------------
DR         Peak       RMS        Filename
----------------------------------------------------------------------------------------------

DR11       -1.77 dB   -17.31 dB  01. Horizon Ouverture - Daft Punk.wav
DR10       -0.01 dB   -12.91 dB  02. Horizon (Japan CD) - Daft Punk.wav
DR9        over       -10.56 dB  03. GLBTM (Studio Outtakes) - Daft Punk.wav
DR7        -0.09 dB    -8.09 dB  04. Infinity Repeating (2013 Demo) - Daft Punk.wav
...
----------------------------------------------------------------------------------------------

 Number of Files: 9
 Official DR Value: DR9

==============================================================================================
```

### foobar2000 style (`-f foobar`)

Mimics foobar2000 DR Meter 1.1.1 output, with duration and per-album technical info block:

```
crête 0.6.3 / TT DR Offline Meter
log date: 2026-05-29 06:59:00

--------------------------------------------------------------------------------
Analyzed: Random Access Memories / Disc 2
--------------------------------------------------------------------------------

DR         Peak         RMS     Duration Track
--------------------------------------------------------------------------------
DR11        -1.77 dB   -17.31 dB     2:08 01. Horizon Ouverture - Daft Punk
DR10        -0.01 dB   -12.91 dB     4:22 02. Horizon (Japan CD) - Daft Punk
...
--------------------------------------------------------------------------------

Number of tracks:  9
Official DR value: DR9

Samplerate:        44100 Hz
Channels:          2
Bits per sample:   16
Codec:             WAV
================================================================================
```

### Extended (`-f ext`)

MAAT DROffline MkII–style pipe-delimited table. Twenty-six columns: per-channel sample peaks and true peaks, RMS, max momentary and short-term LUFS, LUFSi, DR (PMF), DR LEFT, DR RIGHT, PLR, LRA, Min PSR, and more. Ideal for parity testing against MAAT reference logs.

### Detail (`-f detail`)

Per-track block view with all per-channel metrics in a vertical table — easier to read for single-track investigation than the wide `ext` format.

### JSON (`-f json`, requires `make cli-json`)

Machine-readable output with every metric for every track, including the full per-channel `channel_metrics` array. Used by the pytest CI harness to compare against reference databases without scraping text output.

```json
{
  "version": "crête 0.6.3",
  "folder_path": "/home/abalaji/Music/Random Access Memories/Disc 2",
  "album_dr": 9,
  "num_tracks": 9,
  "sample_rate": 44100,
  "channels": 2,
  "bit_depth": 16,
  "codec": "WAV",
  "tracks": [
    {
      "filename": "01. Horizon Ouverture - Daft Punk.wav",
      "dr_score": 11,
      "dr_raw": 11.24,
      "peak_dbfs": -1.77,
      "rms_dbfs": -17.31,
      "integrated_lufs": -20.3,
      "max_true_peak_dbtp": -1.65,
      "plr_db": 18.6,
      "lra_lu": 4.2,
      "psr_db": 12.8,
      "channel_metrics": [
        { "label": "Left",  "sample_peak_dbfs": -1.77, "rms_dbfs": -17.42, ... },
        { "label": "Right", "sample_peak_dbfs": -1.81, "rms_dbfs": -17.20, ... }
      ],
      ...
    },
    ...
  ]
}
```

---

## Algorithm

### TT Dynamic Range

1. Split audio into **3-second non-overlapping blocks**.
2. Compute **RMS** (power-summed across channels) and **peak** per block.
3. Discard silent blocks (joint RMS < −60 dBFS).
4. Sort blocks by RMS, descending.
5. Select the **top 20 %** loudest blocks (`floor(0.2 · N)`).
6. **DR = 20 · log₁₀(2nd-highest block peak / mean of top-20 % RMS)**.
7. Round to integer.

The 2nd-highest peak (rather than the highest) follows the foobar2000 DR Meter v0.2 changelog and matches MAAT DROffline behavior on most material.

### EBU R128 integrated loudness

K-weighting (BS.1770-4) implemented via hardcoded biquad coefficients from libebur128, with absolute gating at −70 LUFS and relative gating at −10 LU below the ungated mean. 400 ms momentary blocks at 75 % overlap; 3 s short-term blocks at 67 % overlap.

### True peak

4× polyphase FIR (12 taps per phase) per channel; joint result is the max across channels.

### PLR / PSR / LRA / crest factor

PLR uses `min(max_true_peak, 0) − LUFSi` to match MAAT's clamp behavior on over-full-scale material. PSR is computed per 3-second short-term window with a −50 LUFS floor and reported as the minimum. LRA follows BS.1770-4 with linear-interpolated 95th/10th percentile after relative gating at −20 LU.

### DSD decimation

DSD bitstreams are decimated to **352.8 kHz** PCM using foobar2000 SACD plugin's `fir1_8` (DSD64, 80 taps) or `fir1_16` (DSD128, 160 taps) coefficients, normalized to unity DC gain. This matches foobar's Direct-mode output exactly — peaks are bit-accurate against foobar references. The Multistage 384 kHz mode is *not* supported (planned for v1.1, see `CRETE_TODO.md`).

A byte-LUT polyphase engine (`dsd_lut.hpp`) replaces per-bit FIR convolution with precomputed 256-entry lookup tables — roughly 8× faster than the naive implementation, with one thread per channel.

---

## DR Interpretation

| DR Score | Meaning                                            |
|----------|----------------------------------------------------|
| < 5      | Brickwalled — loudness war casualty                |
| 5–7      | Heavily compressed                                 |
| 8–13     | Typical commercial release                         |
| 14–20    | Audiophile / well-mastered                         |
| > 20     | Exceptional dynamics (classical, jazz, vinyl-era)  |

---

## Project Structure

```
main.cpp          CLI entry point, output formatters, cue-aware grouping
gui_main.cpp      GUI entry point (Dear ImGui + SDL2 + OpenGL)
analysis.hpp      TT DR, EBU R128, true peak, PLR/PSR/LRA, crest factor
audio.hpp         WAV, AIFF/AIFF-C, FLAC, DSF, DFF decoders
dsd_lut.hpp       Byte-LUT polyphase FIR engine for DSD → PCM decimation
cue.hpp           Zero-dependency cue sheet parser
file_dialog.hpp   Portable native file/folder dialogs (Linux/macOS/Windows)
Makefile          Build system (cli / cli-json / gui / all)
third_party/      Dear ImGui, nlohmann/json (fetched on demand)
```

The `analysis.hpp` and `audio.hpp` files are deliberately monolithic — single-translation-unit builds keep the CLI dependency-free and the build trivially reproducible across distros.

---

## License

MIT License — Ashwin Balaji 2026.

DSD decimation coefficients (`fir1_8`, `fir1_16` in `dsd_lut.hpp`) are derived from Maxim V. Anisiutkin's foobar2000 SACD plugin and are used under LGPL 2.1.
