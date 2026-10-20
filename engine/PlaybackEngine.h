#pragma once

// Playback engine, decoupled from the UI (the same idea as Resolve's engine "managers" with
// their own threads and queues):
//
//   UI thread          setTimeline / play / pause / seek, polls currentVideoFrame() each tick
//   video thread       decodes timeline frames ahead into a bounded queue, or services the
//                      latest scrub request (latest wins) while paused
//   audio thread       mixes timeline audio into a lock-free ring
//   device callback    drains the ring; samples played = the master clock
//
// Video frames are presented by the audio clock, so picture follows sound. Without an audio
// device the clock falls back to std::chrono::steady_clock.

#include "core/Snapshot.h"
#include "media/VideoFrame.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ve {

class PlaybackEngine {
public:
    struct Stats {
        std::int64_t framesShown = 0;
        std::int64_t framesDropped = 0;   // decoded too late and skipped
        std::int64_t audioUnderrunFrames = 0;
        bool audioClock = false;
        std::string audioDevice;
    };

    PlaybackEngine();
    ~PlaybackEngine();
    PlaybackEngine(const PlaybackEngine&) = delete;
    PlaybackEngine& operator=(const PlaybackEngine&) = delete;

    // Called by the UI after every model change.
    void setTimeline(TimelineSnapshotPtr snapshot);

    void play();
    void pause();
    bool isPlaying() const;

    // Moves the playhead. While paused the exact frame is decoded (scrubbing); rapid calls
    // coalesce so only the newest position is decoded.
    void seek(Time t);
    // Start of the frame on screen (on the timeline's frame grid).
    Time position() const;
    // True once playback ran past the last frame (the UI then pauses).
    bool reachedEnd() const;

    // The picture for the current position. `changed` is set when it differs from the
    // previous call. nullptr means black (gap or nothing decoded).
    VideoFramePtr currentVideoFrame(bool* changed);

    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace ve
