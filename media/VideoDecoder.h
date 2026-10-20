#pragma once

#include "core/Time.h"
#include "media/VideoFrame.h"

#include <memory>
#include <string>

namespace ve {

// Frame-accurate random access over one video stream. Sequential requests (playback) decode
// forward without seeking; jumps (scrubbing) seek to the previous keyframe and decode up to
// the requested frame. Uses D3D11VA / VideoToolbox when available, with software fallback.
// Not thread-safe: each engine thread owns its own decoders.
class VideoDecoder {
public:
    struct Options {
        bool allowHardware = true;
    };

    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool open(const std::string& path, Options options, std::string* error);
    bool isOpen() const;

    // The frame on screen at media time t (nearest frame), relative to the media start.
    // nullptr on failure.
    VideoFramePtr frameAt(Time t);

    Time frameDuration() const;
    // "d3d11va", "videotoolbox", ... or empty for software decoding.
    std::string hardwareName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace ve
