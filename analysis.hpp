#pragma once
// ============================================================================
// analysis.hpp — Dynamic Range Analysis Engine
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
    Exceptional, Excellent, Normal, Compressed, Brickwalled,
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
    double true_peak_dbtp      = SILENCE_FLOOR_DB;
    double rms_dbfs            = SILENCE_FLOOR_DB;
    double dr_raw              = 0.0;
    int    dr_score            = 0;
    double max_momentary_lufs  = LUFS_FLOOR;
    double max_short_term_lufs = LUFS_FLOOR;
};

// ── Per-track analysis result ──────────────────────────────────────────────
struct TrackResult {
    std::string filename;
    uint32_t    sample_rate    = 0;
    uint32_t    channels       = 0;
    uint32_t    bit_depth      = 0;
    uint64_t    total_samples  = 0;
    std::string codec;
    double      duration_secs  = 0.0;

    int         dr_score       = 0;
    double      dr_score_raw   = 0.0;
    std::vector<double> dr_per_channel;
    size_t      tt_block_count = 0;

    double      peak_dbfs      = SILENCE_FLOOR_DB;
    double      rms_dbfs       = SILENCE_FLOOR_DB;
    bool        is_clipping    = false;

    double      integrated_lufs = LUFS_FLOOR;
    double      plr_db          = 0.0;

    double      crest_factor_db = 0.0;

    std::vector<ChannelMetrics> ch_metrics;

    // Joint true peak (4x oversampled, max across channels) — dBTP scale
    double      max_true_peak_dbtp = SILENCE_FLOOR_DB;

    double      max_momentary_lufs  = LUFS_FLOOR;
    double      max_short_term_lufs = LUFS_FLOOR;

    double      lra_lu = 0.0;
    double      psr_db = 0.0;

    Verdict     verdict = Verdict::Normal;
};

// ── Helper functions ───────────────────────────────────────────────────────
inline double to_dbfs(double amplitude) {
    if (amplitude <= 0.0) return SILENCE_FLOOR_DB;
    return 20.0 * std::log10(amplitude);
}
inline double to_lufs_from_power(double power) {
    if (power <= 0.0) return LUFS_FLOOR;
    return -0.691 + 10.0 * std::log10(power);
}
inline double peak_of(const double* data, size_t n) {
    double pk = 0.0;
    for (size_t i = 0; i < n; ++i) pk = std::max(pk, std::abs(data[i]));
    return pk;
}
inline double rms_of(const double* data, size_t n) {
    if (n == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += data[i] * data[i];
    return std::sqrt(sum / static_cast<double>(n));
}
inline double mean_square(const double* data, size_t n) {
    if (n == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += data[i] * data[i];
    return sum / static_cast<double>(n);
}
inline double vec_mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

// ── True peak (4x polyphase FIR) ────────────────────────────────────────────
static constexpr int TP_PHASES = 4;
static constexpr int TP_TAPS   = 12;
static constexpr double TP_FIR[TP_PHASES][TP_TAPS] = {
    {  0.0017089843750,  0.0109863281250, -0.0196533203125,  0.0332031250000,
      -0.0594482421875,  0.1373291015625,  0.9721679687500, -0.1022949218750,
       0.0476074218750, -0.0266113281250,  0.0148925781250, -0.0083007812500 },
    { -0.0291748046875,  0.0292968750000, -0.0517578125000,  0.0891113281250,
      -0.1665039062500,  0.4650878906250,  0.7797851562500, -0.2003173828125,
       0.1015625000000, -0.0582275390625,  0.0330810546875, -0.0189208984375 },
    { -0.0189208984375,  0.0330810546875, -0.0582275390625,  0.1015625000000,
      -0.2003173828125,  0.7797851562500,  0.4650878906250, -0.1665039062500,
       0.0891113281250, -0.0517578125000,  0.0292968750000, -0.0291748046875 },
    { -0.0083007812500,  0.0148925781250, -0.0266113281250,  0.0476074218750,
      -0.1022949218750,  0.9721679687500,  0.1373291015625, -0.0594482421875,
       0.0332031250000, -0.0196533203125,  0.0109863281250,  0.0017089843750 },
};

inline double compute_true_peak(const double* samples, size_t n) {
    if (n == 0) return 0.0;
    double buf[TP_TAPS] = {};
    int buf_idx = 0;
    double peak = 0.0;
    for (size_t i = 0; i < n; ++i) {
        buf[buf_idx] = samples[i];
        buf_idx = (buf_idx + 1) % TP_TAPS;
        for (int p = 0; p < TP_PHASES; ++p) {
            double acc = 0.0;
            for (int k = 0; k < TP_TAPS; ++k) {
                int idx = (buf_idx - 1 - k + TP_TAPS) % TP_TAPS;
                acc += TP_FIR[p][k] * buf[idx];
            }
            double a = std::abs(acc);
            if (a > peak) peak = a;
        }
    }
    return peak;
}

// ── Biquad filter ───────────────────────────────────────────────────────────
struct BiquadCoeffs { double b[3]; double a[3]; };

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

// FIX 7 (v0.6.x): K-weight high shelf — libebur128 exact BS.1770-4 formula.
// High shelf gain split: Vh=10^(G/20), Vb=Vh^0.4996667741545416 (NOT sqrt).
// Pre-warp via K=tan(pi*f0/fs). Verified vs libebur128 to 17 digits at all
// standard rates; eliminates the ~0.24 dB LUFSi bias at non-48k rates.
// Ref: ebur128.c:251-273.
inline BiquadCoeffs k_weight_prefilter(double fs) {
    constexpr double F0 = 1681.974450955533;
    constexpr double G  = 3.999843853973347;
    constexpr double Q  = 0.7071752369554196;
    double K  = std::tan(PI * F0 / fs);
    double Vh = std::pow(10.0, G / 20.0);
    double Vb = std::pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;
    double b0 = (Vh + Vb * K / Q + K * K) / a0;
    double b1 = 2.0 * (K * K - Vh) / a0;
    double b2 = (Vh - Vb * K / Q + K * K) / a0;
    double a1 = 2.0 * (K * K - 1.0) / a0;
    double a2 = (1.0 - K / Q + K * K) / a0;
    return {{b0, b1, b2}, {1.0, a1, a2}};
}

// FIX 7 (v0.6.x): K-weight high pass — libebur128 exact BS.1770-4 formula.
// Q = 0.5003270373238773 (NOT 1/sqrt(2)); numerator fixed [1,-2,1].
// Ref: ebur128.c:278-283.
inline BiquadCoeffs k_weight_highpass(double fs) {
    constexpr double F0 = 38.13547087602444;
    constexpr double Q  = 0.5003270373238773;
    double K   = std::tan(PI * F0 / fs);
    double den = 1.0 + K / Q + K * K;
    double a1  = 2.0 * (K * K - 1.0) / den;
    double a2  = (1.0 - K / Q + K * K) / den;
    return {{1.0, -2.0, 1.0}, {1.0, a1, a2}};
}

inline std::vector<double> apply_k_weighting(const double* samples, size_t n,
                                              double sample_rate) {
    auto pre = k_weight_prefilter(sample_rate);
    auto hp  = k_weight_highpass(sample_rate);
    auto stage1 = biquad_filter(samples, n, pre);
    return biquad_filter(stage1.data(), n, hp);
}

inline std::vector<std::vector<double>> k_weight_channels(
        const std::vector<std::vector<double>>& channels, double sample_rate) {
    std::vector<std::vector<double>> kw(channels.size());
    for (size_t c = 0; c < channels.size(); ++c)
        kw[c] = apply_k_weighting(channels[c].data(), channels[c].size(), sample_rate);
    return kw;
}

inline double max_block_loudness_single(const double* kw, size_t n,
                                         size_t window, size_t hop,
                                         double power_scale = 1.0) {
    if (n < window) {
        double ms = mean_square(kw, n) * power_scale;
        return to_lufs_from_power(ms);
    }
    double max_lufs = LUFS_FLOOR;
    for (size_t start = 0; start + window <= n; start += hop) {
        double ms = mean_square(&kw[start], window) * power_scale;
        double lufs = to_lufs_from_power(ms);
        if (lufs > max_lufs) max_lufs = lufs;
    }
    return max_lufs;
}

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
    std::vector<double> block_powers;
    for (size_t start = 0; start + block_samples <= n; start += hop) {
        double total = 0.0;
        for (size_t c = 0; c < nch; ++c)
            total += mean_square(&kw_channels[c][start], block_samples);
        block_powers.push_back(total);
    }
    double abs_gate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    std::vector<double> above_abs;
    for (double p : block_powers) if (p > abs_gate) above_abs.push_back(p);
    if (above_abs.empty()) return LUFS_FLOOR;
    double ungated_mean = vec_mean(above_abs);
    double rel_gate = ungated_mean * std::pow(10.0, -10.0 / 10.0);
    std::vector<double> above_rel;
    for (double p : above_abs) if (p > rel_gate) above_rel.push_back(p);
    if (above_rel.empty()) return LUFS_FLOOR;
    return to_lufs_from_power(vec_mean(above_rel));
}

inline double compute_lra(const std::vector<std::vector<double>>& kw_channels,
                           double sample_rate) {
    if (kw_channels.empty()) return 0.0;
    size_t n = kw_channels[0].size();
    size_t nch = kw_channels.size();
    size_t window = static_cast<size_t>(3.0 * sample_rate);
    size_t hop = static_cast<size_t>(1.0 * sample_rate);
    if (n < window) return 0.0;
    struct Block { double power; double lufs; };
    std::vector<Block> blocks;
    for (size_t start = 0; start + window <= n; start += hop) {
        double total = 0.0;
        for (size_t c = 0; c < nch; ++c)
            total += mean_square(&kw_channels[c][start], window);
        double lufs = to_lufs_from_power(total);
        blocks.push_back({total, lufs});
    }
    if (blocks.empty()) return 0.0;
    double abs_gate_power = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    std::vector<Block> above_abs;
    for (const auto& b : blocks) if (b.power > abs_gate_power) above_abs.push_back(b);
    if (above_abs.size() < 6) return 0.0;
    double sum_power = 0.0;
    for (const auto& b : above_abs) sum_power += b.power;
    double mean_power = sum_power / static_cast<double>(above_abs.size());
    double rel_gate_power = mean_power * std::pow(10.0, -20.0 / 10.0);
    std::vector<double> above_rel_lufs;
    for (const auto& b : above_abs) if (b.power > rel_gate_power) above_rel_lufs.push_back(b.lufs);
    if (above_rel_lufs.size() < 6) return 0.0;
    std::sort(above_rel_lufs.begin(), above_rel_lufs.end());
    size_t sz = above_rel_lufs.size();
    auto lerp_percentile = [&](double p) -> double {
        double pos = p * (sz - 1);
        size_t lo = static_cast<size_t>(pos);
        if (lo >= sz - 1) return above_rel_lufs[sz - 1];
        double frac = pos - static_cast<double>(lo);
        return above_rel_lufs[lo] + frac * (above_rel_lufs[lo + 1] - above_rel_lufs[lo]);
    };
    return lerp_percentile(0.95) - lerp_percentile(0.10);
}

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
        double seg_peak = 0.0;
        for (size_t c = 0; c < nch; ++c)
            seg_peak = std::max(seg_peak, peak_of(&channels[c][start], window));
        double total_power = 0.0;
        for (size_t c = 0; c < nch; ++c)
            total_power += mean_square(&kw_channels[c][start], window);
        double st_lufs = to_lufs_from_power(total_power);
        if (st_lufs > -50.0 && seg_peak > 0.0) {
            double psr = to_dbfs(seg_peak) - st_lufs;
            if (psr < min_psr) min_psr = psr;
            any_valid = true;
        }
    }
    return any_valid ? min_psr : 0.0;
}

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
        double sample_rate, double block_seconds = 3.0) {
    size_t nch = channels.size();
    size_t n = channels[0].size();
    size_t block_size = static_cast<size_t>(block_seconds * sample_rate);
    TTDRResult result;
    result.blocks = (n >= block_size) ? (n / block_size) : 1;
    result.dr_per_ch.resize(nch, 0.0);
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
    if (n < block_size) {
        double rms = std::sqrt(total_sum_sq / static_cast<double>(n));
        if (rms <= 0.0) { result.dr = 0.0; return result; }
        result.dr = to_dbfs(track_peak) - to_dbfs(rms);
        for (size_t ch = 0; ch < nch; ++ch) result.dr_per_ch[ch] = result.dr;
        return result;
    }
    size_t n_blocks = n / block_size;
    struct BlockData { double joint_rms; double joint_peak; std::vector<double> ch_peaks; };
    std::vector<BlockData> block_data;
    block_data.reserve(n_blocks);
    for (size_t b = 0; b < n_blocks; ++b) {
        size_t start = b * block_size;
        BlockData bd;
        bd.ch_peaks.resize(nch, 0.0);
        double joint_pk = 0.0;
        double sum_sq = 0.0;
        for (size_t ch = 0; ch < nch; ++ch) {
            double ch_pk = 0.0;
            for (size_t i = start; i < start + block_size; ++i) {
                double s = channels[ch][i];
                double a = std::abs(s);
                if (a > ch_pk) ch_pk = a;
                sum_sq += s * s;
            }
            bd.ch_peaks[ch] = ch_pk;
            if (ch_pk > joint_pk) joint_pk = ch_pk;
        }
        bd.joint_peak = joint_pk;
        bd.joint_rms = std::sqrt(sum_sq / static_cast<double>(block_size));
        if (bd.joint_rms > 1e-3) block_data.push_back(std::move(bd));
    }
    if (block_data.empty()) { result.dr = 0.0; return result; }
    size_t nb = block_data.size();
    std::vector<double> sorted_peaks(nb);
    for (size_t i = 0; i < nb; ++i) sorted_peaks[i] = block_data[i].joint_peak;
    std::sort(sorted_peaks.begin(), sorted_peaks.end(), std::greater<double>());
    double dr_peak = (nb >= 2) ? sorted_peaks[1] : sorted_peaks[0];
    std::vector<size_t> indices(nb);
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(indices.begin(), indices.end(),
              [&](size_t a, size_t b) { return block_data[a].joint_rms > block_data[b].joint_rms; });
    size_t n_top = std::max<size_t>(1, std::min<size_t>(nb,
            static_cast<size_t>(std::floor(nb * 0.2))));
    double sum_rms = 0.0;
    for (size_t i = 0; i < n_top; ++i) sum_rms += block_data[indices[i]].joint_rms;
    double avg_rms = sum_rms / static_cast<double>(n_top);
    if (avg_rms <= 0.0) { result.dr = 0.0; return result; }
    result.dr = to_dbfs(dr_peak) - to_dbfs(avg_rms);
    for (size_t ch = 0; ch < nch; ++ch) {
        std::vector<double> ch_peaks(nb);
        for (size_t i = 0; i < nb; ++i) ch_peaks[i] = block_data[i].ch_peaks[ch];
        std::sort(ch_peaks.begin(), ch_peaks.end(), std::greater<double>());
        double ch_peak2 = (nb >= 2) ? ch_peaks[1] : ch_peaks[0];
        result.dr_per_ch[ch] = to_dbfs(ch_peak2) - to_dbfs(avg_rms);
    }
    return result;
}

inline TrackResult analyze_track(const std::vector<std::vector<double>>& channels,
                                  uint32_t sample_rate, uint32_t bit_depth,
                                  const std::string& codec, const std::string& filename) {
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
    auto tt = compute_tt_dr(channels, sample_rate);
    r.dr_score_raw   = tt.dr;
    r.dr_score       = static_cast<int>(std::round(tt.dr));
    r.dr_per_channel = tt.dr_per_ch;
    r.tt_block_count = tt.blocks;
    r.peak_dbfs      = tt.overall_peak_dbfs;
    r.rms_dbfs       = tt.overall_rms_dbfs;
    r.is_clipping    = tt.is_clipping;
    if (codec == "DSD") r.is_clipping = false;
    auto kw = k_weight_channels(channels, sr);
    // FIX 8 (v0.6.x): hops tightened for EBU 3341 file-based Max M / Max S.
    size_t momentary_window  = static_cast<size_t>(0.4 * sr);
    size_t momentary_hop     = static_cast<size_t>(0.02 * sr);
    size_t short_term_window = static_cast<size_t>(3.0 * sr);
    size_t short_term_hop    = static_cast<size_t>(0.1 * sr);
    double ch_power_scale = static_cast<double>(nch);
    r.ch_metrics.resize(nch);
    double joint_tp = 0.0;
    for (size_t c = 0; c < nch; ++c) {
        auto& cm = r.ch_metrics[c];
        const auto& samp = channels[c];
        double sp = peak_of(samp.data(), n);
        cm.sample_peak_dbfs = to_dbfs(sp);
        double tp = compute_true_peak(samp.data(), n);
        cm.true_peak_dbtp = to_dbfs(tp);
        if (tp > joint_tp) joint_tp = tp;
        double ch_rms = rms_of(samp.data(), n) * std::sqrt(ch_power_scale);
        cm.rms_dbfs = to_dbfs(ch_rms);
        cm.dr_raw   = tt.dr_per_ch[c];
        cm.dr_score = static_cast<int>(std::round(tt.dr_per_ch[c]));
        cm.max_momentary_lufs = max_block_loudness_single(
            kw[c].data(), n, momentary_window, momentary_hop, ch_power_scale);
        cm.max_short_term_lufs = max_block_loudness_single(
            kw[c].data(), n, short_term_window, short_term_hop, ch_power_scale);
    }
    r.max_true_peak_dbtp = to_dbfs(joint_tp);
    r.max_momentary_lufs = max_block_loudness_multi(kw, momentary_window, momentary_hop);
    r.max_short_term_lufs = max_block_loudness_multi(kw, short_term_window, short_term_hop);
    r.integrated_lufs = compute_integrated_loudness_multi(kw, sr);
    r.plr_db = std::min(r.max_true_peak_dbtp, 0.0) - r.integrated_lufs;
    double sum_crest = 0.0;
    for (size_t c = 0; c < nch; ++c)
        sum_crest += r.ch_metrics[c].sample_peak_dbfs - r.ch_metrics[c].rms_dbfs;
    r.crest_factor_db = sum_crest / static_cast<double>(nch);
    r.lra_lu = compute_lra(kw, sr);
    r.psr_db = compute_min_psr(channels, kw, sr);
    r.verdict = classify_dr(r.dr_score_raw);
    return r;
}

inline int album_dr(const std::vector<TrackResult>& tracks) {
    if (tracks.empty()) return 0;
    double sum = 0.0;
    for (const auto& t : tracks) sum += t.dr_score;
    return static_cast<int>(std::round(sum / tracks.size()));
}

} // namespace dr
