// dvda.hpp -- DVD-Audio title-set reader (zero dependency).
//
// The AUDIO_TS counterpart to sacd.hpp: it reads the disc's own tables so a
// DVD-Audio title set presents as TITLES and TRACKS rather than as one
// multi-gigabyte blob. Parsing only -- no decoding, no FFmpeg -- so it builds
// in the zero-dependency tier and is testable on its own.
//
// WHY THIS IS NEEDED AT ALL. Without it, crete reading ATS_01_1.AOB measures
// the whole title set as a single track and stops at the first audio-format
// change with a warning (see audio_ffmpeg.hpp). That is honest but nearly
// useless on a real disc: Fleetwood Mac's Rumours (2001, Warner) is ONE title
// set holding FOUR titles -- a 96 kHz 5.1 mix, a 96 kHz stereo mix, a 48 kHz
// 5.1 version and a 1-second tail -- 39 tracks in total, and the group
// boundaries do not align with the file fragments.
//
// ── The format, and how it was established ─────────────────────────────────
//
// From the published DVD-Audio structure description, then CHECKED against the
// disc, because a plausible-but-wrong parse of a binary table is exactly the
// failure this project keeps finding. Two independent confirmations:
//
//   1. ARITHMETIC. Each title header carries a total length in 90 kHz PTS
//      ticks, and each of its track records carries its own. On this disc the
//      track lengths sum to the title total EXACTLY, on all four titles
//      (240063900, 237918150, 216438000, 94800). A wrong offset or endianness
//      cannot produce four exact matches, which is why validate() asserts it
//      rather than trusting the read.
//
//   2. AGREEMENT WITH THE AUDIO. The three audio-format entries decode to
//      24-bit/96 kHz 6-channel, 24-bit/96 kHz 2-channel and 24-bit/48 kHz
//      6-channel -- the same three groups, in the same order, that decoding
//      the AOBs had already found by bisecting for the point where the channel
//      count changes. The table and the bitstream agree.
//
// ── Layout ────────────────────────────────────────────────────────────────
//
// ATS_NN_0.IFO, all fields BIG-endian:
//
//   0x00  char[12]  "DVDAUDIO-ATS"
//   0x0C  u32       last sector of the title set (AOB total, in 2048-byte sectors)
//   0x1C  u32       last sector of this IFO
//   0xC4  u32       first sector of the AOB data
//   0xCC  u32       sector (within the IFO) holding the title table
//
//   0x100 + 16*i    audio format i, up to 8:
//       +0  u8      coding: 0 = LPCM, 1 = MLP
//       +2  u8      (quantization group 1 << 4) | group 2   0=16 1=20 2=24 f=absent
//       +3  u8      (sample rate  group 1 << 4) | group 2   0=48k 1=96k 2=192k
//                                                           8=44.1k 9=88.2k a=176.4k f=absent
//       +4  u8      channel assignment (see channels_for)
//
//   title table sector:
//       +0  u16     number of titles
//       +8 + 8*t    title t: +4 u32 offset of its track table, from the sector start
//
//   track table:
//       +3  u8      number of tracks
//       +4  u32     total length, 90 kHz ticks
//       +16 + 20*k  track k:
//           +4  u8   track number
//           +10 u32  length, 90 kHz ticks
//
// A DVD-Audio title set splits its GROUP 1 and GROUP 2 channels into separate
// quantization/rate fields because the format allows the surrounds to be
// carried at a lower rate than the fronts. Both are reported; crete measures
// what the decoder produces, so these are descriptive.

#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace dvda {

namespace fs = std::filesystem;

constexpr size_t SECTOR   = 2048;
constexpr unsigned PTS_HZ = 90000;

struct Format {
    bool     mlp        = false;   // false = LPCM
    unsigned bits_g1    = 0, bits_g2 = 0;   // 0 = absent
    unsigned rate_g1    = 0, rate_g2 = 0;   // Hz, 0 = absent
    unsigned ch_assign  = 0;
    unsigned channels   = 0;       // 0 = this assignment is not in the table below
};

struct Track {
    unsigned number   = 0;
    uint32_t pts_len  = 0;
    double   secs() const { return static_cast<double>(pts_len) / PTS_HZ; }
};

struct Title {
    unsigned index      = 0;       // 1-based, as the disc numbers them
    uint32_t total_pts  = 0;
    Format   format;               // empty when the set has fewer formats than titles
    std::vector<Track> tracks;
    double   secs() const { return static_cast<double>(total_pts) / PTS_HZ; }
};

struct TitleSet {
    std::string ifo_path;
    std::vector<std::string> aob_paths;     // ATS_NN_1..9.AOB, in part order
    uint32_t last_sector = 0;               // of the whole title set
    uint32_t aob_start_sector = 0;
    std::vector<Format> formats;
    std::vector<Title>  titles;

    size_t track_count() const {
        size_t n = 0;
        for (const auto& t : titles) n += t.tracks.size();
        return n;
    }
};

// ── Big-endian readers ────────────────────────────────────────────────────
inline uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline uint32_t rd32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8)  |  static_cast<uint32_t>(p[3]);
}

inline unsigned rate_code(unsigned c) {
    switch (c) {
        case 0:  return 48000;  case 1: return 96000;  case 2:  return 192000;
        case 8:  return 44100;  case 9: return 88200;  case 10: return 176400;
        default: return 0;      // 0xf = this group absent; anything else unknown
    }
}
inline unsigned bits_code(unsigned c) {
    switch (c) { case 0: return 16; case 1: return 20; case 2: return 24; default: return 0; }
}

// Channel count for a DVD-Audio channel assignment.
//
// ONLY the values this project has confirmed against real audio are mapped;
// everything else returns 0, meaning "ask the decoder". That is deliberate:
// the assignment table has 21 entries and publishing a guess for the other 19
// would put an unverified number next to measured ones. crete decodes the
// stream anyway, so the true channel count always arrives -- this is for
// describing a disc BEFORE decoding it.
inline unsigned channels_for(unsigned ch_assign) {
    switch (ch_assign) {
        case 1:  return 2;   // L R            -- confirmed on Rumours title 2
        case 20: return 6;   // L R C LFE Ls Rs -- confirmed on Rumours titles 1 and 3
        default: return 0;
    }
}

inline bool looks_like_dvda_ifo(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char id[12] = {0};
    f.read(id, 12);
    return f.gcount() == 12 && std::memcmp(id, "DVDAUDIO-ATS", 12) == 0;
}

// The ATS_NN_0.IFO belonging to the same title set as `any_path`, which may be
// the IFO itself or any of its .AOB fragments. Empty when there is none.
inline std::string ifo_for(const std::string& any_path) {
    const fs::path p(any_path);
    std::string name = p.filename().string();
    for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (name.size() < 10 || name.compare(0, 4, "ATS_") != 0) return {};
    std::string ifo = (p.parent_path() / (name.substr(0, 6) + "_0.IFO")).string();
    if (fs::exists(ifo) && looks_like_dvda_ifo(ifo)) return ifo;
    // Rips are inconsistent about case; try the lowercase spelling too.
    std::string lower = name.substr(0, 6) + "_0.ifo";
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string alt = (p.parent_path() / lower).string();
    if (fs::exists(alt) && looks_like_dvda_ifo(alt)) return alt;
    return {};
}

// Reads the title set described by an ATS_NN_0.IFO. Throws on a malformed
// table rather than returning a half-built answer.
inline TitleSet parse_ifo(const std::string& ifo_path) {
    std::ifstream f(ifo_path, std::ios::binary);
    if (!f) throw std::runtime_error("DVD-Audio: cannot open " + ifo_path);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    if (b.size() < SECTOR || std::memcmp(b.data(), "DVDAUDIO-ATS", 12) != 0)
        throw std::runtime_error("DVD-Audio: not an ATS IFO: " + ifo_path);

    TitleSet ts;
    ts.ifo_path         = ifo_path;
    ts.last_sector      = rd32(&b[0x0C]);
    ts.aob_start_sector = rd32(&b[0xC4]);

    // The IFO declares its own length; a truncated one must not be read past.
    const uint64_t declared = (static_cast<uint64_t>(rd32(&b[0x1C])) + 1) * SECTOR;
    if (b.size() < declared)
        throw std::runtime_error("DVD-Audio: " + ifo_path + " declares " +
                                 std::to_string(declared) + " bytes but holds " +
                                 std::to_string(b.size()) + " -- truncated rip?");

    for (unsigned i = 0; i < 8; ++i) {
        const uint8_t* e = &b[0x100 + 16 * i];
        bool empty = true;
        for (unsigned k = 0; k < 16; ++k) if (e[k]) { empty = false; break; }
        if (empty) continue;
        Format fm;
        fm.mlp       = (e[0] == 1);
        fm.bits_g1   = bits_code(e[2] >> 4);
        fm.bits_g2   = bits_code(e[2] & 0xf);
        fm.rate_g1   = rate_code(e[3] >> 4);
        fm.rate_g2   = rate_code(e[3] & 0xf);
        fm.ch_assign = e[4];
        fm.channels  = channels_for(fm.ch_assign);
        ts.formats.push_back(fm);
    }

    const uint64_t tbase = static_cast<uint64_t>(rd32(&b[0xCC])) * SECTOR;
    if (tbase + 8 > b.size())
        throw std::runtime_error("DVD-Audio: title table past end of " + ifo_path);
    const unsigned n_titles = rd16(&b[tbase]);
    if (n_titles == 0 || n_titles > 99)
        throw std::runtime_error("DVD-Audio: implausible title count " +
                                 std::to_string(n_titles) + " in " + ifo_path);

    for (unsigned t = 0; t < n_titles; ++t) {
        const uint64_t ent = tbase + 8 + 8 * t;
        if (ent + 8 > b.size()) throw std::runtime_error("DVD-Audio: title entry past end");
        const uint64_t tt = tbase + rd32(&b[ent + 4]);
        if (tt + 16 > b.size()) throw std::runtime_error("DVD-Audio: track table past end");

        Title ti;
        ti.index     = t + 1;
        ti.total_pts = rd32(&b[tt + 4]);
        const unsigned n_tracks = b[tt + 3];
        if (n_tracks == 0 || n_tracks > 99)
            throw std::runtime_error("DVD-Audio: implausible track count " +
                                     std::to_string(n_tracks));
        if (tt + 16 + 20ull * n_tracks > b.size())
            throw std::runtime_error("DVD-Audio: track records past end");
        for (unsigned k = 0; k < n_tracks; ++k) {
            const uint8_t* r = &b[tt + 16 + 20 * k];
            Track tr;
            tr.number  = r[4];
            tr.pts_len = rd32(&r[10]);
            ti.tracks.push_back(tr);
        }
        if (t < ts.formats.size()) ti.format = ts.formats[t];
        ts.titles.push_back(std::move(ti));
    }
    return ts;
}

// THE CORRECTNESS ASSERTION, and the reason to trust the offsets above.
//
// Each title states its own total length and each of its tracks states one.
// They are separate fields in separate structures, so if the offsets or the
// endianness were wrong the two would not agree -- and on the reference disc
// they agree EXACTLY on every title, which no accident produces.
//
// Returns an empty string when consistent, otherwise a description. Kept
// separate from parse_ifo so a caller can report rather than throw: a disc
// whose tables disagree is still measurable, it just cannot be trusted to
// slice into tracks.
inline std::string validate(const TitleSet& ts) {
    for (const auto& t : ts.titles) {
        uint64_t sum = 0;
        for (const auto& tr : t.tracks) sum += tr.pts_len;
        if (sum != t.total_pts)
            return "title " + std::to_string(t.index) + ": track lengths sum to " +
                   std::to_string(sum) + " ticks but the title declares " +
                   std::to_string(t.total_pts) + " (delta " +
                   std::to_string(static_cast<int64_t>(sum) -
                                  static_cast<int64_t>(t.total_pts)) + ")";
    }
    return {};
}

inline std::string format_desc(const Format& f) {
    if (!f.rate_g1) return "unknown";
    std::string s = (f.mlp ? "MLP " : "LPCM ") + std::to_string(f.bits_g1) +
                    "-bit / " + std::to_string(f.rate_g1) + " Hz";
    if (f.rate_g2 && (f.rate_g2 != f.rate_g1 || f.bits_g2 != f.bits_g1))
        s += " (surrounds " + std::to_string(f.bits_g2) + "-bit / " +
             std::to_string(f.rate_g2) + " Hz)";
    if (f.channels) s += ", " + std::to_string(f.channels) + " ch";
    else            s += ", channel assignment " + std::to_string(f.ch_assign);
    return s;
}

// ── NOT DONE: per-track byte extents ──────────────────────────────────────
//
// This reader gives titles, formats and track DURATIONS. It does not give
// where each track STARTS in the AOB stream, and MLP is variable-rate so a
// duration cannot be converted to an offset. Slicing a title set into tracks
// the way sacd.hpp slices an area therefore needs one more table.
//
// There is evidence of one. Immediately after the last title's track records
// (0x20D6 on the reference disc) sits:
//
//     01 00 00 00  00 1B CB 68  00 1B CB 72  27 04 00 06 ...
//                  ^ 1821032    ^ 1821042
//
// two big-endian values 10 apart, both inside the title set's 2345817
// sectors -- the shape of a start/end sector pair, and 10 sectors is a
// plausible size for the 1.05-second title 4 that precedes it.
//
// That is a hypothesis, not a finding, and it is deliberately not acted on.
// A wrong byte offset in a lossless stream yields audio that plays and DR
// that looks reasonable; the SACD work hit exactly that three times, and each
// time the wrong answer survived every plausibility check except an
// arithmetic one. Before this table is used it needs the same standard the
// track table just met: a quantity computed from it that must match something
// measured independently -- e.g. every title's extent summing to the title
// set size, or a track's extent decoding to the duration the track table
// already states.
//
// Until then crete measures a DVD-Audio title set as one stream per group and
// says so, and `dvda-info` reports the structure without claiming to extract
// it.

}  // namespace dvda
