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
// in audio.hpp), so the bit-exact agreement with MAAT (PCM) and foobar Direct
// (DSD) is preserved. DSD in particular MUST stay native: FFmpeg's dsd2pcm
// decimation filter differs from foobar's fir1_8/fir1_16.
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
#include <libswresample/swresample.h>
}

#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace audio {

inline AudioData decode_with_ffmpeg(const std::string& path) {
    AudioData ad;

    // ── Open container ──────────────────────────────────────────────────
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
        throw std::runtime_error("FFmpeg: cannot open " + path);
    struct FmtGuard { AVFormatContext** p; ~FmtGuard() { if (*p) avformat_close_input(p); } } fg{&fmt};

    if (avformat_find_stream_info(fmt, nullptr) < 0)
        throw std::runtime_error("FFmpeg: no stream info in " + path);

    // ── Pick best audio stream + decoder ────────────────────────────────
    const AVCodec* dec = nullptr;
    int stream_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
    if (stream_idx < 0 || !dec)
        throw std::runtime_error("FFmpeg: no audio stream in " + path);
    AVStream* st = fmt->streams[stream_idx];

    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if (!ctx) throw std::runtime_error("FFmpeg: codec context alloc failed");
    struct CtxGuard { AVCodecContext** p; ~CtxGuard() { if (*p) avcodec_free_context(p); } } cg{&ctx};

    if (avcodec_parameters_to_context(ctx, st->codecpar) < 0)
        throw std::runtime_error("FFmpeg: copy codec params failed");
    if (avcodec_open2(ctx, dec, nullptr) < 0)
        throw std::runtime_error("FFmpeg: cannot open decoder");

    int in_rate = ctx->sample_rate;
    int nch     = ctx->ch_layout.nb_channels;
    if (in_rate <= 0 || nch <= 0)
        throw std::runtime_error("FFmpeg: invalid stream parameters in " + path);

    // ── Resampler: convert to planar f64, identical rate + layout ───────
    // Copying the input layout (rather than imposing a default) guarantees no
    // channel remapping — this is a pure sample-format conversion.
    AVChannelLayout out_layout;
    av_channel_layout_copy(&out_layout, &ctx->ch_layout);
    struct LayoutGuard { AVChannelLayout* p; ~LayoutGuard() { av_channel_layout_uninit(p); } } lg{&out_layout};

    SwrContext* swr = nullptr;
    if (swr_alloc_set_opts2(&swr,
            &out_layout,     AV_SAMPLE_FMT_DBLP, in_rate,
            &ctx->ch_layout, ctx->sample_fmt,    in_rate,
            0, nullptr) < 0 || !swr)
        throw std::runtime_error("FFmpeg: swr alloc failed");
    struct SwrGuard { SwrContext** p; ~SwrGuard() { if (*p) swr_free(p); } } sg{&swr};
    if (swr_init(swr) < 0)
        throw std::runtime_error("FFmpeg: swr init failed");

    // ── AudioData header ────────────────────────────────────────────────
    {
        std::string name = dec->name ? dec->name : "ffmpeg";
        for (auto& ch : name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        ad.codec = name;  // e.g. ALAC / AAC / MP3 / OPUS — signals FFmpeg origin
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
