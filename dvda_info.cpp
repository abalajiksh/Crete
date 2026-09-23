// dvda_info.cpp -- print the structure of a DVD-Audio title set.
//
//   make dvda-info
//   ./dvda-info /path/to/AUDIO_TS/ATS_01_0.IFO
//   ./dvda-info /path/to/AUDIO_TS/ATS_01_1.AOB     (the IFO is found for you)
//
// Reads the disc's own tables (dvda.hpp) and prints the titles, their audio
// formats and every track length. Parsing only -- it decodes nothing, so it
// runs in a second on a 4.5 GB title set.
//
// WHY A SEPARATE TOOL. crete does not yet SLICE a DVD-Audio title set into
// tracks: doing that needs each track's byte extent in the AOB stream, and
// while the IFO evidently carries an audio-object index (see the note at the
// end of dvda.hpp), that table is not yet proven the way the track table is.
// Shipping a reader whose structure is confirmed, separately from a byte
// mapping that is not, is deliberate -- a wrong byte offset in a lossless
// stream produces audio that plays and numbers that look reasonable, which is
// the failure mode the SACD work ran into three times.
//
// So this tool answers "what is on this disc, and does its own table agree
// with itself" today, and is the harness for the extraction when that lands.

#include "dvda.hpp"
#include "dvd.hpp"

#include <cstdio>
#include <exception>

static void print_set(const dvda::TitleSet& ts) {
    std::printf("IFO              : %s\n", ts.ifo_path.c_str());
    std::printf("Title set        : %u sectors (%.2f GiB), AOB from sector %u\n",
                ts.last_sector, ts.last_sector * 2048.0 / (1024.0 * 1024 * 1024),
                ts.aob_start_sector);
    if (!ts.aob_paths.empty()) {
        std::printf("Fragments        : %zu\n", ts.aob_paths.size());
        for (const auto& p : ts.aob_paths)
            std::printf("                   %s\n",
                        std::filesystem::path(p).filename().string().c_str());
    }
    std::printf("Audio formats    : %zu\n", ts.formats.size());
    for (size_t i = 0; i < ts.formats.size(); ++i)
        std::printf("             [%zu]  %s\n", i, dvda::format_desc(ts.formats[i]).c_str());

    std::printf("Titles           : %zu  (%zu tracks in total)\n",
                ts.titles.size(), ts.track_count());
    for (const auto& t : ts.titles) {
        const unsigned s = static_cast<unsigned>(t.secs());
        std::printf("\n  Title %u — %zu tracks, %u:%02u\n", t.index, t.tracks.size(), s / 60, s % 60);
        std::printf("    %s\n", dvda::format_desc(t.format).c_str());
        for (const auto& tr : t.tracks) {
            const unsigned d = static_cast<unsigned>(tr.secs());
            std::printf("      %2u   %u:%02u   (%.2f s)\n", tr.number, d / 60, d % 60, tr.secs());
        }
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr,
            "usage: %s <ATS_NN_0.IFO | ATS_NN_n.AOB>\n"
            "  Prints the DVD-Audio title set's structure. Decodes nothing.\n", argv[0]);
        return 2;
    }
    try {
        const std::string arg = argv[1];
        const std::string ifo = dvda::looks_like_dvda_ifo(arg) ? arg : dvda::ifo_for(arg);
        if (ifo.empty()) {
            std::fprintf(stderr, "Error: no DVD-Audio ATS IFO found for %s\n"
                                 "  (expected ATS_NN_0.IFO beside it)\n", arg.c_str());
            return 1;
        }
        dvda::TitleSet ts = dvda::parse_ifo(ifo);
        if (!dvda::looks_like_dvda_ifo(arg)) ts.aob_paths = dvd::fragments_of(arg);
        print_set(ts);

        // The disc's tables must agree with themselves. Each title states its
        // own length and each track states one; they are separate fields in
        // separate structures, so agreement is evidence the offsets are right
        // and disagreement means this set must not be sliced on those numbers.
        const std::string err = dvda::validate(ts);
        if (err.empty()) {
            std::printf("\nConsistency      : OK — every title's track lengths sum "
                        "exactly to its declared total\n");
            return 0;
        }
        std::printf("\nConsistency      : FAILED — %s\n", err.c_str());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}
