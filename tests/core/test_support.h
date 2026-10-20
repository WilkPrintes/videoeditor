#pragma once

// Shared fixtures for the core tests.

#include "core/Edit.h"
#include "core/Model.h"

namespace ve::test {

inline constexpr Rate kRate{30, 1};

// Frame n at 30 fps, in flicks.
inline Time F(std::int64_t n)
{
    return timeOfFrame(n, kRate);
}

inline MediaItem testMedia(double seconds = 10.0, bool video = true, bool audio = true)
{
    MediaItem m;
    m.id = newId();
    m.name = "clip.mp4";
    m.path = "C:/media/clip.mp4";
    m.duration = timeFromSeconds(seconds);
    m.hasVideo = video;
    m.width = 1920;
    m.height = 1080;
    m.frameRate = kRate;
    m.hasAudio = audio;
    m.sampleRate = 48000;
    m.channels = 2;
    return m;
}

// A project with one 10 s media placed at 0 on V1/A1 (300 frames).
struct Fixture {
    Project project = makeEmptyProject();
    MediaItem media = testMedia();

    Fixture()
    {
        project.timeline().rate = kRate;
        project.media.items.push_back(media);
        edit::placeMedia(project.timeline(), media);
    }
    Timeline& tl() { return project.timeline(); }
    Track& v1() { return tl().tracks[0]; }
    Track& a1() { return tl().tracks[1]; }
    const MediaPool& pool() const { return project.media; }
};

// Every link group present on V1 has an identical partner on A1 and vice versa.
inline bool linkedTracksAligned(const Timeline& tl)
{
    const auto v = tl.tracks[0].clips();
    const auto a = tl.tracks[1].clips();
    if (v.size() != a.size())
        return false;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i].start != a[i].start || !(v[i].clip->sourceRange == a[i].clip->sourceRange)
            || v[i].clip->linkGroup != a[i].clip->linkGroup)
            return false;
    }
    return true;
}

} // namespace ve::test
