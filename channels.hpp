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
#pragma once

#include <cstddef>
#include <string>

namespace chan {

// LFE only exists in the two layouts that define one, always at index 3.
inline bool has_lfe(unsigned nch)            { return nch == 6 || nch == 8; }
inline bool is_lfe(unsigned nch, size_t i)   { return has_lfe(nch) && i == 3; }

// Channels that count toward a score. By default everything but the LFE; with
// include_lfe the whole layout counts (see dr::LfeScoring in analysis.hpp --
// this is a TT DR aggregation choice, never a BS.1770 one).
inline unsigned scored_count(unsigned nch, bool include_lfe = false) {
    return nch - ((has_lfe(nch) && !include_lfe) ? 1u : 0u);
}

// True when the layout has a discrete front L/R pair whose DR is directly
// comparable with the stereo release of the same master.
inline bool has_front_pair(unsigned nch)     { return nch >= 2; }

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

// Long-form label for the CLI's detail table, which has room for words.
inline std::string label_long(unsigned nch, size_t i) {
    if (nch == 1) return "Mono";
    if (nch == 2) return (i == 0) ? "Left" : "Right";
    return label(nch, i);
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

// One-line description of how loudness was summed, for UI and logs.
inline std::string loudness_mode(unsigned nch) {
    if (nch <= 2) return "BS.1770 · " + std::to_string(nch) + " CH";
    if (!needs_weighting(nch)) return "BS.1770 · " + std::to_string(nch) + " CH UNWEIGHTED";
    return "BS.1770 · " + layout_short(nch) + " WEIGHTED";
}

}  // namespace chan
