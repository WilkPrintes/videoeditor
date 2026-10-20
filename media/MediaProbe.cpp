#include "media/MediaProbe.h"

#include "media/FFmpegUtil.h"

#include <algorithm>
#include <filesystem>

namespace ve {

namespace {

std::string tag(const AVDictionary* dict, const char* key)
{
    const AVDictionaryEntry* e = av_dict_get(dict, key, nullptr, 0);
    return e ? std::string(e->value) : std::string();
}

} // namespace

std::optional<MediaItem> probeMedia(const std::string& path, std::string* error)
{
    ff::InputFormatPtr fmt = ff::openInput(path, error);
    if (!fmt)
        return std::nullopt;

    MediaItem m;
    m.id = newId();
    m.path = path;
    const std::u8string fileName = std::filesystem::path(std::u8string(path.begin(), path.end())).filename().u8string();
    m.name.assign(fileName.begin(), fileName.end());

    const int videoIndex = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audioIndex = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    std::string timecode = tag(fmt->metadata, "timecode");
    if (videoIndex >= 0) {
        AVStream* st = fmt->streams[videoIndex];
        // Cover art (an attached picture) is not a video track.
        if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            m.hasVideo = true;
            m.width = st->codecpar->width;
            m.height = st->codecpar->height;
            m.frameRate = rateFromDouble(av_q2d(av_guess_frame_rate(fmt.get(), st, nullptr)));
            m.videoCodec = avcodec_get_name(st->codecpar->codec_id);
            if (timecode.empty())
                timecode = tag(st->metadata, "timecode");
        }
    }
    if (audioIndex >= 0) {
        AVStream* st = fmt->streams[audioIndex];
        m.hasAudio = true;
        m.sampleRate = st->codecpar->sample_rate;
        m.channels = st->codecpar->ch_layout.nb_channels;
        m.audioCodec = avcodec_get_name(st->codecpar->codec_id);
    }

    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
        m.duration = timeFromMicroseconds(fmt->duration); // AV_TIME_BASE is microseconds
    } else {
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            const AVStream* st = fmt->streams[i];
            if (st->duration != AV_NOPTS_VALUE)
                m.duration = std::max(m.duration, ff::toTime(st->duration, st->time_base));
        }
    }
    if (!timecode.empty() && m.hasVideo)
        parseTimecode(timecode, m.frameRate, &m.startTimecode);
    for (const char* key : {"creation_time", "encoder", "title"}) {
        const std::string value = tag(fmt->metadata, key);
        if (!value.empty())
            m.metadata.emplace(key, value);
    }

    if (!m.hasVideo && !m.hasAudio) {
        if (error)
            *error = "No audio or video stream in " + path;
        return std::nullopt;
    }
    if (m.duration <= Time(0)) {
        if (error)
            *error = "Unknown duration (still images and streams are not supported yet): " + path;
        return std::nullopt;
    }
    return m;
}

} // namespace ve
