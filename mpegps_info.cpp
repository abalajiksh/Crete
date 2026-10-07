// mpegps_info.cpp -- print the structure of an MPEG program stream.
//
//   make mpegps-info
//   ./mpegps-info /path/to/VIDEO_TS/VTS_02_1.VOB     (the whole title set)
//   ./mpegps-info --head 64 movie.mpg                 (first 64 MB only)
//   ./mpegps-info --ids VTS_02_1.VOB                  (one line per stream)
//   ./mpegps-info --single VTS_02_2.VOB               (this fragment alone)
//
// Reads packs and PES headers (mpegps.hpp) and decodes nothing, so it lists
// every elementary stream a .vob / .aob / .mpg carries -- by the ID the disc
// gives it -- in any crete build, including the zero-dependency one. Point it
// at any fragment of a DVD title set and it reads the whole set as one
// stream, as crete does (dvd.hpp).
//
// --ids prints "<ffmpeg id> <kind> <description>" per stream, sorted, the form
// scripts/check_mpegps.sh compares against ffprobe.

#include "mpegps.hpp"
#include "dvd.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

static std::string human(uint64_t b) {
    char buf[32];
    if (b >= (1ull << 30)) std::snprintf(buf, sizeof buf, "%.2f GiB", b / double(1ull << 30));
    else if (b >= (1ull << 20)) std::snprintf(buf, sizeof buf, "%.1f MiB", b / double(1ull << 20));
    else if (b >= 1024) std::snprintf(buf, sizeof buf, "%.1f KiB", b / 1024.0);
    else std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(b));
    return buf;
}

int main(int argc, char** argv) {
    uint64_t head = 0;
    bool ids = false, single = false;
    std::string path;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--head") && i + 1 < argc) {
            head = std::strtoull(argv[++i], nullptr, 10) << 20;
        } else if (!std::strcmp(argv[i], "--ids")) {
            ids = true;
        } else if (!std::strcmp(argv[i], "--single")) {
            single = true;
        } else if (argv[i][0] == '-' && argv[i][1]) {
            path.clear();
            break;
        } else {
            path = argv[i];
        }
    }
    if (path.empty()) {
        std::fprintf(stderr,
            "usage: %s [--head MB] [--ids] [--single] <file.vob | file.aob | file.mpg>\n"
            "  Lists an MPEG program stream's elementary streams. Decodes nothing.\n", argv[0]);
        return 2;
    }
    try {
        const auto parts = single ? std::vector<std::string>{path} : dvd::fragments_of(path);
        if (!mpegps::looks_like_ps(parts.front())) {
            std::fprintf(stderr, "Error: %s does not start with an MPEG pack header\n", path.c_str());
            return 1;
        }
        const mpegps::Info m = mpegps::scan(parts, head);

        if (ids) {
            for (const auto& s : m.streams)
                std::printf("%#x %s %s\n", s.ffmpeg_id(), mpegps::kind_name(s.kind),
                            mpegps::stream_desc(s).c_str());
            return 0;
        }

        std::printf("Input            : %s\n", mpegps::carrier(path, m).c_str());
        if (parts.size() > 1) {
            std::printf("Fragments        : %zu\n", parts.size());
            for (const auto& p : parts)
                std::printf("                   %s\n",
                            std::filesystem::path(p).filename().string().c_str());
        }
        std::printf("Format           : %s, %llu packs%s\n",
                    m.mpeg == 1 ? "MPEG-1 system stream" : "MPEG-2 program stream",
                    static_cast<unsigned long long>(m.packs),
                    m.aligned_2048 ? ", every pack on a 2048-byte boundary" : "");
        if (m.system_header)
            std::printf("System header    : audio_bound %d, video_bound %d\n",
                        m.audio_bound, m.video_bound);
        if (m.has_psm) std::printf("Stream map       : present\n");
        std::printf("Scanned          : %s of %s%s\n", human(m.scanned).c_str(),
                    human(m.total).c_str(), m.reached_end ? " (all)" : "");
        if (m.unknown_private)
            std::printf("Unrecognised     : %llu private_stream_1 packets with a sub-stream ID in no DVD range\n",
                        static_cast<unsigned long long>(m.unknown_private));
        if (m.junk)
            std::printf("Resync           : %s skipped to regain pack sync\n", human(m.junk).c_str());

        std::printf("Streams          : %zu\n", m.streams.size());
        for (const auto& s : m.streams)
            std::printf("  %-6s %-11s %-38s %10llu packets  %s\n",
                        mpegps::hex_id(s.id()).c_str(), mpegps::kind_name(s.kind),
                        mpegps::stream_desc(s).c_str(),
                        static_cast<unsigned long long>(s.packets), human(s.bytes).c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
