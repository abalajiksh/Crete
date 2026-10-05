// ============================================================================
// dsd2wav.cpp — DSD → PCM exporter (crête reference-generation tool)
//
// Decodes .dsf/.dff through crête's own decimation chain and writes 352.8 kHz
// WAV, so the SAME PCM that crête measures can be fed to MAAT DROffline. This
// splits DSD validation into two independently-checkable stages:
//
//   Stage 1  decimation   crête vs foo_input_sacd  — bit-exact (dsd_xcheck)
//   Stage 2  metrics      crête vs MAAT on the exported WAV — same oracle,
//                         same tolerances as the PCM qualification suite
//
// Pass both and DSD is validated end-to-end with foobar out of the loop.
//
// Build:
//   g++ -std=c++17 -O2 -pthread -o dsd2wav dsd2wav.cpp
//
// Usage:
//   dsd2wav [options] <file_or_folder...>
//     --dsd-mode <direct|multistage>   decimation chain (default: multistage)
//     --dsd-filter / --dsd-out-rate / --dsd-bandwidth   as crete's flags
//     -o, --outdir <dir>               output directory (default: ./pcm_out)
//     --bits <f32|i24>                 sample format (default: f32)
//     -q, --quiet
//
// ── Why f32 is the default, and why it matters ──────────────────────────────
// DSD decimation routinely produces samples beyond +/-1.0 — a 997 Hz tone at
// -6 dBFS measured +1.38 dBFS peak through the Direct chain, because DSD's
// shaped noise floor rides on top of the signal. Writing i24 CLIPS those
// samples, which silently destroys exactly the peak/SPPM/true-peak values you
// are trying to validate. Use f32 unless MAAT refuses to open it; if you must
// fall back to i24, this tool reports every clipped sample so you know the
// reference is compromised.
// ============================================================================

#include "audio.hpp"
#include "dsd_decode.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

enum class Bits { F32, I24 };

// ── little-endian writers ──────────────────────────────────────────────────
static void w16(std::ostream& o, uint16_t v) {
    uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
    o.write(reinterpret_cast<char*>(b), 2);
}
static void w32(std::ostream& o, uint32_t v) {
    uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    o.write(reinterpret_cast<char*>(b), 4);
}

// Returns number of samples clipped (i24 only; always 0 for f32).
static size_t write_wav(const fs::path& out, const audio::AudioData& ad, Bits bits) {
    const uint16_t nch  = uint16_t(ad.num_channels());
    const uint64_t nfr  = ad.num_samples();
    const uint32_t rate = ad.sample_rate;

    const uint16_t fmt_tag   = (bits == Bits::F32) ? 3 : 1;   // 3 = IEEE float
    const uint16_t bps       = (bits == Bits::F32) ? 32 : 24;
    const uint16_t blockalgn = uint16_t(nch * bps / 8);
    const uint64_t datasize  = nfr * blockalgn;

    if (datasize + 44 > 0xFFFFFFFFull)
        throw std::runtime_error("WAV: >4 GB, RF64 not supported: " + out.string());

    std::ofstream o(out, std::ios::binary);
    if (!o) throw std::runtime_error("Cannot write: " + out.string());

    o.write("RIFF", 4);
    w32(o, uint32_t(36 + datasize));
    o.write("WAVE", 4);
    o.write("fmt ", 4);
    w32(o, 16);
    w16(o, fmt_tag);
    w16(o, nch);
    w32(o, rate);
    w32(o, rate * blockalgn);
    w16(o, blockalgn);
    w16(o, bps);
    o.write("data", 4);
    w32(o, uint32_t(datasize));

    size_t clipped = 0;
    std::vector<uint8_t> row(blockalgn);

    for (uint64_t i = 0; i < nfr; ++i) {
        uint8_t* p = row.data();
        for (uint16_t c = 0; c < nch; ++c) {
            const double v = ad.channels[c][i];
            if (bits == Bits::F32) {
                const float f = float(v);
                std::memcpy(p, &f, 4);
                p += 4;
            } else {
                double s = v;
                if (s >= 1.0 || s < -1.0) { ++clipped; s = s >= 1.0 ? 0.99999988 : -1.0; }
                int32_t q = int32_t(s * 8388608.0);
                if (q >  8388607) q =  8388607;
                if (q < -8388608) q = -8388608;
                *p++ = uint8_t(q);
                *p++ = uint8_t(q >> 8);
                *p++ = uint8_t(q >> 16);
            }
        }
        o.write(reinterpret_cast<char*>(row.data()), blockalgn);
    }
    if (datasize & 1) o.put(0);   // RIFF word alignment
    return clipped;
}

static bool is_dsd(const fs::path& p) {
    auto e = p.extension().string();
    for (auto& c : e) c = char(std::tolower(c));
    return e == ".dsf" || e == ".dff";
}

int main(int argc, char** argv) {
    dsd::Options opt;
    Bits bits = Bits::F32;
    fs::path outdir = "pcm_out";
    bool quiet = false;
    std::vector<std::string> inputs;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--dsd-mode" && i + 1 < argc) {
            try { opt.mode = dsd::mode_from_string(argv[++i]); }
            catch (const std::exception& e) { std::cerr << "Error: " << e.what() << "\n"; return 1; }
        } else if (a == "--dsd-filter" && i + 1 < argc) {
            try { opt.filter = dsd::filter_from_string(argv[++i]); }
            catch (const std::exception& e) { std::cerr << "Error: " << e.what() << "\n"; return 1; }
        } else if (a == "--dsd-out-rate" && i + 1 < argc) {
            int r = std::atoi(argv[++i]);
            if (!dsd::is_valid_out_rate(r)) {
                std::cerr << "Error: --dsd-out-rate must be 44100, 88200, 176400, or 352800\n";
                return 1;
            }
            opt.out_rate = r;
        } else if (a == "--dsd-bandwidth" && i + 1 < argc) {
            try { opt.bandwidth = dsd::bandwidth_from_string(argv[++i]); }
            catch (const std::exception& e) { std::cerr << "Error: " << e.what() << "\n"; return 1; }
        } else if ((a == "-o" || a == "--outdir") && i + 1 < argc) {
            outdir = argv[++i];
        } else if (a == "--bits" && i + 1 < argc) {
            std::string b = argv[++i];
            if      (b == "f32") bits = Bits::F32;
            else if (b == "i24") bits = Bits::I24;
            else { std::cerr << "Error: --bits must be f32 or i24\n"; return 1; }
        } else if (a == "-q" || a == "--quiet") {
            quiet = true;
        } else if (a == "-h" || a == "--help") {
            std::cerr <<
              "dsd2wav — DSD -> WAV, via crete's decimation chain (libdsddpcm)\n\n"
              "Usage: dsd2wav [options] <file_or_folder...>\n"
              "  --dsd-mode <direct|multistage>              chain (default: multistage)\n"
              "  --dsd-filter <default|firls|kaiser>         final-stage filter (default: default)\n"
              "  --dsd-out-rate <44100|88200|176400|352800>  output rate (default: 44100)\n"
              "  --dsd-bandwidth <default|full|Hz>           band limit (default: 50 kHz;\n"
              "                                              acts at 176400/352800 only)\n"
              "  -o, --outdir <dir>                          output dir (default: pcm_out)\n"
              "  --bits <f32|i24>                            format (default: f32)\n"
              "  -q, --quiet\n\n"
              "f32 is the default because DSD decimation exceeds +/-1.0; i24 clips it.\n"
              "Match --dsd-* to crete's flags so the exported WAV is the SAME PCM it measures.\n";
            return 0;
        } else {
            inputs.push_back(a);
        }
    }

    if (inputs.empty()) { std::cerr << "Error: no input\n"; return 1; }

    std::vector<fs::path> files;
    for (const auto& in : inputs) {
        fs::path p(in);
        if (fs::is_directory(p)) {
            for (const auto& e : fs::directory_iterator(p))
                if (e.is_regular_file() && is_dsd(e.path())) files.push_back(e.path());
        } else if (is_dsd(p)) {
            files.push_back(p);
        } else {
            std::cerr << "Skipping non-DSD: " << in << "\n";
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) { std::cerr << "Error: no .dsf/.dff found\n"; return 1; }

    fs::create_directories(outdir);

    if (!quiet)
        std::cerr << "dsd2wav  mode=" << dsd::mode_name(opt.mode)
                  << "  filter=" << dsd::filter_name(opt.filter)
                  << "  out=" << opt.out_rate << "Hz"
                  << "  bw=" << dsd::bandwidth_name(opt.bandwidth)
                  << "  bits=" << (bits == Bits::F32 ? "f32" : "i24")
                  << "  files=" << files.size() << "  ->  " << outdir << "\n";

    size_t ok = 0, failed = 0, total_clipped = 0;

    for (const auto& f : files) {
        fs::path out = outdir / (f.stem().string() + ".wav");
        try {
            auto ad = audio::decode_file(f.string(), opt);

            // Report true peak headroom so a clipped i24 export is never silent.
            double peak = 0.0;
            for (const auto& ch : ad.channels)
                for (double v : ch) peak = std::max(peak, std::fabs(v));

            size_t clipped = write_wav(out, ad, bits);
            total_clipped += clipped;

            if (!quiet) {
                std::fprintf(stderr, "  %-52s %6u Hz  %zu ch  %8zu smp  peak %+6.2f dBFS%s\n",
                    f.filename().string().c_str(), ad.sample_rate,
                    ad.num_channels(), ad.num_samples(),
                    20.0 * std::log10(peak > 0 ? peak : 1e-12),
                    clipped ? "  *** CLIPPED ***" : "");
            }
            ++ok;
        } catch (const std::exception& e) {
            std::cerr << "  FAIL " << f.filename().string() << ": " << e.what() << "\n";
            ++failed;
        }
    }

    std::cerr << "\n" << ok << " written, " << failed << " failed\n";
    if (total_clipped) {
        std::cerr << "\n*** " << total_clipped << " samples CLIPPED by i24 export.\n"
                  << "*** Peak / SPPM / true-peak measured from these WAVs are INVALID.\n"
                  << "*** Re-export with --bits f32.\n";
        return 2;
    }
    return failed ? 1 : 0;
}
