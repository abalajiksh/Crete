#pragma once
// ============================================================================
// analysis.hpp — Dynamic Range Analysis Engine
//
// Implements three complementary DR measurements:
//   1. Crest Factor          — peak-to-RMS ratio (naive but fast)
//   2. EBU R128 / PLR        — ITU-R BS.1770 K-weighted, gated loudness
//   3. TT Dynamic Range Meter — Pleasurize Music Foundation algorithm
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

// ── Per-track analysis result ──────────────────────────────────────────────
struct TrackResult {
    std::string filename;
    uint32_t    sample_rate    = 0;
    uint32_t    channels       = 0;
    uint32_t    bit_depth      = 0;
    uint64_t    total_samples  = 0;   // per channel
    std::string codec;
    double      duration_secs  = 0.0;

    // TT DR
    int         dr_score       = 0;     // rounded integer
    double      dr_score_raw   = 0.0;   // unrounded
    std::vector<double> dr_per_channel;
    size_t      tt_block_count = 0;

    // Track-level peak and RMS (displayed in output)
    double      peak_dbfs      = SILENCE_FLOOR_DB;  // max across channels
    double      rms_dbfs       = SILENCE_FLOOR_DB;   // overall RMS across channels
    bool        is_clipping    = false;               // peak >= 0.0 dBFS

    // EBU R128
    double      integrated_lufs = LUFS_FLOOR;
    double      plr_db          = 0.0;  // peak-to-loudness ratio

    // Crest factor
    double      crest_factor_db = 0.0;

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

// ── Second-highest absolute sample value ───────────────────────────────────
// The TT Dynamic Range spec uses the second-highest |sample| per block,
// not the absolute max. This prevents single rogue samples (clicks,
// digital errors) from inflating the block peak.
// If the block has fewer than 2 samples, returns the max.
inline double second_peak_of(const double* data, size_t n) {
    if (n == 0) return 0.0;
    if (n == 1) return std::abs(data[0]);

    double first = 0.0, second = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double v = std::abs(data[i]);
        if (v >= first) {
            second = first;
            first = v;
        } else if (v > second) {
            second = v;
        }
    }
    return second;
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

// ── EBU R128 integrated loudness (single channel) ──────────────────────────
inline double compute_integrated_loudness(const double* samples, size_t n,
                                           double sample_rate) {
    auto kw = apply_k_weighting(samples, n, sample_rate);

    size_t block_samples = static_cast<size_t>(0.4 * sample_rate);  // 400ms
    size_t hop = block_samples / 4;  // 75% overlap

    if (kw.size() < block_samples) {
        double rms = rms_of(kw.data(), kw.size());
        return to_lufs(rms);
    }

    std::vector<double> block_powers;
    for (size_t start = 0; start + block_samples <= kw.size(); start += hop) {
        block_powers.push_back(mean_square(&kw[start], block_samples));
    }

    // Absolute gate at -70 LUFS
    double abs_gate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    std::vector<double> above_abs;
    for (double p : block_powers)
        if (p > abs_gate) above_abs.push_back(p);

    if (above_abs.empty()) return LUFS_FLOOR;

    // Relative gate at -10 LU below ungated mean
    double ungated_mean = vec_mean(above_abs);
    double rel_gate = ungated_mean * std::pow(10.0, -10.0 / 10.0);

    std::vector<double> above_rel;
    for (double p : above_abs)
        if (p > rel_gate) above_rel.push_back(p);

    if (above_rel.empty()) return LUFS_FLOOR;

    return to_lufs_from_power(vec_mean(above_rel));
}

// ── TT Dynamic Range Meter ──────────────────────────────────────────────────
// Pleasurize Music Foundation algorithm (matches foobar2000 DR Meter).
//
// Per-channel algorithm:
//   1. Split each channel into 3-second non-overlapping blocks
//   2. Per block: compute peak (second-highest |sample|), RMS
//   3. Discard near-silent blocks (RMS < 1e-3 linear ≈ -60 dBFS)
//   4. Sort remaining blocks by RMS descending, select top 20%
//   5. DR = 20·log10( mean(top_peaks) / mean(top_rms) )
//      using arithmetic means in the linear domain
//   6. Average DR across channels, round to nearest integer
//
// CRITICAL: block peak uses the SECOND-HIGHEST absolute sample value,
// not the absolute max. This prevents single rogue samples (clicks,
// digital errors, inter-sample spikes) from inflating the block peak.
// This is a known aspect of the TT DR spec, confirmed by foobar2000
// DR Meter changelog (v0.2, v1.0.4).
// ────────────────────────────────────────────────────────────────────────────

struct TTDRResult {
    double dr;                     // averaged across channels
    std::vector<double> dr_per_ch; // per-channel DR values
    size_t blocks;
    double overall_rms_dbfs;       // power-summed across channels (display)
    double overall_peak_dbfs;      // max across channels (display)
    bool   is_clipping;
};

// ── Compute DR for a single channel ────────────────────────────────────────
inline double compute_tt_dr_single_channel(
        const double* samples, size_t n,
        double sample_rate,
        double block_seconds = 3.0) {

    size_t block_size = static_cast<size_t>(block_seconds * sample_rate);
    if (n < block_size) {
        double pk = second_peak_of(samples, n);
        double rms = rms_of(samples, n);
        if (rms <= 0.0) return 0.0;
        return to_dbfs(pk) - to_dbfs(rms);
    }

    size_t n_blocks = n / block_size;
    std::vector<double> block_rms_vals, block_peak_vals;
    block_rms_vals.reserve(n_blocks);
    block_peak_vals.reserve(n_blocks);

    for (size_t b = 0; b < n_blocks; ++b) {
        size_t start = b * block_size;

        // Second-highest |sample| in the block
        double pk = second_peak_of(samples + start, block_size);

        // RMS of the block
        double rms = rms_of(samples + start, block_size);

        // Discard near-silent blocks
        if (rms > 1e-3) {
            block_rms_vals.push_back(rms);
            block_peak_vals.push_back(pk);
        }
    }

    if (block_rms_vals.empty()) return 0.0;

    // Sort by RMS descending, take top 20%
    std::vector<size_t> indices(block_rms_vals.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(indices.begin(), indices.end(),
              [&](size_t a, size_t b) {
                  return block_rms_vals[a] > block_rms_vals[b];
              });

    size_t n_top = std::max<size_t>(1,
        std::min<size_t>(block_rms_vals.size(),
            static_cast<size_t>(std::ceil(block_rms_vals.size() * 0.2))));

    double sum_peak = 0.0, sum_rms = 0.0;
    for (size_t i = 0; i < n_top; ++i) {
        sum_peak += block_peak_vals[indices[i]];
        sum_rms  += block_rms_vals[indices[i]];
    }

    double avg_peak = sum_peak / static_cast<double>(n_top);
    double avg_rms  = sum_rms / static_cast<double>(n_top);

    if (avg_rms <= 0.0) return 0.0;
    return to_dbfs(avg_peak) - to_dbfs(avg_rms);
}

// ── Multi-channel wrapper: per-channel DR then average ─────────────────────
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
    // Peak: max across all channels (absolute max, not second-highest)
    // RMS: power-summed across channels, divided by per-channel count
    double track_peak = 0.0;
    double total_sum_sq = 0.0;

    for (size_t ch = 0; ch < nch; ++ch) {
        for (size_t i = 0; i < n; ++i) {
            double s = channels[ch][i];
            track_peak = std::max(track_peak, std::abs(s));
            total_sum_sq += s * s;
        }
    }

    result.overall_peak_dbfs = to_dbfs(track_peak);
    result.overall_rms_dbfs  = to_dbfs(std::sqrt(total_sum_sq / static_cast<double>(n)));
    result.is_clipping       = (track_peak >= 0.9999);

    // ── Per-channel DR computation ───────────────────────────────────
    result.dr_per_ch.resize(nch);
    double sum_dr = 0.0;

    for (size_t ch = 0; ch < nch; ++ch) {
        result.dr_per_ch[ch] = compute_tt_dr_single_channel(
            channels[ch].data(), n, sample_rate, block_seconds);
        sum_dr += result.dr_per_ch[ch];
    }

    result.dr = sum_dr / static_cast<double>(nch);
    return result;
}

// ── True peak via 4x oversampling (sinc interpolation) ─────────────────────
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

    // ── TT DR (per-channel, then averaged) ───────────────────────────
    auto tt = compute_tt_dr(channels, sample_rate);
    r.dr_score_raw   = tt.dr;
    r.dr_score       = static_cast<int>(std::round(tt.dr));
    r.dr_per_channel = tt.dr_per_ch;
    r.tt_block_count = tt.blocks;
    r.peak_dbfs      = tt.overall_peak_dbfs;
    r.rms_dbfs       = tt.overall_rms_dbfs;
    r.is_clipping    = tt.is_clipping;

    // DSD peaks above 0 dBFS are normal (sigma-delta reconstruction),
    // not clipping.
    if (codec == "DSD") r.is_clipping = false;

    // ── EBU R128 and crest factor (per-channel, then average) ────────
    double sum_lufs = 0.0, sum_crest = 0.0;
    for (size_t ch = 0; ch < channels.size(); ++ch) {
        const auto& samp = channels[ch];
        size_t n = samp.size();

        double pk  = peak_of(samp.data(), n);
        double rms = rms_of(samp.data(), n);
        sum_crest += to_dbfs(pk) - to_dbfs(rms);

        double lufs = compute_integrated_loudness(samp.data(), n, sample_rate);
        sum_lufs += lufs;
    }

    size_t nch = channels.size();
    r.integrated_lufs = sum_lufs / nch;
    r.plr_db = r.peak_dbfs - r.integrated_lufs;
    r.crest_factor_db = sum_crest / nch;
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
