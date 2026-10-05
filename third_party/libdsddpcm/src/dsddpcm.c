/* dsddpcm.c — libdsddpcm decode kernel.
 *
 * Architecture (see LIBDSDPCM_PLAN.md):
 *   Stage 1 (DSD domain, bit-exact): integer byte-LUT decimation. For each group
 *   of 8 FIR coefficients a 256-entry table holds the dot product against all
 *   possible bytes of +/-1 samples; one output sample costs ceil(N/8) lookups
 *   and int64 adds. Platform-invariant by construction.
 *   Stage 2 (PCM domain): double-precision halfband cascade to the target rate,
 *   then (above 48 kHz output) an optional bandwidth low-pass at the base rate.
 *
 * Golden (unity-DC) contract: integer accumulation is scaled once, in double, by
 * exactly 1/sum(coeffs) per stage, so every stage — and therefore the cascade —
 * has DC gain 1.0. The +6 dB SACD makeup gain is NOT applied.
 *
 * Chains:
 *   DIRECT     = fir1_64 alone, /64 (DSD64 -> 44.1 kHz only).
 *   MULTISTAGE = fir1_8d/fir1_16/fir1_32/fir1_64m (DSD64/128/256/512) to
 *                8fs = 352.8 kHz, then
 *                hb43_150 halvings to the target, with a sweep-ranked final
 *                88.2k -> 44.1k stage (dsddpcm_filter); optional rational
 *                up-80/down-147 stage for the 48k interop family.
 *
 * License: BSD-2-Clause.
 */
#include "dsddpcm.h"
#include "dsddpcm_tables.h"

#include <stdlib.h>
#include <string.h>

#define S1_COMPACT_SLACK 4096   /* stage-1 byte buffer compaction slack */
#define MAX_HB 4         /* up to 3 halvings + the bandwidth post-filter */
#define PI 3.14159265358979323846

/* ---- small helpers ---------------------------------------------------- */

static const char *status_str[] = {
    "ok", "invalid argument", "unsupported configuration",
    "out of memory", "called out of lifecycle order"
};

const char *dsddpcm_strerror(dsddpcm_status s)
{
    int i = -(int)s;
    return (i >= 0 && i <= 4) ? status_str[i] : "unknown status";
}

int dsddpcm_abi_version(void) { return DSDDPCM_VERSION_MAJOR; }

const char *dsddpcm_version_string(void)
{
    return "0.2.2";
}

static uint8_t bit_reverse(uint8_t b)
{
    b = (uint8_t)((b >> 4) | (b << 4));
    b = (uint8_t)(((b & 0xcc) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xaa) >> 1) | ((b & 0x55) << 1));
    return b;
}

/* ---- stage 2: double halfband decimator ------------------------------- */

typedef struct {
    const int32_t *q;
    int n;
    double inv_sum;
    int dec;           /* 2 = halving, 1 = bandwidth post-filter (no decimation) */
} hb_spec;

typedef struct {
    double *dl;        /* circular delay line, length n */
    int w;             /* next write slot */
    int64_t count;     /* samples pushed */
} hb_state;

/* Push one sample; emit conv output y[j] = sum h[k] x[j-k] at j = count-1 for
 * j >= n-1, decimated by dec (j - (n-1) a multiple of dec) — identical
 * alignment to numpy full-convolution [n-1::dec]. Returns 1 if *out was written. */
static int hb_push(const hb_spec *sp, hb_state *st, double x, double *out)
{
    int n = sp->n, k, i;
    double acc = 0.0;
    st->dl[st->w] = x;
    st->w = (st->w + 1) % n;
    st->count++;
    if (st->count < n || (st->count - n) % sp->dec)
        return 0;
    /* newest sample is at slot w-1, weighted by q[0]; walk backwards */
    i = st->w - 1;
    for (k = 0; k < n; k++) {
        if (i < 0) i += n;
        acc += (double)sp->q[k] * st->dl[i];
        i--;
    }
    *out = acc * sp->inv_sum;
    return 1;
}

/* ---- 48k-family rational stage (up L / down M polyphase) --------------- */

typedef struct {
    const int32_t *q;     /* prototype pre-scaled by L (see dsddpcm_tables.h) */
    int n, up, down;
    double inv;           /* golden scale: L / sum(q)                         */
} rs_spec;

typedef struct {
    double *dl;           /* ring of input samples, indexed by absolute count */
    int cap;
    int64_t count;        /* inputs pushed                                    */
    int64_t m;            /* next output index                                */
} rs_state;

/* Push one base-rate sample; emit y[m] = (conv(q, zero-stuffed x))[m*down]*inv
 * when its newest input has arrived — identical alignment to scipy.upfirdn.
 * down > up, so at most one output per input. */
static int rs_push(const rs_spec *sp, rs_state *st, double x, double *out)
{
    int64_t t, i0;
    int p, k;
    double acc = 0.0;

    st->dl[st->count % st->cap] = x;
    st->count++;
    t = st->m * (int64_t)sp->down;
    i0 = t / sp->up;
    if (i0 > st->count - 1)
        return 0;
    p = (int)(t % sp->up);
    for (k = p; k < sp->n; k += sp->up) {
        int64_t idx = i0 - (k - p) / sp->up;
        if (idx < 0)
            break;                                  /* implicit zero history */
        acc += (double)sp->q[k] * st->dl[idx % st->cap];
    }
    st->m++;
    *out = acc * sp->inv;
    return 1;
}

/* ---- bandwidth post-filter, designed at create time ------------------- *
 * Kaiser-windowed sinc low-pass: flat to the corner fp, >= 150 dB from 1.4 fp
 * (design target 160 dB; >= 153 dB survives the x2^30 rounding). It runs as a
 * non-decimating hb stage, so it inherits golden 1/sum scaling and streaming.
 * libm-free on purpose: only IEEE + - * / (no contraction under strict C99), so
 * the rounded integers are identical on every platform and tools/lut_verify.py
 * reproduces them exactly. */
#define BW_DEFAULT_HZ 50000.0   /* Scarlet Book Annex D.1 measurement band */
#define BW_MIN_HZ     20000.0
#define BW_ATTEN_DB   160.0

static double sin_pi(double x)          /* sin(pi x) */
{
    double t, term, s;
    int k;
    x -= 2.0 * (double)(int64_t)(x / 2.0);          /* |x| < 2 */
    if (x > 1.0) x -= 2.0; else if (x < -1.0) x += 2.0;
    if (x > 0.5) x = 1.0 - x; else if (x < -0.5) x = -1.0 - x;
    t = PI * x;
    term = s = t;
    for (k = 1; k < 20; k++) {
        term *= -t * t / ((2.0 * k) * (2.0 * k + 1.0));
        s += term;
    }
    return s;
}

static double i0_of_sq(double z2)       /* I0(sqrt(z2)) */
{
    double term = 1.0, s = 1.0;
    int k;
    for (k = 1; k < 500 && term > 1e-17 * s; k++) {
        term *= z2 / (4.0 * k * k);
        s += term;
    }
    return s;
}

static dsddpcm_status bw_design(int32_t **q_out, int *n_out, int64_t *sum_out,
                                double fp, double fs)
{
    double fst = 1.4 * fp, dw = 2.0 * PI * (fst - fp) / fs;
    double beta = 0.1102 * (BW_ATTEN_DB - 8.7), a = (fp + fst) / fs, i0b, m;
    int n = ((int)((BW_ATTEN_DB - 7.95) / (2.285 * dw)) + 2) | 1, k;
    int32_t *q = malloc((size_t)n * sizeof(*q));
    int64_t sum = 0;
    if (!q)
        return DSDDPCM_ERR_NOMEM;
    m = (double)(n - 1) / 2.0;
    i0b = i0_of_sq(beta * beta);
    for (k = 0; k < n; k++) {
        double j = (double)k - m, r = j / m;
        double h = (j != 0.0 ? sin_pi(a * j) / (PI * j) : a)
                 * i0_of_sq(beta * beta * (1.0 - r * r)) / i0b;
        double v = h * 1073741824.0;                  /* x2^30 */
        q[k] = (int32_t)(v >= 0.0 ? v + 0.5 : v - 0.5);
        sum += q[k];
    }
    *q_out = q; *n_out = n; *sum_out = sum;
    return DSDDPCM_OK;
}

/* ---- stage 1: integer byte-LUT decimator ------------------------------ */

typedef struct {
    int32_t (*lut)[256];  /* [nbytes][256] partial dot products */
    int nbytes;           /* ceil(ntaps/8) */
    int step;             /* decimation / 8 bytes per output */
    double inv_sum;       /* golden unity-DC scale = 1/sum(q) */
} s1_spec;

typedef struct {
    uint8_t *buf;         /* linear byte window, compacted periodically */
    int len, pos, cap;
} s1_state;

static dsddpcm_status s1_build(s1_spec *sp, const int32_t *q, int ntaps,
                               int64_t sum, int decim)
{
    int b, j, v;
    sp->nbytes = (ntaps + 7) / 8;
    sp->step = decim / 8;
    sp->inv_sum = 1.0 / (double)sum;
    sp->lut = calloc((size_t)sp->nbytes, sizeof(*sp->lut));
    if (!sp->lut)
        return DSDDPCM_ERR_NOMEM;
    for (b = 0; b < sp->nbytes; b++) {
        for (v = 0; v < 256; v++) {
            int64_t acc = 0;
            for (j = 0; j < 8; j++) {
                int i = 8 * b + j;
                if (i >= ntaps)
                    break;
                /* MSB of the byte is the earliest sample; bit 1 -> +1, 0 -> -1 */
                acc += ((v >> (7 - j)) & 1) ? (int64_t)q[i] : -(int64_t)q[i];
            }
            sp->lut[b][v] = (int32_t)acc;
        }
    }
    return DSDDPCM_OK;
}

/* Push one input byte; returns 1 if an output sample was produced.
 * First output uses the first nbytes bytes (window x[0 .. ntaps-1]), matching
 * full-convolution alignment [ntaps-1::decim] for symmetric taps. */
static int s1_push(const s1_spec *sp, s1_state *st, uint8_t byte, double *out)
{
    st->buf[st->len++] = byte;
    if (st->len - st->pos >= sp->nbytes) {
        int64_t acc = 0;
        int b;
        const uint8_t *p = st->buf + st->pos;
        for (b = 0; b < sp->nbytes; b++)
            acc += sp->lut[b][p[b]];
        st->pos += sp->step;
        *out = (double)acc * sp->inv_sum;
        if (st->pos >= S1_COMPACT_SLACK) {
            memmove(st->buf, st->buf + st->pos, (size_t)(st->len - st->pos));
            st->len -= st->pos;
            st->pos = 0;
        }
        return 1;
    }
    return 0;
}

/* ---- context ----------------------------------------------------------- */

struct dsddpcm_ctx {
    dsddpcm_config cfg;
    double bandwidth;       /* as passed to create2 */
    int32_t *bwq;           /* owned post-filter coefficients, or NULL */
    s1_spec s1;
    hb_spec hb[MAX_HB];
    int n_hb;
    rs_spec rs;             /* 48k-family rational stage (use_rs only) */
    int use_rs;
    unsigned decim;         /* DSD-samples per BASE-rate frame (integer) */
    unsigned num_taps;
    double group_delay;     /* in output samples */
    unsigned span_dsd;      /* composite impulse span in DSD samples */
    int flushing;           /* idle bytes still to feed per channel, -1 = done */

    s1_state *s1st;         /* per channel */
    hb_state *hbst;         /* per channel * n_hb */
    rs_state *rsst;         /* per channel (use_rs only) */
};

/* Middle /2 halvings: hb43_150 (Kaiser half-band, >=148 dB) supersedes the
 * incumbent-recipe fir2_2, whose -109 dB constraint capped the composite
 * response in the 66-74 kHz fold band (docs/COMPARISON.md). fir2_2 stays in
 * the tables for provenance. */
static const hb_spec HB_MID = { dsddpcm_hb43_150, DSDDPCM_HB43_150_N,
                                1.0 / (double)DSDDPCM_HB43_150_SUM, 2 };
/* Final 88.2k -> 44.1k stage: the Phase-3 sweep winners (see dsddpcm_tables.h).
 * fir3_2 (the incumbent-recipe halfband) remains in the tables for provenance but
 * is superseded here by the ranked winners. */
static const hb_spec HB_EQ383 = { dsddpcm_eq383_w30, DSDDPCM_EQ383_W30_N,
                                  1.0 / (double)DSDDPCM_EQ383_W30_SUM, 2 };
static const hb_spec HB_FIRLS767 = { dsddpcm_firls767_w10, DSDDPCM_FIRLS767_W10_N,
                                     1.0 / (double)DSDDPCM_FIRLS767_W10_SUM, 2 };
static const hb_spec HB_KAISER4095 = { dsddpcm_kaiser4095_a150,
                                       DSDDPCM_KAISER4095_A150_N,
                                       1.0 / (double)DSDDPCM_KAISER4095_A150_SUM, 2 };

/* Map the public filter enum to the final-stage spec; NULL = invalid choice. */
static const hb_spec *final_stage_spec(dsddpcm_filter f)
{
    switch (f) {
    case DSDDPCM_FILTER_DEFAULT:
    case DSDDPCM_FILTER_EQUIRIPPLE:      return &HB_EQ383;
    case DSDDPCM_FILTER_LEASTSQUARES:    return &HB_FIRLS767;
    case DSDDPCM_FILTER_KAISER:          return &HB_KAISER4095;
    case DSDDPCM_FILTER_BLACKMAN_HARRIS: /* not on the Pareto front; not shipped */
    default:                             return NULL;
    }
}

dsddpcm_status dsddpcm_config_default(dsddpcm_config *cfg,
                                      dsddpcm_dsd_rate in_rate,
                                      dsddpcm_pcm_rate out_rate,
                                      unsigned channels)
{
    if (!cfg || channels == 0)
        return DSDDPCM_ERR_ARG;
    memset(cfg, 0, sizeof(*cfg));
    cfg->in_rate = in_rate;
    cfg->out_rate = out_rate;
    cfg->chain = (in_rate == DSDDPCM_DSD64 && out_rate == DSDDPCM_PCM_44100)
                 ? DSDDPCM_CHAIN_DIRECT : DSDDPCM_CHAIN_MULTISTAGE;
    cfg->filter = DSDDPCM_FILTER_DEFAULT;
    cfg->format = DSDDPCM_FMT_F32;
    cfg->channels = channels;
    cfg->msb_first = 1;
    return DSDDPCM_OK;
}

static dsddpcm_status setup_chain(dsddpcm_ctx *c)
{
    const dsddpcm_config *cfg = &c->cfg;
    unsigned fs_in = (unsigned)cfg->in_rate * 44100u;
    unsigned base_out = (unsigned)cfg->out_rate;
    dsddpcm_status st;
    int i;

    /* 48k family = rational up-80/down-147 stage off the neighbouring
     * 44.1-family "base" rate: 88.2k->48k, 176.4k->96k, 352.8k->192k. */
    c->use_rs = 0;
    if (base_out == 48000u || base_out == 96000u || base_out == 192000u) {
        if (cfg->chain != DSDDPCM_CHAIN_MULTISTAGE ||
            cfg->filter != DSDDPCM_FILTER_DEFAULT)
            return DSDDPCM_ERR_UNSUPPORTED;
        base_out = base_out / DSDDPCM_R48_UP * DSDDPCM_R48_DOWN;
        c->use_rs = 1;
        c->rs.q = dsddpcm_r48_150;
        c->rs.n = DSDDPCM_R48_150_N;
        c->rs.up = DSDDPCM_R48_UP;
        c->rs.down = DSDDPCM_R48_DOWN;
        c->rs.inv = (double)DSDDPCM_R48_UP / (double)DSDDPCM_R48_150_SUM;
    }
    if (base_out > fs_in || (fs_in % base_out) != 0)
        return DSDDPCM_ERR_UNSUPPORTED;
    c->decim = fs_in / base_out;

    if (cfg->chain == DSDDPCM_CHAIN_DIRECT) {
        if (cfg->in_rate != DSDDPCM_DSD64 || cfg->out_rate != DSDDPCM_PCM_44100)
            return DSDDPCM_ERR_UNSUPPORTED;  /* fir1_64 corner is DSD64/44.1k */
        if (cfg->filter != DSDDPCM_FILTER_DEFAULT)
            return DSDDPCM_ERR_UNSUPPORTED;  /* Direct has one filter by design */
        st = s1_build(&c->s1, dsddpcm_fir1_64, DSDDPCM_FIR1_64_N,
                      DSDDPCM_FIR1_64_SUM, 64);
        if (st != DSDDPCM_OK)
            return st;
        c->n_hb = 0;
        c->num_taps = DSDDPCM_FIR1_64_N;
        c->span_dsd = DSDDPCM_FIR1_64_N;
        c->group_delay = (double)(DSDDPCM_FIR1_64_N - 1) / 2.0 / (double)c->decim;
        return DSDDPCM_OK;
    }

    /* MULTISTAGE: stage 1 to 8fs = 352.8 kHz, then halvings (then rs). */
    {
        const int32_t *q;
        int n, s1d;
        int64_t sum;
        unsigned halvings, gd_num;
        if (cfg->in_rate == DSDDPCM_DSD64) {
            /* fir1_8d supersedes the incumbent-recipe fir1_8 (kept in the tables
             * for provenance), lifting the stage-1 fold-band floor -141 -> -157 dB. */
            q = dsddpcm_fir1_8d; n = DSDDPCM_FIR1_8D_N;
            sum = DSDDPCM_FIR1_8D_SUM; s1d = 8;
        } else if (cfg->in_rate == DSDDPCM_DSD128) {
            q = dsddpcm_fir1_16; n = DSDDPCM_FIR1_16_N;
            sum = DSDDPCM_FIR1_16_SUM; s1d = 16;
        } else if (cfg->in_rate == DSDDPCM_DSD256) {
            q = dsddpcm_fir1_32; n = DSDDPCM_FIR1_32_N;
            sum = DSDDPCM_FIR1_32_SUM; s1d = 32;
        } else if (cfg->in_rate == DSDDPCM_DSD512) {
            q = dsddpcm_fir1_64m; n = DSDDPCM_FIR1_64M_N;
            sum = DSDDPCM_FIR1_64M_SUM; s1d = 64;
        } else {
            return DSDDPCM_ERR_UNSUPPORTED;
        }
        if (c->decim % (unsigned)s1d != 0)
            return DSDDPCM_ERR_UNSUPPORTED;
        halvings = c->decim / (unsigned)s1d;   /* 1, 2, 4 or 8 */
        for (c->n_hb = 0; halvings > 1; halvings >>= 1) {
            if (c->n_hb == MAX_HB || (halvings & 1))
                return DSDDPCM_ERR_UNSUPPORTED;
            c->n_hb++;
        }
        /* The ranked winner occupies the tight final stage (88.2k -> 44.1k). At
         * higher base rates that stage does not exist, so only DEFAULT applies. */
        if (base_out == 44100u) {
            const hb_spec *fin = final_stage_spec(cfg->filter);
            if (!fin)
                return DSDDPCM_ERR_UNSUPPORTED;
            for (i = 0; i < c->n_hb; i++)
                c->hb[i] = (i == c->n_hb - 1) ? *fin : HB_MID;
        } else {
            if (cfg->filter != DSDDPCM_FILTER_DEFAULT)
                return DSDDPCM_ERR_UNSUPPORTED;
            for (i = 0; i < c->n_hb; i++)
                c->hb[i] = HB_MID;
        }

        /* Bandwidth post-filter at the base rate. Only above 48 kHz output:
         * at 44.1/48k the final stage already brickwalls below 24 kHz. A corner
         * at or above the output Nyquist is a no-op. */
        if (cfg->out_rate > 48000 && c->bandwidth >= 0.0) {
            double fp = c->bandwidth == 0.0 ? BW_DEFAULT_HZ : c->bandwidth;
            if (fp < (double)cfg->out_rate / 2.0) {
                int bn;
                int64_t bsum;
                st = bw_design(&c->bwq, &bn, &bsum, fp, (double)base_out);
                if (st != DSDDPCM_OK)
                    return st;
                c->hb[c->n_hb].q = c->bwq;
                c->hb[c->n_hb].n = bn;
                c->hb[c->n_hb].inv_sum = 1.0 / (double)bsum;
                c->hb[c->n_hb].dec = 1;
                c->n_hb++;
            }
        }

        st = s1_build(&c->s1, q, n, sum, s1d);
        if (st != DSDDPCM_OK)
            return st;
        c->num_taps = (unsigned)n;
        c->span_dsd = (unsigned)(n - 1) + 1;
        gd_num = (unsigned)(n - 1);            /* 2*delay in DSD samples */
        {
            unsigned up = (unsigned)s1d;       /* DSD samples per stage input */
            for (i = 0; i < c->n_hb; i++) {
                c->num_taps += (unsigned)c->hb[i].n;
                c->span_dsd += (unsigned)(c->hb[i].n - 1) * up;
                gd_num += (unsigned)(c->hb[i].n - 1) * up;
                up *= (unsigned)c->hb[i].dec;
            }
        }
        c->group_delay = (double)gd_num / 2.0 / (double)c->decim;
        if (c->use_rs) {
            /* rs runs at lcm = base*up; convert its delay to output samples and
             * re-express the total at the (rational) output rate. */
            double gd_sec = (double)gd_num / 2.0 / (double)fs_in
                          + (double)(c->rs.n - 1) / 2.0
                            / ((double)base_out * (double)c->rs.up);
            c->group_delay = gd_sec * (double)cfg->out_rate;
            c->num_taps += (unsigned)c->rs.n;
            c->span_dsd += (unsigned)(((c->rs.n - 1) / c->rs.up) + 1) * c->decim;
        }
        return DSDDPCM_OK;
    }
}

dsddpcm_status dsddpcm_create(dsddpcm_ctx **out, const dsddpcm_config *cfg)
{
    return dsddpcm_create2(out, cfg, DSDDPCM_BW_DEFAULT);
}

dsddpcm_status dsddpcm_create2(dsddpcm_ctx **out, const dsddpcm_config *cfg,
                               double bandwidth_hz)
{
    dsddpcm_ctx *c;
    dsddpcm_status st;
    unsigned ch;
    int i;

    if (!out || !cfg || cfg->channels == 0)
        return DSDDPCM_ERR_ARG;
    *out = NULL;
    if (cfg->format != DSDDPCM_FMT_F32 && cfg->format != DSDDPCM_FMT_F64 &&
        cfg->format != DSDDPCM_FMT_S32)
        return DSDDPCM_ERR_ARG;
    /* DEFAULT (0), FULL (< 0), or a corner >= 20 kHz; rejects NaN too */
    if (!(bandwidth_hz <= 0.0 || bandwidth_hz >= BW_MIN_HZ))
        return DSDDPCM_ERR_ARG;

    c = calloc(1, sizeof(*c));
    if (!c)
        return DSDDPCM_ERR_NOMEM;
    c->cfg = *cfg;
    c->bandwidth = bandwidth_hz;
    st = setup_chain(c);
    if (st != DSDDPCM_OK) {
        dsddpcm_free(c);
        return st;
    }

    c->s1st = calloc(cfg->channels, sizeof(*c->s1st));
    c->hbst = calloc((size_t)cfg->channels * (size_t)(c->n_hb ? c->n_hb : 1),
                     sizeof(*c->hbst));
    if (!c->s1st || !c->hbst) {
        dsddpcm_free(c);
        return DSDDPCM_ERR_NOMEM;
    }
    if (c->use_rs) {
        c->rsst = calloc(cfg->channels, sizeof(*c->rsst));
        if (!c->rsst) {
            dsddpcm_free(c);
            return DSDDPCM_ERR_NOMEM;
        }
    }
    for (ch = 0; ch < cfg->channels; ch++) {
        s1_state *s = &c->s1st[ch];
        s->cap = c->s1.nbytes + S1_COMPACT_SLACK + 8;  /* +step slack pre-compaction */
        s->buf = malloc((size_t)s->cap);
        if (!s->buf) {
            dsddpcm_free(c);
            return DSDDPCM_ERR_NOMEM;
        }
        for (i = 0; i < c->n_hb; i++) {
            hb_state *h = &c->hbst[ch * c->n_hb + i];
            h->dl = calloc((size_t)c->hb[i].n, sizeof(double));
            if (!h->dl) {
                dsddpcm_free(c);
                return DSDDPCM_ERR_NOMEM;
            }
        }
        if (c->use_rs) {
            rs_state *r = &c->rsst[ch];
            r->cap = c->rs.n / c->rs.up + 2;
            r->dl = calloc((size_t)r->cap, sizeof(double));
            if (!r->dl) {
                dsddpcm_free(c);
                return DSDDPCM_ERR_NOMEM;
            }
        }
    }
    c->flushing = 0;
    *out = c;
    return DSDDPCM_OK;
}

void dsddpcm_free(dsddpcm_ctx *c)
{
    unsigned ch;
    int i;
    if (!c)
        return;
    if (c->s1st)
        for (ch = 0; ch < c->cfg.channels; ch++)
            free(c->s1st[ch].buf);
    if (c->hbst)
        for (ch = 0; ch < c->cfg.channels; ch++)
            for (i = 0; i < c->n_hb; i++)
                free(c->hbst[ch * c->n_hb + i].dl);
    if (c->rsst)
        for (ch = 0; ch < c->cfg.channels; ch++)
            free(c->rsst[ch].dl);
    free(c->s1st);
    free(c->hbst);
    free(c->rsst);
    free(c->s1.lut);
    free(c->bwq);
    free(c);
}

dsddpcm_status dsddpcm_reset(dsddpcm_ctx *c)
{
    unsigned ch;
    int i;
    if (!c)
        return DSDDPCM_ERR_ARG;
    for (ch = 0; ch < c->cfg.channels; ch++) {
        c->s1st[ch].len = c->s1st[ch].pos = 0;
        for (i = 0; i < c->n_hb; i++) {
            hb_state *h = &c->hbst[ch * c->n_hb + i];
            memset(h->dl, 0, (size_t)c->hb[i].n * sizeof(double));
            h->w = 0;
            h->count = 0;
        }
        if (c->use_rs) {
            rs_state *r = &c->rsst[ch];
            memset(r->dl, 0, (size_t)r->cap * sizeof(double));
            r->count = r->m = 0;
        }
    }
    c->flushing = 0;
    return DSDDPCM_OK;
}

/* Run one input byte for one channel through the whole chain.
 * Returns 1 if a final-rate sample emerged in *out. */
static int chain_push(dsddpcm_ctx *c, unsigned ch, uint8_t byte, double *out)
{
    double v;
    int i;
    if (!s1_push(&c->s1, &c->s1st[ch], byte, &v))
        return 0;
    for (i = 0; i < c->n_hb; i++)
        if (!hb_push(&c->hb[i], &c->hbst[ch * c->n_hb + i], v, &v))
            return 0;
    if (c->use_rs && !rs_push(&c->rs, &c->rsst[ch], v, &v))
        return 0;
    *out = v;
    return 1;
}

static void store(void *dst, dsddpcm_format fmt, size_t idx, double v)
{
    switch (fmt) {
    case DSDDPCM_FMT_F32:
        ((float *)dst)[idx] = (float)v;
        break;
    case DSDDPCM_FMT_F64:
        ((double *)dst)[idx] = v;
        break;
    case DSDDPCM_FMT_S32:
    default: {
        double s = v * 2147483647.0;
        if (s > 2147483647.0)  s = 2147483647.0;
        if (s < -2147483648.0) s = -2147483648.0;
        ((int32_t *)dst)[idx] = (int32_t)s;
        break;
    }
    }
}

size_t dsddpcm_max_output_frames(const dsddpcm_ctx *c, size_t in_bytes)
{
    size_t per_ch, base;
    if (!c || c->cfg.channels == 0)
        return 0;
    per_ch = in_bytes / c->cfg.channels;
    base = (per_ch * 8u) / c->decim + 2u;
    if (c->use_rs)
        return (base * (size_t)c->rs.up) / (size_t)c->rs.down + 2u;
    return base;
}

dsddpcm_status dsddpcm_decode(dsddpcm_ctx *c,
                              const uint8_t *dsd_in, size_t in_bytes,
                              void *pcm_out, size_t out_frames_capacity,
                              size_t *out_frames)
{
    size_t nb, g, frames = 0;
    unsigned ch;

    if (!c || !dsd_in || !pcm_out || !out_frames)
        return DSDDPCM_ERR_ARG;
    *out_frames = 0;
    if (c->flushing)
        return DSDDPCM_ERR_STATE;            /* stream ended; reset() to reuse */
    if (in_bytes % c->cfg.channels != 0)
        return DSDDPCM_ERR_ARG;
    if (out_frames_capacity < dsddpcm_max_output_frames(c, in_bytes))
        return DSDDPCM_ERR_ARG;              /* caller must size via the bound */

    nb = in_bytes / c->cfg.channels;
    for (g = 0; g < nb; g++) {
        int got = 0;
        for (ch = 0; ch < c->cfg.channels; ch++) {
            uint8_t b = dsd_in[g * c->cfg.channels + ch];
            double v;
            if (!c->cfg.msb_first)
                b = bit_reverse(b);
            if (chain_push(c, ch, b, &v)) {
                store(pcm_out, c->cfg.format,
                      frames * c->cfg.channels + ch, v);
                got++;
            }
        }
        if (got) {                            /* channels advance in lockstep */
            if ((unsigned)got != c->cfg.channels)
                return DSDDPCM_ERR_STATE;
            frames++;
        }
    }
    *out_frames = frames;
    return DSDDPCM_OK;
}

dsddpcm_status dsddpcm_flush(dsddpcm_ctx *c,
                             void *pcm_out, size_t out_frames_capacity,
                             size_t *out_frames)
{
    size_t frames = 0;
    unsigned ch;

    if (!c || !pcm_out || !out_frames)
        return DSDDPCM_ERR_ARG;
    *out_frames = 0;
    if (c->flushing < 0)
        return DSDDPCM_ERR_STATE;            /* already fully flushed */
    if (c->flushing == 0)                     /* idle bytes covering the tail */
        c->flushing = (int)((c->span_dsd + 7u) / 8u + (unsigned)c->s1.step);

    while (c->flushing > 0) {
        int got = 0;
        if (frames >= out_frames_capacity)
            return (*out_frames = frames), DSDDPCM_OK;  /* call again */
        for (ch = 0; ch < c->cfg.channels; ch++) {
            double v;
            /* 0x55 = DSD idle (alternating -1/+1): zero mean, energy at fs/2,
             * far above every passband — the physical end-of-stream pattern. */
            if (chain_push(c, ch, 0x55, &v)) {
                store(pcm_out, c->cfg.format,
                      frames * c->cfg.channels + ch, v);
                got++;
            }
        }
        if (got)
            frames++;
        c->flushing--;
    }
    c->flushing = -1;
    *out_frames = frames;
    return DSDDPCM_OK;
}

double dsddpcm_group_delay(const dsddpcm_ctx *c)
{
    return c ? c->group_delay : 0.0;
}

unsigned dsddpcm_decimation(const dsddpcm_ctx *c)
{
    return (c && !c->use_rs) ? c->decim : 0u;   /* 0 = rational (48k family) */
}

unsigned dsddpcm_num_taps(const dsddpcm_ctx *c)
{
    return c ? c->num_taps : 0u;
}
