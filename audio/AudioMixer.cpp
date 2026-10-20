#include "audio/AudioMixer.h"

#include "core/Log.h"
#include "media/AudioDecoder.h"

#include <algorithm>

namespace ve {

AudioMixer::AudioMixer(int sampleRate) : m_sampleRate(sampleRate) {}
AudioMixer::~AudioMixer() = default;

AudioDecoder* AudioMixer::decoderFor(const std::string& key, const MediaItem& media)
{
    auto it = m_decoders.find(key);
    if (it != m_decoders.end())
        return it->second.get();
    if (m_failed.count(key))
        return nullptr;
    auto dec = std::make_unique<AudioDecoder>();
    std::string error;
    if (!dec->open(media.path, m_sampleRate, &error)) {
        logWarning("Audio unavailable for ", media.name, ": ", error);
        m_failed.insert(key);
        return nullptr;
    }
    return m_decoders.emplace(key, std::move(dec)).first->second.get();
}

void AudioMixer::mix(const TimelineSnapshot& snap, std::int64_t start, int frames, float* out)
{
    std::fill(out, out + std::size_t(frames) * 2, 0.0f);
    const Timeline& tl = snap.timeline;
    const std::int64_t end = start + frames;

    for (const Track& track : tl.tracks) {
        if (track.kind != TrackKind::Audio || !track.enabled)
            continue;
        for (const PlacedItem& placed : track.clips()) {
            const Clip* clip = std::get_if<Clip>(placed.item);
            if (!clip || !clip->enabled)
                continue; // compound/multicam audio is not mixed yet
            const std::int64_t clipStart = sampleAt(placed.start, m_sampleRate);
            const std::int64_t clipEnd = sampleAt(placed.end(), m_sampleRate);
            const std::int64_t s0 = std::max(clipStart, start);
            const std::int64_t s1 = std::min(clipEnd, end);
            if (s0 >= s1)
                continue;
            const MediaItem* media = snap.findMedia(clip->mediaId);
            if (!media || !media->hasAudio)
                continue;
            AudioDecoder* dec = decoderFor(track.id + "|" + media->id, *media);
            if (!dec)
                continue;

            const int n = int(s1 - s0);
            // Source sample = where the clip starts in its media + how far into the clip we are.
            const std::int64_t sourceSample = sampleAt(clip->sourceRange.start, m_sampleRate) + (s0 - clipStart);
            m_scratch.resize(std::size_t(n) * 2);
            dec->read(sourceSample, m_scratch.data(), n);

            float* dst = out + std::size_t(s0 - start) * 2;
            for (std::size_t i = 0; i < m_scratch.size(); ++i)
                dst[i] += m_scratch[i];
        }
    }

    for (std::size_t i = 0; i < std::size_t(frames) * 2; ++i)
        out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

} // namespace ve
