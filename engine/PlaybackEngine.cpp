#include "engine/PlaybackEngine.h"

#include "core/Log.h"
#include "audio/AudioMixer.h"
#include "audio/AudioOutput.h"
#include "media/VideoDecoder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace ve {

namespace {

using FrameIndex = std::int64_t; // timeline frame number (the engine works on the frame grid)

constexpr int kSampleRate = 48000;
constexpr std::size_t kVideoQueueDepth = 8;
constexpr int kAudioChunkFrames = 1024;
constexpr int kAudioTargetFillFrames = kSampleRate * 150 / 1000; // keep ~150 ms queued
constexpr int kAudioPrefillFrames = kSampleRate * 60 / 1000;     // before the device starts

struct QueuedFrame {
    FrameIndex frame = 0;
    VideoFramePtr image;
};

// Video decoders owned by the video thread, one per media file.
class VideoSourceCache {
public:
    VideoFramePtr render(const TimelineSnapshot& snap, FrameIndex frame)
    {
        const auto source = resolveVideo(snap, timeOfFrame(frame, snap.timeline.rate));
        if (!source)
            return nullptr;
        VideoDecoder* dec = decoderFor(*source->media);
        return dec ? dec->frameAt(source->sourceTime) : nullptr;
    }

private:
    VideoDecoder* decoderFor(const MediaItem& media)
    {
        auto it = m_decoders.find(media.id);
        if (it != m_decoders.end())
            return it->second.get();
        if (m_failed.count(media.id))
            return nullptr;
        auto dec = std::make_unique<VideoDecoder>();
        std::string error;
        if (!dec->open(media.path, {}, &error)) {
            logWarning("Video unavailable for ", media.name, ": ", error);
            m_failed.insert(media.id);
            return nullptr;
        }
        const std::string hw = dec->hardwareName();
        logInfo("Video decoder for ", media.name, ": ", hw.empty() ? std::string("software") : hw);
        return m_decoders.emplace(media.id, std::move(dec)).first->second.get();
    }

    std::map<Id, std::unique_ptr<VideoDecoder>> m_decoders;
    std::set<Id> m_failed;
};

} // namespace

struct PlaybackEngine::Impl {
    // ---- state shared with the video thread (guarded by `mutex`) ----
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> quit{false};

    TimelineSnapshotPtr snapshot;
    FrameIndex duration = 0;
    Rate rate{30, 1};

    bool playing = false;
    bool clockRunning = false;
    FrameIndex position = 0;  // playhead while paused
    FrameIndex playStart = 0; // timeline frame where the clock started
    std::uint64_t generation = 0;
    bool useAudioClock = false;
    std::chrono::steady_clock::time_point systemStart;

    std::deque<QueuedFrame> queue;
    FrameIndex nextDecode = 0;

    std::optional<FrameIndex> pendingStill;
    bool stillReady = false;
    FrameIndex stillFrame = -1;
    VideoFramePtr stillImage;

    VideoFramePtr displayed;
    FrameIndex displayedFrame = -1;
    Stats stats;

    // ---- audio side (guarded by `audioMutex`) ----
    AudioOutput audio;
    bool audioAvailable = false;
    std::mutex audioMutex;
    AudioMixer mixer{kSampleRate};
    TimelineSnapshotPtr audioSnapshot;
    bool audioActive = false;
    std::int64_t audioPos = 0; // next timeline sample to mix
    std::vector<float> audioBuffer;

    std::thread videoThread;
    std::thread audioThread;

    FrameIndex clockFrameLocked() const
    {
        if (!playing)
            return position;
        if (!clockRunning)
            return playStart;
        Time elapsed;
        if (useAudioClock) {
            // Exact: samples actually played (minus device buffering) on the timeline's frame grid.
            const std::int64_t played = audio.framesPlayed() - audio.latencyFrames();
            elapsed = timeOfSample(std::max<std::int64_t>(played, 0), kSampleRate);
        } else {
            elapsed = timeFromSeconds(
                std::chrono::duration<double>(std::chrono::steady_clock::now() - systemStart).count());
        }
        return playStart + frameAt(elapsed, rate);
    }

    FrameIndex lastFrame() const { return std::max<FrameIndex>(duration - 1, 0); }

    // ---------------------------------------------------------------- video thread
    void videoLoop()
    {
        VideoSourceCache cache;
        std::unique_lock lock(mutex);
        while (!quit) {
            if (pendingStill && snapshot) {
                const FrameIndex frame = *pendingStill;
                pendingStill.reset();
                const TimelineSnapshotPtr snap = snapshot;
                lock.unlock();
                VideoFramePtr image = cache.render(*snap, frame);
                lock.lock();
                if (!playing) {
                    stillImage = std::move(image);
                    stillFrame = frame;
                    stillReady = true;
                }
                continue;
            }
            if (playing && snapshot && queue.size() < kVideoQueueDepth && nextDecode < duration) {
                const FrameIndex frame = nextDecode++;
                const std::uint64_t gen = generation;
                const TimelineSnapshotPtr snap = snapshot;
                lock.unlock();
                VideoFramePtr image = cache.render(*snap, frame);
                lock.lock();
                if (gen == generation && playing) {
                    queue.push_back({frame, std::move(image)});
                    cv.notify_all();
                }
                continue;
            }
            cv.wait(lock);
        }
    }

    // ---------------------------------------------------------------- audio thread
    void mixChunkLocked(int frames)
    {
        audioBuffer.resize(std::size_t(frames) * 2);
        mixer.mix(*audioSnapshot, audioPos, frames, audioBuffer.data());
        audio.ring().write(audioBuffer.data(), std::size_t(frames));
        audioPos += frames;
    }

    void audioLoop()
    {
        while (!quit) {
            {
                std::lock_guard lock(audioMutex);
                StereoRing& ring = audio.ring();
                if (audioActive && audioSnapshot && ring.writable() >= std::size_t(kAudioChunkFrames)
                    && ring.readable() < std::size_t(kAudioTargetFillFrames)) {
                    mixChunkLocked(kAudioChunkFrames);
                    continue;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    }

    bool startAudio(FrameIndex from, const TimelineSnapshotPtr& snap)
    {
        if (!audioAvailable)
            return false;
        std::lock_guard lock(audioMutex);
        audio.stop();
        audio.ring().reset();
        audio.resetCounters();
        audioSnapshot = snap;
        audioPos = sampleAt(timeOfFrame(from, snap->timeline.rate), kSampleRate);
        audioActive = true;
        for (int done = 0; done < kAudioPrefillFrames; done += kAudioChunkFrames)
            mixChunkLocked(kAudioChunkFrames);
        audio.start();
        return true;
    }

    void stopAudio()
    {
        std::lock_guard lock(audioMutex);
        audioActive = false;
        audio.stop();
    }

    // ---------------------------------------------------------------- transport
    void playFrom(FrameIndex from)
    {
        TimelineSnapshotPtr snap;
        {
            std::lock_guard lock(mutex);
            if (playing || duration <= 0 || !snapshot)
                return;
            from = std::clamp<FrameIndex>(from, 0, lastFrame());
            playing = true;
            clockRunning = false;
            playStart = from;
            position = from;
            ++generation;
            queue.clear();
            nextDecode = from;
            pendingStill.reset();
            stillReady = false;
            snap = snapshot;
        }
        cv.notify_all();

        // Let the video thread get a couple of frames ahead before the clock starts.
        {
            std::unique_lock lock(mutex);
            cv.wait_for(lock, std::chrono::milliseconds(500),
                        [&] { return queue.size() >= 2 || nextDecode >= duration || !playing; });
        }
        const bool audioClock = startAudio(from, snap);
        {
            std::lock_guard lock(mutex);
            useAudioClock = audioClock;
            systemStart = std::chrono::steady_clock::now();
            clockRunning = true;
        }
    }

    void stop()
    {
        {
            std::lock_guard lock(mutex);
            if (!playing)
                return;
            position = std::clamp<FrameIndex>(clockFrameLocked(), 0, lastFrame());
            playing = false;
            clockRunning = false;
            ++generation;
            queue.clear();
            pendingStill = position; // show exactly the frame we stopped on
        }
        stopAudio();
        cv.notify_all();
    }
};

PlaybackEngine::PlaybackEngine()
    : d(std::make_unique<Impl>())
{
    std::string error;
    d->audioAvailable = d->audio.open(kSampleRate, &error);
    if (!d->audioAvailable)
        logWarning(error, " - playback will use the system clock");
    d->stats.audioDevice = d->audio.deviceName();
    d->videoThread = std::thread([impl = d.get()] { impl->videoLoop(); });
    d->audioThread = std::thread([impl = d.get()] { impl->audioLoop(); });
}

PlaybackEngine::~PlaybackEngine()
{
    {
        std::lock_guard lock(d->mutex);
        d->quit = true;
    }
    d->cv.notify_all();
    d->videoThread.join();
    d->audioThread.join();
    d->audio.stop();
}

void PlaybackEngine::setTimeline(TimelineSnapshotPtr snapshot)
{
    bool wasPlaying = false;
    FrameIndex resumeAt = 0;
    {
        std::lock_guard lock(d->mutex);
        wasPlaying = d->playing;
        resumeAt = d->clockFrameLocked();
        d->snapshot = std::move(snapshot);
        d->rate = d->snapshot ? d->snapshot->timeline.rate : Rate{};
        d->duration = d->snapshot ? frameAt(d->snapshot->timeline.duration(), d->rate) : 0;
        if (!d->playing) {
            d->position = std::clamp<FrameIndex>(d->position, 0, d->lastFrame());
            d->pendingStill = d->position; // the edit may have changed the picture under the playhead
            ++d->generation;
        }
    }
    d->cv.notify_all();
    if (wasPlaying) {
        d->stop();
        d->playFrom(resumeAt);
    }
}

void PlaybackEngine::play()
{
    FrameIndex from = 0;
    {
        std::lock_guard lock(d->mutex);
        from = d->position >= d->lastFrame() ? 0 : d->position; // at the end: start over
    }
    d->playFrom(from);
}

void PlaybackEngine::pause()
{
    d->stop();
}

bool PlaybackEngine::isPlaying() const
{
    std::lock_guard lock(d->mutex);
    return d->playing;
}

void PlaybackEngine::seek(Time t)
{
    FrameIndex frame = 0;
    {
        std::lock_guard lock(d->mutex);
        frame = frameAt(t, d->rate);
    }
    if (isPlaying()) {
        d->stop();
        d->playFrom(frame);
        return;
    }
    {
        std::lock_guard lock(d->mutex);
        d->position = std::clamp<FrameIndex>(frame, 0, d->lastFrame());
        d->pendingStill = d->position;
        ++d->generation;
    }
    d->cv.notify_all();
}

Time PlaybackEngine::position() const
{
    std::lock_guard lock(d->mutex);
    return timeOfFrame(std::min(d->clockFrameLocked(), d->lastFrame()), d->rate);
}

bool PlaybackEngine::reachedEnd() const
{
    std::lock_guard lock(d->mutex);
    return d->playing && d->clockRunning && d->clockFrameLocked() >= d->duration;
}

VideoFramePtr PlaybackEngine::currentVideoFrame(bool* changed)
{
    std::lock_guard lock(d->mutex);
    *changed = false;
    if (d->playing) {
        const FrameIndex pos = d->clockFrameLocked();
        std::optional<QueuedFrame> pick;
        while (!d->queue.empty() && d->queue.front().frame <= pos) {
            if (pick)
                ++d->stats.framesDropped;
            pick = std::move(d->queue.front());
            d->queue.pop_front();
        }
        if (pick && pick->frame != d->displayedFrame) {
            d->displayed = std::move(pick->image);
            d->displayedFrame = pick->frame;
            ++d->stats.framesShown;
            *changed = true;
        }
        if (d->queue.empty() && d->nextDecode <= pos && pos < d->duration) {
            // The decoder fell behind the clock: skip ahead instead of showing stale frames.
            d->stats.framesDropped += pos + 1 - d->nextDecode;
            d->nextDecode = pos + 1;
        }
        d->cv.notify_all();
    } else if (d->stillReady) {
        d->stillReady = false;
        d->displayed = d->stillImage;
        d->displayedFrame = d->stillFrame;
        *changed = true;
    }
    return d->displayed;
}

PlaybackEngine::Stats PlaybackEngine::stats() const
{
    std::lock_guard lock(d->mutex);
    Stats s = d->stats;
    s.audioClock = d->useAudioClock;
    s.audioUnderrunFrames = d->audio.underrunFrames();
    return s;
}

} // namespace ve
