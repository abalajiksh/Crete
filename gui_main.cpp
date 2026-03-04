// ============================================================================
// crête GUI — Dear ImGui Dynamic Range Meter
//
// Build: make gui   (requires SDL2 + OpenGL + Dear ImGui in third_party/)
// ============================================================================

#include "analysis.hpp"
#include "audio.hpp"
#include "file_dialog.hpp"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

#include <SDL.h>
#if defined(IMGUI_IMPL_OPENGL_ES2)
#include <SDL_opengles2.h>
#else
#include <SDL_opengl.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ── Version info ───────────────────────────────────────────────────────────
#ifndef CRETE_VERSION
#define CRETE_VERSION "0.1.3"
#endif
static constexpr const char* PROGRAM_NAME    = "crête";
static constexpr const char* PROGRAM_VERSION = CRETE_VERSION;

// ── DR color coding ────────────────────────────────────────────────────────
static ImVec4 dr_color(int dr) {
    if (dr > 20) return ImVec4(0.2f, 0.9f, 0.3f, 1.0f);  // green
    if (dr >= 14) return ImVec4(0.4f, 0.85f, 0.4f, 1.0f); // light green
    if (dr >= 8)  return ImVec4(0.9f, 0.85f, 0.3f, 1.0f); // yellow
    if (dr >= 5)  return ImVec4(0.95f, 0.55f, 0.2f, 1.0f); // orange
    return ImVec4(0.95f, 0.25f, 0.2f, 1.0f);               // red
}

static const char* verdict_short(dr::Verdict v) {
    switch (v) {
        case dr::Verdict::Exceptional: return "Exceptional";
        case dr::Verdict::Excellent:   return "Excellent";
        case dr::Verdict::Normal:      return "Normal";
        case dr::Verdict::Compressed:  return "Compressed";
        case dr::Verdict::Brickwalled: return "Brickwalled";
    }
    return "";
}

// ── Application state ──────────────────────────────────────────────────────
struct AppState {
    // Input
    char path_buf[4096] = "";

    // Output
    int  output_mode = 0;   // 0 = analysis folder, 1 = custom
    char output_path[4096] = "";
    int  format_idx = 0;    // 0=std, 1=foobar, 2=ext

    // Analysis state
    std::atomic<bool>  is_analyzing{false};
    std::atomic<bool>  cancel_requested{false};
    std::atomic<float> progress{0.0f};
    std::atomic<int>   current_file{0};
    std::atomic<int>   total_files{0};

    std::mutex         mtx;
    std::string        current_filename;
    std::vector<dr::TrackResult> results;
    std::string        error_log;
    bool               analysis_done = false;

    std::thread        worker;

    // UI state
    bool show_settings = false;

    // Collect files from path
    std::vector<std::string> collect_files_from_path() {
        std::vector<std::string> files;
        std::string p = path_buf;

        // Trim trailing whitespace and slashes
        while (!p.empty() && (p.back() == ' ' || p.back() == '\n' || p.back() == '\r'))
            p.pop_back();
        while (p.size() > 1 && (p.back() == '/' || p.back() == '\\'))
            p.pop_back();

        if (p.empty()) return files;

        try {
            if (fs::is_regular_file(p)) {
                if (audio::is_supported_format(p))
                    files.push_back(p);
                else {
                    std::lock_guard<std::mutex> lock(mtx);
                    error_log += "Unsupported format: " + fs::path(p).extension().string() + "\n";
                }
            } else if (fs::is_directory(p)) {
                for (const auto& entry : fs::directory_iterator(p)) {
                    if (entry.is_regular_file() &&
                        audio::is_supported_format(entry.path().string()))
                        files.push_back(entry.path().string());
                }
                std::sort(files.begin(), files.end());
                if (files.empty()) {
                    std::lock_guard<std::mutex> lock(mtx);
                    error_log += "No supported audio files (.wav .flac .aif .dsf .dff) in folder.\n";
                }
            } else {
                std::lock_guard<std::mutex> lock(mtx);
                error_log += "Path not found: " + p + "\n";
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mtx);
            error_log += "Error scanning path: " + std::string(e.what()) + "\n";
        }
        return files;
    }

    // Determine output file path
    std::string get_output_filepath() {
        std::string dir;
        if (output_mode == 0) {
            // Analysis folder
            std::string p = path_buf;
            if (fs::is_directory(p))
                dir = p;
            else if (fs::is_regular_file(p))
                dir = fs::path(p).parent_path().string();
        } else {
            dir = output_path;
        }
        if (dir.empty()) return "";
        if (dir.back() != '/' && dir.back() != '\\') dir += '/';
        return dir + "dr_analysis.txt";
    }
};

// ── Format helpers ─────────────────────────────────────────────────────────
static std::string format_duration(double secs) {
    int m = static_cast<int>(secs) / 60;
    int s = static_cast<int>(secs) % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

// ── Log generation (same format as CLI) ────────────────────────────────────
static std::string generate_log(const std::vector<dr::TrackResult>& tracks,
                                 const std::string& folder_path, int format_idx) {
    std::string out;
    char line[512];

    if (format_idx == 0) {
        // Standard format
        std::string sep(94, '-');
        std::string sep2(94, '=');
        out += sep + "\n";
        out += " Analyzed Folder: " + folder_path + "\n";
        out += sep + "\n";
        out += "DR         Peak       RMS        Filename\n";
        out += sep + "\n\n";

        for (const auto& t : tracks) {
            std::string peak;
            if (t.is_clipping) peak = "over      ";
            else { std::snprintf(line, sizeof(line), "%.2f dB  ", t.peak_dbfs); peak = line; }

            std::snprintf(line, sizeof(line), "DR%-3d      %-10s %.2f dB   %s\n",
                          t.dr_score, peak.c_str(), t.rms_dbfs, t.filename.c_str());
            out += line;
        }

        out += sep + "\n\n";
        std::snprintf(line, sizeof(line), " Number of Files: %zu\n", tracks.size());
        out += line;
        std::snprintf(line, sizeof(line), " Official DR Value: DR%d\n", dr::album_dr(tracks));
        out += line;
        out += "\n" + sep2 + "\n";

    } else if (format_idx == 1) {
        // Foobar style
        std::string sep(80, '-');
        std::string sep2(80, '=');

        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        char date_buf[32];
        std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&tt));

        out += std::string(PROGRAM_NAME) + " " + PROGRAM_VERSION + " / TT DR Offline Meter\n";
        out += "log date: " + std::string(date_buf) + "\n\n";
        out += sep + "\n";
        out += "Analyzed: " + folder_path + "\n";
        out += sep + "\n\n";
        out += "DR         Peak         RMS     Duration Track\n";
        out += sep + "\n";

        for (const auto& t : tracks) {
            auto name = t.filename;
            auto dot = name.rfind('.');
            if (dot != std::string::npos) name = name.substr(0, dot);

            char peak_s[16];
            if (t.is_clipping) std::snprintf(peak_s, sizeof(peak_s), "  0.00 dB");
            else std::snprintf(peak_s, sizeof(peak_s), "%6.2f dB", t.peak_dbfs);

            std::snprintf(line, sizeof(line), "DR%-3d      %s  %7.2f dB %8s %s\n",
                          t.dr_score, peak_s, t.rms_dbfs,
                          format_duration(t.duration_secs).c_str(), name.c_str());
            out += line;
        }

        out += sep + "\n\n";
        std::snprintf(line, sizeof(line), "Number of tracks:  %zu\n", tracks.size());
        out += line;
        std::snprintf(line, sizeof(line), "Official DR value: DR%d\n", dr::album_dr(tracks));
        out += line;

        if (!tracks.empty()) {
            const auto& f = tracks[0];
            std::snprintf(line, sizeof(line),
                "\nSamplerate:        %u Hz\nChannels:          %u\n"
                "Bits per sample:   %u\nCodec:             %s\n",
                f.sample_rate, f.channels, f.bit_depth, f.codec.c_str());
            out += line;
        }
        out += sep2 + "\n";

    } else {
        // Extended
        std::string sep(120, '-');
        std::string sep2(120, '=');

        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        char date_buf[32];
        std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&tt));

        out += std::string(PROGRAM_NAME) + " " + PROGRAM_VERSION +
               " — Extended Dynamic Range Analysis\n";
        out += "log date: " + std::string(date_buf) + "\n\n";
        out += sep + "\nAnalyzed: " + folder_path + "\n" + sep + "\n\n";

        std::snprintf(line, sizeof(line), "%-6s %-10s %-10s %-10s %-10s %-8s %-8s  %s\n",
                      "DR", "Peak", "RMS", "LUFS", "PLR", "Crest", "Dur", "Filename");
        out += line;
        out += sep + "\n";

        double sum_lufs = 0.0;
        for (const auto& t : tracks) {
            auto name = t.filename;
            auto dot = name.rfind('.');
            if (dot != std::string::npos) name = name.substr(0, dot);

            char peak_s[16];
            if (t.is_clipping) std::snprintf(peak_s, sizeof(peak_s), "over");
            else std::snprintf(peak_s, sizeof(peak_s), "%.2f dB", t.peak_dbfs);

            std::snprintf(line, sizeof(line),
                "DR%-3d  %-10s %-10.2f %-10.1f %-10.1f %-8.1f %-8s  %s\n",
                t.dr_score, peak_s, t.rms_dbfs, t.integrated_lufs,
                t.plr_db, t.crest_factor_db,
                format_duration(t.duration_secs).c_str(), name.c_str());
            out += line;
            sum_lufs += t.integrated_lufs;
        }

        out += sep + "\n\n";
        std::snprintf(line, sizeof(line), "Number of tracks:  %zu\n", tracks.size());
        out += line;
        std::snprintf(line, sizeof(line), "Official DR value: DR%d\n", dr::album_dr(tracks));
        out += line;
        if (!tracks.empty()) {
            std::snprintf(line, sizeof(line), "Avg. loudness:     %.1f LUFS\n",
                          sum_lufs / tracks.size());
            out += line;
            const auto& f = tracks[0];
            std::snprintf(line, sizeof(line),
                "\nSamplerate:        %u Hz\nChannels:          %u\n"
                "Bits per sample:   %u\nCodec:             %s\n"
                "Verdict:           %s\n",
                f.sample_rate, f.channels, f.bit_depth, f.codec.c_str(),
                dr::verdict_string(dr::classify_dr(dr::album_dr(tracks))));
            out += line;
        }
        out += sep2 + "\n";
    }

    return out;
}

// ── Analysis worker thread ─────────────────────────────────────────────────
static void analysis_worker(AppState* app) {
    auto files = app->collect_files_from_path();
    app->total_files.store(static_cast<int>(files.size()));

    if (files.empty()) {
        std::lock_guard<std::mutex> lock(app->mtx);
        app->error_log += "No supported audio files found.\n";
        app->is_analyzing.store(false);
        return;
    }

    std::vector<dr::TrackResult> results;
    results.reserve(files.size());

    for (size_t i = 0; i < files.size(); ++i) {
        if (app->cancel_requested.load()) break;

        auto fname = fs::path(files[i]).filename().string();
        {
            std::lock_guard<std::mutex> lock(app->mtx);
            app->current_filename = fname;
        }
        app->current_file.store(static_cast<int>(i + 1));
        app->progress.store(static_cast<float>(i) / static_cast<float>(files.size()));

        try {
            auto ad = audio::decode_file(files[i]);
            auto result = dr::analyze_track(
                ad.channels, ad.sample_rate, ad.bit_depth, ad.codec, fname);
            results.push_back(std::move(result));
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(app->mtx);
            app->error_log += fname + ": " + e.what() + "\n";
        }
    }

    app->progress.store(1.0f);

    // Write log file
    if (!results.empty() && !app->cancel_requested.load()) {
        std::string out_path = app->get_output_filepath();
        if (!out_path.empty()) {
            std::string folder = app->path_buf;
            std::string log = generate_log(results, folder, app->format_idx);
            try {
                std::ofstream ofs(out_path);
                if (ofs) {
                    ofs << log;
                    std::lock_guard<std::mutex> lock(app->mtx);
                    app->error_log += "Log saved to: " + out_path + "\n";
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(app->mtx);
                app->error_log += "Failed to write: " + out_path + "\n";
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(app->mtx);
        app->results = std::move(results);
        app->analysis_done = true;
        app->current_filename.clear();
    }
    app->is_analyzing.store(false);
}

// ── Start / cancel analysis ────────────────────────────────────────────────
static void start_analysis(AppState* app) {
    if (app->is_analyzing.load()) return;

    // Reset state
    {
        std::lock_guard<std::mutex> lock(app->mtx);
        app->results.clear();
        app->error_log.clear();
        app->analysis_done = false;
    }
    app->progress.store(0.0f);
    app->current_file.store(0);
    app->cancel_requested.store(false);
    app->is_analyzing.store(true);

    // Detach old thread if any
    if (app->worker.joinable()) app->worker.join();
    app->worker = std::thread(analysis_worker, app);
}

static void cancel_analysis(AppState* app) {
    app->cancel_requested.store(true);
}

// ── Custom ImGui styling ───────────────────────────────────────────────────
static void setup_style() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.WindowPadding = ImVec2(12, 12);
    style.FramePadding = ImVec2(8, 4);
    style.ItemSpacing = ImVec2(8, 6);

    // Slightly tweaked dark theme
    ImGui::StyleColorsDark();
    auto& c = style.Colors;
    c[ImGuiCol_WindowBg]   = ImVec4(0.10f, 0.10f, 0.12f, 1.0f);
    c[ImGuiCol_Header]     = ImVec4(0.20f, 0.22f, 0.27f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.30f, 0.38f, 1.0f);
    c[ImGuiCol_Button]     = ImVec4(0.22f, 0.24f, 0.32f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.35f, 0.48f, 1.0f);
    c[ImGuiCol_FrameBg]    = ImVec4(0.15f, 0.16f, 0.19f, 1.0f);
    c[ImGuiCol_Tab]        = ImVec4(0.18f, 0.20f, 0.25f, 1.0f);
    c[ImGuiCol_TabSelected]   = ImVec4(0.28f, 0.32f, 0.42f, 1.0f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.18f, 0.20f, 0.25f, 1.0f);
}

// ── Render settings modal ──────────────────────────────────────────────────
static void render_settings(bool* open) {
    ImGui::SetNextWindowSize(ImVec2(520, 440), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal("Settings", open,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar)) return;

    if (ImGui::BeginTabBar("SettingsTabs")) {
        // ── About tab ──
        if (ImGui::BeginTabItem("About")) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                "%s %s", PROGRAM_NAME, PROGRAM_VERSION);
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Zero-dependency dynamic range meter.\n\n"
                "Measures TT Dynamic Range (Pleasurize Music Foundation algorithm), "
                "EBU R128 integrated loudness, and crest factor for lossless audio files.\n\n"
                "Output compatible with dr.loudness-war.info submission format.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("Supported formats:");
            ImGui::BulletText("WAV (PCM 8/16/24/32-bit, IEEE float)");
            ImGui::BulletText("AIFF / AIFF-C");
            ImGui::BulletText("FLAC (built-in decoder)");
            ImGui::BulletText("DSF / DFF (DSD, decimated to PCM)");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextDisabled("MIT License - Ashwin Balaji 2026");
            ImGui::EndTabItem();
        }

        // ── DR Guide tab ──
        if (ImGui::BeginTabItem("DR Guide")) {
            ImGui::Spacing();
            ImGui::TextWrapped(
                "The Dynamic Range (DR) value measures the difference between the "
                "loudest and average levels in a recording. Higher values indicate "
                "more dynamic range — more contrast between loud and quiet passages.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::BeginTable("DRGuide", 3,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("DR Score", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Rating", ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                auto row = [](const char* score, const char* rating,
                              const char* desc, ImVec4 col) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(col, "%s", score);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(col, "%s", rating);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextWrapped("%s", desc);
                };

                row("> 20", "Exceptional",
                    "Audiophile recordings, classical, jazz. Full dynamic expression.",
                    ImVec4(0.2f, 0.9f, 0.3f, 1.0f));
                row("14 - 20", "Excellent",
                    "Well-mastered releases. Vinyl-era, high-quality remasters.",
                    ImVec4(0.4f, 0.85f, 0.4f, 1.0f));
                row("8 - 13", "Normal",
                    "Typical commercial releases. Adequate dynamics for most genres.",
                    ImVec4(0.9f, 0.85f, 0.3f, 1.0f));
                row("5 - 7", "Compressed",
                    "Heavily compressed. Loudness war casualty. Listening fatigue likely.",
                    ImVec4(0.95f, 0.55f, 0.2f, 1.0f));
                row("< 5", "Brickwalled",
                    "Severely clipped. Near-constant loudness, no dynamics. Distorted.",
                    ImVec4(0.95f, 0.25f, 0.2f, 1.0f));

                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Algorithm: Audio is split into 3-second blocks. "
                "RMS and peak are computed per block with power summed across channels. "
                "Silent blocks are discarded. The top 20%% loudest blocks by RMS are selected. "
                "DR = 20 * log10(mean_peak / mean_rms) of those blocks.");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(120, 0)))
        ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

// ── Main render function ───────────────────────────────────────────────────
static void render_ui(AppState& app) {
    // Fullscreen window
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##Main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    bool analyzing = app.is_analyzing.load();

    // ── Header ─────────────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", PROGRAM_NAME);
    ImGui::SameLine();
    ImGui::TextDisabled("v%s", PROGRAM_VERSION);
    ImGui::SameLine(ImGui::GetWindowWidth() - 90);
    if (ImGui::Button("Settings", ImVec2(76, 0)))
        app.show_settings = true;

    ImGui::Separator();
    ImGui::Spacing();

    // ── Path input ─────────────────────────────────────────────────────
    ImGui::Text("Path:");
    ImGui::SameLine();
    float btn_w = 72;
    float avail = ImGui::GetContentRegionAvail().x - (btn_w * 2 + 16);
    ImGui::SetNextItemWidth(avail);
    ImGui::InputText("##path", app.path_buf, sizeof(app.path_buf));

    ImGui::SameLine();
    if (ImGui::Button("File...", ImVec2(btn_w, 0))) {
        auto f = nfd::open_file("Select Audio File");
        if (!f.empty())
            std::strncpy(app.path_buf, f.c_str(), sizeof(app.path_buf) - 1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Folder...", ImVec2(btn_w, 0))) {
        auto f = nfd::open_folder("Select Album Folder");
        if (!f.empty())
            std::strncpy(app.path_buf, f.c_str(), sizeof(app.path_buf) - 1);
    }

    ImGui::Spacing();

    // ── Output settings ────────────────────────────────────────────────
    ImGui::Text("Save log to:");
    ImGui::SameLine();
    ImGui::RadioButton("Analysis folder", &app.output_mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Custom:", &app.output_mode, 1);
    if (app.output_mode == 1) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btn_w - 8);
        ImGui::InputText("##outpath", app.output_path, sizeof(app.output_path));
        ImGui::SameLine();
        if (ImGui::Button("...##out", ImVec2(btn_w, 0))) {
            auto f = nfd::open_folder("Select Output Folder");
            if (!f.empty())
                std::strncpy(app.output_path, f.c_str(), sizeof(app.output_path) - 1);
        }
    }

    // ── Format selector ────────────────────────────────────────────────
    ImGui::Text("Format:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    const char* fmts[] = {"Standard (dr.loudness-war.info)", "foobar2000 style", "Extended (all metrics)"};
    ImGui::Combo("##format", &app.format_idx, fmts, 3);

    ImGui::Spacing();

    // ── Action buttons + progress ──────────────────────────────────────
    if (!analyzing) {
        bool path_empty = (app.path_buf[0] == '\0');
        if (path_empty) ImGui::BeginDisabled();
        if (ImGui::Button("Analyze", ImVec2(100, 28)))
            start_analysis(&app);
        if (path_empty) ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Cancel", ImVec2(100, 28)))
            cancel_analysis(&app);
    }

    ImGui::SameLine();

    if (analyzing) {
        float prog = app.progress.load();
        int cur = app.current_file.load();
        int tot = app.total_files.load();
        char overlay[128];
        {
            std::lock_guard<std::mutex> lock(app.mtx);
            if (!app.current_filename.empty())
                std::snprintf(overlay, sizeof(overlay), "[%d/%d] %s",
                              cur, tot, app.current_filename.c_str());
            else
                std::snprintf(overlay, sizeof(overlay), "%d%%",
                              static_cast<int>(prog * 100));
        }
        ImGui::ProgressBar(prog, ImVec2(ImGui::GetContentRegionAvail().x, 28), overlay);
    } else if (app.analysis_done) {
        std::lock_guard<std::mutex> lock(app.mtx);
        if (app.results.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.3f, 1.0f),
                "No results. Check error log.");
        } else {
            int adr = dr::album_dr(app.results);
            ImGui::TextColored(dr_color(adr),
                "Album DR: DR%d  |  %zu files  |  %s",
                adr, app.results.size(),
                dr::verdict_string(dr::classify_dr(static_cast<double>(adr))));
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Results table ──────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lock(app.mtx);

        if (!app.results.empty()) {
            ImGuiTableFlags tbl_flags =
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_Sortable | ImGuiTableFlags_Reorderable;

            int n_cols = (app.format_idx == 2) ? 8 : 5;
            float table_h = ImGui::GetContentRegionAvail().y -
                            (app.error_log.empty() ? 10 : 80);

            if (ImGui::BeginTable("Results", n_cols, tbl_flags, ImVec2(0, table_h))) {
                ImGui::TableSetupColumn("DR", ImGuiTableColumnFlags_WidthFixed |
                    ImGuiTableColumnFlags_DefaultSort, 50);
                ImGui::TableSetupColumn("Peak", ImGuiTableColumnFlags_WidthFixed, 85);
                ImGui::TableSetupColumn("RMS", ImGuiTableColumnFlags_WidthFixed, 85);
                if (app.format_idx == 2) {
                    ImGui::TableSetupColumn("LUFS", ImGuiTableColumnFlags_WidthFixed, 70);
                    ImGui::TableSetupColumn("PLR", ImGuiTableColumnFlags_WidthFixed, 70);
                    ImGui::TableSetupColumn("Crest", ImGuiTableColumnFlags_WidthFixed, 65);
                }
                ImGui::TableSetupColumn("Dur", ImGuiTableColumnFlags_WidthFixed, 55);
                ImGui::TableSetupColumn("Filename", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                // Handle sorting
                if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
                    if (sort->SpecsDirty && sort->SpecsCount > 0) {
                        auto& spec = sort->Specs[0];
                        bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                        std::sort(app.results.begin(), app.results.end(),
                            [&](const dr::TrackResult& a, const dr::TrackResult& b) {
                                switch (spec.ColumnIndex) {
                                    case 0: return asc ? a.dr_score < b.dr_score
                                                       : a.dr_score > b.dr_score;
                                    case 1: return asc ? a.peak_dbfs < b.peak_dbfs
                                                       : a.peak_dbfs > b.peak_dbfs;
                                    case 2: return asc ? a.rms_dbfs < b.rms_dbfs
                                                       : a.rms_dbfs > b.rms_dbfs;
                                    default: return asc ? a.filename < b.filename
                                                        : a.filename > b.filename;
                                }
                            });
                        sort->SpecsDirty = false;
                    }
                }

                for (const auto& t : app.results) {
                    ImGui::TableNextRow();
                    char buf[64];

                    // DR (color-coded)
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(dr_color(t.dr_score), "DR%d", t.dr_score);

                    // Peak
                    ImGui::TableSetColumnIndex(1);
                    if (t.is_clipping)
                        ImGui::TextColored(ImVec4(0.95f, 0.3f, 0.2f, 1.0f), "over");
                    else {
                        std::snprintf(buf, sizeof(buf), "%.2f dB", t.peak_dbfs);
                        ImGui::Text("%s", buf);
                    }

                    // RMS
                    ImGui::TableSetColumnIndex(2);
                    std::snprintf(buf, sizeof(buf), "%.2f dB", t.rms_dbfs);
                    ImGui::Text("%s", buf);

                    int col_offset = 3;
                    if (app.format_idx == 2) {
                        // LUFS
                        ImGui::TableSetColumnIndex(3);
                        std::snprintf(buf, sizeof(buf), "%.1f", t.integrated_lufs);
                        ImGui::Text("%s", buf);

                        // PLR
                        ImGui::TableSetColumnIndex(4);
                        std::snprintf(buf, sizeof(buf), "%.1f dB", t.plr_db);
                        ImGui::Text("%s", buf);

                        // Crest
                        ImGui::TableSetColumnIndex(5);
                        std::snprintf(buf, sizeof(buf), "%.1f dB", t.crest_factor_db);
                        ImGui::Text("%s", buf);

                        col_offset = 6;
                    }

                    // Duration
                    ImGui::TableSetColumnIndex(col_offset);
                    ImGui::Text("%s", format_duration(t.duration_secs).c_str());

                    // Filename (strip extension)
                    ImGui::TableSetColumnIndex(col_offset + 1);
                    auto name = t.filename;
                    auto dot = name.rfind('.');
                    if (dot != std::string::npos) name = name.substr(0, dot);
                    ImGui::Text("%s", name.c_str());
                }

                ImGui::EndTable();
            }

            // ── Summary bar ──
            if (!app.results.empty()) {
                ImGui::Spacing();
                const auto& f = app.results[0];
                int adr = dr::album_dr(app.results);
                ImGui::TextColored(dr_color(adr), "DR%d", adr);
                ImGui::SameLine();
                ImGui::Text("| %zu files | %s %u-bit / %u Hz | %s",
                    app.results.size(), f.codec.c_str(),
                    f.bit_depth, f.sample_rate,
                    verdict_short(dr::classify_dr(static_cast<double>(adr))));
            }
        }

        // ── Error log ──────────────────────────────────────────────────
        if (!app.error_log.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "Log:");
            ImGui::SameLine();
            ImGui::TextWrapped("%s", app.error_log.c_str());
        }
    }

    ImGui::End();

    // ── Settings popup ─────────────────────────────────────────────────
    if (app.show_settings) ImGui::OpenPopup("Settings");
    render_settings(&app.show_settings);
}

// ── Main ───────────────────────────────────────────────────────────────────
int main(int, char**) {
    // ── SDL init ───────────────────────────────────────────────────────
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        return 1;
    }

    // GL context — platform-appropriate version
#if defined(IMGUI_IMPL_OPENGL_ES2)
    const char* glsl_version = "#version 100";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#elif defined(__APPLE__)
    // macOS requires OpenGL 3.2 Core with forward compatibility
    const char* glsl_version = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    // Linux / Windows: OpenGL 3.0 Core
    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow(
        "crête — Dynamic Range Meter",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1100, 700,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1);  // vsync

    // ── ImGui init ─────────────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // no imgui.ini

    setup_style();

    // Scale font for readability
    io.FontGlobalScale = 1.15f;

    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // ── App state ──────────────────────────────────────────────────────
    AppState app;

    // ── Main loop ──────────────────────────────────────────────────────
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window))
                running = false;

            // Handle file/folder drop
            if (event.type == SDL_DROPFILE) {
                char* dropped = event.drop.file;
                if (dropped) {
                    std::strncpy(app.path_buf, dropped, sizeof(app.path_buf) - 1);
                    SDL_free(dropped);
                }
            }
        }

        // If minimized, throttle
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(100);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        render_ui(app);

        ImGui::Render();
        int w, h;
        SDL_GL_GetDrawableSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    // ── Cleanup ────────────────────────────────────────────────────────
    if (app.is_analyzing.load()) {
        app.cancel_requested.store(true);
        if (app.worker.joinable()) app.worker.join();
    } else if (app.worker.joinable()) {
        app.worker.join();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
