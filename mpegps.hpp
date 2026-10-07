// mpegps.hpp -- MPEG program stream identification (zero dependency).
//
// IDENTIFICATION, NOT DECODE. This reads the structure of an MPEG-1 system
// stream or an MPEG-2 program stream -- packs, the system header, PES packets
// and what each elementary stream is -- and decodes no audio or video. crete
// measures DVD audio through FFmpeg (crete-ffmpeg); this header exists so that
// every build can say precisely what a .vob / .aob / .mpg holds, and so that
// a stream is named by the ID the disc gives it rather than by FFmpeg's index.
//
// Why the ID. FFmpeg numbers streams in order of first appearance, which is
// not stable across fragments of one DVD title (dvd.hpp: on the DSOTM 50th DVD
// index 2 is 0x80 AC-3 in VTS_02_1 and 0xA2 LPCM in VTS_02_2). The stream ID
// is what the disc's own tables use and does not move.
//
// SOURCES.
//   ITU-T H.222.0 | ISO/IEC 13818-1 (draft N0801, 1994): pack header
//   (Table 2-33: '01' marker, pack_stuffing_length), system header (Table
//   2-34), PES packet (Table 2-18: the '10' marker, PES_header_data_length),
//   stream_id assignments (Table 2-19: 0xBC map, 0xBD private_stream_1,
//   0xBE padding, 0xBF private_stream_2, 110x xxxx MPEG audio, 1110 xxxx
//   video).
//   ISO/IEC 11172-1 (MPEG-1 systems), as widely documented: a pack header
//   whose first bits are '0010' and is 12 bytes long, and a packet header of
//   stuffing bytes, an optional STD buffer field ('01') and a PTS / PTS+DTS /
//   0x0F byte.
//   ISO/IEC 13818-2 / 11172-2 video: sequence_header_code 0x000001B3 (size,
//   frame_rate_code) and, in MPEG-2 video only, the sequence_extension that
//   follows it (0x000001B5, extension_start_code_identifier 1).
//   DVD private_stream_1 SUB-STREAM IDs (the first payload byte: 0x20-0x3F
//   subpicture, 0x80-0x87 AC-3, 0x88-0x8F DTS, 0x90-0x97 SDDS, 0xA0-0xAF
//   linear PCM, and MLP on DVD-Audio) are NOT in H.222.0: they come from the
//   licensed DVD specification and are community-sourced here. They were
//   checked against ffprobe on the corpus below rather than taken on trust,
//   and MLP is told from LPCM by the MLP major sync in the payload, not by
//   the sub-stream number.
//
// ORACLE. ffprobe's stream list (ID and type) on every VOB / AOB of three
// DVD-Video discs and one DVD-Audio disc, and on synthetic streams built by
// scripts/check_mpegps.sh.

#pragma once

#include "dvd.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace mpegps {

enum class Kind { Audio, Video, Subpicture, Navigation, Data };

struct Stream {
    int      stream_id = 0;     // PES stream_id (0xBD for private_stream_1)
    int      sub_id    = -1;    // private_stream_1 sub-stream ID, else -1
    Kind     kind      = Kind::Data;
    std::string codec;          // "AC-3", "MPEG-2 video", "MPEG-1 Layer II", ...
    uint64_t packets   = 0;     // PES packets in the scanned range
    uint64_t bytes     = 0;     // PES payload bytes in the scanned range

    // Video, from the sequence header.
    int width = 0, height = 0, frame_rate_code = 0;

    // The ID the disc uses for this stream: the sub-stream ID for
    // private_stream_1, else the stream_id. "0x80", "0xE0".
    int id() const { return sub_id >= 0 ? sub_id : stream_id; }

    // The same, spelled as FFmpeg's AVStream::id / ffprobe's "[0x...]":
    // the sub-stream ID, or 0x100 | stream_id (the full start code).
    int ffmpeg_id() const { return sub_id >= 0 ? sub_id : 0x100 | stream_id; }

    // Header bytes kept until the codec is settled (video sequence header,
    // MPEG audio frame header, MLP major sync). Not reported.
    std::vector<uint8_t> head;
    uint64_t searched = 0;
    bool settled = false;
};

struct Info {
    bool     is_ps    = false;  // at least one pack header was read
    int      mpeg     = 0;      // 1 = MPEG-1 system stream, 2 = MPEG-2 PS
    uint64_t packs    = 0;
    uint64_t scanned  = 0;      // bytes read
    uint64_t total    = 0;      // bytes in the input (all parts)
    bool     reached_end = false;   // scanned to the end, not stopped by max_bytes
    bool     aligned_2048 = true;   // every pack started on a 2048-byte boundary
    bool     system_header = false;
    int      audio_bound = -1, video_bound = -1;
    bool     has_psm   = false; // program_stream_map present
    uint64_t junk      = 0;     // bytes skipped to regain sync
    // private_stream_1 packets whose sub-stream ID is in no DVD range. FFmpeg
    // makes no stream of these and neither does this: on a Last Waltz VOB the
    // one such packet is 0xFF filler from end to end.
    uint64_t unknown_private = 0;
    std::vector<Stream> streams;    // ordered by id()

    bool has_video() const {
        for (const auto& s : streams) if (s.kind == Kind::Video) return true;
        return false;
    }
    std::vector<const Stream*> audio() const {
        std::vector<const Stream*> v;
        for (const auto& s : streams) if (s.kind == Kind::Audio) v.push_back(&s);
        return v;
    }
};

// ── Names ───────────────────────────────────────────────────────────────────

inline std::string hex_id(int id) {
    char b[8];
    std::snprintf(b, sizeof b, "0x%02X", id);
    return b;
}

// frame_rate_code (13818-2 Table 6-4; 11172-2 uses the same codes 1-8).
inline const char* frame_rate(int code) {
    static const char* t[] = {nullptr, "23.976", "24", "25", "29.97",
                              "30", "50", "59.94", "60"};
    return (code >= 1 && code <= 8) ? t[code] : nullptr;
}

// "MPEG-2 video 720x480 29.97 fps", "AC-3", "subpicture".
inline std::string stream_desc(const Stream& s) {
    std::string d = s.codec;
    if (s.kind == Kind::Video && s.width) {
        d += " " + std::to_string(s.width) + "x" + std::to_string(s.height);
        if (const char* fr = frame_rate(s.frame_rate_code)) d += std::string(" ") + fr + " fps";
    }
    return d;
}

inline const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Audio:      return "audio";
        case Kind::Video:      return "video";
        case Kind::Subpicture: return "subpicture";
        case Kind::Navigation: return "navigation";
        default:               return "data";
    }
}

// What the input is, from its NAME where the DVD layout gives it one (the
// disc's directory structure is authoritative for that) and from its content
// otherwise.
inline std::string carrier(const std::string& path, const Info& m) {
    const std::string name = std::filesystem::path(path).filename().string();
    std::string key; int part;
    if (dvd::parse_fragment(name, key, part))
        return (key.compare(0, 3, "VTS") == 0 ? "DVD-Video title set " : "DVD-Audio title set ") + key;
    if (dvd::is_menu(name)) return "DVD menu (" + name + ")";
    if (m.mpeg == 1) return "MPEG-1 system stream";
    if (m.mpeg == 2) return "MPEG-2 program stream";
    return "not an MPEG program stream";
}

// One line: "MPEG-2 program stream; MPEG-2 video 720x480 29.97 fps; audio
// 0x80 AC-3, 0x81 AC-3, 0xA2 LPCM; 2 subpicture".
inline std::string summary(const Info& m) {
    std::string s = m.mpeg == 1 ? "MPEG-1 system stream" : "MPEG-2 program stream";
    for (const auto& st : m.streams)
        if (st.kind == Kind::Video) s += "; " + stream_desc(st);
    const auto a = m.audio();
    if (a.empty()) s += "; no audio";
    for (size_t i = 0; i < a.size(); ++i)
        s += (i ? ", " : "; audio ") + hex_id(a[i]->id()) + " " + a[i]->codec;
    size_t subs = 0;
    for (const auto& st : m.streams) subs += st.kind == Kind::Subpicture;
    if (subs) s += "; " + std::to_string(subs) + " subpicture";
    return s;
}

// ── Scanner ─────────────────────────────────────────────────────────────────

namespace detail {

// Reads several files as one byte stream: a DVD title set is split at a pack
// boundary that need not be a PACKET boundary, so a packet may straddle two
// fragments and must be read across the join.
class Reader {
public:
    explicit Reader(const std::vector<std::string>& parts) : parts_(parts) {}

    // Make at least n bytes available at cur(); false if the input ends first.
    bool need(size_t n) {
        while (buf_.size() - pos_ < n) {
            if (pos_ > 0) {
                buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
                base_ += pos_;
                pos_ = 0;
            }
            if (!read_more()) return false;
        }
        return true;
    }
    const uint8_t* cur() const { return buf_.data() + pos_; }
    size_t avail() const { return buf_.size() - pos_; }
    void skip(size_t n) { pos_ += n; }
    uint64_t offset() const { return base_ + pos_; }

private:
    bool read_more() {
        constexpr size_t kChunk = 1u << 20;
        while (idx_ < parts_.size()) {
            if (!f_.is_open()) {
                f_.open(parts_[idx_], std::ios::binary);
                if (!f_) { ++idx_; f_.clear(); continue; }
            }
            const size_t old = buf_.size();
            buf_.resize(old + kChunk);
            f_.read(reinterpret_cast<char*>(buf_.data() + old), kChunk);
            const size_t got = static_cast<size_t>(f_.gcount());
            buf_.resize(old + got);
            if (got > 0) return true;
            f_.close();
            f_.clear();
            ++idx_;
        }
        return false;
    }

    std::vector<std::string> parts_;
    size_t idx_ = 0;
    std::ifstream f_;
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    uint64_t base_ = 0;
};

inline bool start_code(const uint8_t* p) { return p[0] == 0 && p[1] == 0 && p[2] == 1; }

// Payload bytes searched for a header before a stream keeps its default
// name. A fragment that starts mid-title meets its first video sequence
// header only at the next GOP -- more than 256 KB into the video on the Last
// Waltz VOBs -- so this is generous; the search window itself stays small.
constexpr uint64_t kGiveUp = 64ull << 20;

// The DVD sub-stream ID ranges stream_for() names. Anything else is not
// made a stream (Info::unknown_private).
inline bool known_sub_id(int sub) {
    return (sub >= 0x20 && sub <= 0x3F) || (sub >= 0x80 && sub <= 0x97) ||
           (sub >= 0xA0 && sub <= 0xAF);
}

inline Stream& stream_for(std::map<int, Stream>& m, int stream_id, int sub_id) {
    const int key = sub_id >= 0 ? sub_id : 0x100 | stream_id;
    auto it = m.find(key);
    if (it != m.end()) return it->second;
    Stream s;
    s.stream_id = stream_id;
    s.sub_id    = sub_id;
    if (stream_id == 0xBF) {
        // Named by scan(), which sees the packet shape.
        s.codec = "private_stream_2";
        s.settled = true;
    } else if ((stream_id & 0xF0) == 0xE0) {
        s.kind = Kind::Video;
        s.codec = "video";
    } else if ((stream_id & 0xE0) == 0xC0) {
        s.kind = Kind::Audio;
        s.codec = "MPEG audio";
    } else if (stream_id == 0xBD && sub_id >= 0) {
        // DVD sub-stream ranges: community-sourced, see the header comment.
        s.settled = true;
        if (sub_id >= 0x20 && sub_id <= 0x3F)      { s.kind = Kind::Subpicture; s.codec = "subpicture"; }
        else if (sub_id >= 0x80 && sub_id <= 0x87) { s.kind = Kind::Audio; s.codec = "AC-3"; }
        else if (sub_id >= 0x88 && sub_id <= 0x8F) { s.kind = Kind::Audio; s.codec = "DTS"; }
        else if (sub_id >= 0x90 && sub_id <= 0x97) { s.kind = Kind::Audio; s.codec = "SDDS"; }
        else if (sub_id >= 0xA0 && sub_id <= 0xAF) { s.kind = Kind::Audio; s.codec = "LPCM"; s.settled = false; }
    } else {
        s.codec = "stream_id " + hex_id(stream_id);
        s.settled = true;
    }
    return m.emplace(key, std::move(s)).first->second;
}

// Settle a stream's codec from the payload gathered so far. Only a short
// tail is kept between calls (a sync word may straddle two packets), except
// that a video sequence header, once found, is kept until the start code
// after it arrives. `final` is set once the scan ends, when whatever is
// known has to do.
inline void settle(Stream& s, bool final) {
    if (s.settled) return;
    auto& h = s.head;
    size_t keep = h.size() >= 3 ? h.size() - 3 : 0;   // erase h[0, keep) afterwards

    if (s.kind == Kind::Video) {
        for (size_t i = 0; i + 4 <= h.size(); ++i) {
            if (!(start_code(&h[i]) && h[i + 3] == 0xB3)) continue;
            keep = i;
            if (i + 12 > h.size()) break;
            s.width  = (h[i + 4] << 4) | (h[i + 5] >> 4);
            s.height = ((h[i + 5] & 0x0F) << 8) | h[i + 6];
            s.frame_rate_code = h[i + 7] & 0x0F;
            // The next start code after the header (and its optional
            // quantiser matrices) is the sequence_extension in MPEG-2 video,
            // and is absent from MPEG-1 video.
            for (size_t j = i + 12; j + 4 < h.size(); ++j) {
                if (!start_code(&h[j])) continue;
                s.codec = (h[j + 3] == 0xB5 && (h[j + 4] >> 4) == 1) ? "MPEG-2 video"
                                                                    : "MPEG-1 video";
                s.settled = true;
                break;
            }
            if (!s.settled && final) { s.codec = "MPEG video"; s.settled = true; }
            break;
        }
    } else if ((s.stream_id & 0xE0) == 0xC0) {
        for (size_t i = 0; i + 4 <= h.size(); ++i) {
            if (h[i] != 0xFF || (h[i + 1] & 0xE0) != 0xE0) continue;
            const int ver   = (h[i + 1] >> 3) & 3;   // 3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5
            const int layer = (h[i + 1] >> 1) & 3;   // 3 I, 2 II, 1 III
            if (ver == 1) continue;                  // reserved
            if (layer == 0) {
                if ((h[i + 1] & 0xF6) == 0xF0) { s.codec = "AAC (ADTS)"; s.settled = true; break; }
                continue;
            }
            static const char* L[] = {"", "Layer III", "Layer II", "Layer I"};
            s.codec = std::string(ver == 3 ? "MPEG-1 " : ver == 2 ? "MPEG-2 " : "MPEG-2.5 ") + L[layer];
            s.settled = true;
            break;
        }
    } else if (s.stream_id == 0xBD && s.sub_id >= 0xA0 && s.sub_id <= 0xAF) {
        // MLP major sync F8 72 6F BB (TrueHD's is ...BA). Its presence is
        // what makes this MLP; the sub-stream number alone is not trusted.
        for (size_t i = 0; i + 4 <= h.size(); ++i)
            if (h[i] == 0xF8 && h[i + 1] == 0x72 && h[i + 2] == 0x6F &&
                (h[i + 3] == 0xBB || h[i + 3] == 0xBA)) {
                s.codec = h[i + 3] == 0xBB ? "MLP" : "TrueHD";
                s.settled = true;
                break;
            }
    }
    if (!s.settled && (final || s.searched >= kGiveUp)) s.settled = true;   // keep the default name
    if (s.settled) { h.clear(); h.shrink_to_fit(); }
    else h.erase(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(keep));
}

}  // namespace detail

// Scan up to `max_bytes` (0 = all) of `parts` read as one stream.
inline Info scan(const std::vector<std::string>& parts, uint64_t max_bytes = 0) {
    Info m;
    std::error_code ec;
    for (const auto& p : parts) {
        const auto n = std::filesystem::file_size(p, ec);
        if (!ec) m.total += n;
    }
    detail::Reader r(parts);
    std::map<int, Stream> streams;
    bool ended = false;          // a program_end_code since the last pack

    // A program stream starts with a pack header. Anything else is not one,
    // and is not scanned: the resync below is for damage inside a stream,
    // not for finding a stream inside an arbitrary file.
    if (!r.need(4) || !(detail::start_code(r.cur()) && r.cur()[3] == 0xBA)) {
        m.scanned = 0;
        return m;
    }

    for (;;) {
        if (max_bytes != 0 && r.offset() >= max_bytes) break;
        if (!r.need(4)) { m.reached_end = true; break; }
        const uint8_t* p = r.cur();
        if (!detail::start_code(p) || p[3] < 0xB9) {
            // Lost sync: find the next pack start.
            size_t skipped = 0;
            for (;;) {
                if (!r.need(4)) { m.reached_end = true; break; }
                p = r.cur();
                if (detail::start_code(p) && p[3] == 0xBA) break;
                r.skip(1);
                ++skipped;
            }
            if (!ended) m.junk += skipped;
            continue;
        }
        const uint8_t code = p[3];

        if (code == 0xBA) {                                   // pack header
            if (!r.need(14)) { m.reached_end = true; break; }
            p = r.cur();
            size_t len;
            if ((p[4] & 0xC0) == 0x40)      { len = 14 + (p[13] & 0x07); if (!m.mpeg) m.mpeg = 2; }
            else if ((p[4] & 0xF0) == 0x20) { len = 12;                  if (!m.mpeg) m.mpeg = 1; }
            else { r.skip(4); m.junk += 4; continue; }
            if (r.offset() % 2048 != 0) m.aligned_2048 = false;
            ++m.packs;
            ended = false;
            m.is_ps = true;
            if (!r.need(len)) { m.reached_end = true; break; }
            r.skip(len);
            continue;
        }
        if (code == 0xB9) {                                   // program end
            // What follows an end code up to the next pack is stuffing (0xFF
            // to the sector's end on a DVD still-video object), not damage.
            r.skip(4);
            ended = true;
            continue;
        }

        if (!r.need(6)) { m.reached_end = true; break; }
        p = r.cur();
        const size_t plen = 6 + (size_t(p[4]) << 8 | p[5]);
        if (!r.need(plen)) { m.reached_end = true; break; }
        p = r.cur();

        if (code == 0xBB) {                                   // system header
            if (plen >= 12) {
                m.system_header = true;
                m.audio_bound = p[9] >> 2;
                m.video_bound = p[10] & 0x1F;
            }
        } else if (code == 0xBC) {
            m.has_psm = true;
        } else if (code == 0xBE) {
            // padding
        } else if (code == 0xBF) {
            // A DVD-Video navigation pack carries two private_stream_2
            // packets: PCI (substream 0x00, 980 bytes) and DSI (0x01, 1018
            // bytes). Only that shape is called navigation; DVD-Audio's
            // still-video set uses private_stream_2 for something else
            // (745-byte packets, first byte 0x02, on Rumours' AUDIO_SV.VOB).
            auto& s = detail::stream_for(streams, code, -1);
            ++s.packets;
            s.bytes += plen - 6;
            if (plen > 6 && ((plen - 6 == 980 && p[6] == 0x00) || (plen - 6 == 1018 && p[6] == 0x01))) {
                s.kind  = Kind::Navigation;
                s.codec = "DVD navigation (PCI/DSI)";
            }
        } else {
            // PES with a header: MPEG-2 syntax ('10' marker) or MPEG-1's.
            size_t i = 6;
            bool ok = true;
            if (plen > 8 && (p[6] & 0xC0) == 0x80) {
                i = 9 + size_t(p[8]);
            } else {
                while (i < plen && p[i] == 0xFF && i < 6 + 16) ++i;      // stuffing
                if (i < plen && (p[i] & 0xC0) == 0x40) i += 2;           // STD buffer
                if (i < plen && (p[i] & 0xF0) == 0x20) i += 5;           // PTS
                else if (i < plen && (p[i] & 0xF0) == 0x30) i += 10;     // PTS + DTS
                else if (i < plen && p[i] == 0x0F) i += 1;
                else ok = false;
            }
            if (ok && i <= plen) {
                int sub = -1;
                if (code == 0xBD) {
                    if (i < plen) sub = p[i];
                    else ok = false;
                    if (ok && !detail::known_sub_id(sub)) { ++m.unknown_private; ok = false; }
                }
                if (ok) {
                    auto& s = detail::stream_for(streams, code, sub);
                    ++s.packets;
                    s.bytes += plen - i;
                    if (!s.settled) {
                        s.head.insert(s.head.end(), p + i, p + plen);
                        s.searched += plen - i;
                        detail::settle(s, false);
                    }
                }
            }
        }
        r.skip(plen);
    }
    m.scanned = m.reached_end ? m.total : r.offset();

    for (auto& kv : streams) {
        detail::settle(kv.second, true);
        m.streams.push_back(std::move(kv.second));
    }
    std::sort(m.streams.begin(), m.streams.end(),
              [](const Stream& a, const Stream& b) { return a.id() < b.id(); });
    return m;
}

// ── What crete records on a measured track ─────────────────────────────────

// How a track measured from a program stream was carried, and which of the
// stream's audio streams it is. Descriptive only: no measured value depends
// on any of it.
struct Carriage {
    bool        present   = false;
    std::string carrier;            // carrier(): "DVD-Video title set VTS_02"
    size_t      fragments = 1;
    int         mpeg      = 0;      // 1 or 2
    std::string video;              // "MPEG-2 video 720x480 29.97 fps", "" if none
    std::vector<std::pair<int, std::string>> audio;   // (ID, codec), by ID
    int         measured  = -1;     // ID of the measured stream, -1 unknown
    uint64_t    scanned   = 0;      // bytes the stream list was read from
    bool        complete  = false;  // ... which was the whole input
};

// How much of a title set is read to describe it. Every audio stream of a
// DVD title is interleaved from its first VOBU, so the head describes the
// whole; JSON says how much was read.
constexpr uint64_t kDescribeBytes = 32ull << 20;

inline Carriage carriage(const std::string& path, const std::vector<std::string>& parts,
                         const Info& m) {
    Carriage c;
    c.present   = m.is_ps;
    c.carrier   = carrier(path, m);
    c.fragments = parts.size();
    c.mpeg      = m.mpeg;
    for (const auto& s : m.streams) {
        if (s.kind == Kind::Video && c.video.empty()) c.video = stream_desc(s);
        if (s.kind == Kind::Audio) c.audio.emplace_back(s.id(), s.codec);
    }
    c.scanned  = m.scanned;
    c.complete = m.reached_end;
    return c;
}

// "DVD-Video title set VTS_02, 3 fragments · MPEG-2 video 720x480 29.97 fps"
inline std::string carriage_line(const Carriage& c) {
    std::string s = c.carrier;
    if (c.fragments > 1) s += ", " + std::to_string(c.fragments) + " fragments";
    s += c.video.empty() ? " · no video" : " · " + c.video;
    return s;
}

// "0xA2 LPCM (of 0x80 AC-3, 0x81 AC-3, 0xA2 LPCM)"; just "0xA2 LPCM" when
// it is the only one; "" when the measured stream's ID is not known.
inline std::string measured_line(const Carriage& c) {
    std::string self, all;
    for (const auto& a : c.audio) {
        const std::string one = hex_id(a.first) + " " + a.second;
        if (a.first == c.measured) self = one;
        all += (all.empty() ? "" : ", ") + one;
    }
    if (self.empty()) return {};
    return c.audio.size() > 1 ? self + " (of " + all + ")" : self;
}

// The warning a build that cannot measure this input prints instead.
// `ffmpeg_tier`: whether this build decodes DVD .vob / .aob at all.
inline std::string refusal(const std::string& path, const std::vector<std::string>& parts,
                           bool ffmpeg_tier) {
    const Info m = scan(parts, kDescribeBytes);
    const std::string name = std::filesystem::path(path).filename().string();
    std::string s = "Warning: " + name + ": ";
    const std::string what = carrier(path, m);
    if (what != (m.mpeg == 1 ? "MPEG-1 system stream" : "MPEG-2 program stream")) {
        s += what;
        if (parts.size() > 1) s += ", " + std::to_string(parts.size()) + " fragments";
        s += " -- ";
    }
    s += summary(m) + ". Not measured: ";
    const std::string ext = dvd::upper(std::filesystem::path(path).extension().string());
    if ((ext == ".VOB" || ext == ".AOB") && !ffmpeg_tier)
        s += "this build has no decoder for DVD audio; crete-ffmpeg measures it.";
    else
        s += "crete reads program streams only from DVD .vob / .aob files.";
    return s;
}

// Is this file a program stream? Reads 14 bytes.
inline bool looks_like_ps(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    uint8_t b[14];
    if (!f.read(reinterpret_cast<char*>(b), sizeof b)) return false;
    return detail::start_code(b) && b[3] == 0xBA &&
           ((b[4] & 0xC0) == 0x40 || (b[4] & 0xF0) == 0x20);
}

inline bool is_candidate_ext(const std::string& ext) {
    return ext == ".vob" || ext == ".aob" || ext == ".mpg" || ext == ".mpeg";
}

}  // namespace mpegps
