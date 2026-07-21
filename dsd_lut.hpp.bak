#pragma once
// ============================================================================
// dsd_lut.hpp — Byte-LUT DSD→PCM Decimation Engine
//
// Replaces per-bit FIR convolution with precomputed byte lookup tables.
// For each group of 8 FIR coefficients, a 256-entry table stores the
// dot product for all 256 possible byte values (all combinations of 8
// DSD ±1 samples). At runtime, each output sample requires only
// ceil(N/8) table lookups and additions instead of N multiply-accumulates.
//
// Threading model: each call to decimate_channel() processes one channel
// sequentially. Callers parallelize across channels (one thread per channel).
//
// ── Provenance ─────────────────────────────────────────────────────────────
// Coefficient tables and the decimation chain topology are transcribed from
// libdsdpcm in Maxim V. Anisiutkin's foobar2000 Super Audio CD Decoder plugin
// (foo_input_sacd v2.0.24), which is LGPL-2.1-or-later.  This file inherits
// that licence.
//
// ── The two decimation modes (verified against foo_input_sacd 2.0.24) ───────
//
//   DIRECT      fir1_64 (641 taps) with decimation = ratio, then 2:1 PCM
//               halving stages if ratio > 32.  Direct does NOT use fir1_8 or
//               fir1_16 — those belong exclusively to Multistage.
//
//   MULTISTAGE  fir1_8 (80 taps) @ /8   [fir1_16 (160 taps) @ /16 if ratio > 32]
//               then 2:1 PCM halving stages (fir2_2 / fir3_2).
//
//   ratio = dsd_rate / pcm_rate.  Must be a power of two and >= 8, so output
//   rates are always dsd_rate / 2^n.  384 kHz and 768 kHz are NOT reachable —
//   the old rational resampler was removed upstream in v2.0.12.
//
//   Chains to 352800 Hz:
//     DSD64  r=8    Direct: fir1_64/8               Multi: fir1_8/8
//     DSD128 r=16   Direct: fir1_64/16              Multi: fir1_8/8  -> fir3_2/2
//     DSD256 r=32   Direct: fir1_64/32              Multi: fir1_8/8  -> fir2_2/2 -> fir3_2/2
//     DSD512 r=64   Direct: fir1_64/32 -> fir3_2/2  Multi: fir1_16/16 -> fir2_2/2 -> fir3_2/2
//
// ── Parity-critical invariants (do not "fix") ──────────────────────────────
//
//   * FIR history is primed with DSD_SILENCE_BYTE (0x69), not zeros, and PCM
//     stage history with 0.0.  Out-of-range reads pad with those values, which
//     is exactly equivalent to upstream's primed ring buffers.
//   * Group delay is NOT compensated (upstream removed compensation in v1.2.5).
//     The leading output samples are the filter ramp-in; leave them.
//   * Coefficient gains are upstream's fixed 2^-28 / 2^-31, NOT a normalise-to-
//     unity-DC division.  DC gain is therefore 1.000002027 (fir1_8) /
//     1.000000001 (fir1_64), a deliberate ~1.8e-5 dB offset that must be kept
//     to match foobar exactly.
//   * Bit order: DSF is LSB-first, DFF is MSB-first (lsb_first flag).
// ============================================================================

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace dsd {

// DSD silence: alternating pattern (0b01101001) — zero-mean idle tone
static constexpr uint8_t DSD_SILENCE_BYTE = 0x69;

// ── Decimation mode ────────────────────────────────────────────────────────
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

// ── Lookup table type ──────────────────────────────────────────────────────
using ctable_t = std::array<double, 256>;

// ═══════════════════════════════════════════════════════════════════════════
// Coefficients — foo_input_sacd/libdsdpcm/dsdpcm_constants.h
// Copyright (c) 2011-2026 Maxim V. Anisiutkin.  LGPL-2.1-or-later.
// Integer-valued; GAIN converts to the upstream fixed-point normalisation.
// ═══════════════════════════════════════════════════════════════════════════

// fir1_8   — 80 taps.  Multistage stage-1, decimation 8.
static constexpr int FIR1_8_LENGTH = 80;
static constexpr double FIR1_8_GAIN = 1.0 / static_cast<double>(1u << 28);
static constexpr double FIR1_8_RAW[FIR1_8_LENGTH] = {
    -142, -651, -1997, -4882, -10198, -18819, -31226, -46942, -63892, -77830, -82099, -67999,
    -26010, 52003, 169742, 323000, 496497, 662008, 778827, 797438, 666789, 344848, -188729,
    -919845, -1789769, -2690283, -3466610, -3929490, -3876295, -3119266, -1517221, 994203,
    4379191, 8490255, 13072043, 17781609, 22223533, 25995570, 28738430, 30182209, 30182209,
    28738430, 25995570, 22223533, 17781609, 13072043, 8490255, 4379191, 994203, -1517221,
    -3119266, -3876295, -3929490, -3466610, -2690283, -1789769, -919845, -188729, 344848,
    666789, 797438, 778827, 662008, 496497, 323000, 169742, 52003, -26010, -67999, -82099,
    -77830, -63892, -46942, -31226, -18819, -10198, -4882, -1997, -651, -142
};

// fir1_16  — 160 taps. Multistage stage-1, decimation 16 (ratio > 32 only).
static constexpr int FIR1_16_LENGTH = 160;
static constexpr double FIR1_16_GAIN = 1.0 / static_cast<double>(1u << 28);
static constexpr double FIR1_16_RAW[FIR1_16_LENGTH] = {
    -42, -102, -220, -420, -739, -1220, -1914, -2878, -4171, -5851, -7967, -10555, -13625,
    -17154, -21075, -25266, -29539, -33636, -37219, -39874, -41114, -40390, -37108, -30659,
    -20450, -5948, 13272, 37474, 66704, 100733, 139006, 180597, 224174, 267987, 309866,
    347255, 377263, 396750, 402440, 391067, 359534, 305112, 225636, 119722, -13034, -171854,
    -354614, -557713, -775985, -1002675, -1229481, -1446662, -1643229, -1807208, -1925973,
    -1986643, -1976541, -1883674, -1697253, -1408195, -1009619, -497293, 129993, 870122,
    1717463, 2662800, 3693381, 4793111, 5942870, 7120962, 8303674, 9465936, 10582054,
    11626490, 12574667, 13403753, 14093414, 14626488, 14989568, 15173448, 15173448, 14989568,
    14626488, 14093414, 13403753, 12574667, 11626490, 10582054, 9465936, 8303674, 7120962,
    5942870, 4793111, 3693381, 2662800, 1717463, 870122, 129993, -497293, -1009619, -1408195,
    -1697253, -1883674, -1976541, -1986643, -1925973, -1807208, -1643229, -1446662, -1229481,
    -1002675, -775985, -557713, -354614, -171854, -13034, 119722, 225636, 305112, 359534,
    391067, 402440, 396750, 377263, 347255, 309866, 267987, 224174, 180597, 139006, 100733,
    66704, 37474, 13272, -5948, -20450, -30659, -37108, -40390, -41114, -39874, -37219,
    -33636, -29539, -25266, -21075, -17154, -13625, -10555, -7967, -5851, -4171, -2878, -1914,
    -1220, -739, -420, -220, -102, -42
};

// fir1_64  — 641 taps. Direct stage-1, ALL ratios. This is the Direct filter.
static constexpr int FIR1_64_LENGTH = 641;
static constexpr double FIR1_64_GAIN = 1.0 / static_cast<double>(1u << 31);
static constexpr double FIR1_64_RAW[FIR1_64_LENGTH] = {
    1652, 421, 509, 606, 714, 832, 960, 1098, 1245, 1402, 1567, 1739, 1917, 2101, 2287, 2475,
    2663, 2848, 3027, 3199, 3359, 3504, 3632, 3738, 3818, 3869, 3886, 3864, 3800, 3689, 3525,
    3305, 3024, 2677, 2260, 1769, 1201, 550, -184, -1006, -1918, -2921, -4016, -5203, -6483,
    -7854, -9314, -10859, -12487, -14190, -15965, -17802, -19693, -21630, -23600, -25591,
    -27591, -29584, -31555, -33486, -35359, -37155, -38854, -40434, -41872, -43146, -44233,
    -45108, -45747, -46124, -46216, -45997, -45443, -44530, -43236, -41537, -39414, -36847,
    -33818, -30310, -26311, -21809, -16796, -11265, -5214, 1355, 8439, 16029, 24112, 32673,
    41689, 51136, 60984, 71196, 81734, 92553, 103603, 114829, 126173, 137571, 148954, 160248,
    171377, 182260, 192811, 202943, 212564, 221581, 229897, 237417, 244040, 249669, 254204,
    257547, 259603, 260277, 259477, 257115, 253109, 247380, 239855, 230471, 219167, 205896,
    190617, 173299, 153922, 132479, 108972, 83419, 55848, 26302, -5160, -38467, -73531,
    -110251, -148507, -188168, -229083, -271089, -314005, -357638, -401777, -446201, -490671,
    -534940, -578744, -621813, -663865, -704607, -743744, -780970, -815977, -848454, -878088,
    -904566, -927580, -946823, -961997, -972811, -978983, -980246, -976346, -967047, -952131,
    -931400, -904681, -871825, -832710, -787243, -735363, -677041, -612281, -541124, -463648,
    -379970, -290244, -194666, -93474, 13055, 124601, 240802, 361256, 485518, 613104, 743490,
    876114, 1010376, 1145641, 1281243, 1416482, 1550632, 1682939, 1812627, 1938901, 2060949,
    2177947, 2289061, 2393456, 2490292, 2578736, 2657963, 2727161, 2785536, 2832319, 2866766,
    2888167, 2895851, 2889189, 2867598, 2830552, 2777578, 2708268, 2622279, 2519340, 2399255,
    2261904, 2107255, 1935358, 1746352, 1540469, 1318035, 1079470, 825295, 556126, 272680,
    -24227, -333679, -654661, -986063, -1326676, -1675201, -2030245, -2390331, -2753893,
    -3119291, -3484807, -3848654, -4208980, -4563877, -4911383, -5249495, -5576169, -5889335,
    -6186898, -6466754, -6726792, -6964906, -7179003, -7367014, -7526900, -7656665, -7754364,
    -7818111, -7846090, -7836566, -7787889, -7698508, -7566979, -7391970, -7172272, -6906808,
    -6594638, -6234967, -5827149, -5370698, -4865289, -4310766, -3707142, -3054607, -2353529,
    -1604454, -808113, 34585, 922545, 1854492, 2828973, 3844355, 4898832, 5990428, 7117000,
    8276247, 9465711, 10682788, 11924736, 13188679, 14471619, 15770447, 17081948, 18402817,
    19729666, 21059038, 22387418, 23711244, 25026925, 26330844, 27619383, 28888926, 30135877,
    31356675, 32547801, 33705799, 34827283, 35908952, 36947604, 37940146, 38883606, 39775147,
    40612074, 41391849, 42112096, 42770616, 43365391, 43894592, 44356589, 44749958, 45073480,
    45326155, 45507200, 45616052, 45652373, 45616052, 45507200, 45326155, 45073480, 44749958,
    44356589, 43894592, 43365391, 42770616, 42112096, 41391849, 40612074, 39775147, 38883606,
    37940146, 36947604, 35908952, 34827283, 33705799, 32547801, 31356675, 30135877, 28888926,
    27619383, 26330844, 25026925, 23711244, 22387418, 21059038, 19729666, 18402817, 17081948,
    15770447, 14471619, 13188679, 11924736, 10682788, 9465711, 8276247, 7117000, 5990428,
    4898832, 3844355, 2828973, 1854492, 922545, 34585, -808113, -1604454, -2353529, -3054607,
    -3707142, -4310766, -4865289, -5370698, -5827149, -6234967, -6594638, -6906808, -7172272,
    -7391970, -7566979, -7698508, -7787889, -7836566, -7846090, -7818111, -7754364, -7656665,
    -7526900, -7367014, -7179003, -6964906, -6726792, -6466754, -6186898, -5889335, -5576169,
    -5249495, -4911383, -4563877, -4208980, -3848654, -3484807, -3119291, -2753893, -2390331,
    -2030245, -1675201, -1326676, -986063, -654661, -333679, -24227, 272680, 556126, 825295,
    1079470, 1318035, 1540469, 1746352, 1935358, 2107255, 2261904, 2399255, 2519340, 2622279,
    2708268, 2777578, 2830552, 2867598, 2889189, 2895851, 2888167, 2866766, 2832319, 2785536,
    2727161, 2657963, 2578736, 2490292, 2393456, 2289061, 2177947, 2060949, 1938901, 1812627,
    1682939, 1550632, 1416482, 1281243, 1145641, 1010376, 876114, 743490, 613104, 485518,
    361256, 240802, 124601, 13055, -93474, -194666, -290244, -379970, -463648, -541124,
    -612281, -677041, -735363, -787243, -832710, -871825, -904681, -931400, -952131, -967047,
    -976346, -980246, -978983, -972811, -961997, -946823, -927580, -904566, -878088, -848454,
    -815977, -780970, -743744, -704607, -663865, -621813, -578744, -534940, -490671, -446201,
    -401777, -357638, -314005, -271089, -229083, -188168, -148507, -110251, -73531, -38467,
    -5160, 26302, 55848, 83419, 108972, 132479, 153922, 173299, 190617, 205896, 219167,
    230471, 239855, 247380, 253109, 257115, 259477, 260277, 259603, 257547, 254204, 249669,
    244040, 237417, 229897, 221581, 212564, 202943, 192811, 182260, 171377, 160248, 148954,
    137571, 126173, 114829, 103603, 92553, 81734, 71196, 60984, 51136, 41689, 32673, 24112,
    16029, 8439, 1355, -5214, -11265, -16796, -21809, -26311, -30310, -33818, -36847, -39414,
    -41537, -43236, -44530, -45443, -45997, -46216, -46124, -45747, -45108, -44233, -43146,
    -41872, -40434, -38854, -37155, -35359, -33486, -31555, -29584, -27591, -25591, -23600,
    -21630, -19693, -17802, -15965, -14190, -12487, -10859, -9314, -7854, -6483, -5203, -4016,
    -2921, -1918, -1006, -184, 550, 1201, 1769, 2260, 2677, 3024, 3305, 3525, 3689, 3800,
    3864, 3886, 3869, 3818, 3738, 3632, 3504, 3359, 3199, 3027, 2848, 2663, 2475, 2287, 2101,
    1917, 1739, 1567, 1402, 1245, 1098, 960, 832, 714, 606, 509, 421, 1652
};

// fir2_2   — 27 taps.  PCM 2:1 halving, intermediate stages.
static constexpr int FIR2_2_LENGTH = 27;
static constexpr double FIR2_2_GAIN = 1.0 / static_cast<double>(1u << 31);
static constexpr double FIR2_2_RAW[FIR2_2_LENGTH] = {
    349146, 0, -2503287, 0, 10155531, 0, -30459917, 0, 76750087, 0, -185782569, 0, 668365690,
    1073741824, 668365690, 0, -185782569, 0, 76750087, 0, -30459917, 0, 10155531, 0, -2503287,
    0, 349146
};

// fir3_2   — 151 taps. PCM 2:1 halving, final stage.
static constexpr int FIR3_2_LENGTH = 151;
static constexpr double FIR3_2_GAIN = 1.0 / static_cast<double>(1u << 31);
static constexpr double FIR3_2_RAW[FIR3_2_LENGTH] = {
    -5412, 0, 10344, 0, -19926, 0, 35056, 0, -57881, 0, 91092, 0, -138012, 0, 202658, 0,
    -289823, 0, 405153, 0, -555217, 0, 747573, 0, -990842, 0, 1294782, 0, -1670364, 0,
    2129866, 0, -2687005, 0, 3357104, 0, -4157326, 0, 5107022, 0, -6228238, 0, 7546440, 0,
    -9091589, 0, 10899739, 0, -13015406, 0, 15495180, 0, -18413298, 0, 21870494, 0, -26008543,
    0, 31035142, 0, -37268765, 0, 45224971, 0, -55796870, 0, 70676173, 0, -93495917, 0,
    133715464, 0, -226044891, 0, 682959923, 1073741824, 682959923, 0, -226044891, 0,
    133715464, 0, -93495917, 0, 70676173, 0, -55796870, 0, 45224971, 0, -37268765, 0,
    31035142, 0, -26008543, 0, 21870494, 0, -18413298, 0, 15495180, 0, -13015406, 0, 10899739,
    0, -9091589, 0, 7546440, 0, -6228238, 0, 5107022, 0, -4157326, 0, 3357104, 0, -2687005, 0,
    2129866, 0, -1670364, 0, 1294782, 0, -990842, 0, 747573, 0, -555217, 0, 405153, 0,
    -289823, 0, 202658, 0, -138012, 0, 91092, 0, -57881, 0, 35056, 0, -19926, 0, 10344, 0,
    -5412
};

// ═══════════════════════════════════════════════════════════════════════════
// Byte lookup tables for the DSD stage
//
// For N taps, produces ceil(N/8) tables. table[ct] covers h[N-1 - ct*8 - j]
// for j = 0..7, paired with the 8 DSD bits of one byte.
//
//   lsb_first = true   DSF  (bit 0 = earliest sample in time)
//   lsb_first = false  DFF  (bit 7 = earliest sample in time)
//
// gain is applied here (upstream's set_ctables multiplies by fir_gain).
// ═══════════════════════════════════════════════════════════════════════════

inline std::vector<ctable_t> build_ctables(
        const double* coeffs, int num_taps, double gain, bool lsb_first) {

    const int num_tables = (num_taps + 7) / 8;
    std::vector<ctable_t> tables(num_tables);

    for (int ct = 0; ct < num_tables; ++ct) {
        int k = num_taps - ct * 8;
        if (k > 8) k = 8;

        for (int byte_val = 0; byte_val < 256; ++byte_val) {
            double sum = 0.0;
            for (int j = 0; j < k; ++j) {
                const int bit_pos = lsb_first ? j : (7 - j);
                const double s = ((byte_val >> bit_pos) & 1) ? 1.0 : -1.0;
                sum += s * coeffs[num_taps - 1 - (ct * 8 + j)];
            }
            tables[ct][byte_val] = sum * gain;
        }
    }
    return tables;
}

// ═══════════════════════════════════════════════════════════════════════════
// DSD stage — bitstream to PCM via byte LUT.
//
// Output sample s consumes bytes [ (s+1)*byte_dec - num_tables,
//                                  (s+1)*byte_dec ).
// Indices outside the stream read as DSD_SILENCE_BYTE, which reproduces
// upstream's 0x69-primed ring buffer (and its uncompensated group delay).
// ═══════════════════════════════════════════════════════════════════════════

inline std::vector<double> decimate_with_lut(
        const uint8_t* dsd_bytes,
        size_t num_bytes,
        size_t total_dsd_bits,
        const std::vector<ctable_t>& tables,
        int decimation_bits) {

    if (decimation_bits % 8 != 0 || decimation_bits <= 0)
        throw std::runtime_error("dsd: DSD-stage decimation must be a positive multiple of 8");

    const int    byte_dec   = decimation_bits / 8;
    const int    num_tables = static_cast<int>(tables.size());
    const size_t num_out    = total_dsd_bits / static_cast<size_t>(decimation_bits);
    if (num_out == 0) return {};

    std::vector<double> pcm(num_out);
    const long nb = static_cast<long>(num_bytes);

    for (size_t s = 0; s < num_out; ++s) {
        const long base = static_cast<long>((s + 1) * static_cast<size_t>(byte_dec))
                        - num_tables;
        double acc = 0.0;
        for (int ct = 0; ct < num_tables; ++ct) {
            const long idx = base + ct;
            const uint8_t bv = (idx >= 0 && idx < nb) ? dsd_bytes[idx] : DSD_SILENCE_BYTE;
            acc += tables[ct][bv];
        }
        pcm[s] = acc;
    }
    return pcm;
}

// ═══════════════════════════════════════════════════════════════════════════
// PCM stage — 2:1 decimating FIR (fir2_2 / fir3_2).
//
// Coefficients are time-reversed and gain-applied, matching upstream set_coefs.
// Output sample s reads x[(s+1)*decim - L + i]; out-of-range reads as 0.0,
// reproducing upstream's zero-primed ring buffer.
// ═══════════════════════════════════════════════════════════════════════════

inline std::vector<double> pcm_decimate(
        const std::vector<double>& in,
        const double* raw, int len, double gain, int decim) {

    std::vector<double> c(len);
    for (int i = 0; i < len; ++i) c[i] = raw[len - 1 - i] * gain;

    const size_t num_out = in.size() / static_cast<size_t>(decim);
    std::vector<double> out(num_out);
    const long n = static_cast<long>(in.size());

    for (size_t s = 0; s < num_out; ++s) {
        const long base = static_cast<long>((s + 1) * static_cast<size_t>(decim)) - len;
        double acc = 0.0;
        for (int i = 0; i < len; ++i) {
            const long idx = base + i;
            if (idx >= 0 && idx < n) acc += c[i] * in[static_cast<size_t>(idx)];
        }
        out[s] = acc;
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// Chain planning
// ═══════════════════════════════════════════════════════════════════════════

// ratio must be a power of two and >= 8.
inline bool ratio_supported(int ratio) {
    return ratio >= 8 && (ratio & (ratio - 1)) == 0;
}

// Human-readable chain, e.g. "fir1_8/8 -> fir3_2/2". For logs and --version.
inline std::string describe_chain(Mode mode, int ratio) {
    if (!ratio_supported(ratio)) return "unsupported";
    std::string s;
    if (mode == Mode::Direct) {
        if (ratio > 64)       { s = "fir1_64/64"; ratio /= 64; }
        else if (ratio == 64) { s = "fir1_64/32"; ratio /= 32; }
        else                  { s = "fir1_64/" + std::to_string(ratio); ratio = 1; }
    } else {
        if (ratio > 32)       { s = "fir1_16/16"; ratio /= 16; }
        else                  { s = "fir1_8/8";   ratio /= 8;  }
    }
    while (ratio > 2) { s += " -> fir2_2/2"; ratio /= 2; }
    if (ratio > 1)    { s += " -> fir3_2/2"; ratio /= 2; }
    return s;
}

// ═══════════════════════════════════════════════════════════════════════════
// High-level: decimate one channel.  Called once per channel; the caller
// spawns one thread per channel.
//
//   ch_dsd      raw DSD bytes for this channel
//   total_bits  valid DSD sample bits (<= ch_dsd.size() * 8)
//   decimation  dsd_rate / pcm_rate; power of two, >= 8
//   lsb_first   true for DSF, false for DFF
//   mode        Direct (fir1_64) or Multistage (fir1_8 / fir1_16 + PCM stages)
// ═══════════════════════════════════════════════════════════════════════════

inline std::vector<double> decimate_channel(
        const std::vector<uint8_t>& ch_dsd,
        size_t total_bits,
        int decimation,
        bool lsb_first,
        Mode mode = Mode::Direct) {

    if (!ratio_supported(decimation))
        throw std::runtime_error(
            "dsd: decimation " + std::to_string(decimation) +
            " unsupported (must be a power of two >= 8; 384000/768000 output "
            "rates are not reachable)");

    int ratio = decimation;

    // ── Stage 1: DSD -> PCM ────────────────────────────────────────────────
    const double* raw  = nullptr;
    int           len  = 0;
    double        gain = 0.0;
    int           dec1 = 0;

    if (mode == Mode::Direct) {
        raw = FIR1_64_RAW; len = FIR1_64_LENGTH; gain = FIR1_64_GAIN;
        if (ratio > 64)       { dec1 = 64; ratio /= 64; }
        else if (ratio == 64) { dec1 = 32; ratio /= 32; }
        else                  { dec1 = ratio; ratio = 1; }
    } else {
        if (ratio > 32) {
            raw = FIR1_16_RAW; len = FIR1_16_LENGTH; gain = FIR1_16_GAIN;
            dec1 = 16; ratio /= 16;
        } else {
            raw = FIR1_8_RAW;  len = FIR1_8_LENGTH;  gain = FIR1_8_GAIN;
            dec1 = 8;  ratio /= 8;
        }
    }

    auto tables = build_ctables(raw, len, gain, lsb_first);
    auto pcm = decimate_with_lut(ch_dsd.data(), ch_dsd.size(), total_bits, tables, dec1);

    // ── Stages 2..N: PCM 2:1 halving ───────────────────────────────────────
    while (ratio > 2) {
        pcm = pcm_decimate(pcm, FIR2_2_RAW, FIR2_2_LENGTH, FIR2_2_GAIN, 2);
        ratio /= 2;
    }
    if (ratio > 1) {
        pcm = pcm_decimate(pcm, FIR3_2_RAW, FIR3_2_LENGTH, FIR3_2_GAIN, 2);
        ratio /= 2;
    }

    return pcm;
}

} // namespace dsd
