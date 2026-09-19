#pragma once
// ============================================================================
// audio.hpp — Zero-dependency audio file decoders
//
// Built-in support for: WAV, AIFF/AIFF-C, FLAC, DSF (DSD)
// ALAC (.m4a) requires an MP4 container parser — not yet implemented.
//
// All decoders produce interleaved f64 samples in [-1.0, 1.0], then
// deinterleave into per-channel vectors for analysis.
// ============================================================================

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <exception>
#include <thread>
#include <vector>

#include "dsd_decode.hpp"
#include "sacd.hpp"

namespace audio {

// ── Audio buffer returned by all decoders ──────────────────────────────────
struct AudioData {
    std::vector<std::vector<double>> channels;  // per-channel samples [-1,1]
    uint32_t sample_rate = 0;
    uint32_t bit_depth   = 0;
    std::string codec;

    // Which audio stream this came from, for containers that carry several.
    // A music Blu-ray rip routinely holds six (LPCM / DTS-HD MA / TrueHD /
    // AC-3 stereo and surround mixes of the same album), and FFmpeg's
    // av_find_best_stream() silently picks ONE by bitrate and channel count
    // -- not necessarily the default-disposition track. Reporting the choice
    // is what makes it auditable; --stream is what makes it selectable.
    // stream_index is the container's own index (matching ffprobe's index=),
    // or -1 for a native decode, where the file IS the stream.
    int stream_index        = -1;
    int audio_stream_count  = 1;

    // The codec's own profile string, where the bitstream carries one --
    // "Dolby TrueHD + Dolby Atmos", "Dolby Digital Plus + Dolby Atmos",
    // "DTS-HD MA", "DTS-X". Empty for every native decode and for any codec
    // whose stream declares no profile.
    //
    // This is IDENTIFICATION, not decode. FFmpeg has no Atmos or DTS:X
    // renderer: it decodes the underlying BED (TrueHD 7.1, or the E-AC-3 5.1
    // core for JOC) and discards the object metadata. So crete's numbers for
    // such a file are the bed's numbers, which is a real and complete
    // measurement of what a non-immersive playback chain delivers -- but an
    // unqualified "crete measures Atmos" would be false, and that is exactly
    // what this field exists to prevent. Same precedent as mqa_detect.hpp:
    // measure the delivered stream, name the premium layer separately.
    std::string codec_profile;

    size_t num_channels() const { return channels.size(); }
    size_t num_samples()  const { return channels.empty() ? 0 : channels[0].size(); }
};

// ── File reading utilities ─────────────────────────────────────────────────
inline std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open: " + path);
    auto sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

// Little-endian readers
inline uint16_t rd16le(const uint8_t* p) { return p[0] | (p[1]<<8); }
inline uint32_t rd32le(const uint8_t* p) { return p[0]|(p[1]<<8)|(p[2]<<16)|(p[3]<<24); }
inline uint64_t rd64le(const uint8_t* p) {
    return static_cast<uint64_t>(rd32le(p)) | (static_cast<uint64_t>(rd32le(p+4))<<32);
}

// Big-endian readers
inline uint16_t rd16be(const uint8_t* p) { return (p[0]<<8)|p[1]; }
inline uint32_t rd32be(const uint8_t* p) { return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }
inline uint64_t rd64be(const uint8_t* p) {
    return (static_cast<uint64_t>(rd32be(p))<<32) | rd32be(p+4);
}

// Convert 80-bit extended float (big-endian) to double — used by AIFF
inline double extended_to_double(const uint8_t* p) {
    int sign = (p[0] >> 7) & 1;
    int exponent = ((p[0] & 0x7F) << 8) | p[1];
    uint64_t mantissa = 0;
    for (int i = 0; i < 8; ++i)
        mantissa = (mantissa << 8) | p[2+i];

    if (exponent == 0 && mantissa == 0) return 0.0;
    double f = static_cast<double>(mantissa) / (1ULL << 63);
    f = std::ldexp(f, exponent - 16383);
    return sign ? -f : f;
}

// ── String helpers ─────────────────────────────────────────────────────────
inline std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

inline std::string get_extension(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    return to_lower(path.substr(dot));
}

// ════════════════════════════════════════════════════════════════════════════
// WAV / RF64 / BW64 / Wave64 Decoder
// ════════════════════════════════════════════════════════════════════════════
//
// Three containers, one payload. Plain RIFF carries 32-bit chunk sizes and so
// stops dead at 4 GB — which is not an exotic limit for this tool's own
// material: extracting the 43-minute DSOTM LPCM 5.1 96 kHz/24 stream produces
// 4.47 GB, and long high-rate multichannel captures are RF64 or Wave64 in
// practice for exactly that reason.
//
//   RIFF    "RIFF" u32 size  "WAVE"   chunks: id[4] + u32 size, pad to 2
//   RF64    "RF64" 0xFFFFFFFF "WAVE"  the FIRST chunk must be "ds64", holding
//   BW64    "BW64" 0xFFFFFFFF "WAVE"  u64 riffSize / dataSize / sampleCount
//                                     plus a table of 64-bit sizes for any
//                                     other oversized chunk. A chunk whose
//                                     32-bit size reads 0xFFFFFFFF takes its
//                                     real size from there. (EBU Tech 3306;
//                                     BW64 is ITU-R BS.2088's name for the
//                                     same structure, so both magics parse
//                                     identically.)
//   Wave64  16-byte GUIDs instead of 4CCs and u64 sizes throughout. Each
//           chunk GUID is the RIFF fourCC followed by one fixed 12-byte
//           suffix, so the chunk BODIES are byte-for-byte RIFF's — only the
//           framing differs. Sizes INCLUDE the 24-byte chunk header and each
//           chunk pads to an 8-byte boundary.

// Wave64 GUIDs. The chunk and form GUIDs share a suffix; the outer file GUID
// does not, which is what makes it a reliable magic number.
inline constexpr uint8_t W64_GUID_SUFFIX[12] = {
    0xF3,0xAC,0xD3,0x11,0x8C,0xD1,0x00,0xC0,0x4F,0x8E,0xDB,0x8A };
inline constexpr uint8_t W64_RIFF_GUID[16] = {
    'r','i','f','f',0x2E,0x91,0xCF,0x11,0xA5,0xD6,0x28,0xDB,0x04,0xC1,0x00,0x00 };

inline bool w64_id_is(const uint8_t* p, const char* fourcc) {
    return std::memcmp(p, fourcc, 4) == 0 &&
           std::memcmp(p + 4, W64_GUID_SUFFIX, 12) == 0;
}

// The shared tail. The three containers differ only in how the fmt and data
// chunks are FOUND, so one payload decoder is what keeps them from ever
// drifting apart on bit depth, channel count or float handling — and is why a
// plain RIFF file decodes byte-identically to before this was split out.
inline AudioData decode_wave_payload(const uint8_t* fmt, size_t fmt_size,
                                     const uint8_t* data_ptr, size_t data_size,
                                     const char* codec) {
    AudioData ad;
    ad.codec = codec;

    uint16_t audio_fmt = 0;
    uint16_t n_channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    uint16_t block_align [[maybe_unused]] = 0;

    if (fmt && fmt_size >= 16) {
        audio_fmt      = rd16le(fmt);
        n_channels     = rd16le(fmt + 2);
        sample_rate    = rd32le(fmt + 4);
        block_align    = rd16le(fmt + 12);
        bits_per_sample = rd16le(fmt + 14);

        // WAVE_FORMAT_EXTENSIBLE
        if (audio_fmt == 0xFFFE && fmt_size >= 40) {
            bits_per_sample = rd16le(fmt + 18);  // valid bits
            audio_fmt = rd16le(fmt + 24);        // sub-format GUID first word
        }
    }

    if (!data_ptr || n_channels == 0 || sample_rate == 0)
        throw std::runtime_error(std::string(codec) + ": missing fmt or data chunk");

    ad.sample_rate = sample_rate;
    ad.bit_depth   = bits_per_sample;

    size_t bytes_per_sample = (bits_per_sample + 7) / 8;
    size_t frame_size = bytes_per_sample * n_channels;
    if (frame_size == 0) throw std::runtime_error(std::string(codec) + ": invalid frame size");
    size_t n_frames = data_size / frame_size;

    ad.channels.resize(n_channels);
    for (auto& ch : ad.channels) ch.resize(n_frames);

    bool is_float = (audio_fmt == 3);

    for (size_t f = 0; f < n_frames; ++f) {
        for (uint16_t c = 0; c < n_channels; ++c) {
            const uint8_t* p = data_ptr + f * frame_size + c * bytes_per_sample;
            double val = 0.0;

            if (is_float) {
                if (bits_per_sample == 32) {
                    uint32_t u = rd32le(p);
                    float fv;
                    std::memcpy(&fv, &u, 4);
                    val = fv;
                } else if (bits_per_sample == 64) {
                    uint64_t u = rd64le(p);
                    double dv;
                    std::memcpy(&dv, &u, 8);
                    val = dv;
                }
            } else {
                // Integer PCM
                switch (bits_per_sample) {
                    case 8:
                        val = (p[0] - 128) / 128.0;
                        break;
                    case 16: {
                        int16_t s = static_cast<int16_t>(rd16le(p));
                        val = s / 32768.0;
                        break;
                    }
                    case 24: {
                        int32_t s = p[0] | (p[1]<<8) | (p[2]<<16);
                        if (s & 0x800000) s |= 0xFF000000u;  // sign extend
                        val = s / 8388608.0;
                        break;
                    }
                    case 32: {
                        int32_t s = static_cast<int32_t>(rd32le(p));
                        val = s / 2147483648.0;
                        break;
                    }
                    default:
                        throw std::runtime_error(std::string(codec) +
                                ": unsupported bit depth " +
                                std::to_string(bits_per_sample));
                }
            }
            ad.channels[c][f] = val;
        }
    }

    return ad;
}

// RIFF and RF64/BW64. One walker: RF64 IS RIFF plus a size-escape mechanism,
// and a writer that can fit a size in 32 bits is allowed to just write it, so
// both forms have to work in the same file.
inline AudioData decode_wav(const std::vector<uint8_t>& buf) {
    if (buf.size() < 44)
        throw std::runtime_error("WAV: file too small");
    const bool rf64 = std::memcmp(buf.data(), "RF64", 4) == 0 ||
                      std::memcmp(buf.data(), "BW64", 4) == 0;
    if ((!rf64 && std::memcmp(buf.data(), "RIFF", 4) != 0) ||
        std::memcmp(buf.data()+8, "WAVE", 4) != 0)
        throw std::runtime_error("WAV: invalid header");

    constexpr uint32_t SIZE_IS_64BIT = 0xFFFFFFFFu;

    const uint8_t* fmt_ptr  = nullptr;  size_t   fmt_size  = 0;
    const uint8_t* data_ptr = nullptr;  uint64_t data_size = 0;

    uint64_t ds64_data_size = 0;
    bool     have_ds64      = false;

    // Parse chunks
    size_t pos = 12;
    while (pos + 8 <= buf.size()) {
        const uint8_t* id    = buf.data() + pos;
        const uint8_t* chunk = id + 8;
        const size_t   avail = buf.size() - (pos + 8);
        uint64_t chunk_size  = rd32le(id + 4);

        const bool is_data = std::memcmp(id, "data", 4) == 0;
        if (is_data && chunk_size == SIZE_IS_64BIT) {
            if (rf64) {
                if (!have_ds64)
                    throw std::runtime_error(
                        "RF64: data chunk defers to a 64-bit size but no ds64 chunk precedes it");
                chunk_size = ds64_data_size;
            } else {
                // Plain RIFF with 0xFFFFFFFF: a streaming writer that never
                // went back to patch the size. Read to the end of the file,
                // which is what the value means in practice.
                chunk_size = avail;
            }
        }

        if (rf64 && !have_ds64 && std::memcmp(id, "ds64", 4) == 0 && chunk_size >= 28) {
            // u64 riffSize, u64 dataSize, u64 sampleCount, u32 tableLength,
            // then the table. Only dataSize can matter here: fmt is 16-40
            // bytes, so no fmt chunk has ever needed the table, and reading
            // sampleCount would only duplicate data_size / frame_size.
            ds64_data_size = rd64le(chunk + 8);
            have_ds64 = true;
        } else if (std::memcmp(id, "fmt ", 4) == 0 && chunk_size >= 16) {
            fmt_ptr  = chunk;
            fmt_size = static_cast<size_t>(std::min<uint64_t>(chunk_size, avail));
        } else if (is_data) {
            data_ptr = chunk;
            // A truncated capture — an interrupted recording, a partial copy —
            // declares more data than it holds. Read what is actually there
            // instead of running off the end of the buffer.
            data_size = std::min<uint64_t>(chunk_size, avail);
        }

        if (chunk_size > avail) break;
        pos += 8 + static_cast<size_t>(chunk_size);
        if (chunk_size & 1) pos++;  // pad byte
    }

    return decode_wave_payload(fmt_ptr, fmt_size, data_ptr,
                               static_cast<size_t>(data_size),
                               rf64 ? "RF64" : "WAV");
}

// Sony Wave64.
inline AudioData decode_w64(const std::vector<uint8_t>& buf) {
    if (buf.size() < 40)
        throw std::runtime_error("Wave64: file too small");
    if (std::memcmp(buf.data(), W64_RIFF_GUID, 16) != 0 ||
        !w64_id_is(buf.data() + 24, "wave"))
        throw std::runtime_error("Wave64: invalid header");

    const uint8_t* fmt_ptr  = nullptr;  size_t   fmt_size  = 0;
    const uint8_t* data_ptr = nullptr;  uint64_t data_size = 0;

    size_t pos = 40;                         // 16 riff GUID + 8 size + 16 wave GUID
    while (pos + 24 <= buf.size()) {
        const uint8_t* id   = buf.data() + pos;
        uint64_t       size = rd64le(id + 16);
        if (size < 24) break;                // malformed: would not advance
        const uint8_t* chunk = id + 24;
        const size_t   avail = buf.size() - (pos + 24);
        uint64_t body = std::min<uint64_t>(size - 24, avail);

        if (w64_id_is(id, "fmt ")) {
            fmt_ptr  = chunk;
            fmt_size = static_cast<size_t>(body);
        } else if (w64_id_is(id, "data")) {
            data_ptr  = chunk;
            data_size = body;
        }

        // Chunks pad to an 8-byte boundary; the pad is not counted in size.
        uint64_t adv = (size + 7) & ~static_cast<uint64_t>(7);
        if (adv > buf.size() - pos) break;
        pos += static_cast<size_t>(adv);
    }

    return decode_wave_payload(fmt_ptr, fmt_size, data_ptr,
                               static_cast<size_t>(data_size), "WAVE64");
}

// Entry point for the whole RIFF family. Dispatch is by MAGIC, not extension,
// deliberately: RF64 files are conventionally named .wav (that is the point of
// the format — it is a drop-in escape hatch), and a Wave64 capture renamed
// .wav is common enough to be worth surviving.
inline AudioData decode_wave(const std::vector<uint8_t>& buf) {
    if (buf.size() >= 16 && std::memcmp(buf.data(), W64_RIFF_GUID, 16) == 0)
        return decode_w64(buf);
    return decode_wav(buf);
}

// ════════════════════════════════════════════════════════════════════════════
// AIFF / AIFF-C Decoder
// ════════════════════════════════════════════════════════════════════════════

inline AudioData decode_aiff(const std::vector<uint8_t>& buf) {
    if (buf.size() < 12)
        throw std::runtime_error("AIFF: file too small");
    if (std::memcmp(buf.data(), "FORM", 4) != 0)
        throw std::runtime_error("AIFF: not a FORM file");

    bool is_aifc = (std::memcmp(buf.data()+8, "AIFC", 4) == 0);
    bool is_aiff = (std::memcmp(buf.data()+8, "AIFF", 4) == 0);
    if (!is_aiff && !is_aifc)
        throw std::runtime_error("AIFF: not AIFF or AIFC");

    AudioData ad;
    ad.codec = is_aifc ? "AIFF-C" : "AIFF";

    uint16_t n_channels = 0;
    uint32_t n_frames = 0;
    uint16_t bits_per_sample = 0;
    double sample_rate = 0.0;

    const uint8_t* ssnd_data = nullptr;
    size_t ssnd_size = 0;
    uint32_t ssnd_offset = 0;

    uint32_t compression_type = 0;  // for AIFF-C

    size_t pos = 12;
    while (pos + 8 <= buf.size()) {
        uint32_t chunk_size = rd32be(buf.data() + pos + 4);
        const uint8_t* chunk = buf.data() + pos + 8;

        if (std::memcmp(buf.data()+pos, "COMM", 4) == 0) {
            n_channels     = rd16be(chunk);
            n_frames       = rd32be(chunk + 2);
            bits_per_sample = rd16be(chunk + 6);
            sample_rate    = extended_to_double(chunk + 8);

            if (is_aifc && chunk_size >= 22) {
                compression_type = rd32be(chunk + 18);
            }
        } else if (std::memcmp(buf.data()+pos, "SSND", 4) == 0) {
            ssnd_offset = rd32be(chunk);
            ssnd_data = chunk + 8 + ssnd_offset;
            ssnd_size = chunk_size - 8 - ssnd_offset;
        }

        pos += 8 + chunk_size;
        if (chunk_size & 1) pos++;
    }

    if (!ssnd_data || n_channels == 0 || sample_rate == 0.0)
        throw std::runtime_error("AIFF: missing COMM or SSND chunk");

    // Check for compressed AIFF-C formats we don't support
    if (is_aifc && compression_type != 0 &&
        compression_type != 0x4E4F4E45 /* 'NONE' */ &&
        compression_type != 0x736F7774 /* 'sowt' (little-endian) */) {
        throw std::runtime_error("AIFF-C: unsupported compression");
    }

    bool little_endian = (compression_type == 0x736F7774);

    ad.sample_rate = static_cast<uint32_t>(sample_rate);
    ad.bit_depth   = bits_per_sample;

    size_t bytes_per_sample = (bits_per_sample + 7) / 8;
    size_t frame_size = bytes_per_sample * n_channels;
    if (frame_size == 0) throw std::runtime_error("AIFF: invalid frame size");
    size_t actual_frames = std::min<size_t>(n_frames, ssnd_size / frame_size);

    ad.channels.resize(n_channels);
    for (auto& ch : ad.channels) ch.resize(actual_frames);

    for (size_t f = 0; f < actual_frames; ++f) {
        for (uint16_t c = 0; c < n_channels; ++c) {
            const uint8_t* p = ssnd_data + f * frame_size + c * bytes_per_sample;
            double val = 0.0;

            if (little_endian) {
                // Same as WAV integer decoding
                switch (bits_per_sample) {
                    case 16: { int16_t s = static_cast<int16_t>(rd16le(p)); val = s/32768.0; break; }
                    case 24: { int32_t s = p[0]|(p[1]<<8)|(p[2]<<16); if(s&0x800000) s|=0xFF000000u; val = s/8388608.0; break; }
                    case 32: { int32_t s = static_cast<int32_t>(rd32le(p)); val = s/2147483648.0; break; }
                    default: throw std::runtime_error("AIFF: unsupported bit depth");
                }
            } else {
                // Big-endian (standard AIFF)
                switch (bits_per_sample) {
                    case 8:  val = (static_cast<int8_t>(p[0])) / 128.0; break;
                    case 16: { int16_t s = static_cast<int16_t>(rd16be(p)); val = s/32768.0; break; }
                    case 24: {
                        int32_t s = (p[0]<<16)|(p[1]<<8)|p[2];
                        if (s & 0x800000) s |= 0xFF000000u;
                        val = s / 8388608.0;
                        break;
                    }
                    case 32: { int32_t s = static_cast<int32_t>(rd32be(p)); val = s/2147483648.0; break; }
                    default: throw std::runtime_error("AIFF: unsupported bit depth");
                }
            }
            ad.channels[c][f] = val;
        }
    }

    return ad;
}

// ════════════════════════════════════════════════════════════════════════════
// FLAC Decoder (built-in, zero dependencies)
//
// Implements the full FLAC decoding pipeline:
//   Metadata → Frame header → Subframe decoding → Rice entropy → LPC/Fixed
//
// Reference: https://xiph.org/flac/format.html
// ════════════════════════════════════════════════════════════════════════════

namespace flac {

// ── Bit reader (MSB-first) ─────────────────────────────────────────────────
class BitReader {
    const uint8_t* data_;
    size_t size_;
    size_t byte_pos_;
    int bit_pos_;  // bits remaining in current byte (8..1)

public:
    BitReader(const uint8_t* data, size_t size)
        : data_(data), size_(size), byte_pos_(0), bit_pos_(8) {}

    bool eof() const { return byte_pos_ >= size_; }

    size_t position_bytes() const { return byte_pos_; }
    int position_bits() const { return 8 - bit_pos_; }

    // Align to next byte boundary
    void align() {
        if (bit_pos_ != 8) {
            bit_pos_ = 8;
            byte_pos_++;
        }
    }

    uint32_t read_bits(int n) {
        uint32_t result = 0;
        while (n > 0) {
            if (byte_pos_ >= size_)
                throw std::runtime_error("FLAC: unexpected end of stream");
            int avail = bit_pos_;
            int take = std::min(n, avail);
            int shift = avail - take;
            uint32_t mask = ((1u << take) - 1);
            result = (result << take) | ((data_[byte_pos_] >> shift) & mask);
            bit_pos_ -= take;
            n -= take;
            if (bit_pos_ == 0) {
                bit_pos_ = 8;
                byte_pos_++;
            }
        }
        return result;
    }

    int32_t read_bits_signed(int n) {
        uint32_t val = read_bits(n);
        if (n > 0 && (val & (1u << (n-1)))) {
            // Sign extend
            val |= ~((1u << n) - 1);
        }
        return static_cast<int32_t>(val);
    }

    // Read unary: count of 0-bits before first 1-bit
    uint32_t read_unary() {
        uint32_t count = 0;
        while (true) {
            if (byte_pos_ >= size_)
                throw std::runtime_error("FLAC: unexpected end in unary");
            uint32_t bit = read_bits(1);
            if (bit) return count;
            count++;
            if (count > 1000000)
                throw std::runtime_error("FLAC: runaway unary code");
        }
    }

    // Read a single raw byte (must be byte-aligned)
    uint8_t read_byte() {
        return static_cast<uint8_t>(read_bits(8));
    }

    // Skip to absolute byte position
    void seek_byte(size_t pos) {
        byte_pos_ = pos;
        bit_pos_ = 8;
    }

    // Read UTF-8 coded number (used in frame headers)
    uint64_t read_utf8() {
        uint8_t first = read_byte();
        if ((first & 0x80) == 0) return first;

        int n_extra;
        uint64_t val;
        if ((first & 0xE0) == 0xC0)      { n_extra = 1; val = first & 0x1F; }
        else if ((first & 0xF0) == 0xE0)  { n_extra = 2; val = first & 0x0F; }
        else if ((first & 0xF8) == 0xF0)  { n_extra = 3; val = first & 0x07; }
        else if ((first & 0xFC) == 0xF8)  { n_extra = 4; val = first & 0x03; }
        else if ((first & 0xFE) == 0xFC)  { n_extra = 5; val = first & 0x01; }
        else if (first == 0xFE)           { n_extra = 6; val = 0; }
        else throw std::runtime_error("FLAC: invalid UTF-8 start byte");

        for (int i = 0; i < n_extra; ++i) {
            uint8_t b = read_byte();
            if ((b & 0xC0) != 0x80)
                throw std::runtime_error("FLAC: invalid UTF-8 continuation");
            val = (val << 6) | (b & 0x3F);
        }
        return val;
    }
};

// ── STREAMINFO ─────────────────────────────────────────────────────────────
struct StreamInfo {
    uint16_t min_block_size;
    uint16_t max_block_size;
    uint32_t min_frame_size;
    uint32_t max_frame_size;
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  bits_per_sample;
    uint64_t total_samples;
};

inline StreamInfo parse_streaminfo(const uint8_t* data) {
    StreamInfo si;
    si.min_block_size = rd16be(data);
    si.max_block_size = rd16be(data + 2);
    si.min_frame_size = (data[4]<<16)|(data[5]<<8)|data[6];
    si.max_frame_size = (data[7]<<16)|(data[8]<<8)|data[9];

    // 20 bits sample rate, 3 bits channels-1, 5 bits bps-1, 36 bits total samples
    uint64_t packed = rd64be(data + 10);
    si.sample_rate    = static_cast<uint32_t>((packed >> 44) & 0xFFFFF);
    si.channels       = static_cast<uint8_t>(((packed >> 41) & 0x7) + 1);
    si.bits_per_sample = static_cast<uint8_t>(((packed >> 36) & 0x1F) + 1);
    si.total_samples  = packed & 0xFFFFFFFFFULL;

    return si;
}

// ── Rice residual decoding ─────────────────────────────────────────────────
inline void decode_rice_partition(BitReader& br, int32_t* residual,
                                   size_t block_size, int predictor_order,
                                   int partition_order,
                                   int rice_param_bits) {
    size_t n_partitions = 1u << partition_order;
    size_t base_partition_size = block_size >> partition_order;
    size_t sample_idx = 0;
    size_t total_residuals = block_size - predictor_order;

    for (size_t p = 0; p < n_partitions; ++p) {
        uint32_t rice_param = br.read_bits(rice_param_bits);
        // First partition has fewer samples due to predictor warmup
        size_t n_in_partition = (p == 0)
            ? base_partition_size - predictor_order
            : base_partition_size;

        if (rice_param == ((1u << rice_param_bits) - 1)) {
            // Escape: raw bits encoding
            uint32_t raw_bits = br.read_bits(5);
            for (size_t i = 0; i < n_in_partition && sample_idx < total_residuals; ++i, ++sample_idx) {
                residual[sample_idx] = br.read_bits_signed(raw_bits);
            }
        } else {
            for (size_t i = 0; i < n_in_partition && sample_idx < total_residuals; ++i, ++sample_idx) {
                uint32_t q = br.read_unary();
                uint32_t r = (rice_param > 0) ? br.read_bits(rice_param) : 0;
                uint32_t val = (q << rice_param) | r;
                // Zigzag decode: 0→0, 1→-1, 2→1, 3→-2, ...
                residual[sample_idx] = (val & 1) ? -static_cast<int32_t>((val+1)>>1)
                                                  : static_cast<int32_t>(val>>1);
            }
        }
    }
}

// ── Fixed predictor coefficients ───────────────────────────────────────────
// order 0: prediction = 0
// order 1: prediction = s[n-1]
// order 2: prediction = 2*s[n-1] - s[n-2]
// order 3: prediction = 3*s[n-1] - 3*s[n-2] + s[n-3]
// order 4: prediction = 4*s[n-1] - 6*s[n-2] + 4*s[n-3] - s[n-4]

inline void restore_fixed(int32_t* samples, size_t n, int order) {
    // Warmup samples are already in place
    for (size_t i = order; i < n; ++i) {
        int64_t prediction = 0;
        switch (order) {
            case 0: break;
            case 1: prediction = samples[i-1]; break;
            case 2: prediction = 2LL*samples[i-1] - samples[i-2]; break;
            case 3: prediction = 3LL*samples[i-1] - 3LL*samples[i-2] + samples[i-3]; break;
            case 4: prediction = 4LL*samples[i-1] - 6LL*samples[i-2] + 4LL*samples[i-3] - samples[i-4]; break;
        }
        samples[i] += static_cast<int32_t>(prediction);
    }
}

// ── Decode a single subframe ───────────────────────────────────────────────
inline void decode_subframe(BitReader& br, int32_t* output, size_t block_size,
                             int bps) {
    // Subframe header
    uint32_t pad = br.read_bits(1);
    if (pad != 0) throw std::runtime_error("FLAC: subframe padding bit != 0");

    uint32_t type_bits = br.read_bits(6);
    bool has_wasted = br.read_bits(1);
    int wasted = 0;
    if (has_wasted) {
        wasted = 1;
        while (br.read_bits(1) == 0) wasted++;
        bps -= wasted;
    }

    if (type_bits == 0) {
        // CONSTANT
        int32_t val = br.read_bits_signed(bps);
        for (size_t i = 0; i < block_size; ++i) output[i] = val;
    }
    else if (type_bits == 1) {
        // VERBATIM
        for (size_t i = 0; i < block_size; ++i)
            output[i] = br.read_bits_signed(bps);
    }
    else if (type_bits >= 8 && type_bits <= 12) {
        // FIXED, order = type_bits - 8
        int order = type_bits - 8;

        // Warmup samples
        for (int i = 0; i < order; ++i)
            output[i] = br.read_bits_signed(bps);

        // Residual
        uint32_t residual_type = br.read_bits(2);
        int rice_bits = (residual_type == 0) ? 4 : 5;
        int partition_order = br.read_bits(4);

        size_t residual_count = block_size - order;

        // Decode residuals into output starting at [order]
        std::vector<int32_t> residual(residual_count);
        decode_rice_partition(br, residual.data(), block_size, order,
                              partition_order, rice_bits);
        for (size_t i = 0; i < residual_count; ++i)
            output[order + i] = residual[i];

        restore_fixed(output, block_size, order);
    }
    else if (type_bits >= 32 && type_bits <= 63) {
        // LPC, order = type_bits - 31
        int order = type_bits - 31;

        // Warmup samples
        for (int i = 0; i < order; ++i)
            output[i] = br.read_bits_signed(bps);

        // LPC parameters
        int precision = br.read_bits(4) + 1;
        int shift = br.read_bits_signed(5);

        std::vector<int32_t> coeffs(order);
        for (int i = 0; i < order; ++i)
            coeffs[i] = br.read_bits_signed(precision);

        // Residual
        uint32_t residual_type = br.read_bits(2);
        int rice_bits = (residual_type == 0) ? 4 : 5;
        int partition_order = br.read_bits(4);

        size_t residual_count = block_size - order;
        std::vector<int32_t> residual(residual_count);
        decode_rice_partition(br, residual.data(), block_size, order,
                              partition_order, rice_bits);

        // Restore LPC
        for (size_t i = 0; i < residual_count; ++i) {
            int64_t prediction = 0;
            for (int j = 0; j < order; ++j)
                prediction += static_cast<int64_t>(coeffs[j]) * output[order + i - 1 - j];
            output[order + i] = residual[i] + static_cast<int32_t>(prediction >> shift);
        }
    }
    else {
        throw std::runtime_error("FLAC: reserved subframe type " + std::to_string(type_bits));
    }

    // Undo wasted bits
    if (wasted > 0) {
        for (size_t i = 0; i < block_size; ++i)
            output[i] <<= wasted;
    }
}

} // namespace flac

// ── Top-level FLAC decoder ─────────────────────────────────────────────────
inline AudioData decode_flac(const std::vector<uint8_t>& buf) {
    if (buf.size() < 42 || std::memcmp(buf.data(), "fLaC", 4) != 0)
        throw std::runtime_error("FLAC: invalid magic");

    AudioData ad;
    ad.codec = "FLAC";

    // Parse metadata blocks
    size_t pos = 4;
    flac::StreamInfo si{};
    bool have_si = false;

    while (pos + 4 <= buf.size()) {
        uint8_t header = buf[pos];
        bool is_last = (header & 0x80) != 0;
        uint8_t block_type = header & 0x7F;
        uint32_t block_size = (buf[pos+1]<<16) | (buf[pos+2]<<8) | buf[pos+3];
        pos += 4;

        if (block_type == 0 && block_size >= 34) {
            si = flac::parse_streaminfo(buf.data() + pos);
            have_si = true;
        }

        pos += block_size;
        if (is_last) break;
    }

    if (!have_si)
        throw std::runtime_error("FLAC: no STREAMINFO block");

    ad.sample_rate = si.sample_rate;
    ad.bit_depth   = si.bits_per_sample;

    // Allocate output channels
    size_t total_out = si.total_samples;
    if (total_out == 0) total_out = si.sample_rate * 3600;  // fallback estimate

    ad.channels.resize(si.channels);
    for (auto& ch : ad.channels) ch.reserve(std::min<size_t>(total_out, 1ULL<<28));

    double norm = 1.0 / (1LL << (si.bits_per_sample - 1));

    // Decode audio frames
    while (pos + 2 < buf.size()) {
        // Fast byte-level pre-filter: 0xFF followed by 0xF8 or 0xF9
        if (buf[pos] != 0xFF || (buf[pos+1] & 0xFC) != 0xF8) {
            pos++;
            continue;
        }

        size_t frame_start = pos;
        flac::BitReader br(buf.data() + pos, buf.size() - pos);

        try {
            uint32_t sync = br.read_bits(14);
            if (sync != 0x3FFE) {
                pos++;
                continue;
            }

            uint32_t reserved0 = br.read_bits(1);
            if (reserved0 != 0) { pos++; continue; }

            [[maybe_unused]] uint32_t blocking_strategy = br.read_bits(1);
            uint32_t bs_code = br.read_bits(4);
            uint32_t sr_code = br.read_bits(4);
            uint32_t ch_assign = br.read_bits(4);
            [[maybe_unused]] uint32_t bps_code = br.read_bits(3);
            uint32_t reserved1 = br.read_bits(1);
            if (reserved1 != 0) { pos++; continue; }

            // Frame/sample number (UTF-8 coded)
            br.read_utf8();

            // Block size
            uint32_t block_size;
            switch (bs_code) {
                case 0: throw std::runtime_error("FLAC: reserved block size 0");
                case 1: block_size = 192; break;
                case 2: case 3: case 4: case 5:
                    block_size = 576 * (1 << (bs_code - 2)); break;
                case 6: block_size = br.read_bits(8) + 1; break;
                case 7: block_size = br.read_bits(16) + 1; break;
                default:
                    block_size = 256 * (1 << (bs_code - 8)); break;
            }

            // Sample rate from header (if coded)
            if (sr_code == 12) br.read_bits(8);
            else if (sr_code == 13 || sr_code == 14) br.read_bits(16);

            // CRC-8 (skip)
            br.read_bits(8);

            // Determine bits per sample for each subframe
            int bps = si.bits_per_sample;
            int n_subframes;

            // Channel assignment
            if (ch_assign <= 7) {
                n_subframes = ch_assign + 1;
            } else if (ch_assign >= 8 && ch_assign <= 10) {
                n_subframes = 2;
            } else {
                throw std::runtime_error("FLAC: reserved channel assignment");
            }

            // Decode subframes
            std::vector<std::vector<int32_t>> subframes(n_subframes);
            for (int sf = 0; sf < n_subframes; ++sf) {
                subframes[sf].resize(block_size);
                int sf_bps = bps;
                // Side channel gets +1 bit
                if (ch_assign == 8 && sf == 1) sf_bps++;
                else if (ch_assign == 9 && sf == 0) sf_bps++;
                else if (ch_assign == 10 && sf == 1) sf_bps++;

                flac::decode_subframe(br, subframes[sf].data(), block_size, sf_bps);
            }

            // Decorrelate channels
            if (ch_assign == 8) {
                for (size_t i = 0; i < block_size; ++i)
                    subframes[1][i] = subframes[0][i] - subframes[1][i];
            } else if (ch_assign == 9) {
                for (size_t i = 0; i < block_size; ++i)
                    subframes[0][i] = subframes[0][i] + subframes[1][i];
            } else if (ch_assign == 10) {
                for (size_t i = 0; i < block_size; ++i) {
                    int64_t mid  = subframes[0][i];
                    int64_t side = subframes[1][i];
                    mid = mid * 2 + (side & 1);
                    subframes[0][i] = static_cast<int32_t>((mid + side) >> 1);
                    subframes[1][i] = static_cast<int32_t>((mid - side) >> 1);
                }
            }

            // Append to output channels
            for (int c = 0; c < static_cast<int>(si.channels); ++c) {
                int src = std::min(c, n_subframes - 1);
                for (size_t i = 0; i < block_size; ++i)
                    ad.channels[c].push_back(subframes[src][i] * norm);
            }

            // Align to byte boundary and skip CRC-16
            br.align();
            br.read_bits(16);

            pos = frame_start + br.position_bytes();
        }
        catch (const std::exception&) {
            pos++;
            continue;
        }
    }

    if (ad.channels.empty() || ad.channels[0].empty())
        throw std::runtime_error("FLAC: no audio frames decoded");

    return ad;
}

// ════════════════════════════════════════════════════════════════════════════
// DSD → PCM Decimation — Byte-LUT polyphase FIR engine
//
// See dsd_lut.hpp for implementation details. The LUT approach replaces
// per-bit FIR convolution with precomputed byte lookup tables, achieving
// ~8× speedup while producing identical output.
// ════════════════════════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════════════════════════
// DSF (DSD Stream File) Decoder
// ════════════════════════════════════════════════════════════════════════════

inline AudioData decode_dsf(const std::vector<uint8_t>& buf,
                            const dsd::Options& dsd_opt = {}) {
    if (buf.size() < 28 || std::memcmp(buf.data(), "DSD ", 4) != 0)
        throw std::runtime_error("DSF: invalid header");

    AudioData ad;
    ad.codec = "DSD";

    // DSD chunk
    uint64_t total_file_size = rd64le(buf.data() + 12);
    uint64_t metadata_offset = rd64le(buf.data() + 20);
    (void)total_file_size;
    (void)metadata_offset;

    // fmt chunk
    size_t fmt_pos = 28;
    if (fmt_pos + 52 > buf.size() || std::memcmp(buf.data()+fmt_pos, "fmt ", 4) != 0)
        throw std::runtime_error("DSF: missing fmt chunk");

    uint64_t fmt_size = rd64le(buf.data() + fmt_pos + 4);
    uint32_t format_version = rd32le(buf.data() + fmt_pos + 12);
    uint32_t format_id      = rd32le(buf.data() + fmt_pos + 16);
    uint32_t channel_type   = rd32le(buf.data() + fmt_pos + 20);
    uint32_t n_channels     = rd32le(buf.data() + fmt_pos + 24);
    uint32_t dsd_sample_rate = rd32le(buf.data() + fmt_pos + 28);
    uint32_t bits_per_sample = rd32le(buf.data() + fmt_pos + 32);
    uint64_t sample_count   = rd64le(buf.data() + fmt_pos + 36);
    uint32_t block_size_per_ch = rd32le(buf.data() + fmt_pos + 44);
    (void)format_version;
    (void)format_id;
    (void)channel_type;

    // Data chunk
    size_t data_pos = fmt_pos + static_cast<size_t>(fmt_size);
    if (data_pos + 12 > buf.size() || std::memcmp(buf.data()+data_pos, "data", 4) != 0)
        throw std::runtime_error("DSF: missing data chunk");

    uint64_t data_chunk_size = rd64le(buf.data() + data_pos + 4);
    const uint8_t* dsd_data = buf.data() + data_pos + 12;
    size_t dsd_data_size = static_cast<size_t>(data_chunk_size) - 12;

    ad.sample_rate = static_cast<uint32_t>(dsd_opt.out_rate);
    ad.bit_depth   = 1;

    // Fail fast (on this thread) before the O(file) deinterleave if the
    // rate/chain/filter combination is unsupported.
    dsd::validate(static_cast<int>(dsd_sample_rate), dsd_opt);

    // Deinterleave DSD blocks
    size_t total_dsd_blocks = dsd_data_size / (block_size_per_ch * n_channels);
    size_t total_dsd_bytes_per_ch = total_dsd_blocks * block_size_per_ch;
    uint64_t total_dsd_samples_per_ch = std::min<uint64_t>(
        sample_count / (bits_per_sample > 1 ? bits_per_sample : 1),
        total_dsd_bytes_per_ch * 8);

    // Extract per-channel DSD bit streams
    std::vector<std::vector<uint8_t>> ch_dsd(n_channels);
    for (auto& v : ch_dsd) v.reserve(total_dsd_bytes_per_ch);

    for (size_t blk = 0; blk < total_dsd_blocks; ++blk) {
        for (uint32_t c = 0; c < n_channels; ++c) {
            size_t offset = blk * block_size_per_ch * n_channels + c * block_size_per_ch;
            if (offset + block_size_per_ch <= dsd_data_size) {
                ch_dsd[c].insert(ch_dsd[c].end(),
                    dsd_data + offset, dsd_data + offset + block_size_per_ch);
            }
        }
    }

    // Byte-LUT polyphase FIR decimation: DSD → 352.8 kHz PCM
    ad.channels.resize(n_channels);
    {
        std::vector<std::thread> ch_threads;
        std::vector<std::exception_ptr> ch_err(n_channels);
        for (uint32_t c = 0; c < n_channels; ++c) {
            ch_threads.emplace_back([&, c]() {
                try {
                    ad.channels[c] = dsd::decimate_channel(
                        ch_dsd[c], static_cast<size_t>(total_dsd_samples_per_ch),
                        static_cast<int>(dsd_sample_rate),
                        true /* LSB first in DSF */, dsd_opt);
                } catch (...) {
                    ch_err[c] = std::current_exception();
                }
            });
        }
        for (auto& t : ch_threads) t.join();
        for (auto& e : ch_err) if (e) std::rethrow_exception(e);
    }

    ch_dsd.clear();
    ch_dsd.shrink_to_fit();

    return ad;
}

// ════════════════════════════════════════════════════════════════════════════
// DFF (DSDIFF) Decoder
// ════════════════════════════════════════════════════════════════════════════

inline AudioData decode_dff(const std::vector<uint8_t>& buf,
                            const dsd::Options& dsd_opt = {}) {
    if (buf.size() < 16 || std::memcmp(buf.data(), "FRM8", 4) != 0)
        throw std::runtime_error("DFF: invalid header");
    if (std::memcmp(buf.data() + 12, "DSD ", 4) != 0)
        throw std::runtime_error("DFF: not a DSD file");

    AudioData ad;
    ad.codec = "DSD";
    ad.bit_depth = 1;

    uint32_t n_channels = 0;
    uint32_t dsd_sample_rate = 0;
    const uint8_t* dsd_data = nullptr;
    size_t dsd_data_size = 0;

    size_t pos = 16;
    while (pos + 12 <= buf.size()) {
        uint64_t chunk_size = rd64be(buf.data() + pos + 4);
        const uint8_t* chunk = buf.data() + pos + 12;

        if (std::memcmp(buf.data()+pos, "PROP", 4) == 0) {
            size_t sub_pos = 4;  // skip "SND "
            while (sub_pos + 12 <= chunk_size) {
                uint64_t sub_size = rd64be(chunk + sub_pos + 4);
                if (std::memcmp(chunk+sub_pos, "FS  ", 4) == 0) {
                    dsd_sample_rate = rd32be(chunk + sub_pos + 12);
                } else if (std::memcmp(chunk+sub_pos, "CHNL", 4) == 0) {
                    n_channels = rd16be(chunk + sub_pos + 12);
                }
                sub_pos += 12 + sub_size;
                if (sub_size & 1) sub_pos++;
            }
        } else if (std::memcmp(buf.data()+pos, "DSD ", 4) == 0) {
            dsd_data = chunk;
            dsd_data_size = static_cast<size_t>(chunk_size);
        }

        pos += 12 + static_cast<size_t>(chunk_size);
        if (chunk_size & 1) pos++;
    }

    if (!dsd_data || n_channels == 0 || dsd_sample_rate == 0)
        throw std::runtime_error("DFF: incomplete header");

    ad.sample_rate = static_cast<uint32_t>(dsd_opt.out_rate);

    // Fail fast (on this thread) before the O(file) deinterleave if the
    // rate/chain/filter combination is unsupported.
    dsd::validate(static_cast<int>(dsd_sample_rate), dsd_opt);

    size_t total_bytes_per_ch = dsd_data_size / n_channels;
    size_t total_bits_per_ch = total_bytes_per_ch * 8;

    // Deinterleave
    std::vector<std::vector<uint8_t>> ch_dsd(n_channels);
    for (auto& v : ch_dsd) v.reserve(total_bytes_per_ch);

    for (size_t i = 0; i < dsd_data_size; ++i) {
        uint32_t c = i % n_channels;
        ch_dsd[c].push_back(dsd_data[i]);
    }

    // Byte-LUT polyphase FIR decimation: DSD → 352.8 kHz PCM
    ad.channels.resize(n_channels);
    {
        std::vector<std::thread> ch_threads;
        std::vector<std::exception_ptr> ch_err(n_channels);
        for (uint32_t c = 0; c < n_channels; ++c) {
            ch_threads.emplace_back([&, c]() {
                try {
                    ad.channels[c] = dsd::decimate_channel(
                        ch_dsd[c], total_bits_per_ch,
                        static_cast<int>(dsd_sample_rate),
                        false /* MSB first in DFF */, dsd_opt);
                } catch (...) {
                    ch_err[c] = std::current_exception();
                }
            });
        }
        for (auto& t : ch_threads) t.join();
        for (auto& e : ch_err) if (e) std::rethrow_exception(e);
    }

    ch_dsd.clear();
    ch_dsd.shrink_to_fit();

    return ad;
}

// ════════════════════════════════════════════════════════════════════════════
// Unified decoder dispatch
// ════════════════════════════════════════════════════════════════════════════

#ifdef CRETE_HAS_FFMPEG
// Defined in audio_ffmpeg.hpp (included at the end of this header).
inline AudioData decode_with_ffmpeg(const std::string& path, int want_stream);
#endif

// Formats crête decodes natively. These are the validated paths (vs MAAT for
// PCM; vs foobar's chosen DSD mode for DSD) and must NEVER be routed to FFmpeg.
// ── Immersive (object-based) carriers ──────────────────────────────────────
// Does this profile string name a format whose objects FFmpeg does NOT
// render? Atmos (over TrueHD or E-AC-3 JOC) and DTS:X both decode to their
// channel BED with the object metadata discarded, so a layout reported for
// such a file describes the bed and must say so. A plain "DTS-HD MA" or
// "DTS Express" profile is NOT this: it is lossless/lossy channel audio with
// nothing unrendered, and gets no qualifier.
//
// Substring matching on FFmpeg's own profile table entries, which are fixed
// strings in libavcodec: "Dolby TrueHD + Dolby Atmos", "Dolby Digital Plus +
// Dolby Atmos", "DTS-HD MA + DTS:X", "DTS-HD MA + DTS:X IMAX".
inline bool profile_is_immersive(const std::string& profile) {
    auto has = [&](const char* needle) {
        return profile.find(needle) != std::string::npos;
    };
    return has("Atmos") || has("DTS:X") || has("DTS-X");
}

// The one-line qualifier to hang off a layout, or empty when none is needed.
inline std::string layout_qualifier(const std::string& profile) {
    if (!profile_is_immersive(profile)) return {};
    return profile.find("DTS") != std::string::npos
         ? "DTS:X objects not rendered"
         : "Atmos objects not rendered";
}

// Disc images. Kept OUT of is_native_format(): the extension says nothing --
// .iso is every disc image ever made -- so acceptance is decided by content,
// via sacd::looks_like_sacd_iso(). This predicate exists only so a directory
// scan considers the file at all.
inline bool is_disc_image_format(const std::string& ext) {
    return ext == ".iso";
}

inline bool is_native_format(const std::string& ext) {
    // .rf64/.bw64/.w64 are listed so a directory scan picks them up, but the
    // decoder dispatches on the file's MAGIC rather than its name: an RF64
    // file is conventionally named .wav, so the extension is not evidence of
    // anything. See decode_wave().
    return ext == ".wav" || ext == ".flac" || ext == ".aif" || ext == ".aiff"
        || ext == ".dsf" || ext == ".dff"
        || ext == ".rf64" || ext == ".bw64" || ext == ".w64";
}

// Extra container/codec extensions accepted only in FFmpeg builds. Curated so
// directory scans pick up common lossy/ALAC files without feeding arbitrary
// non-audio files to the decoder.
inline bool is_ffmpeg_extra_format(const std::string& ext) {
#ifdef CRETE_HAS_FFMPEG
    return ext == ".m4a" || ext == ".mp4" || ext == ".m4b" || ext == ".aac"
        || ext == ".mp3" || ext == ".opus" || ext == ".ogg" || ext == ".oga"
        || ext == ".mka" || ext == ".webm" || ext == ".wma"
        || ext == ".wv"  || ext == ".ape"  || ext == ".mpc" || ext == ".tak"
        // Disc audio. .mkv is a video container, but so is .mp4 above, and
        // the FFmpeg backend picks an audio stream and ignores video and
        // subtitles -- music Blu-ray rips are routinely .mkv. Such a rip often
        // carries SEVERAL audio streams (different mixes of the same album);
        // decode_with_ffmpeg reports the count and honours --stream. The rest
        // are raw elementary
        // streams matching the demuxers enabled in build_ffmpeg_compact.sh.
        // NOTE: these carry the channel BED only. Atmos/DTS:X objects are not
        // rendered by FFmpeg; see the profile note in that script.
        || ext == ".mkv" || ext == ".thd"  || ext == ".dts"
        || ext == ".ac3" || ext == ".eac3" || ext == ".ec3";
#else
    (void)ext;
    return false;
#endif
}

inline bool is_supported_format(const std::string& path) {
    auto ext = get_extension(path);
    return is_native_format(ext) || is_ffmpeg_extra_format(ext);
}

// want_stream: container audio-stream index to decode, or -1 for "let FFmpeg
// choose". Only meaningful for the FFmpeg-routed container formats; the native
// decoders each read a file that holds exactly one stream, so asking for a
// specific index there is a user error rather than a silent no-op.
// One track of an SACD area, decoded through the SAME libdsddpcm chain every
// .dsf and .dff goes through -- which is what makes "crete on the ISO" and
// "crete on a sacd_extract'ed .dsf of the same disc" a valid crete-vs-crete
// gate needing no external oracle.
//
// Takes an open Image so a 10-track album does not reopen and re-probe the
// disc ten times, and decodes ONE track rather than the whole area: the
// stereo area of a 43-minute disc is 1.8 GB of f64 at the default 44.1 kHz
// output and 14.6 GB at 352.8, so the cue-style "decode once, slice" model
// does not transfer here.
inline AudioData decode_sacd_track(sacd::Image& img, const sacd::Area& area,
                                   size_t track_index,
                                   const dsd::Options& dsd_opt = {}) {
    auto ch_dsd = sacd::read_track(img, area, track_index);

    AudioData ad;
    ad.codec       = "DSD";
    ad.bit_depth   = 1;
    ad.sample_rate = static_cast<uint32_t>(dsd_opt.out_rate);
    const size_t nch = ch_dsd.size();
    ad.channels.resize(nch);

    std::vector<std::thread> ch_threads;
    std::vector<std::exception_ptr> ch_err(nch);
    for (size_t c = 0; c < nch; ++c) {
        ch_threads.emplace_back([&, c]() {
            try {
                ad.channels[c] = dsd::decimate_channel(
                    ch_dsd[c], ch_dsd[c].size() * 8,
                    static_cast<int>(sacd::DSD_RATE),
                    false /* SACD is MSB-first, as DFF is */, dsd_opt);
            } catch (...) {
                ch_err[c] = std::current_exception();
            }
        });
    }
    for (auto& t : ch_threads) t.join();
    for (auto& e : ch_err) if (e) std::rethrow_exception(e);
    return ad;
}

inline AudioData decode_file(const std::string& path,
                             const dsd::Options& dsd_opt = {},
                             int want_stream = -1) {
    auto ext = get_extension(path);

#ifdef CRETE_HAS_FFMPEG
    // Native decoders own their formats; everything else falls back to FFmpeg.
    // DSD (.dsf/.dff) is intentionally excluded — FFmpeg's DSD decimation
    // differs from foo_input_sacd's chains and would break DSD parity.
    if (!is_native_format(ext))
        return decode_with_ffmpeg(path, want_stream);
#endif

    if (want_stream >= 0)
        throw std::runtime_error(
            "--stream applies only to multi-stream containers; " + ext +
            " is decoded natively and holds a single audio stream");

    auto buf = read_file(path);

    if (ext == ".wav" || ext == ".rf64" || ext == ".bw64" || ext == ".w64")
        return decode_wave(buf);
    else if (ext == ".aif" || ext == ".aiff")
        return decode_aiff(buf);
    else if (ext == ".flac")
        return decode_flac(buf);
    else if (ext == ".dsf")
        return decode_dsf(buf, dsd_opt);
    else if (ext == ".dff")
        return decode_dff(buf, dsd_opt);
    else
        throw std::runtime_error("Unsupported format: " + ext);
}

} // namespace audio

#ifdef CRETE_HAS_FFMPEG
#include "audio_ffmpeg.hpp"
#endif
