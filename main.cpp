// ============================================================================
// crête — Zero-dependency Dynamic Range Meter
//
// Measures TT Dynamic Range (Pleasurize Music Foundation algorithm),
// EBU R128 integrated loudness, and crest factor for audio files.
//
// Supported formats: WAV, AIFF, FLAC, DSF, DFF (DSD)
// Output compatible with dr.loudness-war.info submission format.
//
// Usage: crete [options] <path...>
//
// Build: make  (or: g++ -std=c++17 -O2 -o crete main.cpp)
// ============================================================================

#include "analysis.hpp"
#include "audio.hpp"

#ifdef CRETE_HAS_JSON
#include <nlohmann/json.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// ── Version info ───────────────────────────────────────────────────────────
#ifndef CRETE_VERSION
#define CRETE_VERSION "0.1.0"
#endif

static constexpr const char* PROGRAM_NAME    = "crête";
static constexpr const char* PROGRAM_BIN     = "crete";
static constexpr const char* PROGRAM_VERSION = CRETE_VERSION;
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

// Format sample rate: 44100 → "44.1k", 96000 → "96k", 352800 → "352.8k"
std::string format_sr(uint32_t sr) {
    double k = sr / 1000.0;
    if (k == std::floor(k))
        return std::to_string(static_cast<int>(k)) + "k";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1fk", k);
    return buf;
}

// Format dB with explicit +/- sign: +0.39, -1.76
std::string format_signed(double db) {
    char buf[16];
    if (db >= 0.0)
        std::snprintf(buf, sizeof(buf), "+%.2f", db);
    else
        std::snprintf(buf, sizeof(buf), "%.2f", db);
    return buf;
}

// Format dB with natural sign, 2 decimal places
std::string format_db(double db) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f", db);
    return buf;
}

// Strip file extension
std::string strip_ext(const std::string& filename) {
    auto dot = filename.rfind('.');
    if (dot != std::string::npos) return filename.substr(0, dot);
    return filename;
}

// Get file extension (including dot)
std::string get_ext(const std::string& filename) {
    auto dot = filename.rfind('.');
    if (dot != std::string::npos) return filename.substr(dot);
    return "";
}

// ── Collect audio files from a path ────────────────────────────────────────
std::vector<std::string> collect_files(const std::string& path) {
    std::vector<std::string> files;

    if (fs::is_regular_file(path)) {
        if (audio::is_supported_format(path))
            files.push_back(path);
    } else if (fs::is_directory(path)) {
        for (const auto& entry : fs::directory_iterator(path)) {
            if (entry.is_regular_file() && audio::is_supported_format(entry.path().string()))
                files.push_back(entry.path().string());
        }
        std::sort(files.begin(), files.end());
    }

    return files;
}

// ── Analyze a single file ──────────────────────────────────────────────────
dr::TrackResult analyze_file(const std::string& path) {
    auto ad = audio::decode_file(path);
    auto filename = fs::path(path).filename().string();
    return dr::analyze_track(ad.channels, ad.sample_rate, ad.bit_depth,
                              ad.codec, filename);
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
//
// Columns (matching MAAT DROffline MkII log format):
//   File Name | Format | SR | Word Length | Max. TPL |
//   Max. SPPM LEFT | Max. SPPM RIGHT | Max. SPPM (JOINT) |
//   RMS LEFT | RMS RIGHT | Max. M LEFT | Max. M RIGHT |
//   Max. S LEFT | Max. S RIGHT | LUFSi | DR (PMF) | Bits Used |
//   PLR | LRA | Max. M | Max. S | DR LEFT |
//   Max. TPL LEFT | Max. TPL RIGHT | DR RIGHT | Min. PSR |

void print_extended(const std::vector<dr::TrackResult>& tracks,
                    const std::string& folder_path) {
    if (tracks.empty()) return;

    // ── Pre-format all values & compute dynamic column widths ────────

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

    size_t fname_w = 9;  // min = strlen("File Name")
    size_t sr_w    = 2;  // min = strlen("SR")

    for (const auto& t : tracks) {
        Row r;
        r.name = strip_ext(t.filename);
        r.ext  = get_ext(t.filename);
        r.sr   = format_sr(t.sample_rate);
        r.wl   = std::to_string(t.bit_depth);

        size_t nch = t.ch_metrics.size();

        // Joint true peak (explicit sign)
        r.tpl = format_signed(t.true_peak_dbfs);

        // Per-channel sample peaks
        double sp_l = (nch > 0) ? t.ch_metrics[0].sample_peak_dbfs : t.peak_dbfs;
        double sp_r = (nch > 1) ? t.ch_metrics[1].sample_peak_dbfs : sp_l;
        r.sppm_l = format_db(sp_l);
        r.sppm_r = format_db(sp_r);
        r.sppm_j = format_db(std::max(sp_l, sp_r));

        // Per-channel RMS
        r.rms_l = format_db((nch > 0) ? t.ch_metrics[0].rms_dbfs : t.rms_dbfs);
        r.rms_r = format_db((nch > 1) ? t.ch_metrics[1].rms_dbfs
                                       : ((nch > 0) ? t.ch_metrics[0].rms_dbfs : t.rms_dbfs));

        // Per-channel max momentary
        r.mom_l = format_db((nch > 0) ? t.ch_metrics[0].max_momentary_lufs : t.max_momentary_lufs);
        r.mom_r = format_db((nch > 1) ? t.ch_metrics[1].max_momentary_lufs
                                       : ((nch > 0) ? t.ch_metrics[0].max_momentary_lufs : t.max_momentary_lufs));

        // Per-channel max short-term
        r.st_l = format_db((nch > 0) ? t.ch_metrics[0].max_short_term_lufs : t.max_short_term_lufs);
        r.st_r = format_db((nch > 1) ? t.ch_metrics[1].max_short_term_lufs
                                      : ((nch > 0) ? t.ch_metrics[0].max_short_term_lufs : t.max_short_term_lufs));

        // Joint metrics
        r.lufs_i    = format_db(t.integrated_lufs);
        r.dr_pmf    = std::to_string(t.dr_score);
        r.bits_used = std::to_string(t.bit_depth);
        r.plr       = format_db(t.plr_db);
        r.lra       = format_db(t.lra_lu);
        r.mom_j     = format_db(t.max_momentary_lufs);
        r.st_j      = format_db(t.max_short_term_lufs);

        // Per-channel DR (raw, 2 decimal places)
        r.dr_l = format_db((nch > 0) ? t.ch_metrics[0].dr_raw : t.dr_score_raw);
        r.dr_r = format_db((nch > 1) ? t.ch_metrics[1].dr_raw
                                      : ((nch > 0) ? t.ch_metrics[0].dr_raw : t.dr_score_raw));

        // Per-channel true peak (explicit sign)
        r.tpl_l = format_signed((nch > 0) ? t.ch_metrics[0].true_peak_dbfs : t.true_peak_dbfs);
        r.tpl_r = format_signed((nch > 1) ? t.ch_metrics[1].true_peak_dbfs
                                           : ((nch > 0) ? t.ch_metrics[0].true_peak_dbfs : t.true_peak_dbfs));

        r.min_psr = format_db(t.psr_db);

        fname_w = std::max(fname_w, r.name.length());
        sr_w    = std::max(sr_w,    r.sr.length());

        rows.push_back(std::move(r));
    }

    // ── Column alignment widths ──────────────────────────────────────

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

    // ── Output ───────────────────────────────────────────────────────
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
        std::printf("  Duration:            %s\n\n",
                    format_duration(t.duration_secs).c_str());

        auto ch_label = [&](size_t c) -> std::string {
            if (nch == 1) return "Mono";
            if (nch == 2) return (c == 0) ? "Left" : "Right";
            return "Ch" + std::to_string(c + 1);
        };

        std::printf("  %-22s", "");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12s", ch_label(c).c_str());
        std::printf("%-12s\n", "Joint");
        std::cout << "  " << std::string(22 + 12 * (nch + 1), '-') << "\n";

        std::printf("  %-22s", "Max True Peak");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12.2f", t.ch_metrics[c].true_peak_dbfs);
        std::printf("%.2f dB\n", t.true_peak_dbfs);

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
        std::printf("%.2f LUFS\n", t.max_momentary_lufs);

        std::printf("  %-22s", "Max Short-Term");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12.2f", t.ch_metrics[c].max_short_term_lufs);
        std::printf("%.2f LUFS\n", t.max_short_term_lufs);

        std::printf("  %-22s", "Integrated");
        for (size_t c = 0; c < nch; ++c) std::printf("%-12s", "—");
        std::printf("%.2f LUFS\n", t.integrated_lufs);

        std::printf("  %-22s", "DR (PMF)");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12.2f", t.ch_metrics[c].dr_raw);
        std::printf("DR%d\n", t.dr_score);

        std::printf("  %-22s", "Min PSR");
        for (size_t c = 0; c < nch; ++c) std::printf("%-12s", "—");
        std::printf("%.2f dB\n", t.psr_db);

        std::printf("  %-22s", "PLR");
        for (size_t c = 0; c < nch; ++c) std::printf("%-12s", "—");
        std::printf("%.2f dB\n", t.plr_db);

        std::printf("  %-22s", "LRA");
        for (size_t c = 0; c < nch; ++c) std::printf("%-12s", "—");
        std::printf("%.2f LU\n", t.lra_lu);
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
        std::cout << "Channels:          " << first.channels << "\n";
        std::cout << "Bits per sample:   " << first.bit_depth << "\n";
        std::cout << "Codec:             " << first.codec << "\n";

        auto v = dr::classify_dr(static_cast<double>(dr::album_dr(tracks)));
        std::cout << "Verdict:           " << dr::verdict_string(v) << "\n";
    }
    std::cout << sep << "\n";
}

// ════════════════════════════════════════════════════════════════════════════
// JSON output (machine-readable, for testing and integration)
//
// Conditionally compiled: only available when built with -DCRETE_HAS_JSON.
// Build:  make cli-json    (auto-fetches nlohmann/json header)
// ════════════════════════════════════════════════════════════════════════════

#ifdef CRETE_HAS_JSON

void print_json(const std::vector<dr::TrackResult>& tracks,
                const std::string& folder_path) {
    json j;
    j["version"] = std::string(PROGRAM_NAME) + " " + PROGRAM_VERSION;
    j["folder_path"] = folder_path;
    j["album_dr"] = dr::album_dr(tracks);
    j["num_tracks"] = tracks.size();

    if (!tracks.empty()) {
        const auto& first = tracks[0];
        j["sample_rate"] = first.sample_rate;
        j["channels"] = first.channels;
        j["bit_depth"] = first.bit_depth;
        j["codec"] = first.codec;
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

        // TT DR
        jt["dr_score"] = t.dr_score;
        jt["dr_raw"] = t.dr_score_raw;

        // Peak / RMS (joint)
        jt["peak_dbfs"] = t.peak_dbfs;
        jt["rms_dbfs"] = t.rms_dbfs;
        jt["true_peak_dbfs"] = t.true_peak_dbfs;
        jt["is_clipping"] = t.is_clipping;

        // EBU R128
        jt["integrated_lufs"] = t.integrated_lufs;
        jt["max_momentary_lufs"] = t.max_momentary_lufs;
        jt["max_short_term_lufs"] = t.max_short_term_lufs;
        jt["lra_lu"] = t.lra_lu;

        // Derived metrics
        jt["plr_db"] = t.plr_db;
        jt["psr_db"] = t.psr_db;
        jt["crest_factor_db"] = t.crest_factor_db;

        // Verdict
        jt["verdict"] = dr::verdict_string(t.verdict);

        // Per-channel detail
        json jch = json::array();
        size_t nch = t.ch_metrics.size();
        for (size_t c = 0; c < nch; ++c) {
            const auto& cm = t.ch_metrics[c];
            json jc;
            if (nch == 1)      jc["label"] = "Mono";
            else if (nch == 2) jc["label"] = (c == 0) ? "Left" : "Right";
            else               jc["label"] = "Ch" + std::to_string(c + 1);

            jc["sample_peak_dbfs"] = cm.sample_peak_dbfs;
            jc["true_peak_dbfs"] = cm.true_peak_dbfs;
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
              << "  -q, --quiet                    Suppress progress output\n"
              << "  -v, --version                  Show version\n"
              << "  -h, --help                     Show this help\n\n"
              << "Supported formats: WAV, AIFF, FLAC, DSF, DFF (DSD)\n"
              << "Algorithm: TT Dynamic Range (3s blocks, top-20% percentile)\n";
}

void print_version() {
    std::cout << PROGRAM_NAME << " " << PROGRAM_VERSION
              << " — " << PROGRAM_DESC
#ifdef CRETE_HAS_JSON
              << " [+json]"
#endif
              << "\n"
              << "Built: " << __DATE__ << " " << __TIME__ << "\n";
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

    for (const auto& path : paths) {
        auto files = collect_files(path);
        if (files.empty()) {
            std::cerr << "Warning: no supported audio files in " << path << "\n";
            continue;
        }

        std::vector<dr::TrackResult> results;
        results.reserve(files.size());

        for (size_t i = 0; i < files.size(); ++i) {
            auto fname = fs::path(files[i]).filename().string();
            if (!quiet)
                std::cerr << "\rAnalyzing [" << (i+1) << "/" << files.size()
                          << "] " << fname << "...          " << std::flush;
            try {
                results.push_back(analyze_file(files[i]));
            } catch (const std::exception& e) {
                std::cerr << "\nError: " << fname << ": " << e.what() << "\n";
            }
        }

        if (!quiet) std::cerr << "\r" << std::string(80, ' ') << "\r";

        if (results.empty()) continue;

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
    }

    if (out_fp) std::fclose(out_fp);
    return 0;
}
