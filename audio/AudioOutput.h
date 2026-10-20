#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ve {

// Lock-free single-producer/single-consumer ring of interleaved stereo float frames.
class StereoRing {
public:
    explicit StereoRing(std::size_t capacityFrames);

    std::size_t readable() const;
    std::size_t writable() const;
    std::size_t write(const float* frames, std::size_t count); // producer thread
    std::size_t read(float* frames, std::size_t count);        // consumer thread
    void reset(); // only while neither side is running

private:
    std::vector<float> m_buffer;
    std::size_t m_capacity;
    std::size_t m_mask;
    std::atomic<std::uint64_t> m_read{0};
    std::atomic<std::uint64_t> m_write{0};
};

// Audio device (WASAPI / Core Audio via miniaudio). The device callback pulls from the ring;
// the count of real samples it has played is the playback master clock.
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    bool open(int sampleRate, std::string* error); // stereo float32
    bool isOpen() const;
    void start();
    void stop(); // synchronous: the callback is not running when this returns

    StereoRing& ring() { return *m_ring; }

    // Frames of real (non-underrun) audio handed to the device since the last reset.
    std::int64_t framesPlayed() const { return m_played.load(std::memory_order_acquire); }
    std::int64_t underrunFrames() const { return m_underrun.load(std::memory_order_relaxed); }
    void resetCounters(); // only while stopped
    // Approximate device buffering between the callback and the speaker, in frames.
    int latencyFrames() const;
    int sampleRate() const { return m_sampleRate; }
    std::string deviceName() const;

private:
    struct Impl;
    static void dataCallback(void* device, void* output, const void* input, unsigned frameCount);

    std::unique_ptr<Impl> d;
    std::unique_ptr<StereoRing> m_ring;
    std::atomic<std::int64_t> m_played{0};
    std::atomic<std::int64_t> m_underrun{0};
    int m_sampleRate = 48000;
    bool m_started = false;
};

} // namespace ve
