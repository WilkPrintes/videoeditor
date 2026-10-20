#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace ve {

// Sample-accurate reader for one audio stream, resampled to interleaved stereo float at a fixed
// output rate. Contiguous reads stream forward; discontinuities seek. Not thread-safe.
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    bool open(const std::string& path, int outputSampleRate, std::string* error);
    bool isOpen() const;

    // Writes `frames` stereo frames (2 floats each) starting at output-rate sample `sampleIndex`
    // of media time (0 = media start). Silence before the first and after the last sample.
    void read(std::int64_t sampleIndex, float* out, int frames);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace ve
