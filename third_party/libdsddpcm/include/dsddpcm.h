/* dsddpcm.h — libdsddpcm public API (DRAFT / locked surface, no implementation)
 *
 * A standalone, zero-dependency DSD -> PCM decode kernel. Decode only: no
 * container parsing (DSF/DFF), no file I/O, no resampling to arbitrary rates.
 * The caller extracts the raw 1-bit stream and feeds it here.
 *
 * Naming (locked):  library libdsddpcm  |  SONAME libdsddpcm.so.<MAJOR>
 *                   pkg-config module `dsddpcm`  |  header <dsddpcm.h>
 *
 * Contract (locked):
 *   - Linear-phase FIR decimation only (constant group delay).
 *   - Unity DC gain. The +6 dB SACD makeup gain is NOT applied here.
 *   - Flat C ABI, opaque streaming context, C99/C11. A thin optional C++
 *     wrapper may sit on top; it is not part of this ABI.
 *
 * License: BSD-2-Clause.
 */
#ifndef DSDDPCM_H
#define DSDDPCM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- versioning ------------------------------------------------------- */
#define DSDDPCM_VERSION_MAJOR 0
#define DSDDPCM_VERSION_MINOR 2
#define DSDDPCM_VERSION_PATCH 2
/* Runtime SONAME major; must equal DSDDPCM_VERSION_MAJOR at load. */
int dsddpcm_abi_version(void);
const char *dsddpcm_version_string(void);

/* ---- status codes ----------------------------------------------------- */
typedef enum {
    DSDDPCM_OK = 0,
    DSDDPCM_ERR_ARG        = -1,   /* NULL/invalid argument                */
    DSDDPCM_ERR_UNSUPPORTED= -2,   /* rate/chain/filter combo not built    */
    DSDDPCM_ERR_NOMEM      = -3,
    DSDDPCM_ERR_STATE      = -4    /* called out of lifecycle order        */
} dsddpcm_status;

const char *dsddpcm_strerror(dsddpcm_status s);

/* ---- decode topology -------------------------------------------------- */
/* Input DSD rate as a multiple of 44.1 kHz (DSD64 = 2.8224 MHz, etc). */
typedef enum {
    DSDDPCM_DSD64  = 64,
    DSDDPCM_DSD128 = 128,
    DSDDPCM_DSD256 = 256,
    DSDDPCM_DSD512 = 512
} dsddpcm_dsd_rate;

/* Output PCM sample rate family. The 44.1k family is primary (pure integer
 * power-of-two decimation). The 48k family (48/96/192) is interop-only: a
 * rational up-80/down-147 stage off the neighbouring 44.1-family rate,
 * MULTISTAGE chain with the DEFAULT filter only. */
typedef enum {
    DSDDPCM_PCM_44100 = 44100,
    DSDDPCM_PCM_48000 = 48000,
    DSDDPCM_PCM_88200 = 88200,
    DSDDPCM_PCM_96000 = 96000,
    DSDDPCM_PCM_176400= 176400,
    DSDDPCM_PCM_192000= 192000,
    DSDDPCM_PCM_352800= 352800     /* 8fs intermediate, if exposed         */
} dsddpcm_pcm_rate;

/* Decimation chain. Direct = single wide FIR (gentle ~30 kHz corner, passes
 * attenuated top octave). Multistage = LUT->8fs then halving cascade
 * (flat to 20 kHz, brickwall at Nyquist). See the plan for measurement
 * consequences; they are not interchangeable in the top octave. */
typedef enum {
    DSDDPCM_CHAIN_DIRECT = 0,
    DSDDPCM_CHAIN_MULTISTAGE = 1
} dsddpcm_chain;

/* Decimation filter for the MULTISTAGE chain's final 88.2k -> 44.1k stage,
 * populated from the Phase-3 sweep (CRFB10 fixture; see the repo README).
 * Applies only when out_rate is 44100; other rates and the DIRECT chain
 * accept DEFAULT only. All are linear-phase, unity-DC, int32 coefficients.
 *   DEFAULT / EQUIRIPPLE : equiripple 383 taps, 154.7 dB stopband — the
 *                          golden default (tap-cheapest at the point where
 *                          deeper stopbands stop being measurable).
 *   LEASTSQUARES         : firls 767 taps, 153.1 dB — cross-method check.
 *   KAISER               : Kaiser 4095 taps, 169.1 dB — reference/maximum
 *                          decode (higher latency; for offline analysis).
 *   BLACKMAN_HARRIS      : not shipped (never on the Pareto front) ->
 *                          DSDDPCM_ERR_UNSUPPORTED.                        */
typedef enum {
    DSDDPCM_FILTER_DEFAULT = 0,
    DSDDPCM_FILTER_EQUIRIPPLE,
    DSDDPCM_FILTER_KAISER,
    DSDDPCM_FILTER_BLACKMAN_HARRIS,
    DSDDPCM_FILTER_LEASTSQUARES
} dsddpcm_filter;

/* Output sample format. */
typedef enum {
    DSDDPCM_FMT_F32 = 0,           /* float, nominal full-scale +/-1.0     */
    DSDDPCM_FMT_F64,
    DSDDPCM_FMT_S32                /* fixed, full-scale +/- 2^31-1         */
} dsddpcm_format;

/* ---- configuration ---------------------------------------------------- */
typedef struct {
    dsddpcm_dsd_rate  in_rate;
    dsddpcm_pcm_rate  out_rate;
    dsddpcm_chain     chain;
    dsddpcm_filter    filter;
    dsddpcm_format    format;
    unsigned          channels;    /* interleaved                          */
    /* Input bit convention: 1 = MSB is earliest sample in each byte. */
    int               msb_first;
} dsddpcm_config;

/* Fill cfg with the recommended defaults for a given in/out rate
 * (chain = DIRECT, filter = DEFAULT, format = F32, msb_first = 1). */
dsddpcm_status dsddpcm_config_default(dsddpcm_config *cfg,
                                      dsddpcm_dsd_rate  in_rate,
                                      dsddpcm_pcm_rate  out_rate,
                                      unsigned channels);

/* ---- opaque streaming context ---------------------------------------- */
typedef struct dsddpcm_ctx dsddpcm_ctx;

/* dsddpcm_create == dsddpcm_create2(out, cfg, DSDDPCM_BW_DEFAULT). */
dsddpcm_status dsddpcm_create(dsddpcm_ctx **out, const dsddpcm_config *cfg);

/* ---- bandwidth (output low-pass corner), independent of out_rate ------ *
 * The output rate sets how densely the signal is sampled; the bandwidth sets
 * how much of the spectrum is signal. Above 48 kHz output, the halving cascade
 * alone would pass the DSD modulator's shaped ultrasonic noise (at 352.8k,
 * everything up to the ~157 kHz stage-1 corner), so a linear-phase low-pass
 * (flat to the corner, >= 150 dB from 1.4x the corner, unity DC) is added at
 * the end of the chain. Its delay is included in dsddpcm_group_delay().
 *   DSDDPCM_BW_DEFAULT : 50 kHz (Scarlet Book Annex D.1 measurement band).
 *                        May become per-DSD-rate later without an API change.
 *   DSDDPCM_BW_FULL    : no post-filter — the 0.2 behaviour, for archival /
 *                        DXD-style processing where the caller filters later.
 *   any value >= 20000 : custom corner in Hz. Below 20 kHz -> DSDDPCM_ERR_ARG.
 * No effect at 44.1/48 kHz output, or when the corner is at or above the
 * output Nyquist. */
#define DSDDPCM_BW_DEFAULT 0.0
#define DSDDPCM_BW_FULL    (-1.0)
dsddpcm_status dsddpcm_create2(dsddpcm_ctx **out, const dsddpcm_config *cfg,
                               double bandwidth_hz);
void           dsddpcm_free(dsddpcm_ctx *ctx);

/* Clear filter state / history; keep configuration. Call between tracks. */
dsddpcm_status dsddpcm_reset(dsddpcm_ctx *ctx);

/* ---- streaming decode ------------------------------------------------- *
 * Feed packed 1-bit DSD (per channel, interleaved by byte as the caller's
 * container delivers). Produces interleaved PCM frames. Streaming and
 * incremental: call repeatedly. *out_frames returns frames actually written.
 * The library never allocates the output; caller sizes it via
 * dsddpcm_max_output_frames(). */
dsddpcm_status dsddpcm_decode(dsddpcm_ctx *ctx,
                              const uint8_t *dsd_in, size_t in_bytes,
                              void *pcm_out, size_t out_frames_capacity,
                              size_t *out_frames);

/* Drain the filter tail at end-of-stream (flushes group-delay latency). */
dsddpcm_status dsddpcm_flush(dsddpcm_ctx *ctx,
                             void *pcm_out, size_t out_frames_capacity,
                             size_t *out_frames);

/* Upper bound on output frames for a given input byte count (for sizing). */
size_t dsddpcm_max_output_frames(const dsddpcm_ctx *ctx, size_t in_bytes);

/* ---- introspection (needed for true-peak / alignment correctness) ----- *
 * Group delay of the active decode chain, in *output* samples. Linear phase
 * => an integer/half-integer constant; callers use it to time-align decoded
 * peaks with the source. */
double dsddpcm_group_delay(const dsddpcm_ctx *ctx);

/* Total decimation ratio (diagnostics). Returns 0 for the rational 48k-family
 * outputs, where the DSD-to-PCM ratio is not an integer. */
unsigned dsddpcm_decimation(const dsddpcm_ctx *ctx);
unsigned dsddpcm_num_taps(const dsddpcm_ctx *ctx);

#ifdef __cplusplus
}
#endif
#endif /* DSDDPCM_H */
