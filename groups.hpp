// ============================================================================
// groups.hpp — what an input path expands into, and how each expansion is
//              measured. Shared by the CLI and the GUI.
//
// An input path is not one file and one track. crête recognises three shapes,
// and both front-ends must agree on all three or they measure different things
// from the same folder:
//
//   1. a standalone audio file            -> 1 track
//   2. a cue sheet + its monolithic audio -> N tracks sliced from one decode
//   3. an SACD .iso                       -> N tracks, each read from its own
//                                            sectors (NOT one decode sliced)
//
// This header exists because the GUI used to know only shape 1. It scanned a
// folder with is_supported_format(), so a cue sheet was ignored (while the
// landing screen advertised "CUE") and a disc image was invisible. Rather than
// teach it the other two shapes separately -- which is how two front-ends come
// to disagree about a corpus -- the whole of collect_groups() and the per-group
// analysis moved here verbatim, and main.cpp now calls in like everyone else.
// Same rule channels.hpp already follows for layouts.
//
// Warnings and errors are returned as strings rather than printed. The CLI
// replays them to stderr after the progress line; the GUI appends them to its
// error log. Neither front-end's reporting style is baked in here.
// ============================================================================
#pragma once

#include "analysis.hpp"
#include "audio.hpp"
#include "budget.hpp"
#include "cue.hpp"
#include "mqa_detect.hpp"
#include "sacd.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace grp {

namespace fs = std::filesystem;

// ── One unit of work ───────────────────────────────────────────────────────
struct Group {
    std::string audio_path;
    cue::Sheet  sheet;   // empty tracks => analyse audio_path as one file

    // SACD ISO. A third shape alongside "one file" and "cue-sliced": the disc
    // TOC already names N tracks by sector range, so unlike a cue this is NOT
    // a slice of one decoded buffer. Each track is read from its own sectors
    // and decoded on its own -- the stereo area of a 43-minute disc is 1.8 GB
    // of f64 at the default 44.1 kHz output and 14.6 GB at 352.8, so decoding
    // the area once and slicing is not an option.
    bool       is_sacd = false;
    sacd::Area sacd_area;

    size_t track_count() const {
        if (is_sacd) return sacd_area.tracks.size();
        return sheet.tracks.empty() ? 1 : sheet.tracks.size();
    }
};

// Every measurement axis a group needs. Assembled by the front-end from its
// flags (CLI) or its controls (GUI) so analyse_group() reads exactly one
// struct and neither front-end can quietly default an axis the other sets.
struct Options {
    dsd::Options   dsd;
    dr::RmsMean    rms_mean = dr::RmsMean::Quadratic;
    dr::LfeScoring lfe      = dr::LfeScoring::Exclude;
    dr::BlockSet   blocks   = dr::BlockSet::Reference;
    int            stream   = -1;   // container audio-stream index, -1 = auto
};

// ── Natural (numeric-aware) ordering ───────────────────────────────────────
//
// Album order, for the very common case where it is encoded in the filename.
// A plain lexicographic sort compares "10" against "2" one character at a
// time and puts track 10 third:
//
//     1 - Track    10 - Track    11 - Track    2 - Track    20 - Track  ...
//
// which is what made an album list "willy nilly". Zero-padded numbering
// ("01".."14") happens to sort correctly either way, which is why this went
// unnoticed -- most of the reference corpus is padded.
//
// Digit runs compare by VALUE, everything else case-insensitively, with a
// raw-byte tiebreak at the end so the result is a total order (std::sort has
// undefined behaviour on a comparator that is not a strict weak ordering, and
// "mostly ordered" is not a thing it tolerates).
//
// Deliberately NOT metadata-driven: crete parses no tags, and inventing a tag
// reader here would make listing order depend on how a file was tagged rather
// than on what the folder plainly shows.
inline int natural_compare(const std::string& a, const std::string& b) {
    auto lower = [](char c) {
        return static_cast<unsigned char>(
            std::tolower(static_cast<unsigned char>(c)));
    };
    auto is_digit = [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
    };

    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (is_digit(a[i]) && is_digit(b[j])) {
            const size_t zi = i, zj = j;
            while (i < a.size() && a[i] == '0') ++i;
            while (j < b.size() && b[j] == '0') ++j;
            size_t di = i, dj = j;
            while (di < a.size() && is_digit(a[di])) ++di;
            while (dj < b.size() && is_digit(b[dj])) ++dj;
            const size_t la = di - i, lb = dj - j;
            // More significant digits => larger value (leading zeros stripped).
            if (la != lb) return la < lb ? -1 : 1;
            for (size_t k = 0; k < la; ++k)
                if (a[i + k] != b[j + k]) return a[i + k] < b[j + k] ? -1 : 1;
            // Same value: order by how it was written, so "01" and "1" have a
            // stable relative position instead of comparing equal.
            const size_t pad_a = i - zi, pad_b = j - zj;
            if (pad_a != pad_b) return pad_a < pad_b ? -1 : 1;
            i = di; j = dj;
        } else {
            const unsigned char ca = lower(a[i]), cb = lower(b[j]);
            if (ca != cb) return ca < cb ? -1 : 1;
            ++i; ++j;
        }
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    if (a != b) return a < b ? -1 : 1;   // differed only by case
    return 0;
}

inline bool natural_less(const std::string& a, const std::string& b) {
    return natural_compare(a, b) < 0;
}

// ── Path -> groups ─────────────────────────────────────────────────────────

// Inode-level dedup. We can't compare claimed paths to directory entries as
// byte-equal strings, because UTF-8 normalization differs between filesystems
// — notably, a cue file generated on Linux/Windows usually carries NFC
// codepoints (`é` = U+00E9), while macOS APFS may report the same filename
// from a directory listing in NFD (`é` = U+0065 U+0301). The two strings name
// the same file but compare unequal; without this dedup the referenced audio
// file would be analysed twice — once via the cue (sliced into N tracks) and
// once standalone (yielding N+1 total). fs::equivalent goes to stat(2) for
// device + inode identity and sidesteps the whole question.
inline bool is_same_file(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::equivalent(a, b, ec);   // false on any error; cheap and safe
}

// Expand one input path. Warnings (a malformed cue, an .iso that is not an
// SACD) are appended to `warnings`; they are not failures, and the caller
// decides how to show them.
inline std::vector<Group> collect(const std::string& path,
                                  std::vector<std::string>& warnings) {
    std::vector<Group> groups;

    if (fs::is_regular_file(path)) {
        // A bare file argument is never cue-sliced: there's no directory
        // context to find a companion .cue, and the user named the file
        // explicitly.
        std::string fext = audio::get_extension(path);
        if (audio::is_disc_image_format(fext)) {
            // Decided by CONTENT: .iso names every disc image ever made, so
            // a data DVD must not be handed to the audio path.
            if (sacd::looks_like_sacd_iso(path)) {
                Group g;
                g.audio_path = path;
                g.is_sacd    = true;
                groups.push_back(std::move(g));
            } else if (const std::string kind = dvd::image_kind(path); !kind.empty()) {
                const char* dir = kind == "DVD-Audio" ? "AUDIO_TS" : "VIDEO_TS";
                warnings.push_back("Warning: " + fs::path(path).filename().string()
                                   + " is a " + kind + " disc image. crete reads DVD title "
                                   "sets from the disc's folders, not from an image: mount it "
                                   "(open it in Finder, or `mount -o loop` on Linux) "
                                   "and measure its " + dir + " folder.");
            } else {
                warnings.push_back("Warning: " + fs::path(path).filename().string()
                                   + " is not an SACD image (no SACDMTOC at LSN 510)");
            }
            return groups;
        }
        if (audio::is_supported_format(path)) {
            Group g;
            g.audio_path = path;
            groups.push_back(std::move(g));
        }
        return groups;
    }
    if (!fs::is_directory(path)) return groups;

    // Pass 1: enumerate cue and audio files separately.
    std::vector<std::string> cue_paths;
    std::vector<std::string> audio_paths;
    for (const auto& e : fs::directory_iterator(path)) {
        if (!e.is_regular_file()) continue;
        std::string fp = e.path().string();
        std::string ext = audio::get_extension(fp);
        if (ext == ".cue")                       cue_paths.push_back(fp);
        else if (audio::is_disc_image_format(ext)) {
            if (sacd::looks_like_sacd_iso(fp)) audio_paths.push_back(fp);
        }
        else if (audio::is_supported_format(fp)) audio_paths.push_back(fp);
    }
    std::sort(cue_paths.begin(), cue_paths.end(), natural_less);
    std::sort(audio_paths.begin(), audio_paths.end(), natural_less);

    // Pass 2: parse each cue, resolve its referenced audio file(s), claim
    // them. A sheet may name several FILEs (one per vinyl side or disc), and
    // INDEX times restart in each, so every run of consecutive tracks in the
    // same FILE becomes its own group -- sliced against its own decode, never
    // against another file's timeline.
    std::vector<fs::path> claimed_paths;
    for (const auto& cp : cue_paths) {
        cue::Sheet sheet;
        if (!cue::parse(cp, sheet)) {
            warnings.push_back("Warning: failed to parse cue file: " + cp);
            continue;
        }
        const std::string cue_name = fs::path(cp).filename().string();
        const auto& all = sheet.tracks;
        for (size_t i = 0, j; i < all.size(); i = j) {
            j = i + 1;
            while (j < all.size() && all[j].file == all[i].file) ++j;

            std::string ref = cue::resolve_file(sheet, all[i].file);
            if (ref.empty()) {
                warnings.push_back("Warning: cue " + cue_name
                                   + " references missing file: " + all[i].file);
                continue;
            }
            if (!audio::is_supported_format(ref)) {
                warnings.push_back("Warning: cue " + cue_name
                                   + " references unsupported file: " + ref);
                continue;
            }
            Group g;
            g.audio_path = ref;
            g.sheet = sheet;
            g.sheet.tracks.assign(all.begin() + static_cast<std::ptrdiff_t>(i),
                                  all.begin() + static_cast<std::ptrdiff_t>(j));
            groups.push_back(std::move(g));
            claimed_paths.emplace_back(ref);
        }
    }

    // Pass 3: audio files not claimed by any cue → single-track groups.
    // is_same_file uses fs::equivalent — see comment above.
    for (const auto& ap : audio_paths) {
        bool is_claimed = false;
        fs::path ap_path(ap);
        for (const auto& cp : claimed_paths) {
            if (is_same_file(ap_path, cp)) { is_claimed = true; break; }
        }
        if (is_claimed) continue;
        Group g;
        g.audio_path = ap;
        if (audio::is_disc_image_format(audio::get_extension(ap)))
            g.is_sacd = true;
        groups.push_back(std::move(g));
    }

    return groups;
}

// Resolve each SACD image's area up front: the track count comes from the disc
// TOC, and a bad area selection (asking for multichannel on a stereo-only
// disc, or for an area crête cannot decode) must fail HERE, on the calling
// thread, rather than inside a worker. Same fail-fast rule dsd::validate()
// follows. Returns false and fills `error` on the first image that cannot be
// resolved; the caller decides whether that is fatal.
inline bool resolve_sacd_areas(std::vector<Group>& groups, sacd::AreaSel sel,
                               std::string& error) {
    for (auto& g : groups) {
        if (!g.is_sacd) continue;
        try {
            sacd::Disc d = sacd::probe(g.audio_path);
            g.sacd_area = sacd::pick_area(d, sel);
        } catch (const std::exception& e) {
            error = "Error: " + fs::path(g.audio_path).filename().string()
                  + ": " + e.what();
            return false;
        }
    }
    return true;
}

inline size_t count_tracks(const std::vector<Group>& groups) {
    size_t n = 0;
    for (const auto& g : groups) n += g.track_count();
    return n;
}

// ── Group -> tracks ────────────────────────────────────────────────────────

// Result of measuring one group. `errlog` carries this group's warnings and
// its decode error, if any; a group that threw contributes no tracks but is
// not otherwise special -- a partial album still reports the tracks that read.
//
// `notes` holds the same lines as `errlog`, one per entry and without the
// trailing newline, so a machine-readable format can carry them as a list
// instead of a blob a consumer would have to parse. `failed` counts groups
// that produced no tracks at all, and `degraded` counts tracks whose input
// was measurably damaged -- together they are what lets the CLI return a
// distinct exit code instead of the flat 0 that made a corrupt album look
// like a clean one.
struct GroupResult {
    std::vector<dr::TrackResult> tracks;
    std::string                  errlog;
    std::vector<std::string>     notes;
    size_t                       failed   = 0;
    size_t                       degraded = 0;

    // One place that appends, so errlog and notes cannot drift apart.
    void note(const std::string& line) {
        errlog += line + "\n";
        notes.push_back(line);
    }
};

// `progress` is called once per track, with the name about to be measured,
// before the work for that track starts. It runs on the worker thread, so a
// front-end that touches shared state from it must do its own locking.
using ProgressFn = std::function<void(const std::string&)>;

inline GroupResult analyse(const Group& g, const Options& opt,
                           const ProgressFn& progress) {
    GroupResult out;
    std::string basename = fs::path(g.audio_path).filename().string();
    std::string ext_str  = fs::path(g.audio_path).extension().string();

    try {
        // ── SACD ISO: N tracks straight from the disc TOC ──────────────────
        // Handled before decode_file() because an ISO is not one stream: the
        // area's track list already gives sector ranges, and each track is
        // decoded from its own sectors rather than sliced out of one huge
        // decode (see Group).
        if (g.is_sacd) {
            sacd::Image img(g.audio_path);
            const auto& area = g.sacd_area;
            const std::string stem = fs::path(g.audio_path).stem().string();
            for (size_t t = 0; t < area.tracks.size(); ++t) {
                char tn[8];
                std::snprintf(tn, sizeof(tn), "%02zu", t + 1);
                std::string tname = stem + " - " + tn;
                if (progress) progress(tname);
                auto ad = audio::decode_sacd_track(img, area, t, opt.dsd);
                out.tracks.push_back(dr::analyze_track(
                    ad.channels, ad.sample_rate, ad.bit_depth,
                    ad.codec, tname, opt.rms_mean, opt.lfe, opt.blocks));
                out.tracks.back().mqa = mqa::analyze_pcm(
                    ad.channels, ad.sample_rate, ad.bit_depth);
            }
            return out;
        }

        auto ad = audio::decode_file(g.audio_path, opt.dsd, opt.stream);

        // Several audio streams and no explicit choice: say which one was
        // measured. A Blu-ray rip's LPCM 5.1, DTS-HD MA 5.1 and TrueHD 7.1
        // tracks are different mixes with different DR, so silently metering
        // whichever FFmpeg ranks highest would make the number unreproducible
        // without this line.
        if (opt.stream < 0 && ad.audio_stream_count > 1) {
            out.note("Warning: " + basename + " has "
                + std::to_string(ad.audio_stream_count)
                + " audio streams; measured stream "
                + std::to_string(ad.stream_index)
                + " (" + ad.codec + ", "
                + std::to_string(ad.num_channels()) + " ch, "
                + std::to_string(ad.sample_rate)
                + " Hz). Use --stream N to choose.");
        }

        // Integrity findings from the decoder: a failed frame CRC, a declared
        // length the file does not hold. These do NOT stop the measurement --
        // the numbers below are whatever the damaged bytes decode to -- but
        // they mark every one of them as untrustworthy, which is the whole
        // point. Each one counts toward `degraded`, so the run's exit status
        // can say so too.
        for (const auto& w : ad.warnings) {
            out.note("Warning: " + basename + ": " + w);
            out.degraded++;
        }

        // File-level MQA check, stamped onto every track from this decode.
        const mqa::Info minfo =
            mqa::analyze_pcm(ad.channels, ad.sample_rate, ad.bit_depth);

        if (g.sheet.tracks.empty()) {
            if (progress) progress(basename);
            out.tracks.push_back(dr::analyze_track(
                ad.channels, ad.sample_rate, ad.bit_depth,
                ad.codec, basename, opt.rms_mean, opt.lfe, opt.blocks));
            out.tracks.back().mqa                = minfo;
            out.tracks.back().stream_index       = ad.stream_index;
            out.tracks.back().audio_stream_count = ad.audio_stream_count;
            out.tracks.back().codec_profile      = ad.codec_profile;
        } else {
            size_t total_samples = ad.channels.empty()
                                   ? 0 : ad.channels[0].size();
            const auto& tracks_cue = g.sheet.tracks;

            for (size_t i = 0; i < tracks_cue.size(); ++i) {
                const auto& trk = tracks_cue[i];

                uint64_t start = cue::frames_to_samples(
                    trk.start_frame, ad.sample_rate);
                uint64_t end = (i + 1 < tracks_cue.size())
                    ? cue::frames_to_samples(
                          tracks_cue[i + 1].start_frame, ad.sample_rate)
                    : total_samples;
                if (end > total_samples) end = total_samples;

                // NN - Title.ext, mirroring sacd_extract / common splitter
                // conventions so cue-derived tracks pair cleanly with
                // per-track siblings under helpers.py's _base_norm
                // normalization.
                char track_name[512];
                std::snprintf(track_name, sizeof(track_name),
                              "%02d - %s%s",
                              trk.number, trk.title.c_str(),
                              ext_str.c_str());

                if (progress) progress(track_name);

                // A track that cannot be sliced is DROPPED from the album,
                // so the run cannot claim to be complete: it counts toward
                // `degraded` and the exit status becomes 2.
                if (start >= end) {
                    out.note(std::string("Warning: empty cue range for ")
                             + track_name + " -- track dropped (its start is"
                               " not before the next track's, or past the end"
                               " of " + basename + ")");
                    out.degraded++;
                    continue;
                }

                std::vector<std::vector<double>> slice(ad.channels.size());
                for (size_t c = 0; c < ad.channels.size(); ++c) {
                    slice[c].assign(
                        ad.channels[c].begin() + static_cast<std::ptrdiff_t>(start),
                        ad.channels[c].begin() + static_cast<std::ptrdiff_t>(end));
                }

                out.tracks.push_back(dr::analyze_track(
                    slice, ad.sample_rate, ad.bit_depth,
                    ad.codec, track_name, opt.rms_mean, opt.lfe, opt.blocks));
                out.tracks.back().mqa                = minfo;
                out.tracks.back().stream_index       = ad.stream_index;
                out.tracks.back().audio_stream_count = ad.audio_stream_count;
                out.tracks.back().codec_profile      = ad.codec_profile;
            }
        }

        // Result-level sanity check: on integer PCM a sample peak above
        // 0 dBFS is arithmetically impossible -- the samples are bounded by
        // their own word length -- so seeing one is proof the decode went
        // wrong, whatever the CRCs said. It is the check that catches the
        // damage no checksum covers, because WAV and AIFF carry none at all.
        //
        // Deliberately NOT applied to float PCM, to decimated DSD or to
        // FFmpeg's float decoders: all three can legitimately exceed full
        // scale (DSD's retained noise shaping alone reads +1.7 dBFS at
        // 352.8 kHz out), so the invariant only holds where integers enforce
        // it. ad.integer_pcm is set by exactly those decoders that do.
        if (ad.integer_pcm) {
            for (const auto& t : out.tracks) {
                if (t.peak_dbfs > 0.0) {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "%.2f", t.peak_dbfs);
                    out.note("Warning: " + basename + ": CORRUPT -- sample peak "
                             + buf + " dBFS on " + std::to_string(ad.bit_depth)
                             + "-bit integer PCM, which cannot exceed 0 dBFS. "
                               "The decode produced values outside the format's "
                               "range; every number for this track is unreliable.");
                    out.degraded++;
                }
            }
        }
    } catch (const std::exception& e) {
        out.note("Error: " + basename + ": " + e.what());
        out.failed++;
    }
    return out;
}

// ── Parallel run over a group list ─────────────────────────────────────────
//
// A group = one standalone file, one cue-sliced monolith yielding N contiguous
// tracks, or one SACD area yielding N TOC tracks. decode_file/analyze_track
// hold no shared mutable state, DSD decodes still fan out per-channel inside
// each group, and results land in per-group slots so the flattened output
// order matches a sequential walk exactly. Measurement is unchanged --
// parallelism only reorders the wall-clock schedule.
//
// `jobs` <= 0 means "all cores". `cancelled`, when supplied, is polled between
// groups so a GUI can stop early; a cancelled run returns whatever finished.
struct RunResult {
    std::vector<dr::TrackResult> tracks;
    std::string                  errlog;
    std::vector<std::string>     notes;
    size_t                       failed   = 0;   // groups that yielded nothing
    size_t                       degraded = 0;   // tracks with damaged input

    // True when the run measured something but not everything, or measured
    // something it could prove was damaged. Distinct from "nothing decoded",
    // which the caller sees as an empty track list.
    bool partial() const { return failed > 0 || degraded > 0; }
};

// ── How much memory a group will need ──────────────────────────────────────
//
// Sized on what the worker HOLDS AT ONCE, which differs by group shape:
//
//   plain file   the decoded file, whole
//   cue sheet    the same -- the monolith is decoded once and then sliced, so
//                a 70-minute CD image costs its full length, not a track's
//   SACD image   the LARGEST TRACK only. An area is decoded track by track
//                from its own sector ranges, never as one buffer; the stereo
//                area of a 43-minute disc would be 14.6 GB of f64 at 352.8 kHz
//                out, which is exactly why it is not done that way.
inline uint64_t estimate_bytes(const Group& g, const Options& opt) {
    const unsigned out_rate = opt.dsd.out_rate ? opt.dsd.out_rate : 44100;

    if (g.is_sacd) {
        const auto& a = g.sacd_area;
        uint64_t total_lsn = 0, max_lsn = 0;
        for (const auto& t : a.tracks) {
            total_lsn += t.length_lsn;
            max_lsn = std::max<uint64_t>(max_lsn, t.length_lsn);
        }
        // Sector share of the area's declared playing time. Exact for a plain
        // DSD area; for a DST area the compression ratio varies per track, so
        // this is proportional rather than precise -- good enough to size a
        // buffer, and an ISO is one group so it never gates parallelism.
        double secs = (total_lsn && a.playtime_secs > 0)
                    ? a.playtime_secs * (double(max_lsn) / double(total_lsn))
                    : a.playtime_secs;
        double decoded = double(a.channels ? a.channels : 2) * out_rate * secs * 8.0;
        uint64_t est = uint64_t(double(max_lsn) * 2048.0 + budget::MEM_K * decoded);
        return std::max(est, budget::WORKER_FLOOR);
    }

    // A DVD title set is several files read as one stream, so its cost is
    // the whole set, not the fragment that named it.
    const std::vector<std::string> parts = dvd::fragments_of(g.audio_path);
    std::error_code ec;
    uint64_t fsz = 0;
    for (const auto& p : parts) {
        uint64_t n = std::filesystem::file_size(p, ec);
        if (!ec) fsz += n;
    }

    budget::Shape sh = budget::peek(g.audio_path);

#ifdef CRETE_HAS_FFMPEG
    // No native header answered -- a container. File size is a USELESS proxy
    // here: a 40 MB AC-3 track decodes to several GB of f64, so the generic
    // ratio under-reads it by more than ten times and would happily start a
    // worker per core on it. The probe already knows channels and rate; it
    // now reports duration too, which is everything the model needs.
    if (!sh.known) {
        const auto streams = audio::probe_streams(parts);
        double best = 0;
        for (const auto& si : streams) {
            if (!si.decodable || si.channels <= 0 || si.sample_rate <= 0) continue;
            if (opt.stream >= 0 && si.index != opt.stream) continue;
            best = std::max(best, double(si.channels) * si.sample_rate * si.duration_secs);
            if (opt.stream >= 0) break;
        }
        // No --stream given: take the HEAVIEST stream. Which one FFmpeg picks
        // unaided is documented here as unstable across remuxes of the same
        // content, so planning for the cheapest would be planning for luck.
        if (best > 0)
            return std::max<uint64_t>(uint64_t(double(fsz) + budget::MEM_K * best * 8.0),
                                      budget::WORKER_FLOOR);
    }
#endif

    return budget::estimate_bytes(fsz, sh, out_rate);
}

// The peak over ALL groups, not the mean. The pool is order-preserving and
// hands out work by an atomic cursor, so nothing stops every worker sitting on
// a long track at the same moment -- which is what the Weekly #69/#70 dsd512
// kills came down to.
inline uint64_t largest_estimate(const std::vector<Group>& groups, const Options& opt) {
    uint64_t m = 0;
    for (const auto& g : groups) m = std::max(m, estimate_bytes(g, opt));
    return m;
}

inline RunResult run(const std::vector<Group>& groups, const Options& opt,
                     int jobs, const ProgressFn& progress,
                     const std::atomic<bool>* cancelled = nullptr) {
    const size_t ngroups = groups.size();
    std::vector<GroupResult> slots(ngroups);
    std::atomic<size_t> next{0};

    auto process = [&](size_t gi) {
        slots[gi] = analyse(groups[gi], opt, progress);
    };

    unsigned hw = std::thread::hardware_concurrency();
    size_t nthreads = jobs > 0 ? static_cast<size_t>(jobs)
                               : (hw ? static_cast<size_t>(hw) : 4);
    nthreads = std::min(nthreads, ngroups);

    if (nthreads <= 1) {
        for (size_t gi = 0; gi < ngroups; ++gi) {
            if (cancelled && cancelled->load()) break;
            process(gi);
        }
    } else {
        auto worker = [&]() {
            for (;;) {
                if (cancelled && cancelled->load()) break;
                size_t gi = next.fetch_add(1);
                if (gi >= ngroups) break;
                process(gi);
            }
        };
        std::vector<std::thread> pool;
        pool.reserve(nthreads);
        for (size_t k = 0; k < nthreads; ++k) pool.emplace_back(worker);
        for (auto& th : pool) th.join();
    }

    // Flatten per-group slots in original group order; errors and warnings are
    // gathered afterwards so they never interleave with a progress line.
    RunResult out;
    out.tracks.reserve(count_tracks(groups));
    for (size_t gi = 0; gi < ngroups; ++gi)
        for (auto& t : slots[gi].tracks) out.tracks.push_back(std::move(t));
    for (size_t gi = 0; gi < ngroups; ++gi) {
        out.errlog += slots[gi].errlog;
        for (auto& n : slots[gi].notes) out.notes.push_back(std::move(n));
        out.failed   += slots[gi].failed;
        out.degraded += slots[gi].degraded;
    }
    return out;
}

}  // namespace grp
