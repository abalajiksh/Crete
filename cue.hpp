#pragma once
// ============================================================================
// cue.hpp — Minimal zero-dependency cue sheet parser
//
// Parses CDDA-style cue sheets to expose:
//   - The referenced audio FILE (relative to the cue file's directory)
//   - Album title / performer
//   - Per-track number, title, performer, and INDEX 01 start time
//
// Recognized directives: FILE, TRACK, TITLE, PERFORMER, INDEX.
// Ignored:               REM, ISRC, FLAGS, PREGAP, POSTGAP, CATALOG, CDTEXTFILE.
//
// Track times use INDEX 01 (the actual track start, after any pregap), which
// matches foobar2000 and most cue-aware analysers. Times are stored as CDDA
// frames (1/75 sec each) and converted to PCM samples at analysis time via
// frames_to_samples() using the decoded sample rate, so the same cue works
// for 44.1 kHz WAV and 352.8 kHz DSD-decoded PCM without rounding loss
// (every standard rate is divisible by 75).
// ============================================================================

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace cue {

struct Track {
    int         number      = 0;
    std::string title;
    std::string performer;
    uint64_t    start_frame = 0;   // INDEX 01 time in CDDA frames (1/75 sec)
};

struct Sheet {
    std::string source_path;       // path to the .cue file itself
    std::string referenced_file;   // raw FILE "..." value from the cue
    std::string album_title;
    std::string album_performer;
    std::vector<Track> tracks;
};

// ── Time / size conversions ────────────────────────────────────────────────

// Parse "MM:SS:FF" (frames = 1/75 sec) into total CDDA frames.
inline uint64_t parse_time_to_frames(const std::string& s) {
    int m = 0, sec = 0, f = 0;
    if (std::sscanf(s.c_str(), "%d:%d:%d", &m, &sec, &f) != 3)
        return 0;
    if (m < 0 || sec < 0 || f < 0) return 0;
    return static_cast<uint64_t>(m) * 60 * 75
         + static_cast<uint64_t>(sec) * 75
         + static_cast<uint64_t>(f);
}

// Convert CDDA frames to PCM samples at the decoded rate. Integer division
// is exact for every standard rate (44.1, 88.2, 176.4, 352.8, 48, 96, 192 kHz)
// because all are integer multiples of 75.
inline uint64_t frames_to_samples(uint64_t frames, uint32_t sample_rate) {
    return frames * static_cast<uint64_t>(sample_rate) / 75ULL;
}

// ── Lightweight string utilities ───────────────────────────────────────────

inline std::string ltrim(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

inline std::string uppercase(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
    return out;
}

inline std::string lowercase(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return out;
}

// Extract a quoted or single-word value that follows `keyword` on a line.
//   TITLE "Hello World"  -> Hello World
//   TITLE Hello          -> Hello
inline std::string value_after(const std::string& line, const std::string& keyword) {
    auto pos = line.find(keyword);
    if (pos == std::string::npos) return "";
    pos += keyword.length();
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])))
        ++pos;
    if (pos >= line.size()) return "";
    if (line[pos] == '"') {
        auto end = line.find('"', pos + 1);
        if (end == std::string::npos) return "";
        return line.substr(pos + 1, end - pos - 1);
    }
    auto end = pos;
    while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end])))
        ++end;
    return line.substr(pos, end - pos);
}

// Read the cue file as bytes, stripping any UTF-8 BOM.
inline std::string read_text(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 &&
        static_cast<uint8_t>(s[0]) == 0xEF &&
        static_cast<uint8_t>(s[1]) == 0xBB &&
        static_cast<uint8_t>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

// ── Parser ─────────────────────────────────────────────────────────────────
//
// Returns true if at least one TRACK was successfully extracted.

inline bool parse(const std::string& cue_path, Sheet& out) {
    std::string content = read_text(cue_path);
    if (content.empty()) return false;

    out = {};
    out.source_path = cue_path;

    std::istringstream stream(content);
    std::string line;
    Track* cur = nullptr;
    bool in_track = false;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = ltrim(line);
        if (trimmed.empty() || trimmed[0] == ';') continue;

        // First token determines the directive.
        std::istringstream toks(trimmed);
        std::string tok;
        toks >> tok;
        std::string utok = uppercase(tok);

        if (utok == "REM" || utok == "ISRC" || utok == "FLAGS" ||
            utok == "PREGAP" || utok == "POSTGAP" || utok == "CATALOG" ||
            utok == "CDTEXTFILE")
            continue;

        if (utok == "FILE") {
            out.referenced_file = value_after(trimmed, "FILE");
        }
        else if (utok == "TITLE") {
            std::string v = value_after(trimmed, "TITLE");
            if (in_track && cur) cur->title = v;
            else                 out.album_title = v;
        }
        else if (utok == "PERFORMER") {
            std::string v = value_after(trimmed, "PERFORMER");
            if (in_track && cur) cur->performer = v;
            else                 out.album_performer = v;
        }
        else if (utok == "TRACK") {
            int n = 0;
            std::string type;
            toks >> n >> type;
            out.tracks.push_back({});
            cur = &out.tracks.back();
            cur->number = n;
            // Inherit album performer; an explicit per-track PERFORMER
            // line later in the block will overwrite it.
            cur->performer = out.album_performer;
            in_track = true;
        }
        else if (utok == "INDEX") {
            int idx = 0;
            std::string time_str;
            toks >> idx >> time_str;
            if (in_track && cur && idx == 1)
                cur->start_frame = parse_time_to_frames(time_str);
        }
        // Unrecognized directives are silently ignored.
    }

    return !out.tracks.empty();
}

// ── Locate the audio file the cue refers to ────────────────────────────────
//
// Joins the FILE value with the cue's parent directory. If that exact path
// doesn't exist, falls back to a case-insensitive scan of the cue's directory
// (common when cue files travel between OSes).

inline std::string resolve_referenced_file(const Sheet& s) {
    namespace fs = std::filesystem;
    if (s.referenced_file.empty() || s.source_path.empty()) return "";

    std::error_code ec;
    fs::path cue_dir = fs::path(s.source_path).parent_path();
    fs::path candidate = cue_dir / s.referenced_file;

    if (fs::exists(candidate, ec)) return candidate.string();

    if (fs::is_directory(cue_dir, ec)) {
        std::string want = lowercase(s.referenced_file);
        for (const auto& e : fs::directory_iterator(cue_dir, ec)) {
            if (!e.is_regular_file()) continue;
            std::string fn = lowercase(e.path().filename().string());
            if (fn == want) return e.path().string();
        }
    }
    return "";
}

} // namespace cue
