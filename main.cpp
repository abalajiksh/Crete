// ============================================================================
// crête — Zero-dependency Dynamic Range Meter
//
// Measures TT Dynamic Range (Pleasurize Music Foundation algorithm),
// EBU R128 integrated loudness, and crest factor for audio files.
//
// Supported formats: WAV, AIFF, FLAC, DSF, DFF (DSD)
// Cue sheets: parsed automatically when present alongside a monolithic
//             audio file; each TRACK becomes its own analysed entry.
// Output compatible with dr.loudness-war.info submission format.
//
// Usage: crete [options] <path...>
//
// Build: make  (or: g++ -std=c++17 -O2 -o crete main.cpp)
// ============================================================================

#include "analysis.hpp"
#include "audio.hpp"
#include "cue.hpp"

#ifdef CRETE_HAS_JSON
#include <nlohmann/json.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>

#ifdef CRETE_HAS_FFMPEG
extern "C" {
#include <libavutil/avutil.h>
}
#endif

// ── Version info ───────────────────────────────────────────────────────────
#ifndef CRETE_VERSION
#define CRETE_VERSION "0.14.0"
#endif

static constexpr const char* PROGRAM_NAME    = "crête";
static constexpr const char* PROGRAM_BIN     = "crete";
static constexpr const char* PROGRAM_VERSION = CRETE_VERSION;

// DSD decode options in effect for this run (chain / final-stage filter /
// output rate). Set once from --dsd-* flags in main(); read by print_json() so
// a JSON result always records how its DSD numbers were produced.
static dsd::Options g_dsd_opt;

// Container audio-stream index to decode, or -1 for "let FFmpeg choose".
// Only multi-stream containers (.mkv/.mp4 Blu-ray rips) can honour it.
static int g_stream = -1;
static dr::RmsMean  g_rms_mean = dr::RmsMean::Quadratic;

// Whether the LFE counts toward the TT DR score. Exclude is crete's rule and
// the default; Include matches foobar2000's DR Meter. Affects TT DR only --
// BS.1770 loudness and Min. PSR are untouched. See dr::LfeScoring.
static dr::LfeScoring g_lfe = dr::LfeScoring::Exclude;
// Which 3 s blocks enter the DR pools. Reference is the PMF reference
// implementation and the default since 0.14.0 -- it keeps the trailing partial
// block and drops the block gate, both of which biased crete's DR low. Legacy
// is the pre-0.14.0 accounting, kept so any reference row recorded under it
// stays reproducible. See dr::BlockSet.
static dr::BlockSet g_dr_blocks = dr::BlockSet::Reference;
static constexpr const char* PROGRAM_DESC    = "TT Dynamic Range Meter (zero-dependency)";

namespace fs = std::filesystem;

#ifdef CRETE_HAS_JSON
using json = nlohmann::json;
#endif

// ── Output format modes ────────────────────────────────────────────────────
enum class OutputFormat {
    Standard,   // dr.loudness-war.info compatible
    Foobar,     // foobar2000 DR Meter style
    Extended,   // MAAT DROffline–style pipe-delimited table
    Detail,     // per-track per-channel block view
    Json,       // machine-readable JSON (requires CRETE_HAS_JSON)
};

// ── Formatting helpers ─────────────────────────────────────────────────────

std::string format_duration(double secs) {
    int m = static_cast<int>(secs) / 60;
    int s = static_cast<int>(secs) % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

std::string format_peak(double peak_dbfs, bool is_clipping, OutputFormat fmt) {
    if (fmt == OutputFormat::Foobar) {
        if (is_clipping) return "   0.00 dB";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%7.2f dB", peak_dbfs);
        return buf;
    } else {
        if (is_clipping) return "over      ";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.2f dB", peak_dbfs);
        return buf;
    }
}

std::string format_sr(uint32_t sr) {
    double k = sr / 1000.0;
    if (k == std::floor(k))
        return std::to_string(static_cast<int>(k)) + "k";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1fk", k);
    return buf;
}

std::string format_signed(double db) {
    char buf[16];
    if (db >= 0.0)
        std::snprintf(buf, sizeof(buf), "+%.2f", db);
    else
        std::snprintf(buf, sizeof(buf), "%.2f", db);
    return buf;
}

std::string format_db(double db) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f", db);
    return buf;
}

std::string strip_ext(const std::string& filename) {
    auto dot = filename.rfind('.');
    if (dot != std::string::npos) return filename.substr(0, dot);
    return filename;
}

std::string get_ext(const std::string& filename) {
    auto dot = filename.rfind('.');
    if (dot != std::string::npos) return filename.substr(dot);
    return "";
}

// ════════════════════════════════════════════════════════════════════════════
// Analysis grouping
//
// An AnalysisGroup represents one decode operation. Two cases:
//   1. sheet.tracks.empty()   — a single-track file, analysed as-is
//   2. sheet.tracks non-empty — a monolithic file paired with a cue sheet;
//      the decoded PCM is sliced per TRACK and each slice analysed separately
//
// collect_groups() scans a directory for cue files first, parses them, and
// claims the audio files they reference. Audio files not claimed by any cue
// fall through to single-track groups. A folder with no cue files behaves
// exactly as it did before cue support was added.
// ════════════════════════════════════════════════════════════════════════════

struct AnalysisGroup {
    std::string audio_path;
    cue::Sheet  sheet;   // empty tracks => analyse audio_path as one file
};

// Inode-level dedup. We can't compare claimed paths to directory entries as
// byte-equal strings, because UTF-8 normalization differs between filesystems
// — notably, a cue file generated on Linux/Windows usually carries NFC
// codepoints (`é` = U+00E9), while macOS APFS may report the same filename
// from a directory listing in NFD (`é` = U+0065 U+0301). The two strings name
// the same file but compare unequal; without this dedup the referenced audio
// file would be analysed twice — once via the cue (sliced into N tracks) and
// once standalone (yielding N+1 total). fs::equivalent goes to stat(2) for
// device + inode identity and sidesteps the whole question.
static bool is_same_file(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::equivalent(a, b, ec);   // false on any error; cheap and safe
}

std::vector<AnalysisGroup> collect_groups(const std::string& path) {
    std::vector<AnalysisGroup> groups;

    if (fs::is_regular_file(path)) {
        // A bare file argument is never cue-sliced: there's no directory
        // context to find a companion .cue, and the user named the file
        // explicitly.
        if (audio::is_supported_format(path)) {
            AnalysisGroup g;
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
        else if (audio::is_supported_format(fp)) audio_paths.push_back(fp);
    }
    std::sort(cue_paths.begin(), cue_paths.end());
    std::sort(audio_paths.begin(), audio_paths.end());

    // Pass 2: parse each cue, resolve its referenced audio file, claim it.
    std::vector<fs::path> claimed_paths;
    claimed_paths.reserve(cue_paths.size());
    for (const auto& cp : cue_paths) {
        cue::Sheet sheet;
        if (!cue::parse(cp, sheet)) {
            std::cerr << "Warning: failed to parse cue file: " << cp << "\n";
            continue;
        }
        std::string ref = cue::resolve_referenced_file(sheet);
        if (ref.empty()) {
            std::cerr << "Warning: cue " << fs::path(cp).filename().string()
                      << " references missing file: "
                      << sheet.referenced_file << "\n";
            continue;
        }
        if (!audio::is_supported_format(ref)) {
            std::cerr << "Warning: cue " << fs::path(cp).filename().string()
                      << " references unsupported file: " << ref << "\n";
            continue;
        }
        AnalysisGroup g;
        g.audio_path = ref;
        g.sheet = std::move(sheet);
        groups.push_back(std::move(g));
        claimed_paths.emplace_back(ref);
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
        AnalysisGroup g;
        g.audio_path = ap;
        groups.push_back(std::move(g));
    }

    return groups;
}

// ── MQA warning block (all human-facing formats) ───────────────────────────
//
// A listener measuring DR is entitled to know the source is MQA-encoded lossy
// audio rather than lossless. This block fires ONLY on a verified datasync
// packet — a 40-bit magic plus a well-formed field structure, which is never a
// coincidence. The LSB-entropy heuristic is deliberately NOT surfaced here: it
// cannot separate MQA from analog tape hiss or a prior lossy generation, and a
// false "this is MQA" on legitimate lossless would betray the whole tool. Hits
// are grouped by their authoritative fields so an album collapses to one line.
std::string format_khz(int hz) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << (hz / 1000.0) << " kHz";
    return os.str();
}

void print_mqa_summary(const std::vector<dr::TrackResult>& tracks) {
    // key: (orig_rate, studio, auth_level, bit_position) → track count
    std::map<std::tuple<int,int,int,int>, int> hits;
    for (const auto& t : tracks) {
        if (t.mqa.datasync_found)
            hits[std::make_tuple(t.mqa.orig_rate, t.mqa.studio ? 1 : 0,
                                 t.mqa.auth_level, t.mqa.bit_position)]++;
    }
    if (hits.empty()) return;

    const std::string top(94, '=');
    const std::string mid(94, '-');

    std::cout << "\n" << top << "\n";
    std::cout << "  ⚠  MQA LOSSY ENCODING DETECTED — source is not lossless\n";
    std::cout << mid << "\n";

    for (const auto& [k, count] : hits) {
        const int  orig   = std::get<0>(k);
        const bool studio = std::get<1>(k) != 0;
        const int  auth   = std::get<2>(k);
        const int  bp     = std::get<3>(k);
        std::cout << "  " << count << " track(s) · "
                  << (studio ? "MQA Studio" : "MQA")
                  << " · authored " << (orig ? format_khz(orig) : "unknown rate")
                  << " · carrier bit " << bp
                  << " · auth level " << auth << "\n";
    }

    std::cout << mid << "\n";
    std::cout << "  DR is measured on the delivered PCM stream; content encoded above the base\n";
    std::cout << "  band is not reconstructed (no open MQA decoder exists).\n";
    std::cout << top << "\n";
}

// ── Standard output (dr.loudness-war.info compatible) ──────────────────────
void print_standard(const std::vector<dr::TrackResult>& tracks,
                    const std::string& folder_path) {
    const std::string sep(94, '-');
    const std::string sep2(94, '=');

    std::cout << sep << "\n";
    std::cout << " Analyzed Folder: " << folder_path << "\n";
    std::cout << sep << "\n";
    std::cout << "DR         Peak       RMS        Filename\n";
    std::cout << sep << "\n\n";

    for (const auto& t : tracks) {
        char dr_str[8];
        std::snprintf(dr_str, sizeof(dr_str), "DR%-3d", t.dr_score);

        std::string peak_str;
        if (t.is_clipping) {
            peak_str = "over      ";
        } else {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.2f dB  ", t.peak_dbfs);
            peak_str = buf;
        }

        char rms_str[16];
        std::snprintf(rms_str, sizeof(rms_str), "%.2f dB", t.rms_dbfs);

        std::printf("%-10s %-10s %-10s %s\n",
                    dr_str, peak_str.c_str(), rms_str, t.filename.c_str());
    }

    std::cout << sep << "\n\n";
    std::cout << " Number of Files: " << tracks.size() << "\n";
    std::cout << " Official DR Value: DR" << dr::album_dr(tracks) << "\n";
    std::cout << "\n" << sep2 << "\n";
}

// ── Foobar2000 style output ────────────────────────────────────────────────
void print_foobar(const std::vector<dr::TrackResult>& tracks,
                  const std::string& folder_name) {
    const std::string sep(80, '-');
    const std::string sep2(80, '=');

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char date_buf[32];
    std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));

    std::cout << PROGRAM_NAME << " " << PROGRAM_VERSION
              << " / TT DR Offline Meter\n";
    std::cout << "log date: " << date_buf << "\n\n";
    std::cout << sep << "\n";
    std::cout << "Analyzed: " << folder_name << "\n";
    std::cout << sep << "\n\n";
    std::cout << "DR         Peak         RMS     Duration Track\n";
    std::cout << sep << "\n";

    for (const auto& t : tracks) {
        char dr_str[8];
        std::snprintf(dr_str, sizeof(dr_str), "DR%-3d", t.dr_score);

        char peak_str[16];
        if (t.is_clipping)
            std::snprintf(peak_str, sizeof(peak_str), "  0.00 dB");
        else
            std::snprintf(peak_str, sizeof(peak_str), "%6.2f dB", t.peak_dbfs);

        char rms_str[16];
        std::snprintf(rms_str, sizeof(rms_str), "%7.2f dB", t.rms_dbfs);

        std::string dur = format_duration(t.duration_secs);
        auto name = strip_ext(t.filename);

        std::printf("%-10s %s  %s %8s %s\n",
                    dr_str, peak_str, rms_str, dur.c_str(), name.c_str());
    }

    std::cout << sep << "\n\n";
    std::cout << "Number of tracks:  " << tracks.size() << "\n";
    std::cout << "Official DR value: DR" << dr::album_dr(tracks) << "\n\n";

    if (!tracks.empty()) {
        const auto& first = tracks[0];
        std::cout << "Samplerate:        " << first.sample_rate << " Hz\n";
        std::cout << "Channels:          " << first.channels << "\n";
        std::cout << "Bits per sample:   " << first.bit_depth << "\n";
        std::cout << "Codec:             " << first.codec << "\n";
    }

    std::cout << sep2 << "\n";
}

// ── Extended output (MAAT DROffline–style pipe-delimited table) ────────────
void print_extended(const std::vector<dr::TrackResult>& tracks,
                    const std::string& folder_path) {
    if (tracks.empty()) return;

    struct Row {
        std::string name, ext, sr, wl;
        std::string tpl, sppm_l, sppm_r, sppm_j;
        std::string rms_l, rms_r;
        std::string mom_l, mom_r, st_l, st_r;
        std::string lufs_i, dr_pmf, bits_used;
        std::string plr, lra;
        std::string mom_j, st_j;
        std::string dr_l, tpl_l, tpl_r, dr_r, min_psr;
    };

    std::vector<Row> rows;
    rows.reserve(tracks.size());

    size_t fname_w = 9;
    size_t sr_w    = 2;

    for (const auto& t : tracks) {
        Row r;
        r.name = strip_ext(t.filename);
        r.ext  = get_ext(t.filename);
        r.sr   = format_sr(t.sample_rate);
        r.wl   = std::to_string(t.bit_depth);

        size_t nch = t.ch_metrics.size();

        r.tpl = format_signed(t.max_true_peak_dbtp);

        double sp_l = (nch > 0) ? t.ch_metrics[0].sample_peak_dbfs : t.peak_dbfs;
        double sp_r = (nch > 1) ? t.ch_metrics[1].sample_peak_dbfs : sp_l;
        r.sppm_l = format_db(sp_l);
        r.sppm_r = format_db(sp_r);
        r.sppm_j = format_db(std::max(sp_l, sp_r));

        r.rms_l = format_db((nch > 0) ? t.ch_metrics[0].rms_dbfs : t.rms_dbfs);
        r.rms_r = format_db((nch > 1) ? t.ch_metrics[1].rms_dbfs
                                       : ((nch > 0) ? t.ch_metrics[0].rms_dbfs : t.rms_dbfs));

        r.mom_l = format_db((nch > 0) ? t.ch_metrics[0].max_momentary_lufs : t.max_momentary_lufs);
        r.mom_r = format_db((nch > 1) ? t.ch_metrics[1].max_momentary_lufs
                                       : ((nch > 0) ? t.ch_metrics[0].max_momentary_lufs : t.max_momentary_lufs));

        r.st_l = format_db((nch > 0) ? t.ch_metrics[0].max_short_term_lufs : t.max_short_term_lufs);
        r.st_r = format_db((nch > 1) ? t.ch_metrics[1].max_short_term_lufs
                                      : ((nch > 0) ? t.ch_metrics[0].max_short_term_lufs : t.max_short_term_lufs));

        r.lufs_i    = format_db(t.integrated_lufs);
        r.dr_pmf    = std::to_string(t.dr_score);
        r.bits_used = std::to_string(t.bit_depth);
        r.plr       = format_db(t.plr_db);
        r.lra       = format_db(t.lra_lu);
        r.mom_j     = format_db(t.max_momentary_lufs);
        r.st_j      = format_db(t.max_short_term_lufs);

        r.dr_l = format_db((nch > 0) ? t.ch_metrics[0].dr_raw : t.dr_score_raw);
        r.dr_r = format_db((nch > 1) ? t.ch_metrics[1].dr_raw
                                      : ((nch > 0) ? t.ch_metrics[0].dr_raw : t.dr_score_raw));

        r.tpl_l = format_signed((nch > 0) ? t.ch_metrics[0].true_peak_dbtp : t.max_true_peak_dbtp);
        r.tpl_r = format_signed((nch > 1) ? t.ch_metrics[1].true_peak_dbtp
                                           : ((nch > 0) ? t.ch_metrics[0].true_peak_dbtp : t.max_true_peak_dbtp));

        r.min_psr = format_db(t.psr_db);

        fname_w = std::max(fname_w, r.name.length());
        sr_w    = std::max(sr_w,    r.sr.length());

        rows.push_back(std::move(r));
    }

    int aFN  = static_cast<int>(fname_w);
    int aFmt = 6;
    int aSR  = static_cast<int>(sr_w);
    int aWL  = 11;
    int aTPL = 8;
    int aSPL = 14;
    int aSPR = 15;
    int aSPJ = 17;
    int aRL  = 8;
    int aRR  = 9;
    int aML  = 11;
    int aMR  = 12;
    int aSL  = 11;
    int aSRt = 12;
    int aLU  = 6;
    int aDR  = 8;
    int aBU  = 9;
    int aPL  = 5;
    int aLA  = 5;
    int aMJ  = 6;
    int aSJ  = 6;
    int aDL  = 7;
    int aTL  = 13;
    int aTR  = 14;
    int aDRR = 8;
    int aPS  = 8;

    auto print_row = [&](
            const char* fn, const char* fmt, const char* sr, const char* wl,
            const char* tpl,
            const char* spl, const char* spr, const char* spj,
            const char* rl,  const char* rr,
            const char* ml,  const char* mr,  const char* sl,  const char* srt,
            const char* lu,  const char* dr,  const char* bu,
            const char* pl,  const char* la,
            const char* mj,  const char* sj,
            const char* dl,  const char* tl,  const char* tr,
            const char* drr, const char* ps)
    {
        std::printf("%*s | %*s | %*s | %*s | %*s | %*s | %*s | %*s | "
                    "%*s | %*s | %*s | %*s | %*s | %*s | %*s | %*s | "
                    "%*s | %*s | %*s | %*s | %*s | %*s | %*s | %*s | "
                    "%*s | %*s | \n",
            aFN, fn,   aFmt, fmt, aSR, sr,   aWL, wl,
            aTPL, tpl,
            aSPL, spl,  aSPR, spr,  aSPJ, spj,
            aRL, rl,    aRR, rr,
            aML, ml,    aMR, mr,    aSL, sl,    aSRt, srt,
            aLU, lu,    aDR, dr,    aBU, bu,
            aPL, pl,    aLA, la,
            aMJ, mj,    aSJ, sj,
            aDL, dl,    aTL, tl,    aTR, tr,
            aDRR, drr,  aPS, ps);
    };

    std::cout << "Folder Path:   " << folder_path << "\n\n";

    print_row("File Name", "Format", "SR", "Word Length",
              "Max. TPL",
              "Max. SPPM LEFT", "Max. SPPM RIGHT", "Max. SPPM (JOINT)",
              "RMS LEFT", "RMS RIGHT",
              "Max. M LEFT", "Max. M RIGHT", "Max. S LEFT", "Max. S RIGHT",
              "LUFSi", "DR (PMF)", "Bits Used",
              "PLR", "LRA",
              "Max. M", "Max. S",
              "DR LEFT", "Max. TPL LEFT", "Max. TPL RIGHT",
              "DR RIGHT", "Min. PSR");

    std::cout << "\n";

    for (const auto& r : rows) {
        print_row(r.name.c_str(), r.ext.c_str(), r.sr.c_str(), r.wl.c_str(),
                  r.tpl.c_str(),
                  r.sppm_l.c_str(), r.sppm_r.c_str(), r.sppm_j.c_str(),
                  r.rms_l.c_str(), r.rms_r.c_str(),
                  r.mom_l.c_str(), r.mom_r.c_str(), r.st_l.c_str(), r.st_r.c_str(),
                  r.lufs_i.c_str(), r.dr_pmf.c_str(), r.bits_used.c_str(),
                  r.plr.c_str(), r.lra.c_str(),
                  r.mom_j.c_str(), r.st_j.c_str(),
                  r.dr_l.c_str(), r.tpl_l.c_str(), r.tpl_r.c_str(),
                  r.dr_r.c_str(), r.min_psr.c_str());
    }

    std::printf("\nNumber of EP/Album Files: %zu\n", tracks.size());
    std::printf("Official EP/Album DR: %d\n", dr::album_dr(tracks));
}

// ── Detail output (per-track per-channel block view) ───────────────────────
void print_detail(const std::vector<dr::TrackResult>& tracks,
                  const std::string& folder_name) {
    const std::string sep(80, '=');

    auto now = std::chrono::system_clock::now();
    auto t_now = std::chrono::system_clock::to_time_t(now);
    char date_buf[32];
    std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t_now));

    std::cout << PROGRAM_NAME << " " << PROGRAM_VERSION
              << " — Detailed Track Analysis\n";
    std::cout << "log date: " << date_buf << "\n\n";
    std::cout << sep << "\n";
    std::cout << "Analyzed: " << folder_name << "\n";
    std::cout << sep << "\n";

    for (size_t idx = 0; idx < tracks.size(); ++idx) {
        const auto& t = tracks[idx];
        size_t nch = t.ch_metrics.size();
        auto name = strip_ext(t.filename);

        std::cout << "\n" << sep << "\n";
        std::printf("Track %zu/%zu: %s\n", idx + 1, tracks.size(), name.c_str());
        std::cout << sep << "\n\n";

        std::printf("  Format:              %s %u-bit / %u Hz\n",
                    t.codec.c_str(), t.bit_depth, t.sample_rate);
        std::printf("  Layout:              %s\n",
                    chan::layout_detail(t.channels).c_str());
        std::printf("  Duration:            %s\n\n",
                    format_duration(t.duration_secs).c_str());

        // Speaker names come from the shared layout table so the CLI, the JSON
        // and the GUI cannot drift apart. See channels.hpp.
        const unsigned unch = static_cast<unsigned>(nch);
        auto ch_label = [&](size_t c) -> std::string {
            return chan::label_long(unch, c);
        };

        std::printf("  %-22s", "");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12s", ch_label(c).c_str());
        std::printf("%-12s\n", "Joint");
        std::cout << "  " << std::string(22 + 12 * (nch + 1), '-') << "\n";

        // Standards tags mirror MAAT DROffline's convention: the governing
        // standard follows the joint value; per-channel M/S are (non-ITU)
        // informative variants (BS.1770 defines them for the summed program
        // only). Legend printed once in the summary footer.
        std::printf("  %-22s", "Max True Peak");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12.2f", t.ch_metrics[c].true_peak_dbtp);
        std::printf("%.2f dB (ITU)\n", t.max_true_peak_dbtp);

        std::printf("  %-22s", "Max Sample Peak");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12.2f", t.ch_metrics[c].sample_peak_dbfs);
        if (t.is_clipping) std::printf("over\n");
        else std::printf("%.2f dBFS\n", t.peak_dbfs);

        std::printf("  %-22s", "RMS");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12.2f", t.ch_metrics[c].rms_dbfs);
        std::printf("%.2f dB\n", t.rms_dbfs);

        std::printf("  %-22s", "Max Momentary");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12.2f", t.ch_metrics[c].max_momentary_lufs);
        std::printf("%.2f LUFS (ITU)\n", t.max_momentary_lufs);

        std::printf("  %-22s", "Max Short-Term");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12.2f", t.ch_metrics[c].max_short_term_lufs);
        std::printf("%.2f LUFS (ITU)\n", t.max_short_term_lufs);

        std::printf("  %-22s", "Integrated");
        for (size_t c = 0; c < nch; ++c) std::printf("%-12s", "—");
        std::printf("%.2f LUFS (ITU)\n", t.integrated_lufs);

        std::printf("  %-22s", "DR (PMF)");
        for (size_t c = 0; c < nch; ++c) {
            // Brackets mark a channel that is measured but not scored (LFE).
            if (t.ch_metrics[c].scored) {
                std::printf("%-12.2f", t.ch_metrics[c].dr_raw);
            } else {
                char br[16];
                std::snprintf(br, sizeof(br), "[%.2f]", t.ch_metrics[c].dr_raw);
                std::printf("%-12s", br);
            }
        }
        std::printf("DR%d (PMF)\n", t.dr_score);

        std::printf("  %-22s", "Min PSR");
        for (size_t c = 0; c < nch; ++c) std::printf("%-12s", "—");
        std::printf("%.2f dB (AES)\n", t.psr_db);

        std::printf("  %-22s", "PLR");
        for (size_t c = 0; c < nch; ++c) std::printf("%-12s", "—");
        std::printf("%.2f dB (AES)\n", t.plr_db);

        std::printf("  %-22s", "LRA");
        for (size_t c = 0; c < nch; ++c) std::printf("%-12s", "—");
        std::printf("%.2f LU (EBU)\n", t.lra_lu);

        // Multichannel needs its aggregation rule stated next to the number:
        // TT DR defines no procedure beyond stereo, so the reader is entitled
        // to know which channels produced the headline and which did not.
        if (nch > 2) {
            const bool lfe_in = (g_lfe == dr::LfeScoring::Include);
            std::printf("\n  DR (PMF) = mean of the %u scored channel%s%s.\n",
                        chan::scored_count(unch, lfe_in),
                        chan::scored_count(unch, lfe_in) == 1 ? "" : "s",
                        !chan::has_lfe(unch) ? ""
                            : lfe_in ? " — LFE INCLUDED (--dr-lfe include)"
                                     : " — LFE measured, not scored");
            std::printf("  Front L/R DR:        %.2f   (the figure comparable "
                        "with a stereo master)\n", t.dr_front_raw);
            if (t.worst_channel >= 0)
                std::printf("  Lowest scored:       %.2f   (%s)\n",
                            t.ch_metrics[t.worst_channel].dr_raw,
                            chan::label_long(unch, t.worst_channel).c_str());
        }
    }

    std::cout << "\n" << sep << "\nSummary\n" << sep << "\n\n";
    std::cout << "Number of tracks:  " << tracks.size() << "\n";
    std::cout << "Official DR value: DR" << dr::album_dr(tracks) << "\n";

    if (!tracks.empty()) {
        double sum_lufs = 0.0;
        for (const auto& t : tracks) sum_lufs += t.integrated_lufs;
        std::printf("Avg. loudness:     %.1f LUFS\n", sum_lufs / tracks.size());

        const auto& first = tracks[0];
        std::cout << "\nSamplerate:        " << first.sample_rate << " Hz\n";
        std::cout << "Channels:          " << first.channels
                  << " (" << chan::layout_detail(first.channels) << ")\n";
        std::cout << "Bits per sample:   " << first.bit_depth << "\n";
        std::cout << "Codec:             " << first.codec << "\n";
        if (first.audio_stream_count > 1)
            std::cout << "Audio stream:      " << first.stream_index
                      << " of " << first.audio_stream_count
                      << " in container\n";
        if (first.channels > 2)
            std::cout << "Loudness summing:  "
                      << chan::loudness_mode(first.channels) << "\n";

        auto v = dr::classify_dr(static_cast<double>(dr::album_dr(tracks)));
        std::cout << "Verdict:           " << dr::verdict_string(v) << "\n";
    }
    std::cout << sep << "\n";
    std::cout <<
        "Standards\n"
        "  (ITU)  ITU-R BS.1770-4 — K-weighted loudness (LUFS) and 4x-oversampled\n"
        "         true peak. Gating per EBU R 128.\n"
        "  (EBU)  EBU Tech 3342 — Loudness Range (LRA), 3 s short-term distribution,\n"
        "         10th-95th percentile with -20 LU relative gate.\n"
        "  (AES)  AES loudness conventions — PLR (true peak minus integrated LUFS),\n"
        "         Min PSR (window sample peak minus 3 s short-term loudness, minimum\n"
        "         over the track).\n"
        "  (PMF)  TT Dynamic Range, Pleasurize Music Foundation — peak-to-loudest-RMS\n"
        "         metric matching dr.loudness-war.info.\n"
        "  Per-channel Momentary/Short-Term columns are informative (non-ITU):\n"
        "  BS.1770 defines these values for the summed program only.\n"
        << sep << "\n";
    if (!tracks.empty() && tracks[0].channels > 2) {
        std::cout <<
            "Multichannel\n"
            "  TT DR / dr.loudness-war.info define no procedure beyond stereo, so\n"
            "  crete states its rule rather than implying one: DR (PMF) is the mean\n"
            "  of the scored channels, whose two-channel case IS the stereo rule.\n"
            "  The front L/R DR (comparable with a stereo master) and the lowest\n"
            "  scored channel are reported alongside it, never in place of it.\n"
            "  The LFE is measured in full and scored in nothing: a band-limited\n"
            "  effects channel has a nearly flat crest factor, so folding it in\n"
            "  would measure bass management rather than the music.\n"
            "  Loudness follows BS.1770-4 channel weighting: front channels unity,\n"
            "  surrounds +1.5 dB, LFE excluded.\n"
            << sep << "\n";
    }
}

// ════════════════════════════════════════════════════════════════════════════
// JSON output
// ════════════════════════════════════════════════════════════════════════════

#ifdef CRETE_HAS_JSON

void print_json(const std::vector<dr::TrackResult>& tracks,
                const std::string& folder_path) {
    json j;
    j["version"] = std::string(PROGRAM_NAME) + " " + PROGRAM_VERSION;
    j["folder_path"] = folder_path;
    j["album_dr"] = dr::album_dr(tracks);
    j["num_tracks"] = tracks.size();

    // Self-documenting standards map: which spec governs each reported field.
    // Mirrors MAAT DROffline's per-metric tags. Per-channel momentary/short-term
    // are non-ITU informative variants (BS.1770 defines them for the summed
    // program only).
    j["standards"] = {
        {"dr_score",             "TT Dynamic Range (PMF)"},
        {"dr_raw",               "TT Dynamic Range (PMF)"},
        {"peak_dbfs",            "sample peak (dBFS)"},
        {"rms_dbfs",             "RMS (dBFS)"},
        {"max_true_peak_dbtp",   "ITU-R BS.1770-4"},
        {"integrated_lufs",      "ITU-R BS.1770-4 / EBU R 128"},
        {"max_momentary_lufs",   "ITU-R BS.1770-4 (EBU 3341)"},
        {"max_short_term_lufs",  "ITU-R BS.1770-4 (EBU 3341)"},
        {"lra_lu",               "EBU Tech 3342"},
        {"plr_db",               "AES (TP - integrated LUFS)"},
        {"psr_db",               "AES (min window SPPM - short-term LUFS)"},
        {"crest_factor_db",      "sample peak - RMS, per-channel mean"},
        {"channel_metrics.max_momentary_lufs",  "non-ITU (per-channel, informative)"},
        {"channel_metrics.max_short_term_lufs", "non-ITU (per-channel, informative)"},
        {"dr_front_raw",         "TT DR of the front L/R pair (crete, multichannel)"},
        {"worst_channel",        "lowest-DR scored channel index (crete, multichannel)"},
        {"channel_layout",       "WAVE/ITU channel order (crete)"},
        {"loudness_summing",     "ITU-R BS.1770-4 channel weighting"},
    };

    if (!tracks.empty()) {
        const auto& first = tracks[0];
        j["sample_rate"] = first.sample_rate;
        j["channels"] = first.channels;
        j["channel_layout"] = chan::layout_short(first.channels);
        j["scored_channels"] = chan::scored_count(first.channels, g_lfe == dr::LfeScoring::Include);
        j["loudness_summing"] = chan::loudness_mode(first.channels);
        j["bit_depth"] = first.bit_depth;
        j["codec"] = first.codec;
        j["dr_rms_mean"] = dr::rms_mean_name(g_rms_mean);
        j["dr_blocks"]   = dr::block_set_name(g_dr_blocks);
        j["dr_lfe"]      = dr::lfe_scoring_name(g_lfe);
        if (first.audio_stream_count > 1) {
            j["audio_stream_count"] = first.audio_stream_count;
            j["stream_index"]       = first.stream_index;
        }
        if (first.codec == "DSD") {
            j["dsd_mode"]     = dsd::mode_name(g_dsd_opt.mode);
            j["dsd_filter"]   = dsd::filter_name(g_dsd_opt.filter);
            j["dsd_out_rate"] = g_dsd_opt.out_rate;
        }
    }

    json jtracks = json::array();

    for (const auto& t : tracks) {
        json jt;
        jt["filename"] = t.filename;
        jt["codec"] = t.codec;
        jt["sample_rate"] = t.sample_rate;
        jt["bit_depth"] = t.bit_depth;
        jt["channels"] = t.channels;
        jt["duration_secs"] = t.duration_secs;
        if (t.audio_stream_count > 1) {
            jt["audio_stream_count"] = t.audio_stream_count;
            jt["stream_index"]       = t.stream_index;
        }

        jt["dr_score"] = t.dr_score;
        jt["dr_raw"] = t.dr_score_raw;

        jt["peak_dbfs"] = t.peak_dbfs;
        jt["rms_dbfs"] = t.rms_dbfs;
        jt["max_true_peak_dbtp"] = t.max_true_peak_dbtp;
        jt["is_clipping"] = t.is_clipping;

        jt["integrated_lufs"] = t.integrated_lufs;
        jt["max_momentary_lufs"] = t.max_momentary_lufs;
        jt["max_short_term_lufs"] = t.max_short_term_lufs;
        jt["lra_lu"] = t.lra_lu;

        jt["plr_db"] = t.plr_db;
        jt["psr_db"] = t.psr_db;
        jt["crest_factor_db"] = t.crest_factor_db;

        jt["verdict"] = dr::verdict_string(t.verdict);

        // MQA forensic verdict. Emitted for every track so downstream tools can
        // flag lossy-encoded "hi-res" without re-analysing the PCM.
        {
            json jm;
            jm["detected"]        = t.mqa.datasync_found;
            jm["confidence"]      = t.mqa.confidence;
            if (t.mqa.datasync_found) {
                jm["studio"]          = t.mqa.studio;
                jm["original_rate"]   = t.mqa.orig_rate;
                jm["stream_rate"]     = t.mqa.src_rate;
                jm["auth_level"]      = t.mqa.auth_level;
                jm["render_filter"]   = t.mqa.render_filter;
                jm["render_bitdepth"] = t.mqa.render_bitdepth;
                jm["carrier_bit"]     = t.mqa.bit_position;
            } else {
                jm["lsb_entropy"]     = t.mqa.lsb_entropy;
            }
            jt["mqa"] = jm;
        }

        jt["channel_layout"]  = chan::layout_short(t.channels);
        jt["scored_channels"] = chan::scored_count(t.channels, g_lfe == dr::LfeScoring::Include);
        // Multichannel companions to dr_score_raw. On mono/stereo dr_front_raw
        // equals dr_score_raw and worst_channel is just the lower-DR channel,
        // so consumers can read them unconditionally.
        jt["dr_front_raw"]    = t.dr_front_raw;
        jt["worst_channel"]   = t.worst_channel;

        json jch = json::array();
        size_t nch = t.ch_metrics.size();
        for (size_t c = 0; c < nch; ++c) {
            const auto& cm = t.ch_metrics[c];
            json jc;
            // label_long keeps the historical "Mono"/"Left"/"Right" spellings
            // for 1- and 2-channel files, so the existing JSON schema is
            // unchanged for every consumer that predates multichannel.
            jc["label"]  = chan::label_long(static_cast<unsigned>(nch), c);
            jc["short_label"] = chan::label(static_cast<unsigned>(nch), c);
            jc["is_lfe"] = cm.is_lfe;
            jc["scored"] = cm.scored;

            jc["sample_peak_dbfs"] = cm.sample_peak_dbfs;
            jc["true_peak_dbtp"] = cm.true_peak_dbtp;
            jc["rms_dbfs"] = cm.rms_dbfs;
            jc["dr_score"] = cm.dr_score;
            jc["dr_raw"] = cm.dr_raw;
            jc["max_momentary_lufs"] = cm.max_momentary_lufs;
            jc["max_short_term_lufs"] = cm.max_short_term_lufs;
            jch.push_back(std::move(jc));
        }
        jt["channel_metrics"] = std::move(jch);

        jtracks.push_back(std::move(jt));
    }

    j["tracks"] = std::move(jtracks);

    std::cout << j.dump(2) << "\n";
}

#endif // CRETE_HAS_JSON

// ── Usage ──────────────────────────────────────────────────────────────────
void print_usage(const char* argv0) {
    std::cerr << PROGRAM_NAME << " — Zero-dependency Dynamic Range Meter\n\n"
              << "Usage: " << argv0 << " [options] <file_or_folder...>\n\n"
              << "Options:\n"
              << "  -f, --format <std|foobar|ext|detail"
#ifdef CRETE_HAS_JSON
              << "|json"
#endif
              << ">\n"
              << "      std    — dr.loudness-war.info compatible (default)\n"
              << "      foobar — foobar2000 DR Meter style\n"
              << "      ext    — MAAT DROffline–style pipe-delimited table\n"
              << "      detail — per-track per-channel block view\n"
#ifdef CRETE_HAS_JSON
              << "      json   — machine-readable JSON (all metrics)\n"
#endif
              << "  -o, --output <file>            Write output to file\n"
              << "  --dr-mean <quadratic|arithmetic>  Top-20% RMS averaging (default: quadratic)\n"
              << "  --dr-lfe <exclude|include>     Count the LFE toward DR (default: exclude).\n"
              << "                                 Affects TT DR only; BS.1770 loudness and\n"
              << "                                 Min. PSR are unchanged. No-op without an LFE.\n"
              << "      quadratic  — sqrt(mean(rms^2)), averages POWER. The PMF procedure,\n"
              << "                   and what MAAT and foobar2000 DR Meter both do.\n"
              << "      arithmetic — sum(rms)/n, averages AMPLITUDE. What crete shipped up\n"
              << "                   to 0.12.1; biases DR high. For A/B only.\n"
              << "  --dr-blocks <reference|legacy>    3 s block set for DR (default: reference).\n"
              << "      reference  — PMF reference implementation: keeps the trailing partial\n"
              << "                   block, no block gate. Default since 0.14.0.\n"
              << "      legacy     — what crete shipped up to 0.13.1: trailing block discarded,\n"
              << "                   blocks gated on joint RMS. Biases DR low. For A/B only.\n"
              << "  --dsd-mode <direct|multistage> DSD decimation chain (default: multistage)\n"
              << "      multistage — brickwall at Nyquist (measurement-grade). Works for\n"
              << "                   DSD64/128/256/512 at every output rate.\n"
              << "      direct     — single wide FIR, gentle ~30 kHz corner. DSD64 ->\n"
              << "                   44100 only (libdsddpcm restriction).\n"
              << "  --dsd-filter <default|firls|kaiser>  Final-stage filter (default: default)\n"
              << "                   Honoured only at --dsd-out-rate 44100; ignored otherwise.\n"
              << "  --dsd-out-rate <44100|88200|176400|352800>  DSD output rate (default: 44100)\n"
              << "  --stream <N>                   Audio stream index to decode in a multi-\n"
              << "                                 stream container (.mkv/.mp4 disc rips).\n"
              << "                                 Index as `ffprobe -show_streams` prints it.\n"
              << "                                 Default: FFmpeg's own choice, which ranks by\n"
              << "                                 bitrate/channels and is reported when the\n"
              << "                                 file holds more than one audio stream.\n"
              << "  -j, --jobs <N>                 Parallel file workers (default: all cores;\n"
              << "                                 1 = sequential). Does not change any numbers.\n"
              << "  -q, --quiet                    Suppress progress output\n"
              << "  -v, --version                  Show version\n"
              << "  -h, --help                     Show this help\n\n"
              << "Supported formats: WAV, AIFF, FLAC, DSF, DFF (DSD)\n"
              << "Cue sheets: any .cue file in the input folder is parsed\n"
              << "  automatically; each TRACK becomes its own analysed entry,\n"
              << "  sliced from the referenced monolithic audio file.\n"
              << "Algorithm: TT Dynamic Range (3s blocks, top-20% percentile)\n";
}

void print_version() {
    std::cout << PROGRAM_NAME << " " << PROGRAM_VERSION
              << " — " << PROGRAM_DESC
#ifdef CRETE_HAS_JSON
              << " [+json]"
#endif
#ifdef CRETE_HAS_FFMPEG
              << " [+ffmpeg-compact " << av_version_info() << "]"
#endif
              << "\n"
              << "Built: " << __DATE__ << " " << __TIME__ << "\n"
              << "DSD engine: libdsddpcm " << dsddpcm_version_string()
              << " (BSD-2-Clause, vendored)\n"
              << "DSD chains to " << g_dsd_opt.out_rate << " Hz (filter: "
              << dsd::filter_name(g_dsd_opt.filter) << "):\n";
    const int rates[4]     = { 2822400, 5644800, 11289600, 22579200 };
    const char* labels[4]  = { "DSD64 ", "DSD128", "DSD256", "DSD512" };
    for (int i = 0; i < 4; ++i) {
        dsd::Options direct = g_dsd_opt; direct.mode = dsd::Mode::Direct;
        dsd::Options multi  = g_dsd_opt; multi.mode  = dsd::Mode::Multistage;
        std::cout << "  " << labels[i] << " direct     "
                  << dsd::describe_chain(direct, rates[i]) << "\n"
                  << "  " << labels[i] << " multistage "
                  << dsd::describe_chain(multi, rates[i]) << "\n";
    }
}

// ── Main ───────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    OutputFormat format = OutputFormat::Standard;
    std::string output_file;
    bool quiet = false;
    int jobs = 0;   // 0 = auto (hardware_concurrency)
    std::vector<std::string> paths;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            print_version();
            return 0;
        } else if (arg == "-q" || arg == "--quiet") {
            quiet = true;
        } else if ((arg == "-j" || arg == "--jobs") && i+1 < argc) {
            jobs = std::atoi(argv[++i]);
            if (jobs < 1) {
                std::cerr << "Error: --jobs must be >= 1\n";
                return 1;
            }
        } else if ((arg == "-f" || arg == "--format") && i+1 < argc) {
            std::string fmt = argv[++i];
            if (fmt == "std")              format = OutputFormat::Standard;
            else if (fmt == "foobar")      format = OutputFormat::Foobar;
            else if (fmt == "ext" ||
                     fmt == "maat")        format = OutputFormat::Extended;
            else if (fmt == "detail")      format = OutputFormat::Detail;
            else if (fmt == "json") {
#ifdef CRETE_HAS_JSON
                format = OutputFormat::Json;
#else
                std::cerr << "Error: JSON output not available.\n"
                          << "Rebuild with: make cli-json\n";
                return 1;
#endif
            }
            else {
                std::cerr << "Unknown format: " << fmt << "\n";
                return 1;
            }
        } else if (arg == "--dr-lfe" && i+1 < argc) {
            try {
                g_lfe = dr::lfe_scoring_from_string(argv[++i]);
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n";
                return 1;
            }
        } else if (arg == "--dr-blocks" && i+1 < argc) {
            try {
                g_dr_blocks = dr::block_set_from_string(argv[++i]);
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n";
                return 1;
            }
        } else if (arg == "--dr-mean" && i+1 < argc) {
            try {
                g_rms_mean = dr::rms_mean_from_string(argv[++i]);
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n";
                return 1;
            }
        } else if (arg == "--dsd-mode" && i+1 < argc) {
            try {
                g_dsd_opt.mode = dsd::mode_from_string(argv[++i]);
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n";
                return 1;
            }
        } else if (arg == "--dsd-filter" && i+1 < argc) {
            try {
                g_dsd_opt.filter = dsd::filter_from_string(argv[++i]);
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n";
                return 1;
            }
        } else if (arg == "--dsd-out-rate" && i+1 < argc) {
            int r = std::atoi(argv[++i]);
            if (!dsd::is_valid_out_rate(r)) {
                std::cerr << "Error: --dsd-out-rate must be 44100, 88200, 176400, or 352800\n";
                return 1;
            }
            g_dsd_opt.out_rate = r;
        } else if (arg == "--stream" && i+1 < argc) {
            char* end = nullptr;
            long v = std::strtol(argv[++i], &end, 10);
            if (!end || *end || v < 0 || v > 1000000) {
                std::cerr << "Error: --stream must be a non-negative stream index"
                             " (as shown by `ffprobe -show_streams`)\n";
                return 1;
            }
            g_stream = static_cast<int>(v);
        } else if ((arg == "-o" || arg == "--output") && i+1 < argc) {
            output_file = argv[++i];
        } else {
            paths.push_back(arg);
        }
    }

    if (paths.empty()) {
        std::cerr << "Error: no files or folders specified\n";
        return 1;
    }

    FILE* out_fp = nullptr;
    if (!output_file.empty()) {
        out_fp = std::freopen(output_file.c_str(), "w", stdout);
        if (!out_fp) {
            std::cerr << "Error: cannot open output file: " << output_file << "\n";
            return 1;
        }
    }

    // Did ANY path yield a measurement? crete exits non-zero when nothing

    // could be analysed: a meter that measured nothing has not succeeded,

    // and with -f json it emits no JSON at all, so a caller that trusts the

    // exit code parses an empty string. That is exactly how a stale FFmpeg

    // build (no truehd/dca decoder) surfaced in CI as an unreadable

    // JSONDecodeError instead of crete's own "Error:" line.

    // Per-file failures inside an album are NOT this case -- a partial

    // success still exits 0, as before.

    bool any_results = false;


    for (const auto& path : paths) {
        auto groups = collect_groups(path);
        if (groups.empty()) {
            std::cerr << "Warning: no supported audio files in " << path << "\n";
            continue;
        }

        size_t total_tracks = 0;
        for (const auto& g : groups)
            total_tracks += g.sheet.tracks.empty() ? 1 : g.sheet.tracks.size();

        // Decode + analyse groups in parallel (a group = one standalone file,
        // or one cue-sliced monolith yielding N contiguous tracks). Mirrors
        // the GUI worker pool: decode_file/analyze_track hold no shared
        // mutable state, DSD decodes still fan out per-channel inside each
        // group, and results land in per-group slots so the flattened output
        // order matches the sequential walk exactly. Measurement is
        // unchanged — parallelism only reorders the wall-clock schedule.
        const size_t ngroups = groups.size();
        std::vector<std::vector<dr::TrackResult>> slots(ngroups);
        std::vector<std::string> errlog(ngroups);
        std::atomic<size_t> next{0}, done{0};
        std::mutex io_mtx;

        auto progress = [&](const std::string& name) {
            size_t d = done.fetch_add(1) + 1;
            if (!quiet) {
                std::lock_guard<std::mutex> lk(io_mtx);
                std::cerr << "\rAnalyzing [" << d << "/" << total_tracks
                          << "] " << name << "...          " << std::flush;
            }
        };

        auto process_group = [&](size_t gi) {
            const auto& g = groups[gi];
            std::string basename = fs::path(g.audio_path).filename().string();
            std::string ext_str  = fs::path(g.audio_path).extension().string();

            try {
                auto ad = audio::decode_file(g.audio_path, g_dsd_opt, g_stream);

                // Several audio streams and no explicit choice: say which one
                // was measured. A Blu-ray rip's LPCM 5.1, DTS-HD MA 5.1 and
                // TrueHD 7.1 tracks are different mixes with different DR, so
                // silently metering whichever FFmpeg ranks highest would make
                // the number unreproducible without this line.
                if (g_stream < 0 && ad.audio_stream_count > 1) {
                    errlog[gi] += "Warning: " + basename + " has "
                        + std::to_string(ad.audio_stream_count)
                        + " audio streams; measured stream "
                        + std::to_string(ad.stream_index)
                        + " (" + ad.codec + ", "
                        + std::to_string(ad.num_channels()) + " ch, "
                        + std::to_string(ad.sample_rate)
                        + " Hz). Use --stream N to choose.\n";
                }

                // File-level MQA check, stamped onto every track from this decode.
                const mqa::Info minfo =
                    mqa::analyze_pcm(ad.channels, ad.sample_rate, ad.bit_depth);

                if (g.sheet.tracks.empty()) {
                    progress(basename);
                    slots[gi].push_back(dr::analyze_track(
                        ad.channels, ad.sample_rate, ad.bit_depth,
                        ad.codec, basename, g_rms_mean, g_lfe, g_dr_blocks));
                    slots[gi].back().mqa = minfo;
                    slots[gi].back().stream_index       = ad.stream_index;
                    slots[gi].back().audio_stream_count = ad.audio_stream_count;
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

                        // NN - Title.ext, mirroring sacd_extract / common
                        // splitter conventions so cue-derived tracks pair
                        // cleanly with per-track siblings under helpers.py's
                        // _base_norm normalization.
                        char track_name[512];
                        std::snprintf(track_name, sizeof(track_name),
                                      "%02d - %s%s",
                                      trk.number, trk.title.c_str(),
                                      ext_str.c_str());

                        progress(track_name);

                        if (start >= end) {
                            errlog[gi] += std::string("Warning: empty cue range for ")
                                          + track_name + "\n";
                            continue;
                        }

                        std::vector<std::vector<double>> slice(ad.channels.size());
                        for (size_t c = 0; c < ad.channels.size(); ++c) {
                            slice[c].assign(
                                ad.channels[c].begin() + static_cast<std::ptrdiff_t>(start),
                                ad.channels[c].begin() + static_cast<std::ptrdiff_t>(end));
                        }

                        slots[gi].push_back(dr::analyze_track(
                            slice, ad.sample_rate, ad.bit_depth,
                            ad.codec, track_name, g_rms_mean, g_lfe, g_dr_blocks));
                        slots[gi].back().mqa = minfo;
                    slots[gi].back().stream_index       = ad.stream_index;
                    slots[gi].back().audio_stream_count = ad.audio_stream_count;
                    }
                }
            } catch (const std::exception& e) {
                errlog[gi] += "Error: " + basename + ": " + e.what() + "\n";
            }
        };

        unsigned hw = std::thread::hardware_concurrency();
        size_t nthreads = jobs > 0 ? (size_t)jobs : (hw ? (size_t)hw : 4);
        nthreads = std::min(nthreads, ngroups);

        if (nthreads <= 1) {
            for (size_t gi = 0; gi < ngroups; ++gi) process_group(gi);
        } else {
            auto worker = [&]() {
                for (;;) {
                    size_t gi = next.fetch_add(1);
                    if (gi >= ngroups) break;
                    process_group(gi);
                }
            };
            std::vector<std::thread> pool;
            pool.reserve(nthreads);
            for (size_t k = 0; k < nthreads; ++k) pool.emplace_back(worker);
            for (auto& th : pool) th.join();
        }

        if (!quiet) std::cerr << "\r" << std::string(80, ' ') << "\r";

        // Flatten per-group slots in original group order; errors/warnings
        // are replayed afterwards so they never interleave mid-progress.
        std::vector<dr::TrackResult> results;
        results.reserve(total_tracks);
        for (size_t gi = 0; gi < ngroups; ++gi)
            for (auto& t : slots[gi]) results.push_back(std::move(t));
        for (size_t gi = 0; gi < ngroups; ++gi)
            if (!errlog[gi].empty()) std::cerr << errlog[gi];

        if (results.empty()) continue;
        any_results = true;

        std::string display_name;
        if (fs::is_directory(path))
            display_name = fs::absolute(path).string();
        else
            display_name = fs::path(path).parent_path().string();

        std::string folder_name;
        if (fs::is_directory(path)) {
            auto abs = fs::absolute(path).lexically_normal();
            auto dir = abs.filename().string();
            auto parent = abs.parent_path().filename().string();
            if (!parent.empty() && parent != "/" && parent != ".")
                folder_name = parent + " / " + dir;
            else
                folder_name = dir;
        } else {
            folder_name = fs::path(path).filename().string();
        }

        switch (format) {
            case OutputFormat::Standard:
                print_standard(results, display_name);
                break;
            case OutputFormat::Foobar:
                print_foobar(results, folder_name);
                break;
            case OutputFormat::Extended:
                print_extended(results, display_name);
                break;
            case OutputFormat::Detail:
                print_detail(results, folder_name);
                break;
            case OutputFormat::Json:
#ifdef CRETE_HAS_JSON
                print_json(results, display_name);
#endif
                break;
        }

        // MQA is reported in-band for JSON (per track); for every human-facing
        // format we append an unmissable warning block.
        if (format != OutputFormat::Json)
            print_mqa_summary(results);
    }

    if (out_fp) std::fclose(out_fp);
    if (!any_results) {
        std::cerr << "Error: no audio could be analysed"
                     " (no supported files, or every file failed to decode)\n";
        return 1;
    }
    return 0;
}
