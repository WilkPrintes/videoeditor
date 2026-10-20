#include "media/AudioDecoder.h"

#include "core/Log.h"
#include "media/FFmpegUtil.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace ve {

namespace {
constexpr std::int64_t kNoPos = std::numeric_limits<std::int64_t>::min();
} // namespace

struct AudioDecoder::Impl {
    std::string path;
    ff::InputFormatPtr fmt;
    ff::CodecContextPtr dec;
    int streamIndex = -1;
    AVRational timeBase{1, 1};
    Time startOffset; // container start (media time zero)
    int outRate = 48000;

    ff::PacketPtr packet = ff::makePacket();
    ff::FramePtr frame = ff::makeFrame();
    ff::SwrPtr swr;
    AVChannelLayout swrInLayout{};
    int swrInRate = 0;
    int swrInFormat = -1;

    // Decoded, resampled samples. fifoStart is the output-rate sample index of fifo[fifoRead].
    std::vector<float> fifo;
    std::size_t fifoRead = 0;
    std::int64_t fifoStart = kNoPos;
    std::int64_t mediaEnd = kNoPos; // learned the first time decoding reaches the end
    bool inputDone = false;
    bool decoderDone = false;
    bool neverDecoded = true;

    ~Impl() { av_channel_layout_uninit(&swrInLayout); }

    std::size_t available() const { return (fifo.size() - fifoRead) / 2; }

    void seek(std::int64_t sample)
    {
        const Time t = timeOfSample(std::max<std::int64_t>(0, sample), outRate);
        const std::int64_t ts = ff::fromTime(t + startOffset, timeBase);
        int r = av_seek_frame(fmt.get(), streamIndex, ts, AVSEEK_FLAG_BACKWARD);
        if (r < 0)
            r = av_seek_frame(fmt.get(), streamIndex, ts, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
        if (r < 0)
            logWarning("Audio seek failed in ", path, ": ", ff::errorString(r));
        avcodec_flush_buffers(dec.get());
        fifo.clear();
        fifoRead = 0;
        fifoStart = kNoPos;
        inputDone = false;
        decoderDone = false;
        swr.reset(); // drop resampler history
    }

    bool ensureResampler(const AVFrame* f)
    {
        AVChannelLayout in{};
        if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || f->ch_layout.nb_channels == 0) {
            const int channels = f->ch_layout.nb_channels > 0 ? f->ch_layout.nb_channels : dec->ch_layout.nb_channels;
            av_channel_layout_default(&in, std::max(1, channels));
        } else {
            av_channel_layout_copy(&in, &f->ch_layout);
        }
        const bool same = swr && av_channel_layout_compare(&in, &swrInLayout) == 0
            && f->sample_rate == swrInRate && f->format == swrInFormat;
        if (same) {
            av_channel_layout_uninit(&in);
            return true;
        }

        AVChannelLayout stereo{};
        av_channel_layout_default(&stereo, 2);
        SwrContext* s = nullptr;
        int r = swr_alloc_set_opts2(&s, &stereo, AV_SAMPLE_FMT_FLT, outRate, &in,
                                    AVSampleFormat(f->format), f->sample_rate, 0, nullptr);
        if (r >= 0)
            r = swr_init(s);
        if (r < 0) {
            swr_free(&s);
            av_channel_layout_uninit(&in);
            logWarning("Cannot create resampler for ", path, ": ", ff::errorString(r));
            return false;
        }
        swr.reset(s);
        av_channel_layout_uninit(&swrInLayout);
        swrInLayout = in; // takes ownership
        swrInRate = f->sample_rate;
        swrInFormat = f->format;
        return true;
    }

    void appendConverted(const std::uint8_t** in, int inSamples, int maxOut)
    {
        if (maxOut <= 0)
            return;
        const std::size_t old = fifo.size();
        fifo.resize(old + std::size_t(maxOut) * 2);
        auto* outPtr = reinterpret_cast<std::uint8_t*>(fifo.data() + old);
        const int got = swr_convert(swr.get(), &outPtr, maxOut, in, inSamples);
        fifo.resize(old + std::size_t(std::max(got, 0)) * 2);
    }

    void appendFrame(const AVFrame* f)
    {
        if (!ensureResampler(f))
            return;
        if (fifoStart == kNoPos) {
            std::int64_t ts = f->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE)
                ts = f->pts;
            const Time t = ts == AV_NOPTS_VALUE ? Time(0) : ff::toTime(ts, timeBase) - startOffset;
            fifoStart = sampleAt(t, outRate);
        }
        appendConverted(const_cast<const std::uint8_t**>(f->extended_data), f->nb_samples,
                        swr_get_out_samples(swr.get(), f->nb_samples));
    }

    void markEnd()
    {
        decoderDone = true;
        if (swr && fifoStart != kNoPos)
            appendConverted(nullptr, 0, swr_get_out_samples(swr.get(), 0)); // drain the resampler
        if (mediaEnd == kNoPos && fifoStart != kNoPos)
            mediaEnd = fifoStart + std::int64_t(available());
    }

    // Decodes one more frame into the fifo. False once the stream is exhausted.
    bool decodeMore()
    {
        for (;;) {
            int r = avcodec_receive_frame(dec.get(), frame.get());
            if (r == 0) {
                neverDecoded = false;
                appendFrame(frame.get());
                av_frame_unref(frame.get());
                return true;
            }
            if (r == AVERROR_EOF || inputDone) {
                markEnd();
                return false;
            }
            if (r != AVERROR(EAGAIN)) {
                logWarning("Audio decode error in ", path, ": ", ff::errorString(r));
                markEnd();
                return false;
            }
            r = av_read_frame(fmt.get(), packet.get());
            if (r < 0) {
                avcodec_send_packet(dec.get(), nullptr);
                inputDone = true;
                // keep receiving: the decoder may still hold frames
                r = avcodec_receive_frame(dec.get(), frame.get());
                if (r == 0) {
                    appendFrame(frame.get());
                    av_frame_unref(frame.get());
                    return true;
                }
                markEnd();
                return false;
            }
            if (packet->stream_index == streamIndex) {
                r = avcodec_send_packet(dec.get(), packet.get());
                if (r < 0 && r != AVERROR(EAGAIN))
                    logWarning("Skipping bad audio packet in ", path, ": ", ff::errorString(r));
            }
            av_packet_unref(packet.get());
        }
    }
};

AudioDecoder::AudioDecoder() : d(std::make_unique<Impl>()) {}
AudioDecoder::~AudioDecoder() = default;

bool AudioDecoder::open(const std::string& path, int outputSampleRate, std::string* error)
{
    d = std::make_unique<Impl>();
    d->path = path;
    d->outRate = outputSampleRate;
    d->fmt = ff::openInput(path, error);
    if (!d->fmt)
        return false;
    d->streamIndex = av_find_best_stream(d->fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (d->streamIndex < 0) {
        if (error)
            *error = "No audio stream in " + path;
        d->fmt.reset();
        return false;
    }
    AVStream* st = d->fmt->streams[d->streamIndex];
    d->timeBase = st->time_base;
    d->startOffset = ff::containerStart(d->fmt.get());
    for (unsigned i = 0; i < d->fmt->nb_streams; ++i) {
        if (int(i) != d->streamIndex)
            d->fmt->streams[i]->discard = AVDISCARD_ALL;
    }

    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        if (error)
            *error = std::string("No decoder for ") + avcodec_get_name(st->codecpar->codec_id);
        d->fmt.reset();
        return false;
    }
    d->dec.reset(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(d->dec.get(), st->codecpar);
    d->dec->pkt_timebase = st->time_base;
    const int r = avcodec_open2(d->dec.get(), codec, nullptr);
    if (r < 0) {
        if (error)
            *error = "Cannot open audio decoder: " + ff::errorString(r);
        d->dec.reset();
        d->fmt.reset();
        return false;
    }
    return true;
}

bool AudioDecoder::isOpen() const
{
    return d->dec != nullptr;
}

void AudioDecoder::read(std::int64_t sampleIndex, float* out, int frames)
{
    if (frames <= 0)
        return;
    if (!d->dec) {
        std::fill(out, out + std::size_t(frames) * 2, 0.0f);
        return;
    }

    std::int64_t pos = sampleIndex;

    if (d->mediaEnd != kNoPos && pos >= d->mediaEnd) {
        std::fill(out, out + std::size_t(frames) * 2, 0.0f);
        return;
    }

    const bool discontinuous = d->fifoStart == kNoPos || pos < d->fifoStart
        || pos > d->fifoStart + std::int64_t(d->available()) + d->outRate / 2;
    if (discontinuous && !(d->neverDecoded && pos < d->outRate / 2))
        d->seek(pos);

    int filled = 0;
    while (filled < frames) {
        if (d->available() == 0) {
            if (d->decoderDone || !d->decodeMore())
                break;
            continue;
        }
        if (pos < d->fifoStart) { // silence before the first sample
            const int n = int(std::min<std::int64_t>(d->fifoStart - pos, frames - filled));
            std::fill(out + std::size_t(filled) * 2, out + std::size_t(filled + n) * 2, 0.0f);
            filled += n;
            pos += n;
            continue;
        }
        if (pos > d->fifoStart) { // drop samples before the requested position
            const std::int64_t drop = std::min<std::int64_t>(pos - d->fifoStart, std::int64_t(d->available()));
            d->fifoRead += std::size_t(drop) * 2;
            d->fifoStart += drop;
            continue;
        }
        const int n = int(std::min<std::int64_t>(std::int64_t(d->available()), frames - filled));
        std::memcpy(out + std::size_t(filled) * 2, d->fifo.data() + d->fifoRead, std::size_t(n) * 2 * sizeof(float));
        d->fifoRead += std::size_t(n) * 2;
        d->fifoStart += n;
        filled += n;
        pos += n;
    }
    if (filled < frames)
        std::fill(out + std::size_t(filled) * 2, out + std::size_t(frames) * 2, 0.0f);

    // Keep the fifo from growing without bound.
    if (d->fifoRead > std::size_t(d->outRate) * 2) {
        d->fifo.erase(d->fifo.begin(), d->fifo.begin() + std::ptrdiff_t(d->fifoRead));
        d->fifoRead = 0;
    }
}

} // namespace ve
