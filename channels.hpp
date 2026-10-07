// channels.hpp — channel layout: names, LFE identification, BS.1770 weights.
//
// Single source of truth shared by analysis.hpp (weighting / LFE exclusion),
// main.cpp (CLI labels, JSON) and gui_main.cpp (channel matrix).  Zero
// dependencies, header-only, part of the default build.
//
// Channel ORDER is assumed to be the canonical WAVE / ITU interleave order that
// every decoder in audio.hpp hands us unchanged:
//
//     1 ch  M
//     2 ch  L  R
//     3 ch  L  R  C
//     4 ch  L  R  Ls  Rs                      (quad)
//     5 ch  L  R  C   Ls   Rs                 (5.0)
//     6 ch  L  R  C   LFE  Ls   Rs            (5.1)
//     8 ch  L  R  C   LFE  Lrs  Rrs  Lss Rss  (7.1, rears then sides)
//
// This is what FLAC mandates, what WAVE writes for the standard channel masks,
// and what FFmpeg hands back for 5.1 / 5.1(side) / 7.1. For 7.1 that order is
// FL FR FC LFE BL BR SL SR: the BACK pair precedes the SIDE pair, which is how
// foobar2000 labels it too. Up to 0.19.5 crete printed the two pairs the other
// way round (labels only -- both pairs carry the same BS.1770 weight, so no
// measured value depended on it).  Layouts outside this
// list are measured per channel but carry no speaker semantics: no LFE is
// assumed and every channel is scored at unity weight.
//
// CHANNEL MASK (0.21.0).  The count alone cannot tell an Auro-3D 8.0 file
// (L R Ls Rs HL HR HLs HRs) from a 7.1 one, and up to 0.20.0 crete called it
// 7.1, took its Rs for an LFE and left it out of DR and loudness.  Every
// function below therefore takes an optional speaker mask -- WAVEFORMATEX-
// TENSIBLE dwChannelMask, FLAC's WAVEFORMATEXTENSIBLE_CHANNEL_MASK tag, or
// FFmpeg's native ch_layout -- and derives labels, LFE and weights from the
// speaker bits when, and only when, the mask is usable AND describes a layout
// the count table does not.  A 5.1 file whose mask says FL FR FC LFE SL SR
// (or BL BR) is the count table's 5.1 and takes the count path below,
// unchanged; so does every mask on mono and stereo.  mask == 0 means "none
// declared" and is the count path by definition.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace chan {

// ── Speaker mask ────────────────────────────────────────────────────────────
// WAVEFORMATEXTENSIBLE speaker bits (ksmedia.h). FFmpeg's AV_CH_* use the same
// numbering for these 18; its bits above them (wide, surround-direct, LFE2,
// top-side, bottom) have no WAVE equivalent and make a mask unusable here.
enum : uint32_t {
    SPK_FL  = 0x1,     SPK_FR  = 0x2,     SPK_FC  = 0x4,     SPK_LFE = 0x8,
    SPK_BL  = 0x10,    SPK_BR  = 0x20,    SPK_FLC = 0x40,    SPK_FRC = 0x80,
    SPK_BC  = 0x100,   SPK_SL  = 0x200,   SPK_SR  = 0x400,   SPK_TC  = 0x800,
    SPK_TFL = 0x1000,  SPK_TFC = 0x2000,  SPK_TFR = 0x4000,
    SPK_TBL = 0x8000,  SPK_TBC = 0x10000, SPK_TBR = 0x20000,
    SPK_KNOWN = 0x3FFFF,
    SPK_TOP   = SPK_TC | SPK_TFL | SPK_TFC | SPK_TFR | SPK_TBL | SPK_TBC | SPK_TBR,
};

inline unsigned mask_popcount(uint32_t m) {
    unsigned n = 0;
    for (; m; m &= m - 1) ++n;
    return n;
}

// A mask is usable when it names exactly one speaker per channel and every
// speaker is one this table knows. WAVE allows a mask with more or fewer bits
// than channels (the surplus channels are "unassigned"); crete does not guess
// at that, and measures such a file by count exactly as before.
inline bool mask_usable(unsigned nch, uint32_t mask) {
    return nch >= 3 && mask != 0 && (mask & ~uint32_t(SPK_KNOWN)) == 0 &&
           mask_popcount(mask) == nch;
}

// ── Count table: no mask, or a mask that agrees with it ────────────────────
// This is the whole of channels.hpp up to 0.20.0, unchanged. Every file
// without a usable, disagreeing mask is described by it alone.
namespace count {

// LFE only exists in the two layouts that define one, always at index 3.
inline bool has_lfe(unsigned nch)            { return nch == 6 || nch == 8; }
inline bool is_lfe(unsigned nch, size_t i)   { return has_lfe(nch) && i == 3; }

inline std::string label(unsigned nch, size_t i) {
    switch (nch) {
        case 1: return "M";
        case 2: return (i == 0) ? "L" : "R";
        case 3: { static const char* k[] = {"L","R","C"};                return k[i]; }
        case 4: { static const char* k[] = {"L","R","Ls","Rs"};          return k[i]; }
        case 5: { static const char* k[] = {"L","R","C","Ls","Rs"};      return k[i]; }
        case 6: { static const char* k[] = {"L","R","C","LFE","Ls","Rs"}; return k[i]; }
        case 8: { static const char* k[] = {"L","R","C","LFE","Lrs","Rrs","Lss","Rss"};
                  return k[i]; }
        default: return "Ch" + std::to_string(i + 1);
    }
}

inline std::string layout_short(unsigned nch) {
    switch (nch) {
        case 1: return "MONO";  case 2: return "STEREO"; case 3: return "3.0";
        case 4: return "QUAD";  case 5: return "5.0";    case 6: return "5.1";
        case 8: return "7.1";
        default: return std::to_string(nch) + " CH";
    }
}

inline std::string layout_detail(unsigned nch) {
    switch (nch) {
        case 1: return "1.0 · M";
        case 2: return "2.0 · L R";
        case 3: return "3.0 · L R C";
        case 4: return "QUAD · L R Ls Rs";
        case 5: return "5.0 · L R C Ls Rs";
        case 6: return "5.1 · L R C LFE Ls Rs";
        case 8: return "7.1 · L R C LFE Lrs Rrs Lss Rss";
        default: return layout_short(nch);
    }
}

// ITU-R BS.1770-4 channel weighting for the summed loudness measurement:
// front channels (L, R, C) unity, surrounds +1.5 dB, LFE excluded outright.
//
//   G = 1.0   -> L, R, C
//   G = 1.41  -> Ls, Rs  (and, by extension, the 7.1 rears)
//   G = 0.0   -> LFE
//
// BS.1770-4 tabulates weights only up to 5.1; the 7.1 rears take the same
// +1.5 dB as the sides, which is what EBU and every broadcast meter do.  Mono
// and stereo are all-unity, so the weighted sum is bit-identical to the
// unweighted one it replaced (multiplication by exactly 1.0 is exact in IEEE
// 754) and no stereo measurement moves.  Unknown layouts are all-unity too.
inline double bs1770_weight(unsigned nch, size_t i) {
    if (is_lfe(nch, i)) return 0.0;
    const double SURROUND = 1.41;  // +1.5 dB
    switch (nch) {
        case 4: return (i >= 2) ? SURROUND : 1.0;              // L R | Ls Rs
        case 5: return (i >= 3) ? SURROUND : 1.0;              // L R C | Ls Rs
        case 6: return (i >= 4) ? SURROUND : 1.0;              // L R C LFE | Ls Rs
        case 8: return (i >= 4) ? SURROUND : 1.0;              // L R C LFE | 4 surrounds
        default: return 1.0;
    }
}

// True when this layout needs any weighting at all. Mono/stereo do not, which
// is the guarantee that the multichannel work cannot perturb the stereo corpus.
inline bool needs_weighting(unsigned nch) {
    return nch > 2 && (nch == 4 || nch == 5 || nch == 6 || nch == 8);
}

}  // namespace count

// ── Mask table: one entry per speaker bit ──────────────────────────────────
// Channels appear in ascending bit order, which is what WAVE, FLAC and FFmpeg
// all mandate for a masked layout, so channel i is the mask's i-th set bit.
//
// Labels follow the count table's: a back pair alone or a side pair alone is
// Ls Rs, and only a layout with both splits them into rears (Lrs Rrs) and
// sides (Lss Rss) -- so 5.1 reads L R C LFE Ls Rs whichever pair its mask
// names. Height channels take an H prefix (HL HR HLs HRs, as Auro-3D names
// them); top centre is T.
//
// Weights extend the count table's BS.1770-4 reading, not replace it:
//   LFE                                   0.0
//   ear-level surrounds BL BR BC SL SR    1.41  (the count table already gives
//                                                the 7.1 rears 1.41; the back
//                                                centre goes with them)
//   L R C, Lc Rc, and every height        1.0   (BS.1770-4 Table 4: +1.5 dB
//                                                only below 30 deg elevation)
struct Speaker {
    const char* label;
    double      weight;
    bool        lfe;
};

inline Speaker speaker(uint32_t bit, uint32_t mask) {
    const double SURROUND = 1.41;  // +1.5 dB
    const bool split = (mask & (SPK_BL | SPK_BR)) && (mask & (SPK_SL | SPK_SR));
    switch (bit) {
        case SPK_FL:  return {"L",   1.0, false};
        case SPK_FR:  return {"R",   1.0, false};
        case SPK_FC:  return {"C",   1.0, false};
        case SPK_LFE: return {"LFE", 0.0, true};
        case SPK_BL:  return {split ? "Lrs" : "Ls", SURROUND, false};
        case SPK_BR:  return {split ? "Rrs" : "Rs", SURROUND, false};
        case SPK_FLC: return {"Lc",  1.0, false};
        case SPK_FRC: return {"Rc",  1.0, false};
        case SPK_BC:  return {"Cs",  SURROUND, false};
        case SPK_SL:  return {split ? "Lss" : "Ls", SURROUND, false};
        case SPK_SR:  return {split ? "Rss" : "Rs", SURROUND, false};
        case SPK_TC:  return {"T",   1.0, false};
        case SPK_TFL: return {"HL",  1.0, false};
        case SPK_TFC: return {"HC",  1.0, false};
        case SPK_TFR: return {"HR",  1.0, false};
        case SPK_TBL: return {"HLs", 1.0, false};
        case SPK_TBC: return {"HCs", 1.0, false};
        case SPK_TBR: return {"HRs", 1.0, false};
        default:      return {"?",   1.0, false};   // unreachable: mask_usable()
    }
}

// The speaker bit carried by channel i of a usable mask.
inline uint32_t nth_speaker(uint32_t mask, size_t i) {
    for (; mask; mask &= mask - 1)
        if (i-- == 0) return mask & (~mask + 1);
    return 0;
}

inline Speaker mask_speaker(uint32_t mask, size_t i) {
    return speaker(nth_speaker(mask, i), mask);
}

// True when the mask describes exactly what the count table already says:
// same label, same weight, same LFE on every channel.
inline bool mask_agrees_with_count(unsigned nch, uint32_t mask) {
    for (size_t i = 0; i < nch; ++i) {
        const Speaker s = mask_speaker(mask, i);
        if (s.label != count::label(nch, i) || s.weight != count::bs1770_weight(nch, i) ||
            s.lfe != count::is_lfe(nch, i))
            return false;
    }
    return true;
}

// The one decision every function below makes: describe this file by its
// speaker mask, or by its channel count as crete always has.
inline bool use_mask(unsigned nch, uint32_t mask) {
    return mask_usable(nch, mask) && !mask_agrees_with_count(nch, mask);
}

// ── Public interface ───────────────────────────────────────────────────────
inline bool is_lfe(unsigned nch, size_t i, uint32_t mask = 0) {
    return use_mask(nch, mask) ? mask_speaker(mask, i).lfe : count::is_lfe(nch, i);
}

inline bool has_lfe(unsigned nch, uint32_t mask = 0) {
    return use_mask(nch, mask) ? (mask & SPK_LFE) != 0 : count::has_lfe(nch);
}

// Channels that count toward a score. By default everything but the LFE; with
// include_lfe the whole layout counts (see dr::LfeScoring in analysis.hpp --
// this is a TT DR aggregation choice, never a BS.1770 one).
inline unsigned scored_count(unsigned nch, bool include_lfe = false, uint32_t mask = 0) {
    return nch - ((has_lfe(nch, mask) && !include_lfe) ? 1u : 0u);
}

// True when the layout has a discrete front L/R pair whose DR is directly
// comparable with the stereo release of the same master.
inline bool has_front_pair(unsigned nch)     { return nch >= 2; }

inline std::string label(unsigned nch, size_t i, uint32_t mask = 0) {
    return use_mask(nch, mask) ? std::string(mask_speaker(mask, i).label)
                               : count::label(nch, i);
}

// Long-form label for the CLI's detail table, which has room for words.
inline std::string label_long(unsigned nch, size_t i, uint32_t mask = 0) {
    if (nch == 1) return "Mono";
    if (nch == 2) return (i == 0) ? "Left" : "Right";
    return label(nch, i, mask);
}

// A masked layout is named floor.lfe, or floor.lfe.height when it has height
// channels: Auro-3D 9.1 is 5.1.4, its 8.0 is 4.0.4.
inline std::string layout_short(unsigned nch, uint32_t mask = 0) {
    if (!use_mask(nch, mask)) return count::layout_short(nch);
    const unsigned lfe = (mask & SPK_LFE) ? 1u : 0u;
    const unsigned top = mask_popcount(mask & SPK_TOP);
    std::string s = std::to_string(nch - lfe - top) + "." + std::to_string(lfe);
    if (top) s += "." + std::to_string(top);
    return s;
}

// The Auro-3D name of a discrete layout, or nullptr. The Auro whitepaper
// ("AURO-3D in multi-channel WAV-files", NEWAURO, Rev 2, 2024-04-19) gives the
// exact mask of each Auro layout, so this is a lookup, not an inference --
// but a mask names SPEAKERS, not a format. Auro 9.1 (0x2D60F) is speaker for
// speaker 5.1.4, and Auro 11.1 in its 7.1 + 4H form (0x2D63F) is 7.1.4; an
// Atmos bed rendered to either would be called Auro-3D. So only the masks no
// other system uses are named: those with Top (T) or a front height centre
// (HC), and 8.0, the 4.0.4 that has no other name.
inline const char* auro_layout(unsigned nch, uint32_t mask = 0) {
    if (!use_mask(nch, mask)) return nullptr;
    switch (mask) {
        case 0x2D603: return "Auro-3D 8.0";    // L R Ls Rs HL HR HLs HRs
        case 0x2DE0F: return "Auro-3D 10.1";   // 5.1 + T + 4H
        case 0x2FE0F: return "Auro-3D 11.1";   // 5.1 + T + 5H
        case 0x2F63F: return "Auro-3D 12.1";   // 7.1 + 5H
        case 0x2FE3F: return "Auro-3D 13.1";   // 7.1 + T + 5H
        default:      return nullptr;
    }
}

inline std::string layout_detail(unsigned nch, uint32_t mask = 0) {
    if (!use_mask(nch, mask)) return count::layout_detail(nch);
    std::string s = layout_short(nch, mask) + " ·";
    for (size_t i = 0; i < nch; ++i) s += std::string(" ") + mask_speaker(mask, i).label;
    if (const char* auro = auro_layout(nch, mask)) s += std::string(" · ") + auro + " layout";
    return s;
}

inline double bs1770_weight(unsigned nch, size_t i, uint32_t mask = 0) {
    return use_mask(nch, mask) ? mask_speaker(mask, i).weight : count::bs1770_weight(nch, i);
}

inline bool needs_weighting(unsigned nch, uint32_t mask = 0) {
    if (!use_mask(nch, mask)) return count::needs_weighting(nch);
    for (size_t i = 0; i < nch; ++i)
        if (mask_speaker(mask, i).weight != 1.0) return true;
    return false;
}

// The mask as reported in JSON: the declared mask when it is what described
// the layout, otherwise 0 (nothing to report -- the count table did it).
inline uint32_t effective_mask(unsigned nch, uint32_t mask) {
    return use_mask(nch, mask) ? mask : 0u;
}

// "0x2D603", for JSON and logs -- the spelling ksmedia.h and FLAC's tag use.
inline std::string mask_string(uint32_t mask) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%X", static_cast<unsigned>(mask));
    return buf;
}

// One-line description of how loudness was summed, for UI and logs.
inline std::string loudness_mode(unsigned nch, uint32_t mask = 0) {
    if (nch <= 2) return "BS.1770 · " + std::to_string(nch) + " CH";
    if (!needs_weighting(nch, mask)) return "BS.1770 · " + std::to_string(nch) + " CH UNWEIGHTED";
    return "BS.1770 · " + layout_short(nch, mask) + " WEIGHTED";
}

}  // namespace chan
