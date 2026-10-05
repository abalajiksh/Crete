#pragma once
// ============================================================================
// dsd_decode.hpp — crête's C++ front-end over libdsddpcm.
//
// Replaces the former in-tree byte-LUT engine (dsd_lut.hpp, now retired to
// dsd_lut.hpp.bak). All DSD -> PCM decimation now goes through the vendored,
// BSD-2-Clause libdsddpcm kernel (third_party/libdsddpcm), which is clean-room
// and shares no code with the LGPL foo_input_sacd oracle. Vendored release:
// v0.2.2 (v0.2.1 added DSD512, v0.2.2 the output bandwidth control).
//
// crête keeps its own container parsing (DSF/DFF) and its per-channel threading:
// each channel is decoded through an independent channels=1 context, which is
// bit-identical to interleaved decode but preserves the existing one-thread-per-
// channel structure in audio.hpp.
//
// ── Library capability matrix (enforced by the kernel — see dsddpcm.c) ───────
//   * Output rates: 44100 / 88200 / 176400 / 352800 Hz (44.1k family).
//   * DIRECT chain: DSD64 -> 44100 only, DEFAULT filter only. Gentle ~30 kHz
//     corner, passes an attenuated top octave. UNSUPPORTED at any other combo.
//   * MULTISTAGE chain: DSD64/128/256/512 -> any of the four rates. This is the
//     measurement-grade path (brickwall at Nyquist for 44100 output).
//   * Filter selection (default / leastsquares / kaiser) applies ONLY at 44100
//     output (the ranked 88.2k->44.1k final stage). Higher rates: DEFAULT only.
//   * Bandwidth (default 50 kHz / full / custom Hz >= 20000) adds a linear-phase
//     low-pass at the end of the chain. It acts only above 48 kHz output and
//     only when the corner is below the output Nyquist, so the default changes
//     176.4 and 352.8 kHz output and nothing else.
//   * DSD1024 and above: NOT supported (input enum tops out at DSD512).
//
// Unsupported combinations throw a std::runtime_error explaining the constraint.
//
// ── Contract inherited from libdsddpcm (locked) ─────────────────────────────
//   * Linear-phase FIR only, constant group delay (queryable, uncompensated).
//   * Unity DC gain; the +6 dB SACD makeup gain is NOT applied.
//   * DSF is LSB-first (msb_first=0), DFF is MSB-first (msb_first=1).
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include <dsddpcm.h>

namespace dsd {

// ── Decimation chain (maps to dsddpcm_chain) ─────────────────────────────────
enum class Mode { Direct, Multistage };

inline const char* mode_name(Mode m) {
    return m == Mode::Direct ? "direct" : "multistage";
}

// Parse "direct" / "multistage" (accepts "d"/"m"). Throws on anything else.
inline Mode mode_from_string(const std::string& s) {
    if (s == "direct"     || s == "d") return Mode::Direct;
    if (s == "multistage" || s == "m") return Mode::Multistage;
    throw std::runtime_error("Unknown DSD mode: " + s + " (expected direct|multistage)");
}

// ── Final-stage filter (maps to dsddpcm_filter; honoured only at 44100 out) ──
enum class Filter { Default, LeastSquares, Kaiser };

inline const char* filter_name(Filter f) {
    switch (f) {
        case Filter::LeastSquares: return "leastsquares";
        case Filter::Kaiser:       return "kaiser";
        case Filter::Default:      default: return "default";
    }
}

// Parse the filter name. "default"/"equiripple" -> Default (equiripple/383 is
// the golden default); "firls"/"leastsquares" -> LeastSquares; "kaiser".
inline Filter filter_from_string(const std::string& s) {
    if (s == "default" || s == "equiripple")     return Filter::Default;
    if (s == "firls"   || s == "leastsquares")   return Filter::LeastSquares;
    if (s == "kaiser")                           return Filter::Kaiser;
    throw std::runtime_error(
        "Unknown DSD filter: " + s + " (expected default|firls|kaiser)");
}

// ── Output bandwidth (dsddpcm_create2's bandwidth_hz; libdsddpcm >= 0.2.2) ───
// The output rate sets how densely the signal is sampled; the bandwidth sets how
// much of it is signal. Without a post-filter, 176.4/352.8 kHz output keeps the
// modulator's shaped ultrasonic noise (at 352.8k everything up to the ~157 kHz
// stage-1 corner), which inflates RMS, peak and DR. Measured on the DSD corpus
// against MAAT (2026-10-05): DSD64 at 352.8k read up to 4.8 dB louder in RMS
// and up to 2.4 dB higher in raw DR (3 integer DR) than with the 50 kHz default.
//   default  50 kHz corner, Scarlet Book Annex D.1 (DSDDPCM_BW_DEFAULT)
//   full     no post-filter, the v0.2.1 behaviour (DSDDPCM_BW_FULL)
//   <Hz>     custom corner, >= 20000
inline bool is_full_bandwidth(double bw) { return bw < 0.0; }

// "default" | "full" | the corner in Hz, as the flag accepts it.
inline std::string bandwidth_name(double bw) {
    if (bw == DSDDPCM_BW_DEFAULT) return "default";
    if (is_full_bandwidth(bw))    return "full";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", bw);
    return buf;
}

// Parse "default" / "full" / a corner in Hz (>= 20000). Throws on anything else.
inline double bandwidth_from_string(const std::string& s) {
    if (s == "default") return DSDDPCM_BW_DEFAULT;
    if (s == "full")    return DSDDPCM_BW_FULL;
    char* end = nullptr;
    const double hz = std::strtod(s.c_str(), &end);
    if (s.empty() || !end || *end || !(hz >= 20000.0) || hz > 1e7)
        throw std::runtime_error(
            "Unknown DSD bandwidth: " + s + " (expected default|full|<Hz> >= 20000)");
    return hz;
}

// ── Decode options ───────────────────────────────────────────────────────────
// Defaults are the measurement-grade path: multistage + default filter to
// 44.1 kHz, which is valid for every supported DSD rate.
struct Options {
    Mode   mode      = Mode::Multistage;
    Filter filter    = Filter::Default;
    int    out_rate  = 44100;                // 44100 | 88200 | 176400 | 352800
    double bandwidth = DSDDPCM_BW_DEFAULT;   // see bandwidth_from_string
};

// True when the bandwidth post-filter actually runs for this output rate
// (libdsddpcm: out_rate > 48000 and the corner below the output Nyquist).
// 50000 mirrors BW_DEFAULT_HZ in dsddpcm.c, which the public header does not
// export; re-check it when re-vendoring.
inline bool bandwidth_applies(const Options& opt) {
    if (is_full_bandwidth(opt.bandwidth) || opt.out_rate <= 48000) return false;
    const double fp = opt.bandwidth == DSDDPCM_BW_DEFAULT ? 50000.0 : opt.bandwidth;
    return fp < opt.out_rate / 2.0;
}

// ── Enum mapping helpers (throwing; used on the decode path) ─────────────────
inline dsddpcm_dsd_rate to_dsd_rate(int dsd_rate_hz) {
    switch (dsd_rate_hz / 44100) {
        case 64:  return DSDDPCM_DSD64;
        case 128: return DSDDPCM_DSD128;
        case 256: return DSDDPCM_DSD256;
        case 512: return DSDDPCM_DSD512;
        default:
            throw std::runtime_error(
                "dsd: unsupported DSD rate " + std::to_string(dsd_rate_hz) +
                " Hz — libdsddpcm supports DSD64/128/256/512 only "
                "(DSD1024 and above are not available)");
    }
}

inline dsddpcm_pcm_rate to_pcm_rate(int out_rate_hz) {
    switch (out_rate_hz) {
        case 44100:  return DSDDPCM_PCM_44100;
        case 88200:  return DSDDPCM_PCM_88200;
        case 176400: return DSDDPCM_PCM_176400;
        case 352800: return DSDDPCM_PCM_352800;
        default:
            throw std::runtime_error(
                "dsd: unsupported output rate " + std::to_string(out_rate_hz) +
                " (expected 44100|88200|176400|352800)");
    }
}

inline dsddpcm_filter to_dsddpcm_filter(Filter f) {
    switch (f) {
        case Filter::LeastSquares: return DSDDPCM_FILTER_LEASTSQUARES;
        case Filter::Kaiser:       return DSDDPCM_FILTER_KAISER;
        case Filter::Default:      default: return DSDDPCM_FILTER_DEFAULT;
    }
}

inline bool is_valid_out_rate(int hz) {
    return hz == 44100 || hz == 88200 || hz == 176400 || hz == 352800;
}

// Populate a config from crête's options. Does not create a context.
inline dsddpcm_config make_config(int dsd_rate_hz, bool lsb_first,
                                  const Options& opt, unsigned channels) {
    dsddpcm_config cfg;
    dsddpcm_status rc = dsddpcm_config_default(
        &cfg, to_dsd_rate(dsd_rate_hz), to_pcm_rate(opt.out_rate), channels);
    if (rc != DSDDPCM_OK)
        throw std::runtime_error(std::string("dsd: config_default failed: ") +
                                 dsddpcm_strerror(rc));
    cfg.chain     = (opt.mode == Mode::Direct) ? DSDDPCM_CHAIN_DIRECT
                                               : DSDDPCM_CHAIN_MULTISTAGE;
    cfg.filter    = to_dsddpcm_filter(opt.filter);
    cfg.format    = DSDDPCM_FMT_F64;
    cfg.msb_first = lsb_first ? 0 : 1;
    return cfg;
}

// Throw (on the calling thread) if this DSD-rate / options combination is not
// supported by libdsddpcm — call once before deinterleaving/threading so the
// error surfaces cleanly instead of aborting inside a worker thread.
inline void validate(int dsd_rate_hz, const Options& opt);

[[noreturn]] inline void throw_config_error(dsddpcm_status rc, int dsd_rate_hz,
                                            const Options& opt) {
    throw std::runtime_error(
        std::string("dsd: libdsddpcm rejected this configuration (") +
        dsddpcm_strerror(rc) + "): DSD" + std::to_string(dsd_rate_hz / 44100) +
        " -> " + std::to_string(opt.out_rate) + " Hz, " + mode_name(opt.mode) +
        " chain, " + filter_name(opt.filter) + " filter, bandwidth " +
        bandwidth_name(opt.bandwidth) + ".\n"
        "  Note: the direct chain is DSD64->44100 only, and filter selection "
        "(firls|kaiser) applies only at 44100 Hz output.");
}

inline void validate(int dsd_rate_hz, const Options& opt) {
    // make_config throws for DSD1024+ / bad out-rate; create2() rejects the rest
    // (e.g. direct at >44100, non-default filter above 44100, bandwidth < 20 kHz).
    dsddpcm_config cfg = make_config(dsd_rate_hz, /*lsb_first=*/true, opt, 1u);
    dsddpcm_ctx* ctx = nullptr;
    dsddpcm_status rc = dsddpcm_create2(&ctx, &cfg, opt.bandwidth);
    if (rc != DSDDPCM_OK)
        throw_config_error(rc, dsd_rate_hz, opt);
    dsddpcm_free(ctx);
}

// ═══════════════════════════════════════════════════════════════════════════
// Decimate one channel of packed DSD to PCM (F64).
//
//   ch_dsd       contiguous packed 1-bit DSD bytes for THIS channel
//   total_bits   valid DSD sample bits (<= ch_dsd.size() * 8)
//   dsd_rate_hz  source DSD rate in Hz (2822400 = DSD64, ...)
//   lsb_first    true for DSF, false for DFF
//   opt          chain / filter / output rate
//
// Runs an independent channels=1 context, so callers keep one thread per
// channel. Decodes the whole per-channel window, then drains the filter tail.
// ═══════════════════════════════════════════════════════════════════════════
inline std::vector<double> decimate_channel(
        const std::vector<uint8_t>& ch_dsd,
        size_t total_bits,
        int dsd_rate_hz,
        bool lsb_first,
        const Options& opt = {}) {

    dsddpcm_config cfg = make_config(dsd_rate_hz, lsb_first, opt, 1u);

    dsddpcm_ctx* ctx = nullptr;
    dsddpcm_status rc = dsddpcm_create2(&ctx, &cfg, opt.bandwidth);
    if (rc != DSDDPCM_OK)
        throw_config_error(rc, dsd_rate_hz, opt);

    // DSD is byte-aligned; feed floor(total_bits / 8) whole bytes.
    const size_t num_bytes = std::min(ch_dsd.size(), total_bits / 8);

    std::vector<double> pcm;
    try {
        if (num_bytes > 0) {
            const size_t cap = dsddpcm_max_output_frames(ctx, num_bytes);
            pcm.resize(cap);
            size_t got = 0;
            rc = dsddpcm_decode(ctx, ch_dsd.data(), num_bytes,
                                pcm.data(), cap, &got);
            if (rc != DSDDPCM_OK)
                throw_config_error(rc, dsd_rate_hz, opt);
            pcm.resize(got);
        }

        // Drain the linear-phase filter tail at end-of-stream. flush() writes up
        // to `chunk` frames per call and returns ERR_STATE once fully drained.
        for (;;) {
            const size_t chunk = 8192;
            const size_t before = pcm.size();
            pcm.resize(before + chunk);
            size_t got = 0;
            rc = dsddpcm_flush(ctx, pcm.data() + before, chunk, &got);
            if (rc != DSDDPCM_OK) { pcm.resize(before); break; }
            pcm.resize(before + got);
            if (got < chunk) break;
        }
    } catch (...) {
        dsddpcm_free(ctx);
        throw;
    }

    dsddpcm_free(ctx);
    return pcm;
}

// ── Human-readable chain summary for --version, e.g.
//    "multistage, /64, 4478 taps, gd 104.0 smp"  or  "unsupported". ───────────
inline std::string describe_chain(const Options& opt, int dsd_rate_hz) {
    try {
        dsddpcm_config cfg = make_config(dsd_rate_hz, /*lsb_first=*/true, opt, 1u);
        dsddpcm_ctx* ctx = nullptr;
        if (dsddpcm_create2(&ctx, &cfg, opt.bandwidth) != DSDDPCM_OK)
            return "unsupported";
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s, /%u, %u taps, gd %.1f smp",
                      mode_name(opt.mode), dsddpcm_decimation(ctx),
                      dsddpcm_num_taps(ctx), dsddpcm_group_delay(ctx));
        dsddpcm_free(ctx);
        return buf;
    } catch (...) {
        return "unsupported";
    }
}

} // namespace dsd
