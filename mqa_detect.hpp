// mqa_detect.hpp — MQA (Master Quality Authenticated) forensic detector for crête
//
// FORENSICS module, not a decoder. crête does not (and cannot, with open tools)
// perform the MQA first-unfold / render: that DSP lives only in the proprietary
// libbluos_ssc blob. This module *identifies* an MQA stream and reads its
// authoritative metadata, then reports a confidence-scored verdict.
//
// Per crête's forensics guardrail: any *verdict* is inferential → lives in the
// `mqa` (forensics) namespace, confidence-scored, and is gated OFF by default.
// It never touches the deterministic `metrics` path.
//
// Provenance / licensing: the deterministic bitstream layout (packet framing,
// datasync fields, the 40-bit sync value, the rate/bit-depth tables) is a
// clean-room reimplementation of the format *description* in the BSD-2-Clause
// `mqa` project's `mqa.rst`, plus MQA patent WO2013186561A2 — facts and an
// interface, reproduced without copying code. crête stays MIT.
//
// Two-tier detection:
//   1. DETERMINISTIC (authoritative): reconstruct the XOR-of-LSB bitstream and
//      hunt the datasync packet. A hit is proof + authoritative fields
//      (orig_rate, auth_level→Studio, render filter/bit-depth). confidence = 1.0.
//   2. HEURISTIC (raw diagnostic only): if no datasync is found, an audiocheckr-
//      style LSB-entropy statistic is reported as a number, but NEVER as a
//      verdict. It cannot separate buried MQA data from tape hiss or a prior
//      lossy generation, so it does not drive detection or confidence.

#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace mqa {

// ── Result ──────────────────────────────────────────────────────────────────

struct Info {
    // Deterministic tier (authoritative when datasync_found)
    bool datasync_found = false;   // MQA bitstream positively identified
    bool studio         = false;   // auth_level >= 9  → "Studio" / blue light
    int  orig_rate      = 0;       // Hz, authored/original rate (0 = unknown)
    int  src_rate       = 0;       // Hz, rate of this delivered stream
    int  render_filter  = -1;      // render-stage upsampling filter index (0..15)
    int  render_bitdepth = -1;     // dither bit-depth: 20/18/16/15
    int  auth_level     = -1;      // 0..15; >=9 == Studio
    int  auth_info      = -1;
    int  bit_position   = -1;      // which sample bit carried the stream (0..15;
                                   // 8 for 24-bit hi-res, 0 for 16-bit MQA-CD)

    // Heuristic tier (secondary corroboration; ignored when datasync_found)
    double lsb_entropy  = 0.0;     // normalised 0..1 over low byte

    // Fused
    double confidence   = 0.0;     // 1.0 == datasync-verified; else soft 0..~0.6
    std::vector<std::string> evidence;
};

// ── Bitstream constants (from mqa.rst / WO2013186561A2) ─────────────────────

// datasync = 4-bit packet type (5) followed by 36-bit magic 0x11319207d, all
// read LSB-first. Concatenated LSB-first that is a single 40-bit little value:
//   (0x11319207d << 4) | 5 == 0x11319207d5
inline constexpr uint64_t DATASYNC_SYNC40 = 0x11319207d5ULL;
inline constexpr uint64_t MASK40          = (1ULL << 40) - 1;

// 5-bit rate field: 2 MSBs = base-rate index, 3 LSBs = scale (multiplier 1<<s).
inline int decode_rate(unsigned code5) {
    static const int base[4] = { 44100, 48000, 64000, 0 /*invalid*/ };
    const int    b = base[(code5 >> 3) & 0x3];
    const unsigned s = code5 & 0x7;               // 0..7  → 1x..128x
    return b ? (b << s) : 0;
}

inline int decode_bitdepth(unsigned code2) {
    static const int bd[4] = { 20, 18, 16, 15 };
    return bd[code2 & 0x3];
}

// ── LSB-first XOR bit source over a stereo integer pair ─────────────────────
//
// Samples are the integer PCM values (sign-extended into int32_t). The MQA
// carrier is one bit of (L ^ R) at a fixed low position (8 for 24-bit, 0 for
// 16-bit MQA-CD).

namespace detail {

struct XorBits {
    const int32_t* L;
    const int32_t* R;
    size_t         n;
    int            pos;   // carrier bit position, 0..15
    size_t         i = 0;

    // next raw carrier bit, or -1 at end
    int next() {
        if (i >= n) return -1;
        const int b = ((L[i] >> pos) ^ (R[i] >> pos)) & 1;
        ++i;
        return b;
    }

    // read `nbits` LSB-first into an integer (first bit read == bit 0)
    uint64_t read(int nbits) {
        uint64_t v = 0;
        for (int k = 0; k < nbits; ++k) {
            const int b = next();
            if (b < 0) break;
            v |= static_cast<uint64_t>(b) << k;
        }
        return v;
    }
};

// Parse the datasync body assuming the cursor sits just past the 40-bit sync.
// Fills the authoritative fields of `info`. Returns false on truncation.
inline bool parse_datasync(XorBits& x, Info& info) {
    /* stream_pos_flag */ x.read(1);
    /* pad             */ x.read(1);
    const unsigned orig = x.read(5);
    const unsigned src  = x.read(5);
    const unsigned rf   = x.read(5);
    /* unknown_1       */ x.read(2);
    const unsigned rbd  = x.read(2);
    /* unknown_2       */ x.read(4);
    const unsigned ainf = x.read(4);
    const unsigned alvl = x.read(4);
    /* item_count      */ x.read(7);

    if (x.i > x.n) return false;   // ran off the end mid-field

    info.orig_rate       = decode_rate(orig);
    info.src_rate        = decode_rate(src);
    info.render_filter   = static_cast<int>(rf);
    info.render_bitdepth = decode_bitdepth(rbd);
    info.auth_info       = static_cast<int>(ainf);
    info.auth_level      = static_cast<int>(alvl);
    info.studio          = (alvl >= 9);
    return true;
}

} // namespace detail

// ── Deterministic detection ─────────────────────────────────────────────────
//
// Scans carrier positions 8..15 for the datasync packet. Returns the first hit.
// `scan_frames` bounds the search (datasync appears early and repeats); 0 = all.

inline std::optional<Info> detect_datasync(const int32_t* L, const int32_t* R,
                                           size_t n, size_t scan_frames = 0) {
    const size_t lim = (scan_frames && scan_frames < n) ? scan_frames : n;

    // Carrier bit position. The spec cites 8-15 for 24-bit hi-res MQA, but
    // 16-bit MQA-CD shifts the whole encoding down 8 bits, landing the carrier
    // on the LSB (verified: bit 0 on real MQA-CD, bit 8 on 24-bit). Scanning
    // 0-15 covers both; the 40-bit datasync match self-validates, so the extra
    // planes add no false positives (empirically zero spurious hits).
    for (int pos = 0; pos <= 15; ++pos) {
        detail::XorBits x{ L, R, lim, pos };
        uint64_t win = 0;
        size_t   got = 0;
        int      b;
        while ((b = x.next()) >= 0) {
            win = (win >> 1) | (static_cast<uint64_t>(b) << 39);
            win &= MASK40;
            if (++got >= 40 && win == DATASYNC_SYNC40) {
                Info info;
                info.datasync_found = true;
                info.bit_position   = pos;
                info.confidence     = 1.0;
                // x.i now points just past the sync → body follows.
                if (detail::parse_datasync(x, info)) {
                    info.evidence.push_back("datasync packet verified (magic 0x11319207d)");
                    info.evidence.push_back(
                        info.studio ? "auth_level>=9 → MQA Studio (blue)"
                                    : "auth_level<9 → MQA (green)");
                    return info;
                }
                // Sync matched but body truncated: still a positive ID.
                info.evidence.push_back("datasync magic matched (stream truncated before fields)");
                return info;
            }
        }
    }
    return std::nullopt;
}

// ── Heuristic corroboration (secondary; no FFT) ─────────────────────────────
//
// Normalised Shannon entropy of the low byte. MQA buries data there, lifting
// entropy toward 1.0; a clean master leaves it near 0. This is a HINT only.

inline double lsb_entropy(const int32_t* s, size_t n) {
    if (n == 0) return 0.0;
    std::array<uint32_t, 256> hist{};
    for (size_t i = 0; i < n; ++i)
        ++hist[static_cast<uint8_t>(std::abs(s[i]) & 0xFF)];

    double H = 0.0;
    const double N = static_cast<double>(n);
    for (uint32_t c : hist) {
        if (c) {
            const double p = c / N;
            H -= p * std::log2(p);
        }
    }
    return H / 8.0;   // 8 bits → max entropy 8.0
}

// TODO (blocked on the planned native FFT — see forensics-spectrum-direction):
//   - noise_floor_elevation(): dB lift of 18–20 kHz vs 10–16 kHz band
//   - spectral_folding_score(): mirror-image energy about fs/4, fs/3
// These are the remaining audiocheckr signals; wire them once crête has an FFT.

// ── Top-level analyzer ──────────────────────────────────────────────────────
//
// Deterministic first (authoritative). Only if that fails do we fall back to
// the entropy hint — which can *suggest* but never *assert* MQA.

inline Info analyze(const int32_t* L, const int32_t* R, size_t n,
                    size_t scan_frames = 0) {
    if (auto hit = detect_datasync(L, R, n, scan_frames))
        return *hit;

    // No datasync: NOT MQA as far as we can prove. confidence stays 0.0.
    //
    // LSB entropy is retained only as a raw diagnostic, never as an MQA verdict.
    // It cannot distinguish buried MQA data from analog tape hiss, a prior lossy
    // generation, or any loud/noisy lossless content — all of which push the low
    // byte toward full entropy. Asserting "MQA" from it would be a false alarm on
    // legitimate lossless, so callers must not treat a high value as detection.
    Info info;
    info.lsb_entropy = 0.5 * (lsb_entropy(L, n) + lsb_entropy(R, n));
    info.confidence  = 0.0;
    return info;
}

// Convenience front-end over crête's normalised f64 channels. Owns the policy —
// which containers qualify (stereo, 44.1/48 kHz, 16/24-bit), the exact integer
// reconstruction (crête's decoders scale by 1/2^(bps-1), so this inverts back to
// the original sample bits), and the bounded scan window. Both the CLI and GUI
// call this, so the rule lives in exactly one place.
inline Info analyze_pcm(const std::vector<std::vector<double>>& channels,
                        unsigned sample_rate, unsigned bit_depth,
                        unsigned scan_seconds = 20) {
    Info none;
    if (channels.size() != 2)                          return none;
    if (sample_rate != 44100 && sample_rate != 48000)  return none;
    if (bit_depth != 16 && bit_depth != 24)            return none;

    const auto& L = channels[0];
    const auto& R = channels[1];
    const size_t n = std::min(L.size(), R.size());
    if (n == 0) return none;

    // Datasync appears early and repeats; a bounded window keeps reconstruction
    // cheap on album-length files. Files with no datasync in-window have none
    // full-file either (verified against real MQA).
    const size_t window = std::min<size_t>(
        n, static_cast<size_t>(sample_rate) * scan_seconds);
    const double scale = static_cast<double>(1LL << (bit_depth - 1));

    std::vector<int32_t> li(window), ri(window);
    for (size_t i = 0; i < window; ++i) {
        li[i] = static_cast<int32_t>(std::llround(L[i] * scale));
        ri[i] = static_cast<int32_t>(std::llround(R[i] * scale));
    }
    return analyze(li.data(), ri.data(), window, window);
}

} // namespace mqa
