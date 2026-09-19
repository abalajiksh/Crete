#pragma once
// ============================================================================
// dst.hpp — DST (Direct Stream Transfer) decoder
//
// SACD's lossless compression for DSD. Decodes one DST frame back to the raw
// 1-bit-per-channel DSD it encodes, which sacd.hpp then hands to the ordinary
// libdsddpcm decimation path — so a DST-compressed multichannel area measures
// through exactly the same chain as every .dsf and .dff.
//
// PROVENANCE. Written from the normative syntax and semantics of
// ISO/IEC 14496-3 Subpart 10 ("Technical description of lossless coding of
// oversampled audio"), which is where DST is specified. Syntax tables and
// decoding equations are the format itself — facts, not expression — and are
// the only thing used here. NO existing implementation was consulted or
// derived from: sacd_extract's DST decoder is `#ifdef __lv2ppu__`, a PS3 SPU
// offload with no software path to derive from even if its GPL allowed it,
// and FFmpeg's dstdec.c is LGPL, which is precisely the licence class crête
// removed when it swapped out the old DSD engine. crête stays MIT.
//
// ── How DST works, in one paragraph ────────────────────────────────────────
//
// Each 1/75 s frame codes every channel's DSD bits with a linear predictor
// plus an arithmetic coder. A frame carries a small set of FIR prediction
// filters and a small set of probability tables, and partitions each
// channel's frame into SEGMENTS that each pick one filter and one table. For
// bit n the filter runs over the channel's own past DSD bits (as +/-1) giving
// Z[n]; its sign is the predicted bit F[n], and its magnitude indexes the
// probability table to give P[n], the likelihood that the prediction is
// right. The arithmetic decoder turns the coded data plus P[n] into E[n], the
// "prediction was wrong" flag, and the real bit is X[n] = E[n] XOR F[n].
// Channels are interleaved bit-by-bit through one arithmetic decoder, so they
// cannot be decoded independently or in parallel within a frame.
//
// ── Two things the spec says that are easy to get wrong ────────────────────
//
// THE PROSE IS RIGHT AND THE FLOWCHART IS NOT. Clause 10.7.2.2 says E=1 when
// "C is greater than or equal to A-Q"; Figure 10.10 draws the test as
// "C > A-Q". They disagree, and `>=` is the one that works. This was
// implemented the other way first and the failure is worth describing,
// because it does NOT announce itself: every frame still decoded, every
// frame header parsed, mispredictions came out at a plausible-looking 0.75%,
// and the output was 1-bit data with ~0.5 density that decimated to
// something audible. What it actually produced was the prediction filter
// free-running on its own output.
//
// The measurement that exposed it is BIT CONSUMPTION. Arithmetic coding
// consumes exactly the bits the encoder produced if and only if the decoder
// applies the same probability sequence, so "fraction of the frame consumed"
// is a self-checking oracle that needs no reference decoder. With `>` the
// decoder used 15% of each frame; with `>=` it uses 100.0%, on quiet and
// loud material alike. Everything else agreed with both readings.
//
// Do not "fix" this back to the flowchart, and if this decoder is ever
// modified, check consumption first -- see decode_frame()'s final assert.
//
// Table 10.20's coefficient-prediction coefficients are fractional for
// CC_Method '10' (-9/8, -5/8, 6/8), which is why Table 10.13 accumulates
// 8*CCPC and then divides with explicit round-half-away-from-zero. Keep the
// integer form; a floating-point transcription rounds differently.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace dst {

// Limits from the spec (10.6.2.1.2.5 / .6 / .8 / .9).
static constexpr unsigned MAX_CHANNELS   = 8;
static constexpr unsigned MAX_PRED_ORDER = 128;   // Coded_Pred_Order is 7 bits
static constexpr unsigned MAX_ELEMENTS   = 16;    // Element is 0..15
static constexpr unsigned MAXNRSEGS_F    = 4;     // Filter_Segmentation
static constexpr unsigned MAXNRSEGS_P    = 8;     // Ptable_Segmentation
static constexpr unsigned MINSEGLEN_F    = 128;   // bytes
static constexpr unsigned MINSEGLEN_P    = 4;     // bytes

// ── Bit reader, MSB-first ──────────────────────────────────────────────────
// Past the end it returns 0 rather than throwing: the spec requires exactly
// that of the arithmetic decoder's "Input next bit D" ("After all bits from
// A_Data[] have been read, the function sets bit D to 0"), and the header
// parse is bounds-checked separately by the caller.
class BitReader {
public:
    BitReader(const uint8_t* p, size_t n) : p_(p), nbits_(n * 8) {}

    unsigned bit() {
        if (pos_ >= nbits_) { ++pos_; return 0; }
        const unsigned b = (p_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1;
        ++pos_;
        return b;
    }
    uint32_t bits(unsigned n) {          // unsigned, MSB first
        uint32_t v = 0;
        while (n--) v = (v << 1) | bit();
        return v;
    }
    int32_t sbits(unsigned n) {          // two's complement, n bits
        const uint32_t v = bits(n);
        const uint32_t sign = 1u << (n - 1);
        return (v & sign) ? static_cast<int32_t>(v) - static_cast<int32_t>(sign << 1)
                          : static_cast<int32_t>(v);
    }
    size_t pos()       const { return pos_; }
    size_t size_bits() const { return nbits_; }
    bool   overrun()   const { return pos_ > nbits_; }

private:
    const uint8_t* p_;
    size_t nbits_;
    size_t pos_ = 0;
};

// Bits needed to code a value in [0, v]: n such that 2^(n-1) <= v < 2^n.
// Table 10.16 (Scaled_Length, from Range) and Table 10.17 (Element, from
// Nr_Of_Elements) are the same function; Table 10.17 additionally gives 0
// bits when Nr_Of_Elements is 0, which falls out of bit_width(0) == 0.
inline unsigned bit_width(uint32_t v) {
    unsigned n = 0;
    while (v) { ++n; v >>= 1; }
    return n;
}

// ── Per-frame decoded state ────────────────────────────────────────────────
struct Segments {
    // [channel][segment] -> start byte, and the element (filter/table) index
    unsigned n_seg[MAX_CHANNELS] = {0};
    uint32_t start[MAX_CHANNELS][MAXNRSEGS_P + 1] = {{0}};
    unsigned elem [MAX_CHANNELS][MAXNRSEGS_P] = {{0}};
    unsigned n_elements = 0;
};

struct Filter {
    unsigned order = 0;
    int32_t  coef[MAX_PRED_ORDER] = {0};
    // Tap-group lookup: group g covers taps 8g..8g+7, indexed by the 8 history
    // bits those taps see. Turns the per-bit dot product from up to 128
    // multiply-adds into at most 16 table lookups, which is what makes
    // decoding a 43-minute album finish this side of an hour. Y[] is always
    // +/-1, so every partial sum is enumerable.
    int32_t  table[MAX_PRED_ORDER / 8][256] = {{0}};
    unsigned n_groups = 0;

    void build_table() {
        n_groups = (order + 7) / 8;
        for (unsigned g = 0; g < n_groups; ++g) {
            for (unsigned b = 0; b < 256; ++b) {
                int32_t s = 0;
                for (unsigned j = 0; j < 8; ++j) {
                    const unsigned tap = g * 8 + j;
                    if (tap >= order) break;
                    s += ((b >> j) & 1) ? coef[tap] : -coef[tap];
                }
                table[g][b] = s;
            }
        }
    }
};

struct Ptable {
    unsigned len = 1;
    int32_t  p_one[256] = {0};   // Coded_Ptable_Len is 6 bits -> len <= 64
};

// ── The decoder ────────────────────────────────────────────────────────────
class Decoder {
public:
    // n_channels and frame_bytes come from the SACD area (6 channels, 4704
    // bytes per channel per 1/75 s frame at DSD64).
    Decoder(unsigned n_channels, unsigned frame_bytes)
        : nch_(n_channels), flen_(frame_bytes) {
        if (nch_ == 0 || nch_ > MAX_CHANNELS)
            throw std::runtime_error("DST: unsupported channel count " +
                                     std::to_string(nch_));
        for (unsigned c = 0; c < nch_; ++c) out_[c].resize(flen_);
        fseg_of_byte_.resize(flen_ * nch_);
        pseg_of_byte_.resize(flen_ * nch_);
    }

    // Decodes one DST frame. out[c] holds frame_bytes of DSD for channel c,
    // MSB-first, exactly as an uncompressed SACD frame would.
    void decode_frame(const uint8_t* data, size_t size) {
        BitReader br(data, size);
        frame_bits_ = size * 8;
        // Reusable across frames: the output is OR-ed into, so it must start
        // clear or a second frame would be a union of two.
        for (unsigned c = 0; c < nch_; ++c)
            std::fill(out_[c].begin(), out_[c].end(), uint8_t{0});
        n_errors_ = 0;

        const unsigned processing_mode = br.bit();
        if (processing_mode == 0) {
            // Not DST coded: DST_X_Bit, 6 reserved bits, then raw DSD in the
            // same channel-interleaved byte order an uncompressed frame uses.
            br.bit();
            br.bits(6);
            for (unsigned b = 0; b < flen_; ++b)
                for (unsigned c = 0; c < nch_; ++c)
                    out_[c][b] = static_cast<uint8_t>(br.bits(8));
            if (br.overrun()) throw std::runtime_error("DST: truncated raw frame");
            return;
        }

        read_segmentation(br);
        read_mapping(br);
        for (unsigned c = 0; c < nch_; ++c) half_prob_[c] = br.bit();
        read_filters(br);
        read_ptables(br);

        // The header must not have run past the frame. A misparse here
        // otherwise sails on into the arithmetic data and produces noise that
        // still looks like audio -- the failure mode this whole file has to
        // avoid.
        if (br.overrun() || br.pos() >= br.size_bits())
            throw std::runtime_error("DST: frame header overruns the frame "
                                     "(parsed " + std::to_string(br.pos()) +
                                     " of " + std::to_string(br.size_bits()) + " bits)");

        hdr_bits_ = br.pos();
        build_segment_maps();
        arithmetic_decode(br);
        used_bits_ = br.pos();

        // THE self-check, and the only one that catches a wrong probability
        // sequence. Arithmetic coding is exact: the decoder consumes exactly
        // the bits the encoder emitted iff it applies the same probabilities.
        // Anything else means the filters, the Ptables, the segment mapping
        // or the coder itself is wrong -- and every one of those failures
        // still yields plausible-looking 1-bit audio, so nothing downstream
        // would notice. Measured on this corpus: 100.0% on every frame.
        //
        // The tolerance allows the byte-alignment stuffing the spec mentions
        // ("the last bits of A_Data[] ... are stuffing bits for alignment"),
        // plus the 13-bit init prefetch and one renormalisation window.
        if (used_bits_ + 64 < frame_bits_ || used_bits_ > frame_bits_ + 64)
            throw std::runtime_error(
                "DST: decoder consumed " + std::to_string(used_bits_) +
                " of " + std::to_string(frame_bits_) + " frame bits -- the "
                "probability sequence does not match the encoder's, so the "
                "output is not this frame's audio");
    }

    const std::vector<uint8_t>& channel(unsigned c) const { return out_[c]; }
    // Clause 10.7.3.1 caps mispredictions at half the samples in a frame.
    // A desynchronised decoder sits at ~50%; a working one is far below, and
    // that is the cheapest internal check there is -- it needs no reference.
    uint64_t n_errors() const { return n_errors_; }
    uint64_t n_samples() const { return (uint64_t)flen_ * 8 * nch_; }
    size_t hdr_bits()  const { return hdr_bits_; }
    size_t used_bits() const { return used_bits_; }
    size_t frame_bits() const { return frame_bits_; }

private:
    unsigned nch_, flen_;
    std::vector<uint8_t> out_[MAX_CHANNELS];

    Segments fseg_, pseg_;
    unsigned half_prob_[MAX_CHANNELS] = {0};
    std::vector<Filter> filters_;
    std::vector<Ptable> ptables_;
    // byte -> element index, flattened [channel*flen + byte]
    std::vector<uint8_t> fseg_of_byte_, pseg_of_byte_;
    uint64_t n_errors_ = 0;
    size_t hdr_bits_ = 0, used_bits_ = 0, frame_bits_ = 0;
public:
private:

    // ── Segmentation (Tables 10.5 – 10.7) ──────────────────────────────────
    void read_segmentation(BitReader& br) {
        const unsigned same = br.bit();
        if (same == 0) {
            segment_alloc(br, fseg_, MINSEGLEN_F, MAXNRSEGS_F);
            segment_alloc(br, pseg_, MINSEGLEN_P, MAXNRSEGS_P);
        } else {
            // One segmentation drives both. MINSEGLEN/MAXNRSEGS take the
            // Filter_And_Ptable_Segmentation values (128 / 4), NOT the Ptable
            // ones -- using 4 here shifts every Scaled_Length field.
            segment_alloc(br, fseg_, MINSEGLEN_F, MAXNRSEGS_F);
            pseg_ = fseg_;
        }
    }

    void segment_alloc(BitReader& br, Segments& s, unsigned minseglen,
                       unsigned maxnrsegs) {
        // Resolution is read once, in the first channel that has more than
        // one segment, and is shared by the remaining channels of THIS
        // allocation only (filter and ptable segmentation each have their
        // own Resolution_Read).
        bool res_read = false;
        uint32_t resolution = 0;

        const unsigned same_all = br.bit();
        const unsigned n_read = same_all ? 1u : nch_;
        for (unsigned c = 0; c < n_read; ++c)
            channel_segmentation(br, s, c, minseglen, maxnrsegs, res_read, resolution);
        if (same_all)
            for (unsigned c = 1; c < nch_; ++c) {
                s.n_seg[c] = s.n_seg[0];
                std::memcpy(s.start[c], s.start[0], sizeof(s.start[0]));
            }
    }

    void channel_segmentation(BitReader& br, Segments& s, unsigned c,
                              unsigned minseglen, unsigned maxnrsegs,
                              bool& res_read, uint32_t& resolution) {
        unsigned n = 1;
        s.start[c][0] = 0;
        unsigned end = br.bit();
        while (end == 0) {
            if (n >= maxnrsegs)
                throw std::runtime_error("DST: too many segments");
            if (!res_read) {
                resolution = br.bits(13);
                if (resolution == 0 || resolution > flen_ - minseglen)
                    throw std::runtime_error("DST: bad segment Resolution " +
                                             std::to_string(resolution));
                res_read = true;
            }
            // Table 10.16: the field width follows from how much frame is
            // left, so it changes segment to segment.
            const int64_t room = static_cast<int64_t>(flen_) - s.start[c][n - 1]
                               - static_cast<int64_t>(minseglen);
            if (room < 0) throw std::runtime_error("DST: segment past end of frame");
            const uint32_t range = static_cast<uint32_t>(room / resolution);
            const unsigned nb = bit_width(range);
            const uint32_t scaled = nb ? br.bits(nb) : 0;
            const uint32_t seglen = resolution * scaled;
            s.start[c][n] = s.start[c][n - 1] + seglen;
            if (s.start[c][n] > flen_)
                throw std::runtime_error("DST: segment past end of frame");
            ++n;
            end = br.bit();
        }
        s.n_seg[c] = n;
        s.start[c][n] = flen_;          // sentinel: end of the last segment
    }

    // ── Mapping (Tables 10.8 – 10.10) ──────────────────────────────────────
    void read_mapping(BitReader& br) {
        const unsigned same = br.bit();
        if (same == 0) {
            maps(br, fseg_);
            maps(br, pseg_);
        } else {
            maps(br, fseg_);
            // One element set indexes both filters and tables. The segment
            // boundaries may still differ if Same_Segmentation was 0, so only
            // the element numbers are copied across, per segment.
            pseg_.n_elements = fseg_.n_elements;
            for (unsigned c = 0; c < nch_; ++c)
                for (unsigned g = 0; g < pseg_.n_seg[c]; ++g)
                    pseg_.elem[c][g] = fseg_.elem[c][g];
        }
    }

    void maps(BitReader& br, Segments& s) {
        s.n_elements = 0;
        const unsigned same_all = br.bit();
        const unsigned n_read = same_all ? 1u : nch_;
        for (unsigned c = 0; c < n_read; ++c) {
            for (unsigned g = 0; g < s.n_seg[c]; ++g) {
                // Element[1][1] is always 0 and costs no bits, which
                // bit_width(0) == 0 gives for free.
                const unsigned nb = bit_width(s.n_elements);
                const unsigned e = nb ? br.bits(nb) : 0u;
                if (e > s.n_elements || e >= MAX_ELEMENTS)
                    throw std::runtime_error("DST: element index out of range");
                s.elem[c][g] = e;
                if (e == s.n_elements) ++s.n_elements;
            }
        }
        if (same_all)
            for (unsigned c = 1; c < nch_; ++c)
                for (unsigned g = 0; g < s.n_seg[c]; ++g)
                    s.elem[c][g] = s.elem[0][g];
        if (s.n_elements == 0)
            throw std::runtime_error("DST: no elements mapped");
    }

    // ── Filter coefficient sets (Table 10.13) ──────────────────────────────
    void read_filters(BitReader& br) {
        // Table 10.19 / 10.20, with CCPC pre-multiplied by 8 so the whole
        // prediction stays in integers (see the header note).
        static const int  CCPO[4]      = { 1, 2, 3, 0 };
        static const int  CCPC8[4][3]  = { {-8,0,0}, {-16,8,0}, {-9,-5,6}, {0,0,0} };

        filters_.assign(fseg_.n_elements, Filter{});
        for (auto& f : filters_) {
            f.order = br.bits(7) + 1;
            const unsigned coded = br.bit();
            if (coded == 0) {
                for (unsigned i = 0; i < f.order; ++i) f.coef[i] = br.sbits(9);
            } else {
                const unsigned method = br.bits(2);
                if (method == 3) throw std::runtime_error("DST: CC_Method '11' is not used");
                const unsigned ccpo = static_cast<unsigned>(CCPO[method]);
                if (ccpo >= f.order)
                    throw std::runtime_error("DST: CCPO >= Pred_Order");
                for (unsigned i = 0; i < ccpo; ++i) f.coef[i] = br.sbits(9);
                const unsigned ccm = br.bits(3);
                for (unsigned i = ccpo; i < f.order; ++i) {
                    int32_t delta = rice(br, ccm);
                    f.coef[i] = delta;
                    // Undo the linear prediction of the coefficients.
                    int64_t d8 = 0;
                    for (unsigned t = 0; t < ccpo; ++t)
                        d8 += static_cast<int64_t>(CCPC8[method][t]) * f.coef[i - t - 1];
                    f.coef[i] -= (d8 >= 0) ? static_cast<int32_t>((d8 + 4) / 8)
                                           : -static_cast<int32_t>((-d8 + 3) / 8);
                }
            }
            f.build_table();
        }
    }

    // ── Probability tables (Table 10.14) ───────────────────────────────────
    void read_ptables(BitReader& br) {
        static const int PCPO[4]    = { 1, 2, 3, 0 };
        static const int PCPC[4][3] = { {-1,0,0}, {-2,1,0}, {-3,3,-1}, {0,0,0} };

        ptables_.assign(pseg_.n_elements, Ptable{});
        for (auto& t : ptables_) {
            t.len = br.bits(6) + 1;
            if (t.len == 1) { t.p_one[0] = 128; continue; }
            const unsigned coded = br.bit();
            if (coded == 0) {
                for (unsigned i = 0; i < t.len; ++i) t.p_one[i] = br.bits(7) + 1;
            } else {
                const unsigned method = br.bits(2);
                if (method == 3) throw std::runtime_error("DST: PC_Method '11' is not used");
                const unsigned pcpo = static_cast<unsigned>(PCPO[method]);
                if (pcpo >= t.len)
                    throw std::runtime_error("DST: PCPO >= Ptable_Len");
                for (unsigned i = 0; i < pcpo; ++i) t.p_one[i] = br.bits(7) + 1;
                const unsigned pcm = br.bits(3);
                for (unsigned i = pcpo; i < t.len; ++i) {
                    int32_t delta = rice(br, pcm);
                    t.p_one[i] = delta;
                    // Note this prediction is NOT scaled by 8 -- unlike the
                    // filter coefficients, PCPC is integral (Table 10.22).
                    for (unsigned k = 0; k < pcpo; ++k)
                        t.p_one[i] -= PCPC[method][k] * t.p_one[i - k - 1];
                }
            }
            for (unsigned i = 0; i < t.len; ++i)
                if (t.p_one[i] < 1 || t.p_one[i] > 128)
                    throw std::runtime_error("DST: Ptable entry out of range 1..128");
        }
    }

    // Rice code as Tables 10.13/10.14 spell it out: unary run of zeros
    // terminated by a 1, then `m` raw LSBs, then a sign bit if non-zero.
    static int32_t rice(BitReader& br, unsigned m) {
        uint32_t run = 0;
        while (br.bit() == 0) {
            if (++run > 1u << 20) throw std::runtime_error("DST: runaway Rice code");
        }
        const uint32_t lsbs = m ? br.bits(m) : 0;
        int32_t delta = static_cast<int32_t>((run << m) + lsbs);
        if (delta != 0 && br.bit()) delta = -delta;
        return delta;
    }

    // Flatten segment boundaries to a per-byte element index so the inner
    // loop does a lookup rather than a search (10.7.2.3's Filter_N / Ptable_N).
    void build_segment_maps() {
        for (unsigned c = 0; c < nch_; ++c) {
            for (unsigned g = 0; g < fseg_.n_seg[c]; ++g)
                for (uint32_t b = fseg_.start[c][g]; b < fseg_.start[c][g + 1] && b < flen_; ++b)
                    fseg_of_byte_[c * flen_ + b] = static_cast<uint8_t>(fseg_.elem[c][g]);
            for (unsigned g = 0; g < pseg_.n_seg[c]; ++g)
                for (uint32_t b = pseg_.start[c][g]; b < pseg_.start[c][g + 1] && b < flen_; ++b)
                    pseg_of_byte_[c * flen_ + b] = static_cast<uint8_t>(pseg_.elem[c][g]);
        }
    }

    // ── Arithmetic decoding (Figures 10.8 – 10.12) ─────────────────────────
    void arithmetic_decode(BitReader& br) {
        // Figure 10.9. 13 bits into a 12-bit register: the first is always 0
        // and is intentionally shifted out.
        uint32_t A = 0xFFF, C = 0;
        for (unsigned i = 0; i <= 12; ++i) C = ((C << 1) | br.bit()) & 0xFFF;

        // The very first coded bit is DST_X_Bit, whose probability is derived
        // from filter 0's first coefficient by reversing its low 7 bits
        // (10.7.2.4). Its value is then discarded -- 10.6.2.1.2.2 says a
        // decoder shall ignore it -- but decoding it is what aligns every
        // subsequent bit, so it cannot be skipped.
        {
            const uint32_t c0 = static_cast<uint32_t>(filters_[0].coef[0]) & 0x1FF;
            uint32_t p = 0;
            for (unsigned i = 0; i < 7; ++i)
                if ((c0 >> i) & 1) p |= 1u << (6 - i);
            decode_bit(br, A, C, static_cast<int32_t>(p) + 1);
        }

        // Per-channel running state. hist holds the channel's own past DSD
        // bits, most recent in bit 0, which is exactly the order the filter's
        // tap-group tables are built for.
        uint64_t hist_lo[MAX_CHANNELS] = {0}, hist_hi[MAX_CHANNELS] = {0};
        // Y[m] = (-1)^m for -N <= m < 0, i.e. the bit at distance k from the
        // most recent is +1 when k is odd (m = -k, (-1)^-k = (-1)^k ... m=-1
        // gives -1, m=-2 gives +1). In X terms (1 -> +1, 0 -> -1) the initial
        // history bit at distance k is 0 for k even, 1 for k odd.
        for (unsigned c = 0; c < nch_; ++c) {
            uint64_t lo = 0, hi = 0;
            for (unsigned k = 0; k < 64; ++k) if (k & 1)         lo |= (1ull << k);
            for (unsigned k = 0; k < 64; ++k) if ((k + 64) & 1)   hi |= (1ull << k);
            hist_lo[c] = lo; hist_hi[c] = hi;
        }

        const unsigned total_bits = flen_ * 8;
        for (unsigned n = 0; n < total_bits; ++n) {
            const unsigned byte = n >> 3;
            const unsigned shift = 7 - (n & 7);
            for (unsigned c = 0; c < nch_; ++c) {
                const Filter& f = filters_[fseg_of_byte_[c * flen_ + byte]];
                const Ptable& t = ptables_[pseg_of_byte_[c * flen_ + byte]];

                // Z[n] = sum Y[n-1-i] * H[i], via the tap-group tables.
                int32_t Z = 0;
                const uint64_t lo = hist_lo[c], hi = hist_hi[c];
                for (unsigned g = 0; g < f.n_groups; ++g) {
                    const unsigned b = (g < 8) ? static_cast<unsigned>((lo >> (g * 8)) & 0xFF)
                                               : static_cast<unsigned>((hi >> ((g - 8) * 8)) & 0xFF);
                    Z += f.table[g][b];
                }

                const unsigned F = (Z >= 0) ? 1u : 0u;
                const int32_t  az = (Z >= 0) ? Z : -Z;
                int32_t P;
                if (half_prob_[c] && n < f.order) {
                    P = 128;
                } else {
                    unsigned idx = static_cast<unsigned>(az >> 3);
                    if (idx > t.len - 1) idx = t.len - 1;
                    P = t.p_one[idx];
                }

                const unsigned E = decode_bit(br, A, C, P);
                n_errors_ += E;
                const unsigned X = E ^ F;

                out_[c][byte] |= static_cast<uint8_t>(X << shift);
                hist_hi[c] = (hi << 1) | (lo >> 63);
                hist_lo[c] = (lo << 1) | X;
            }
        }
    }

    // Figures 10.10 and 10.11, together: decode one bit, then renormalise.
    static unsigned decode_bit(BitReader& br, uint32_t& A, uint32_t& C, int32_t P) {
        const uint32_t K = (A >> 8) | ((A >> 7) & 1);
        const uint32_t Q = K * static_cast<uint32_t>(P);
        unsigned E;
        if (C >= A - Q) { E = 1; C -= (A - Q); A = Q; }
        else           { E = 0; A -= Q; }
        while (A < 0x800) { A <<= 1; C = ((C << 1) | br.bit()) & 0xFFF; }
        return E;
    }
};

// Convenience: decode one frame into caller-owned per-channel buffers.
inline void decode_frame(const uint8_t* data, size_t size, unsigned n_channels,
                         unsigned frame_bytes,
                         std::vector<std::vector<uint8_t>>& out) {
    Decoder d(n_channels, frame_bytes);
    d.decode_frame(data, size);
    out.resize(n_channels);
    for (unsigned c = 0; c < n_channels; ++c) out[c] = d.channel(c);
}

} // namespace dst
