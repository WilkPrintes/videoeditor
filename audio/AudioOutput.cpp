#include "audio/AudioOutput.h"

#include "core/Log.h"

#include <miniaudio.h>

#include <algorithm>
#include <cstring>

namespace ve {

// ---------------------------------------------------------------------------
// StereoRing
// ---------------------------------------------------------------------------

StereoRing::StereoRing(std::size_t capacityFrames)
{
    std::size_t cap = 1;
    while (cap < capacityFrames)
        cap <<= 1;
    m_capacity = cap;
    m_mask = cap - 1;
    m_buffer.resize(cap * 2);
}

std::size_t StereoRing::readable() const
{
    return std::size_t(m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire));
}

std::size_t StereoRing::writable() const
{
    return m_capacity - readable();
}

std::size_t StereoRing::write(const float* frames, std::size_t count)
{
    const std::uint64_t w = m_write.load(std::memory_order_relaxed);
    const std::uint64_t r = m_read.load(std::memory_order_acquire);
    count = std::min(count, m_capacity - std::size_t(w - r));
    const std::size_t at = std::size_t(w & m_mask);
    const std::size_t first = std::min(count, m_capacity - at);
    std::memcpy(m_buffer.data() + at * 2, frames, first * 2 * sizeof(float));
    std::memcpy(m_buffer.data(), frames + first * 2, (count - first) * 2 * sizeof(float));
    m_write.store(w + count, std::memory_order_release);
    return count;
}

std::size_t StereoRing::read(float* frames, std::size_t count)
{
    const std::uint64_t r = m_read.load(std::memory_order_relaxed);
    const std::uint64_t w = m_write.load(std::memory_order_acquire);
    count = std::min(count, std::size_t(w - r));
    const std::size_t at = std::size_t(r & m_mask);
    const std::size_t first = std::min(count, m_capacity - at);
    std::memcpy(frames, m_buffer.data() + at * 2, first * 2 * sizeof(float));
    std::memcpy(frames + first * 2, m_buffer.data(), (count - first) * 2 * sizeof(float));
    m_read.store(r + count, std::memory_order_release);
    return count;
}

void StereoRing::reset()
{
    m_read.store(0, std::memory_order_relaxed);
    m_write.store(0, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// AudioOutput
// ---------------------------------------------------------------------------

struct AudioOutput::Impl {
    ma_device device{};
    bool initialized = false;

    ~Impl()
    {
        if (initialized)
            ma_device_uninit(&device);
    }
};

AudioOutput::AudioOutput()
    : m_ring(std::make_unique<StereoRing>(32768))
{
}

AudioOutput::~AudioOutput()
{
    stop();
}

bool AudioOutput::open(int sampleRate, std::string* error)
{
    if (isOpen())
        return true;
    m_sampleRate = sampleRate;
    auto impl = std::make_unique<Impl>();

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = ma_uint32(sampleRate);
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.pUserData = this;
    cfg.dataCallback = [](ma_device* device, void* output, const void* input, ma_uint32 frameCount) {
        AudioOutput::dataCallback(device, output, input, frameCount);
    };

    const ma_result r = ma_device_init(nullptr, &cfg, &impl->device);
    if (r != MA_SUCCESS) {
        if (error)
            *error = std::string("Audio device unavailable: ") + ma_result_description(r);
        return false;
    }
    impl->initialized = true;
    d = std::move(impl);
    logInfo("Audio output: ", deviceName(), " @ ", d->device.playback.internalSampleRate,
            " Hz, ~", latencyFrames(), " frames of device buffering");
    return true;
}

bool AudioOutput::isOpen() const
{
    return d && d->initialized;
}

void AudioOutput::start()
{
    if (!isOpen() || m_started)
        return;
    const ma_result r = ma_device_start(&d->device);
    if (r != MA_SUCCESS) {
        logError("Cannot start audio device: ", ma_result_description(r));
        return;
    }
    m_started = true;
}

void AudioOutput::stop()
{
    if (!isOpen() || !m_started)
        return;
    ma_device_stop(&d->device);
    m_started = false;
}

void AudioOutput::resetCounters()
{
    m_played.store(0, std::memory_order_release);
    m_underrun.store(0, std::memory_order_relaxed);
}

int AudioOutput::latencyFrames() const
{
    if (!isOpen())
        return 0;
    const auto& pb = d->device.playback;
    double frames = double(pb.internalPeriodSizeInFrames) * double(pb.internalPeriods);
    if (pb.internalSampleRate != 0 && int(pb.internalSampleRate) != m_sampleRate)
        frames *= double(m_sampleRate) / double(pb.internalSampleRate);
    return int(frames);
}

std::string AudioOutput::deviceName() const
{
    return isOpen() ? std::string(d->device.playback.name) : std::string();
}

void AudioOutput::dataCallback(void* device, void* output, const void* /*input*/, unsigned frameCount)
{
    auto* self = static_cast<AudioOutput*>(static_cast<ma_device*>(device)->pUserData);
    auto* out = static_cast<float*>(output);
    const std::size_t got = self->m_ring->read(out, frameCount);
    if (got < frameCount) {
        std::fill(out + got * 2, out + std::size_t(frameCount) * 2, 0.0f);
        self->m_underrun.fetch_add(std::int64_t(frameCount - got), std::memory_order_relaxed);
    }
    self->m_played.fetch_add(std::int64_t(got), std::memory_order_release);
}

} // namespace ve
