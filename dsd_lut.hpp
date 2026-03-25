#pragma once
// ============================================================================
// dsd_lut.hpp — Byte-LUT DSD→PCM Decimation Engine
//
// Replaces per-bit FIR convolution with precomputed byte lookup tables.
// For each group of 8 FIR coefficients, a 256-entry table stores the
// dot product for all 256 possible byte values (all combinations of 8
// DSD ±1 samples). At runtime, each output sample requires only
// ceil(N/8) table lookups and additions instead of N multiply-
// accumulates — roughly 8× fewer operations with better cache locality.
//
// Threading model: each call to decimate_channel() processes one channel
// sequentially. Callers parallelize across channels (one thread per
// channel), matching the foobar2000 SACD plugin architecture. This
// avoids cache-hostile overlapping reads within a single channel.
//
// DSD64 first stage uses coefficients from Maxim Anisiutkin's SACD
// Decoder plugin (LGPL 2.1), normalized to unity DC gain.
// Higher DSD rates use a runtime-designed Kaiser FIR.
// ============================================================================

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dsd {

// DSD silence: alternating 0/1 pattern (0b01101001) — zero-mean idle tone
static constexpr uint8_t DSD_SILENCE_BYTE = 0x69;

// ── Lookup table type ──────────────────────────────────────────────────────
using ctable_t = std::array<double, 256>;

// ── foobar2000 SACD plugin fir1_8 coefficients ────────────────────────────
// 80-tap symmetric FIR, originally integer-valued. Used in Multistage mode
// for ÷8 decimation (DSD64 → 352.8 kHz). Normalized to unity DC gain at
// runtime by dividing each coefficient by the sum.
//
// Source: dsdpcm_constants.h from foo_input_sacd (LGPL 2.1)
// Copyright (c) 2011-2023 Maxim V.Anisiutkin
static constexpr int FIR1_8_LENGTH = 80;
static constexpr double FIR1_8_RAW[FIR1_8_LENGTH] = {
    -142,     -651,     -1997,    -4882,
    -10198,   -18819,   -31226,   -46942,
    -63892,   -77830,   -82099,   -67999,
    -26010,    52003,   169742,   323000,
    496497,   662008,   778827,   797438,
    666789,   344848,  -188729,  -919845,
   -1789769, -2690283, -3466610, -3929490,
   -3876295, -3119266, -1517221,  994203,
    4379191,  8490255, 13072043, 17781609,
   22223533, 25995570, 28738430, 30182209,
   30182209, 28738430, 25995570, 22223533,
   17781609, 13072043,  8490255,  4379191,
     994203, -1517221, -3119266, -3876295,
   -3929490, -3466610, -2690283, -1789769,
    -919845,  -188729,  344848,   666789,
     797438,  778827,   662008,   496497,
     323000,  169742,    52003,   -26010,
     -67999,  -82099,   -77830,   -63892,
     -46942,  -31226,   -18819,   -10198,
      -4882,   -1997,     -651,     -142,
};

// ── foobar2000 SACD plugin fir1_16 coefficients ───────────────────────────
// 160-tap symmetric FIR for ÷16 decimation (DSD128 → 352.8 kHz or
// DSD64 → 176.4 kHz in multistage). Normalized to unity DC gain.
//
// Source: dsdpcm_constants.h from foo_input_sacd (LGPL 2.1)
// Copyright (c) 2011-2023 Maxim V.Anisiutkin
static constexpr int FIR1_16_LENGTH = 160;
static constexpr double FIR1_16_RAW[FIR1_16_LENGTH] = {
       -42,    -102,    -220,    -420,
      -739,   -1220,   -1914,   -2878,
     -4171,   -5851,   -7967,  -10555,
    -13625,  -17154,  -21075,  -25266,
    -29539,  -33636,  -37219,  -39874,
    -41114,  -40390,  -37108,  -30659,
    -20450,   -5948,   13272,   37474,
     66704,  100733,  139006,  180597,
    224174,  267987,  309866,  347255,
    377263,  396750,  402440,  391067,
    359534,  305112,  225636,  119722,
    -13034, -171854, -354614, -557713,
   -775985,-1002675,-1229481,-1446662,
  -1643229,-1807208,-1925973,-1986643,
  -1976541,-1883674,-1697253,-1408195,
  -1009619, -497293,  129993,  870122,
   1717463, 2662800, 3693381, 4793111,
   5942870, 7120962, 8303674, 9465936,
  10582054,11626490,12574667,13403753,
  14093414,14626488,14989568,15173448,
  15173448,14989568,14626488,14093414,
  13403753,12574667,11626490,10582054,
   9465936, 8303674, 7120962, 5942870,
   4793111, 3693381, 2662800, 1717463,
    870122,  129993, -497293,-1009619,
  -1408195,-1697253,-1883674,-1976541,
  -1986643,-1925973,-1807208,-1643229,
  -1446662,-1229481,-1002675, -775985,
   -557713, -354614, -171854,  -13034,
    119722,  225636,  305112,  359534,
    391067,  402440,  396750,  377263,
    347255,  309866,  267987,  224174,
    180597,  139006,  100733,   66704,
     37474,   13272,   -5948,  -20450,
    -30659,  -37108,  -40390,  -41114,
    -39874,  -37219,  -33636,  -29539,
    -25266,  -21075,  -17154,  -13625,
    -10555,   -7967,   -5851,   -4171,
     -2878,   -1914,   -1220,    -739,
      -420,    -220,    -102,     -42,
};

// ── Normalize raw integer coefficients to unity DC gain ────────────────────
inline std::vector<double> normalize_coeffs(const double* raw, int len) {
    double dc = 0.0;
    for (int i = 0; i < len; ++i) dc += raw[i];

    std::vector<double> h(len);
    if (std::abs(dc) > 1e-10) {
        for (int i = 0; i < len; ++i) h[i] = raw[i] / dc;
    } else {
        for (int i = 0; i < len; ++i) h[i] = raw[i];
    }
    return h;
}

// ── Build byte lookup tables from FIR coefficients ─────────────────────────
//
// For N FIR taps, produces ceil(N/8) tables.
//
// Table[0]                → oldest 8 samples in the convolution window
// Table[num_tables - 1]   → newest 8 samples
//
// Within each table, coefficient ordering matches the convolution direction:
// table[ct] covers h[N-1 - ct*8 - j] for j = 0..7, paired with 8 DSD bits
// from one byte, where bit extraction order depends on lsb_first.
//
// lsb_first:  true for DSF format (bit 0 = earliest sample in time)
//             false for DFF format (bit 7 = earliest sample in time)

inline std::vector<ctable_t> build_ctables(
        const double* coeffs, int num_taps, bool lsb_first) {

    int num_tables = (num_taps + 7) / 8;
    std::vector<ctable_t> tables(num_tables);

    for (int ct = 0; ct < num_tables; ++ct) {
        int k = num_taps - ct * 8;
        if (k > 8) k = 8;

        for (int byte_val = 0; byte_val < 256; ++byte_val) {
            double sum = 0.0;
            for (int j = 0; j < k; ++j) {
                int bit_pos = lsb_first ? j : (7 - j);
                double dsd_sample = ((byte_val >> bit_pos) & 1) ? 1.0 : -1.0;
                sum += dsd_sample * coeffs[num_taps - 1 - (ct * 8 + j)];
            }
            tables[ct][byte_val] = sum;
        }
    }
    return tables;
}

// ── Decimate a DSD byte stream using precomputed lookup tables ─────────────
//
// Processes one channel sequentially. Callers handle per-channel parallelism.
//
// dsd_bytes:       raw DSD byte stream for one channel
// num_bytes:       byte count of the DSD stream
// total_dsd_bits:  number of valid DSD sample bits (<= num_bytes * 8)
// tables:          precomputed ctables from build_ctables()
// decimation_bits: DSD bits consumed per output sample (e.g. 8 for DSD64→352.8k)

inline std::vector<double> decimate_with_lut(
        const uint8_t* dsd_bytes,
        size_t num_bytes,
        size_t total_dsd_bits,
        const std::vector<ctable_t>& tables,
        int decimation_bits) {

    int byte_dec = decimation_bits / 8;
    int num_tables = static_cast<int>(tables.size());
    size_t num_out = total_dsd_bits / static_cast<size_t>(decimation_bits);

    if (num_out == 0) return {};

    std::vector<double> pcm(num_out);
    int nb = static_cast<int>(num_bytes);

    for (size_t s = 0; s < num_out; ++s) {
        int base = static_cast<int>((s + 1) * byte_dec) - num_tables;
        double acc = 0.0;

        for (int ct = 0; ct < num_tables; ++ct) {
            int byte_idx = base + ct;
            uint8_t bv = (byte_idx >= 0 && byte_idx < nb)
                         ? dsd_bytes[byte_idx]
                         : DSD_SILENCE_BYTE;
            acc += tables[ct][bv];
        }
        pcm[s] = acc;
    }

    return pcm;
}

// ── Kaiser window I0 ──────────────────────────────────────────────────────
inline double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 25; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-15 * sum) break;
    }
    return sum;
}

// ── Design prototype lowpass FIR (fallback for non-standard DSD rates) ────
inline std::vector<double> design_prototype_fir(
        double cutoff_hz, double fs, int num_taps, double beta = 5.0) {

    constexpr double PI = 3.14159265358979323846;
    double f_cut = cutoff_hz / fs;
    int half = num_taps / 2;
    double i0_beta = bessel_i0(beta);

    std::vector<double> h(num_taps);
    for (int n = 0; n < num_taps; ++n) {
        double x = n - half;
        if (std::abs(x) < 1e-10)
            h[n] = 2.0 * f_cut;
        else
            h[n] = std::sin(2.0 * PI * f_cut * x) / (PI * x);

        double t = 2.0 * x / num_taps;
        double arg_sq = 1.0 - t * t;
        if (arg_sq > 0.0)
            h[n] *= bessel_i0(beta * std::sqrt(arg_sq)) / i0_beta;
        else
            h[n] = 0.0;
    }

    double dc = 0.0;
    for (double v : h) dc += v;
    if (std::abs(dc) > 1e-10)
        for (double& v : h) v /= dc;

    return h;
}

// ── High-level: select filter + build LUTs + decimate one channel ──────────
//
// Uses foobar's exact coefficients for standard DSD rates:
//   DSD64  (2.8224 MHz) → fir1_8  (80 taps, ÷8)  → 352.8 kHz
//   DSD128 (5.6448 MHz) → fir1_16 (160 taps, ÷16) → 352.8 kHz
//
// For non-standard rates, falls back to a runtime-designed Kaiser FIR.
//
// Called once per channel; callers spawn one thread per channel.

inline std::vector<double> decimate_channel(
        const std::vector<uint8_t>& ch_dsd,
        size_t total_bits,
        int decimation,
        bool lsb_first) {

    std::vector<double> fir;
    int fir_len = 0;

    if (decimation == 8) {
        // DSD64 → 352.8 kHz: use foobar's fir1_8 (80 taps, ÷8)
        fir = normalize_coeffs(FIR1_8_RAW, FIR1_8_LENGTH);
        fir_len = FIR1_8_LENGTH;
    } else if (decimation == 16) {
        // DSD128 → 352.8 kHz: use foobar's fir1_16 (160 taps, ÷16)
        fir = normalize_coeffs(FIR1_16_RAW, FIR1_16_LENGTH);
        fir_len = FIR1_16_LENGTH;
    } else {
        // DSD256/512 or non-standard: runtime-designed Kaiser FIR
        double fs_dsd = 352800.0 * decimation;
        fir_len = decimation * 10;              // 10 taps per phase
        fir_len = (fir_len + 7) & ~7;          // round up to multiple of 8
        fir = design_prototype_fir(130000.0, fs_dsd, fir_len, 5.0);
    }

    // Build byte LUTs
    auto tables = build_ctables(fir.data(), fir_len, lsb_first);

    // Decimate
    return decimate_with_lut(
        ch_dsd.data(), ch_dsd.size(), total_bits,
        tables, decimation);
}

} // namespace dsd
