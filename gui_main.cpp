// ============================================================================
// crête GUI — Dear ImGui Dynamic Range Meter (Modernist redesign)
//
// Build: make gui   (requires SDL2 + OpenGL + Dear ImGui in third_party/)
//
// Layout is organised as horizontal bands (Modernist grid):
//   header · source bar · DSD strip (DSD sources only) · verdict band ·
//   results table (DR meter-bar) + grouped detail panel · error log.
// Settings / DR Guide / output options live in an inline right-side panel,
// not a modal. Light + dark shells share one palette (crete_theme.hpp).
// ============================================================================

#include "analysis.hpp"
#include "audio.hpp"
#include "file_dialog.hpp"
#include "crete_theme.hpp"

#ifdef CRETE_HAS_FFMPEG
extern "C" {
#include <libavutil/avutil.h>
}
#endif

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

#ifndef CRETE_VERSION
#define CRETE_VERSION "0.9.2"
#endif
static constexpr const char* PROGRAM_NAME    = "crête";
static constexpr const char* PROGRAM_VERSION = CRETE_VERSION;

static const char* FORMAT_LABELS[] = { "Standard", "foobar", "Extended" };
static const char* DSD_MODE_LABELS[]   = { "Multistage", "Direct" };
static const char* DSD_FILTER_LABELS[] = { "default", "firls", "kaiser" };
static const char* DSD_RATE_LABELS[]   = { "44.1", "88.2", "176.4", "352.8" };
static const int   DSD_RATES[]         = { 44100, 88200, 176400, 352800 };

// ── Application state ──────────────────────────────────────────────────────
struct AppState {
    char path_buf[4096] = "";

    // Log saving is opt-in: results live only in the session unless the user
    // asks for a file (privacy default — nothing written to disk unasked).
    bool save_log = false;
    int  output_mode = 0;
    char output_path[4096] = "";
    int  format_idx = 0;

    // DSD decode options (exposed in the DSD strip)
    int  dsd_mode_idx   = 0;   // 0 Multistage, 1 Direct
    int  dsd_filter_idx = 0;   // 0 default, 1 firls, 2 kaiser
    int  dsd_rate_idx   = 0;   // index into DSD_RATES
    dsd::Options active_dsd_opt;   // snapshotted at analysis start

    bool dark_mode = false;
    bool restyle_requested = false;

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

    // UI
    bool side_open = false;
    int  side_tab  = 0;   // 0 About/Settings, 1 DR Guide
    int  selected_track = 0;

    // Cached DSD probe — recomputed only when path_buf changes, so the folder
    // is not re-scanned from disk on every frame (source_is_dsd runs per-frame).
    std::string dsd_probe_path;
    bool        dsd_probe_val = false;

    dsd::Options build_dsd_opts() const {
        dsd::Options o;
        o.mode = (dsd_mode_idx == 1) ? dsd::Mode::Direct : dsd::Mode::Multistage;
        o.filter = (dsd_filter_idx == 2) ? dsd::Filter::Kaiser
                 : (dsd_filter_idx == 1) ? dsd::Filter::LeastSquares
                                         : dsd::Filter::Default;
        o.out_rate = DSD_RATES[dsd_rate_idx];
        // Direct chain is DSD64 -> 44100 / default only; force valid combo.
        if (o.mode == dsd::Mode::Direct) { o.filter = dsd::Filter::Default; o.out_rate = 44100; }
        return o;
    }

    std::vector<std::string> collect_files_from_path() {
        std::vector<std::string> files;
        std::string p = path_buf;
        while (!p.empty() && (p.back()==' '||p.back()=='\n'||p.back()=='\r')) p.pop_back();
        while (p.size()>1 && (p.back()=='/'||p.back()=='\\')) p.pop_back();
        if (p.empty()) return files;
        try {
            if (fs::is_regular_file(p)) {
                if (audio::is_supported_format(p)) files.push_back(p);
                else { std::lock_guard<std::mutex> lk(mtx);
                       error_log += "Unsupported format: " + fs::path(p).extension().string() + "\n"; }
            } else if (fs::is_directory(p)) {
                for (const auto& e : fs::directory_iterator(p))
                    if (e.is_regular_file() && audio::is_supported_format(e.path().string()))
                        files.push_back(e.path().string());
                std::sort(files.begin(), files.end());
                if (files.empty()) { std::lock_guard<std::mutex> lk(mtx);
#ifdef CRETE_HAS_FFMPEG
                    error_log += "No supported audio files (.wav .flac .aif .dsf .dff + "
                                 "ffmpeg: .m4a .mp3 .aac .opus .ogg .wma .wv .ape .mpc .tak ...) in folder.\n";
#else
                    error_log += "No supported audio files (.wav .flac .aif .dsf .dff) in folder.\n";
#endif
                }
            } else { std::lock_guard<std::mutex> lk(mtx); error_log += "Path not found: " + p + "\n"; }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(mtx); error_log += "Error scanning path: " + std::string(e.what()) + "\n";
        }
        return files;
    }

    std::string get_output_filepath() {
        std::string dir;
        if (output_mode == 0) {
            std::string p = path_buf;
            if (fs::is_directory(p)) dir = p;
            else if (fs::is_regular_file(p)) dir = fs::path(p).parent_path().string();
        } else dir = output_path;
        if (dir.empty()) return "";
        if (dir.back()!='/' && dir.back()!='\\') dir += '/';
        return dir + "dr_analysis.txt";
    }
};

// Filesystem probe: is this path a DSD file, or a folder containing one?
// This touches the disk (directory scan), so callers must cache the result.
static bool probe_path_is_dsd(const std::string& path) {
    std::string ext = audio::get_extension(path);
    if (ext == ".dsf" || ext == ".dff") return true;
    if (fs::is_directory(path)) {
        try {
            for (const auto& e : fs::directory_iterator(path)) {
                std::string x = audio::get_extension(e.path().string());
                if (x == ".dsf" || x == ".dff") return true;
            }
        } catch (...) {}
    }
    return false;
}

// True if the current path (or the loaded results) points at DSD material.
// Runs every frame, so the disk probe is memoised on the path string and the
// folder is only re-scanned when the path actually changes.
static bool source_is_dsd(AppState& app) {
    for (const auto& t : app.results) if (t.codec == "DSD") return true;
    if (app.dsd_probe_path != app.path_buf) {
        app.dsd_probe_path = app.path_buf;
        app.dsd_probe_val  = probe_path_is_dsd(app.path_buf);
    }
    return app.dsd_probe_val;
}

// ── Format helpers ─────────────────────────────────────────────────────────
static std::string format_duration(double secs) {
    int m = (int)secs / 60, s = (int)secs % 60;
    char b[16]; std::snprintf(b, sizeof(b), "%d:%02d", m, s); return b;
}

// ── Log generation (unchanged output formats: std / foobar / ext) ───────────
static std::string generate_log(const std::vector<dr::TrackResult>& tracks,
                                const std::string& folder_path, int format_idx) {
    std::string out; char line[512];
    if (format_idx == 0) {
        std::string sep(94,'-'), sep2(94,'=');
        out += sep+"\n Analyzed Folder: "+folder_path+"\n"+sep+"\nDR         Peak       RMS        Filename\n"+sep+"\n\n";
        for (const auto& t : tracks) {
            std::string peak;
            if (t.is_clipping) peak = "over      ";
            else { std::snprintf(line,sizeof(line),"%.2f dB  ",t.peak_dbfs); peak=line; }
            std::snprintf(line,sizeof(line),"DR%-3d      %-10s %.2f dB   %s\n",
                          t.dr_score,peak.c_str(),t.rms_dbfs,t.filename.c_str());
            out += line;
        }
        out += sep+"\n\n";
        std::snprintf(line,sizeof(line)," Number of Files: %zu\n",tracks.size()); out += line;
        std::snprintf(line,sizeof(line)," Official DR Value: DR%d\n",dr::album_dr(tracks)); out += line;
        out += "\n"+sep2+"\n";
    } else if (format_idx == 1) {
        std::string sep(80,'-'), sep2(80,'=');
        auto now=std::chrono::system_clock::now(); auto tt=std::chrono::system_clock::to_time_t(now);
        char db[32]; std::strftime(db,sizeof(db),"%Y-%m-%d %H:%M:%S",std::localtime(&tt));
        out += std::string(PROGRAM_NAME)+" "+PROGRAM_VERSION+" / TT DR Offline Meter\n";
        out += "log date: "+std::string(db)+"\n\n"+sep+"\nAnalyzed: "+folder_path+"\n"+sep+"\n\n";
        out += "DR         Peak         RMS     Duration Track\n"+sep+"\n";
        for (const auto& t : tracks) {
            auto name=t.filename; auto dot=name.rfind('.'); if(dot!=std::string::npos) name=name.substr(0,dot);
            char ps[16]; if(t.is_clipping) std::snprintf(ps,sizeof(ps),"  0.00 dB"); else std::snprintf(ps,sizeof(ps),"%6.2f dB",t.peak_dbfs);
            std::snprintf(line,sizeof(line),"DR%-3d      %s  %7.2f dB %8s %s\n",
                          t.dr_score,ps,t.rms_dbfs,format_duration(t.duration_secs).c_str(),name.c_str());
            out += line;
        }
        out += sep+"\n\n";
        std::snprintf(line,sizeof(line),"Number of tracks:  %zu\n",tracks.size()); out += line;
        std::snprintf(line,sizeof(line),"Official DR value: DR%d\n",dr::album_dr(tracks)); out += line;
        if (!tracks.empty()) { const auto& f=tracks[0];
            std::snprintf(line,sizeof(line),"\nSamplerate:        %u Hz\nChannels:          %u\nBits per sample:   %u\nCodec:             %s\n",
                          f.sample_rate,f.channels,f.bit_depth,f.codec.c_str()); out += line; }
        out += sep2+"\n";
    } else {
        std::string sep(120,'-'), sep2(120,'=');
        auto now=std::chrono::system_clock::now(); auto tt=std::chrono::system_clock::to_time_t(now);
        char db[32]; std::strftime(db,sizeof(db),"%Y-%m-%d %H:%M:%S",std::localtime(&tt));
        out += std::string(PROGRAM_NAME)+" "+PROGRAM_VERSION+" — Extended Dynamic Range Analysis\n";
        out += "log date: "+std::string(db)+"\n\n"+sep+"\nAnalyzed: "+folder_path+"\n"+sep+"\n\n";
        std::snprintf(line,sizeof(line),"%-6s %-10s %-10s %-10s %-10s %-8s %-8s  %s\n","DR","Peak","RMS","LUFS","PLR","Crest","Dur","Filename");
        out += line; out += sep+"\n";
        double sum_lufs=0.0;
        for (const auto& t : tracks) {
            auto name=t.filename; auto dot=name.rfind('.'); if(dot!=std::string::npos) name=name.substr(0,dot);
            char ps[16]; if(t.is_clipping) std::snprintf(ps,sizeof(ps),"over"); else std::snprintf(ps,sizeof(ps),"%.2f dB",t.peak_dbfs);
            std::snprintf(line,sizeof(line),"DR%-3d  %-10s %-10.2f %-10.1f %-10.1f %-8.1f %-8s  %s\n",
                          t.dr_score,ps,t.rms_dbfs,t.integrated_lufs,t.plr_db,t.crest_factor_db,
                          format_duration(t.duration_secs).c_str(),name.c_str());
            out += line; sum_lufs += t.integrated_lufs;
        }
        out += sep+"\n\n";
        std::snprintf(line,sizeof(line),"Number of tracks:  %zu\n",tracks.size()); out += line;
        std::snprintf(line,sizeof(line),"Official DR value: DR%d\n",dr::album_dr(tracks)); out += line;
        if (!tracks.empty()) {
            std::snprintf(line,sizeof(line),"Avg. loudness:     %.1f LUFS\n",sum_lufs/tracks.size()); out += line;
            const auto& f=tracks[0];
            std::snprintf(line,sizeof(line),"\nSamplerate:        %u Hz\nChannels:          %u\nBits per sample:   %u\nCodec:             %s\nVerdict:           %s\n",
                          f.sample_rate,f.channels,f.bit_depth,f.codec.c_str(),
                          dr::verdict_string(dr::classify_dr(dr::album_dr(tracks)))); out += line;
        }
        out += sep2+"\n";
    }

    // MQA warning appended to every log format, mirroring the CLI banner.
    int mqa_n = 0; bool mqa_studio = false; double mqa_rate = 0;
    for (const auto& t : tracks)
        if (t.mqa.datasync_found) { mqa_n++; mqa_studio = mqa_studio || t.mqa.studio; mqa_rate = t.mqa.orig_rate/1000.0; }
    if (mqa_n) {
        std::snprintf(line,sizeof(line),
            "\n!! MQA LOSSY ENCODING DETECTED — source is not lossless\n"
            "   %d track(s) · %s · authored %.1f kHz\n"
            "   DR is measured on the delivered PCM; encoded content above the base band\n"
            "   is not reconstructed (no open MQA decoder exists).\n",
            mqa_n, mqa_studio ? "MQA Studio" : "MQA", mqa_rate);
        out += line;
    }
    return out;
}

// ── Analysis worker ─────────────────────────────────────────────────────────
static void analysis_worker(AppState* app) {
    auto files = app->collect_files_from_path();
    const int n = (int)files.size();
    app->total_files.store(n);
    if (files.empty()) {
        std::lock_guard<std::mutex> lk(app->mtx);
        app->error_log += "No supported audio files found.\n";
        app->is_analyzing.store(false);
        return;
    }

    // Decode + analyze files in parallel. Each file is independent (decode_file
    // and analyze_track hold no shared mutable state); DSD files still fan out
    // per-channel inside decode_file. Results land in per-index slots so the
    // original file order is preserved; a failure just leaves ok[i] = 0 and is
    // recorded in the error log. Threads pull work via an atomic cursor and
    // stop early on cancel.
    std::vector<dr::TrackResult> slots(n);
    std::vector<uint8_t>         ok(n, 0);
    std::atomic<int> next{0}, completed{0};

    unsigned hw = std::thread::hardware_concurrency();
    const int nthreads = std::min<int>(hw ? (int)hw : 4, n);

    auto do_work = [&]() {
        for (;;) {
            if (app->cancel_requested.load()) break;
            int i = next.fetch_add(1);
            if (i >= n) break;
            auto fname = fs::path(files[i]).filename().string();
            { std::lock_guard<std::mutex> lk(app->mtx); app->current_filename = fname; }
            try {
                auto ad = audio::decode_file(files[i], app->active_dsd_opt);
                slots[i] = dr::analyze_track(ad.channels, ad.sample_rate,
                                             ad.bit_depth, ad.codec, fname);
                slots[i].mqa = mqa::analyze_pcm(ad.channels, ad.sample_rate,
                                                ad.bit_depth);
                ok[i] = 1;
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lk(app->mtx);
                app->error_log += fname + ": " + e.what() + "\n";
            }
            int done = completed.fetch_add(1) + 1;
            app->current_file.store(done);
            app->progress.store((float)done / (float)n);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(nthreads);
    for (int k = 0; k < nthreads; ++k) pool.emplace_back(do_work);
    for (auto& th : pool) th.join();

    app->progress.store(1.0f);

    // Compact successful results back into original file order.
    std::vector<dr::TrackResult> results;
    results.reserve(n);
    for (int i = 0; i < n; ++i) if (ok[i]) results.push_back(std::move(slots[i]));

    if (!results.empty() && !app->cancel_requested.load() && app->save_log) {
        std::string out_path = app->get_output_filepath();
        if (!out_path.empty()) {
            std::string log = generate_log(results, app->path_buf, app->format_idx);
            try { std::ofstream ofs(out_path); if (ofs) { ofs<<log;
                   std::lock_guard<std::mutex> lk(app->mtx); app->error_log += "Log saved to: "+out_path+"\n"; } }
            catch (...) { std::lock_guard<std::mutex> lk(app->mtx); app->error_log += "Failed to write: "+out_path+"\n"; }
        }
    }
    { std::lock_guard<std::mutex> lk(app->mtx);
      app->results = std::move(results); app->analysis_done = true; app->current_filename.clear(); }
    app->is_analyzing.store(false);
}

static void start_analysis(AppState* app) {
    if (app->is_analyzing.load()) return;
    { std::lock_guard<std::mutex> lk(app->mtx);
      app->results.clear(); app->error_log.clear(); app->analysis_done = false; }
    app->progress.store(0.0f); app->current_file.store(0);
    app->cancel_requested.store(false); app->is_analyzing.store(true); app->selected_track = 0;
    app->active_dsd_opt = app->build_dsd_opts();   // snapshot options for the thread
    if (app->worker.joinable()) app->worker.join();
    app->worker = std::thread(analysis_worker, app);
}
static void cancel_analysis(AppState* app) { app->cancel_requested.store(true); }

// ── Bands ────────────────────────────────────────────────────────────────────
static void band_header(AppState& app) {
    ImGui::PushFont(theme::g_brand);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::P().accent);
    ImGui::TextUnformatted("crête");
    ImGui::PopStyleColor(); ImGui::PopFont();

    ImGui::SameLine(0, 14);
    ImGui::AlignTextToFramePadding();
    theme::Kicker("DYNAMIC RANGE METER");
    ImGui::SameLine(0, 12);
#ifdef CRETE_HAS_FFMPEG
    ImGui::TextDisabled("v%s +ffmpeg", PROGRAM_VERSION);
#else
    ImGui::TextDisabled("v%s", PROGRAM_VERSION);
#endif

    float btns = 190.0f;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - btns + ImGui::GetCursorPosX());
    if (theme::GhostButton("DR Guide")) { app.side_open = !(app.side_open && app.side_tab==1); app.side_tab = 1; }
    ImGui::SameLine();
    if (theme::GhostButton("Settings")) { app.side_open = !(app.side_open && app.side_tab==0); app.side_tab = 0; }
    theme::Rule(2.0f);
}

static void band_source(AppState& app, bool analyzing) {
    theme::Kicker("SOURCE");
    float btn = 88, fmt_w = 260, analyze_w = 120;
    float reserve = btn*2 + fmt_w + analyze_w + 8*4;
    ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x - reserve));
    ImGui::InputTextWithHint("##path", "Drop a folder or choose a file…", app.path_buf, sizeof(app.path_buf));
    ImGui::SameLine();
    if (ImGui::Button("File…", ImVec2(btn,0))) { auto f=nfd::open_file("Select Audio File");
        if(!f.empty()) std::strncpy(app.path_buf,f.c_str(),sizeof(app.path_buf)-1); }
    ImGui::SameLine();
    if (ImGui::Button("Folder…", ImVec2(btn,0))) { auto f=nfd::open_folder("Select Album Folder");
        if(!f.empty()) std::strncpy(app.path_buf,f.c_str(),sizeof(app.path_buf)-1); }

    ImGui::SameLine(0, 16);
    theme::Segmented("fmt", FORMAT_LABELS, 3, &app.format_idx);

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - analyze_w);
    if (!analyzing) {
        bool empty = (app.path_buf[0]=='\0');
        if (empty) ImGui::BeginDisabled();
        if (theme::PrimaryButton(app.analysis_done ? "Re-analyze" : "Analyze", ImVec2(analyze_w,0)))
            start_analysis(&app);
        if (empty) ImGui::EndDisabled();
    } else {
        if (ImGui::Button("Cancel", ImVec2(analyze_w,0))) cancel_analysis(&app);
    }
    theme::Rule(1.0f);
}

static void band_dsd(AppState& app) {
    const theme::Palette& p = theme::P();
    // accentSoft inset with a left accent bar
    ImVec2 c0 = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float pad = 10.0f, h = 34.0f;
    dl->AddRectFilled(ImVec2(c0.x-6, c0.y), ImVec2(c0.x+w, c0.y+h), theme::u32(p.accentSoft));
    dl->AddRectFilled(ImVec2(c0.x-6, c0.y), ImVec2(c0.x-3, c0.y+h), theme::u32(p.accent));
    ImGui::Dummy(ImVec2(0, pad*0.4f));

    ImGui::PushStyleColor(ImGuiCol_Text, p.accentInk);
    ImGui::PushFont(theme::g_head); ImGui::TextUnformatted("DSD SOURCE"); ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::SameLine(); ImGui::TextDisabled("· DSF / DFF");

    bool direct = (app.dsd_mode_idx == 1);
    ImGui::SameLine(0, 20); theme::Kicker("CHAIN"); ImGui::SameLine();
    theme::Segmented("dsdmode", DSD_MODE_LABELS, 2, &app.dsd_mode_idx);

    ImGui::SameLine(0, 16); theme::Kicker("FILTER"); ImGui::SameLine();
    if (direct) app.dsd_filter_idx = 0;
    theme::Segmented("dsdfilt", DSD_FILTER_LABELS, 3, &app.dsd_filter_idx, direct ? 0b110u : 0u);

    ImGui::SameLine(0, 16); theme::Kicker("RATE"); ImGui::SameLine();
    if (direct) app.dsd_rate_idx = 0;
    theme::Segmented("dsdrate", DSD_RATE_LABELS, 4, &app.dsd_rate_idx, direct ? 0b1110u : 0u);
    ImGui::SameLine(0, 6); ImGui::TextDisabled("kHz");
    ImGui::Dummy(ImVec2(0, 8));
    theme::Rule(2.0f);
}

static void stat(const char* value, const char* label, const ImVec4* vc = nullptr) {
    ImGui::BeginGroup();
    ImGui::PushFont(theme::g_head);
    ImGui::PushStyleColor(ImGuiCol_Text, vc ? *vc : theme::P().ink);
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor(); ImGui::PopFont();
    theme::Kicker(label);
    ImGui::EndGroup();
}

static void band_verdict(AppState& app) {
    int adr = dr::album_dr(app.results);
    const char* verdict = dr::verdict_string(dr::classify_dr((double)adr));
    ImGui::BeginGroup();
    ImGui::PushFont(theme::g_big);
    char drs[16]; std::snprintf(drs,sizeof(drs),"DR%d",adr);
    ImGui::TextUnformatted(drs);
    ImGui::PopFont();
    theme::Tag(verdict);
    ImGui::EndGroup();

    ImGui::SameLine(0, 24);
    // stats: files / avg lufs / max TP / total
    double sum_lufs=0, dur=0, maxtp=-99;
    for (auto& t: app.results){ sum_lufs+=t.integrated_lufs; dur+=t.duration_secs; maxtp=std::max(maxtp,t.max_true_peak_dbtp); }
    char b1[24],b2[24],b3[24],b4[24];
    std::snprintf(b1,sizeof(b1),"%zu",app.results.size());
    std::snprintf(b2,sizeof(b2),"%.1f",app.results.empty()?0:sum_lufs/app.results.size());
    std::snprintf(b3,sizeof(b3),"%+.1f",maxtp);
    std::snprintf(b4,sizeof(b4),"%s",format_duration(dur).c_str());
    ImVec4 tpc = (maxtp > -0.1) ? theme::P().accent : theme::P().ink;
    ImGui::AlignTextToFramePadding();
    stat(b1,"FILES");           ImGui::SameLine(0,22);
    stat(b2,"AVG LUFS");        ImGui::SameLine(0,22);
    stat(b3,"MAX dBTP",&tpc);   ImGui::SameLine(0,22);
    stat(b4,"TOTAL");
    theme::Rule(2.0f);
}

static void band_progress(AppState& app) {
    float prog = app.progress.load(); int cur=app.current_file.load(), tot=app.total_files.load();
    ImGui::PushFont(theme::g_head);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::P().accent);
    ImGui::TextUnformatted("ANALYZING"); ImGui::PopStyleColor(); ImGui::PopFont();
    ImGui::SameLine();
    // render_ui already holds app.mtx across this call — do NOT re-lock here:
    // app.mtx is non-recursive, so a second lock on the UI thread deadlocks it.
    ImGui::TextDisabled("· %d / %d · %s", cur, tot, app.current_filename.c_str());
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 40 + ImGui::GetCursorPosX());
    ImGui::Text("%d%%", (int)(prog*100));
    ImGui::ProgressBar(prog, ImVec2(-FLT_MIN, 8), "");
    theme::Rule(2.0f);
}

static void empty_state(AppState&) {
    ImGui::Dummy(ImVec2(0, 40));
    const char* t1 = "Drop an album folder or an audio file";
    float tw = 0; { ImGui::PushFont(theme::g_brand); tw = ImGui::CalcTextSize(t1).x; ImGui::PopFont(); }
    ImGui::SetCursorPosX((ImGui::GetWindowWidth()-tw)*0.5f);
    ImGui::PushFont(theme::g_brand); ImGui::TextUnformatted(t1); ImGui::PopFont();
    const char* t2 = "…or use the File / Folder buttons above to browse";
    ImGui::SetCursorPosX((ImGui::GetWindowWidth()-ImGui::CalcTextSize(t2).x)*0.5f);
    ImGui::TextDisabled("%s", t2);
    const char* chips = "WAV   AIFF   FLAC   DSF   DFF   CUE";
    ImGui::Dummy(ImVec2(0,8));
    ImGui::SetCursorPosX((ImGui::GetWindowWidth()-ImGui::CalcTextSize(chips).x)*0.5f);
    ImGui::TextDisabled("%s", chips);
}

// ── Detail panel (grouped, not a dense grid) ────────────────────────────────
static void detail_panel(AppState& app) {
    if (app.results.empty()) return;
    int n=(int)app.results.size();
    app.selected_track = std::clamp(app.selected_track, 0, n-1);
    const auto& t = app.results[app.selected_track];
    size_t nch = t.ch_metrics.size();
    auto name = t.filename; auto dot=name.rfind('.'); if(dot!=std::string::npos) name=name.substr(0,dot);
    char b[64];

    ImVec4 acc = theme::P().accent;
    theme::Kicker("SELECTED TRACK", &acc);
    ImGui::PushFont(theme::g_head); ImGui::TextWrapped("%s", name.c_str()); ImGui::PopFont();
    ImGui::TextDisabled("%02d · %u ch · %s %u-bit / %u Hz · %s",
        app.selected_track+1, t.channels, t.codec.c_str(), t.bit_depth, t.sample_rate,
        format_duration(t.duration_secs).c_str());

    // MQA warning — a listener measuring DR is entitled to know the source is
    // lossy MQA, not lossless. Deterministic datasync only (see mqa_detect.hpp).
    if (t.mqa.datasync_found) {
        ImGui::Spacing();
        char mb[96];
        std::snprintf(mb, sizeof(mb), "MQA %s · authored %.1f kHz",
                      t.mqa.studio ? "Studio" : "(lossy)", t.mqa.orig_rate / 1000.0);
        theme::Tag(mb, theme::P().accentSoft, theme::P().accent);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::P().accent);
        ImGui::TextWrapped("Lossy MQA encoding — this is not lossless audio.");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("DR is measured on the delivered %u kHz stream; the encoded "
                            "content above the base band is not reconstructed.", t.sample_rate);
    }

    // Hero DR + per-channel
    theme::Rule(2.0f, 8.0f);
    ImGui::BeginGroup();
    ImGui::PushFont(theme::g_big);
    std::snprintf(b,sizeof(b),"DR%d",t.dr_score); ImGui::TextUnformatted(b);
    ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::SameLine(0,14);
    ImGui::BeginGroup();
    theme::Kicker("DYNAMIC RANGE · PMF");
    if (nch >= 2) ImGui::Text("L  %.1f   /   R  %.1f", t.ch_metrics[0].dr_raw, t.ch_metrics[1].dr_raw);
    else if (nch == 1) ImGui::Text("Mono  %.1f", t.ch_metrics[0].dr_raw);
    ImGui::EndGroup();
    theme::Rule(2.0f, 8.0f);

    // Standards tags mirror MAAT DROffline: (ITU) BS.1770-4, (EBU) Tech 3342,
    // (AES) loudness-ratio conventions, (PMF) TT Dynamic Range. Explained in
    // the About tab.
    theme::SectionLabel("DYNAMICS");
    std::snprintf(b,sizeof(b),"%.2f dB", t.psr_db);          theme::KeyValue("Min PSR (AES)", b);
    std::snprintf(b,sizeof(b),"%.2f dB", t.plr_db);          theme::KeyValue("PLR (AES)", b);
    std::snprintf(b,sizeof(b),"%.2f LU", t.lra_lu);          theme::KeyValue("LRA (EBU)", b);
    std::snprintf(b,sizeof(b),"%.2f dB", t.crest_factor_db); theme::KeyValue("Crest factor", b);

    theme::SectionLabel("LOUDNESS");
    std::snprintf(b,sizeof(b),"%.2f LUFS", t.integrated_lufs);    theme::KeyValue("Integrated (ITU)", b);
    std::snprintf(b,sizeof(b),"%.2f LUFS", t.max_momentary_lufs); theme::KeyValue("Max Momentary (ITU)", b);
    std::snprintf(b,sizeof(b),"%.2f LUFS", t.max_short_term_lufs);theme::KeyValue("Max Short-term (ITU)", b);

    theme::SectionLabel(nch>=2 ? "PEAKS · L / R" : "PEAKS");
    if (nch >= 2) {
        std::snprintf(b,sizeof(b),"%.2f / %.2f dB", t.ch_metrics[0].sample_peak_dbfs, t.ch_metrics[1].sample_peak_dbfs);
        theme::KeyValue("Sample Peak", b);
        std::snprintf(b,sizeof(b),"%.2f / %.2f dBTP", t.ch_metrics[0].true_peak_dbtp, t.ch_metrics[1].true_peak_dbtp);
        theme::KeyValue("True Peak (ITU)", b);
    } else {
        ImVec4 pc = t.is_clipping ? theme::P().accent : theme::P().ink;
        if (t.is_clipping) theme::KeyValue("Sample Peak", "over", &pc);
        else { std::snprintf(b,sizeof(b),"%.2f dB", t.peak_dbfs); theme::KeyValue("Sample Peak", b); }
        std::snprintf(b,sizeof(b),"%.2f dBTP", t.max_true_peak_dbtp); theme::KeyValue("True Peak (ITU)", b);
    }
    std::snprintf(b,sizeof(b),"%.2f dBTP", t.max_true_peak_dbtp); theme::KeyValue("Max True Peak (ITU)", b);
}

// ── Results (table + detail split) ──────────────────────────────────────────
static void band_results(AppState& app) {
    float avail_h = ImGui::GetContentRegionAvail().y;
    float detail_w = 372.0f;
    ImGui::BeginChild("tbl", ImVec2(ImGui::GetContentRegionAvail().x - detail_w - 20, avail_h), false);

    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Sortable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_BordersInnerH;
    int ncols = (app.format_idx==2) ? 7 : 7; // DR TRACK PEAK RMS LUFS PLR DUR
    if (ImGui::BeginTable("Results", ncols, flags)) {
        ImGui::TableSetupColumn("DR", ImGuiTableColumnFlags_WidthFixed|ImGuiTableColumnFlags_DefaultSort, 120);
        ImGui::TableSetupColumn("TRACK", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("PEAK", ImGuiTableColumnFlags_WidthFixed, 78);
        ImGui::TableSetupColumn("RMS", ImGuiTableColumnFlags_WidthFixed, 74);
        ImGui::TableSetupColumn("LUFS", ImGuiTableColumnFlags_WidthFixed, 66);
        ImGui::TableSetupColumn("PLR", ImGuiTableColumnFlags_WidthFixed, 58);
        ImGui::TableSetupColumn("DUR", ImGuiTableColumnFlags_WidthFixed, 54);
        ImGui::TableSetupScrollFreeze(0,1);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs())
            if (sort->SpecsDirty && sort->SpecsCount>0) {
                auto& sp = sort->Specs[0]; bool asc = sp.SortDirection==ImGuiSortDirection_Ascending;
                std::sort(app.results.begin(), app.results.end(),
                    [&](const dr::TrackResult&a,const dr::TrackResult&b){
                        switch(sp.ColumnIndex){
                            case 0: return asc? a.dr_score<b.dr_score : a.dr_score>b.dr_score;
                            case 2: return asc? a.peak_dbfs<b.peak_dbfs : a.peak_dbfs>b.peak_dbfs;
                            case 3: return asc? a.rms_dbfs<b.rms_dbfs : a.rms_dbfs>b.rms_dbfs;
                            case 4: return asc? a.integrated_lufs<b.integrated_lufs : a.integrated_lufs>b.integrated_lufs;
                            case 5: return asc? a.plr_db<b.plr_db : a.plr_db>b.plr_db;
                            case 6: return asc? a.duration_secs<b.duration_secs : a.duration_secs>b.duration_secs;
                            default: return asc? a.filename<b.filename : a.filename>b.filename;
                        }});
                sort->SpecsDirty=false;
            }

        char b[64];
        for (int i=0;i<(int)app.results.size();++i) {
            const auto& t = app.results[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            char sid[24]; std::snprintf(sid,sizeof(sid),"##r%d",i);
            if (ImGui::Selectable(sid, i==app.selected_track, ImGuiSelectableFlags_SpanAllColumns))
                app.selected_track = i;
            ImGui::SameLine(0,0);
            theme::DrMeterBar(t.dr_score);

            ImGui::TableSetColumnIndex(1);
            auto name=t.filename; auto dot=name.rfind('.'); if(dot!=std::string::npos) name=name.substr(0,dot);
            ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(name.c_str());
            if (t.mqa.datasync_found) {
                ImGui::SameLine(0, 8);
                ImGui::PushStyleColor(ImGuiCol_Text, theme::P().accent);
                ImGui::TextUnformatted("MQA");
                ImGui::PopStyleColor();
            }

            ImGui::TableSetColumnIndex(2);
            if (t.is_clipping) { ImGui::PushStyleColor(ImGuiCol_Text, theme::P().accent); ImGui::TextUnformatted("over"); ImGui::PopStyleColor(); }
            else { std::snprintf(b,sizeof(b),"%.2f",t.peak_dbfs); ImGui::TextUnformatted(b); }
            ImGui::TableSetColumnIndex(3); std::snprintf(b,sizeof(b),"%.1f",t.rms_dbfs); ImGui::TextUnformatted(b);
            ImGui::TableSetColumnIndex(4); std::snprintf(b,sizeof(b),"%.1f",t.integrated_lufs); ImGui::TextUnformatted(b);
            ImGui::TableSetColumnIndex(5); std::snprintf(b,sizeof(b),"%.1f",t.plr_db); ImGui::TextUnformatted(b);
            ImGui::TableSetColumnIndex(6);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::P().muted);
            ImGui::TextUnformatted(format_duration(t.duration_secs).c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SameLine(0, 0);
    // 2px divider between table and detail
    ImVec2 dp = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(dp, ImVec2(dp.x+2, dp.y+avail_h), theme::u32(theme::P().lineStrong));
    ImGui::SameLine(0, 18);

    ImGui::BeginChild("detail", ImVec2(detail_w-20, avail_h), false);
    detail_panel(app);
    ImGui::EndChild();
}

// ── Right side panel: Settings / DR Guide ────────────────────────────────────
static void side_panel(AppState& app) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::P().surface);
    ImGui::BeginChild("side", ImVec2(360, 0), true);

    const char* tabs[] = { "Settings", "DR Guide" };
    theme::Segmented("sidetabs", tabs, 2, &app.side_tab);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 24 + ImGui::GetCursorPosX());
    if (theme::GhostButton("×")) app.side_open = false;
    theme::Rule(2.0f);

    if (app.side_tab == 0) {
        theme::SectionLabel("APPEARANCE");
        if (ImGui::Checkbox("Dark shell", &app.dark_mode)) app.restyle_requested = true;

        theme::SectionLabel("LOG FILE");
        // Opt-in: default is analyze-only, nothing written to disk. The
        // destination controls only appear once saving is enabled.
        ImGui::Checkbox("Save analysis log to disk", &app.save_log);
        if (!app.save_log) {
            ImGui::TextDisabled("Results stay in this window only.");
        } else {
            ImGui::RadioButton("Analysis folder", &app.output_mode, 0);
            ImGui::RadioButton("Custom folder",   &app.output_mode, 1);
            if (app.output_mode == 1) {
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 40);
                ImGui::InputText("##out", app.output_path, sizeof(app.output_path));
                ImGui::SameLine();
                if (ImGui::Button("…")) { auto f=nfd::open_folder("Select Output Folder");
                    if(!f.empty()) std::strncpy(app.output_path,f.c_str(),sizeof(app.output_path)-1); }
            }
        }

        theme::SectionLabel("ABOUT");
#ifdef CRETE_HAS_FFMPEG
        ImGui::PushFont(theme::g_head);
        ImGui::Text("%s %s  [+ffmpeg-compact %s]", PROGRAM_NAME, PROGRAM_VERSION, av_version_info());
        ImGui::PopFont();
#else
        ImGui::PushFont(theme::g_head); ImGui::Text("%s %s", PROGRAM_NAME, PROGRAM_VERSION); ImGui::PopFont();
#endif
        ImGui::TextWrapped("Zero-dependency dynamic range meter. TT Dynamic Range (PMF), "
            "EBU R128 integrated loudness, true peak, PLR/PSR/LRA and crest factor. "
            "Output compatible with dr.loudness-war.info.");
        ImGui::Dummy(ImVec2(0,6));
#ifdef CRETE_HAS_FFMPEG
        theme::SectionLabel("FFMPEG DECODE (COMPACT, LGPL-ONLY)");
        ImGui::TextWrapped("This build statically links a compact, decode-only FFmpeg. "
            "Native formats (WAV, AIFF, FLAC, DSF/DFF) always use crête's own "
            "validated decoders; FFmpeg adds the following codecs:");
        ImGui::Dummy(ImVec2(0,2));
        // Mirrors DECODERS in scripts/build_ffmpeg_compact.sh and the extension
        // whitelist in audio.hpp is_ffmpeg_extra_format().
        ImGui::TextDisabled("ALAC · AAC (incl. LATM) · MP3 · Opus · Vorbis");
        ImGui::TextDisabled("WMA v1/v2 · WMA Pro · WMA Lossless");
        ImGui::TextDisabled("WavPack · Monkey's Audio (APE) · Musepack (SV7/SV8) · TAK");
        ImGui::Dummy(ImVec2(0,2));
        ImGui::TextWrapped("Accepted extensions: .m4a .mp4 .m4b .aac .mp3 .opus "
            ".ogg .oga .mka .webm .wma .wv .ape .mpc .tak");
        ImGui::Dummy(ImVec2(0,6));
#endif

        theme::SectionLabel("MEASUREMENT STANDARDS");
        ImGui::TextWrapped("(ITU) — ITU-R BS.1770-4: K-weighted loudness (LUFS) and "
            "4x-oversampled true peak (dBTP). Integrated loudness gated per EBU R 128.");
        ImGui::Dummy(ImVec2(0,4));
        ImGui::TextWrapped("(EBU) — EBU Tech 3342: Loudness Range (LRA). Distribution of "
            "3-second short-term loudness, 10th to 95th percentile with a -20 LU "
            "relative gate.");
        ImGui::Dummy(ImVec2(0,4));
        ImGui::TextWrapped("(AES) — AES loudness conventions: PLR is true peak minus "
            "integrated loudness; Min PSR is the smallest gap between a 3-second "
            "window's sample peak and its short-term loudness.");
        ImGui::Dummy(ImVec2(0,4));
        ImGui::TextWrapped("(PMF) — TT Dynamic Range, Pleasurize Music Foundation: the DR "
            "score, peak-to-loudest-RMS. Matches dr.loudness-war.info submissions.");
        ImGui::Dummy(ImVec2(0,4));
        ImGui::TextDisabled("Per-channel Momentary/Short-Term values are informative\n"
            "(non-ITU): BS.1770 defines them for the summed program only.");
        ImGui::Dummy(ImVec2(0,6));
        ImGui::TextDisabled("Native: WAV · AIFF/AIFF-C · FLAC · DSF/DFF (DSD)");
        ImGui::TextDisabled("MIT License — Ashwin Balaji 2026");
        ImGui::TextDisabled("Archivo: SIL Open Font License 1.1");
    } else {
        theme::SectionLabel("WHAT DR MEANS");
        ImGui::TextWrapped("The Dynamic Range (DR) value is the gap between the loudest and "
            "average levels. Higher = more contrast between loud and quiet passages.");
        theme::SectionLabel("SCALE");
        struct Row { const char* s; const char* r; };
        Row rows[] = {
            {"> 20",   "Exceptional — audiophile, classical, jazz"},
            {"14 – 20","Excellent — well-mastered, vinyl-era"},
            {"8 – 13", "Normal — typical commercial release"},
            {"5 – 7",  "Compressed — loudness-war casualty"},
            {"< 5",    "Brickwalled — clipped, no dynamics"},
        };
        for (auto& r : rows) {
            ImGui::PushFont(theme::g_head); ImGui::TextUnformatted(r.s); ImGui::PopFont();
            ImGui::SameLine(80); ImGui::TextWrapped("%s", r.r);
        }
        theme::SectionLabel("ALGORITHM");
        ImGui::TextWrapped("Audio is split into 3-second blocks; RMS and peak are computed per "
            "block (power-summed across channels). Silent blocks are dropped. DR is taken from "
            "the top 20%% loudest blocks, per channel, then averaged.");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ── Main render ──────────────────────────────────────────────────────────────
static void render_ui(AppState& app) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##Main", nullptr,
        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|
        ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoBringToFrontOnFocus);

    bool analyzing = app.is_analyzing.load();
    float side_w = app.side_open ? (360 + 20) : 0;

    ImGui::BeginChild("content", ImVec2(ImGui::GetContentRegionAvail().x - side_w, 0), false);
    band_header(app);
    band_source(app, analyzing);
    if (source_is_dsd(app)) band_dsd(app);

    // Held across the bands below (band_progress / band_verdict / band_results
    // / error log) which read shared state — they must NOT re-lock app.mtx.
    std::lock_guard<std::mutex> lk(app.mtx);
    if (analyzing)              band_progress(app);
    else if (!app.results.empty()) band_verdict(app);

    if (!app.results.empty())  band_results(app);
    else if (!analyzing)       empty_state(app);

    if (!app.error_log.empty()) {
        ImGui::Dummy(ImVec2(0,4)); theme::Rule(1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::P().accentInk);
        ImGui::TextWrapped("%s", app.error_log.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    if (app.side_open) { ImGui::SameLine(0, 20); side_panel(app); }
    ImGui::End();
}

// ── Main ─────────────────────────────────────────────────────────────────────
int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError()); return 1;
    }
#if defined(IMGUI_IMPL_OPENGL_ES2)
    const char* glsl_version = "#version 100";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#elif defined(__APPLE__)
    const char* glsl_version = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window* window = SDL_CreateWindow("crête — Dynamic Range Meter",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1180, 780,
        SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) { fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError()); return 1; }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    theme::load_fonts();
    AppState app;
    theme::apply(app.dark_mode);

    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init(glsl_version);

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                ev.window.windowID == SDL_GetWindowID(window)) running = false;
            if (ev.type == SDL_DROPFILE && ev.drop.file) {
                std::strncpy(app.path_buf, ev.drop.file, sizeof(app.path_buf)-1);
                SDL_free(ev.drop.file);
            }
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) { SDL_Delay(100); continue; }

        if (app.restyle_requested) { theme::apply(app.dark_mode); app.restyle_requested = false; }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        render_ui(app);
        ImGui::Render();

        int w,h; SDL_GL_GetDrawableSize(window,&w,&h);
        glViewport(0,0,w,h);
        const ImVec4 bg = theme::P().bg;
        glClearColor(bg.x,bg.y,bg.z,1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    if (app.is_analyzing.load()) { app.cancel_requested.store(true); if (app.worker.joinable()) app.worker.join(); }
    else if (app.worker.joinable()) app.worker.join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
