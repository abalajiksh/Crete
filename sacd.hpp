#pragma once
// ============================================================================
// sacd.hpp — SACD (Scarlet Book) ISO reader
//
// Zero-dependency reader for SACD disc images: master TOC, both audio areas,
// the track list, and per-channel DSD extraction. Feeds the existing
// dsd::decimate_channel path in dsd_decode.hpp — the decimation this produces
// is the same reference-validated engine every .dsf and .dff goes through, so
// an ISO and a sacd_extract'ed .dsf of the same disc must measure identically.
// That is the gate for this file, and it needs no external oracle.
//
// PROVENANCE. Written from the Scarlet Book format description. sacd_extract
// (the usual reference implementation) is GPL-2.0 and crête is MIT +
// BSD-2-Clause; the whole libdsddpcm swap existed to remove an LGPL conflict,
// so GPL code would be strictly worse than the problem that closed. Structure
// layouts are facts about a format and not copyrightable, so they are used as
// documentation; no implementation is derived. (Moot for the codec anyway:
// sacd_extract's DST decoder is #ifdef __lv2ppu__, a PS3 SPU offload, so there
// is no software DST decoder in it to derive from even if that were allowed.)
//
// ── On-disc layout ─────────────────────────────────────────────────────────
//
// Sectors are 2048 bytes and addressed as LSNs from the start of the image.
//
//   LSN 510   "SACDMTOC"  master TOC: album info + the four area-TOC LSNs
//   +area     "TWOCHTOC"  2-channel area TOC   (absent on some discs)
//   +area     "MULCHTOC"  multichannel area TOC (absent on stereo-only discs)
//   +1        "SACDTRL1"  per-area track list: start LSN + length, 255 slots
//
// Each area TOC gives its channel count, its frame format (DSD or DST) and the
// LSN range holding its audio. An area's audio sectors are NOT raw DSD: each
// 2048-byte sector carries a small header and up to 7 length-prefixed packets,
// and only the DATA_TYPE_AUDIO packets are audio. Concatenating those packets
// across an area's sectors yields its audio stream.
//
// That stream is a sequence of FRAMES, one per 1/75 s — the same grid a CD
// frame uses, which is why cue.hpp's CDDA-frame timing lines up. At DSD64 a
// frame holds 2822400/75 = 37632 bits = 4704 bytes per channel, and within
// the frame the channels are BYTE-INTERLEAVED, exactly as in DFF:
//
//     [ch0][ch1][ch0][ch1]...   = one 1/75 s frame, 4704*nch bytes
//
// MEASURED, and the obvious test for this is worthless -- read the next
// paragraph before "improving" anything here.
//
// The tempting discriminator is the L/R split: de-interleave wrongly and the
// two channels should collapse toward each other. It does not work. Both
// readings produce plausible DR and a plausible L/R difference, because a
// wrong de-interleave of a 1-bit stream still decimates to something
// music-shaped. An early version of this reader "confirmed" the WRONG layout
// that way, on data a parser bug had already corrupted.
//
// What settles it is the property DSD exists for: noise shaping. A correct
// de-interleave preserves it, a chimera destroys it. Decimated to 352.8 kHz
// and averaged over octave bands (mid-track, 2026-09-19):
//
//                          0-2k   2-15k  15-20k  24-40k  40-100k
//   byte-interleaved      25.8    -20.2   -20.4   -17.6    15.4    <- correct
//   per-channel blocks    27.5     13.7    17.7    23.2    30.4
//
// Byte-interleaved gives the textbook DSD picture: music at low frequency, a
// quiet 2-20 kHz band, and the shaped quantisation noise climbing steeply
// above 40 kHz. Per-channel blocks give a floor that rises smoothly at EVERY
// frequency with no quiet band anywhere -- broadband noise, i.e. a broken
// stream. 34 dB apart in the 2-15 kHz band, so it is not a close call once
// the right question is asked.
//
// Bit order is MSB-first, as in DFF and unlike DSF.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dst.hpp"

namespace sacd {

static constexpr size_t   SECTOR       = 2048;
static constexpr uint32_t MASTER_TOC_LSN = 510;
// Bytes per channel in one 1/75 s frame at DSD64. Every SACD is DSD64 by
// specification (sample_frequency code 4 = 64 x 44.1 kHz); there is no other
// legal value, so this is a constant rather than a computed field.
static constexpr size_t   FRAME_BYTES_PER_CH = 2822400 / 8 / 75;   // 4704
static constexpr uint32_t DSD_RATE     = 2822400;

// Packet data types from the audio sector header.
enum : unsigned { DATA_TYPE_AUDIO = 2, DATA_TYPE_SUPPLEMENTARY = 3,
                  DATA_TYPE_PADDING = 7 };

// frame_format nibble in the area TOC. NOTE the values: DST is ZERO and the
// uncompressed forms are 2 and 3, which is the opposite of the obvious
// guess -- getting this backwards silently mislabels a compressed area as
// uncompressed, so it is cross-checked against the area size in probe().
enum : unsigned { FRAME_FORMAT_DST = 0, FRAME_FORMAT_DSD_3_IN_14 = 2,
                  FRAME_FORMAT_DSD_3_IN_16 = 3 };

struct Track {
    uint32_t start_lsn  = 0;
    uint32_t length_lsn = 0;
};

struct Area {
    bool      present   = false;
    uint32_t  toc_lsn   = 0;
    unsigned  channels  = 0;
    unsigned  loudspeaker_config = 0;
    unsigned  frame_format = FRAME_FORMAT_DSD_3_IN_14;
    uint32_t  audio_start = 0;      // first audio LSN
    uint32_t  audio_end   = 0;      // last audio LSN, inclusive
    double    playtime_secs = 0.0;
    std::vector<Track> tracks;

    bool is_dst()   const { return frame_format == FRAME_FORMAT_DST; }
    bool is_stereo() const { return channels == 2; }
};

struct Disc {
    Area stereo;
    Area multichannel;
    std::string catalog;
    int  year = 0, month = 0, day = 0;

    bool any() const { return stereo.present || multichannel.present; }
};

// Which area to measure. A disc may hold one or both.
enum class AreaSel { Auto, Stereo, Multichannel };

inline const char* area_sel_name(AreaSel a) {
    switch (a) {
        case AreaSel::Stereo:       return "stereo";
        case AreaSel::Multichannel: return "multichannel";
        default:                    return "auto";
    }
}

// Throws std::runtime_error on an unknown name (caller reports and exits).
inline AreaSel area_sel_from_string(const std::string& s) {
    if (s == "auto")                            return AreaSel::Auto;
    if (s == "stereo" || s == "2ch")            return AreaSel::Stereo;
    if (s == "multichannel" || s == "multi" ||
        s == "mch" || s == "5.1")               return AreaSel::Multichannel;
    throw std::runtime_error("unknown --sacd-area '" + s +
                             "' (expected auto|stereo|multichannel)");
}

// ── Byte readers (Scarlet Book is big-endian throughout) ───────────────────
inline uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline uint32_t rd32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) <<  8) |  static_cast<uint32_t>(p[3]);
}

// ── Sector-level image access ──────────────────────────────────────────────
// An SACD image is far too large to slurp (this one is 3.88 GiB, and the
// multichannel area alone is 2.16 GiB), so unlike crête's other decoders this
// one seeks rather than taking a whole-file buffer.
class Image {
public:
    explicit Image(const std::string& path) : f_(path, std::ios::binary) {
        if (!f_) throw std::runtime_error("SACD: cannot open " + path);
        f_.seekg(0, std::ios::end);
        bytes_ = static_cast<uint64_t>(f_.tellg());
        sectors_ = bytes_ / SECTOR;
    }
    uint64_t sectors() const { return sectors_; }

    // Reads n sectors at lsn into buf. Short reads at end-of-image are an
    // error: a truncated SACD rip is common enough to name explicitly rather
    // than silently measure a prefix of.
    void read(uint32_t lsn, size_t n, std::vector<uint8_t>& buf) {
        if (static_cast<uint64_t>(lsn) + n > sectors_)
            throw std::runtime_error("SACD: read past end of image (LSN " +
                                     std::to_string(lsn) + ", image holds " +
                                     std::to_string(sectors_) + " sectors) -- "
                                     "truncated or damaged rip?");
        buf.resize(n * SECTOR);
        f_.seekg(static_cast<std::streamoff>(static_cast<uint64_t>(lsn) * SECTOR));
        f_.read(reinterpret_cast<char*>(buf.data()),
                static_cast<std::streamsize>(buf.size()));
        if (static_cast<size_t>(f_.gcount()) != buf.size())
            throw std::runtime_error("SACD: short read at LSN " + std::to_string(lsn));
    }
private:
    std::ifstream f_;
    uint64_t bytes_ = 0, sectors_ = 0;
};

// ── Cheap identification ───────────────────────────────────────────────────
// Dispatch is by CONTENT, not extension: an SACD image is conventionally
// ".iso" but so is every other disc image, and crête must not try to decode a
// data DVD as audio. Reading one sector is enough.
inline bool looks_like_sacd_iso(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(static_cast<std::streamoff>(MASTER_TOC_LSN * SECTOR));
    char id[8] = {0};
    f.read(id, 8);
    return f.gcount() == 8 && std::memcmp(id, "SACDMTOC", 8) == 0;
}

// ── Area TOC ───────────────────────────────────────────────────────────────
inline void parse_area(Image& img, uint32_t toc_lsn, const char* magic, Area& out) {
    std::vector<uint8_t> b;
    img.read(toc_lsn, 1, b);
    if (std::memcmp(b.data(), magic, 8) != 0) return;   // area absent/not there

    out.present   = true;
    out.toc_lsn   = toc_lsn;
    out.frame_format      = b[21] & 0x0F;
    out.channels          = b[32];
    out.loudspeaker_config = b[33] & 0x1F;
    out.playtime_secs     = b[64] * 60.0 + b[65] + b[66] / 75.0;
    unsigned track_count  = b[69];
    out.audio_start = rd32(&b[72]);
    out.audio_end   = rd32(&b[76]);

    if (out.channels == 0 || out.channels > 8)
        throw std::runtime_error("SACD: area declares " +
                                 std::to_string(out.channels) + " channels");

    // The track list lives in the area TOC block, conventionally one sector
    // in. Searched rather than assumed: the TOC size varies by disc (8 vs 40
    // sectors on the one measured here), so a fixed offset is not safe.
    std::vector<uint8_t> blk;
    const size_t scan = 16;
    img.read(toc_lsn, scan, blk);
    size_t at = std::string::npos;
    for (size_t s = 0; s < scan; ++s) {
        if (std::memcmp(&blk[s * SECTOR], "SACDTRL1", 8) == 0) { at = s * SECTOR; break; }
    }
    if (at == std::string::npos)
        throw std::runtime_error("SACD: no SACDTRL1 track list in area TOC");

    out.tracks.reserve(track_count);
    for (unsigned i = 0; i < track_count; ++i) {
        Track t;
        t.start_lsn  = rd32(&blk[at + 8 + 4 * i]);
        t.length_lsn = rd32(&blk[at + 8 + 4 * 255 + 4 * i]);
        if (t.length_lsn == 0) continue;            // unused slot
        out.tracks.push_back(t);
    }
}

inline Disc probe(const std::string& path) {
    Image img(path);
    std::vector<uint8_t> m;
    img.read(MASTER_TOC_LSN, 1, m);
    if (std::memcmp(m.data(), "SACDMTOC", 8) != 0)
        throw std::runtime_error("SACD: no master TOC at LSN 510 -- not an SACD image");

    Disc d;
    d.year  = rd16(&m[120]);
    d.month = m[122];
    d.day   = m[123];
    {
        const char* c = reinterpret_cast<const char*>(&m[88]);
        size_t n = 0; while (n < 16 && c[n] != '\0') ++n;
        while (n > 0 && c[n - 1] == ' ') --n;
        d.catalog.assign(c, n);
    }

    uint32_t two = rd32(&m[64]);        // 2-channel area TOC 1
    uint32_t mul = rd32(&m[72]);        // multichannel area TOC 1
    if (two) parse_area(img, two, "TWOCHTOC", d.stereo);
    if (mul) parse_area(img, mul, "MULCHTOC", d.multichannel);

    // Cross-check the frame_format nibble against the area's actual size.
    // Uncompressed DSD64 is a fixed 352800 bytes/s per channel, so an area
    // claiming to be uncompressed must be big enough to hold its own declared
    // playing time. Getting this backwards is easy (DST is 0, not 2) and the
    // consequence -- feeding DST bytes to the DSD path -- produces noise that
    // still looks like a plausible measurement, so it is checked rather than
    // trusted. See CLAUDE.md: on the DSOTM disc the arithmetic, the declared
    // max byte rate and the nibble all agree, but only after the enum was
    // read correctly.
    for (Area* a : { &d.stereo, &d.multichannel }) {
        if (!a->present || a->is_dst() || a->playtime_secs <= 0) continue;
        const double need = a->playtime_secs * 352800.0 * a->channels;
        const double have = static_cast<double>(a->audio_end - a->audio_start + 1) * SECTOR;
        if (have < need * 0.95)
            throw std::runtime_error(
                "SACD: area claims uncompressed DSD but holds only " +
                std::to_string(static_cast<long long>(have)) + " bytes for " +
                std::to_string(static_cast<long long>(need)) +
                " bytes of declared playing time -- frame_format misread, or a "
                "damaged image");
    }
    return d;
}

inline const Area& pick_area(const Disc& d, AreaSel sel) {
    switch (sel) {
        case AreaSel::Stereo:
            if (!d.stereo.present)
                throw std::runtime_error("SACD: this disc has no 2-channel area");
            return d.stereo;
        case AreaSel::Multichannel:
            if (!d.multichannel.present)
                throw std::runtime_error("SACD: this disc has no multichannel area");
            return d.multichannel;
        default:
            // Auto prefers STEREO. Deliberate: it is the layer crête's DR
            // references are built on and the layer comparable with every
            // other release of the album, and on most discs it is also the
            // uncompressed one. --sacd-area multichannel is one flag away.
            if (d.stereo.present)       return d.stereo;
            if (d.multichannel.present) return d.multichannel;
            throw std::runtime_error("SACD: image declares no audio area");
    }
}

// ── Audio extraction ───────────────────────────────────────────────────────
// Walks an LSN range, concatenating the DATA_TYPE_AUDIO packets, and splits
// the resulting frame stream into per-channel DSD (MSB-first) ready for
// dsd::decimate_channel.
//
// Handles both frame formats. An uncompressed area's frames are copied out
// directly; a DST area's are handed to dst::Decoder, which returns the same
// per-channel bytes the uncompressed form would have held -- DST is lossless,
// so downstream nothing can tell which the disc used.
inline std::vector<std::vector<uint8_t>>
read_dsd(Image& img, const Area& area, uint32_t first_lsn, uint32_t n_lsn) {
    const size_t nch = area.channels;
    const size_t frame = FRAME_BYTES_PER_CH * nch;

    std::vector<std::vector<uint8_t>> chans(nch);
    // Reserve from the sector count: the payload is ~2040 of every 2048
    // bytes, so this over-reserves by well under 1% and avoids regrowing a
    // multi-hundred-MB buffer.
    const size_t est = static_cast<size_t>(n_lsn) * SECTOR / nch;
    for (auto& c : chans) c.reserve(est);

    std::vector<uint8_t> carry;          // partial frame across sector reads
    carry.reserve(frame * 2);
    // A track's first sector rarely starts on a frame: it usually opens with
    // the tail of the previous track's last frame, and only a later packet
    // carries frame_start. Collecting from the first byte instead shifts the
    // whole track by that tail -- every frame then straddles two real frames
    // and the track gains the previous track's last few ms while losing its
    // own. It still measures plausibly (Hotel California, UDSACD 2233: tracks
    // 3, 4, 7 and 9 start 336 or 672 bytes/ch early, DR within 1e-3 dB), and
    // a tail that is not a multiple of the channel count would also rotate
    // the channels. So, like the DST path, nothing is kept before the first
    // frame_start.
    bool dsd_started = false;

    // DST state. A DST frame is variable length and delimited by the
    // frame_start flag on its first audio packet, not by a byte count, so it
    // is accumulated separately from the fixed-size uncompressed path.
    const bool is_dst = area.is_dst();
    dst::Decoder dec(static_cast<unsigned>(nch), FRAME_BYTES_PER_CH);
    std::vector<uint8_t> dstframe;
    bool dst_started = false;
    // `final` marks the flush after the last sector, where the accumulated
    // frame is usually INCOMPLETE: a track's sector range ends where the TOC
    // says, not on a 1/75 s frame boundary, so the tail is whatever part of
    // the next frame happened to fit. There is no length field to detect
    // that with -- but the decoder's bit-consumption check is exactly a
    // completeness test, since a truncated frame cannot consume its own
    // coded length. So the last frame is decoded if it decodes and dropped
    // if it does not, which mirrors the uncompressed path dropping its own
    // trailing partial frame. Mid-stream failures still propagate.
    auto flush_dst = [&](bool final_frame) {
        if (!dst_started || dstframe.empty()) return;
        try {
            dec.decode_frame(dstframe.data(), dstframe.size());
        } catch (const std::exception&) {
            dstframe.clear();
            if (final_frame) return;      // truncated tail: drop it
            throw;
        }
        for (size_t c = 0; c < nch; ++c) {
            const auto& v = dec.channel(static_cast<unsigned>(c));
            chans[c].insert(chans[c].end(), v.begin(), v.end());
        }
        dstframe.clear();
    };

    // Read in chunks rather than a sector at a time: one seek per 512 sectors
    // instead of one per sector is worth an order of magnitude on spinning
    // media and is free on SSD.
    const uint32_t CHUNK = 512;
    std::vector<uint8_t> buf;
    for (uint32_t done = 0; done < n_lsn; done += CHUNK) {
        const uint32_t n = (n_lsn - done < CHUNK) ? (n_lsn - done) : CHUNK;
        img.read(first_lsn + done, n, buf);

        for (uint32_t s = 0; s < n; ++s) {
            const uint8_t* sec = &buf[static_cast<size_t>(s) * SECTOR];
            const unsigned npkt = (sec[0] >> 5) & 7;
            const unsigned nfrm = (sec[0] >> 2) & 7;
            const bool sec_dst  = (sec[0] & 1) != 0;
            // Frame info is 3 bytes on a DSD sector and 4 on a DST one --
            // the 4th byte carries the channel/sector-count bits and is only
            // filled when DST-encoded. MEASURED over 3000 sectors of each
            // area: every observed (npkt, nfrm) shape matches
            // 1 + 2n + 3f on the DSD area and 1 + 2n + 4f on the DST area,
            // with no exceptions. Reading 4 unconditionally overruns the
            // first packet by one byte per frame-start sector, which drops
            // that sector's whole audio packet through the bounds check
            // below and silently loses ~14% of the track (58.65 s of a
            // 68.45 s track, when this was first written that way).
            // Taken from the SECTOR's own dst bit, not the area's, so a
            // mixed image cannot desynchronise it.
            size_t off = 1 + 2 * npkt + (sec_dst ? 4 : 3) * nfrm;

            for (unsigned p = 0; p < npkt; ++p) {
                const uint16_t pi = rd16(&sec[1 + 2 * p]);
                const unsigned fstart = (pi >> 15) & 1;
                const unsigned dtype = (pi >> 11) & 7;
                const size_t   plen  = pi & 0x7FF;
                if (off + plen > SECTOR) break;      // malformed; skip the rest
                if (dtype == DATA_TYPE_AUDIO) {
                    if (is_dst) {
                        if (fstart) { flush_dst(false); dst_started = true; }
                        if (dst_started)
                            dstframe.insert(dstframe.end(), sec + off, sec + off + plen);
                    } else {
                        if (fstart) {
                            // Frames drain only at sector ends, so a frame
                            // start must find the carry on a frame boundary.
                            // Anything else means the packet walk has lost
                            // sync, which would measure, not fail.
                            if (dsd_started && carry.size() % frame != 0)
                                throw std::runtime_error(
                                    "SACD: frame start " +
                                    std::to_string(carry.size() % frame) +
                                    " bytes into a frame -- packet walk out of sync");
                            dsd_started = true;
                        }
                        if (dsd_started)
                            carry.insert(carry.end(), sec + off, sec + off + plen);
                    }
                }
                off += plen;
            }
            if (is_dst) continue;

            // Drain whole frames out of the carry buffer, de-blocking each
            // into its per-channel run.
            size_t pos = 0;
            while (carry.size() - pos >= frame) {
                const uint8_t* fr = carry.data() + pos;
                // Byte-interleaved across channels -- see the header note on
                // why the L/R test does not establish this and the spectral
                // one does.
                for (size_t c = 0; c < nch; ++c) {
                    auto& out = chans[c];
                    const size_t base = out.size();
                    out.resize(base + FRAME_BYTES_PER_CH);
                    for (size_t i = 0; i < FRAME_BYTES_PER_CH; ++i)
                        out[base + i] = fr[i * nch + c];
                }
                pos += frame;
            }
            if (pos) carry.erase(carry.begin(),
                                 carry.begin() + static_cast<std::ptrdiff_t>(pos));
        }
    }
    // The final DST frame has no following frame_start to flush it.
    flush_dst(true);
    // A trailing partial frame is dropped: it is less than 1/75 s and cannot
    // be split across channels without inventing data.
    return chans;
}

// One track of an area.
inline std::vector<std::vector<uint8_t>>
read_track(Image& img, const Area& area, size_t track_index) {
    if (track_index >= area.tracks.size())
        throw std::runtime_error("SACD: track index out of range");
    const Track& t = area.tracks[track_index];
    return read_dsd(img, area, t.start_lsn, t.length_lsn);
}

} // namespace sacd
