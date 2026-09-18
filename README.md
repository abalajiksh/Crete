# crête

Zero-dependency C++17 dynamic range meter. The name is French for *peak* / *crest* — the origin of the audio term *crest factor*.

Measures **TT Dynamic Range** (Pleasurize Music Foundation algorithm), **EBU R128 integrated loudness**, **true peak** (4× polyphase FIR), **PLR**, **PSR**, **LRA**, **crest factor**, and per-channel variants for lossless audio files. PCM output is compatible with [dr.loudness-war.info](http://dr.loudness-war.info/) submission format; DSD is decimated by the vendored, BSD-2-Clause **[libdsddpcm](https://codeberg.org/abksh/libdsddpcm)** kernel — a clean-room linear-phase, unity-DC decoder — with selectable **chain** (Direct / Multistage), **filter**, and **output rate**.

crête also **detects MQA-encoded files** and reports them unconditionally: if you are measuring dynamic range, you are entitled to know the source is lossy MQA rather than lossless. Detection is deterministic — it reconstructs the buried MQA bitstream (XOR of the stereo low bits) and verifies the `datasync` packet, then reports the authored sample rate and the green/Studio authentication level. It never asserts MQA from statistics alone, so it does not false-positive on noisy-but-lossless masters (e.g. analog tape hiss). No open MQA decoder exists, so DR is measured on the delivered PCM stream; the warning makes clear the encoded content above the base band is not reconstructed. See *MQA detection* below.

The default build has **zero dependencies**. An optional third build tier statically links a compact, LGPL-only FFmpeg to add lossy/ALAC decode support — see below.

**Current version: 0.13.0**

---

## Build

Three CLI build tiers, one GUI:

| Target | Binary | Dependencies | Adds |
|--------|--------|--------------|------|
| `make` / `make cli` | `crete` | none | core analysis, text output formats |
| `make cli-json` | `crete` | nlohmann/json header (auto-fetched) | `-f json` machine-readable output |
| `make cli-json-ffmpeg` | `crete-ffmpeg` | compact static FFmpeg (see below) | ALAC / MP3 / AAC / Opus decode |
| `make gui` | `crete-gui` | SDL2, OpenGL, Dear ImGui | drag-and-drop GUI |
| `make gui-ffmpeg` | `crete-gui-ffmpeg` | SDL2 + compact static FFmpeg | GUI with ALAC / MP3 / AAC / Opus decode |

### CLI (zero dependencies)

```bash
make                    # optimized build, no JSON output
make VERSION=0.13.0     # custom version stamp
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

- **Native formats (WAV/RF64/Wave64, AIFF, FLAC, DSF, DFF) always use crête's own decoders**, even in the FFmpeg build. This preserves parity against the MAAT reference data.
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

The GUI binary is `crete-gui`. Its Modernist redesign lays the app out as horizontal bands — source bar, DSD strip, verdict band, DR meter-bar table + grouped detail panel — with a light/dark shell. It supports drag-and-drop, native file/folder dialogs, sortable results, and DR meter bars. Log saving is opt-in (Settings → "Save analysis log to disk"); by default results stay in the window only.

- **DSD strip** — appears only for DSD sources and exposes the three decode axes (chain / filter / output rate), matching the CLI's `--dsd-*` flags; picking *Direct* auto-restricts filter and rate to the one valid combo.
- **Parallel analysis** — an album's files are decoded and analysed across a thread pool (`hardware_concurrency`), preserving file order. Measurement is unchanged (bit-identical to sequential); it is purely faster (~5–6× on a typical album). DSD files still fan out per-channel inside each decode. The CLI uses the same pool since 0.10.1 (`-j` to cap it).

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
crete -j 2 /path/to/album/             # cap parallel file workers (default: all cores)

crete --dsd-mode multistage /path/to/dsd/           # default chain (measurement-grade)
crete --dsd-mode direct     /path/to/dsd/           # DSD64 -> 44.1 kHz only
crete --dsd-filter kaiser   /path/to/dsd/           # final-stage filter (44.1 kHz out only)
crete --dsd-out-rate 352800 /path/to/dsd/           # 44100 (default) |88200|176400|352800

crete --dr-mean arithmetic  /path/to/album/         # legacy <=0.12.1 DR averaging (A/B only)
crete --dr-blocks legacy    /path/to/album/         # pre-0.14.0 3 s block set (A/B only)

crete-ffmpeg /path/to/album/           # cli-json-ffmpeg build: adds ALAC/MP3/AAC/Opus
```

`--dsd-mode` (chain), `--dsd-filter` (final-stage filter), and `--dsd-out-rate` select the DSD→PCM decode; none affect PCM formats. `crete --version` prints the resolved chain, decimation, tap count, and group delay for each DSD rate at the current settings. See *DSD decimation* below for the support matrix.

`--dr-mean` selects how the loudest 20 % of blocks are averaged when computing DR: `quadratic` (default, the PMF procedure) or `arithmetic` (what crête shipped up to 0.12.1). It applies to every format, not just DSD. See *TT Dynamic Range* below.

`--dr-blocks` selects **which** 3 s blocks are ranked in the first place: `reference` (default since 0.14.0 — the PMF reference implementation: the trailing partial block is kept, normalised by its own length, and there is no gate) or `legacy` (what crête shipped up to 0.13.1: the trailing partial block is discarded and blocks are gated at `joint_rms > 1e-3`). This is a separate axis from `--dr-mean`. See *TT Dynamic Range* below.

In `-f json`, every result carries `"dr_rms_mean"` and `"dr_blocks"`, and DSD tracks additionally carry `"dsd_mode"`, `"dsd_filter"`, and `"dsd_out_rate"` — recording exactly how the numbers were produced. An unlabelled DSD result is not interpretable.

Since 0.10.1 the CLI analyses an album's files **in parallel** (one worker per core by default), same as the GUI: results are order-preserving and bit-identical to a sequential run — `-j` only changes wall-clock time, never a number. Use `-j N` to cap workers (e.g. `-j 2` for high-rate DSD output on memory-constrained hosts, since each in-flight file holds its full decoded float64 buffer — see the memory note under *Cue sheet support*), or `-j 1` for a strictly sequential run.

### GUI

```bash
crete-gui
```

Browse for a file or folder (or drag-and-drop) and click Analyze. The log output format lives in Settings → Log Format; a DSD Guide side panel documents the decimation chain, filters, and support matrix.

Since 0.11.0 results stay in the window only by default — writing a log file is opt-in ("Save analysis log to disk" in Settings, with the analysis-folder/custom-folder destination choice appearing once enabled). The About panel documents the measurement standards behind each reported metric.

`make gui-ffmpeg` builds `crete-gui-ffmpeg`, the GUI with the same compact LGPL-only FFmpeg tier as `crete-ffmpeg`: folder scans, drag-and-drop, and the file dialogs additionally accept `.m4a .mp4 .m4b .aac .mp3 .opus .ogg .oga .mka .webm .wma .wv .ape .mpc .tak`, with the same strict routing (native formats always use crête's own decoders; DSD is never routed through FFmpeg). The header shows `+ffmpeg`, and the About panel lists the linked FFmpeg version and every extra codec (ALAC, AAC, MP3, Opus, Vorbis, WMA v1/v2/Pro/Lossless, WavPack, APE, Musepack, TAK).

---

## Supported Formats

### Native decoders (all builds)

| Format        | Extension          | Notes                                                                  |
|---------------|--------------------|------------------------------------------------------------------------|
| WAV           | `.wav`             | PCM 8/16/24/32-bit, IEEE float 32/64-bit, `WAVE_FORMAT_EXTENSIBLE`     |
| RF64 / BW64   | `.wav`, `.rf64`, `.bw64` | 64-bit sizes via `ds64` — the >4 GB escape hatch (EBU Tech 3306 / ITU-R BS.2088). Same payload as WAV |
| Wave64        | `.w64`             | Sony Wave64: GUID chunk ids and 64-bit sizes. Same payload as WAV       |
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
| TrueHD / MLP | `.mkv`, `.thd` | Lossless disc audio, up to 7.1. **Channel bed only** — see below |
| DTS / DTS-HD MA | `.mkv`, `.dts` | Core + lossless MA extension |
| E-AC-3 / AC-3 | `.mkv`, `.mp4`, `.eac3`, `.ec3`, `.ac3` | Lossy disc audio. **Channel bed only** |

`.mkv` and `.mp4` are video containers; crête ignores video and subtitles, so music Blu-ray rips work directly.

#### Choosing the audio stream

A music Blu-ray commonly ships the same album several times over. One DSOTM remux carries six audio streams — LPCM 5.1 96 kHz, DTS-HD MA 5.1, TrueHD 7.1 Atmos, an AC-3 core, and stereo LPCM and DTS-HD MA — and **these are different mixes with different DR**, not different encodings of one mix.

With no `--stream`, crête takes FFmpeg's own pick, which ranks by bitrate and channel count. That is a reasonable default but it is not the container's "default" flag and it is not stable across remuxes of the same content: on the file above it selects the 13.8 Mbps LPCM track, while on a 60-second copy of the same six streams it selects the TrueHD one. So whenever a file holds more than one audio stream, crête **says which one it measured**:

```
Warning: album.mkv has 6 audio streams; measured stream 2 (PCM_S24LE, 6 ch, 96000 Hz). Use --stream N to choose.
```

`--stream N` pins it, using the index `ffprobe -show_streams` prints. The chosen index and the stream count also appear in the detail output and in JSON (`stream_index`, `audio_stream_count`), so a measurement is reproducible from its own record. Single-stream files are unaffected — no warning, and no extra JSON keys.


In zero-dependency builds these formats are rejected with a hint to use `crete-ffmpeg` (or transcode: `ffmpeg -i input.m4a -c:a flac output.flac`).

#### Dolby DRC is disabled

AC-3 and E-AC-3 carry Dolby dynamic-range-control metadata, and FFmpeg's decoder **applies it by default**. That is right for a player and wrong for a meter — DRC is a playback-time compressor driven by authoring metadata, so measuring it means measuring a rendition that is not on the disc. crête pins `drc_scale=0` (and `heavy_compr`, `target_level`) so it measures the encoded audio.

The difference is large and easy to miss: on one JOC 5.1 bed, leaving DRC on moved **peak by 6.2 dB** (−0.75 → −6.91), RMS by 3.9 dB and integrated loudness by 4.0 LU — while DR itself moved only 0.24, because DR is peak-minus-RMS and DRC scales both. If you compare crête against a player's readout, this is usually why they differ.

#### Object audio (Atmos, DTS:X) — the bed is measured, the objects are not

FFmpeg has no Atmos or DTS:X renderer. It decodes the underlying **channel bed** — TrueHD 7.1, or E-AC-3 5.1 for Dolby Digital Plus + Atmos — and discards the object metadata. crête's numbers for such a file are the bed's numbers, which is exactly what a non-Atmos playback chain delivers, and *not* what an Atmos renderer would produce. Do not read a bed measurement as an Atmos measurement.

#### Counting the LFE toward DR

crête's DR (PMF) for a 5.1 or 7.1 file is the mean of the **scored** channels — everything but the LFE. A `.1` track is a band-limited effects channel whose crest factor is nearly flat and whose level is mix-dependent, so averaging it into the programme's dynamic range measures the mixer's bass management rather than the music. TT DR defines nothing past stereo, so this is a chosen rule, and crête says which rule produced which number rather than presenting one as *the* multichannel DR.

foobar2000's DR Meter chooses the other way. `--dr-lfe include` (GUI: **Settings → SCORING**) matches it, so the two can be compared directly:

| DSOTM 5.1 96 kHz | DR raw | scored | lowest |
|---|---|---|---|
| `--dr-lfe exclude` (default) | 11.9772 | 5 | Ls |
| `--dr-lfe include` | 11.8606 | 6 | LFE |

It affects **TT DR only**: the DR mean, the joint sample peak and overall RMS behind it, the 3 s block gate and `worst_channel`. Integrated loudness, Max M/S, LRA and Min. PSR do not move at all — BS.1770 weights the LFE at zero by ITU-R BS.1770-4, which is a standard rather than a preference. Mono and stereo have no LFE, so the option is a no-op there and no stereo measurement can change.

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

Per-track block view with all per-channel metrics in a vertical table — easier to read for single-track investigation than the wide `ext` format. Since 0.11.0 each joint value carries its governing-standard tag, MAAT-style — `(ITU)` BS.1770-4, `(EBU)` Tech 3342, `(AES)` loudness-ratio conventions, `(PMF)` TT Dynamic Range — with a legend in the summary footer; per-channel Momentary/Short-Term columns are informative `(non-ITU)` variants.

### JSON (`-f json`, requires `make cli-json` or `make cli-json-ffmpeg`)

Machine-readable output with every metric for every track, including the full per-channel `channel_metrics` array, and `dsd_mode` / `dsd_filter` / `dsd_out_rate` for DSD sources. Used by the pytest CI harness to compare against reference databases without scraping text output. Since 0.11.0 a top-level `standards` map documents which specification governs each reported field.

---

## Algorithm

### TT Dynamic Range

Per channel:

1. Split audio into **3-second non-overlapping blocks**.
2. Compute **RMS** (power-summed across channels) and **peak** per block.
3. Discard silent blocks (joint RMS < −60 dBFS).
4. Sort **that channel's own** block RMS values, descending; take the **top 20 %** (`floor(0.2 · N)`) and combine them with the **quadratic mean** — `sqrt(mean(rms²))`, i.e. averaging *power*, not amplitude.
5. **DR(channel) = 20 · log₁₀(2nd-highest peak of that channel / that channel's top-20 % RMS)**.

Then:

6. **DR (PMF) = mean of the per-channel DR values**, rounded to integer.

The 2nd-highest peak (rather than the highest) follows the foobar2000 DR Meter v0.2 changelog.

> **Step 6 is not cosmetic.** Until v0.8.1, DR (PMF) was computed as a separate *joint* measurement — the 2nd-highest of the per-block **joint** peaks (`max` over channels) minus the same top-20 % RMS. Because `max` over channels is ≥ either channel's own peak and both paths subtract the same RMS, that construction guaranteed `DR (PMF) ≥ max(DR_LEFT, DR_RIGHT)` **structurally**: it could never fall below either channel. This produced the long-standing "crête reads +1, never −1" bias against both MAAT and foobar.
>
> Taking the channel mean instead matches foobar2000 DR Meter v1.0.8 on **20/20** I Robot track/mode pairs (Direct and Multistage), drops PCM `DR (PMF)` max Δ vs MAAT from **3.00 → 1.00 dB** and flagged tracks from **32/153 → 13/135**, and closes the rate-independent "Likoo +2 at every rate" discrepancy. The residual is now symmetric rounding noise rather than a one-sided bias.

> **Step 4 changed in v0.13.0.** Up to 0.12.1 the top-20 % blocks were combined with an *arithmetic* mean of RMS **amplitudes**. The PMF procedure averages **power**. Since AM ≤ QM, the old form was always biased low, so DR was always biased **high** — by an amount that grows with block-to-block RMS variance: ~0.03 dB on dense stereo, up to 1.47 dB on sparse channels (LFE, centre, surrounds).
>
> It was found by cross-checking a 5.1 corpus against foobar2000 DR Meter v1.0.8 and MAAT DROffline simultaneously. Over 264 per-channel values the two references agree with **each other** to a mean of 0.006 dB (median 0.000, 162 exact) while crête sat 0.095 from both — so the residual was crête's, not reference noise. Switching to the quadratic mean removes the bias (mean Δ +0.056 → −0.011) and improves per-channel DR accuracy against MAAT by **27–38 %** across the corpus.
>
> `--dr-mean arithmetic` reproduces ≤ 0.12.1 bit-for-bit, for A/B work and for re-reading old reference data.

> **Steps 1–2 have a second axis: which blocks exist at all (`--dr-blocks`). Changed in v0.14.0.** Up to 0.13.1 crête's block set differed from the PMF reference implementation (`dr14_t.meter`) in two places: it discarded the trailing partial block, and it gated blocks at `joint_rms > 1e-3`. The reference keeps the tail (RMS normalised by its own length) and has no gate.
>
> Both deviations shrink the block count, and since `n_top = floor(nb × 0.2)` a smaller count keeps **fewer and therefore louder** top blocks — raising the top-20 % RMS and **lowering DR**. The effect grows as the discarded blocks become a larger fraction of the track, so it is worst on short tracks and on sparse channels.
>
> Measured on the 5.1 corpus (45 tracks × 6 channels) against foobar2000 DR Meter v1.0.8 and MAAT DROffline simultaneously — `reference` reproduces a literal transcription of `dr14_t.meter` to 3.9e-5:
>
> | block set | vs foobar | vs MAAT (stems) |
> |---|---|---|
> | `legacy` (≤ 0.13.1) | mean \|Δ\| 0.0598, bias −0.011 | 0.0628, bias −0.014 |
> | `reference` (default) | **0.0355, bias −0.004** | **0.0339, bias −0.001** |
>
> It is not a surround-only effect — the Turtles stereo album improves 0.0472 → 0.0221 against MAAT — and it accounts for what earlier analysis recorded as a *short-track effect*: `corr(1/duration, |Δ|)` falls from +0.465 to −0.044.
>
> **The full corpus decided it.** Two weeklies, same binary, same corpus, run back to back — #68 under `legacy`, #69 under `reference`. Weighted mean \|Δ\| over **2241 DR comparisons: 0.0392 → 0.0262 (−33 %)**, DR flags 29 → 19, integer `DR (PMF)` flags 10 → 5, and **not one DR row regressed**. The crête-vs-crête control suites (`cue`, `dsd_parity`, `dsd_roundtrip`, `ffmpeg_parity`, `format`) stayed at 0.000 and **no non-DR value moved anywhere**, which is what confirms the change is confined to the block accounting. `reference` is the default from v0.14.0; `--dr-blocks legacy` reproduces ≤ 0.13.1 bit-for-bit, for A/B work and for re-reading reference rows recorded under it.

### EBU R128 integrated loudness

K-weighting (BS.1770-4) implemented via hardcoded biquad coefficients from libebur128, with absolute gating at −70 LUFS and relative gating at −10 LU below the ungated mean. 400 ms momentary blocks at 75 % overlap; 3 s short-term blocks at 67 % overlap.

### True peak

4× polyphase FIR (12 taps per phase) per channel; joint result is the max across channels.

The result is clamped to be **≥ the sample peak**, which BS.1770 requires by definition (the reconstructed continuous waveform passes through every sample). The interpolation FIR is designed for audio-band content and attenuates energy near Nyquist, so without the clamp the oversampled peak can land *below* the sample peak on signals with heavy ultrasonic content. This never occurs on PCM or on DSD Direct; it does occur on **DSD Multistage**, whose 80-tap `fir1_8` leaves enough of DSD's shaped noise floor in place to trigger it (5 violations on I Robot, worst −0.143 dB). The clamp is a no-op wherever TP ≥ SP already, so the EBU 3341 true-peak battery is unaffected.

### Channel layouts — mono, stereo, 5.1, 7.1

crête measures every channel a decoder hands it and assumes the canonical WAVE / ITU
interleave order (`channels.hpp`, shared by the CLI, the JSON and the GUI so they cannot
describe a layout differently):

| ch | layout | order |
|----|--------|-------|
| 1 | mono | M |
| 2 | stereo | L R |
| 3 / 4 / 5 | 3.0 / quad / 5.0 | L R C · L R Ls Rs · L R C Ls Rs |
| 6 | 5.1 | L R C **LFE** Ls Rs |
| 8 | 7.1 | L R C **LFE** Lss Rss Lrs Rrs |

Layouts outside this list are measured per channel but carry no speaker semantics: no LFE
is assumed and every channel is scored at unity weight.

**Loudness** follows ITU-R BS.1770-4 channel weighting: front channels (L, R, C) unity,
surrounds +1.5 dB, **LFE excluded**. BS.1770-4 tabulates weights only to 5.1; the 7.1 rears
take the same +1.5 dB as the sides, as EBU and broadcast meters do. Mono and stereo weights
are all unity, so the weighted sum is *bit-identical* to the unweighted code it replaced —
the stereo corpus is untouched by construction, not by measurement. Cross-checked against
ffmpeg's `ebur128` on a 5.1 master: I −13.57 vs −13.6 LUFS, LRA 8.22 vs 8.2 LU.

**Dynamic range.** TT DR / dr.loudness-war.info define no procedure beyond stereo, so crête
states its rule rather than implying one. `DR (PMF)` is the **mean of the scored channels**,
whose two-channel case *is* the stereo rule (FIX 9), with the **LFE excluded** — a
band-limited effects channel has a nearly flat crest factor, so folding it in would measure
bass management rather than the music. Two companions are reported alongside it, never in
place of it:

- **Front L/R DR** (`dr_front_raw`) — mean of L and R alone, the figure directly comparable
  with the stereo release of the same master.
- **Lowest scored channel** (`worst_channel`) — what a mastering check wants flagged, since
  a mean cannot fall below its worst member.

The LFE is measured in full — its own peak, RMS and DR are reported (bracketed in
`-f detail`, `scored: false` in JSON) — and scored in nothing. Joint peak, overall RMS and
the 3 s block gate likewise run over the scored channels only.

### PLR / PSR / LRA / crest factor

PLR uses `min(max_true_peak, 0) − LUFSi` to match MAAT's clamp behavior on over-full-scale material. PSR follows AES eBrief 373 — the 3-second window's sample peak minus its BS.1770-4 short-term loudness, minimum over the track, with a −50 LUFS floor. LRA follows EBU Tech 3342 after relative gating at −20 LU.

Since 0.11.0, Max Short-Term, LRA, and Min PSR share one short-term block grid (3 s window at the EBU 3341 ≥10 Hz meter rate, 0.1 s hop) computed once per track. Previously LRA and PSR sampled the same window on a private 1.0 s grid, which biased Min PSR high (the true minimum window could fall between hops) and thinned the LRA percentile distribution on sparse material. All other metrics are bit-identical across the change; crête passes the official EBU Tech 3341/3342 vector suites on the new grid.

---

## DSD decimation

DSD bitstreams are decimated to PCM by **[libdsddpcm](https://codeberg.org/abksh/libdsddpcm) v0.2.1**, vendored under `third_party/libdsddpcm/` and statically linked. It is a clean-room, BSD-2-Clause kernel: linear-phase FIR only (constant group delay), **unity DC gain** (the +6 dB SACD makeup gain is *not* applied). crête keeps its own DSF/DFF container parsing and its one-thread-per-channel decode.

> **This replaces the former `dsd_lut.hpp`** (LGPL, transcribed from foobar's `foo_input_sacd`). DSD numbers **change** versus crête ≤ 0.8.1 — different clean-room filters and a different default output rate — so the DSD reference baselines were regenerated against MAAT DROffline (2026-07; see *DSD decimation → Validation*).

### Three axes: chain, filter, output rate

- **`--dsd-out-rate`** — `44100` (default) | `88200` | `176400` | `352800`.
- **`--dsd-mode`** (chain):
  - **`multistage`** (default) — LUT decimation to 8fs then a halving cascade; brickwall at Nyquist at 44.1 kHz output. **The measurement-grade path**, valid for DSD64/128/256/512 at every output rate.
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
| **multistage** | DSD64/128/256/512, any filter | DSD64/128/256/512, `default` filter only |
| **direct** | DSD64 only, `default` only | unsupported |

**DSD1024 and above are not supported** (the library's input enum tops out at DSD512); such files are rejected with a clear error. Unsupported chain/filter/rate combinations fail fast with an explanatory message. `crete --version` prints the resolved chain, decimation, tap count, and group delay for the current settings.

### Validation

- **Round-trip:** crête on a `.dsf` and crête on the WAV `dsd2wav` exports from it agree exactly (both go through the same libdsddpcm decode).
- libdsddpcm ships its own clean-room validation (see its repo). crête's DSD reference suites were **re-baselined against MAAT DROffline (2026-07)** on this engine: MAAT measures the 32-bit-float WAVs `dsd2wav` exports at the pinned decode config, at both 44.1 kHz and 352.8 kHz output. foobar2000 is fully retired as a reference oracle.

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

## MQA detection

MQA is a lossy codec delivered inside an ordinary lossless-looking container (44.1/48 kHz,
stereo, 16/24-bit FLAC/WAV/AIFF), with the encoded data hidden in the low bits. Because a
DR measurement on such a file describes lossy audio dressed as "hi-res," crête flags it
**unconditionally** — there is no flag to enable and no flag to suppress it.

**How it works (deterministic).** The primary MQA bitstream is the XOR of one low bit from
each left/right sample pair. crête reconstructs that bitstream across candidate carrier
positions (bit 8 for 24-bit hi-res MQA; bit 0 for 16-bit MQA-CD, which shifts the whole
encoding down a byte) and searches for the `datasync` packet — a 40-bit sync value
(`0x11319207d5`) followed by a well-formed field structure. A match is proof, not a guess:
the odds of a coincidental 40-bit magic *plus* valid fields are negligible, and in practice
only the true carrier plane ever hits. From the packet it reads the authoritative metadata:

- **authored sample rate** (`orig_rate`, e.g. 44.1 / 96 / 192 kHz),
- **authentication level** — `auth_level ≥ 9` is "Studio" / blue, below that is "green,"
- render filter / bit-depth indices and the carrier bit position.

```
==============================================================================================
  ⚠  MQA LOSSY ENCODING DETECTED — source is not lossless
----------------------------------------------------------------------------------------------
  11 track(s) · MQA Studio · authored 44.1 kHz · carrier bit 8 · auth level 9
----------------------------------------------------------------------------------------------
  DR is measured on the delivered PCM stream; content encoded above the base
  band is not reconstructed (no open MQA decoder exists).
==============================================================================================
```

The `-f json` output carries a per-track `mqa` object (`detected`, `studio`, `original_rate`,
`auth_level`, `render_filter`, `render_bitdepth`, `carrier_bit`).

**What it does not do.** crête does not *decode* (unfold/render) MQA — that DSP exists only
in the proprietary licensed decoder, so DR is always measured on the delivered base-band PCM.
Detection is also deliberately conservative: it reports MQA **only** on a verified datasync,
which catches both 24-bit hi-res MQA and 16-bit MQA-CD. A raw LSB-entropy figure is exposed
in JSON as a diagnostic, but it is never treated as a verdict — near-full low-bit entropy is
equally consistent with analog tape hiss or a prior lossy generation, so using it to claim
"MQA" would false-positive on legitimate lossless masters. Provenance: the bitstream layout is a clean-room reimplementation of the format
*description* in the BSD-2-Clause `mqa` project's spec and MQA patent WO2013186561A2 — facts
and an interface, no decoder code — so crête stays MIT.

---

## Pending work

Most of this section is a resolved record (kept for provenance). Closed since: **FIX 12 corpus re-validation** (validated — per-channel DR flags collapsed from 83/77 to 8/5), and the **DSD output-rate parity decision** (settled — 44.1 kHz is the parity-guaranteed rate, see below).

Three further items closed in 0.13.1:

- **Cross-arch identity** — measured on both CI agents, 8/8 each. The two Linux agents produce *identical* deltas against the same baseline, so the x86-64 vs aarch64 claim behind `-ffp-contract=off` now has real files behind it instead of being an untested assertion. Gated weekly. (A separate, genuinely platform-level finding fell out of it: Linux and macOS differ by one ULP on `20·log10(x)`, identically on both architectures — `libm` rounding, not FP contraction. A macOS-produced JSON cannot be equality-compared against a Linux one.)
- **The already-low per-channel DR tail** — root-caused, not merely characterised. crête's 3 s block set discarded the trailing partial block and gated blocks at `joint_rms > 1e-3`; the PMF reference implementation does neither. Both shrink the block count and so `n_top = floor(nb × 0.2)`, keeping fewer and louder top blocks — a structural *low* bias. The reference block set closes it (mean |Δ| 0.0598 → 0.0355 against foobar, 0.0628 → 0.0339 against MAAT, bias to ≈0). It was held off by default pending a full corpus run; that run happened (Weekly #68 vs #69, −33 % on 2241 DR comparisons, zero regressions, controls unmoved) and **it is the default from v0.14.0**. See *TT Dynamic Range*.
- **The East_Meets_West LRA flags** — not a crête defect. crête passes all six EBU Tech 3342 compliance vectors (10.00 / 5.00 / 20.00 / 15.00 / 4.98 / 15.01 LU against nominal 10/5/20/15/5/15, ±1 LU tolerance), and its LRA is rate-invariant to ≤0.06 LU. MAAT's own LRA moves by up to **2.77 LU** across sample rates of the same album — more than the entire crête-vs-MAAT disagreement — while the short-term loudness it is derived from moves ≤0.01 LU. crête agrees with MAAT better than MAAT agrees with itself.

v0.14.0 adds **RF64 / BW64 / Wave64**, closing the 4 GB ceiling on plain RIFF — not an exotic limit for this tool's own material, since a 43-minute 5.1 96 kHz/24 LPCM extract is 4.47 GB. All three containers share one payload decoder, and RF64/Wave64 renders of the same audio measure **identically** to plain WAV (verified on 24-bit integer and 32-bit float, 6 channels; the `ds64` 64-bit `dataSize` is proven to be honoured by a file carrying a chunk *after* `data`, which a naive "read to end of buffer" would swallow — control: doing so moves duration 20 s → 21 s and DR 12.28 → −9.21). It also fixes a reachable crash the rewrite exposed: a truncated WAV — an interrupted recording or a partial copy, declaring more data than it holds — read past the end of the buffer and **segfaulted** (0.13.1 on a half-copied file: exit 139, no output at all). All three walkers now clamp to the bytes actually present and report the audio that is there. **Not yet exercised end to end above 4 GB**: this host has 18 GB of RAM and 19 GB free, and crête loads the whole file before decoding, so a genuine >4 GB decode needs a bigger machine. The 64-bit size *path* is exercised; the >4 GB *allocation* is not.

As of 0.14.0 the one item still genuinely open is an **external oracle for the disc-audio formats**: TrueHD / DTS-HD MA / AC-3 decode is gated against crête's own recorded values and against the LPCM carrier of the same master, but against nothing outside crête. Not a release blocker.

### FIX 12 — per-channel DR and album rounding (0.11.3, awaiting re-validation)

Weekly #53 passed 141/141 with no catastrophic breach, but the DR deltas underneath exposed two real defects, both now fixed:

- **Per-channel DR shared a single joint RMS across channels**, so `DR LEFT` and `DR RIGHT` could differ only through the peak term. The error cancelled in the mean — mean-of-L/R matched MAAT to a **0.01 dB median** — but split wrongly per channel (up to **2.37 dB**, spread often opposite in sign), and collapsed to `DR LEFT == DR RIGHT` bit-identically whenever both channels shared a 2nd-highest block peak. Each channel now uses its own top-20% RMS, per the TT DR / PMF definition. Block set, gate and top-20% count are unchanged.
- **Album DR rounded twice** (`round(mean(round(track_dr)))`), which desynchronised the album score from MAAT even when every individual track matched. It now averages the raw per-track DR and rounds once.

Both are covered by `scripts/check_fix12.sh`, a zero-dependency synthetic self-check with analytically known truth (no corpus required). **The full corpus has not yet been re-measured against these fixes** — treat the per-channel DR reference rows as pending until the next weekly reports.

Note: joint DR is the mean of the per-channel values, so correcting the RMS term shifts joint DR slightly **upward** on channel-asymmetric material (zero shift when L and R carry equal energy). This is the PMF definition, but it is the opposite direction from the residual integer-DR bias, and the corpus run is what settles it.

### DSD output-rate parity — 44.1 kHz is the parity-guaranteed rate

DSD DR is output-rate dependent: at 352.8 kHz the retained ultrasonic noise-shaping inflates the 2nd-highest block peak far more than it inflates RMS, so DR reads +1…+3 higher than at 44.1 kHz. crête and MAAT agree on the direction but not the magnitude.

**The shipping default, `--dsd-out-rate 44100`, is the rate crête's parity claims apply to.** At that rate album DR is clean across all 8 DSD albums, and in the most recent weekly the continuous per-track DR comparison flags nothing on any 44.1 kHz DSD album. Every DSD album-level mismatch on record occurs only at 352.8 kHz. **Results at 88.2 / 176.4 / 352.8 kHz are offered for analysis, not parity** — treat them as a different measurement, not a more precise one.

The cause is quantified rather than assumed. Band-limiting a 352.8 kHz decode to 20 Hz–20 kHz removes roughly **78 %** of the inflation (3.05 dB of a 3.92 dB gap on one measured track) and returns the affected tracks to their 44.1 kHz integer DR — which is simply a worse-quality version of what decimating to 44.1 kHz already does, with libdsddpcm's 641-tap brickwall. Unfiltered, the ultrasonic content alone drives the meter past full scale (a sample peak of **+1.7 dBFS** on one track). There is no `--bandpass` flag for this reason: it would produce a third number with no reference implementation to validate it against.

### Reference-data hygiene — RESOLVED (2026-07-23)

The harness repo re-measured all 11 QUALIFICATION albums (Thriller × 5 rates, East_Meets_West × 6 rates) fresh against MAAT DROffline MkII 2.2.3 (2026-07-22), and crête was re-validated against the new rows the following day — 11/11 green. Both former outliers are closed:

- **`Min. PSR` flags** had three stacked causes, all resolved: a `+0.00` MAAT export sentinel in the old rows (now dropped at import), a too-coarse 1.0 s short-term sampling grid in crête (fixed in 0.11.0 — see FIX 11), and a genuine definitional difference — MAAT's Min. PSR was reverse-engineered (harness `tools/psr_discriminator/`) as a **0.5 dB/s decaying peak-hold** against 3 s short-term loudness, while crête implements the published AES eBrief 373 formula. A decode-vs-formula confirmation experiment (bit-exact FLAC→WAV through MAAT) then **closed the investigation**: the residual is a genuine formula difference, not a decode artifact, so no MAAT-compat mode is warranted. crête stays spec-true; the harness reporting tolerance reflects the documented variance.
- **`EMW_96k` track 11** was a **MAAT DROffline decode bug on a valid FLAC**: the file's STREAMINFO MD5 verifies (crête decodes it bit-perfectly, matching the 5-rate consensus), yet MAAT deterministically mismeasures RMS/LRA/DR on that encoding — and measures a re-encode of the identical audio correctly. The corpus and references were re-recorded from the new render; the original file is kept as a minimal repro.

### Test harness — overhauled 2026-07, former pending items done

- ~~`BoneyM_10k_DSD128_352` PASS with zero comparisons~~ — an album contributing **zero** metric comparisons now fails outright.
- ~~Baseline-relative regression harness~~ — implemented: a fully-green run snapshots per-album per-metric max |Δ| into `baselines/<suite>.json`; subsequent runs fail only when a metric worsens beyond baseline + margin, capped by an absolute backstop that still catches gross breakage.

Still open: a **cross-arch identity smoke check** — the same FLAC through the x86-64 and aarch64 binaries must be bit-identical (the regression test for the `-ffp-contract=off` guarantee).

### MAAT as the DSD metric oracle — done (2026-07)

foobar2000 is retired as an oracle entirely (its `ext` export carried only DR, RMS, and Peak, leaving most metrics untested on DSD); **MAAT DROffline is the sole external reference**. The planned `dsd2wav` route is implemented and recorded: crête exports the decimated PCM to 32-bit float WAV, MAAT measures those WAVs, and the results are imported as DSD references — every DSD album in the corpus, at both 44.1 kHz and 352.8 kHz output (chain=multistage, filter=default pinned).

Answers to the old open questions: MAAT accepts 32-bit float WAV at 352.8 kHz without resampling, but reports sentinel values for the BS.1770 loudness family at that rate — those rows are dropped at import, so the 352.8 kHz suites gate on SPPM / TPL / RMS / DR only.

Standing caveats:

- The exported WAV comes out of crête's own decimation, so this validates **metrics, not decimation**, end-to-end. The old bit-exact `dsd_xcheck` gate against the LGPL `foo_input_sacd` transcription is obsolete (libdsddpcm is clean-room and intentionally differs); decimation correctness rests on libdsddpcm's own golden vectors plus crête's internal parity suites.
- MAAT references recorded this way are keyed to a specific `crete_version` + decode config, stored alongside the reference rows. The exported WAVs are **never committed as corpus** — they are regenerated, so no one decimation gets baked into the reference data permanently.

### Code

- ~~Round-trip guard: crête on a `.dsf` and crête on the WAV exported from it must agree. Not yet a pytest case.~~ — **done**, `test_dsd_roundtrip.py` on Thriller_DSD64. Order statistics agree to 4.5e-07 dB (under the 5.18e-07 float32 mantissa bound), energy averages to 2.9e-10, decoded sample counts exactly.

### Not yet exercised

- ~~**DFF container.**~~ **Covered.** `test_dsd_parity.py` gates DSF against DFF on Café Blue, with a two-tier threshold model (sample-invariant metrics at 0.01 dB as the decoder gate; window-phase metrics relaxed, since DSF and DFF rips can split a track boundary by a CD frame). Deliberately one album: the suite proves a property of the *code* — that the DSF reader and the DFF deinterleaver emit the same samples — not of any album, so a second capture would re-prove the same invariant for another multi-GB corpus.
- ~~**AIFF.**~~ **Covered** by the codec-agnostic FORMAT suite, which transcodes every master to AIFF big-endian and AIFF-C `sowt` alongside the WAV and FLAC variants and requires bit-identical metrics. There are no float AIFF variants because there can't be: AIFF proper is integer PCM only, float is an AIFF-C extension (`fl32`/`fl64`), and crête's decoder accepts only uncompressed `NONE` and `sowt`.

DSD512 graduated 2026-07: it now has a dedicated suite and MAAT reference data (`SoundLiaison_JMS_DSD512`, `fir1_64m` multistage, both 44.1 and 352.8 kHz out), joining DSD256 (`fir1_32`). Direct is DSD64 → 44.1 kHz only, so it does not apply to these rates.

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
gui_main.cpp              GUI entry point (Dear ImGui + SDL2 + OpenGL); parallel decode
crete_theme.hpp           GUI Modernist theme: palette (light/dark), fonts, widgets
analysis.hpp              TT DR, EBU R128, true peak, PLR/PSR/LRA, crest factor
audio.hpp                 WAV/RF64/BW64/Wave64, AIFF/AIFF-C, FLAC, DSF, DFF decoders + FFmpeg routing
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

**DSD decode** is provided by the vendored **libdsddpcm** (`third_party/libdsddpcm/`, release v0.2.1), which is **BSD-2-Clause** — a clean-room implementation that shares no code with the LGPL `foo_input_sacd` transcription it replaced. Its license text ships at `third_party/libdsddpcm/LICENSE`. This replaces the former `dsd_lut.hpp` (LGPL-2.1, transcribed from foobar's `libdsdpcm`), which is retired to `dsd_lut.hpp.bak` and is no longer built. crête's licensing is therefore now cleanly **MIT + BSD-2-Clause**.

`crete-ffmpeg` builds statically link a compact FFmpeg configured LGPL-only (decode-only, no GPL or nonfree components); FFmpeg is used under LGPL 2.1+.
