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

// ── Output format modes ────────────────────────────────────────────────────
enum class OutputFormat {
    Standard,   // dr.loudness-war.info compatible
    Foobar,     // foobar2000 DR Meter style
    Extended,   // all metrics
    Detail,     // per-track per-channel (MAAT DROffline style)
};

// ── Duration formatting ────────────────────────────────────────────────────
std::string format_duration(double secs) {
    int m = static_cast<int>(secs) / 60;
    int s = static_cast<int>(secs) % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

// ── Peak display ───────────────────────────────────────────────────────────
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

        auto name = t.filename;
        auto dot = name.rfind('.');
        if (dot != std::string::npos) name = name.substr(0, dot);

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

// ── Extended output with all metrics ───────────────────────────────────────
void print_extended(const std::vector<dr::TrackResult>& tracks,
                    const std::string& folder_name) {
    const std::string sep(120, '-');
    const std::string sep2(120, '=');

    auto now = std::chrono::system_clock::now();
    auto t_now = std::chrono::system_clock::to_time_t(now);
    char date_buf[32];
    std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t_now));

    std::cout << PROGRAM_NAME << " " << PROGRAM_VERSION
              << " — Extended Dynamic Range Analysis\n";
    std::cout << "log date: " << date_buf << "\n\n";
    std::cout << sep << "\n";
    std::cout << "Analyzed: " << folder_name << "\n";
    std::cout << sep << "\n\n";

    std::printf("%-6s %-10s %-10s %-10s %-10s %-8s %-8s  %s\n",
                "DR", "Peak", "RMS", "LUFS", "PLR", "Crest", "Dur", "Filename");
    std::cout << sep << "\n";

    double sum_lufs = 0.0;

    for (const auto& t : tracks) {
        char dr_str[8];
        std::snprintf(dr_str, sizeof(dr_str), "DR%-3d", t.dr_score);

        char peak_str[16];
        if (t.is_clipping)
            std::snprintf(peak_str, sizeof(peak_str), "over");
        else
            std::snprintf(peak_str, sizeof(peak_str), "%.2f dB", t.peak_dbfs);

        char rms_str[16], lufs_str[16], plr_str[16], crest_str[16];
        std::snprintf(rms_str,   sizeof(rms_str),   "%.2f dB", t.rms_dbfs);
        std::snprintf(lufs_str,  sizeof(lufs_str),  "%.1f",    t.integrated_lufs);
        std::snprintf(plr_str,   sizeof(plr_str),   "%.1f dB", t.plr_db);
        std::snprintf(crest_str, sizeof(crest_str),  "%.1f dB", t.crest_factor_db);

        std::string dur = format_duration(t.duration_secs);

        auto name = t.filename;
        auto dot = name.rfind('.');
        if (dot != std::string::npos) name = name.substr(0, dot);

        std::printf("%-6s %-10s %-10s %-10s %-10s %-8s %-8s  %s\n",
                    dr_str, peak_str, rms_str, lufs_str, plr_str,
                    crest_str, dur.c_str(), name.c_str());

        sum_lufs += t.integrated_lufs;
    }

    std::cout << sep << "\n\n";
    std::cout << "Number of tracks:  " << tracks.size() << "\n";
    std::cout << "Official DR value: DR" << dr::album_dr(tracks) << "\n";

    if (!tracks.empty()) {
        double avg_lufs = sum_lufs / tracks.size();
        std::printf("Avg. loudness:     %.1f LUFS\n", avg_lufs);

        const auto& first = tracks[0];
        std::cout << "\nSamplerate:        " << first.sample_rate << " Hz\n";
        std::cout << "Channels:          " << first.channels << "\n";
        std::cout << "Bits per sample:   " << first.bit_depth << "\n";
        std::cout << "Codec:             " << first.codec << "\n";

        auto album_verdict = dr::classify_dr(
            static_cast<double>(dr::album_dr(tracks)));
        std::cout << "Verdict:           " << dr::verdict_string(album_verdict) << "\n";
    }

    std::cout << sep2 << "\n";
}

// ── Detail output (per-track per-channel, MAAT DROffline style) ────────────
void print_detail(const std::vector<dr::TrackResult>& tracks,
                  const std::string& folder_name) {
    const std::string sep(80, '=');
    const std::string sep2(80, '-');

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

        auto name = t.filename;
        auto dot = name.rfind('.');
        if (dot != std::string::npos) name = name.substr(0, dot);

        std::cout << "\n" << sep << "\n";
        std::printf("Track %zu/%zu: %s\n", idx + 1, tracks.size(), name.c_str());
        std::cout << sep << "\n\n";

        std::printf("  Format:              %s %u-bit / %u Hz\n",
                    t.codec.c_str(), t.bit_depth, t.sample_rate);
        std::printf("  Duration:            %s\n\n", format_duration(t.duration_secs).c_str());

        // Channel labels
        auto ch_label = [&](size_t c) -> std::string {
            if (nch == 1) return "Mono";
            if (nch == 2) return (c == 0) ? "Left" : "Right";
            return "Ch" + std::to_string(c + 1);
        };

        // Header
        std::printf("  %-22s", "");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12s", ch_label(c).c_str());
        std::printf("%-12s\n", "Joint");
        std::cout << "  " << std::string(22 + 12 * (nch + 1), '-') << "\n";

        // True Peak
        std::printf("  %-22s", "Max True Peak");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12.2f", t.ch_metrics[c].true_peak_dbfs);
        std::printf("%.2f dB\n", t.true_peak_dbfs);

        // Sample Peak
        std::printf("  %-22s", "Max Sample Peak");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12.2f", t.ch_metrics[c].sample_peak_dbfs);
        if (t.is_clipping)
            std::printf("over\n");
        else
            std::printf("%.2f dBFS\n", t.peak_dbfs);

        // RMS
        std::printf("  %-22s", "RMS");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12.2f", t.ch_metrics[c].rms_dbfs);
        std::printf("%.2f dB\n", t.rms_dbfs);

        // Max Momentary
        std::printf("  %-22s", "Max Momentary");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12.2f", t.ch_metrics[c].max_momentary_lufs);
        std::printf("%.2f LUFS\n", t.max_momentary_lufs);

        // Max Short-Term
        std::printf("  %-22s", "Max Short-Term");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12.2f", t.ch_metrics[c].max_short_term_lufs);
        std::printf("%.2f LUFS\n", t.max_short_term_lufs);

        // Integrated (joint only)
        std::printf("  %-22s", "Integrated");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12s", "—");
        std::printf("%.2f LUFS\n", t.integrated_lufs);

        // DR (PMF)
        std::printf("  %-22s", "DR (PMF)");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12.2f", t.ch_metrics[c].dr_raw);
        std::printf("DR%d\n", t.dr_score);

        // Min PSR
        std::printf("  %-22s", "Min PSR");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12s", "—");
        std::printf("%.2f dB\n", t.psr_db);

        // PLR
        std::printf("  %-22s", "PLR");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12s", "—");
        std::printf("%.2f dB\n", t.plr_db);

        // LRA
        std::printf("  %-22s", "LRA");
        for (size_t c = 0; c < nch; ++c)
            std::printf("%-12s", "—");
        std::printf("%.2f LU\n", t.lra_lu);
    }

    // ── Summary ────────────────────────────────────────────────────────
    std::cout << "\n" << sep << "\n";
    std::cout << "Summary\n";
    std::cout << sep << "\n\n";

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

        auto album_verdict = dr::classify_dr(
            static_cast<double>(dr::album_dr(tracks)));
        std::cout << "Verdict:           " << dr::verdict_string(album_verdict) << "\n";
    }

    std::cout << sep << "\n";
}

// ── Usage ──────────────────────────────────────────────────────────────────
void print_usage(const char* argv0) {
    std::cerr << PROGRAM_NAME << " — Zero-dependency Dynamic Range Meter\n\n"
              << "Usage: " << argv0 << " [options] <file_or_folder...>\n\n"
              << "Options:\n"
              << "  -f, --format <std|foobar|ext|detail>\n"
              << "      std    — dr.loudness-war.info compatible (default)\n"
              << "      foobar — foobar2000 DR Meter style\n"
              << "      ext    — extended with LUFS, PLR, crest factor\n"
              << "      detail — per-track per-channel (MAAT DROffline style)\n"
              << "  -o, --output <file>            Write output to file\n"
              << "  -q, --quiet                    Suppress progress output\n"
              << "  -v, --version                  Show version\n"
              << "  -h, --help                     Show this help\n\n"
              << "Supported formats: WAV, AIFF, FLAC, DSF, DFF (DSD)\n"
              << "Algorithm: TT Dynamic Range (3s blocks, top-20% percentile)\n";
}

void print_version() {
    std::cout << PROGRAM_NAME << " " << PROGRAM_VERSION
              << " — " << PROGRAM_DESC << "\n"
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
            if (fmt == "std")         format = OutputFormat::Standard;
            else if (fmt == "foobar") format = OutputFormat::Foobar;
            else if (fmt == "ext")    format = OutputFormat::Extended;
            else if (fmt == "detail") format = OutputFormat::Detail;
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
                print_extended(results, folder_name);
                break;
            case OutputFormat::Detail:
                print_detail(results, folder_name);
                break;
        }
    }

    if (out_fp) std::fclose(out_fp);
    return 0;
}
