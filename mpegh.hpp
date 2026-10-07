// mpegh.hpp -- MPEG-H 3D Audio identification (zero dependency).
//
// IDENTIFICATION, NOT DECODE. No crete build can measure MPEG-H: the compact
// FFmpeg has no decoder for it, and the one open decoder (Fraunhofer mpeghdec)
// was rejected as a dependency -- its licence grants no patent rights and
// binds redistributors, its build fetches two more repositories at configure
// time, and it RENDERS to a chosen speaker layout, so a DR taken from it would
// depend on that choice and have no oracle. Sony 360 Reality Audio is MPEG-H
// in MP4, so this is what a 360RA file meets.
//
// What this header does instead is name the file precisely, so it is refused
// with a reason rather than with FFmpeg's "Could not find codec parameters ...
// unspecified number of channels" followed by "no audio stream". It reads two
// carriers:
//
//   MP4   the audio sample entry 'mhm1' / 'mha1' (and the '2' variants) and
//         its MHAConfigurationBox 'mhaC': configurationVersion,
//         mpegh3daProfileLevelIndication, referenceChannelLayout. An 'enca'
//         entry whose sinf/frma names an MPEG-H format is reported as
//         encrypted MPEG-H.
//   TS    the PMT: stream_type 0x2D, and the MPEG-H 3D Audio descriptor
//         carried in the extension descriptor (tag 0x3F, extension tag 0x08).
//         188-byte packets and 192-byte (BDAV .m2ts) packets.
//
// Read bounded: box headers and moov only for MP4, the first 4 MB of packets
// for TS. No audio payload is touched. Nothing here feeds metrics.
//
// PROVENANCE. Field layouts were checked byte-for-byte against the Fraunhofer
// IIS mpegh-test-content transport-layer set (CICP 1-20 except 8, Baseline
// and LC-Baseline-compatible profiles, plain and fragmented MP4, TS packed
// one access unit per PES, five per PES and 2000 bytes per PES), whose
// .printmp4.txt dumps are the oracle for the MP4 side. On every TS file the
// PMT descriptor carries the same profile-level indication and reference
// layout as the MP4 of the same encode. Notably ffprobe names the MP4
// correctly but calls the TS "mp3"; the TS is where a native reader earns
// its keep.

#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mpegh {

struct Info {
    bool        found        = false;
    std::string container;          // "MP4" or "MPEG-2 TS"
    std::string sample_entry;       // "mhm1", "mha1", ... (MP4 only)
    bool        encrypted    = false;   // MP4 'enca' wrapping an MPEG-H entry
    int         profile_level = -1;     // mpegh3daProfileLevelIndication
    int         ref_layout    = -1;     // referenceChannelLayout (CICP)
    int         other_audio   = 0;      // non-MPEG-H audio tracks / streams
};

// ── Names ───────────────────────────────────────────────────────────────────

// mpegh3daProfileLevelIndication. Only the two ranges the test set exercises
// are named: 0x0B-0x0F Low Complexity levels 1-5 and 0x10-0x14 Baseline
// levels 1-5 (the LC-Baseline-compatible encodes signal LC). Anything else is
// printed as its number rather than named from memory.
inline std::string profile_name(int pli) {
    char buf[64];
    if (pli >= 0x0B && pli <= 0x0F)
        std::snprintf(buf, sizeof buf, "Low Complexity profile, level %d", pli - 0x0A);
    else if (pli >= 0x10 && pli <= 0x14)
        std::snprintf(buf, sizeof buf, "Baseline profile, level %d", pli - 0x0F);
    else
        std::snprintf(buf, sizeof buf, "profile-level indication 0x%02X", pli);
    return buf;
}

// referenceChannelLayout is a ChannelConfiguration code point (ISO/IEC
// 23091-3, "CICP"). The channel COUNTS were confirmed on the test set, whose
// encodes run at 32 kbit/s per signal: the mdat bitrate divided by 32 kbit/s
// lands within one signal of each count below (CICP 8, dual mono, is the one
// layout the set does not carry). The short names are given only where the
// layout is unambiguous; the rest print as "CICP n, N channels".
struct Layout { int channels; const char* name; };
inline Layout cicp_layout(int cicp) {
    switch (cicp) {
        case 1:  return {1,  "mono"};
        case 2:  return {2,  "stereo"};
        case 3:  return {3,  "3.0"};
        case 4:  return {4,  "3/1"};
        case 5:  return {5,  "5.0"};
        case 6:  return {6,  "5.1"};
        case 7:  return {8,  "7.1 front"};
        case 8:  return {2,  "dual mono"};
        case 9:  return {3,  "2/1"};
        case 10: return {4,  "2/2"};
        case 11: return {7,  "6.1"};
        case 12: return {8,  "7.1"};
        case 13: return {24, "22.2"};
        case 14: return {8,  "5.1.2"};
        case 15: return {12, nullptr};
        case 16: return {10, "5.1.4"};
        case 17: return {12, nullptr};
        case 18: return {14, nullptr};
        case 19: return {12, "7.1.4"};
        case 20: return {14, nullptr};
        default: return {0,  nullptr};
    }
}

inline std::string layout_desc(int cicp) {
    if (cicp < 0) return "reference layout not declared";
    if (cicp == 0) return "reference layout CICP 0 (not a channel configuration)";
    const Layout l = cicp_layout(cicp);
    char buf[96];
    if (l.name)
        std::snprintf(buf, sizeof buf, "reference layout CICP %d = %s, %d channel%s",
                      cicp, l.name, l.channels, l.channels == 1 ? "" : "s");
    else if (l.channels)
        std::snprintf(buf, sizeof buf, "reference layout CICP %d, %d channels",
                      cicp, l.channels);
    else
        std::snprintf(buf, sizeof buf, "reference layout CICP %d", cicp);
    return buf;
}

// One line: "MPEG-H 3D Audio in MP4 (mhm1, Baseline profile, level 4,
// reference layout CICP 13 = 22.2, 24 channels)".
inline std::string describe(const Info& m) {
    std::string s = "MPEG-H 3D Audio in " + m.container + " (";
    if (!m.sample_entry.empty()) s += m.sample_entry + ", ";
    if (m.encrypted) s += "encrypted, ";
    if (m.profile_level >= 0) s += profile_name(m.profile_level) + ", ";
    s += layout_desc(m.ref_layout) + ")";
    return s;
}

// The refusal crete prints in place of FFmpeg's.
inline std::string refusal(const std::string& filename, const Info& m) {
    return "Warning: " + filename + " is " + describe(m) + ". crete cannot "
           "measure MPEG-H: no crete build has a decoder for it, and a "
           "rendered measurement would depend on the playback layout chosen. "
           "Skipped.";
}

// ── MP4 ─────────────────────────────────────────────────────────────────────

namespace detail {

inline uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint64_t be64(const uint8_t* p) {
    return uint64_t(be32(p)) << 32 | be32(p + 4);
}
inline std::string fourcc(const uint8_t* p) {
    return std::string(reinterpret_cast<const char*>(p), 4);
}
inline bool is_mpegh_entry(const std::string& t) {
    return t == "mhm1" || t == "mhm2" || t == "mha1" || t == "mha2";
}

// One box inside a buffer: [off, off+size) with the payload after `hdr`.
struct Box { std::string type; size_t off, size, hdr; };

// Children of buf[begin, end). Stops at the first malformed header: a box
// that overruns its parent ends the walk rather than reading past it.
inline std::vector<Box> boxes(const std::vector<uint8_t>& buf, size_t begin, size_t end) {
    std::vector<Box> out;
    size_t off = begin;
    while (off + 8 <= end) {
        uint64_t size = be32(&buf[off]);
        size_t hdr = 8;
        if (size == 1) {
            if (off + 16 > end) break;
            size = be64(&buf[off + 8]);
            hdr = 16;
        } else if (size == 0) {
            size = end - off;
        }
        if (size < hdr || size > end - off) break;
        out.push_back({fourcc(&buf[off + 4]), off, static_cast<size_t>(size), hdr});
        off += static_cast<size_t>(size);
    }
    return out;
}

// By value: the list is usually a temporary from boxes().
inline std::optional<Box> child(const std::vector<Box>& v, const char* type) {
    for (const auto& b : v) if (b.type == type) return b;
    return std::nullopt;
}

// An audio sample entry (ISO/IEC 14496-12 AudioSampleEntry, version 0): 8
// bytes of SampleEntry, 20 bytes of audio fields, then child boxes.
constexpr size_t kAudioEntryFields = 28;

inline void read_mhac(const std::vector<uint8_t>& buf, const Box& mhac, Info& m) {
    const size_t p = mhac.off + mhac.hdr;
    if (p + 3 > mhac.off + mhac.size) return;
    // configurationVersion, mpegh3daProfileLevelIndication,
    // referenceChannelLayout, then mpegh3daConfigLength + the config.
    m.profile_level = buf[p + 1];
    m.ref_layout    = buf[p + 2];
}

inline void read_stsd(const std::vector<uint8_t>& buf, const Box& stsd, Info& m) {
    // FullBox (version + flags) and entry_count precede the entries.
    const size_t begin = stsd.off + stsd.hdr + 8;
    const size_t end   = stsd.off + stsd.size;
    if (begin > end) return;
    for (const auto& e : boxes(buf, begin, end)) {
        const size_t kids = e.off + e.hdr + kAudioEntryFields;
        if (kids > e.off + e.size) { ++m.other_audio; continue; }
        const auto sub = boxes(buf, kids, e.off + e.size);

        std::string fmt = e.type;
        bool enc = false;
        if (e.type == "enca") {
            // Protected entry: the original format is in sinf/frma.
            if (const auto sinf = child(sub, "sinf"))
                if (const auto frma = child(boxes(buf, sinf->off + sinf->hdr,
                                                  sinf->off + sinf->size), "frma"))
                    if (frma->size >= frma->hdr + 4) {
                        fmt = fourcc(&buf[frma->off + frma->hdr]);
                        enc = true;
                    }
        }
        if (!is_mpegh_entry(fmt)) { ++m.other_audio; continue; }
        if (m.found) continue;            // first MPEG-H track describes the file
        m.found        = true;
        m.container    = "MP4";
        m.sample_entry = fmt;
        m.encrypted    = enc;
        if (const auto mhac = child(sub, "mhaC")) read_mhac(buf, *mhac, m);
    }
}

inline void read_moov(const std::vector<uint8_t>& buf, Info& m) {
    for (const auto& trak : boxes(buf, 0, buf.size())) {
        if (trak.type != "trak") continue;
        const auto mdia = child(boxes(buf, trak.off + trak.hdr, trak.off + trak.size), "mdia");
        if (!mdia) continue;
        const auto mk = boxes(buf, mdia->off + mdia->hdr, mdia->off + mdia->size);
        // Only sound tracks: hdlr is FullBox(4) + pre_defined(4) + handler_type.
        const auto hdlr = child(mk, "hdlr");
        if (!hdlr || hdlr->size < hdlr->hdr + 12) continue;
        if (fourcc(&buf[hdlr->off + hdlr->hdr + 8]) != "soun") continue;
        const auto minf = child(mk, "minf");
        if (!minf) continue;
        const auto stbl = child(boxes(buf, minf->off + minf->hdr, minf->off + minf->size), "stbl");
        if (!stbl) continue;
        const auto stsd = child(boxes(buf, stbl->off + stbl->hdr, stbl->off + stbl->size), "stsd");
        if (stsd) read_stsd(buf, *stsd, m);
    }
}

// moov larger than this is not an audio file's.
constexpr uint64_t kMaxMoov = 64ull << 20;

inline bool identify_mp4(std::ifstream& f, uint64_t file_size, Info& m) {
    uint8_t h[16];
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(h), 8) || fourcc(h + 4) != "ftyp") return false;

    uint64_t off = 0;
    while (off + 8 <= file_size) {
        f.clear();
        f.seekg(static_cast<std::streamoff>(off));
        if (!f.read(reinterpret_cast<char*>(h), 8)) break;
        uint64_t size = be32(h);
        uint64_t hdr = 8;
        if (size == 1) {
            if (!f.read(reinterpret_cast<char*>(h + 8), 8)) break;
            size = be64(h + 8);
            hdr = 16;
        } else if (size == 0) {
            size = file_size - off;
        }
        if (size < hdr || size > file_size - off) break;
        if (fourcc(h + 4) == "moov") {
            if (size - hdr > kMaxMoov) break;
            std::vector<uint8_t> moov(static_cast<size_t>(size - hdr));
            if (!f.read(reinterpret_cast<char*>(moov.data()),
                        static_cast<std::streamsize>(moov.size()))) break;
            read_moov(moov, m);
            return true;
        }
        off += size;
    }
    return true;   // an MP4, but no moov reached: nothing to report
}

// ── MPEG-2 TS ───────────────────────────────────────────────────────────────

constexpr size_t kTsScanBytes = 4u << 20;

// 188-byte packets, or 192 with a 4-byte BDAV timestamp in front. Returns the
// packet size and the offset of the first sync byte, or 0 if neither fits.
inline size_t ts_packet_size(const std::vector<uint8_t>& b, size_t& lead) {
    for (size_t ps : {size_t(188), size_t(192)}) {
        lead = ps - 188;
        if (b.size() < lead + 5 * ps) continue;
        bool ok = true;
        for (size_t k = 0; k < 5 && ok; ++k) ok = b[lead + k * ps] == 0x47;
        if (ok) return ps;
    }
    return 0;
}

// The MPEG-H 3D Audio descriptor, inside the extension descriptor:
//   0x3F len  0x08  profile_level  interactivity(1) reserved(7)
//             reserved(2) reference_channel_layout(6)
inline void read_mpegh_descriptor(const uint8_t* d, size_t n, Info& m) {
    for (size_t i = 0; i + 2 <= n;) {
        const uint8_t tag = d[i], len = d[i + 1];
        if (i + 2 + len > n) break;
        if (tag == 0x3F && len >= 4 && d[i + 2] == 0x08) {
            m.profile_level = d[i + 3];
            m.ref_layout    = d[i + 5] & 0x3F;
        }
        i += 2 + size_t(len);
    }
}

inline bool identify_ts(std::ifstream& f, Info& m) {
    std::vector<uint8_t> b(kTsScanBytes);
    f.clear();
    f.seekg(0);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()));
    b.resize(static_cast<size_t>(f.gcount()));
    size_t lead = 0;
    const size_t ps = ts_packet_size(b, lead);
    if (!ps) return false;

    // Section reassembly per PID, for PAT (PID 0) and the PMT PIDs it names.
    std::map<int, std::vector<uint8_t>> pending;
    std::map<int, bool> pmt_pids;
    bool pmt_done = false;

    auto on_section = [&](int pid, const std::vector<uint8_t>& s) {
        if (s.size() < 3) return;
        const size_t len = (size_t(s[1] & 0x0F) << 8 | s[2]) + 3;
        if (len > s.size() || len < 12) return;
        if (pid == 0 && s[0] == 0x00) {
            // PAT: program_number(16) reserved(3) PID(13), minus the CRC.
            for (size_t i = 8; i + 4 <= len - 4; i += 4) {
                const int prog = s[i] << 8 | s[i + 1];
                const int p    = (s[i + 2] & 0x1F) << 8 | s[i + 3];
                if (prog != 0) pmt_pids[p] = true;
            }
        } else if (pmt_pids.count(pid) && s[0] == 0x02) {
            const size_t pinfo = size_t(s[10] & 0x0F) << 8 | s[11];
            for (size_t i = 12 + pinfo; i + 5 <= len - 4;) {
                const int type = s[i];
                const size_t es_len = size_t(s[i + 3] & 0x0F) << 8 | s[i + 4];
                if (i + 5 + es_len > len - 4) break;
                if (type == 0x2D) {
                    if (!m.found) {
                        m.found = true;
                        m.container = "MPEG-2 TS";
                        read_mpegh_descriptor(&s[i + 5], es_len, m);
                    }
                } else if (type == 0x03 || type == 0x04 || type == 0x0F ||
                           type == 0x11 || type == 0x81 || type == 0x87) {
                    // MPEG-1/2 audio, AAC ADTS / LATM, AC-3, E-AC-3.
                    ++m.other_audio;
                }
                i += 5 + es_len;
            }
            pmt_done = true;
        }
    };

    for (size_t off = lead; off + 188 <= b.size() && !pmt_done; off += ps) {
        const uint8_t* p = &b[off];
        if (p[0] != 0x47) break;
        const int  pid  = (p[1] & 0x1F) << 8 | p[2];
        const bool pusi = (p[1] & 0x40) != 0;
        const int  afc  = (p[3] >> 4) & 0x3;
        if (pid != 0 && !pmt_pids.count(pid)) continue;
        if (!(afc & 0x1)) continue;                       // no payload
        size_t i = 4;
        if (afc & 0x2) i += 1 + size_t(p[4]);             // adaptation field
        if (i >= 188) continue;
        auto& acc = pending[pid];
        if (pusi) {
            const size_t ptr = p[i];
            ++i;
            if (!acc.empty() && i + ptr <= 188) {          // tail of the previous
                acc.insert(acc.end(), p + i, p + i + ptr);
                on_section(pid, acc);
            }
            acc.assign(p + std::min<size_t>(i + ptr, 188), p + 188);
        } else if (!acc.empty()) {
            acc.insert(acc.end(), p + i, p + 188);
        }
        if (acc.size() >= 3) {
            const size_t need = (size_t(acc[1] & 0x0F) << 8 | acc[2]) + 3;
            if (acc.size() >= need) { on_section(pid, acc); acc.clear(); }
        }
    }
    return true;
}

}  // namespace detail

// Identify MPEG-H in an MP4 or MPEG-2 TS file, by content. found == false
// for anything else, including files that cannot be opened.
inline Info identify(const std::string& path) {
    Info m;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return m;
    const uint64_t size = static_cast<uint64_t>(f.tellg());
    if (detail::identify_mp4(f, size, m)) return m;
    detail::identify_ts(f, m);
    return m;
}

// Extensions worth sniffing. Gated by name only so a folder scan does not
// read 4 MB from the head of every WAV looking for TS packets; the verdict
// itself is always by content.
inline bool is_candidate_ext(const std::string& ext) {
    return ext == ".mp4" || ext == ".m4a" || ext == ".m4b" || ext == ".mov"
        || ext == ".ts"  || ext == ".m2ts" || ext == ".mts";
}

// Should crete refuse this file as MPEG-H? Only when MPEG-H is the ONLY
// audio in it: an MP4 or TS that also carries, say, an AAC or AC-3 track is
// left to the normal decode path, which measures what it can decode.
inline bool refuse(const Info& m) { return m.found && m.other_audio == 0; }

}  // namespace mpegh
