#pragma once

#include "core/Snapshot.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace ve {

class AudioDecoder;

// Renders the timeline's audio tracks to interleaved stereo float. Used by playback (in the
// audio producer thread) and by export. Owns one decoder per (track, media) pair so two tracks
// playing the same file do not fight over one read position. Not thread-safe.
class AudioMixer {
public:
    explicit AudioMixer(int sampleRate);
    ~AudioMixer();

    int sampleRate() const { return m_sampleRate; }

    // Mixes `frames` stereo frames starting at timeline sample `start` into `out`.
    void mix(const TimelineSnapshot& snap, std::int64_t start, int frames, float* out);

private:
    AudioDecoder* decoderFor(const std::string& key, const MediaItem& media);

    int m_sampleRate;
    std::map<std::string, std::unique_ptr<AudioDecoder>> m_decoders;
    std::set<std::string> m_failed;
    std::vector<float> m_scratch;
};

} // namespace ve
