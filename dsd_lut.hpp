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
// The direct-indexing approach (no circular buffer) makes each output
// sample independent, enabling trivial multi-threaded parallelism.
//
// Technique based on the ctable method from Maxim Anisiutkin's SACD
// Decoder plugin (LGPL 2.1). This is a clean-room implementation.
// ============================================================================

#include <array>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace dsd {

// DSD silence: alternating 0/1 pattern (0b01101001) — zero-mean idle tone
static constexpr uint8_t DSD_SILENCE_BYTE = 0x69;

// ── Lookup table type ──────────────────────────────────────────────────────
// One table per group of 8 FIR coefficients.
// Entry[byte_val] = Σ (bit_j mapped to ±1) × coeff_j, for j = 0..7
using ctable_t = std::array<double, 256>;

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
// Direct-indexing approach: each output sample independently computes its
// result by indexing into the input byte array. No circular buffer needed.
// Bytes before the start of the array are treated as silence (warmup).
//
// This independence enables trivial parallelism across output samples.
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

    // ── Worker: process a range of output samples ────────────────────
    auto worker = [&](size_t out_start, size_t out_end) {
        for (size_t s = out_start; s < out_end; ++s) {
            // For output sample s, the newest input byte consumed is at
            // index (s+1)*byte_dec - 1, and the convolution window extends
            // num_tables bytes back from there.
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
    };

    // ── Parallel dispatch ────────────────────────────────────────────
    unsigned n_threads = std::thread::hardware_concurrency();
    if (n_threads < 1) n_threads = 1;
    if (n_threads > 16) n_threads = 16;

    if (n_threads <= 1 || num_out < 10000) {
        worker(0, num_out);
    } else {
        std::vector<std::thread> threads;
        size_t chunk = (num_out + n_threads - 1) / n_threads;
        for (unsigned t = 0; t < n_threads; ++t) {
            size_t start = t * chunk;
            size_t end = std::min(start + chunk, num_out);
            if (start < end)
                threads.emplace_back(worker, start, end);
        }
        for (auto& th : threads) th.join();
    }

    return pcm;
}

// ── Kaiser window I0 (modified Bessel function, first kind, order 0) ──────
inline double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 25; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-15 * sum) break;
    }
    return sum;
}

// ── Design prototype lowpass FIR at DSD rate ───────────────────────────────
// Kaiser-windowed sinc, normalized to unity DC gain.
//
// cutoff_hz:   passband edge in Hz
// fs:          DSD sample rate in Hz
// num_taps:    filter length (should be multiple of 8 for clean LUT grouping)
// beta:        Kaiser parameter (5.0 ≈ 44 dB, 7.86 ≈ 80 dB rejection)

inline std::vector<double> design_prototype_fir(
        double cutoff_hz, double fs, int num_taps, double beta = 5.0) {

    constexpr double PI = 3.14159265358979323846;
    double f_cut = cutoff_hz / fs;
    int half = num_taps / 2;
    double i0_beta = bessel_i0(beta);

    std::vector<double> h(num_taps);
    for (int n = 0; n < num_taps; ++n) {
        double x = n - half;

        // Sinc
        if (std::abs(x) < 1e-10)
            h[n] = 2.0 * f_cut;
        else
            h[n] = std::sin(2.0 * PI * f_cut * x) / (PI * x);

        // Kaiser window
        double t = 2.0 * x / num_taps;
        double arg_sq = 1.0 - t * t;
        if (arg_sq > 0.0)
            h[n] *= bessel_i0(beta * std::sqrt(arg_sq)) / i0_beta;
        else
            h[n] = 0.0;
    }

    // Normalize DC gain to 1.0
    double dc = 0.0;
    for (double v : h) dc += v;
    if (std::abs(dc) > 1e-10)
        for (double& v : h) v /= dc;

    return h;
}

// ── High-level: design FIR + build LUTs + decimate ─────────────────────────
//
// Drop-in replacement for the previous per-bit dsd::decimate_channel().
// Same signature, same output, ~8× faster.
//
// Filter parameters:
//   640 taps, 140 kHz cutoff, Kaiser β=5.0
//   Target output: 352.8 kHz for all DSD rates
//
// These can be tuned to better match a specific reference (foobar2000,
// MaatDROffline, etc.) by adjusting cutoff, beta, and tap count.

inline std::vector<double> decimate_channel(
        const std::vector<uint8_t>& ch_dsd,
        size_t total_bits,
        int decimation,
        bool lsb_first) {

    // ── Filter parameters ────────────────────────────────────────────
    static constexpr int    PROTO_TAPS  = 640;
    static constexpr double CUTOFF_HZ   = 140000.0;
    static constexpr double KAISER_BETA = 5.0;

    double fs_dsd = 352800.0 * decimation;

    // ── Design FIR ───────────────────────────────────────────────────
    auto fir = design_prototype_fir(CUTOFF_HZ, fs_dsd, PROTO_TAPS, KAISER_BETA);

    // ── Build byte LUTs ──────────────────────────────────────────────
    auto tables = build_ctables(fir.data(), PROTO_TAPS, lsb_first);

    // ── Decimate ─────────────────────────────────────────────────────
    return decimate_with_lut(
        ch_dsd.data(), ch_dsd.size(), total_bits,
        tables, decimation);
}

} // namespace dsd
