# crête

Zero-dependency C++17 dynamic range meter. The name is French for *peak* / *crest* — the origin of the audio term *crest factor*.

Measures **TT Dynamic Range** (Pleasurize Music Foundation algorithm), **EBU R128 integrated loudness**, **true peak** (4× polyphase FIR), **PLR**, **PSR**, **LRA**, **crest factor**, and per-channel variants for lossless audio files. PCM output is compatible with [dr.loudness-war.info](http://dr.loudness-war.info/) submission format; DSD is decimated by the vendored, BSD-2-Clause **[libdsddpcm](https://codeberg.org/abksh/libdsddpcm)** kernel — a clean-room linear-phase, unity-DC decoder — with selectable **chain** (Direct / Multistage), **filter**, and **output rate**.

crête also **detects MQA-encoded files** and reports them unconditionally: if you are measuring dynamic range, you are entitled to know the source is lossy MQA rather than lossless. Detection is deterministic — it reconstructs the buried MQA bitstream (XOR of the stereo low bits) and verifies the `datasync` packet, then reports the authored sample rate and the green/Studio authentication level. It never asserts MQA from statistics alone, so it does not false-positive on noisy-but-lossless masters (e.g. analog tape hiss). No open MQA decoder exists, so DR is measured on the delivered PCM stream; the warning makes clear the encoded content above the base band is not reconstructed. See *MQA detection* below.

The default build has **zero dependencies**. An optional third build tier statically links a compact, LGPL-only FFmpeg to add lossy/ALAC decode support — see below.

**Current version: 0.19.4**

---

## Install (Linux packages)

Prebuilt packages are published from the openSUSE Build Service project
[`home:abksh:crete`](https://build.opensuse.org/package/show/home:abksh:crete/crete).
The one-click page generates the right commands for your distribution:
**<https://software.opensuse.org/download.html?project=home:abksh:crete&package=crete>**

| Package | Installs | Built from |
|---------|----------|------------|
| `crete` | `/usr/bin/crete` | `make cli-ffmpeg` — native decoders + the compact FFmpeg set (no `-f json`) |
| `crete-gui` | `/usr/bin/crete-gui`, desktop entry, fonts in `/usr/share/crete` | `make gui-ffmpeg` |

| Distribution | Repository | Architectures |
|--------------|------------|---------------|
| openSUSE Tumbleweed | `openSUSE_Tumbleweed` (x86_64), `openSUSE_Factory_ARM` (aarch64) | x86_64, aarch64 |
| openSUSE Leap 16.0 | `16.0` | x86_64, aarch64 |
| Fedora 43, 44, Rawhide | `Fedora_43`, `Fedora_44`, `Fedora_Rawhide` | x86_64, aarch64 (Rawhide x86_64 only) |
| Debian 12, 13 | `Debian_12`, `Debian_13` | x86_64 |
| Ubuntu 22.04, 24.04, 26.04 | `xUbuntu_22.04`, `xUbuntu_24.04`, `xUbuntu_26.04` | x86_64 |
| Arch Linux | `Arch` | x86_64 |

For example (substitute the repository name from the table):

```bash
# openSUSE
sudo zypper addrepo https://download.opensuse.org/repositories/home:abksh:crete/openSUSE_Tumbleweed/home:abksh:crete.repo
sudo zypper install crete crete-gui

# Fedora
sudo dnf config-manager addrepo --from-repofile=https://download.opensuse.org/repositories/home:abksh:crete/Fedora_44/home:abksh:crete.repo
sudo dnf install crete crete-gui

# Debian / Ubuntu
echo 'deb https://download.opensuse.org/repositories/home:/abksh:/crete/Debian_13/ /' | sudo tee /etc/apt/sources.list.d/crete.list
curl -fsSL https://download.opensuse.org/repositories/home:abksh:crete/Debian_13/Release.key | gpg --dearmor | sudo tee /etc/apt/trusted.gpg.d/crete.gpg > /dev/null
sudo apt update && sudo apt install crete crete-gui
```

The packages are built with crête's own compiler flags, not the distribution's,
so the x86_64 and aarch64 binaries keep the bit-identical measurements that
`-ffp-contract=off` exists for. The compact FFmpeg (LGPL-2.1, decode-only) is
built from source inside the package build and linked statically; nothing is
downloaded at build time. To produce the upload set yourself:
`scripts/make_obs_source.sh OUTDIR [REF]`.

## Install (macOS, Homebrew)

```bash
brew tap abksh/crete https://codeberg.org/abksh/homebrew-crete
brew install abksh/crete/crete        # /opt/homebrew/bin/crete
brew install abksh/crete/crete-gui    # /opt/homebrew/bin/crete-gui
```

Same two builds as the Linux packages (`make cli-ffmpeg`, `make gui-ffmpeg`),
compiled from the release tag with crête's own compact LGPL FFmpeg linked
statically -- never Homebrew's `ffmpeg`, which is `--enable-gpl`. Formulae,
not casks, so nothing is quarantined by Gatekeeper. Tap source:
<https://codeberg.org/abksh/homebrew-crete>.

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

### First time on a new machine

```bash
make setup-deps-check                 # report what's missing, change nothing
make setup-deps                       # install it
```

`scripts/setup-deps.sh` (macOS and Linux — bash 3.2 compatible, so it also runs
under zsh) and `scripts/setup-deps.ps1` (Windows, via MSYS2) install the
toolchain for whichever tiers you ask for: `--tier cli|json|ffmpeg|gui|cross`,
default all but `cross`. They check by **capability**, not by package name, so a
compiler you built yourself or an SDL2 from another package still counts; they
print every command before running it, support `--check` and `--dry-run`, and
never install something already present. Package managers covered: Homebrew,
dnf, apt, pacman, zypper, and MSYS2's pacman.

Two things they check that are easy to get wrong:

- **nasm on x86.** `build_ffmpeg_compact.sh` does not pass `--disable-x86asm`,
  so FFmpeg's configure *errors out* on x86_64 without an assembler — as a deep,
  confusing build failure rather than a clear missing-dependency message.
  Irrelevant on aarch64.
- **The MinGW threading model on Windows.** The `win32` model ships a libstdc++
  with no `std::thread` at all, and crête's parallel per-file decode is its
  default path, so such a build links cleanly and then dies on the first album.
  The `posix` model works with zero source changes.

The setup scripts install the *toolchain*; `make setup-imgui`, `make
setup-nlohmann` and `make setup-ffmpeg` fetch the *sources* crête vendors
(`setup-deps.sh --fetch` will run those too).

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
make cli-ffmpeg              # FFmpeg decode, no JSON
make cli-json-ffmpeg         # FFmpeg decode + JSON (binary: crete-ffmpeg)
```

**The compact FFmpeg builds itself.** Since 0.16.1 the FFmpeg tiers carry it as an order-only prerequisite, so there is no separate script to remember — an FFmpeg tier is one command like every other tier. It is also **staleness-aware**: `scripts/build_ffmpeg_compact.sh` stamps the prefix with an md5 of itself, and the Makefile rebuilds when that stamp is missing or no longer matches this checkout. That matters because a stale cache does not fail at link time — it fails at *decode* time, which is far harder to read. (When the disc-audio decoders were added, every warm cache silently kept a build that could not decode them, and the DISCAUDIO suite failed on four of six carriers with no hint why. Both Jenkinsfiles learned to compare the stamp; a local tree never did. Now both do.)

- `make setup-ffmpeg` — build it on its own, no-op if already fresh.
- `make ffmpeg-rebuild` — force a rebuild regardless of the stamp.
- `make ... FFMPEG_AUTOBUILD=0` — never build it automatically.

An externally-provided FFmpeg (`FFMPEG_PREFIX=/path`) is detected and never clobbered; crête says so and warns that it cannot verify the decoder set. The script stays the single source of build truth rather than being inlined into the Makefile, because the stamp hashes the *script*: hashing the Makefile instead would make every unrelated target or `CXXFLAGS` edit invalidate every agent's FFmpeg cache.

The compact FFmpeg build is **LGPL-only** (no GPL, no nonfree components), **decode-only**, and statically linked — `crete-ffmpeg` is still a single self-contained binary. Point the build at a non-default FFmpeg install with `make cli-json-ffmpeg FFMPEG_PREFIX=/path/to/ffmpeg`. Requires `pkg-config`.

Decoder routing is strict:

- **Native formats (WAV/RF64/Wave64, AIFF, FLAC, DSF, DFF) always use crête's own decoders**, even in the FFmpeg build. This preserves parity against the MAAT reference data.
- FFmpeg handles only non-native formats: **ALAC (`.m4a`), MP3, AAC, Opus, Vorbis, WMA, WavPack, APE, Musepack, TAK**, plus the disc-audio set — **TrueHD/MLP, AC-3, E-AC-3, DTS/DTS-HD MA**.
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
crete -j 2 /path/to/album/             # pin parallel file workers (default: automatic)
crete --memory-limit 4G /path/to/album/   # plan for at most 4 GB
crete --memory-limit 40% /path/to/album/  # ...or a share of this machine

crete --dsd-mode multistage /path/to/dsd/           # default chain (measurement-grade)
crete --dsd-mode direct     /path/to/dsd/           # DSD64 -> 44.1 kHz only
crete --dsd-filter kaiser   /path/to/dsd/           # final-stage filter (44.1 kHz out only)
crete --dsd-out-rate 352800 /path/to/dsd/           # 44100 (default) |88200|176400|352800

crete --dr-mean arithmetic  /path/to/album/         # legacy <=0.12.1 DR averaging (A/B only)
crete --dr-blocks legacy    /path/to/album/         # pre-0.14.0 3 s block set (A/B only)

crete-ffmpeg /path/to/album/           # cli-json-ffmpeg build: adds ALAC/MP3/AAC/Opus
```

### Memory and parallelism

crête decodes whole tracks to 64-bit float, so one worker can hold several GB:
a 4:18 track at 192 kHz / 4 ch peaks near 4 GB. Running one worker per core on
a machine that cannot feed them is how you get killed by the OS — and an
OOM-killed process exits with **empty stderr**, which reads as a crash rather
than as "you asked for too much".

So the default worker count is fitted to **free memory as well as core count**.
crête reads the host's RAM (honouring a **container/cgroup limit** where one is
in force, rather than the physical machine underneath), estimates what each
item will cost from its header, and sizes the pool to fit. When memory rather
than cores decides, it says so:

```
Note: using 3 of 12 cores -- each worker needs about 3.7 GB and 12 GB is
available. Override with --jobs or --memory-limit.
```

The estimate is a measured model, not a rule of thumb — peak resident memory
fits `file_bytes + K × (channels × rate × seconds × 8)`, with K measured at
2.0–2.5 across WAV, FLAC and the FFmpeg decoders. File size alone is not usable:
measured file→peak ratios span 6.9×–13.4× on lossless and are off by more than
ten times on lossy, where a 7 MB AC-3 clip decodes to ~600 MB.

- **`--jobs N` still wins.** An explicit `-j` is obeyed exactly; crête warns if
  it does not expect it to fit, then proceeds. Automatic sizing is what changes,
  not the flag.
- **`--memory-limit`** overrides what crête plans for: `8G`, `512M`, or `40%`.
  Useful on a shared machine, or where the detected figure is wrong.
- If a **single item** exceeds the budget, no worker count can help, so crête
  warns before starting rather than letting the OS end the run.

**None of this changes a measurement.** Worker count and memory limits affect
only scheduling; the JSON `tracks` array is byte-identical across job counts,
and results stay in album order regardless.

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

**The GUI reads everything the CLI reads.** Since 0.16.1 both front-ends expand an input path through the same collector (`groups.hpp`), so a folder cannot mean different things depending on which one opened it. That means the GUI now handles all three input shapes, not just "one file, one track":

| shape | what appears |
|---|---|
| a standalone file | one track, as before |
| a **cue sheet** + its monolithic audio | each `TRACK` analysed separately, sliced from one decode |
| an **SACD `.iso`** | every track from the disc TOC; an **SACD IMAGE** strip appears with an `Auto / Stereo / Multichannel` area selector |

Tracks list in **album order**: a folder is sorted numerically rather than lexicographically, so `2 - Track` precedes `10 - Track` (plain string ordering put track 10 third, which made albums with unpadded numbering look randomly ordered). Zero-padded numbering is unaffected. In the GUI the TRACK column is the default sort and ascending restores exactly the order the analysis produced — the TOC order for a cue sheet or an SACD image. Sorting by any other column changes only the view, never the underlying order or the selected track.

A **multi-stream container** (a Blu-ray `.mkv` carrying several mixes of one album) raises an **AUDIO STREAM** picker listing every stream with its codec, profile, channel count and sample rate, marking the one FFmpeg would choose unaided and flagging any Atmos/DTS:X stream as *bed only*. This is the GUI's equivalent of `--stream N`, shown *before* the run rather than reported after it. It appears for a single selected file; when a whole folder is analysed, crête's "measured stream N of M" warning still lands in the error log.

Two measurement axes are deliberately **not** exposed: `--dr-mean` and `--dr-blocks` are documented A/B-comparison flags, so the GUI pins crête's defaults (`quadratic` / `reference`) and states them in About under NATIVE DECODERS, next to the live LFE setting. Anything needing a non-default axis is a CLI job.

`make gui-ffmpeg` builds `crete-gui-ffmpeg`, the GUI with the same compact LGPL-only FFmpeg tier as `crete-ffmpeg`: folder scans, drag-and-drop, and the file dialogs additionally accept `.m4a .mp4 .m4b .aac .mp3 .opus .ogg .oga .mka .webm .wma .wv .ape .mpc .tak .mkv .thd .dts .ac3 .eac3 .ec3`, with the same strict routing (native formats always use crête's own decoders; DSD is never routed through FFmpeg). The header shows `+ffmpeg`, and the About panel lists the linked FFmpeg version and every extra codec — ALAC, AAC, MP3, Opus, Vorbis, WMA v1/v2/Pro/Lossless, WavPack, APE, Musepack, TAK, plus the disc-audio set (Dolby TrueHD/MLP, AC-3, E-AC-3, DTS / DTS-ES / 96-24 / HD HRA / HD MA / Express).

---

## Supported Formats

### Native decoders (all builds)

| Format        | Extension          | Notes                                                                  |
|---------------|--------------------|------------------------------------------------------------------------|
| WAV           | `.wav`             | PCM 8/16/24/32-bit, IEEE float 32/64-bit, `WAVE_FORMAT_EXTENSIBLE`     |
| RF64 / BW64   | `.wav`, `.rf64`, `.bw64` | 64-bit sizes via `ds64` — the >4 GB escape hatch (EBU Tech 3306 / ITU-R BS.2088). Same payload as WAV |
| Wave64        | `.w64`             | Sony Wave64: GUID chunk ids and 64-bit sizes. Same payload as WAV       |
| AIFF / AIFF-C | `.aif`, `.aiff`    | Standard big-endian; AIFF-C uncompressed (`NONE`) and `sowt` little-endian |
| SACD disc image | `.iso`           | Scarlet Book. Both areas read from the disc TOC, DSD and DST — see below |
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
| DVD LPCM | `.vob`, `.aob` | Lossless — DVD's own LPCM framing, not plain PCM |
| DVD-Video | `.vob` | MPEG program stream: AC-3 / DTS / LPCM beside MPEG-2 video |
| DVD-Audio | `.aob` | MPEG program stream: MLP or LPCM, no video |

`.mkv` and `.mp4` are video containers; crête ignores video and subtitles, so music Blu-ray rips work directly.

#### DVD title sets

A DVD does not store a title as one file. The spec caps a file at 1 GB, so the
program stream is split at a pack boundary and numbered:

```
VIDEO_TS/VTS_02_1.VOB  VTS_02_2.VOB  VTS_02_3.VOB    one 43-minute title
AUDIO_TS/ATS_01_1.AOB  ...           ATS_01_5.AOB    one DVD-Audio title set
```

**Point crête at any one fragment and it measures the whole title set.** The
pieces are one stream and metering them separately would be wrong twice over:
each is metered alone, and the split lands wherever 1 GB fell, so the 3 s DR
block straddling it belongs to neither piece. crête reads them through a single
demuxer, so the boundary costs nothing — measured on the test corpus, the
joined MLP duration is 19 ms *longer* than the two pieces measured separately,
that being the frame the split cut in half.

It also removes a trap. The FFmpeg stream **index is not stable across
fragments of one title** — on the DSOTM 50th DVD the same three substreams come
back as `2=ac3 3=ac3 4=lpcm` in the first fragment and `2=lpcm 3=ac3 4=ac3` in
the second, because the index follows order of first appearance in the program
stream. Reading the set as one stream means one probe and one answer, which is
what makes `--stream` meaningful here at all.

One caveat on `--stream` for `.vob`: the compact FFmpeg reports **one more
audio stream than the disc carries**. It cannot identify the MPEG-2 video
elementary stream, content-probes it, and matches it as MP3 — so a disc with
three audio streams lists four, the extra decoding to a fraction of a second of
noise. A full FFmpeg types it correctly. Automatic selection is unaffected (it
ranks by bitrate and the phantom has none) and every genuine stream is
reachable by index; only the listing is wrong.

#### DVD-Audio title sets hold more than one mix

A DVD-Audio title set can carry several audio **groups**. Fleetwood Mac's
*Rumours* (2001, Warner) holds a 96 kHz 5.1 mix, a 96 kHz stereo mix and a
48 kHz version in one `ATS_01`, and **the group boundary does not align with
the file fragments** — on that disc the 6-channel to 2-channel change falls
partway through `ATS_01_3.AOB`, so even a single `.aob` can cross one.

crête measures the first group, says so, and exits 2:

```
Warning: audio format changes partway through this stream (a DVD-Audio title
set holds several audio groups); measured the first group only, 6 ch / 96000 Hz
```

Left to itself FFmpeg decodes straight through the change into a resampler
configured for the previous format, reporting nothing but a `Parity check
failed` and a `DTS discontinuity` on stderr — the numbers stay in range and
nothing says they are meaningless. Choosing *which* group to measure would need
the `ATS_01_0.IFO` group table parsed; the first is the one the disc presents
first, and it is the 5.1 mix on every DVD-Audio seen here.

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

### SACD disc images

SACD rips circulate overwhelmingly as `.iso`, so crête reads them directly
rather than requiring an external extraction to `.dsf`/`.dff` first. The disc's
own TOC supplies the track list, and each track is decoded from its own sector
range through the **same libdsddpcm chain** every `.dsf` and `.dff` goes
through — so `--dsd-mode`, `--dsd-filter` and `--dsd-out-rate` all apply, and
crête on an ISO must agree with crête on an extracted `.dsf` of the same disc.
That is a crête-vs-crête gate needing no external oracle, and it is how this
reader was validated: on the 2018 DSOTM SACD every one of the 10 track
durations matches the published running order, and *The Great Gig in the Sky*
returns peak **−5.489** / RMS **−19.753** against the **−5.49 / −19.75**
recorded earlier from a `.dsf` rip of the same album.

A disc may carry a 2-channel area, a multichannel area, or both:

```bash
crete album.iso                                 # auto: prefer the 2-channel area
crete --sacd-area multichannel album.iso        # the 5.1 area
crete --sacd-area stereo       album.iso
```

`auto` prefers stereo deliberately — it is the layer crête's DR references are
built on, the layer comparable with every other release of an album, and
usually the uncompressed one.

**DST-compressed areas decode too.** DST is SACD's lossless compression, and
multichannel areas commonly use it — on the DSOTM disc the stereo area is plain
DSD at 1.72 GiB while the 5.1 area is DST at 2.16 GiB against the 5.16 GiB it
would need uncompressed. crête carries its own DST decoder (`dst.hpp`, written
from the ISO/IEC 14496-3 Subpart 10 syntax, zero-dependency like everything
else), so both areas of a hybrid disc measure through the same libdsddpcm chain
and nothing downstream can tell which the disc used. DST is lossless, so this is
not an approximation.

It is not fast: roughly 2× realtime for six channels, because DST interleaves
all channels through one arithmetic decoder and so cannot be parallelised
within a frame. A 43-minute 5.1 disc takes about 20 minutes.

**The decoder self-checks, and this matters more than it sounds.** Arithmetic
coding consumes exactly the bits the encoder produced if and only if the decoder
applies the same probability sequence, so crête asserts that every frame
consumes its own coded length. That check exists because a *wrong* DST decoder
does not announce itself: an early version of this one parsed every frame
header correctly, reported a plausible 0.75 % misprediction rate, and emitted
1-bit data at the right density that decimated to something audible — while
actually producing the prediction filter free-running on its own output. It
consumed 15 % of each frame. The corrected decoder consumes 100.0 %.

Dispatch is by **content, not extension**: `.iso` names every disc image ever
made, so crête checks for `SACDMTOC` at LSN 510 and quietly ignores anything
else rather than trying to meter a data DVD.

**This path is now gated.** It had no automated test at all until 0.17.0 — the
reader and the DST decoder were validated by hand once and then left, which is
uncomfortable for code whose failure mode is silence: each of the three format
traps found while writing it produced audible output and a plausible DR. The
`sacd` suite measures a **reduced** image — the first tracks of each area, the
multichannel one relocated so the file need not span the 1.85 GB gap before it,
229 MB instead of 4 GB. Every audio sector in it is the disc's own bytes; only
the TOC fields describing extent are rewritten, and the builder
(`scripts/make_sacd_test_image.py --verify`) measures every kept track in both
the reduced and the full image and requires identical results, so its own
address arithmetic cannot become a test that certifies its own mistake. 6
passed / 2 skipped on its first weekly (#77); the 2 skips are the `.dsf` parity
tier below.

### Cue sheet support

When a folder contains a `.cue` file, crête:

1. Parses the cue (`FILE`, `TRACK`, `TITLE`, `PERFORMER`, `INDEX 01`).
2. Locates each referenced audio file (case-insensitive fallback). A cue may name several `FILE`s — one per vinyl side or per disc — and `INDEX` times restart in each, so every track is sliced from the file its `INDEX 01` sits in.
3. Decodes each file once, then slices its PCM by track boundaries and runs the full analysis pipeline on each slice.

A track whose range is empty (its start is not before the next track's, or lies past the end of the file) is dropped with a warning, and the run exits **2**: an album missing a track is not a complete measurement.

Each track is reported as `NN - Title.ext` (e.g. `01 - What A Shame.dff`), matching the per-track naming convention produced by `sacd_extract`, `flac --split`, and most other splitters — so a cue-sliced monolithic album pairs cleanly against the equivalent per-track album. Audio files in the same folder *not* referenced by any cue are still analysed individually.

Track times are stored internally as CDDA frames (1/75 sec) and converted to PCM samples at the actual decoded rate, so the same cue works for 44.1 kHz WAV and 352.8 kHz DSD-decoded PCM without rounding loss (every standard rate is divisible by 75).

> **Memory note.** At the default 44.1 kHz DSD output a monolithic 60-minute DSD64 album decodes to ~2.6 GB of float64 PCM before slicing; at `--dsd-out-rate 352800` that rises 8× to ~21 GB. Cue-sliced analysis adds one slice plus the analyser's working set (~2× the slice) on top. Plan accordingly on memory-constrained hosts.

---

## Damaged files

A meter's worst failure is not refusing a file — it is measuring a broken one and reporting a number as if nothing happened. Before 0.17.0 crête did exactly that: a FLAC with one bad sector in it was scored, averaged into the album DR, and returned exit 0 with no error field anywhere in the JSON, while the reference `flac` decoder refused the same file outright. A single flipped byte was worse, producing numbers *identical* to the clean file.

crête now verifies what each format lets it verify, and says so.

| check | formats | what it catches |
|---|---|---|
| **frame CRC-8 + CRC-16** | FLAC | altered audio data — the bit-flip nothing else can see |
| **declared vs decoded length** | FLAC (STREAMINFO), WAV/RF64/Wave64 (`data`), AIFF (COMM/SSND) | truncation, interrupted copies, frames lost to a resync |
| **sample peak ≤ 0 dBFS** | any integer PCM | a decode that produced values the format cannot represent |

**A damaged file is still measured.** crête is a meter, not a repair tool: it reports what the bytes decode to and marks every affected number as untrustworthy. That separation is deliberate — it is what keeps the measurement path on an intact file bit-identical to the one that existed before these checks, verified across 62 files spanning FLAC, WAV (integer and float), AIFF, RF64, Wave64, ALAC, TrueHD, AC-3 and DTS-HD MA in mono, stereo, quad, 5.1 and 7.1.

```
Warning: 04 - damaged.flac: FLAC: CORRUPT -- 1 of 215 frames failed the CRC-16 check (0.5%).
Warning: 04 - damaged.flac: FLAC: TRUNCATED -- STREAMINFO declares 882000 samples per channel but 877904 decoded (4096 missing, 0.5%).
Warning: 04 - damaged.flac: CORRUPT -- sample peak 45.32 dBFS on 16-bit integer PCM, which cannot exceed 0 dBFS.
Error:   05 - unreadable.flac: FLAC: invalid magic
```

Every one of those lines also appears in `-f json` under **`warnings`**, which is **always present** — an empty array is the positive statement "nothing was wrong", rather than the absence of evidence a missing key would be. `num_tracks` counts what was *measured*, so a file that failed to decode contributes a warning and no track; the two together are what tell a consumer something is missing.

**Exit status:**

| code | meaning |
|---|---|
| `0` | every input decoded cleanly |
| `1` | nothing could be analysed |
| `2` | completed, but some input failed to decode or was damaged — the numbers are unreliable, including any album value computed from them |

### The limit, stated plainly

**WAV and AIFF carry no checksum.** Altered *sample values* in them are undetectable — by crête or by any other meter — because a run of zeros inside a `data` chunk is indistinguishable from a passage of digital silence that the artist put there. Only length and range can be checked in those formats. FLAC is the one that can prove itself.

This is pinned rather than glossed: the INTEGRITY suite asserts that a WAV with 4 KB of zeros written into its audio comes back **clean**, so the limitation stays visible in a green run instead of being quietly assumed away.

### Testing

`scripts/make_integrity_corpus.sh` builds the corpus from a fixed seed, and the zip is deterministic (fixed mtimes, sorted entry list) so two runs on two machines produce the same sha256 — a corpus CI pins by checksum has to be reproducible in the artifact that is actually compared. It is the only crête corpus that is **not** commercial music, so unlike every reference suite it can be redistributed, regenerated anywhere, and needs no oracle — it gates crête's reporting contract, not a DR value. The harness suite is `tests/test_integrity.py` (marker `integrity`): 19 tests, ~5 s, zero-dep binary. It passes on 0.17.0 and fails 13 tests on 0.16.1 — a regression suite that cannot fail against the code it was written for is decoration. **19/19 in CI on its first weekly** (#77).

Two things that run were worth knowing. **No reference file trips any of the new checks**: crête's exit 2 appears nowhere in a run spanning 51 albums and 11,931 comparisons, which is the corpus-wide version of a 255-file local sweep that also found none. And **the checks moved no measurement** — see *Pending work*.

Generating that corpus immediately found a **reachable segfault**: a truncated AIFF walked off the end of the buffer, because the AIFF chunk walker used file-supplied lengths and an SSND offset without bounds-checking either — the same defect the RIFF walker had fixed in 0.14.0, in the decoder that never got the same treatment. Fixed, and the suite now sweeps 21 truncation offsets per format to keep it fixed.

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

4× polyphase FIR (12 taps per phase) per channel; the joint result is the max across the **scored** channels — so on a multichannel layout the LFE is excluded by default, exactly as it is from the joint sample peak, and `--dr-lfe include` moves both together. Up to v0.14.0 the joint true peak alone had no such guard, which on a file whose LFE was its loudest channel produced a true peak far above the sample peak for reasons that had nothing to do with inter-sample overshoot (see *Pending work*).

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

### Immersive carriers — Atmos, DTS:X, Auro-3D

**crête measures the channel bed, and since v0.15.0 it says so.** FFmpeg has no Atmos or
DTS:X renderer: given a Dolby Atmos stream it decodes the underlying TrueHD 7.1 bed (or the
E-AC-3 5.1 core, for JOC) and discards the object metadata; DTS:X has the same shape over a
DTS-HD MA core. That is a real and complete measurement of what a non-immersive playback
chain actually delivers — it is what your receiver plays if it is not an Atmos receiver —
but an unqualified "crête measures Atmos" would be false, so the profile is reported
separately from the layout:

```
Format:              TRUEHD 24-bit / 48000 Hz
Profile:             Dolby TrueHD + Dolby Atmos
Layout:              7.1 · L R C LFE Lss Rss Lrs Rrs  [bed -- Atmos objects not rendered]
```

JSON carries `codec_profile` and a boolean `immersive_bed`. The profile comes from the
bitstream itself (`AVCodecContext::profile`), so this costs no parsing of MLP substreams or
`dec3` boxes — but it does need the raised probe crête already applies: at FFmpeg's default
5 MB probe a TrueHD stream interleaved among five others reports `profile=unknown`.

This is **identification, not decode**, and it follows the precedent `mqa_detect.hpp`
already set for MQA: measure the delivered stream deterministically, name the premium layer
separately from authoritative bitstream fields, and never imply the premium layer was
rendered. The field is descriptive — **no measured value depends on it**. It appears only in
`crete-ffmpeg` builds, since the zero-dependency binary cannot decode these codecs at all.

A non-immersive profile such as `DTS-HD MA` is reported too, without the bed qualifier:
there is nothing unrendered about it.

**Auro-3D is not detectable this way**, and is not attempted. FFmpeg reports an Auro carrier
as plain DTS-HD MA; on the one disc measured here "Auro-3D 9.1" was only a container title
tag, with the height channels buried in the carrier's low bits. That signature is real — on
a solo-piano recording whose LFE is otherwise digitally silent, the Auro carrier's LFE held
803 853 non-zero samples in ±24 (5 bits of payload) against the DTS:X carrier's exact zero —
but reading it is inference, so if it is ever built it belongs in the confidence-scored
`forensics` namespace, gated off, never in `metrics`. Worth knowing meanwhile: such a carrier
reads as ~19 effective bits and is useless for noise-floor or bit-depth forensics, while DR
and loudness are untouched (−116 dB is nowhere near a 3 s block).

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

**Weekly #77 (0.17.0): 200 tests, 184 passed, 16 skipped, 0 failed, 0 broken. 51 albums, all PASS. 11,931 comparisons, 44 flagged (0.37 %), zero catastrophic.** Two album-DR mismatches, both long documented: `BoneyM_10k_DSD128@44100` (a 0.020 dB miss of a `.5` boundary, not a rounding-rule problem) and `Thriller_DSD64@352800` (the 352.8 kHz rate dependence). `Min. PSR` remains the worst metric by average delta (0.598 dB, max 2.89) and remains deliberately out of scope.

**The integrity work of 0.17.0 moved nothing, and that is measured rather than argued.** Every one of the 15 per-suite detailed reports in #77 is **identical** to the corresponding report from the last archived weekly, #73 — the same 11,931 comparisons, the same 44 flags, the same per-metric maxima and averages, the same two album mismatches — with the sole textual difference being one added header line. #73 recorded **0.13.1** and #77 records **0.17.0**, so that identity spans five releases (0.14.0, 0.15.0, 0.16.0, 0.16.1, 0.17.0): the RF64/Wave64 containers, the joint true-peak LFE guard, the DST decoder, the shared input collector with its natural-order fix, and the integrity checks. Each of those was individually argued to be a no-op on this corpus; this is the corpus agreeing, on every number it holds.

Three further items closed in 0.13.1:

- **Cross-arch identity** — measured on both CI agents, 8/8 each. The two Linux agents produce *identical* deltas against the same baseline, so the x86-64 vs aarch64 claim behind `-ffp-contract=off` now has real files behind it instead of being an untested assertion. Gated weekly. (A separate, genuinely platform-level finding fell out of it: Linux and macOS differ by one ULP on `20·log10(x)`, identically on both architectures — `libm` rounding, not FP contraction. A macOS-produced JSON cannot be equality-compared against a Linux one.)
- **The already-low per-channel DR tail** — root-caused, not merely characterised. crête's 3 s block set discarded the trailing partial block and gated blocks at `joint_rms > 1e-3`; the PMF reference implementation does neither. Both shrink the block count and so `n_top = floor(nb × 0.2)`, keeping fewer and louder top blocks — a structural *low* bias. The reference block set closes it (mean |Δ| 0.0598 → 0.0355 against foobar, 0.0628 → 0.0339 against MAAT, bias to ≈0). It was held off by default pending a full corpus run; that run happened (Weekly #68 vs #69, −33 % on 2241 DR comparisons, zero regressions, controls unmoved) and **it is the default from v0.14.0**. See *TT Dynamic Range*.
- **The East_Meets_West LRA flags** — not a crête defect. crête passes all six EBU Tech 3342 compliance vectors (10.00 / 5.00 / 20.00 / 15.00 / 4.98 / 15.01 LU against nominal 10/5/20/15/5/15, ±1 LU tolerance), and its LRA is rate-invariant to ≤0.06 LU. MAAT's own LRA moves by up to **2.77 LU** across sample rates of the same album — more than the entire crête-vs-MAAT disagreement — while the short-term loudness it is derived from moves ≤0.01 LU. crête agrees with MAAT better than MAAT agrees with itself.

v0.14.0 adds **RF64 / BW64 / Wave64**, closing the 4 GB ceiling on plain RIFF — not an exotic limit for this tool's own material, since a 43-minute 5.1 96 kHz/24 LPCM extract is 4.47 GB. All three containers share one payload decoder, and RF64/Wave64 renders of the same audio measure **identically** to plain WAV (verified on 24-bit integer and 32-bit float, 6 channels; the `ds64` 64-bit `dataSize` is proven to be honoured by a file carrying a chunk *after* `data`, which a naive "read to end of buffer" would swallow — control: doing so moves duration 20 s → 21 s and DR 12.28 → −9.21). It also fixes a reachable crash the rewrite exposed: a truncated WAV — an interrupted recording or a partial copy, declaring more data than it holds — read past the end of the buffer and **segfaulted** (0.13.1 on a half-copied file: exit 139, no output at all). All three walkers now clamp to the bytes actually present and report the audio that is there. **Not yet exercised end to end above 4 GB**: this host has 18 GB of RAM and 19 GB free, and crête loads the whole file before decoding, so a genuine >4 GB decode needs a bigger machine. The 64-bit size *path* is exercised; the >4 GB *allocation* is not.

v0.15.0 closes two more. **The joint true peak excluded nothing.** `peak_dbfs` (the joint *sample* peak) has skipped unscored channels since v0.12.0, but the loop computing `max_true_peak_dbtp` had no such guard, so on a multichannel file the two were measured over different channel sets and `--dr-lfe` reached one of them and not the other. True peak is ≥ sample peak by definition and the excess is inter-sample overshoot — a few tenths of a dB — so a file whose LFE is its loudest channel produced an uninterpretable reading, and `plr_db` inherited it, mixing an LFE-inclusive numerator with an LFE-excluded denominator. On a synthetic 5.1 with every channel at −20 dBFS and the LFE at −3, sample peak −20.000 was reported against a true peak of **−2.986**: a 17.01 dB gap, all of it the LFE. It never surfaced on the corpus because the LFE has to be the loudest channel and there it never is (real DSOTM 5.1: LFE −11.14, L −1.67). The joint true peak now takes the scored channels, matching the sample peak and honouring `--dr-lfe`; the same synthetic reads −19.992, i.e. 0.008 dB of genuine overshoot. Two invariants were measured on all six carriers of the DSOTM Blu-ray, whole-JSON: **`--dr-lfe include` on 0.15.0 reproduces `--dr-lfe include` on 0.14.0 exactly** (the old behaviour was unconditional inclusion, so that is the bit-identical path), and **at the default setting nothing moved either** — because in this corpus the LFE is never the loudest channel, so the guard is a no-op on every real file we hold and bites only where an LFE actually dominates. Mono and stereo have no LFE and are unaffected by construction, and no MAAT-referenced value moves: MAAT reads at most two channels, so every referenced `Max. TPL` row is stereo/mono or per-channel.

v0.15.0 also adds **`codec_profile`** (see *Immersive carriers*) and **SACD ISO reading** (see *SACD disc images*) — the format gap a user with a DSD collection hits first.

v0.16.0 adds the **DST decoder**, so SACD multichannel areas measure rather than being refused — see *SACD disc images*. Written from ISO/IEC 14496-3 Subpart 10; lossless, so the numbers are the disc's. It carries its own correctness assertion (every frame must consume its own coded length), which exists because the bug found while writing it satisfied every other plausibility check available: frames parsed, mispredictions looked healthy at 0.75 %, and the output was 1-bit data that decimated to audible sound, while actually being the prediction filter free-running on its own output.

Two items are still genuinely open, neither a release blocker. First, an **external oracle for the disc-audio and SACD formats**: TrueHD / DTS-HD MA / AC-3 decode is gated against crête's own recorded values and against the LPCM carrier of the same master, but against nothing outside crête, and the same is true of DST. For SACD the right comparison is written and is the strongest gate in that suite — crête on the ISO must equal crête on the `.dsf` extracted from the same disc, two independent container readers over one decode chain, no snapshot — but it **is not running in CI**: it needs the DSD64 corpus staged beside the SACD one, and wiring that through the harness's corpus-reuse mechanism would make the whole SACD suite skip whenever that sibling was not ready. Until then the suite gates drift against recorded values, not correctness against an oracle. Second, **DST speed**: roughly 2× realtime for six channels. All channels share one arithmetic decoder so no parallelism exists inside a frame, but the prediction history is reinitialised per frame, so frames are independent and could be decoded in parallel.

### FIX 12 — per-channel DR and album rounding (0.11.3, validated)

Weekly #53 passed 141/141 with no catastrophic breach, but the DR deltas underneath exposed two real defects, both now fixed:

- **Per-channel DR shared a single joint RMS across channels**, so `DR LEFT` and `DR RIGHT` could differ only through the peak term. The error cancelled in the mean — mean-of-L/R matched MAAT to a **0.01 dB median** — but split wrongly per channel (up to **2.37 dB**, spread often opposite in sign), and collapsed to `DR LEFT == DR RIGHT` bit-identically whenever both channels shared a 2nd-highest block peak. Each channel now uses its own top-20% RMS, per the TT DR / PMF definition. Block set, gate and top-20% count are unchanged.
- **Album DR rounded twice** (`round(mean(round(track_dr)))`), which desynchronised the album score from MAAT even when every individual track matched. It now averages the raw per-track DR and rounds once.

Both are covered by `scripts/check_fix12.sh`, a zero-dependency synthetic self-check with analytically known truth (no corpus required). **The corpus has since confirmed both.** Weekly #54 collapsed the per-channel DR flags from **83 → 8** (LEFT) and **77 → 5** (RIGHT), with qualification, mqa, dsd256 and dsd512 all reaching zero; the error this was measuring directly — crête's L−R spread against MAAT's — fell to a **median of 0.000 dB** (max 0.29), against pre-fix errors of up to 3.4 dB with the sign frequently inverted. Where crête still reports L == R, MAAT reports L == R too. Album mismatches went 3 → 2. Two later averaging and block-set corrections (v0.12.x `--dr-mean quadratic`, v0.14.0 `--dr-blocks reference`) have since improved per-channel DR by a further 27–38 % and then 33 %, so the numbers below are the *starting* point of that sequence, not the current state.

Note: joint DR is the mean of the per-channel values, so correcting the RMS term shifts joint DR slightly **upward** on channel-asymmetric material (zero shift when L and R carry equal energy). This is the PMF definition, and it is the opposite direction from the residual integer-DR bias that was open at the time — in the event joint DR improved rather than drifting, and the bias was closed separately by the quadratic mean (net signed bias +9 → 0 over integer DR mismatches, Weekly #56).

### DSD output-rate parity — 44.1 kHz is the parity-guaranteed rate

DSD DR is output-rate dependent: at 352.8 kHz the retained ultrasonic noise-shaping inflates the 2nd-highest block peak far more than it inflates RMS, so DR reads +1…+3 higher than at 44.1 kHz. crête and MAAT agree on the direction but not the magnitude.

**The shipping default, `--dsd-out-rate 44100`, is the rate crête's parity claims apply to.** At that rate the **continuous** per-track DR comparison flags nothing on any of the 8 DSD albums (Weekly #77: the only `DR (PMF) raw` flags in the whole run are 3 tracks of `Thriller_DSD64@352800`), and album DR matches on 7 of 8. The exception is not rate-related: `BoneyM_10k_DSD128@44100` misses a `.5` boundary by **0.020 dB**, which no aggregation rule can fix and which is documented as an accuracy question rather than a rounding one. At 352.8 kHz, by contrast, `Thriller_DSD64` mismatches at album level *and* on 4 of 9 tracks with a maximum integer delta of 2. **Results at 88.2 / 176.4 / 352.8 kHz are offered for analysis, not parity** — treat them as a different measurement, not a more precise one.

The cause is quantified rather than assumed. Band-limiting a 352.8 kHz decode to 20 Hz–20 kHz removes roughly **78 %** of the inflation (3.05 dB of a 3.92 dB gap on one measured track) and returns the affected tracks to their 44.1 kHz integer DR — which is simply a worse-quality version of what decimating to 44.1 kHz already does, with libdsddpcm's 641-tap brickwall. Unfiltered, the ultrasonic content alone drives the meter past full scale (a sample peak of **+1.7 dBFS** on one track). There is no `--bandpass` flag for this reason: it would produce a third number with no reference implementation to validate it against.

### Reference-data hygiene — RESOLVED (2026-07-23)

The harness repo re-measured all 11 QUALIFICATION albums (Thriller × 5 rates, East_Meets_West × 6 rates) fresh against MAAT DROffline MkII 2.2.3 (2026-07-22), and crête was re-validated against the new rows the following day — 11/11 green. Both former outliers are closed:

- **`Min. PSR` flags** had three stacked causes, all resolved: a `+0.00` MAAT export sentinel in the old rows (now dropped at import), a too-coarse 1.0 s short-term sampling grid in crête (fixed in 0.11.0 — see FIX 11), and a genuine definitional difference — MAAT's Min. PSR was reverse-engineered (harness `tools/psr_discriminator/`) as a **0.5 dB/s decaying peak-hold** against 3 s short-term loudness, while crête implements the published AES eBrief 373 formula. A decode-vs-formula confirmation experiment (bit-exact FLAC→WAV through MAAT) then **closed the investigation**: the residual is a genuine formula difference, not a decode artifact, so no MAAT-compat mode is warranted. crête stays spec-true; the harness reporting tolerance reflects the documented variance.
- **`EMW_96k` track 11** was a **MAAT DROffline decode bug on a valid FLAC**: the file's STREAMINFO MD5 verifies (crête decodes it bit-perfectly, matching the 5-rate consensus), yet MAAT deterministically mismeasures RMS/LRA/DR on that encoding — and measures a re-encode of the identical audio correctly. The corpus and references were re-recorded from the new render; the original file is kept as a minimal repro.

### Test harness — overhauled 2026-07, former pending items done

- ~~`BoneyM_10k_DSD128_352` PASS with zero comparisons~~ — an album contributing **zero** metric comparisons now fails outright.
- ~~Baseline-relative regression harness~~ — implemented: a fully-green run snapshots per-album per-metric max |Δ| into `baselines/<suite>.json`; subsequent runs fail only when a metric worsens beyond baseline + margin, capped by an absolute backstop that still catches gross breakage.

~~Still open: a cross-arch identity smoke check.~~ **Done**, and gated weekly — see the top of this section. The comparator asserts *equality*, not a tolerance, on every numeric field of the lossless carriers, because equality is the claim; lossy carriers get their own tier, since FFmpeg's AC-3 decoder is float with per-architecture SIMD and is not required to be bit-identical across them. It is deliberately UNSTABLE-not-FAILED when an agent is offline: a weekly must not collapse because one machine is off, and "could not check" must not read like "checked and passed".

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
sacd.hpp                  SACD (Scarlet Book) ISO reader: TOC, areas, tracks, DSD
dst.hpp                   DST decoder (ISO/IEC 14496-3 Subpart 10), zero-dependency
file_dialog.hpp           Portable native file/folder dialogs (Linux/macOS/Windows)
mqa_detect.hpp            MQA identification (forensics; never feeds metrics)
channels.hpp              Channel layout table: speaker names, LFE, BS.1770 weights
Makefile                  Build system (cli / cli-json / cli-json-ffmpeg / gui / dsd2wav)
scripts/setup-deps.sh     Install build dependencies (macOS + Linux; also runs under zsh)
scripts/setup-deps.ps1    Install build dependencies (Windows, via MSYS2)
scripts/build_ffmpeg_compact.sh  One-time compact static FFmpeg (LGPL-only, decode-only)
scripts/check_fix12.sh    Corpus-free synthetic DR self-check
third_party/libdsddpcm/   Vendored BSD-2-Clause DSD decode kernel (committed, compiled in)
third_party/              Dear ImGui, nlohmann/json, compact FFmpeg (fetched on demand)
```

`analysis.hpp` and `audio.hpp` are deliberately monolithic — single-translation-unit builds keep the CLI dependency-free and the build trivially reproducible across distros. `audio_ffmpeg.hpp` compiles to nothing unless `CRETE_HAS_FFMPEG` is defined by an FFmpeg build tier.

---

## License

MIT License — Ashwin Balaji 2026.

**DSD decode** is provided by the vendored **libdsddpcm** (`third_party/libdsddpcm/`, release v0.2.1), which is **BSD-2-Clause** — a clean-room implementation that shares no code with the LGPL `foo_input_sacd` transcription it replaced. Its license text ships at `third_party/libdsddpcm/LICENSE`. This replaces the former `dsd_lut.hpp` (LGPL-2.1, transcribed from foobar's `libdsdpcm`), which is retired to `dsd_lut.hpp.bak` and is no longer built. crête's licensing is therefore now cleanly **MIT + BSD-2-Clause**.

`crete-ffmpeg` builds statically link a compact FFmpeg configured LGPL-only (decode-only, no GPL or nonfree components); FFmpeg is used under LGPL 2.1+.
