// dvd.hpp -- DVD title-set fragment grouping (zero dependency).
//
// A DVD stores one logical title as SEVERAL files. UDF caps a file at 1 GB in
// the DVD spec, so authoring software splits the stream at a pack boundary and
// numbers the pieces:
//
//     VIDEO_TS/VTS_02_1.VOB  VTS_02_2.VOB  VTS_02_3.VOB     one 43-minute title
//     AUDIO_TS/ATS_01_1.AOB  ...           ATS_01_5.AOB     one DVD-Audio title set
//
// Measuring those as five separate "tracks" would be wrong in the obvious way
// (each piece metered on its own) and in a subtler one: the split lands
// wherever 1 GB fell, so a 3 s DR block straddles it and the boundary blocks
// belong to neither piece. They are one stream and are metered as one.
//
// Two further reasons this cannot be left to "just open the first file":
//
//   1. The FFmpeg stream INDEX is not stable across fragments of one title.
//      Measured on the DSOTM 50th DVD, same title, same three substreams:
//
//          VTS_02_1.VOB   index 2 = 0x80 ac3   3 = 0x81 ac3   4 = 0xa2 lpcm
//          VTS_02_2.VOB   index 2 = 0xa2 lpcm  3 = 0x80 ac3   4 = 0x81 ac3
//
//      Index follows order of first appearance in the program stream, so a
//      --stream pin correct for one fragment selects a different CODEC in the
//      next. Reading the fragments as one continuous stream means one probe
//      and one answer.
//
//   2. A DVD-Audio title set can hold SEVERAL audio groups with different
//      formats, and the group boundary need not align with the fragment
//      boundary. Fleetwood Mac's Rumours (2001, Warner) is one ATS whose
//      fragments probe as 96 kHz 6ch, 96 kHz 6ch, 96 kHz 6ch, 96 kHz 2ch,
//      48 kHz 6ch -- the 5.1 mix, the stereo mix and a 48 kHz version. See
//      README; crete measures the first group and says so.
//
// This header is filenames only: no I/O beyond a directory listing, no FFmpeg,
// so it builds in the zero-dependency tier and is testable on its own. The one
// exception is image_kind() at the end, which reads a disc image's ISO 9660
// root directory so an .iso of a DVD is named as one, not "not an SACD".

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

namespace dvd {

namespace fs = std::filesystem;

inline std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// "VTS_02_1.VOB" -> set "VTS_02", part 1.  Returns false for anything else,
// including VIDEO_TS.VOB (the disc-level menu, which is not part of a title
// set) and the .IFO/.BUP siblings.
//
// Part 0 is REJECTED for VOB: by the DVD-Video spec VTS_NN_0.VOB is the title
// set's menu, not its content, and metering a menu alongside the album would
// be wrong. DVD-Audio has no ATS_NN_0.AOB at all -- the _0 slot is the .IFO.
inline bool parse_fragment(const std::string& filename,
                           std::string& set_key, int& part) {
    const std::string f = upper(filename);
    std::string prefix, ext;
    if (f.size() > 4 && f.compare(0, 4, "VTS_") == 0)      { prefix = "VTS_"; ext = ".VOB"; }
    else if (f.size() > 4 && f.compare(0, 4, "ATS_") == 0) { prefix = "ATS_"; ext = ".AOB"; }
    else return false;

    // VTS_ dd _ d .VOB  -- exactly two digits, underscore, one digit.
    if (f.size() != prefix.size() + 2 + 1 + 1 + 4) return false;
    if (!std::isdigit(static_cast<unsigned char>(f[4])) ||
        !std::isdigit(static_cast<unsigned char>(f[5]))) return false;
    if (f[6] != '_') return false;
    if (!std::isdigit(static_cast<unsigned char>(f[7]))) return false;
    if (f.compare(8, 4, ext) != 0) return false;

    part = f[7] - '0';
    if (part < 1) return false;            // menu VOB / non-existent AOB slot
    set_key = f.substr(0, 6);              // "VTS_02"
    return true;
}

inline bool is_fragment(const std::string& path) {
    std::string k; int p;
    return parse_fragment(fs::path(path).filename().string(), k, p);
}

// Every fragment of the title set that `path` belongs to, in PART order.
// Returns just {path} when the name is not a fragment, so callers can use it
// unconditionally.
//
// Ordered by the part DIGIT, never by string comparison: the parts are single
// digits here, but sorting the names would tie this to the numbering surviving
// a rename, and the part number is the thing that actually defines the order.
inline std::vector<std::string> fragments_of(const std::string& path) {
    std::string key; int part;
    const fs::path p(path);
    if (!parse_fragment(p.filename().string(), key, part)) return {path};

    std::vector<std::pair<int, std::string>> found;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(p.parent_path(), ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        std::string k2; int p2;
        if (!parse_fragment(e.path().filename().string(), k2, p2)) continue;
        if (k2 != key) continue;
        found.emplace_back(p2, e.path().string());
    }
    if (found.empty()) return {path};

    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::string> out;
    out.reserve(found.size());
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

// What to call a title set in output: "VTS_02" rather than "VTS_02_1", so a
// three-fragment album does not read as if only the first piece was measured.
inline std::string display_name(const std::string& path) {
    std::string key; int part;
    if (parse_fragment(fs::path(path).filename().string(), key, part)) return key;
    return fs::path(path).stem().string();
}

// What kind of DVD a disc image holds, from its ISO 9660 root directory:
// "DVD-Audio" (an AUDIO_TS directory with content), "DVD-Video" (VIDEO_TS),
// or "" when it is not an ISO 9660 image or holds neither. The DVD spec
// requires the UDF/ISO 9660 bridge, so the ISO 9660 view is always there.
//
// Only identifies -- crete does not read title sets out of an image. It is
// here so the refusal can say what the image IS and what to do instead.
inline std::string image_kind(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    auto le32 = [](const unsigned char* p) {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    };
    unsigned char pvd[2048];
    f.seekg(16 * 2048);
    if (!f.read(reinterpret_cast<char*>(pvd), sizeof pvd)) return "";
    if (pvd[0] != 1 || std::string(reinterpret_cast<char*>(pvd + 1), 5) != "CD001") return "";

    struct Entry { std::string name; uint32_t lba, size; bool dir; };
    auto list = [&](uint32_t lba, uint32_t size) {
        std::vector<Entry> out;
        size = std::min<uint32_t>(size, 64 * 2048);
        std::vector<unsigned char> d(size);
        f.clear();
        f.seekg(std::streamoff(lba) * 2048);
        if (size == 0 || !f.read(reinterpret_cast<char*>(d.data()), size)) return out;
        for (size_t i = 0; i < d.size();) {
            const unsigned len = d[i];
            if (len == 0) { i = (i / 2048 + 1) * 2048; continue; }   // pad to sector end
            if (i + 33 > d.size() || i + len > d.size()) break;
            const unsigned nlen = d[i + 32];
            if (i + 33 + nlen > d.size()) break;
            out.push_back({upper(std::string(reinterpret_cast<char*>(&d[i + 33]), nlen)),
                           le32(&d[i + 2]), le32(&d[i + 10]), (d[i + 25] & 0x02) != 0});
            i += len;
        }
        return out;
    };

    bool audio = false, video = false;
    for (const auto& e : list(le32(pvd + 156 + 2), le32(pvd + 156 + 10))) {
        if (!e.dir) continue;
        if (e.name == "VIDEO_TS") video = true;
        // A DVD-Video disc usually carries an EMPTY AUDIO_TS: only one that
        // holds an audio object (ATS_xx_n.AOB) makes the disc DVD-Audio.
        if (e.name == "AUDIO_TS")
            for (const auto& a : list(e.lba, e.size))
                if (!a.dir && a.name.rfind("ATS_", 0) == 0
                           && a.name.find(".AOB") != std::string::npos) audio = true;
    }
    return audio ? "DVD-Audio" : video ? "DVD-Video" : "";
}

}  // namespace dvd
