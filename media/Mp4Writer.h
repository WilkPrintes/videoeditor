#pragma once

// H.264 + AAC into MP4, plus the frame scaler that feeds it. Internal to the media/engine layers
// (exposes FFmpeg types). Must be used on an encoder thread (media/EncoderThread.h).

#include "core/Time.h"
#include "media/FFmpegUtil.h"
#include "media/VideoFrame.h"

#include <string>
#include <vector>

namespace ve {

inline constexpr int kEncodeAudioRate = 48000;

// "auto" order: hardware encoders first, then x264 (GPL builds only), Media Foundation, OpenH264.
std::vector<std::string> h264EncoderPreference();

class Mp4Writer {
public:
    Mp4Writer() = default;
    ~Mp4Writer();
    Mp4Writer(const Mp4Writer&) = delete;
    Mp4Writer& operator=(const Mp4Writer&) = delete;

    bool open(const std::string& path, Rate rate, int width, int height, const std::string& encoder,
              int videoKbps, bool withAudio, int audioKbps, std::string* error);

    const std::string& encoderLabel() const { return m_encoderLabel; }
    bool hasAudio() const { return m_aenc != nullptr; }

    // A writable picture in the encoder's pixel format, cleared to black.
    AVFrame* beginVideoFrame();
    int writeVideoFrame(std::int64_t frameIndex);
    // Interleaved stereo float at kEncodeAudioRate; encoded in AAC frames as soon as enough is buffered.
    int writeAudio(const float* interleaved, int frames);
    bool finish(std::string* error);

private:
    int encode(AVCodecContext* enc, AVStream* st, AVFrame* frame);
    int drainAudio(bool flushAll);

    AVFormatContext* m_oc = nullptr;
    ff::CodecContextPtr m_venc;
    ff::CodecContextPtr m_aenc;
    AVStream* m_vst = nullptr;
    AVStream* m_ast = nullptr;
    ff::FramePtr m_frame;
    ff::PacketPtr m_packet = ff::makePacket();
    ff::AudioFifoPtr m_fifo;
    std::vector<float> m_left;
    std::vector<float> m_right;
    std::int64_t m_audioPts = 0;
    std::string m_encoderLabel;
};

// Scales decoded frames into an encoder frame: aspect-fit with letterbox, limited range BT.709.
class FrameScaler {
public:
    void scaleInto(const VideoFrame& src, AVFrame* dst);

private:
    ff::SwsPtr m_sws;
    int m_srcW = 0, m_srcH = 0, m_dstW = 0, m_dstH = 0;
    AVPixelFormat m_srcFormat = AV_PIX_FMT_NONE;
    bool m_fullRange = false;
    ColorMatrix m_matrix = ColorMatrix::BT709;
};

// Removes a file (UTF-8 path), ignoring errors. Used to drop truncated outputs.
void removeFileQuietly(const std::string& path);

} // namespace ve
