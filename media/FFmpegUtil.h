#pragma once

// Private to the engine: FFmpeg includes and RAII wrappers.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "core/Time.h"

#include <memory>
#include <string>

namespace ve::ff {

std::string errorString(int err);

double toSeconds(std::int64_t ts, AVRational tb);
std::int64_t fromSeconds(double seconds, AVRational tb);

// Exact conversions between stream timestamps and flicks (core/Time.h).
inline ve::Time toTime(std::int64_t ts, AVRational tb)
{
    return ve::Time(ve::mulDivFloor(ts, std::int64_t(tb.num) * ve::kFlicksPerSecond, tb.den));
}
inline std::int64_t fromTime(ve::Time t, AVRational tb)
{
    return ve::mulDivFloor(t.flicks, tb.den, std::int64_t(tb.num) * ve::kFlicksPerSecond);
}

struct InputFormatDeleter {
    void operator()(AVFormatContext* c) const { avformat_close_input(&c); }
};
struct CodecContextDeleter {
    void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
struct FrameDeleter {
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
struct BufferRefDeleter {
    void operator()(AVBufferRef* b) const { av_buffer_unref(&b); }
};
struct SwsDeleter {
    void operator()(SwsContext* s) const { sws_freeContext(s); }
};
struct SwrDeleter {
    void operator()(SwrContext* s) const { swr_free(&s); }
};
struct AudioFifoDeleter {
    void operator()(AVAudioFifo* f) const { av_audio_fifo_free(f); }
};

using InputFormatPtr = std::unique_ptr<AVFormatContext, InputFormatDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;
using AudioFifoPtr = std::unique_ptr<AVAudioFifo, AudioFifoDeleter>;

inline FramePtr makeFrame() { return FramePtr(av_frame_alloc()); }
inline PacketPtr makePacket() { return PacketPtr(av_packet_alloc()); }

// Opens a file for demuxing and reads stream info. Media time zero is the container start.
InputFormatPtr openInput(const std::string& path, std::string* error);
ve::Time containerStart(const AVFormatContext* fmt);

// The hardware decoder type for this platform (D3D11VA, VideoToolbox, VAAPI).
AVHWDeviceType platformHwDeviceType();

// Routes av_log into ve::log (errors only; VE_FFMPEG_LOG=1 for more). Idempotent.
void installLogBridge();

} // namespace ve::ff
