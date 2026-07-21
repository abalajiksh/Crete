# crête

Zero-dependency C++17 dynamic range meter. The name is French for *peak* / *crest* — the origin of the audio term *crest factor*.

Measures **TT Dynamic Range** (Pleasurize Music Foundation algorithm), **EBU R128 integrated loudness**, **true peak** (4× polyphase FIR), **PLR**, **PSR**, **LRA**, **crest factor**, and per-channel variants for lossless audio files. PCM output is compatible with [dr.loudness-war.info](http://dr.loudness-war.info/) submission format; DSD is decimated by the vendored, BSD-2-Clause **[libdsddpcm](https://codeberg.org/abksh/libdsddpcm)** kernel — a clean-room linear-phase, unity-DC decoder — with selectable **chain** (Direct / Multistage), **filter**, and **output rate**.

The default build has **zero dependencies**. An optional third build tier statically links a compact, LGPL-only FFmpeg to add lossy/ALAC decode support — see below.

**Current version: 0.9.0**

---

## Build

Three CLI build tiers, one GUI:

| Target | Binary | Dependencies | Adds |
|--------|--------|--------------|------|
| `make` / `make cli` | `crete` | none | core analysis, text output formats |
| `make cli-json` | `crete` | nlohmann/json header (auto-fetched) | `-f json` machine-readable output |
| `make cli-json-ffmpeg` | `crete-ffmpeg` | compact static FFmpeg (see below) | ALAC / MP3 / AAC / Opus decode |
| `make gui` | `crete-gui` | SDL2, OpenGL, Dear ImGui | drag-and-drop GUI |

### CLI (zero dependencies)

```bash
make                    # optimized build, no JSON output
make VERSION=0.9.0      # custom version stamp
make debug              # debug build with sanitizers
make install            # install to /usr/local/bin
```

Single translation unit (`main.cpp`), no external libraries.

### CLI with JSON output

```bash
make cli-json           # auto-fetches nlohmann/json header into third_party/
```

Adds the `-f json` output mode for machine-readable analysis. The header is downloaded on first build; subsequent builds are offline. The CLI remains a single binary.

### CLI with FFmpeg decode (opt-in)

```bash
./build_ffmpeg_compact.sh    # one-time: builds a minimal static FFmpeg (default 7.1)
make cli-ffmpeg              # FFmpeg decode, no JSON
make cli-json-ffmpeg         # FFmpeg decode + JSON (binary: crete-ffmpeg)
```

The compact FFmpeg build is **LGPL-only** (no GPL, no nonfree components), **decode-only**, and statically linked — `crete-ffmpeg` is still a single self-contained binary. Point the build at a non-default FFmpeg install with `make cli-json-ffmpeg FFMPEG_PREFIX=/path/to/ffmpeg`. Requires `pkg-config`.

Decoder routing is strict:

- **Native formats (WAV, AIFF, FLAC, DSF, DFF) always use crête's own decoders**, even in the FFmpeg build. This preserves parity against the MAAT/foobar reference data.
- FFmpeg handles only non-native formats: **ALAC (`.m4a`), MP3, AAC, Opus**.
- **DSD is never routed through FFmpeg** — FFmpeg's `dsd2pcm` decimation is a different filter chain and would break DSD parity.

`crete-ffmpeg --version` advertises the linked FFmpeg as `[+ffmpeg-compact N.N]`.

The default `make` target stays zero-dependency; nothing about the FFmpeg tier is required for PCM/DSD analysis.

### GUI (Dear ImGui + SDL2)

```bash
sudo dnf install SDL2-devel mesa-libGL-devel    # Fedora
sudo apt install libsdl2-dev libgl-dev          # Ubuntu
brew install sdl2                               # macOS

make setup-imgui        # one-time: fetch Dear ImGui
make gui                # GUI app
make all                # cli-json + gui together
make debug-gui          # GUI with sanitizers
```

The GUI binary is `crete-gui`. It supports drag-and-drop, native file/folder dialogs, sortable results, color-coded DR values, and saves a log next to the analysed folder.

> **Note:** the GUI does not currently expose the `--dsd-*` options and analyses DSD with the defaults (**multistage**, `default` filter, **44.1 kHz**). See *Pending work*.

---

## Usage

### CLI

```bash
crete --version
crete /path/to/album/
crete -f foobar /path/to/album/
crete -f ext /path/to/album/
crete -f detail /path/to/album/
crete -f json /path/to/album/          # cli-json builds only
crete -o dr_log.txt /path/to/album/
crete -q /path/to/album/               # suppress progress, useful in CI

crete --dsd-mode multistage /path/to/dsd/           # default chain (measurement-grade)
crete --dsd-mode direct     /path/to/dsd/           # DSD64 -> 44.1 kHz only
crete --dsd-filter kaiser   /path/to/dsd/           # final-stage filter (44.1 kHz out only)
crete --dsd-out-rate 352800 /path/to/dsd/           # 44100 (default) |88200|176400|352800

crete-ffmpeg /path/to/album/           # cli-json-ffmpeg build: adds ALAC/MP3/AAC/Opus
```

`--dsd-mode` (chain), `--dsd-filter` (final-stage filter), and `--dsd-out-rate` select the DSD→PCM decode; none affect PCM formats. `crete --version` prints the resolved chain, decimation, tap count, and group delay for each DSD rate at the current settings. See *DSD decimation* below for the support matrix.

In `-f json`, DSD tracks carry `"dsd_mode"`, `"dsd_filter"`, and `"dsd_out_rate"` fields recording exactly how the numbers were produced — an unlabelled DSD result is not interpretable.

### GUI

```bash
crete-gui
```

Browse for a file or folder (or drag-and-drop), pick an output format, click Analyze.

---

## Supported Formats

### Native decoders (all builds)

| Format        | Extension          | Notes                                                                  |
|---------------|--------------------|------------------------------------------------------------------------|
| WAV           | `.wav`             | PCM 8/16/24/32-bit, IEEE float 32/64-bit, `WAVE_FORMAT_EXTENSIBLE`     |
| AIFF / AIFF-C | `.aif`, `.aiff`    | Standard big-endian; AIFF-C uncompressed (`NONE`) and `sowt` little-endian |
| FLAC          | `.flac`            | Built-in decoder — no libFLAC dependency                                |
| DSD (DSF)     | `.dsf`             | Sony format, LSB-first. Decimated via libdsddpcm (default 44.1 kHz, `--dsd-out-rate`) |
| DSD (DFF)     | `.dff`             | Philips DSDIFF, MSB-first. Same decode engine as DSF                    |
| Cue sheet     | `.cue`             | Splits a monolithic file into per-track results — see below             |

### FFmpeg decoders (`crete-ffmpeg` builds only)

| Format | Extension | Notes |
|--------|-----------|-------|
| ALAC   | `.m4a`    | Lossless — metrics agree with the FLAC equivalent to float noise |
| MP3    | `.mp3`    | Lossy — measured values reflect the decoded lossy signal |
| AAC    | `.m4a`, `.aac` | Lossy |
| Opus   | `.opus`, `.ogg` | Lossy |

In zero-dependency builds these formats are rejected with a hint to use `crete-ffmpeg` (or transcode: `ffmpeg -i input.m4a -c:a flac output.flac`).

### Cue sheet support

When a folder contains a `.cue` file, crête:

1. Parses the cue (`FILE`, `TRACK`, `TITLE`, `PERFORMER`, `INDEX 01`).
2. Locates the referenced audio file (case-insensitive fallback).
3. Decodes that file once, then slices its PCM by track boundaries and runs the full analysis pipeline on each slice.

Each track is reported as `NN - Title.ext` (e.g. `01 - What A Shame.dff`), matching the per-track naming convention produced by `sacd_extract`, `flac --split`, and most other splitters — so a cue-sliced monolithic album pairs cleanly against the equivalent per-track album. Audio files in the same folder *not* referenced by any cue are still analysed individually.

Track times are stored internally as CDDA frames (1/75 sec) and converted to PCM samples at the actual decoded rate, so the same cue works for 44.1 kHz WAV and 352.8 kHz DSD-decoded PCM without rounding loss (every standard rate is divisible by 75).

> **Memory note.** At the default 44.1 kHz DSD output a monolithic 60-minute DSD64 album decodes to ~2.6 GB of float64 PCM before slicing; at `--dsd-out-rate 352800` that rises 8× to ~21 GB. Cue-sliced analysis adds one slice plus the analyser's working set (~2× the slice) on top. Plan accordingly on memory-constrained hosts.

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
...
----------------------------------------------------------------------------------------------

 Number of Files: 9
 Official DR Value: DR9

==============================================================================================
```

### foobar2000 style (`-f foobar`)

Mimics foobar2000 DR Meter output, with duration and a per-album technical info block.

### Extended (`-f ext`)

MAAT DROffline MkII–style pipe-delimited table. Twenty-six columns: per-channel sample peaks and true peaks, RMS, max momentary and short-term LUFS, LUFSi, DR (PMF), DR LEFT, DR RIGHT, PLR, LRA, Min PSR, and more. Ideal for parity testing against MAAT reference logs.

### Detail (`-f detail`)

Per-track block view with all per-channel metrics in a vertical table — easier to read for single-track investigation than the wide `ext` format.

### JSON (`-f json`, requires `make cli-json` or `make cli-json-ffmpeg`)

Machine-readable output with every metric for every track, including the full per-channel `channel_metrics` array, and `dsd_mode` for DSD sources. Used by the pytest CI harness to compare against reference databases without scraping text output.

---

## Algorithm

### TT Dynamic Range

Per channel:

1. Split audio into **3-second non-overlapping blocks**.
2. Compute **RMS** (power-summed across channels) and **peak** per block.
3. Discard silent blocks (joint RMS < −60 dBFS).
4. Sort blocks by joint RMS, descending; take the **top 20 %** (`floor(0.2 · N)`) and average their RMS.
5. **DR(channel) = 20 · log₁₀(2nd-highest peak of that channel / mean top-20 % joint RMS)**.

Then:

6. **DR (PMF) = mean of the per-channel DR values**, rounded to integer.

The 2nd-highest peak (rather than the highest) follows the foobar2000 DR Meter v0.2 changelog.

> **Step 6 is not cosmetic.** Until v0.8.1, DR (PMF) was computed as a separate *joint* measurement — the 2nd-highest of the per-block **joint** peaks (`max` over channels) minus the same top-20 % RMS. Because `max` over channels is ≥ either channel's own peak and both paths subtract the same RMS, that construction guaranteed `DR (PMF) ≥ max(DR_LEFT, DR_RIGHT)` **structurally**: it could never fall below either channel. This produced the long-standing "crête reads +1, never −1" bias against both MAAT and foobar.
>
> Taking the channel mean instead matches foobar2000 DR Meter v1.0.8 on **20/20** I Robot track/mode pairs (Direct and Multistage), drops PCM `DR (PMF)` max Δ vs MAAT from **3.00 → 1.00 dB** and flagged tracks from **32/153 → 13/135**, and closes the rate-independent "Likoo +2 at every rate" discrepancy. The residual is now symmetric rounding noise rather than a one-sided bias.

### EBU R128 integrated loudness

K-weighting (BS.1770-4) implemented via hardcoded biquad coefficients from libebur128, with absolute gating at −70 LUFS and relative gating at −10 LU below the ungated mean. 400 ms momentary blocks at 75 % overlap; 3 s short-term blocks at 67 % overlap.

### True peak

4× polyphase FIR (12 taps per phase) per channel; joint result is the max across channels.

The result is clamped to be **≥ the sample peak**, which BS.1770 requires by definition (the reconstructed continuous waveform passes through every sample). The interpolation FIR is designed for audio-band content and attenuates energy near Nyquist, so without the clamp the oversampled peak can land *below* the sample peak on signals with heavy ultrasonic content. This never occurs on PCM or on DSD Direct; it does occur on **DSD Multistage**, whose 80-tap `fir1_8` leaves enough of DSD's shaped noise floor in place to trigger it (5 violations on I Robot, worst −0.143 dB). The clamp is a no-op wherever TP ≥ SP already, so the EBU 3341 true-peak battery is unaffected.

### PLR / PSR / LRA / crest factor

PLR uses `min(max_true_peak, 0) − LUFSi` to match MAAT's clamp behavior on over-full-scale material. PSR is computed per 3-second short-term window with a −50 LUFS floor and reported as the minimum. LRA follows BS.1770-4 after relative gating at −20 LU.

---

## DSD decimation

DSD bitstreams are decimated to PCM by **[libdsddpcm](https://codeberg.org/abksh/libdsddpcm) v0.1.0**, vendored under `third_party/libdsddpcm/` and statically linked. It is a clean-room, BSD-2-Clause kernel: linear-phase FIR only (constant group delay), **unity DC gain** (the +6 dB SACD makeup gain is *not* applied). crête keeps its own DSF/DFF container parsing and its one-thread-per-channel decode.

> **This replaces the former `dsd_lut.hpp`** (LGPL, transcribed from foobar's `foo_input_sacd`). DSD numbers **change** versus crête ≤ 0.8.1 — different clean-room filters and a different default output rate — so DSD reference baselines must be regenerated.

### Three axes: chain, filter, output rate

- **`--dsd-out-rate`** — `44100` (default) | `88200` | `176400` | `352800`.
- **`--dsd-mode`** (chain):
  - **`multistage`** (default) — LUT decimation to 8fs then a halving cascade; brickwall at Nyquist at 44.1 kHz output. **The measurement-grade path**, valid for DSD64/128/256 at every output rate.
  - **`direct`** — a single wide FIR with a gentle ~30 kHz corner (passes an attenuated top octave). **DSD64 → 44100 only** (a libdsddpcm restriction); unsupported at any other rate.
- **`--dsd-filter`** — the ranked final 88.2→44.1 kHz stage, honoured **only at 44100 Hz output** (higher rates accept `default` only):

  | Filter | Design | Taps | Notes |
  |--------|--------|------|-------|
  | `default` | equiripple (Parks–McClellan) | 383 | golden default, ~154 dB stopband |
  | `firls` | least-squares | 767 | cross-method check |
  | `kaiser` | Kaiser window (α≈150) | 4095 | reference/maximum decode, higher latency |

### Support matrix

| | 44100 | 88200 / 176400 / 352800 |
|--|-------|--------------------------|
| **multistage** | DSD64/128/256, any filter | DSD64/128/256, `default` filter only |
| **direct** | DSD64 only, `default` only | unsupported |

**DSD512 and above are not supported** (the library's input enum tops out at DSD256); such files are rejected with a clear error. Unsupported chain/filter/rate combinations fail fast with an explanatory message. `crete --version` prints the resolved chain, decimation, tap count, and group delay for the current settings.

### Validation

- **Round-trip:** crête on a `.dsf` and crête on the WAV `dsd2wav` exports from it agree exactly (both go through the same libdsddpcm decode).
- libdsddpcm ships its own clean-room validation (see its repo); crête's own DSD reference suites (formerly foobar-parity at 352.8 kHz) are **pending regeneration** against this engine — see `CRETE_PENDING.md`.

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

## Pending work

### Reference-data hygiene (highest priority)

Now the largest remaining source of disagreement. These are **corpus problems, not algorithm problems**.

- **`Min. PSR` — 101/135 flagged, max Δ 7.83 dB.** The worst case, `EMW_88k` track 15, has a reference value of exactly **`+0.00`** — a failed or missing MAAT export, not a measurement. Purge or re-capture before treating any PSR number as meaningful.
- **`EMW_96k` track 11.** The only PCM track that appears as an outlier across three unrelated metrics (RMS Δ 2.31, LRA Δ 2.94), and — since the FIX 9 bias was removed — the **only negative DR mismatch in the entire corpus** (crête DR16 / ref DR17). Confirm it is a clean downsample with a good reference before trusting it.

### Test harness

- **`BoneyM_10k_DSD128_352` reports PASS with zero comparisons.** Album DR and track count match, but no per-track metric rows are produced, so the album contributes nothing and still passes. A suite that compares nothing must fail.
- **Baseline-relative regression harness.** Commit per-metric deltas-vs-reference as `baselines/vX.Y.Z.json`; fail only when a metric *worsens* beyond baseline + ~0.1 dB. Replaces the per-metric "catastrophic" thresholds, which were a bootstrap.
- Keep one loose absolute backstop (~5 dB) per album suite to catch gross breakage.

### MAAT as a DSD metric oracle

The foobar `ext` export carries only **DR, RMS, and Peak**. Fourteen of crête's twenty-one metrics — LUFSi, LRA, PLR, PSR, TPL, M/S — are therefore **completely untested on DSD**, and no amount of foobar re-recording can change that.

Planned: export decimated PCM to 352.8 kHz WAV (`dsd2wav`) and measure it in MAAT alongside the PCM suite. This splits DSD validation into two independently-verifiable layers:

| Layer | Oracle |
|-------|--------|
| Decimation | `foo_input_sacd` source, bit-exact (`dsd_xcheck`) |
| Metrics on PCM | MAAT, on the exported WAV |

Caveats, by design:

- The exported WAV comes out of crête's own decimation, so this is **not** an end-to-end independent check. **The foobar DSD suite must be retained** — it is the only test that validates decimation *and* metrics against a tool sharing no code with crête.
- MAAT references generated this way are keyed to a specific `crete_version` + `dsd_mode`. Store both alongside the reference rows, gate on `dsd_xcheck`, and **never commit the exported WAVs as corpus** — regenerate them, or one decimation gets baked into the reference data permanently.
- Open questions: does MAAT accept 32-bit float WAV (its spec says "Linear PCM"), and does it accept 352.8 kHz without silently resampling? DSD decimation routinely exceeds ±1.0, so a 24-bit integer export would clip and destroy exactly the peak/SPPM values under test.

### Code

- **GUI does not expose the `--dsd-*` options** — `crete-gui` analyses DSD with the defaults (multistage / `default` filter / 44.1 kHz).
- `audio_ffmpeg.hpp` header comment still describes the old "foobar Direct = `fir1_8`/`fir1_16`" model; the routing logic is correct, the comment is not.
- `nlohmann/json` version split: MinIO seeds v3.12.0, the Makefile pins v3.11.3.
- Round-trip guard: crête on a `.dsf` and crête on the WAV exported from it must agree (measured 2.9e-8, float32 mantissa). Not yet a pytest case.

### Not yet exercised

- **DSD256 / DSD512.** Both chains are implemented and resolve correctly, but there is no corpus and no reference data. (`fir1_32` does **not** exist and is not needed — DSD256 Direct is `fir1_64 ÷32`.)
- **DFF container.** Decoder path exists; no dedicated parity test against the DSF version of the same album.
- **AIFF.** No test files.

### Closed — do not re-investigate

- ~~DR (PMF) floor-vs-ceil top-20 % block count~~ — never the problem. Per-channel DR was already correct to ≤0.44 dB; the bug was the channel collapse (FIX 9).
- ~~DSD64 track-pairing misalignment~~ — was the wrong decimation chain.
- ~~Boney M. 10k reference re-capture~~ — the reference was fine; crête's DSD128 path (`fir1_16 ÷16`) matched no foobar mode.
- ~~libsoxr / native polyphase resampler for Multistage~~ — there is no rational resampler. Multistage is FIRs only, zero-dependency, and implemented.
- ~~DSD reference re-recording at "Direct 352.8 kHz"~~ — foobar 2.0.24 cannot output 384/768 kHz at all.

---

## Project Structure

```
main.cpp                  CLI entry point, output formatters, cue-aware grouping
gui_main.cpp              GUI entry point (Dear ImGui + SDL2 + OpenGL)
analysis.hpp              TT DR, EBU R128, true peak, PLR/PSR/LRA, crest factor
audio.hpp                 WAV, AIFF/AIFF-C, FLAC, DSF, DFF decoders + FFmpeg routing
audio_ffmpeg.hpp          FFmpeg decode backend (guarded by CRETE_HAS_FFMPEG)
dsd_decode.hpp            DSD decode front-end over libdsddpcm (chain/filter/rate)
dsd_lut.hpp.bak           Retired LGPL byte-LUT engine (dormant, not built)
dsd2wav.cpp               DSD -> WAV exporter (make dsd2wav); mirrors --dsd-* flags
cue.hpp                   Zero-dependency cue sheet parser
file_dialog.hpp           Portable native file/folder dialogs (Linux/macOS/Windows)
Makefile                  Build system (cli / cli-json / cli-json-ffmpeg / gui / dsd2wav)
build_ffmpeg_compact.sh   One-time compact static FFmpeg build (LGPL-only, decode-only)
third_party/libdsddpcm/   Vendored BSD-2-Clause DSD decode kernel (committed, compiled in)
third_party/              Dear ImGui, nlohmann/json, compact FFmpeg (fetched on demand)
```

`analysis.hpp` and `audio.hpp` are deliberately monolithic — single-translation-unit builds keep the CLI dependency-free and the build trivially reproducible across distros. `audio_ffmpeg.hpp` compiles to nothing unless `CRETE_HAS_FFMPEG` is defined by an FFmpeg build tier.

---

## License

MIT License — Ashwin Balaji 2026.

**DSD decode** is provided by the vendored **libdsddpcm** (`third_party/libdsddpcm/`, release v0.1.0), which is **BSD-2-Clause** — a clean-room implementation that shares no code with the LGPL `foo_input_sacd` oracle used only offline for validation. Its license text ships at `third_party/libdsddpcm/LICENSE`. This replaces the former `dsd_lut.hpp` (LGPL-2.1, transcribed from foobar's `libdsdpcm`), which is retired to `dsd_lut.hpp.bak` and is no longer built. crête's licensing is therefore now cleanly **MIT + BSD-2-Clause**.

`crete-ffmpeg` builds statically link a compact FFmpeg configured LGPL-only (decode-only, no GPL or nonfree components); FFmpeg is used under LGPL 2.1+.
