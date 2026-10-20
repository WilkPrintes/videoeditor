#include "media/Mp4Writer.h"

#include "core/Log.h"
#include "media/EncoderThread.h"
#include "media/Encoders.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace ve {

namespace {

AVPixelFormat pickPixelFormat(const AVCodecContext* ctx, const AVCodec* codec)
{
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count) < 0 || !configs)
        return AV_PIX_FMT_YUV420P; // encoder accepts anything
    const auto* formats = static_cast<const AVPixelFormat*>(configs);
    for (AVPixelFormat wanted : {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P}) {
        for (int i = 0; i < count; ++i) {
            if (formats[i] == wanted)
                return wanted;
        }
    }
    return AV_PIX_FMT_NONE;
}

ff::CodecContextPtr openVideoEncoderWith(const std::string& name, Rate rate, int width, int height,
                                         int bitrateKbps, bool globalHeader, bool mfHardware, std::string* error)
{
    const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec) {
        *error = "not in this FFmpeg build";
        return nullptr;
    }
    ff::CodecContextPtr ctx(avcodec_alloc_context3(codec));
    ctx->width = width;
    ctx->height = height;
    ctx->time_base = AVRational{rate.den, rate.num};
    ctx->framerate = AVRational{rate.num, rate.den};
    ctx->sample_aspect_ratio = AVRational{1, 1};
    ctx->pix_fmt = pickPixelFormat(ctx.get(), codec);
    if (ctx->pix_fmt == AV_PIX_FMT_NONE) {
        *error = "no NV12/YUV420P input support";
        return nullptr;
    }
    ctx->bit_rate = std::int64_t(bitrateKbps) * 1000;
    ctx->gop_size = std::max(1, int(std::lround(rate.toDouble() * 2.0)));
    ctx->max_b_frames = (name == "h264_mf" || name == "libopenh264") ? 0 : 2;
    ctx->color_primaries = AVCOL_PRI_BT709;
    ctx->color_trc = AVCOL_TRC_BT709;
    ctx->colorspace = AVCOL_SPC_BT709;
    ctx->color_range = AVCOL_RANGE_MPEG;
    if (globalHeader)
        ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVDictionary* opts = nullptr;
    if (name == "h264_nvenc") {
        av_dict_set(&opts, "preset", "p5", 0);
        av_dict_set(&opts, "rc", "vbr", 0);
    } else if (name == "libx264") {
        av_dict_set(&opts, "preset", "medium", 0);
    } else if (name == "h264_videotoolbox") {
        av_dict_set(&opts, "allow_sw", "1", 0);
    } else if (name == "h264_mf" && mfHardware) {
        av_dict_set(&opts, "hw_encoding", "1", 0); // GPU vendor MFT (NVIDIA/AMD/Intel)
    }
    const int r = avcodec_open2(ctx.get(), codec, &opts);
    av_dict_free(&opts);
    if (r < 0) {
        *error = ff::errorString(r);
        return nullptr;
    }
    return ctx;
}

// Media Foundation is tried with the GPU vendor's encoder first, then Microsoft's software MFT.
ff::CodecContextPtr openVideoEncoder(const std::string& name, Rate rate, int width, int height, int bitrateKbps,
                                     bool globalHeader, std::string* error, std::string* label = nullptr)
{
    if (name == "h264_mf") {
        if (auto ctx = openVideoEncoderWith(name, rate, width, height, bitrateKbps, globalHeader, true, error)) {
            if (label)
                *label = "h264_mf (hardware)";
            return ctx;
        }
    }
    auto ctx = openVideoEncoderWith(name, rate, width, height, bitrateKbps, globalHeader, false, error);
    if (ctx && label)
        *label = name;
    return ctx;
}

int swsCoefficientsFor(ColorMatrix m)
{
    switch (m) {
    case ColorMatrix::BT601:
        return SWS_CS_ITU601;
    case ColorMatrix::BT2020:
        return SWS_CS_BT2020;
    case ColorMatrix::BT709:
    default:
        return SWS_CS_ITU709;
    }
}

} // namespace

std::vector<std::string> h264EncoderPreference()
{
#if defined(_WIN32)
    return {"h264_nvenc", "h264_amf", "h264_qsv", "libx264", "h264_mf", "libopenh264"};
#elif defined(__APPLE__)
    return {"h264_videotoolbox", "libx264", "libopenh264"};
#else
    return {"h264_nvenc", "libx264", "libopenh264"};
#endif
}

std::vector<std::string> availableH264Encoders(bool usableOnly)
{
    ff::installLogBridge();
    return runInEncoderThread([&] {
        std::vector<std::string> out;
        for (const std::string& name : h264EncoderPreference()) {
            if (!avcodec_find_encoder_by_name(name.c_str()))
                continue;
            if (usableOnly) {
                std::string error;
                if (!openVideoEncoder(name, Rate{30, 1}, 256, 144, 1000, false, &error))
                    continue;
            }
            out.push_back(name);
        }
        return out;
    });
}

void removeFileQuietly(const std::string& path)
{
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
}

// ---------------------------------------------------------------------------------------------
// Mp4Writer
// ---------------------------------------------------------------------------------------------

Mp4Writer::~Mp4Writer()
{
    if (!m_oc)
        return;
    if (!(m_oc->oformat->flags & AVFMT_NOFILE) && m_oc->pb)
        avio_closep(&m_oc->pb);
    avformat_free_context(m_oc);
}

bool Mp4Writer::open(const std::string& path, Rate rate, int width, int height, const std::string& encoder,
                     int videoKbps, bool withAudio, int audioKbps, std::string* error)
{
    ff::installLogBridge();
    int r = avformat_alloc_output_context2(&m_oc, nullptr, "mp4", path.c_str());
    if (r < 0 || !m_oc) {
        *error = "Cannot create MP4 muxer: " + ff::errorString(r);
        return false;
    }
    const bool globalHeader = (m_oc->oformat->flags & AVFMT_GLOBALHEADER) != 0;

    std::vector<std::string> candidates;
    if (encoder.empty() || encoder == "auto")
        candidates = h264EncoderPreference();
    else
        candidates = {encoder};
    std::string lastError;
    for (const std::string& name : candidates) {
        if (!avcodec_find_encoder_by_name(name.c_str()))
            continue;
        m_venc = openVideoEncoder(name, rate, width, height, videoKbps, globalHeader, &lastError, &m_encoderLabel);
        if (m_venc)
            break;
        logInfo("Encoder ", name, " unavailable: ", lastError);
    }
    if (!m_venc) {
        *error = "No usable H.264 encoder" + (lastError.empty() ? std::string() : " (" + lastError + ")");
        return false;
    }
    m_vst = avformat_new_stream(m_oc, nullptr);
    avcodec_parameters_from_context(m_vst->codecpar, m_venc.get());
    m_vst->time_base = m_venc->time_base;
    m_vst->avg_frame_rate = m_venc->framerate;

    if (withAudio) {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!codec) {
            *error = "AAC encoder missing from this FFmpeg build";
            return false;
        }
        m_aenc.reset(avcodec_alloc_context3(codec));
        m_aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
        m_aenc->sample_rate = kEncodeAudioRate;
        av_channel_layout_default(&m_aenc->ch_layout, 2);
        m_aenc->bit_rate = std::int64_t(audioKbps) * 1000;
        m_aenc->time_base = AVRational{1, kEncodeAudioRate};
        if (globalHeader)
            m_aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        r = avcodec_open2(m_aenc.get(), codec, nullptr);
        if (r < 0) {
            *error = "Cannot open AAC encoder: " + ff::errorString(r);
            return false;
        }
        m_ast = avformat_new_stream(m_oc, nullptr);
        avcodec_parameters_from_context(m_ast->codecpar, m_aenc.get());
        m_ast->time_base = m_aenc->time_base;
        m_fifo.reset(av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, 8192));
    }

    if (!(m_oc->oformat->flags & AVFMT_NOFILE)) {
        r = avio_open(&m_oc->pb, path.c_str(), AVIO_FLAG_WRITE);
        if (r < 0) {
            *error = "Cannot write " + path + ": " + ff::errorString(r);
            return false;
        }
    }
    AVDictionary* muxOpts = nullptr;
    av_dict_set(&muxOpts, "movflags", "+faststart", 0);
    r = avformat_write_header(m_oc, &muxOpts);
    av_dict_free(&muxOpts);
    if (r < 0) {
        *error = "Cannot write MP4 header: " + ff::errorString(r);
        return false;
    }

    m_frame = ff::makeFrame();
    m_frame->format = m_venc->pix_fmt;
    m_frame->width = width;
    m_frame->height = height;
    m_frame->color_range = AVCOL_RANGE_MPEG;
    m_frame->colorspace = AVCOL_SPC_BT709;
    m_frame->color_primaries = AVCOL_PRI_BT709;
    m_frame->color_trc = AVCOL_TRC_BT709;
    if (av_frame_get_buffer(m_frame.get(), 0) < 0) {
        *error = "Out of memory";
        return false;
    }
    return true;
}

AVFrame* Mp4Writer::beginVideoFrame()
{
    if (av_frame_make_writable(m_frame.get()) < 0)
        return nullptr;
    const ptrdiff_t lines[4] = {m_frame->linesize[0], m_frame->linesize[1], m_frame->linesize[2], m_frame->linesize[3]};
    av_image_fill_black(m_frame->data, lines, AVPixelFormat(m_frame->format), AVCOL_RANGE_MPEG, m_frame->width,
                        m_frame->height);
    return m_frame.get();
}

int Mp4Writer::writeVideoFrame(std::int64_t frameIndex)
{
    m_frame->pts = frameIndex;
    return encode(m_venc.get(), m_vst, m_frame.get());
}

int Mp4Writer::writeAudio(const float* interleaved, int frames)
{
    if (frames > 0) {
        m_left.resize(std::size_t(frames));
        m_right.resize(std::size_t(frames));
        for (int i = 0; i < frames; ++i) {
            m_left[std::size_t(i)] = interleaved[std::size_t(i) * 2];
            m_right[std::size_t(i)] = interleaved[std::size_t(i) * 2 + 1];
        }
        void* planes[2] = {m_left.data(), m_right.data()};
        av_audio_fifo_write(m_fifo.get(), planes, frames);
    }
    return drainAudio(false);
}

bool Mp4Writer::finish(std::string* error)
{
    int r = 0;
    if (m_aenc) {
        r = drainAudio(true);
        if (r >= 0)
            r = encode(m_aenc.get(), m_ast, nullptr);
        if (r < 0) {
            *error = "Audio encoding failed: " + ff::errorString(r);
            return false;
        }
    }
    r = encode(m_venc.get(), m_vst, nullptr);
    if (r < 0) {
        *error = "Video encoding failed: " + ff::errorString(r);
        return false;
    }
    r = av_write_trailer(m_oc);
    if (r < 0) {
        *error = "Cannot finalize MP4: " + ff::errorString(r);
        return false;
    }
    return true;
}

int Mp4Writer::encode(AVCodecContext* enc, AVStream* st, AVFrame* frame)
{
    int r = avcodec_send_frame(enc, frame);
    if (r < 0)
        return r;
    for (;;) {
        r = avcodec_receive_packet(enc, m_packet.get());
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
            return 0;
        if (r < 0)
            return r;
        av_packet_rescale_ts(m_packet.get(), enc->time_base, st->time_base);
        m_packet->stream_index = st->index;
        r = av_interleaved_write_frame(m_oc, m_packet.get());
        if (r < 0)
            return r;
    }
}

int Mp4Writer::drainAudio(bool flushAll)
{
    const int frameSize = m_aenc->frame_size > 0 ? m_aenc->frame_size : 1024;
    while (av_audio_fifo_size(m_fifo.get()) >= frameSize || (flushAll && av_audio_fifo_size(m_fifo.get()) > 0)) {
        const int n = std::min(frameSize, av_audio_fifo_size(m_fifo.get()));
        ff::FramePtr af = ff::makeFrame();
        af->nb_samples = n;
        af->format = AV_SAMPLE_FMT_FLTP;
        af->sample_rate = kEncodeAudioRate;
        av_channel_layout_copy(&af->ch_layout, &m_aenc->ch_layout);
        int r = av_frame_get_buffer(af.get(), 0);
        if (r < 0)
            return r;
        av_audio_fifo_read(m_fifo.get(), reinterpret_cast<void**>(af->data), n);
        af->pts = m_audioPts;
        m_audioPts += n;
        r = encode(m_aenc.get(), m_ast, af.get());
        if (r < 0)
            return r;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// FrameScaler
// ---------------------------------------------------------------------------------------------

void FrameScaler::scaleInto(const VideoFrame& src, AVFrame* dst)
{
    const double srcAspect = double(src.width) / double(src.height);
    const double dstAspect = double(dst->width) / double(dst->height);
    int w = dst->width;
    int h = dst->height;
    if (srcAspect > dstAspect)
        h = int(std::lround(dst->width / srcAspect)) & ~1;
    else
        w = int(std::lround(dst->height * srcAspect)) & ~1;
    w = std::max(2, w);
    h = std::max(2, h);
    const int x = ((dst->width - w) / 2) & ~1;
    const int y = ((dst->height - h) / 2) & ~1;

    const AVPixelFormat srcFormat = src.layout == PixelLayout::YUV420P16 ? AV_PIX_FMT_YUV420P16 : AV_PIX_FMT_YUV420P;
    const auto dstFormat = AVPixelFormat(dst->format);
    const bool rebuild = !m_sws || src.width != m_srcW || src.height != m_srcH || srcFormat != m_srcFormat
        || w != m_dstW || h != m_dstH || src.fullRange != m_fullRange || src.matrix != m_matrix;
    if (rebuild) {
        m_sws.reset(sws_getContext(src.width, src.height, srcFormat, w, h, dstFormat, SWS_BICUBIC, nullptr, nullptr,
                                   nullptr));
        if (!m_sws)
            return;
        int* invTable = nullptr;
        int* table = nullptr;
        int srcRange = 0, dstRange = 0, brightness = 0, contrast = 0, saturation = 0;
        sws_getColorspaceDetails(m_sws.get(), &invTable, &srcRange, &table, &dstRange, &brightness, &contrast,
                                 &saturation);
        sws_setColorspaceDetails(m_sws.get(), sws_getCoefficients(swsCoefficientsFor(src.matrix)),
                                 src.fullRange ? 1 : 0, sws_getCoefficients(SWS_CS_ITU709), 0, brightness, contrast,
                                 saturation);
        m_srcW = src.width;
        m_srcH = src.height;
        m_srcFormat = srcFormat;
        m_dstW = w;
        m_dstH = h;
        m_fullRange = src.fullRange;
        m_matrix = src.matrix;
    }

    // Point each destination plane at the letterbox origin.
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(dstFormat);
    int step[4] = {1, 1, 1, 1};
    for (int c = 0; c < desc->nb_components; ++c)
        step[desc->comp[c].plane] = desc->comp[c].step;
    std::uint8_t* dstData[4] = {};
    int dstLines[4] = {};
    for (int p = 0; p < 4 && dst->data[p]; ++p) {
        const int sx = p == 0 ? 0 : desc->log2_chroma_w;
        const int sy = p == 0 ? 0 : desc->log2_chroma_h;
        dstData[p] = dst->data[p] + std::ptrdiff_t(y >> sy) * dst->linesize[p] + std::ptrdiff_t(x >> sx) * step[p];
        dstLines[p] = dst->linesize[p];
    }
    const std::uint8_t* srcData[4] = {src.plane(0), src.plane(1), src.plane(2), nullptr};
    const int srcLines[4] = {src.stride[0], src.stride[1], src.stride[2], 0};
    sws_scale(m_sws.get(), srcData, srcLines, 0, src.height, dstData, dstLines);
}

} // namespace ve
