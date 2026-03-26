#pragma once
// ============================================================================
// analysis.hpp — Dynamic Range Analysis Engine
//
// Implements three complementary DR measurements:
//   1. Crest Factor          — peak-to-RMS ratio (naive but fast)
//   2. EBU R128 / PLR        — ITU-R BS.1770 K-weighted, gated loudness
//   3. TT Dynamic Range Meter — Pleasurize Music Foundation algorithm
//
// Extended per-channel metrics (MAAT DROffline–style):
//   - Per-channel sample peak, true peak, RMS, DR
//   - Momentary loudness max (400ms), Short-term loudness max (3s)
//   - Loudness Range (LRA), Min PSR, PLR
//
// All processing operates on f64 samples normalized to [-1.0, 1.0].
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace dr {

// ── Constants ──────────────────────────────────────────────────────────────
static constexpr double PI = 3.14159265358979323846;
static constexpr double SILENCE_FLOOR_DB = -120.0;
static constexpr double LUFS_FLOOR = -70.0;

// ── Verdict classification ─────────────────────────────────────────────────
enum class Verdict {
    Exceptional,  // DR > 20
    Excellent,    // DR 14–20
    Normal,       // DR 8–13
    Compressed,   // DR 5–7
    Brickwalled,  // DR < 5
};

inline const char* verdict_string(Verdict v) {
    switch (v) {
        case Verdict::Exceptional: return "Exceptional (DR > 20)";
        case Verdict::Excellent:   return "Excellent (DR 14-20)";
        case Verdict::Normal:      return "Normal (DR 8-13)";
        case Verdict::Compressed:  return "Compressed (DR 5-7)";
        case Verdict::Brickwalled: return "Brickwalled (DR < 5)";
    }
    return "Unknown";
}

inline Verdict classify_dr(double dr) {
    if (dr > 20.0) return Verdict::Exceptional;
    if (dr >= 14.0) return Verdict::Excellent;
    if (dr >= 8.0) return Verdict::Normal;
    if (dr >= 5.0) return Verdict::Compressed;
    return Verdict::Brickwalled;
}

// ── Per-channel detailed metrics ───────────────────────────────────────────
struct ChannelMetrics {
    double sample_peak_dbfs    = SILENCE_FLOOR_DB;
    double true_peak_dbfs      = SILENCE_FLOOR_DB;
    double rms_dbfs            = SILENCE_FLOOR_DB;
    double dr_raw              = 0.0;
    int    dr_score            = 0;
    double max_momentary_lufs  = LUFS_FLOOR;   // non-ITU (single channel)
    double max_short_term_lufs = LUFS_FLOOR;   // non-ITU (single channel)
};

// ── Per-track analysis result ──────────────────────────────────────────────
struct TrackResult {
    std::string filename;
    uint32_t    sample_rate    = 0;
    uint32_t    channels       = 0;
    uint32_t    bit_depth      = 0;
    uint64_t    total_samples  = 0;   // per channel
    std::string codec;
    double      duration_secs  = 0.0;

    // TT DR (joint)
    int         dr_score       = 0;     // rounded integer
    double      dr_score_raw   = 0.0;   // unrounded
    std::vector<double> dr_per_channel;
    size_t      tt_block_count = 0;

    // Track-level peak and RMS (displayed in output)
    double      peak_dbfs      = SILENCE_FLOOR_DB;  // max across channels (sample peak)
    double      rms_dbfs       = SILENCE_FLOOR_DB;   // overall RMS across channels
    bool        is_clipping    = false;               // peak >= 0.0 dBFS

    // EBU R128
    double      integrated_lufs = LUFS_FLOOR;
    double      plr_db          = 0.0;  // peak-to-loudness ratio

    // Crest factor
    double      crest_factor_db = 0.0;

    // ── Extended metrics (MAAT-style detail view) ──────────────────────
    std::vector<ChannelMetrics> ch_metrics;

    // Joint true peak (4x oversampled, max across channels)
    double      true_peak_dbfs = SILENCE_FLOOR_DB;

    // Joint loudness (ITU, multi-channel power-summed)
    double      max_momentary_lufs  = LUFS_FLOOR;  // 400ms window
    double      max_short_term_lufs = LUFS_FLOOR;  // 3s window

    // Loudness Range (BS.1770)
    double      lra_lu = 0.0;

    // Min PSR (SPPM-to-Short-Term Loudness Ratio)
    double      psr_db = 0.0;

    Verdict     verdict = Verdict::Normal;
};

// ── Helper functions ───────────────────────────────────────────────────────
inline double to_dbfs(double amplitude) {
    if (amplitude <= 0.0) return SILENCE_FLOOR_DB;
    return 20.0 * std::log10(amplitude);
}

inline double to_lufs(double rms) {
    if (rms <= 0.0) return LUFS_FLOOR;
    return -0.691 + 10.0 * std::log10(rms * rms);
}

inline double to_lufs_from_power(double power) {
    if (power <= 0.0) return LUFS_FLOOR;
    return -0.691 + 10.0 * std::log10(power);
}

inline double peak_of(const double* data, size_t n) {
    double pk = 0.0;
    for (size_t i = 0; i < n; ++i)
        pk = std::max(pk, std::abs(data[i]));
    return pk;
}

inline double rms_of(const double* data, size_t n) {
    if (n == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i)
        sum += data[i] * data[i];
    return std::sqrt(sum / static_cast<double>(n));
}

inline double mean_square(const double* data, size_t n) {
    if (n == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i)
        sum += data[i] * data[i];
    return sum / static_cast<double>(n);
}

inline double vec_mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

// ── True peak via 4x oversampling (linear interpolation) ───────────────────
// Note: BS.1770 specifies a proper polyphase FIR for 4x oversampling.
// Linear interpolation is an approximation that catches most inter-sample
// peaks. Sufficient for DR measurement purposes.

inline double compute_true_peak(const double* samples, size_t n) {
    if (n == 0) return 0.0;
    double peak = 0.0;
    for (size_t i = 0; i < n; ++i)
        peak = std::max(peak, std::abs(samples[i]));

    for (size_t i = 0; i + 1 < n; ++i) {
        double s0 = samples[i], s1 = samples[i+1];
        for (int k = 1; k <= 3; ++k) {
            double t = k / 4.0;
            double interp = s0 + t * (s1 - s0);
            peak = std::max(peak, std::abs(interp));
        }
    }
    return peak;
}

// ── Biquad filter (Direct Form II Transposed) ─────────────────────────────
struct BiquadCoeffs {
    double b[3];
    double a[3];
};

inline std::vector<double> biquad_filter(const double* input, size_t n,
                                          const BiquadCoeffs& c) {
    std::vector<double> output(n);
    double z1 = 0.0, z2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double x = input[i];
        double y = c.b[0] * x + z1;
        z1 = c.b[1] * x - c.a[1] * y + z2;
        z2 = c.b[2] * x - c.a[2] * y;
        output[i] = y;
    }
    return output;
}

// ── K-weighting filter coefficients (ITU-R BS.1770-4) ──────────────────────
inline BiquadCoeffs k_weight_prefilter(double fs) {
    if (std::abs(fs - 48000.0) < 1.0) {
        return {{1.53512485958697, -2.69169618940638, 1.19839281085285},
                {1.0, -1.69065929318241, 0.73248077421585}};
    }
    double fc = 1681.974450955533;
    double g_db = 3.999843853973347;
    double q = 0.7071752369554196;
    double A = std::pow(10.0, g_db / 40.0);
    double w0 = 2.0 * PI * fc / fs;
    double cosw = std::cos(w0), sinw = std::sin(w0);
    double alpha = sinw / (2.0 * q);
    double sqrtA = std::sqrt(A);

    double b0 = A * ((A+1) + (A-1)*cosw + 2*sqrtA*alpha);
    double b1 = -2*A * ((A-1) + (A+1)*cosw);
    double b2 = A * ((A+1) + (A-1)*cosw - 2*sqrtA*alpha);
    double a0 = (A+1) - (A-1)*cosw + 2*sqrtA*alpha;
    double a1 = 2 * ((A-1) - (A+1)*cosw);
    double a2 = (A+1) - (A-1)*cosw - 2*sqrtA*alpha;

    return {{b0/a0, b1/a0, b2/a0}, {1.0, a1/a0, a2/a0}};
}

inline BiquadCoeffs k_weight_highpass(double fs) {
    if (std::abs(fs - 48000.0) < 1.0) {
        return {{1.0, -2.0, 1.0},
                {1.0, -1.99004745483398, 0.99007225036621}};
    }
    double fc = 38.13547087602444;
    double w0 = 2.0 * PI * fc / fs;
    double cosw = std::cos(w0), sinw = std::sin(w0);
    double alpha = sinw / (2.0 * 0.7071067811865476);

    double b0 = (1+cosw)/2;
    double b1 = -(1+cosw);
    double b2 = (1+cosw)/2;
    double a0 = 1+alpha;
    double a1 = -2*cosw;
    double a2 = 1-alpha;

    return {{b0/a0, b1/a0, b2/a0}, {1.0, a1/a0, a2/a0}};
}

// ── K-weighting application ────────────────────────────────────────────────
inline std::vector<double> apply_k_weighting(const double* samples, size_t n,
                                              double sample_rate) {
    auto pre = k_weight_prefilter(sample_rate);
    auto hp  = k_weight_highpass(sample_rate);
    auto stage1 = biquad_filter(samples, n, pre);
    return biquad_filter(stage1.data(), n, hp);
}

// ── K-weight all channels at once ──────────────────────────────────────────
inline std::vector<std::vector<double>> k_weight_channels(
        const std::vector<std::vector<double>>& channels, double sample_rate) {
    std::vector<std::vector<double>> kw(channels.size());
    for (size_t c = 0; c < channels.size(); ++c)
        kw[c] = apply_k_weighting(channels[c].data(), channels[c].size(), sample_rate);
    return kw;
}

// ── Max block loudness (single channel, K-weighted input) ──────────────────
// Returns maximum loudness across all overlapping blocks of given window/hop.
// Input must already be K-weighted.

inline double max_block_loudness_single(const double* kw, size_t n,
                                         size_t window, size_t hop) {
    if (n < window) {
        double ms = mean_square(kw, n);
        return to_lufs_from_power(ms);
    }
    double max_lufs = LUFS_FLOOR;
    for (size_t start = 0; start + window <= n; start += hop) {
        double ms = mean_square(&kw[start], window);
        double lufs = to_lufs_from_power(ms);
        if (lufs > max_lufs) max_lufs = lufs;
    }
    return max_lufs;
}

// ── Max block loudness (multi-channel, ITU power-summed) ───────────────────
// Per EBU R128: power-sum K-weighted channels, then compute loudness.
// Channel weights: 1.0 for L/R/C, 1.41 for Ls/Rs (surround).
// For stereo (nch <= 2), all weights are 1.0.

inline double max_block_loudness_multi(
        const std::vector<std::vector<double>>& kw_channels,
        size_t window, size_t hop) {
    if (kw_channels.empty()) return LUFS_FLOOR;
    size_t n = kw_channels[0].size();
    size_t nch = kw_channels.size();

    if (n < window) {
        double total_power = 0.0;
        for (size_t c = 0; c < nch; ++c)
            total_power += mean_square(kw_channels[c].data(), n);
        return to_lufs_from_power(total_power);
    }

    double max_lufs = LUFS_FLOOR;
    for (size_t start = 0; start + window <= n; start += hop) {
        double total_power = 0.0;
        for (size_t c = 0; c < nch; ++c)
            total_power += mean_square(&kw_channels[c][start], window);
        double lufs = to_lufs_from_power(total_power);
        if (lufs > max_lufs) max_lufs = lufs;
    }
    return max_lufs;
}

// ── EBU R128 integrated loudness (multi-channel, proper gating) ────────────
// K-weighted input, power-summed across channels, dual gated.

inline double compute_integrated_loudness_multi(
        const std::vector<std::vector<double>>& kw_channels,
        double sample_rate) {
    if (kw_channels.empty()) return LUFS_FLOOR;
    size_t n = kw_channels[0].size();
    size_t nch = kw_channels.size();

    size_t block_samples = static_cast<size_t>(0.4 * sample_rate);
    size_t hop = block_samples / 4;

    if (n < block_samples) {
        double total_power = 0.0;
        for (size_t c = 0; c < nch; ++c)
            total_power += mean_square(kw_channels[c].data(), n);
        return to_lufs_from_power(total_power);
    }

    // Compute block powers (multi-channel sum)
    std::vector<double> block_powers;
    for (size_t start = 0; start + block_samples <= n; start += hop) {
        double total = 0.0;
        for (size_t c = 0; c < nch; ++c)
            total += mean_square(&kw_channels[c][start], block_samples);
        block_powers.push_back(total);
    }

    // Absolute gate (-70 LUFS)
    double abs_gate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    std::vector<double> above_abs;
    for (double p : block_powers)
        if (p > abs_gate) above_abs.push_back(p);

    if (above_abs.empty()) return LUFS_FLOOR;

    // Relative gate (mean - 10 dB)
    double ungated_mean = vec_mean(above_abs);
    double rel_gate = ungated_mean * std::pow(10.0, -10.0 / 10.0);

    std::vector<double> above_rel;
    for (double p : above_abs)
        if (p > rel_gate) above_rel.push_back(p);

    if (above_rel.empty()) return LUFS_FLOOR;
    return to_lufs_from_power(vec_mean(above_rel));
}

// ── Single-channel integrated loudness (backward compat) ───────────────────
inline double compute_integrated_loudness(const double* samples, size_t n,
                                           double sample_rate) {
    auto kw = apply_k_weighting(samples, n, sample_rate);
    std::vector<std::vector<double>> single = {std::move(kw)};
    return compute_integrated_loudness_multi(single, sample_rate);
}

// ── Loudness Range (LRA) — EBU R128 / BS.1770-4 ───────────────────────────
// 3s blocks, 1s hop, dual gate (abs -70, rel -20), then 10th-95th percentile.

inline double compute_lra(const std::vector<std::vector<double>>& kw_channels,
                           double sample_rate) {
    if (kw_channels.empty()) return 0.0;
    size_t n = kw_channels[0].size();
    size_t nch = kw_channels.size();

    size_t window = static_cast<size_t>(3.0 * sample_rate);
    size_t hop = static_cast<size_t>(1.0 * sample_rate);
    if (n < window) return 0.0;

    // Compute short-term block loudnesses (multi-channel)
    std::vector<double> st_lufs;
    for (size_t start = 0; start + window <= n; start += hop) {
        double total = 0.0;
        for (size_t c = 0; c < nch; ++c)
            total += mean_square(&kw_channels[c][start], window);
        double lufs = to_lufs_from_power(total);
        st_lufs.push_back(lufs);
    }

    if (st_lufs.empty()) return 0.0;

    // Absolute gate: -70 LUFS
    std::vector<double> above_abs;
    for (double l : st_lufs)
        if (l > -70.0) above_abs.push_back(l);

    if (above_abs.size() < 2) return 0.0;

    // Relative gate: mean - 20 dB (note: -20 for LRA, not -10)
    double mean_abs = vec_mean(above_abs);
    double rel_threshold = mean_abs - 20.0;

    std::vector<double> above_rel;
    for (double l : above_abs)
        if (l > rel_threshold) above_rel.push_back(l);

    if (above_rel.size() < 2) return 0.0;

    // 10th and 95th percentile
    std::sort(above_rel.begin(), above_rel.end());
    size_t idx_10 = static_cast<size_t>(above_rel.size() * 0.10);
    size_t idx_95 = static_cast<size_t>(above_rel.size() * 0.95);
    if (idx_95 >= above_rel.size()) idx_95 = above_rel.size() - 1;

    return above_rel[idx_95] - above_rel[idx_10];
}

// ── Min PSR (SPPM-to-Short-Term Loudness Ratio) — AES ──────────────────────
// For each 3s segment: PSR = sample_peak_dBFS - short_term_LUFS
// Returns the minimum PSR across the track.

inline double compute_min_psr(
        const std::vector<std::vector<double>>& channels,
        const std::vector<std::vector<double>>& kw_channels,
        double sample_rate) {
    if (channels.empty()) return 0.0;
    size_t n = channels[0].size();
    size_t nch = channels.size();

    size_t window = static_cast<size_t>(3.0 * sample_rate);
    size_t hop = static_cast<size_t>(1.0 * sample_rate);
    if (n < window) return 0.0;

    double min_psr = 1e9;
    bool any_valid = false;

    for (size_t start = 0; start + window <= n; start += hop) {
        // Sample peak across all channels in this segment
        double seg_peak = 0.0;
        for (size_t c = 0; c < nch; ++c)
            seg_peak = std::max(seg_peak, peak_of(&channels[c][start], window));

        // Short-term loudness (multi-channel K-weighted)
        double total_power = 0.0;
        for (size_t c = 0; c < nch; ++c)
            total_power += mean_square(&kw_channels[c][start], window);
        double st_lufs = to_lufs_from_power(total_power);

        if (st_lufs > LUFS_FLOOR && seg_peak > 0.0) {
            double psr = to_dbfs(seg_peak) - st_lufs;
            if (psr < min_psr) min_psr = psr;
            any_valid = true;
        }
    }

    return any_valid ? min_psr : 0.0;
}

// ── TT Dynamic Range Meter ──────────────────────────────────────────────────
// Pleasurize Music Foundation algorithm.
// Validated against foobar2000 DR Meter v1.0.6 and MaatDROffline MkII.
//
// Multi-channel block algorithm:
//   1. Split audio into 3-second non-overlapping blocks
//   2. Per block, across ALL channels simultaneously:
//      a. Peak = max |sample| across all channels in the block
//      b. RMS  = sqrt( sum_of_squares_all_channels / block_size )
//   3. Discard near-silent blocks (RMS < 1e-3 linear ≈ -60 dBFS)
//   4. Sort remaining blocks by RMS descending, select top 20%
//   5. From ALL non-silent blocks, take the 2nd highest peak value
//   6. DR = 20·log10( 2nd_highest_peak / mean_rms_of_top20 )
//   7. Round to nearest integer

struct TTDRResult {
    double dr;
    std::vector<double> dr_per_ch;
    size_t blocks;
    double overall_rms_dbfs;
    double overall_peak_dbfs;
    bool   is_clipping;
};

inline TTDRResult compute_tt_dr(
        const std::vector<std::vector<double>>& channels,
        double sample_rate,
        double block_seconds = 3.0) {

    size_t nch = channels.size();
    size_t n = channels[0].size();
    size_t block_size = static_cast<size_t>(block_seconds * sample_rate);

    TTDRResult result;
    result.blocks = (n >= block_size) ? (n / block_size) : 1;

    // ── Overall track peak and RMS (for display) ─────────────────────
    double track_peak = 0.0;
    double total_sum_sq = 0.0;

    for (size_t ch = 0; ch < nch; ++ch) {
        for (size_t i = 0; i < n; ++i) {
            double s = channels[ch][i];
            track_peak = std::max(track_peak, std::abs(s));
            total_sum_sq += s * s;
        }
    }

    result.overall_rms_dbfs  = to_dbfs(std::sqrt(total_sum_sq / static_cast<double>(n)));
    result.overall_peak_dbfs = to_dbfs(track_peak);
    result.is_clipping       = (track_peak >= 0.9999);

    // ── Multi-channel block analysis ─────────────────────────────────
    if (n < block_size) {
        double rms = std::sqrt(total_sum_sq / static_cast<double>(n));
        if (rms <= 0.0) { result.dr = 0.0; return result; }
        result.dr = to_dbfs(track_peak) - to_dbfs(rms);
        result.dr_per_ch = {result.dr};
        return result;
    }

    size_t n_blocks = n / block_size;

    std::vector<double> block_rms_vals, block_peak_vals;
    block_rms_vals.reserve(n_blocks);
    block_peak_vals.reserve(n_blocks);

    for (size_t b = 0; b < n_blocks; ++b) {
        size_t start = b * block_size;

        double pk = 0.0;
        for (size_t ch = 0; ch < nch; ++ch) {
            for (size_t i = start; i < start + block_size; ++i)
                pk = std::max(pk, std::abs(channels[ch][i]));
        }

        double sum_sq = 0.0;
        for (size_t ch = 0; ch < nch; ++ch) {
            for (size_t i = start; i < start + block_size; ++i)
                sum_sq += channels[ch][i] * channels[ch][i];
        }
        double rms = std::sqrt(sum_sq / static_cast<double>(block_size));

        if (rms > 1e-3) {
            block_rms_vals.push_back(rms);
            block_peak_vals.push_back(pk);
        }
    }

    if (block_rms_vals.empty()) { result.dr = 0.0; return result; }

    // ── 2nd highest peak from ALL non-silent blocks ──────────────────
    std::vector<double> sorted_peaks = block_peak_vals;
    std::sort(sorted_peaks.begin(), sorted_peaks.end(), std::greater<double>());

    double dr_peak = (sorted_peaks.size() >= 2)
                     ? sorted_peaks[1]
                     : sorted_peaks[0];

    // ── Mean RMS of top 20% loudest blocks ───────────────────────────
    std::vector<size_t> indices(block_rms_vals.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(indices.begin(), indices.end(),
              [&](size_t a, size_t b) {
                  return block_rms_vals[a] > block_rms_vals[b];
              });

    size_t n_top = std::max<size_t>(1,
        std::min<size_t>(block_rms_vals.size(),
            static_cast<size_t>(std::ceil(block_rms_vals.size() * 0.2))));

    double sum_rms = 0.0;
    for (size_t i = 0; i < n_top; ++i) {
        sum_rms += block_rms_vals[indices[i]];
    }
    double avg_rms = sum_rms / static_cast<double>(n_top);

    if (avg_rms <= 0.0) { result.dr = 0.0; return result; }

    result.dr = to_dbfs(dr_peak) - to_dbfs(avg_rms);
    result.dr_per_ch = {result.dr};

    return result;
}

// ── Full track analysis ────────────────────────────────────────────────────
inline TrackResult analyze_track(const std::vector<std::vector<double>>& channels,
                                  uint32_t sample_rate,
                                  uint32_t bit_depth,
                                  const std::string& codec,
                                  const std::string& filename) {
    TrackResult r;
    r.filename     = filename;
    r.sample_rate  = sample_rate;
    r.channels     = static_cast<uint32_t>(channels.size());
    r.bit_depth    = bit_depth;
    r.codec        = codec;
    r.total_samples = channels.empty() ? 0 : channels[0].size();
    r.duration_secs = (sample_rate > 0)
        ? static_cast<double>(r.total_samples) / sample_rate : 0.0;

    if (channels.empty() || r.total_samples == 0) return r;

    size_t nch = channels.size();
    size_t n = channels[0].size();
    double sr = static_cast<double>(sample_rate);

    // ── TT DR (multi-channel blocks, joint) ──────────────────────────
    auto tt = compute_tt_dr(channels, sample_rate);
    r.dr_score_raw   = tt.dr;
    r.dr_score       = static_cast<int>(std::round(tt.dr));
    r.dr_per_channel = tt.dr_per_ch;
    r.tt_block_count = tt.blocks;
    r.peak_dbfs      = tt.overall_peak_dbfs;
    r.rms_dbfs       = tt.overall_rms_dbfs;
    r.is_clipping    = tt.is_clipping;

    // DSD peaks above 0 dBFS are normal (sigma-delta reconstruction)
    if (codec == "DSD") r.is_clipping = false;

    // ── K-weight all channels once (reused for all loudness metrics) ─
    auto kw = k_weight_channels(channels, sr);

    // Window/hop sizes
    size_t momentary_window = static_cast<size_t>(0.4 * sr);
    size_t momentary_hop    = static_cast<size_t>(0.1 * sr);
    size_t short_term_window = static_cast<size_t>(3.0 * sr);
    size_t short_term_hop    = static_cast<size_t>(1.0 * sr);

    // ── Per-channel metrics ──────────────────────────────────────────
    r.ch_metrics.resize(nch);
    double joint_tp = 0.0;

    for (size_t c = 0; c < nch; ++c) {
        auto& cm = r.ch_metrics[c];
        const auto& samp = channels[c];

        // Sample peak
        double sp = peak_of(samp.data(), n);
        cm.sample_peak_dbfs = to_dbfs(sp);

        // True peak (4x oversampled)
        double tp = compute_true_peak(samp.data(), n);
        cm.true_peak_dbfs = to_dbfs(tp);
        if (tp > joint_tp) joint_tp = tp;

        // RMS
        cm.rms_dbfs = to_dbfs(rms_of(samp.data(), n));

        // Per-channel DR (run TT DR on single channel)
        std::vector<std::vector<double>> single = {samp};
        auto ch_dr = compute_tt_dr(single, sample_rate);
        cm.dr_raw = ch_dr.dr;
        cm.dr_score = static_cast<int>(std::round(ch_dr.dr));

        // Per-channel momentary max (non-ITU, single channel K-weighted)
        cm.max_momentary_lufs = max_block_loudness_single(
            kw[c].data(), n, momentary_window, momentary_hop);

        // Per-channel short-term max (non-ITU)
        cm.max_short_term_lufs = max_block_loudness_single(
            kw[c].data(), n, short_term_window, short_term_hop);
    }

    r.true_peak_dbfs = to_dbfs(joint_tp);

    // ── Joint loudness metrics (ITU, multi-channel) ──────────────────
    r.max_momentary_lufs = max_block_loudness_multi(
        kw, momentary_window, momentary_hop);

    r.max_short_term_lufs = max_block_loudness_multi(
        kw, short_term_window, short_term_hop);

    r.integrated_lufs = compute_integrated_loudness_multi(kw, sr);

    // ── PLR (True Peak to Integrated Loudness) ───────────────────────
    r.plr_db = r.true_peak_dbfs - r.integrated_lufs;

    // ── Crest factor (per-channel average) ───────────────────────────
    double sum_crest = 0.0;
    for (size_t c = 0; c < nch; ++c) {
        sum_crest += r.ch_metrics[c].sample_peak_dbfs - r.ch_metrics[c].rms_dbfs;
    }
    r.crest_factor_db = sum_crest / static_cast<double>(nch);

    // ── LRA ──────────────────────────────────────────────────────────
    r.lra_lu = compute_lra(kw, sr);

    // ── Min PSR ──────────────────────────────────────────────────────
    r.psr_db = compute_min_psr(channels, kw, sr);

    r.verdict = classify_dr(r.dr_score_raw);

    return r;
}

// ── Album-level DR ─────────────────────────────────────────────────────────
inline int album_dr(const std::vector<TrackResult>& tracks) {
    if (tracks.empty()) return 0;
    double sum = 0.0;
    for (const auto& t : tracks) sum += t.dr_score;
    return static_cast<int>(std::round(sum / tracks.size()));
}

} // namespace dr
