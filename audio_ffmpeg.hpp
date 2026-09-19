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
#include <stdexcept>
#include <string>
#include <vector>

namespace audio {

// want_stream: the container's audio-stream index to decode (as ffprobe prints
// it), or -1 to let FFmpeg choose.
inline AudioData decode_with_ffmpeg(const std::string& path, int want_stream = -1) {
    AudioData ad;

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

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, &fmt_opts) < 0)
        throw std::runtime_error("FFmpeg: cannot open " + path);
    struct FmtGuard { AVFormatContext** p; ~FmtGuard() { if (*p) avformat_close_input(p); } } fg{&fmt};

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
        if (swr_alloc_set_opts2(&swr,
                &out_layout,     AV_SAMPLE_FMT_DBLP, in_rate,
                &ctx->ch_layout, in_fmt,             in_rate,
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
    // reports profile=unknown.
    //
    // av_get_profile_name() is tried FIRST and is expected to fail here.
    // crete's vendored FFmpeg is configured --enable-small, which makes
    // NULL_IF_CONFIG_SMALL(ff_truehd_profiles) expand to NULL, so every
    // decoder in the compact build has codec->profiles == NULL and the
    // lookup returns NULL for every stream -- including DTS-HD MA, which
    // the system ffprobe names without trouble. Measured 2026-09-19 on all
    // six DSOTM Blu-ray carriers: 6/6 empty through av_get_profile_name,
    // 6/6 correct through the table below. The earlier note that "detection
    // costs nothing, profile carries it" was verified against a full
    // ffprobe, not against the build crete actually links.
    //
    // The profile NUMBER survives --enable-small (it is a plain int on the
    // context); only the name TABLE is compiled out. So crete names it
    // itself. These AV_PROFILE_* values are public ABI constants from
    // libavcodec/defs.h, and the strings match FFmpeg's own so anything
    // grepping crete's output against ffprobe's still matches. Keeping the
    // av_get_profile_name() call first means a crete linked against a full
    // FFmpeg picks up profiles for codecs this table does not list.
    if (const char* prof = av_get_profile_name(dec, ctx->profile)) {
        ad.codec_profile = prof;
    } else if (ctx->profile != AV_PROFILE_UNKNOWN) {
        const char* name = nullptr;
        switch (ctx->codec_id) {
            case AV_CODEC_ID_TRUEHD:
            case AV_CODEC_ID_MLP:
                if (ctx->profile == AV_PROFILE_TRUEHD_ATMOS)
                    name = "Dolby TrueHD + Dolby Atmos";
                break;
            case AV_CODEC_ID_EAC3:
                if (ctx->profile == AV_PROFILE_EAC3_DDP_ATMOS)
                    name = "Dolby Digital Plus + Dolby Atmos";
                break;
            case AV_CODEC_ID_DTS:
                switch (ctx->profile) {
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
        if (name) ad.codec_profile = name;
    }
    ad.sample_rate = static_cast<uint32_t>(in_rate);
    // Lossy codecs have no PCM "bit depth"; bits_per_raw_sample is 0 there.
    ad.bit_depth = (ctx->bits_per_raw_sample > 0)
        ? static_cast<uint32_t>(ctx->bits_per_raw_sample) : 0;
    ad.channels.resize(nch);

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

    auto drain = [&](const AVFrame* f) {
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

    if (ad.channels.empty() || ad.channels[0].empty())
        throw std::runtime_error("FFmpeg: decoded no audio from " + path);

    return ad;
}

} // namespace audio
#endif // CRETE_HAS_FFMPEG
