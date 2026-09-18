#pragma once
// ============================================================================
// analysis.hpp — Dynamic Range Analysis Engine
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "channels.hpp"
#include "mqa_detect.hpp"

namespace dr {

// ── Constants ──────────────────────────────────────────────────────────────
static constexpr double PI = 3.14159265358979323846;
static constexpr double SILENCE_FLOOR_DB = -120.0;
static constexpr double LUFS_FLOOR = -70.0;

// FIX 11 (v0.10.x): one short-term grid for Max S / LRA / Min. PSR.
// EBU 3341 specifies the short-term meter at a >= 10 Hz update rate; Max S
// already ran at 0.1 s hop but LRA and Min. PSR sampled the same 3 s window
// on their own coarser 1.0 s grid. The coarse grid biased Min. PSR high
// (true minimum window falls between hops) and thinned the LRA percentile
// distribution on sparse classical material — both showed up as systematic
// flags against MAAT DROffline (Weekly #46/#47). All three metrics now share
// one block grid computed once (identical accumulation order, no extra cost).
// PSR_GATE_LUFS: windows quieter than this are excluded from Min. PSR
// (quiet low-crest passages otherwise produce spurious minima; MAAT gates
// these too — threshold tuned against MAAT references).
static constexpr double ST_WINDOW_SEC  = 3.0;
static constexpr double ST_HOP_SEC     = 0.1;
static constexpr double PSR_GATE_LUFS  = -50.0;

// ── Top-20% RMS averaging (TT DR / PMF) ────────────────────────────────────
// How compute_tt_dr() averages the loudest 20% of 3 s block RMS values.
//
// Quadratic (default): sqrt(mean(rms^2)) -- averages POWER. This is what the
//   PMF procedure specifies, and what MAAT DROffline and foobar2000 DR Meter
//   both do (2026-09-15 cross-check: the two agree with each other to a mean
//   0.006 dB over 264 per-channel values; see CLAUDE.md).
// Arithmetic: sum(rms)/n -- averages AMPLITUDE. What crete shipped up to and
//   including 0.12.1. By AM <= QM this is always biased low, so DR is always
//   biased high, by an amount that grows with block-to-block RMS variance:
//   ~0.03 dB on dense stereo L/R, up to 1.47 dB on sparse channels (LFE/C).
//   Retained so the corpus can be measured both ways, not for new work.
enum class RmsMean { Quadratic, Arithmetic };

inline const char* rms_mean_name(RmsMean m) {
    return m == RmsMean::Arithmetic ? "arithmetic" : "quadratic";
}

// ── 3 s block SET (TT DR / PMF) ────────────────────────────────────────────
// Which 3 s blocks compute_tt_dr() puts into the peak/RMS pools.  This is a
// separate axis from RmsMean above: that one picks how the top 20% are
// averaged, this one picks WHICH blocks exist to be ranked in the first place.
//
// Reference (default since 0.14.0): the trailing partial block is kept, with
//   its RMS normalised by its OWN length, and there is no gate.  This is the
//   PMF reference implementation (dr14_t.meter compute_dr14: seg_cnt =
//   floor(n/bs) + 1, the tail row filled by dr_rms over the remainder, and no
//   block gate at all).
// Legacy: the trailing partial block is discarded (n / block_size) and blocks
//   are gated on joint_rms > 1e-3.  What crete shipped up to and including
//   0.13.1.  Retained so every reference row recorded under it stays
//   reproducible, and so the two can be A/B'd -- not for new work.
//
// Why it matters, and in which direction.  n_top = floor(nb * 0.2), so both
// Legacy deviations shrink nb and therefore shrink n_top -- keeping FEWER and
// so LOUDER top blocks, which raises the top-20% RMS and LOWERS DR.  Legacy is
// structurally biased low, by an amount that grows as the discarded blocks
// become a larger fraction of the track: worst on short tracks and on sparse
// channels (LFE/C/surrounds), whose quiet blocks the gate removes.
//
// Measured 2026-09-16 on the 5.1 corpus (45 tracks x 6 channels, both oracles;
// Reference reproduces a literal transcription of dr14_t.meter to 3.9e-5, the
// residual being that implementation's own off-by-one on the final sample):
//
//                       vs foobar DR Meter      vs MAAT DROffline (stems)
//   Legacy              mean |d| 0.0598          0.0628   bias -0.011/-0.014
//   Reference           mean |d| 0.0355          0.0339   bias -0.004/-0.001
//
// It is not a surround-only effect: the Turtles stereo album improves 0.0472
// -> 0.0221 against MAAT.  And the "short-track effect" recorded in Weekly #55
// was this: corr(1/duration, |d|) falls from +0.465 to -0.044, and DSOTM
// "Speak to Me" (68 s) goes from up to 1.22 dB out to within 0.045 on all six
// channels.  See CLAUDE.md for the full write-up.
//
// Confirmed on the FULL corpus before it became the default: Weekly #68
// (legacy) vs #69 (reference), same binary, same corpus, run sequentially --
// weighted mean |d| over 2241 DR comparisons 0.0392 -> 0.0262 (-33%), DR flags
// 29 -> 19, and NOT ONE DR row regressed.  The crete-vs-crete control suites
// (cue, dsd_parity, dsd_roundtrip, ffmpeg_parity, format) stayed 0.000 and no
// non-DR value moved anywhere, which is what says the change is confined to
// the block accounting.
//
// NOTE for anyone transcribing more of dr14_t.meter: do NOT copy its
// `delta_fs = 60` fudge, which makes a 44.1 kHz block 132480 samples instead
// of 132300.  That is a bug in that implementation, not part of the procedure.
//
// One further reference quirk deliberately NOT replicated: it sizes its arrays
// at floor(n/bs) + 1 unconditionally, so when the track length is an exact
// multiple of the block size the extra row stays all-zero and still counts
// toward seg_cnt -- inflating n_top by one on those tracks alone.  The zero row
// never reaches the top-20% set or the 2nd-highest peak, so it is a pure
// counting artefact; Reference adds a trailing block only when one exists.
// No track in the 5.1 corpus is an exact multiple, which is why the two agree
// to 3.9e-5 there -- expect a divergence on a track that is.
enum class BlockSet { Legacy, Reference };

inline const char* block_set_name(BlockSet b) {
    return b == BlockSet::Reference ? "reference" : "legacy";
}

// Throws std::runtime_error on an unknown name (caller reports and exits).
inline BlockSet block_set_from_string(const std::string& s) {
    if (s == "legacy"    || s == "classic") return BlockSet::Legacy;
    if (s == "reference" || s == "pmf" || s == "dr14") return BlockSet::Reference;
    throw std::runtime_error("unknown --dr-blocks '" + s + "' (use legacy or reference)");
}

// Throws std::runtime_error on an unknown name (caller reports and exits).
inline RmsMean rms_mean_from_string(const std::string& s) {
    if (s == "quadratic"  || s == "qm" || s == "pmf") return RmsMean::Quadratic;
    if (s == "arithmetic" || s == "am" || s == "legacy") return RmsMean::Arithmetic;
    throw std::runtime_error("unknown --dr-mean '" + s +
                             "' (expected quadratic|arithmetic)");
}

// ── Does the LFE count toward the TT DR score? ─────────────────────────────
// crete's default is Exclude: a .1 track is a band-limited effects channel
// whose crest factor is nearly flat and whose level is mix-dependent, so
// folding it into the programme's dynamic range measures the mixer's bass
// management rather than the music. TT DR / dr.loudness-war.info defines
// nothing past stereo, so this is a CHOSEN rule, not a discovered one.
//
// foobar2000's DR Meter chooses the other way -- measured 6-0 on the
// discriminating tracks of the 5.1 corpus (2026-09-15) -- and that divergence
// is most of the remaining album-DR gap against it. Include exists so the
// comparison can be made directly instead of argued about.
//
// SCOPE, deliberately narrow. This moves TT DR only: the DR mean, the joint
// sample peak and overall RMS it is computed from, the 3 s block gate, and
// worst_channel. It does NOT touch the BS.1770 loudness family (integrated,
// Max M/S, LRA) -- LFE is weighted 0 there by ITU-R BS.1770-4, which is a
// standard and not ours to toggle -- nor Min. PSR, which is AES eBrief 373
// over the programme peak. Mono and stereo have no LFE, so every value is
// bit-identical under either setting.
enum class LfeScoring { Exclude, Include };

inline const char* lfe_scoring_name(LfeScoring s) {
    return s == LfeScoring::Include ? "include" : "exclude";
}

// Throws std::runtime_error on an unknown name (caller reports and exits).
inline LfeScoring lfe_scoring_from_string(const std::string& s) {
    if (s == "exclude" || s == "off" || s == "no")  return LfeScoring::Exclude;
    if (s == "include" || s == "on"  || s == "yes") return LfeScoring::Include;
    throw std::runtime_error("unknown --dr-lfe '" + s +
                             "' (expected exclude|include)");
}

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
    // Layout role. `scored` is false only for the LFE of a 5.1 / 7.1 layout:
    // a band-limited .1 track has an almost flat crest factor and would drag a
    // track's DR toward its own value for no musical reason. It is still
    // measured and reported in full -- excluded from aggregates, not hidden.
    bool   is_lfe              = false;
    bool   scored              = true;
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

    // Multichannel extras. On mono/stereo dr_front_raw == dr_score_raw and
    // worst_channel is simply the lower-DR channel, so nothing new is implied
    // for the two layouts the PMF procedure actually defines.
    double      dr_front_raw   = 0.0;   // DR of the front L/R pair alone --
                                        // the figure comparable with the
                                        // stereo release of the same master
    int         worst_channel  = -1;    // index of the lowest-DR scored channel

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

    // Forensic: MQA lossy-encoding identification. Populated by the caller
    // (not analyze_track) since it is a file-level, not per-slice, property.
    // Reported unconditionally — a listener measuring DR is entitled to know
    // the source is MQA and not lossless. See mqa_detect.hpp.
    mqa::Info   mqa;

    // Which container audio stream this track was decoded from. Populated by
    // the caller, like mqa above, because it is a file-level property of the
    // decode rather than anything analyze_track computes. -1 means the file
    // held exactly one stream (every native format), so there was no choice
    // to report. See audio::AudioData.
    int         stream_index       = -1;
    int         audio_stream_count = 1;
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

// ── BS.1770 channel weighting ──────────────────────────────────────────────
// The summed-program loudness measurements (Max M, Max S, Integrated, and the
// LRA / Min. PSR grids derived from them) weight each channel per ITU-R
// BS.1770-4: front channels unity, surrounds +1.5 dB, LFE excluded.  See
// channels.hpp for the table and the layout assumptions behind it.
//
// An EMPTY weights vector means all-unity, and every weighted loop below reads
// `weights.empty() ? 1.0 : weights[c]` and multiplies.  Multiplication by
// exactly 1.0 is exact in IEEE 754, so mono and stereo -- whose weights are all
// unity -- produce bit-identical results to the unweighted code this replaced.
// The entire MAAT-validated stereo corpus is therefore untouched by definition,
// not by measurement.
inline std::vector<double> bs1770_weights(size_t nch) {
    std::vector<double> w(nch);
    for (size_t c = 0; c < nch; ++c)
        w[c] = chan::bs1770_weight(static_cast<unsigned>(nch), c);
    return w;
}

inline double max_block_loudness_multi(
        const std::vector<std::vector<double>>& kw_channels,
        size_t window, size_t hop,
        const std::vector<double>& weights = std::vector<double>()) {
    if (kw_channels.empty()) return LUFS_FLOOR;
    size_t n = kw_channels[0].size();
    size_t nch = kw_channels.size();
    if (n < window) {
        double total_power = 0.0;
        for (size_t c = 0; c < nch; ++c)
            total_power += (weights.empty() ? 1.0 : weights[c])
                         * mean_square(kw_channels[c].data(), n);
        return to_lufs_from_power(total_power);
    }
    double max_lufs = LUFS_FLOOR;
    for (size_t start = 0; start + window <= n; start += hop) {
        double total_power = 0.0;
        for (size_t c = 0; c < nch; ++c)
            total_power += (weights.empty() ? 1.0 : weights[c])
                         * mean_square(&kw_channels[c][start], window);
        double lufs = to_lufs_from_power(total_power);
        if (lufs > max_lufs) max_lufs = lufs;
    }
    return max_lufs;
}

// FIX 11: shared short-term block powers (summed K-weighted mean-square per
// 3 s window, one entry per hop). Same loop structure and accumulation order
// as max_block_loudness_multi so Max S computed from this grid is bit-identical
// to the previous direct computation. Empty result <=> track shorter than one
// window (callers fall back / return their n<window value).
inline std::vector<double> short_term_powers_multi(
        const std::vector<std::vector<double>>& kw_channels,
        size_t window, size_t hop,
        const std::vector<double>& weights = std::vector<double>()) {
    std::vector<double> powers;
    if (kw_channels.empty()) return powers;
    size_t n = kw_channels[0].size();
    size_t nch = kw_channels.size();
    if (n < window || window == 0 || hop == 0) return powers;
    powers.reserve((n - window) / hop + 1);
    for (size_t start = 0; start + window <= n; start += hop) {
        double total = 0.0;
        for (size_t c = 0; c < nch; ++c)
            total += (weights.empty() ? 1.0 : weights[c])
                   * mean_square(&kw_channels[c][start], window);
        powers.push_back(total);
    }
    return powers;
}

// FIX 11: sliding max of |x| over `window` at `hop` steps — one value per
// block, aligned with short_term_powers_multi's grid. Monotonic deque, O(n),
// exact (no accumulation): equals peak_of() over each window.
inline std::vector<double> sliding_window_peaks(const double* x, size_t n,
                                                size_t window, size_t hop) {
    std::vector<double> out;
    if (n < window || window == 0 || hop == 0) return out;
    out.reserve((n - window) / hop + 1);
    std::deque<size_t> dq;  // indices into x, |x| non-increasing front→back
    size_t next_end = window;  // exclusive end of the next block's window
    for (size_t i = 0; i < n; ++i) {
        double v = std::abs(x[i]);
        while (!dq.empty() && std::abs(x[dq.back()]) <= v) dq.pop_back();
        dq.push_back(i);
        if (i + 1 == next_end) {
            while (dq.front() + window < next_end) dq.pop_front();
            out.push_back(std::abs(x[dq.front()]));
            next_end += hop;
            if (next_end > n) break;
        }
    }
    return out;
}

inline double compute_integrated_loudness_multi(
        const std::vector<std::vector<double>>& kw_channels,
        double sample_rate,
        const std::vector<double>& weights = std::vector<double>()) {
    if (kw_channels.empty()) return LUFS_FLOOR;
    size_t n = kw_channels[0].size();
    size_t nch = kw_channels.size();
    size_t block_samples = static_cast<size_t>(0.4 * sample_rate);
    size_t hop = block_samples / 4;
    if (n < block_samples) {
        double total_power = 0.0;
        for (size_t c = 0; c < nch; ++c)
            total_power += (weights.empty() ? 1.0 : weights[c])
                         * mean_square(kw_channels[c].data(), n);
        return to_lufs_from_power(total_power);
    }
    std::vector<double> block_powers;
    for (size_t start = 0; start + block_samples <= n; start += hop) {
        double total = 0.0;
        for (size_t c = 0; c < nch; ++c)
            total += (weights.empty() ? 1.0 : weights[c])
                   * mean_square(&kw_channels[c][start], block_samples);
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

// FIX 11: LRA now consumes the shared 0.1 s-hop short-term grid (was a
// private 1.0 s-hop grid). EBU 3342 permits any overlap >= 2/3; the denser
// grid matches the meter-rate distribution MAAT gates its percentiles on.
inline double compute_lra(const std::vector<double>& st_powers) {
    if (st_powers.empty()) return 0.0;
    struct Block { double power; double lufs; };
    std::vector<Block> blocks;
    blocks.reserve(st_powers.size());
    for (double p : st_powers)
        blocks.push_back({p, to_lufs_from_power(p)});
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

// FIX 11: Min. PSR (AES: max sample peak in window minus short-term loudness,
// minimum over the track) now consumes the shared 0.1 s-hop grid — the 1.0 s
// hop systematically missed the true minimum window (biased high vs MAAT).
// Windows below PSR_GATE_LUFS are excluded (quiet low-crest passages produce
// spurious minima). st_powers must come from short_term_powers_multi at the
// same window/hop so the peak and loudness grids align 1:1.
inline double compute_min_psr(
        const std::vector<std::vector<double>>& channels,
        const std::vector<double>& st_powers,
        size_t window, size_t hop) {
    if (channels.empty() || st_powers.empty()) return 0.0;
    size_t n = channels[0].size();
    size_t nch = channels.size();
    // Joint per-window sample peak = elementwise max of per-channel sliding
    // maxima, over the SCORED channels only: an LFE thump is not a programme
    // peak, and letting one set Min. PSR would read as crest the listener
    // never hears. No-op for mono/stereo, which have no LFE.
    std::vector<double> peaks = sliding_window_peaks(channels[0].data(), n, window, hop);
    for (size_t c = 1; c < nch; ++c) {
        if (chan::is_lfe(static_cast<unsigned>(nch), c)) continue;
        auto p = sliding_window_peaks(channels[c].data(), n, window, hop);
        for (size_t i = 0; i < peaks.size() && i < p.size(); ++i)
            peaks[i] = std::max(peaks[i], p[i]);
    }
    size_t nblocks = std::min(peaks.size(), st_powers.size());
    double min_psr = 1e9;
    bool any_valid = false;
    for (size_t i = 0; i < nblocks; ++i) {
        double st_lufs = to_lufs_from_power(st_powers[i]);
        if (st_lufs > PSR_GATE_LUFS && peaks[i] > 0.0) {
            double psr = to_dbfs(peaks[i]) - st_lufs;
            if (psr < min_psr) min_psr = psr;
            any_valid = true;
        }
    }
    return any_valid ? min_psr : 0.0;
}

struct TTDRResult {
    double dr;
    double dr_front = 0.0;      // mean DR of the front L/R pair alone
    int    worst_ch = -1;       // index of the lowest-DR scored channel
    std::vector<double> dr_per_ch;
    size_t blocks;
    double overall_rms_dbfs;
    double overall_peak_dbfs;
    bool   is_clipping;
};

inline TTDRResult compute_tt_dr(
        const std::vector<std::vector<double>>& channels,
        double sample_rate, double block_seconds = 3.0,
        RmsMean rms_mean = RmsMean::Quadratic,
        LfeScoring lfe = LfeScoring::Exclude,
        BlockSet block_set = BlockSet::Reference) {
    size_t nch = channels.size();
    const unsigned unch = static_cast<unsigned>(nch);
    // The single place the LFE policy is read. Under Exclude this is exactly
    // chan::is_lfe, so every loop below keeps its original form and its
    // original accumulation order -- mono and stereo never have an LFE, so
    // they are bit-identical under either setting by construction.
    const bool lfe_scored = (lfe == LfeScoring::Include);
    auto unscored = [unch, lfe_scored](size_t ch) {
        return !lfe_scored && chan::is_lfe(unch, ch);
    };
    size_t n = channels[0].size();
    size_t block_size = static_cast<size_t>(block_seconds * sample_rate);
    TTDRResult result;
    result.blocks = (n >= block_size) ? (n / block_size) : 1;
    result.dr_per_ch.resize(nch, 0.0);
    double track_peak = 0.0;
    double total_sum_sq = 0.0;
    // Every JOINT figure below -- track peak, overall RMS, the 3 s block gate
    // and the final DR -- is taken over the SCORED channels, i.e. everything
    // but the LFE (chan::is_lfe; mono and stereo have none, so their
    // accumulation order and results are untouched). The LFE still gets its
    // own peak, RMS and DR computed in full for display; it simply never
    // enters an aggregate. A .1 track is a band-limited effects channel: its
    // crest factor is nearly flat and its level is mix-dependent, so folding
    // it into the programme's dynamic range measures the mixer's bass
    // management, not the music.
    for (size_t ch = 0; ch < nch; ++ch) {
        if (unscored(ch)) continue;
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
        result.dr_front = result.dr;
        for (size_t ch = 0; ch < nch; ++ch)
            if (!unscored(ch)) { result.worst_ch = static_cast<int>(ch); break; }
        return result;
    }
    size_t n_blocks = n / block_size;
    struct BlockData { double joint_rms; std::vector<double> ch_peaks, ch_rms; };
    std::vector<BlockData> block_data;
    block_data.reserve(n_blocks);
    for (size_t b = 0; b < n_blocks; ++b) {
        size_t start = b * block_size;
        BlockData bd;
        bd.ch_peaks.resize(nch, 0.0);
        bd.ch_rms.resize(nch, 0.0);
        double sum_sq = 0.0;
        for (size_t ch = 0; ch < nch; ++ch) {
            const bool lfe = unscored(ch);
            double ch_pk = 0.0, ch_sum_sq = 0.0;
            for (size_t i = start; i < start + block_size; ++i) {
                double s = channels[ch][i];
                double a = std::abs(s);
                if (a > ch_pk) ch_pk = a;
                ch_sum_sq += s * s;
                if (!lfe) sum_sq += s * s;   // guarded in place, not hoisted: the
            }                                // running order of joint_rms (and so
                                             // the block gate) stays bit-identical
            bd.ch_peaks[ch] = ch_pk;
            // PMF "x2" RMS convention: sqrt(2 * mean(x^2)).  Identical to joint_rms
            // when the channels carry equal energy, so absolute calibration is unchanged.
            bd.ch_rms[ch] = std::sqrt(2.0 * ch_sum_sq / static_cast<double>(block_size));
        }
        bd.joint_rms = std::sqrt(sum_sq / static_cast<double>(block_size));
        // Reference keeps every block; Legacy gates on joint_rms. Under Legacy
        // the condition is exactly as it always was, so the retained set, its
        // order and n_top are bit-identical.
        if (block_set == BlockSet::Reference || bd.joint_rms > 1e-3)
            block_data.push_back(std::move(bd));
    }
    // Reference only: the trailing partial block. Its RMS is normalised by its
    // OWN length (dr14_t.meter's dr_rms over the remainder), not zero-padded to
    // a full block -- zero-padding was measured and is worse against both
    // oracles (mean |d| 0.0513 vs 0.0415 on the 5.1 corpus).
    size_t tail_len = n - n_blocks * block_size;
    if (block_set == BlockSet::Reference && tail_len > 0) {
        size_t start = n_blocks * block_size;
        BlockData bd;
        bd.ch_peaks.resize(nch, 0.0);
        bd.ch_rms.resize(nch, 0.0);
        double sum_sq = 0.0;
        for (size_t ch = 0; ch < nch; ++ch) {
            const bool lfe = unscored(ch);
            double ch_pk = 0.0, ch_sum_sq = 0.0;
            for (size_t i = start; i < n; ++i) {
                double s = channels[ch][i];
                double a = std::abs(s);
                if (a > ch_pk) ch_pk = a;
                ch_sum_sq += s * s;
                if (!lfe) sum_sq += s * s;
            }
            bd.ch_peaks[ch] = ch_pk;
            bd.ch_rms[ch] = std::sqrt(2.0 * ch_sum_sq / static_cast<double>(tail_len));
        }
        bd.joint_rms = std::sqrt(sum_sq / static_cast<double>(tail_len));
        block_data.push_back(std::move(bd));
    }
    if (block_data.empty()) { result.dr = 0.0; return result; }
    // Legacy keeps reporting the ungated full-block count it always reported.
    if (block_set == BlockSet::Reference) result.blocks = block_data.size();
    size_t nb = block_data.size();
    size_t n_top = std::max<size_t>(1, std::min<size_t>(nb,
            static_cast<size_t>(std::floor(nb * 0.2))));

    // Per-channel DR: 2nd-highest block peak of THIS channel, minus THIS channel's
    // own top-20% RMS -- the TT DR / PMF definition.
    //
    // FIX 12 (v0.11.3): this used to subtract a single JOINT top-20% RMS shared by
    // every channel, so L and R could differ only through the peak term.  That
    // blended the channels together.  The error cancelled in the mean -- Weekly #53
    // showed mean-of-L/R matching MAAT to a 0.01 dB median, 0.39 dB max -- but split
    // wrongly per channel (up to 2.37 dB, with the L-R spread frequently opposite in
    // sign to MAAT's), and collapsed to L == R *exactly* whenever both channels
    // happened to share a 2nd-highest block peak: RAM_CD2 "18. GL" read 8.64/8.64
    // against MAAT's 6.94/9.91, LeeRitenour "Dreamcatcher" 6.35/6.35 against
    // 5.72/6.77.  A per-channel metric cannot return bit-identical values for two
    // different channels; a shared RMS term can, and did.
    //
    // The old form was validated against foobar2000 DR Meter v1.0.8, whose own
    // per-channel errors were likewise anti-correlated with mean ~0 -- so the
    // artefact was accepted as correct.  foobar is a retired oracle (MAAT only).
    //
    // Block SET, block gate and n_top are deliberately unchanged (still joint-RMS
    // gated); only the RMS term and its top-20% selection became per-channel.
    for (size_t ch = 0; ch < nch; ++ch) {
        std::vector<double> ch_peaks(nb), ch_rms(nb);
        for (size_t i = 0; i < nb; ++i) {
            ch_peaks[i] = block_data[i].ch_peaks[ch];
            ch_rms[i]   = block_data[i].ch_rms[ch];
        }
        std::sort(ch_peaks.begin(), ch_peaks.end(), std::greater<double>());
        std::sort(ch_rms.begin(),   ch_rms.end(),   std::greater<double>());
        double ch_peak2 = (nb >= 2) ? ch_peaks[1] : ch_peaks[0];
        // Quadratic mean (PMF): average the POWER of the top blocks.  The
        // arithmetic branch keeps its original accumulation order verbatim so
        // --dr-mean arithmetic reproduces <= 0.12.1 bit-for-bit.
        double ch_avg_rms;
        if (rms_mean == RmsMean::Arithmetic) {
            double sum_rms = 0.0;
            for (size_t i = 0; i < n_top; ++i) sum_rms += ch_rms[i];
            ch_avg_rms = sum_rms / static_cast<double>(n_top);
        } else {
            double sum_sq_rms = 0.0;
            for (size_t i = 0; i < n_top; ++i) sum_sq_rms += ch_rms[i] * ch_rms[i];
            ch_avg_rms = std::sqrt(sum_sq_rms / static_cast<double>(n_top));
        }
        result.dr_per_ch[ch] = (ch_avg_rms > 0.0)
            ? to_dbfs(ch_peak2) - to_dbfs(ch_avg_rms) : 0.0;
    }

    // FIX 9: DR (PMF) is the MEAN of the per-channel DR values.
    //
    // Previously this was a separate "joint" measurement: the 2nd-highest of
    // the per-block JOINT peaks, where joint_peak = max(ch_peak) over channels.
    // Because max() over channels is >= either channel's own peak, and both DR
    // paths subtract the SAME avg_rms, that construction guaranteed
    //
    //     DR (PMF) >= max(DR_LEFT, DR_RIGHT)
    //
    // structurally — it could never fall below either channel.  Hence the
    // long-standing "crete reads +1, never -1" signature.
    //
    // foobar2000's Track DR is exactly round(mean(DR_FL, DR_FR)).  On I Robot
    // the old joint form missed 9 of 20 track/mode pairs (all +1); the mean
    // matches 20/20 in both Direct and Multistage.
    //
    // Weekly #53 answered the "re-run to confirm" note that stood here: FIX 9
    // reduced the always-high signature but did not clear it -- 31 of 34 remaining
    // integer mismatches were still crête-high (29x +1, 2x +2, 3x -1).  The
    // rate-independent "Likoo" offset turned out NOT to be a joint-DR problem at
    // all: it is the per-channel RMS blend fixed above (DR LEFT +1.14, DR RIGHT
    // -1.05 at all six sample rates, mean 0.04).  Whether the residual joint bias
    // survives FIX 12 is what the next weekly measures.
    //
    // MULTICHANNEL (v0.12.0): the mean runs over the SCORED channels, so 5.1
    // reads mean(L,R,C,Ls,Rs) and the LFE is left out.  This is the direct
    // generalisation of the rule above -- stereo is its two-channel case and
    // is arithmetically unchanged -- chosen because TT DR / dr.loudness-war
    // defines no multichannel procedure at all.  Two companion figures are
    // computed rather than substituted for it:
    //
    //   dr_front  mean(L,R): the number directly comparable with the stereo
    //             release of the same master, since it is measured over the
    //             same two channels.
    //   worst_ch  the lowest-DR scored channel, which is what a mastering
    //             check actually wants to see flagged -- a mean cannot fall
    //             below its worst member, so a single crushed surround would
    //             otherwise be averaged away by four healthy channels.
    //
    // Reporting all three keeps the headline comparable, the verdict honest
    // and neither one invented: crete states which rule produced which number
    // instead of picking one and calling it "the" multichannel DR.
    double sum_dr = 0.0;
    size_t n_scored = 0;
    double worst = 0.0;
    for (size_t ch = 0; ch < nch; ++ch) {
        if (unscored(ch)) continue;
        sum_dr += result.dr_per_ch[ch];
        ++n_scored;
        if (result.worst_ch < 0 || result.dr_per_ch[ch] < worst) {
            worst = result.dr_per_ch[ch];
            result.worst_ch = static_cast<int>(ch);
        }
    }
    result.dr = (n_scored > 0) ? sum_dr / static_cast<double>(n_scored) : 0.0;
    result.dr_front = (nch >= 2)
        ? 0.5 * (result.dr_per_ch[0] + result.dr_per_ch[1])
        : result.dr_per_ch[0];
    return result;
}

inline TrackResult analyze_track(const std::vector<std::vector<double>>& channels,
                                  uint32_t sample_rate, uint32_t bit_depth,
                                  const std::string& codec, const std::string& filename,
                                  RmsMean rms_mean = RmsMean::Quadratic,
                                  LfeScoring lfe = LfeScoring::Exclude,
                                  BlockSet block_set = BlockSet::Reference) {
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
    auto tt = compute_tt_dr(channels, sample_rate, 3.0, rms_mean, lfe, block_set);
    r.dr_score_raw   = tt.dr;
    r.dr_score       = static_cast<int>(std::round(tt.dr));
    r.dr_per_channel = tt.dr_per_ch;
    r.dr_front_raw   = tt.dr_front;
    r.worst_channel  = tt.worst_ch;
    r.tt_block_count = tt.blocks;
    r.peak_dbfs      = tt.overall_peak_dbfs;
    r.rms_dbfs       = tt.overall_rms_dbfs;
    r.is_clipping    = tt.is_clipping;
    if (codec == "DSD") r.is_clipping = false;
    auto kw = k_weight_channels(channels, sr);
    // FIX 8 (v0.6.x): hops tightened for EBU 3341 file-based Max M / Max S.
    size_t momentary_window  = static_cast<size_t>(0.4 * sr);
    size_t momentary_hop     = static_cast<size_t>(0.02 * sr);
    size_t short_term_window = static_cast<size_t>(ST_WINDOW_SEC * sr);
    size_t short_term_hop    = static_cast<size_t>(ST_HOP_SEC * sr);
    double ch_power_scale = static_cast<double>(nch);
    // BS.1770 summing weights for this layout; all-unity (and therefore a
    // no-op) for mono and stereo. See bs1770_weights() above.
    const std::vector<double> weights = bs1770_weights(nch);
    r.ch_metrics.resize(nch);
    double joint_tp = 0.0;
    for (size_t c = 0; c < nch; ++c) {
        auto& cm = r.ch_metrics[c];
        cm.is_lfe = chan::is_lfe(static_cast<unsigned>(nch), c);
        cm.scored = !cm.is_lfe || (lfe == LfeScoring::Include);
        const auto& samp = channels[c];
        double sp = peak_of(samp.data(), n);
        cm.sample_peak_dbfs = to_dbfs(sp);
        // FIX 10: true peak is the max of the RECONSTRUCTED continuous waveform,
        // which passes through every sample — so it is >= sample peak by
        // definition (BS.1770).  compute_true_peak() 4x-oversamples with an
        // audio-band interpolation FIR; content near Nyquist is attenuated by
        // that filter, so the oversampled peak can land BELOW the sample peak.
        // Harmless for PCM and for DSD Direct (the 641-tap brickwall removes
        // the offending content), but DSD Multistage's fir1_8 passband leaves
        // enough ultrasonic energy to trigger it: 5 violations on I Robot, worst
        // -0.143 dB.  Clamping restores the invariant and is a no-op wherever
        // TP >= SP already, so the EBU 3341 true-peak battery is unaffected.
        double tp = std::max(compute_true_peak(samp.data(), n), sp);
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
    r.max_momentary_lufs = max_block_loudness_multi(kw, momentary_window,
                                                    momentary_hop, weights);
    // FIX 11: one short-term grid feeds Max S, LRA and Min. PSR. Empty grid
    // (track < 3 s) falls back to the old whole-file Max S path; LRA/PSR
    // return 0 there, as before.
    auto st_powers = short_term_powers_multi(kw, short_term_window,
                                             short_term_hop, weights);
    if (st_powers.empty()) {
        r.max_short_term_lufs = max_block_loudness_multi(kw, short_term_window,
                                                         short_term_hop, weights);
    } else {
        r.max_short_term_lufs = to_lufs_from_power(
            *std::max_element(st_powers.begin(), st_powers.end()));
    }
    r.integrated_lufs = compute_integrated_loudness_multi(kw, sr, weights);
    r.plr_db = std::min(r.max_true_peak_dbtp, 0.0) - r.integrated_lufs;
    double sum_crest = 0.0;
    for (size_t c = 0; c < nch; ++c)
        sum_crest += r.ch_metrics[c].sample_peak_dbfs - r.ch_metrics[c].rms_dbfs;
    r.crest_factor_db = sum_crest / static_cast<double>(nch);
    r.lra_lu = compute_lra(st_powers);
    r.psr_db = compute_min_psr(channels, st_powers, short_term_window, short_term_hop);
    r.verdict = classify_dr(r.dr_score_raw);
    return r;
}

inline int album_dr(const std::vector<TrackResult>& tracks) {
    if (tracks.empty()) return 0;
    // FIX 12 (v0.11.3): average the RAW per-track DR and round ONCE.  This was
    // round(mean(round(dr))) -- double rounding, which desynchronised the album
    // score from MAAT even when every individual track's integer DR matched:
    // Weekly #53 had GramParsons_MQA16 at 10 v 9 (0/9 track mismatches) and
    // BoneyM_Venus_DSD128@352800 at 14 v 13 (0/10 track mismatches).
    double sum = 0.0;
    for (const auto& t : tracks) sum += t.dr_score_raw;
    return static_cast<int>(std::round(sum / tracks.size()));
}

} // namespace dr
