// ============================================================================
// budget.hpp — host resource discovery and per-group memory estimation.
//
// WHY THIS EXISTS.  crete's default is one worker per logical CPU, and a
// worker holds a whole decoded album track in f64.  On a 6-core 16 GB box a
// DSD512 album needs ~3.7 GB per worker, so the default asks for 22 GB and the
// OOM killer takes the process -- with EMPTY stderr and no core, which reads
// as a crash rather than as "you asked for too much".  That failure is already
// on record twice: Weekly #69/#70 died exactly this way and #68 survived only
// by luck.  The harness worked around it OUTSIDE crete (tests/helpers.py
// dsd_jobs()), which helps CI and does nothing for anyone who just runs the
// binary.  This is that logic moved to where the problem is.
//
// TWO NUMBERS, MEASURED NOT GUESSED.  Peak RSS for one worker fits
//
//     bytes ~= file_bytes + K * (channels * out_rate * seconds * 8)
//
// with K measured at 2.21 / 2.25 / 2.50 on WAV 24-bit 6ch/48k, FLAC 24-bit
// 4ch/192k and FLAC 24-bit 2ch/192k respectively (3.9 GB peak on a 320 MB
// file).  K covers the decoded channels plus the K-weighted copy analysis.hpp
// builds beside them.  MEM_K below is the conservative end of that range.
//
// Note what the model does NOT use: a multiple of the file size.  Measured
// file->peak ratios across those same three files were 6.91x, 12.18x and
// 13.42x -- a 2x spread, because compression ratio and bit depth both move it.
// A file-size multiplier alone would either over-restrict WAV or under-protect
// FLAC.  The shape peek exists to avoid that.
//
// ZERO DEPENDENCY.  Platform headers only, same arrangement as
// file_dialog.hpp.  Nothing here affects a measurement; it chooses how many
// workers run at once and prints what it found.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <algorithm>
#include <fstream>
#include <vector>
#include <cmath>
#include <cerrno>
#include <cstdlib>

#if defined(_WIN32)
  #include <windows.h>
#elif defined(__APPLE__)
  #include <sys/sysctl.h>
  #include <mach/mach.h>
  #include <mach/mach_host.h>
#else
  #include <unistd.h>
  #include <sched.h>
#endif

namespace budget {

// ── Host resources ─────────────────────────────────────────────────────────
struct Host {
    uint64_t    total_ram  = 0;   // physical RAM, or a lower cgroup/job limit
    uint64_t    usable_ram = 0;   // what is actually free to take right now
    unsigned    cpus       = 0;   // affinity- and cgroup-aware logical CPUs
    std::string ram_source = "unknown";
    std::string cpu_source = "unknown";
    bool        confined   = false;  // a container limit is in force
};

namespace detail {

inline uint64_t read_u64_file(const char* path, bool* ok = nullptr) {
    if (ok) *ok = false;
    std::ifstream f(path);
    if (!f) return 0;
    std::string s;
    if (!(f >> s)) return 0;
    if (s == "max") return 0;                 // cgroup v2 "no limit"
    errno = 0;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (end == s.c_str() || errno) return 0;
    if (ok) *ok = true;
    return static_cast<uint64_t>(v);
}

// One key out of a "Key:  <n> kB" style file (/proc/meminfo).
inline uint64_t read_meminfo_kb(const char* key) {
    std::ifstream f("/proc/meminfo");
    if (!f) return 0;
    std::string line;
    const size_t klen = std::strlen(key);
    while (std::getline(f, line)) {
        if (line.compare(0, klen, key) != 0) continue;
        const char* p = line.c_str() + klen;
        while (*p == ':' || *p == ' ' || *p == '\t') ++p;
        return static_cast<uint64_t>(std::strtoull(p, nullptr, 10)) * 1024ull;
    }
    return 0;
}

}  // namespace detail

// Physical RAM and what is free right now.
//
// "Usable" is deliberately NOT "total minus what this process already holds".
// Other things are running; on Linux MemAvailable is the kernel's own estimate
// of what a new allocation can get without swapping, which is exactly the
// question being asked.  macOS has no MemAvailable, so free + inactive +
// purgeable is the equivalent: inactive and purgeable pages are reclaimed
// under pressure rather than swapped.
inline Host probe_host() {
    Host h;

#if defined(_WIN32)
    MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        h.total_ram  = ms.ullTotalPhys;
        h.usable_ram = ms.ullAvailPhys;
        h.ram_source = "GlobalMemoryStatusEx";
    }
    SYSTEM_INFO si; GetSystemInfo(&si);
    h.cpus = si.dwNumberOfProcessors;
    h.cpu_source = "GetSystemInfo";

#elif defined(__APPLE__)
    uint64_t mem = 0; size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) {
        h.total_ram  = mem;
        h.ram_source = "sysctl hw.memsize";
    }
    vm_size_t page = 0;
    vm_statistics64_data_t vm;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    if (host_page_size(mach_host_self(), &page) == KERN_SUCCESS &&
        host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vm), &cnt) == KERN_SUCCESS) {
        h.usable_ram = (static_cast<uint64_t>(vm.free_count)
                      + vm.inactive_count + vm.purgeable_count) * page;
        h.ram_source = "sysctl + host_statistics64";
    }
    int ncpu = 0; len = sizeof(ncpu);
    // activecpu, not hw.ncpu: reflects cores actually online.
    if (sysctlbyname("hw.activecpu", &ncpu, &len, nullptr, 0) == 0 && ncpu > 0) {
        h.cpus = static_cast<unsigned>(ncpu);
        h.cpu_source = "sysctl hw.activecpu";
    }

#else   // Linux and other POSIX
    long pages = sysconf(_SC_PHYS_PAGES), psize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psize > 0) {
        h.total_ram  = static_cast<uint64_t>(pages) * static_cast<uint64_t>(psize);
        h.ram_source = "sysconf(_SC_PHYS_PAGES)";
    }
    uint64_t avail = detail::read_meminfo_kb("MemAvailable");
    if (avail) { h.usable_ram = avail; h.ram_source = "/proc/meminfo MemAvailable"; }

    // A CONTAINER LIMIT BEATS PHYSICAL RAM, and this is the case the old code
    // got wrong: hardware_concurrency() and _SC_PHYS_PAGES both report the
    // HOST, so crete inside a 2 GB / 2-CPU container sized itself for the
    // 64-core machine underneath and was killed by the cgroup, not by the OOM
    // killer -- an even quieter death.  cgroup v2 first, then v1.
    bool ok = false;
    uint64_t lim = detail::read_u64_file("/sys/fs/cgroup/memory.max", &ok);
    uint64_t use = ok ? detail::read_u64_file("/sys/fs/cgroup/memory.current") : 0;
    if (!ok) {
        lim = detail::read_u64_file("/sys/fs/cgroup/memory/memory.limit_in_bytes", &ok);
        use = ok ? detail::read_u64_file("/sys/fs/cgroup/memory/memory.usage_in_bytes") : 0;
        // v1 reports "no limit" as a huge sentinel rather than a word.
        if (ok && lim > (uint64_t(1) << 60)) ok = false;
    }
    if (ok && lim && (h.total_ram == 0 || lim < h.total_ram)) {
        h.total_ram  = lim;
        h.usable_ram = lim > use ? lim - use : 0;
        h.ram_source = "cgroup memory limit";
        h.confined   = true;
    }

    // CPUs: affinity mask, then a cgroup CPU quota, then the fallback.
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        h.cpus = static_cast<unsigned>(CPU_COUNT(&set));
        h.cpu_source = "sched_getaffinity";
    }
    {   // cgroup v2 "cpu.max" is "<quota> <period>"; v1 splits it in two files.
        std::ifstream f("/sys/fs/cgroup/cpu.max");
        std::string q, p;
        if (f && (f >> q >> p) && q != "max") {
            double quota = std::strtod(q.c_str(), nullptr);
            double per   = std::strtod(p.c_str(), nullptr);
            if (per > 0 && quota > 0) {
                unsigned n = static_cast<unsigned>(quota / per);
                if (n < 1) n = 1;
                if (h.cpus == 0 || n < h.cpus) {
                    h.cpus = n; h.cpu_source = "cgroup cpu.max"; h.confined = true;
                }
            }
        }
    }
#endif

    if (h.cpus == 0) {
        unsigned hw = std::thread::hardware_concurrency();
        h.cpus = hw ? hw : 4;
        h.cpu_source = hw ? "hardware_concurrency" : "default 4";
    }
    // No usable figure anywhere: assume half of physical is takeable.  Better
    // than assuming all of it, and it still beats the old behaviour of not
    // looking at memory at all.
    if (h.usable_ram == 0 && h.total_ram) {
        h.usable_ram = h.total_ram / 2;
        h.ram_source += " (available unknown, assuming 1/2)";
    }
    return h;
}

// ── What a file will cost, read from its header ────────────────────────────
//
// Only the first 64 KB is read.  Every field needed lives in the opening
// header of each format by construction, and a partial read is reported as
// unknown rather than guessed at.
struct Shape {
    bool        known     = false;
    unsigned    channels  = 0;
    unsigned    rate      = 0;     // PCM sample rate, or the DSD bitstream rate
    double      seconds   = 0;
    bool        is_dsd    = false; // decoded rate is --dsd-out-rate, not `rate`
    std::string basis;             // which header answered, for diagnostics
};

namespace detail {

inline uint16_t b16le(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t b32le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
}
inline uint64_t b64le(const uint8_t* p) {
    uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | p[i]; return v;
}
inline uint32_t b32be(const uint8_t* p) {
    return (uint32_t(p[0])<<24) | (uint32_t(p[1])<<16) | (uint32_t(p[2])<<8) | uint32_t(p[3]);
}
inline uint64_t b64be(const uint8_t* p) {
    uint64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | p[i]; return v;
}
inline bool tag(const uint8_t* p, const char* s) { return std::memcmp(p, s, 4) == 0; }

// 80-bit IEEE 754 extended, as AIFF stores its sample rate.
inline double ext80(const uint8_t* p) {
    int ex = ((p[0] & 0x7F) << 8) | p[1];
    uint64_t m = b64be(p + 2);
    if (ex == 0 && m == 0) return 0;
    double v = double(m) * std::pow(2.0, double(ex - 16383 - 63));
    return (p[0] & 0x80) ? -v : v;
}

}  // namespace detail

inline Shape peek(const std::string& path) {
    Shape sh;
    std::ifstream f(path, std::ios::binary);
    if (!f) return sh;
    std::vector<uint8_t> b(65536);
    f.read(reinterpret_cast<char*>(b.data()), std::streamsize(b.size()));
    size_t n = size_t(f.gcount());
    b.resize(n);
    if (n < 64) return sh;
    const uint8_t* d = b.data();
    using namespace detail;

    auto finish_pcm = [&](unsigned ch, unsigned rate, double secs, const char* basis) {
        if (!ch || !rate || secs <= 0) return;
        sh.known = true; sh.channels = ch; sh.rate = rate;
        sh.seconds = secs; sh.basis = basis;
    };

    // ── FLAC ───────────────────────────────────────────────────────────────
    // STREAMINFO is mandatory and mandatory-first, so it is always in reach.
    if (tag(d, "fLaC") && n >= 4 + 4 + 34) {
        uint64_t v = b64be(d + 8 + 10);        // 4 magic + 4 block header + 10
        unsigned rate = unsigned(v >> 44);
        unsigned ch   = unsigned((v >> 41) & 0x7) + 1;
        uint64_t tot  = v & 0xFFFFFFFFFull;    // 36 bits; 0 means "unknown"
        if (rate && tot) finish_pcm(ch, rate, double(tot) / rate, "FLAC STREAMINFO");
        return sh;
    }

    // ── RIFF / RF64 / BW64 ─────────────────────────────────────────────────
    if ((tag(d, "RIFF") || tag(d, "RF64") || tag(d, "BW64")) && n >= 12 && tag(d + 8, "WAVE")) {
        unsigned ch = 0, rate = 0, bits = 0;
        uint64_t data_bytes = 0, ds64_samples = 0;
        bool have_ds64 = false;
        size_t p = 12;
        while (p + 8 <= n) {
            const uint8_t* id = d + p;
            uint64_t sz = b32le(d + p + 4);
            const uint8_t* body = d + p + 8;
            size_t avail = n - (p + 8);
            if (tag(id, "fmt ") && avail >= 16) {
                ch = b16le(body + 2); rate = b32le(body + 4); bits = b16le(body + 14);
            } else if (tag(id, "ds64") && avail >= 24) {
                data_bytes   = b64le(body + 8);
                ds64_samples = b64le(body + 16);
                have_ds64 = true;
            } else if (tag(id, "data")) {
                if (sz != 0xFFFFFFFFull) data_bytes = sz;
                break;   // data is last in practice; its body is the payload
            }
            if (sz == 0xFFFFFFFFull && !have_ds64) break;
            p += 8 + size_t(sz) + (sz & 1);
        }
        if (ch && rate) {
            double secs = 0;
            if (ds64_samples) secs = double(ds64_samples) / rate;
            else if (data_bytes && bits) secs = double(data_bytes) / (double(ch) * (bits/8.0) * rate);
            finish_pcm(ch, rate, secs, have_ds64 ? "RF64 ds64" : "WAV fmt+data");
        }
        return sh;
    }

    // ── Wave64 ─────────────────────────────────────────────────────────────
    // GUIDs are the RIFF fourCC plus a fixed 12-byte suffix, and chunk sizes
    // INCLUDE the 24-byte header -- both differences from RIFF, both easy to
    // get wrong, so the walk is written out rather than shared.
    if (n >= 40 && tag(d, "riff")) {
        unsigned ch = 0, rate = 0, bits = 0;
        uint64_t data_bytes = 0;
        size_t p = 40;
        while (p + 24 <= n) {
            const uint8_t* id = d + p;
            uint64_t sz = b64le(d + p + 16);
            if (sz < 24) break;
            const uint8_t* body = d + p + 24;
            size_t avail = n - (p + 24);
            if (tag(id, "fmt ") && avail >= 16) {
                ch = b16le(body + 2); rate = b32le(body + 4); bits = b16le(body + 14);
            } else if (tag(id, "data")) { data_bytes = sz - 24; break; }
            p += size_t(sz) + ((8 - (sz & 7)) & 7);
        }
        if (ch && rate && bits && data_bytes)
            finish_pcm(ch, rate, double(data_bytes) / (double(ch) * (bits/8.0) * rate), "Wave64 fmt+data");
        return sh;
    }

    // ── AIFF / AIFF-C ──────────────────────────────────────────────────────
    if (n >= 12 && tag(d, "FORM") && (tag(d + 8, "AIFF") || tag(d + 8, "AIFC"))) {
        size_t p = 12;
        while (p + 8 <= n) {
            const uint8_t* id = d + p;
            uint64_t sz = b32be(d + p + 4);
            const uint8_t* body = d + p + 8;
            if (tag(id, "COMM") && n - (p + 8) >= 18) {
                unsigned ch = (body[0] << 8) | body[1];
                uint32_t frames = b32be(body + 2);
                double rate = ext80(body + 8);
                if (ch && rate > 0 && frames)
                    finish_pcm(ch, unsigned(rate), double(frames) / rate, "AIFF COMM");
                break;
            }
            p += 8 + size_t(sz) + (sz & 1);
        }
        return sh;
    }

    // ── DSF ────────────────────────────────────────────────────────────────
    if (n >= 28 + 52 && tag(d, "DSD ") && tag(d + 28, "fmt ")) {
        const uint8_t* fm = d + 28;
        unsigned ch   = b32le(fm + 24);
        unsigned rate = b32le(fm + 28);
        uint64_t cnt  = b64le(fm + 36);      // sample count PER CHANNEL, in bits
        if (ch && rate && cnt) {
            sh.known = true; sh.channels = ch; sh.rate = rate;
            sh.seconds = double(cnt) / rate; sh.is_dsd = true; sh.basis = "DSF fmt";
        }
        return sh;
    }

    // ── DFF ────────────────────────────────────────────────────────────────
    // Rate lives in PROP/SND/FS, channel count in CHNL, and the payload length
    // in the DSD (or DST) chunk header -- all near the front, so a scan of the
    // opening block finds every one without walking the file.
    if (n >= 16 && tag(d, "FRM8") && tag(d + 12, "DSD ")) {
        unsigned ch = 0, rate = 0;
        uint64_t data_bytes = 0;
        for (size_t p = 0; p + 12 <= n; ++p) {
            if (tag(d + p, "FS  ") && p + 12 <= n && !rate) rate = b32be(d + p + 12);
            else if (tag(d + p, "CHNL") && p + 14 <= n && !ch) ch = (d[p+12] << 8) | d[p+13];
            else if ((tag(d + p, "DSD ") || tag(d + p, "DST ")) && p >= 16 && p + 12 <= n
                     && !data_bytes) data_bytes = b64be(d + p + 4);
        }
        if (ch && rate && data_bytes) {
            sh.known = true; sh.channels = ch; sh.rate = rate;
            sh.seconds = double(data_bytes) * 8.0 / (double(ch) * rate);
            sh.is_dsd = true; sh.basis = "DFF PROP";
        }
        return sh;
    }

    return sh;   // unknown: an FFmpeg container, a disc image, or damaged
}

// ── The model ──────────────────────────────────────────────────────────────
//
// K is the measured multiplier on the decoded f64 payload: the channel vectors
// plus the K-weighted copy analysis.hpp builds beside them.  2.21 / 2.25 / 2.50
// measured (see the header); 2.5 is taken so the estimate errs toward running
// fewer workers than toward being killed.
inline constexpr double   MEM_K          = 2.5;
// Unknown shape: the conservative end of the measured file->peak ratios
// (6.91x .. 13.42x).  Used only when no header answered AND the caller had no
// probe to offer -- for a LOSSY stream it still understates badly, which is
// why FFmpeg containers are shaped from their probe instead of falling here.
inline constexpr double   UNKNOWN_RATIO  = 14.0;
// Never model a worker as costing less than this: process image, FFmpeg
// tables, the oversampling scratch, allocator slack.
inline constexpr uint64_t WORKER_FLOOR   = 64ull << 20;
// Headroom.  Taking every available byte is how you meet the OOM killer one
// page early, and MemAvailable is itself an estimate.
inline constexpr double   USE_FRACTION   = 0.80;

inline uint64_t estimate_bytes(uint64_t file_bytes, const Shape& sh,
                               unsigned dsd_out_rate) {
    double est;
    if (sh.known && sh.channels && sh.seconds > 0) {
        const double rate = sh.is_dsd ? double(dsd_out_rate ? dsd_out_rate : 44100)
                                      : double(sh.rate);
        const double decoded = double(sh.channels) * rate * sh.seconds * 8.0;
        est = double(file_bytes) + MEM_K * decoded;
    } else {
        est = double(file_bytes) * UNKNOWN_RATIO;
    }
    uint64_t v = est > 0 ? uint64_t(est) : 0;
    return std::max(v, WORKER_FLOOR);
}

// ── Choosing the worker count ──────────────────────────────────────────────
struct Plan {
    int         jobs       = 1;
    uint64_t    per_worker = 0;
    uint64_t    budget     = 0;
    unsigned    cpus       = 0;
    bool        reduced    = false;  // memory cut this below the CPU count
    bool        oversized  = false;  // ONE group alone will not fit
    bool        forced     = false;  // the user pinned --jobs; we did not choose
};

// `requested` is the user's --jobs (<=0 means auto).  `limit_override` is
// --memory-limit in bytes (0 = detect).  `largest_group` is the peak estimate
// over ALL groups, not the mean: the pool is order-preserving, so every worker
// can be sitting on a long track at the same moment.  That distinction is what
// the Weekly #69 dsd512 kill came down to.
inline Plan plan_jobs(const Host& h, uint64_t largest_group, size_t ngroups,
                      int requested, uint64_t limit_override) {
    Plan p;
    p.cpus       = h.cpus;
    p.per_worker = std::max(largest_group, WORKER_FLOOR);
    const uint64_t pool = limit_override ? limit_override : h.usable_ram;
    p.budget     = uint64_t(double(pool) * USE_FRACTION);
    p.oversized  = p.budget && p.per_worker > p.budget;

    // An explicit --jobs is an instruction, not a hint.  crete reports what it
    // thinks and then does what it was told; silently overriding the flag
    // would make the run unreproducible, which matters more here than memory.
    if (requested > 0) {
        p.jobs = requested; p.forced = true;
        return p;
    }

    int by_mem = p.budget ? int(p.budget / p.per_worker) : int(h.cpus);
    if (by_mem < 1) by_mem = 1;            // always make progress
    int jobs = std::min<int>(int(h.cpus), by_mem);
    if (ngroups > 0) jobs = std::min<int>(jobs, int(ngroups));
    if (jobs < 1) jobs = 1;
    p.jobs    = jobs;
    p.reduced = (by_mem < int(h.cpus)) && ngroups > size_t(by_mem);
    return p;
}

inline std::string human(uint64_t bytes) {
    static const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(bytes); int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    char buf[64];
    std::snprintf(buf, sizeof buf, (v < 10.0 && i > 0) ? "%.1f %s" : "%.0f %s", v, u[i]);
    return buf;
}

}  // namespace budget
