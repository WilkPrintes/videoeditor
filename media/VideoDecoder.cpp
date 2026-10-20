#include "media/VideoDecoder.h"

#include "core/Log.h"
#include "media/FFmpegUtil.h"

#include <optional>

namespace ve {

namespace {

// Beyond this distance a forward request seeks instead of decoding through.
constexpr Time kSeekThreshold = Time::fromSeconds(2);

struct RawFrame {
    ff::FramePtr frame;
    Time time; // media time relative to the container start
};

ColorMatrix matrixFor(const AVFrame* f)
{
    switch (f->colorspace) {
    case AVCOL_SPC_BT709:
        return ColorMatrix::BT709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_FCC:
        return ColorMatrix::BT601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return ColorMatrix::BT2020;
    default:
        return f->height >= 720 ? ColorMatrix::BT709 : ColorMatrix::BT601;
    }
}

bool isYuvjFormat(AVPixelFormat f)
{
    return f == AV_PIX_FMT_YUVJ420P || f == AV_PIX_FMT_YUVJ422P || f == AV_PIX_FMT_YUVJ444P
        || f == AV_PIX_FMT_YUVJ440P || f == AV_PIX_FMT_YUVJ411P;
}

} // namespace

struct VideoDecoder::Impl {
    std::string path;
    ff::InputFormatPtr fmt;
    ff::CodecContextPtr dec;
    ff::BufferRefPtr hwDevice;
    AVPixelFormat hwFormat = AV_PIX_FMT_NONE;
    std::string hwName;
    int streamIndex = -1;
    AVRational timeBase{1, 1};
    Time startOffset;                        // container start (media time zero)
    Time frameDur = timeOfFrame(1, {30, 1});

    ff::PacketPtr packet = ff::makePacket();
    ff::FramePtr scratch = ff::makeFrame();
    ff::FramePtr swFrame = ff::makeFrame();
    ff::SwsPtr sws;
    int swsWidth = 0;
    int swsHeight = 0;
    int swsSrcFormat = -1;
    int swsDstFormat = -1;

    // Decode-ahead state: `current` is the last frame at or before the requested time,
    // `pending` the first frame after it (already decoded, kept for the next request).
    std::optional<RawFrame> current;
    std::optional<RawFrame> pending;
    VideoFramePtr currentConverted;
    bool inputDone = false;
    bool decoderDone = false;
    bool neverDecoded = true;
    Time lastTime;
    int hardErrors = 0;

    static AVPixelFormat getHwFormat(AVCodecContext* ctx, const AVPixelFormat* formats)
    {
        auto* self = static_cast<Impl*>(ctx->opaque);
        for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
            if (*p == self->hwFormat)
                return *p;
        }
        return avcodec_default_get_format(ctx, formats);
    }

    bool openCodec(bool useHardware, std::string* error)
    {
        const AVStream* st = fmt->streams[streamIndex];
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec) {
            if (error)
                *error = std::string("No decoder for ") + avcodec_get_name(st->codecpar->codec_id);
            return false;
        }
        dec.reset(avcodec_alloc_context3(codec));
        avcodec_parameters_to_context(dec.get(), st->codecpar);
        dec->pkt_timebase = st->time_base;
        dec->opaque = this;

        hwDevice.reset();
        hwFormat = AV_PIX_FMT_NONE;
        hwName.clear();
        if (useHardware) {
            const AVHWDeviceType type = ff::platformHwDeviceType();
            for (int i = 0;; ++i) {
                const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
                if (!cfg)
                    break;
                if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == type) {
                    hwFormat = cfg->pix_fmt;
                    break;
                }
            }
            AVBufferRef* device = nullptr;
            if (hwFormat != AV_PIX_FMT_NONE && av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0) == 0) {
                hwDevice.reset(device);
                dec->hw_device_ctx = av_buffer_ref(device);
                dec->get_format = &Impl::getHwFormat;
                dec->extra_hw_frames = 4;
                hwName = av_hwdevice_get_type_name(type);
            } else {
                hwFormat = AV_PIX_FMT_NONE;
            }
        }
        if (hwFormat == AV_PIX_FMT_NONE) {
            dec->thread_count = 0; // auto
            dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }

        const int r = avcodec_open2(dec.get(), codec, nullptr);
        if (r < 0) {
            if (error)
                *error = "Cannot open decoder: " + ff::errorString(r);
            dec.reset();
            return false;
        }
        hardErrors = 0;
        return true;
    }

    void seek(Time t)
    {
        const std::int64_t ts = ff::fromTime(t + startOffset, timeBase);
        int r = av_seek_frame(fmt.get(), streamIndex, ts, AVSEEK_FLAG_BACKWARD);
        if (r < 0)
            r = av_seek_frame(fmt.get(), streamIndex, ts, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
        if (r < 0)
            logWarning("Seek failed in ", path, ": ", ff::errorString(r));
        avcodec_flush_buffers(dec.get());
        current.reset();
        pending.reset();
        currentConverted.reset();
        inputDone = false;
        decoderDone = false;
    }

    bool decodeNext(RawFrame& out)
    {
        for (;;) {
            int r = avcodec_receive_frame(dec.get(), scratch.get());
            if (r == 0) {
                neverDecoded = false;
                std::int64_t ts = scratch->best_effort_timestamp;
                if (ts == AV_NOPTS_VALUE)
                    ts = scratch->pts;
                const Time t = ts == AV_NOPTS_VALUE ? lastTime + frameDur : ff::toTime(ts, timeBase) - startOffset;
                lastTime = t;
                out.frame = ff::makeFrame();
                av_frame_move_ref(out.frame.get(), scratch.get());
                out.time = t;
                return true;
            }
            if (r == AVERROR_EOF) {
                decoderDone = true;
                return false;
            }
            if (r != AVERROR(EAGAIN)) {
                logWarning("Decode error in ", path, ": ", ff::errorString(r));
                ++hardErrors;
                return false;
            }
            if (inputDone) {
                decoderDone = true;
                return false;
            }
            r = av_read_frame(fmt.get(), packet.get());
            if (r < 0) {
                avcodec_send_packet(dec.get(), nullptr); // drain
                inputDone = true;
                continue;
            }
            if (packet->stream_index == streamIndex) {
                r = avcodec_send_packet(dec.get(), packet.get());
                if (r < 0 && r != AVERROR(EAGAIN))
                    logWarning("Skipping bad packet in ", path, ": ", ff::errorString(r));
            }
            av_packet_unref(packet.get());
        }
    }

    VideoFramePtr convert(const RawFrame& raw)
    {
        AVFrame* src = raw.frame.get();
        if (hwFormat != AV_PIX_FMT_NONE && src->format == hwFormat) {
            av_frame_unref(swFrame.get());
            const int r = av_hwframe_transfer_data(swFrame.get(), src, 0);
            if (r < 0) {
                logWarning("GPU frame download failed: ", ff::errorString(r));
                ++hardErrors;
                return nullptr;
            }
            av_frame_copy_props(swFrame.get(), src);
            src = swFrame.get();
        }

        const auto srcFormat = AVPixelFormat(src->format);
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(srcFormat);
        if (!desc)
            return nullptr;
        const bool rgb = (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
        const PixelLayout layout = desc->comp[0].depth > 8 ? PixelLayout::YUV420P16 : PixelLayout::YUV420P8;
        const AVPixelFormat dstFormat = layout == PixelLayout::YUV420P16 ? AV_PIX_FMT_YUV420P16 : AV_PIX_FMT_YUV420P;

        auto out = std::make_shared<VideoFrame>();
        out->allocate(src->width, src->height, layout);
        out->sourceTime = raw.time;
        out->matrix = rgb ? ColorMatrix::BT601 : matrixFor(src);
        std::uint8_t* dstData[4] = {out->plane(0), out->plane(1), out->plane(2), nullptr};
        int dstLines[4] = {out->stride[0], out->stride[1], out->stride[2], 0};

        if (srcFormat == dstFormat) {
            for (int i = 0; i < 3; ++i) {
                av_image_copy_plane(dstData[i], dstLines[i], src->data[i], src->linesize[i],
                                    out->planeWidth[std::size_t(i)] * out->bytesPerSample(),
                                    out->planeHeight[std::size_t(i)]);
            }
            out->fullRange = src->color_range == AVCOL_RANGE_JPEG;
        } else {
            if (!sws || swsWidth != src->width || swsHeight != src->height || swsSrcFormat != srcFormat
                || swsDstFormat != dstFormat) {
                sws.reset(sws_getContext(src->width, src->height, srcFormat, src->width, src->height,
                                         dstFormat, SWS_BILINEAR, nullptr, nullptr, nullptr));
                swsWidth = src->width;
                swsHeight = src->height;
                swsSrcFormat = srcFormat;
                swsDstFormat = dstFormat;
            }
            if (!sws)
                return nullptr;
            sws_scale(sws.get(), src->data, src->linesize, 0, src->height, dstData, dstLines);
            // swscale converts YUVJ (and RGB) input to limited range; plain YUV passes through.
            out->fullRange = !rgb && !isYuvjFormat(srcFormat) && src->color_range == AVCOL_RANGE_JPEG;
        }
        return out;
    }

    VideoFramePtr decodeTo(Time t)
    {
        const Time target = t + Time(frameDur.flicks / 2); // nearest frame
        if (current) {
            const bool backwards = target < current->time;
            const bool farAhead = t - current->time > kSeekThreshold;
            if (backwards || (farAhead && !decoderDone))
                seek(t);
        } else if (!(neverDecoded && t < kSeekThreshold)) {
            seek(t);
        }

        for (;;) {
            if (!pending) {
                RawFrame f;
                if (!decodeNext(f))
                    break;
                pending = std::move(f);
            }
            if (pending->time <= target) {
                current = std::move(pending);
                pending.reset();
                currentConverted.reset();
                continue;
            }
            break;
        }
        if (!current) {
            // Requested time is before the first decodable frame: show that frame.
            if (!pending)
                return nullptr;
            current = std::move(pending);
            pending.reset();
            currentConverted.reset();
        }
        if (!currentConverted)
            currentConverted = convert(*current);
        return currentConverted;
    }
};

VideoDecoder::VideoDecoder() : d(std::make_unique<Impl>()) {}
VideoDecoder::~VideoDecoder() = default;

bool VideoDecoder::open(const std::string& path, Options options, std::string* error)
{
    d = std::make_unique<Impl>();
    d->path = path;
    d->fmt = ff::openInput(path, error);
    if (!d->fmt)
        return false;
    d->streamIndex = av_find_best_stream(d->fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (d->streamIndex < 0) {
        if (error)
            *error = "No video stream in " + path;
        d->fmt.reset();
        return false;
    }
    AVStream* st = d->fmt->streams[d->streamIndex];
    d->timeBase = st->time_base;
    d->startOffset = ff::containerStart(d->fmt.get());
    const AVRational rate = av_guess_frame_rate(d->fmt.get(), st, nullptr);
    if (rate.num > 0 && rate.den > 0)
        d->frameDur = timeOfFrame(1, Rate{rate.num, rate.den});
    for (unsigned i = 0; i < d->fmt->nb_streams; ++i) {
        if (int(i) != d->streamIndex)
            d->fmt->streams[i]->discard = AVDISCARD_ALL;
    }

    if (d->openCodec(options.allowHardware, error))
        return true;
    if (options.allowHardware && d->openCodec(false, error))
        return true;
    d->fmt.reset();
    return false;
}

bool VideoDecoder::isOpen() const
{
    return d->dec != nullptr;
}

VideoFramePtr VideoDecoder::frameAt(Time t)
{
    if (!d->dec)
        return nullptr;
    VideoFramePtr frame = d->decodeTo(t);
    if (!frame && d->hardErrors > 0 && d->hwFormat != AV_PIX_FMT_NONE) {
        logWarning("Hardware decoding failed for ", d->path, ", switching to software");
        std::string error;
        if (!d->openCodec(false, &error)) {
            logError(error);
            return nullptr;
        }
        d->seek(t);
        frame = d->decodeTo(t);
    }
    return frame;
}

Time VideoDecoder::frameDuration() const
{
    return d->frameDur;
}

std::string VideoDecoder::hardwareName() const
{
    return d->hwName;
}

} // namespace ve
