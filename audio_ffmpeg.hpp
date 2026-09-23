#pragma once
#ifdef CRETE_HAS_FFMPEG
// ============================================================================
// audio_ffmpeg.hpp — Optional FFmpeg decode backend
//
// Provides audio::decode_with_ffmpeg(), producing the SAME AudioData layout as
// the native decoders (per-channel f64 in [-1,1] at the file's native rate).
// It is used ONLY as a fallback for formats crête does not decode natively:
// ALAC (.m4a/.mp4), AAC, MP3, Opus, Vorbis, WMA, WavPack, Monkey's Audio, etc.
//
// The native WAV/AIFF/FLAC/DSF/DFF paths are never routed here (see decode_file
// in audio.hpp), so agreement with the MAAT reference data is preserved. DSD in
// particular MUST stay native: crête decimates via the vendored libdsddpcm
// kernel, whose clean-room filter chain differs from FFmpeg's dsd2pcm.
//
// Conversion is sample-format only. Output rate == input rate, so swresample
// performs no resampling and introduces no delay or sample-count change — the
// measured RMS/peak/DR are computed on the decoder's own PCM, untrimmed beyond
// the normal codec/gapless handling FFmpeg applies from container side-data.
//
// Built against the FFmpeg 5.1+ API (AVChannelLayout, swr_alloc_set_opts2,
// send/receive decode). Tested against the 7.x release series.
//
// NOTE: this header is #included at the END of audio.hpp and relies on
// audio::AudioData already being a complete type. Do not include it standalone.
// ============================================================================

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/dict.h>
#include <libswresample/swresample.h>
}

#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace audio {

// ── Profile naming, shared by the decoder and the stream probe ─────────────
//
// av_get_profile_name() is tried FIRST and is expected to fail here. crete's
// vendored FFmpeg is configured --enable-small, which makes
// NULL_IF_CONFIG_SMALL(ff_truehd_profiles) expand to NULL, so every decoder in
// the compact build has codec->profiles == NULL and the lookup returns NULL
// for every stream -- including DTS-HD MA, which the system ffprobe names
// without trouble. Measured 2026-09-19 on all six DSOTM Blu-ray carriers: 6/6
// empty through av_get_profile_name, 6/6 correct through the table below. The
// earlier note that "detection costs nothing, profile carries it" was verified
// against a full ffprobe, not against the build crete actually links.
//
// The profile NUMBER survives --enable-small (it is a plain int on the
// context); only the name TABLE is compiled out. So crete names it itself.
// These AV_PROFILE_* values are public ABI constants from libavcodec/defs.h,
// and the strings match FFmpeg's own so anything grepping crete's output
// against ffprobe's still matches. Keeping the av_get_profile_name() call
// first means a crete linked against a full FFmpeg picks up profiles for
// codecs this table does not list.
//
// Lives here, called from both decode_with_ffmpeg() and probe_streams(), so
// the stream the picker labels "Dolby TrueHD + Dolby Atmos" is the one the
// decoder reports under that name. Two copies of this table would eventually
// disagree, and the disagreement would be invisible.
inline std::string profile_name_for(const AVCodec* dec, AVCodecID codec_id,
                                    int profile) {
    if (const char* prof = av_get_profile_name(dec, profile)) return prof;
    if (profile == AV_PROFILE_UNKNOWN) return {};
    const char* name = nullptr;
    switch (codec_id) {
        case AV_CODEC_ID_TRUEHD:
        case AV_CODEC_ID_MLP:
            if (profile == AV_PROFILE_TRUEHD_ATMOS)
                name = "Dolby TrueHD + Dolby Atmos";
            break;
        case AV_CODEC_ID_EAC3:
            if (profile == AV_PROFILE_EAC3_DDP_ATMOS)
                name = "Dolby Digital Plus + Dolby Atmos";
            break;
        case AV_CODEC_ID_DTS:
            switch (profile) {
                case AV_PROFILE_DTS:               name = "DTS";            break;
                case AV_PROFILE_DTS_ES:            name = "DTS-ES";         break;
                case AV_PROFILE_DTS_96_24:         name = "DTS 96/24";      break;
                case AV_PROFILE_DTS_HD_HRA:        name = "DTS-HD HRA";     break;
                case AV_PROFILE_DTS_HD_MA:         name = "DTS-HD MA";      break;
                case AV_PROFILE_DTS_EXPRESS:       name = "DTS Express";    break;
                case AV_PROFILE_DTS_HD_MA_X:       name = "DTS-HD MA + DTS:X";      break;
                case AV_PROFILE_DTS_HD_MA_X_IMAX:  name = "DTS-HD MA + DTS:X IMAX"; break;
                default: break;
            }
            break;
        default: break;
    }
    return name ? std::string(name) : std::string{};
}

// ── Reading several files as ONE stream (DVD title sets) ───────────────────
//
// A DVD title is split across VTS_02_1.VOB, _2, _3 (or ATS_01_1.AOB ...) at a
// pack boundary wherever 1 GB fell. The pieces are one MPEG program stream and
// must be demuxed as one -- see dvd.hpp for why measuring them separately is
// wrong twice over.
//
// FFmpeg's `concat:` protocol would do this, but it is a PROTOCOL and the
// compact build enables none beyond file, so this feeds a custom AVIOContext
// instead. That is the better answer anyway: it keeps the compact tier's
// "--disable-protocols except file" posture intact, and it works on a plain
// file list without the protocol's quoting rules.
struct ConcatSource {
    std::vector<std::string> paths;
    std::vector<int64_t>     starts;    // global offset of each part
    int64_t                  total = 0;
    size_t                   cur   = 0; // which part `fh` holds
    std::ifstream            fh;
    int64_t                  pos   = 0; // global read position

    explicit ConcatSource(const std::vector<std::string>& p) : paths(p) {
        starts.reserve(paths.size());
        for (const auto& f : paths) {
            starts.push_back(total);
            std::error_code ec;
            auto sz = std::filesystem::file_size(f, ec);
            if (ec) throw std::runtime_error("FFmpeg: cannot size " + f);
            total += static_cast<int64_t>(sz);
        }
        open_part(0);
    }

    void open_part(size_t i) {
        fh.close();
        fh.clear();
        fh.open(paths[i], std::ios::binary);
        if (!fh) throw std::runtime_error("FFmpeg: cannot open " + paths[i]);
        cur = i;
    }

    // Which part contains a global offset. Linear: a DVD title is at most 9
    // fragments, so a binary search would be ceremony.
    size_t part_at(int64_t off) const {
        size_t i = 0;
        while (i + 1 < paths.size() && off >= starts[i + 1]) ++i;
        return i;
    }

    int read(uint8_t* buf, int size) {
        if (size <= 0) return 0;
        if (pos >= total) return AVERROR_EOF;

        // One read never crosses a part: FFmpeg is happy with a short read and
        // will simply ask again, and stitching two files into one buffer would
        // hide a truncated fragment behind a full-looking result.
        size_t want_part = part_at(pos);
        if (want_part != cur) open_part(want_part);
        int64_t part_end = (want_part + 1 < paths.size()) ? starts[want_part + 1] : total;
        int64_t room     = part_end - pos;
        if (room < size) size = static_cast<int>(room);

        fh.seekg(pos - starts[want_part]);
        fh.read(reinterpret_cast<char*>(buf), size);
        std::streamsize got = fh.gcount();
        if (got <= 0) return AVERROR_EOF;
        pos += got;
        return static_cast<int>(got);
    }

    int64_t seek(int64_t offset, int whence) {
        if (whence == AVSEEK_SIZE) return total;
        int64_t np = pos;
        if      (whence == SEEK_SET) np = offset;
        else if (whence == SEEK_CUR) np = pos + offset;
        else if (whence == SEEK_END) np = total + offset;
        else return -1;
        if (np < 0 || np > total) return -1;
        pos = np;
        return pos;
    }
};

inline int concat_read(void* opaque, uint8_t* buf, int size) {
    return static_cast<ConcatSource*>(opaque)->read(buf, size);
}
inline int64_t concat_seek(void* opaque, int64_t offset, int whence) {
    return static_cast<ConcatSource*>(opaque)->seek(offset, whence);
}

// Owns the AVIOContext and its buffer for as long as the AVFormatContext does.
// The buffer must be freed from avio->buffer, not from the pointer originally
// handed over: FFmpeg may reallocate it.
struct ConcatIO {
    ConcatSource  src;
    AVIOContext*  avio = nullptr;

    explicit ConcatIO(const std::vector<std::string>& paths) : src(paths) {
        constexpr int kBuf = 1 << 16;
        auto* buffer = static_cast<unsigned char*>(av_malloc(kBuf));
        if (!buffer) throw std::runtime_error("FFmpeg: out of memory for AVIO buffer");
        avio = avio_alloc_context(buffer, kBuf, 0, &src, concat_read, nullptr, concat_seek);
        if (!avio) { av_free(buffer); throw std::runtime_error("FFmpeg: avio_alloc_context failed"); }
    }
    ~ConcatIO() {
        if (avio) { av_freep(&avio->buffer); avio_context_free(&avio); }
    }
    ConcatIO(const ConcatIO&) = delete;
    ConcatIO& operator=(const ConcatIO&) = delete;
};

// Open `parts` as one container. A single path opens normally; several are
// stitched by ConcatIO. The returned ConcatIO must outlive the AVFormatContext,
// so callers declare it FIRST -- destruction runs in reverse.
inline std::unique_ptr<ConcatIO> open_parts(AVFormatContext** fmt,
                                            const std::vector<std::string>& parts,
                                            AVDictionary** fmt_opts) {
    std::unique_ptr<ConcatIO> cio;
    if (parts.size() > 1) {
        cio.reset(new ConcatIO(parts));
        *fmt = avformat_alloc_context();
        if (!*fmt) throw std::runtime_error("FFmpeg: avformat_alloc_context failed");
        (*fmt)->pb     = cio->avio;
        (*fmt)->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (avformat_open_input(fmt, nullptr, nullptr, fmt_opts) < 0)
            throw std::runtime_error("FFmpeg: cannot open title set starting at " + parts.front());
    } else {
        if (avformat_open_input(fmt, parts.front().c_str(), nullptr, fmt_opts) < 0)
            throw std::runtime_error("FFmpeg: cannot open " + parts.front());
    }
    return cio;
}

// ── Enumerate a container's audio streams, without decoding one ────────────
//
// A music Blu-ray rip routinely carries several mixes of the same album and
// they are NOT the same measurement, so the CLI reports which one it measured
// and --stream overrides it. A GUI cannot report-then-retype an index: it has
// to show the choice before the run. This opens the container, reads stream
// parameters and closes it -- no decoder is opened and no frame is decoded, so
// it is cheap enough to call when the user picks a file.
//
// It carries the SAME raised probesize as decode_with_ffmpeg(). Without that,
// a TrueHD stream interleaved among five others reports profile=unknown and an
// unspecified sample format, so the picker would describe a stream differently
// from the way the decoder later reports it.
inline std::vector<StreamInfo> probe_streams(const std::vector<std::string>& parts) {
    std::vector<StreamInfo> out;
    if (parts.empty()) return out;

    AVDictionary* fmt_opts = nullptr;
    av_dict_set(&fmt_opts, "probesize",       "100000000", 0);
    av_dict_set(&fmt_opts, "analyzeduration", "100000000", 0);
    struct FOptGuard { AVDictionary** p; ~FOptGuard() { if (*p) av_dict_free(p); } } fog{&fmt_opts};

    std::unique_ptr<ConcatIO> cio;
    AVFormatContext* fmt = nullptr;
    struct FmtGuard { AVFormatContext** p; ~FmtGuard() { if (*p) avformat_close_input(p); } } fg{&fmt};
    try {
        cio = open_parts(&fmt, parts, &fmt_opts);
    } catch (const std::exception&) {
        return out;   // not openable: caller falls back to automatic selection
    }

    if (avformat_find_stream_info(fmt, nullptr) < 0) return out;

    // Which index FFmpeg would pick unaided -- the picker marks it so "auto"
    // is not a mystery. Ranks by bitrate and channel count, which need NOT
    // agree with the container's default disposition: on a 6-audio-stream
    // DSOTM remux it returns the 13.8 Mbps LPCM track, not the default-flagged
    // TrueHD.
    int best = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVCodecParameters* par = fmt->streams[i]->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_AUDIO) continue;

        StreamInfo si;
        si.index       = static_cast<int>(i);
        si.channels    = par->ch_layout.nb_channels;
        si.sample_rate = par->sample_rate;
        si.is_default  = (static_cast<int>(i) == best);

        const AVCodec* dec = avcodec_find_decoder(par->codec_id);
        si.decodable = (dec != nullptr);
        if (const char* n = avcodec_get_name(par->codec_id)) {
            si.codec = n;
            for (auto& c : si.codec)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        si.profile = profile_name_for(dec, par->codec_id, par->profile);

        // Per-stream duration where the container gives one, else the
        // container's own. Both are in their own time base.
        if (fmt->streams[i]->duration > 0) {
            si.duration_secs = double(fmt->streams[i]->duration)
                             * av_q2d(fmt->streams[i]->time_base);
        } else if (fmt->duration > 0) {
            si.duration_secs = double(fmt->duration) / AV_TIME_BASE;
        }
        out.push_back(std::move(si));
    }
    return out;
}

// want_stream: the container's audio-stream index to decode (as ffprobe prints
// it), or -1 to let FFmpeg choose.
inline AudioData decode_with_ffmpeg(const std::vector<std::string>& parts,
                                    int want_stream = -1) {
    AudioData ad;
    if (parts.empty()) throw std::runtime_error("FFmpeg: no input");
    const std::string& path = parts.front();

    // ── Open container ──────────────────────────────────────────────────
    // FFmpeg's defaults (5 MB probe, 0 analyzeduration) are not enough for a
    // Blu-ray rip: with six audio streams interleaved, the probe can run out
    // before it has seen a TrueHD frame, and the stream is then reported with
    // an UNSPECIFIED sample format -- which fails at resampler setup rather
    // than at open, so it looks like a crete bug. Observed on a 60 s copy of a
    // six-stream DSOTM remux: stream 0 (TrueHD 7.1) probed as sample_fmt=-1.
    // These are ceilings, not mandates, so small files still probe briefly.
    AVDictionary* fmt_opts = nullptr;
    av_dict_set(&fmt_opts, "probesize",       "100000000", 0);   // 100 MB
    av_dict_set(&fmt_opts, "analyzeduration", "100000000", 0);   // 100 s
    struct FOptGuard { AVDictionary** p; ~FOptGuard() { if (*p) av_dict_free(p); } } fog{&fmt_opts};

    std::unique_ptr<ConcatIO> cio;
    AVFormatContext* fmt = nullptr;
    struct FmtGuard { AVFormatContext** p; ~FmtGuard() { if (*p) avformat_close_input(p); } } fg{&fmt};
    cio = open_parts(&fmt, parts, &fmt_opts);

    if (avformat_find_stream_info(fmt, nullptr) < 0)
        throw std::runtime_error("FFmpeg: no stream info in " + path);

    // ── Choose the audio stream ─────────────────────────────────────────
    // A music Blu-ray rip commonly carries several mixes of the same album --
    // LPCM 5.1, DTS-HD MA 5.1, TrueHD 7.1, an AC-3 core, stereo variants --
    // and they are NOT the same measurement. av_find_best_stream() ranks by
    // bitrate and channel count, which need not agree with the container's
    // default disposition: on a 6-audio-stream DSOTM remux it returns the
    // 13.8 Mbps LPCM track, not the default-flagged TrueHD. That choice is
    // defensible but it must not be invisible, so the count and the chosen
    // index are recorded on AudioData and --stream overrides them.
    int n_audio = 0;
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) ++n_audio;
    if (n_audio == 0)
        throw std::runtime_error("FFmpeg: no audio stream in " + path);

    const AVCodec* dec = nullptr;
    int stream_idx;

    if (want_stream >= 0) {
        if (want_stream >= static_cast<int>(fmt->nb_streams))
            throw std::runtime_error(
                "--stream " + std::to_string(want_stream) + ": " + path +
                " has only " + std::to_string(fmt->nb_streams) + " stream(s)");
        if (fmt->streams[want_stream]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            throw std::runtime_error(
                "--stream " + std::to_string(want_stream) + ": not an audio "
                "stream in " + path);
        stream_idx = want_stream;
        dec = avcodec_find_decoder(fmt->streams[stream_idx]->codecpar->codec_id);
        if (!dec)
            throw std::runtime_error(
                "FFmpeg: no decoder for stream " + std::to_string(stream_idx) +
                " in " + path);
    } else {
        stream_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
        if (stream_idx < 0 || !dec)
            throw std::runtime_error("FFmpeg: no audio stream in " + path);
    }

    ad.stream_index       = stream_idx;
    ad.audio_stream_count = n_audio;

    AVStream* st = fmt->streams[stream_idx];

    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if (!ctx) throw std::runtime_error("FFmpeg: codec context alloc failed");
    struct CtxGuard { AVCodecContext** p; ~CtxGuard() { if (*p) avcodec_free_context(p); } } cg{&ctx};

    if (avcodec_parameters_to_context(ctx, st->codecpar) < 0)
        throw std::runtime_error("FFmpeg: copy codec params failed");

    // ── Decode the ENCODED audio, not a playback rendition ──────────────
    // AC-3 / E-AC-3 carry Dolby DRC metadata, and FFmpeg's (e)ac3 decoder
    // APPLIES it by default (drc_scale defaults to 1). That is right for a
    // player and wrong for a meter: it is a playback-time compressor driven
    // by authoring metadata, so measuring it means measuring something that
    // exists on nobody's disc. Measured on a JOC 5.1 bed (Pineapple Thief
    // "Our Mire", 2026-09-16), default-on vs off:
    //
    //     peak  -6.91 -> -0.75      RMS  -20.82 -> -16.97
    //     LUFSi -21.14 -> -17.18    DR raw 14.580 -> 14.821
    //
    // DR itself barely moves -- it is peak-minus-RMS and DRC scales both --
    // but every ABSOLUTE metric was off by 4-6 dB. Peak, RMS, LUFSi, PLR and
    // true peak are all reference-validated against MAAT, so this would have
    // been a silent, systematic error on every AC-3/E-AC-3 file.
    //
    // target_level (0) and heavy_compr (false) already default to inert;
    // they are pinned anyway so a future FFmpeg cannot change them under us.
    // Unknown options are ignored by other decoders, so this is unconditional.
    AVDictionary* dec_opts = nullptr;
    av_dict_set(&dec_opts, "drc_scale",    "0", 0);
    av_dict_set(&dec_opts, "heavy_compr",  "0", 0);
    av_dict_set(&dec_opts, "target_level", "0", 0);
    struct OptGuard { AVDictionary** p; ~OptGuard() { if (*p) av_dict_free(p); } } og{&dec_opts};

    if (avcodec_open2(ctx, dec, &dec_opts) < 0)
        throw std::runtime_error("FFmpeg: cannot open decoder");

    int in_rate = ctx->sample_rate;
    int nch     = ctx->ch_layout.nb_channels;
    if (in_rate <= 0 || nch <= 0)
        throw std::runtime_error("FFmpeg: invalid stream parameters in " + path);

    // ── Resampler: convert to planar f64, identical rate + layout ───────
    // Copying the input layout (rather than imposing a default) guarantees no
    // channel remapping — this is a pure sample-format conversion.
    AVChannelLayout out_layout{};   // zero-init: copy() uninits dst first (frees u.map if order==CUSTOM)
    av_channel_layout_copy(&out_layout, &ctx->ch_layout);
    struct LayoutGuard { AVChannelLayout* p; ~LayoutGuard() { av_channel_layout_uninit(p); } } lg{&out_layout};

    // Configured lazily from the FIRST DECODED FRAME rather than from
    // ctx->sample_fmt. The frame's format is what the decoder actually
    // produced; the context's is what the probe predicted, and the probe can
    // legitimately answer AV_SAMPLE_FMT_NONE (see the probesize note above).
    // For every already-validated path -- FLAC, ALAC, MP3, Opus -- the two
    // agree, so this changes nothing there; it only removes the dependency on
    // the prediction being available.
    SwrContext* swr = nullptr;
    struct SwrGuard { SwrContext** p; ~SwrGuard() { if (*p) swr_free(p); } } sg{&swr};

    auto ensure_swr = [&](AVSampleFormat in_fmt) {
        if (swr) return;
        if (in_fmt == AV_SAMPLE_FMT_NONE)
            throw std::runtime_error("FFmpeg: decoder produced an unknown "
                                     "sample format in " + path);
        // Input layout is out_layout as well: this is a pure sample-format
        // conversion with no remapping, and out_layout now carries whatever
        // the first FRAME declared (see the adoption above), which ctx may
        // not.
        if (swr_alloc_set_opts2(&swr,
                &out_layout, AV_SAMPLE_FMT_DBLP, in_rate,
                &out_layout, in_fmt,             in_rate,
                0, nullptr) < 0 || !swr)
            throw std::runtime_error("FFmpeg: swr alloc failed");
        if (swr_init(swr) < 0)
            throw std::runtime_error("FFmpeg: swr init failed");
    };

    // ── AudioData header ────────────────────────────────────────────────
    {
        std::string name = dec->name ? dec->name : "ffmpeg";
        for (auto& ch : name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        ad.codec = name;  // e.g. ALAC / AAC / MP3 / OPUS — signals FFmpeg origin
    }
    // ── Profile: which premium layer this stream declares ───────────────
    // The whole cost of identifying Atmos and DTS:X -- no MLP substream
    // parser, no dec3 box reader -- because AVCodecContext::profile carries
    // it for both carriers. It NEEDS the raised probesize above: at FFmpeg's
    // default 5 MB probe a TrueHD stream interleaved among five others
    // reports profile=unknown. Naming lives in profile_name_for() so the
    // stream picker and this decode cannot label the same stream differently.
    ad.codec_profile = profile_name_for(dec, ctx->codec_id, ctx->profile);
    ad.sample_rate = static_cast<uint32_t>(in_rate);
    // Lossy codecs have no PCM "bit depth"; bits_per_raw_sample is 0 there.
    ad.bit_depth = (ctx->bits_per_raw_sample > 0)
        ? static_cast<uint32_t>(ctx->bits_per_raw_sample) : 0;
    ad.channels.resize(nch);

    // Reserve from the container's declared duration. The frame count is not
    // known up front on a compressed stream, so without this each channel
    // vector grows geometrically and the run PEAKS at roughly 1.5x its final
    // size while the old buffer is still alive beside the new one -- measured
    // at 25-35% above the model on AC-3, TrueHD and DTS-HD MA clips. Purely an
    // allocation change: the same values are inserted in the same order into
    // a vector that already has room, so nothing measured moves.
    //
    // A short or absent duration is harmless -- insert() still grows -- so
    // this is a hint, never a bound.
    if (fmt->duration > 0 && in_rate > 0) {
        const double secs = double(fmt->duration) / AV_TIME_BASE;
        const double frames = secs * double(in_rate);
        if (frames > 0 && frames < 4e10) {
            for (auto& ch : ad.channels)
                ch.reserve(static_cast<size_t>(frames) + 4096);
        }
    }

    // ── Decode loop ─────────────────────────────────────────────────────
    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frm = av_frame_alloc();
    struct PFGuard {
        AVPacket** pk; AVFrame** fr;
        ~PFGuard() { if (*pk) av_packet_free(pk); if (*fr) av_frame_free(fr); }
    } pfg{&pkt, &frm};
    if (!pkt || !frm) throw std::runtime_error("FFmpeg: packet/frame alloc failed");

    std::vector<std::vector<double>> scratch(nch);   // persistent per-channel buffers
    std::vector<uint8_t*>            out_planes(nch);

    // ── Guard against a mid-stream format change ────────────────────────
    // The resampler is built once, from the first decoded frame, and every
    // later frame is pushed through it on the assumption that it matches. If
    // one does not, swr_convert reads the wrong planes at the wrong width and
    // the output is silently wrong -- the failure mode this project keeps
    // finding, where the numbers stay plausible.
    //
    // It is reachable, and not only in theory. A DVD-Audio title set holds
    // several audio GROUPS: Fleetwood Mac's Rumours (2001) carries a 96 kHz
    // 5.1 mix, a 96 kHz stereo mix and a 48 kHz version in one ATS, and the
    // group boundary does NOT align with the file fragments -- measured on
    // that disc the 6ch -> 2ch change happens partway through ATS_01_3.AOB,
    // so even a single .aob can cross one. FFmpeg reports it only as
    // "Parity check failed" and "DTS discontinuity" on stderr and carries on.
    //
    // crete measures up to the change and says so, rather than metering two
    // different programs as one. Choosing WHICH group would need the
    // ATS_01_0.IFO group table parsed; the first is the one the disc presents
    // first, and it is the 5.1 mix on every DVD-Audio seen here.
    int  first_fmt   = -1;
    int  first_ch    = -1;
    int  first_rate  = -1;
    bool fmt_changed = false;

    auto drain = [&](const AVFrame* f) {
        if (f) {
            if (first_fmt < 0) {
                first_fmt  = f->format;
                first_ch   = f->ch_layout.nb_channels;
                first_rate = f->sample_rate;
                // THE FRAME WINS over the container's answer.
                //
                // nch, in_rate, out_layout, ad.channels and the scratch
                // buffers were all sized from ctx, i.e. from what
                // avformat_find_stream_info PREDICTED. On a multi-group
                // DVD-Audio title set that prediction is not merely
                // incomplete, it can describe a DIFFERENT group from the one
                // playback starts in: probing deeply enough to reach the
                // stereo group of Rumours makes the container report 2 ch for
                // a stream whose first frame is 6 ch. Everything downstream
                // would then be sized wrong and the numbers meaningless.
                //
                // This is the same decision already taken for sample_fmt a few
                // lines above -- "the frame's format is what the decoder
                // actually produced; the context's is what the probe
                // predicted" -- extended to the layout and the rate, because
                // the probe can be wrong about those in exactly the same way.
                if (first_ch != nch || first_rate != in_rate) {
                    nch     = first_ch;
                    in_rate = first_rate;
                    av_channel_layout_uninit(&out_layout);
                    av_channel_layout_copy(&out_layout, &f->ch_layout);
                    ad.sample_rate = static_cast<uint32_t>(in_rate);
                    ad.channels.assign(nch, {});
                    scratch.assign(nch, {});
                    out_planes.assign(nch, nullptr);
                }
            } else if (f->format                != first_fmt ||
                       f->ch_layout.nb_channels != first_ch  ||
                       f->sample_rate           != first_rate) {
                fmt_changed = true;
                return;   // do not feed a resampler configured for the old format
            }
        }
        if (f) ensure_swr(static_cast<AVSampleFormat>(f->format));
        if (!swr) return;          // flush before any frame decoded: nothing buffered
        int in_samples = f ? f->nb_samples : 0;
        int cap = swr_get_out_samples(swr, in_samples);
        if (cap < in_samples) cap = in_samples;
        if (cap <= 0) { if (f) return; cap = 1; }
        for (int c = 0; c < nch; ++c) {
            if (static_cast<int>(scratch[c].size()) < cap) scratch[c].resize(cap);
            out_planes[c] = reinterpret_cast<uint8_t*>(scratch[c].data());
        }
        std::vector<const uint8_t*> in_planes;
        const uint8_t** in = nullptr;
        if (f) {
            in_planes.resize(nch);
            for (int c = 0; c < nch; ++c) in_planes[c] = f->extended_data[c];
            in = in_planes.data();
        }
        int got = swr_convert(swr, out_planes.data(), cap, in, in_samples);
        if (got < 0) throw std::runtime_error("FFmpeg: swr_convert failed");
        for (int c = 0; c < nch; ++c)
            ad.channels[c].insert(ad.channels[c].end(),
                                  scratch[c].begin(), scratch[c].begin() + got);
    };

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == stream_idx) {
            if (avcodec_send_packet(ctx, pkt) >= 0) {
                while (avcodec_receive_frame(ctx, frm) >= 0) drain(frm);
            }
        }
        av_packet_unref(pkt);
    }

    // Flush decoder, then flush any samples buffered inside swresample.
    avcodec_send_packet(ctx, nullptr);
    while (avcodec_receive_frame(ctx, frm) >= 0) drain(frm);
    drain(nullptr);

    if (fmt_changed) {
        ad.warnings.push_back(
            "audio format changes partway through this stream (a DVD-Audio title "
            "set holds several audio groups); measured the first group only, "
            + std::to_string(first_ch) + " ch / "
            + std::to_string(first_rate) + " Hz");
    }

    if (ad.channels.empty() || ad.channels[0].empty())
        throw std::runtime_error("FFmpeg: decoded no audio from " + path);

    return ad;
}

} // namespace audio
#endif // CRETE_HAS_FFMPEG
