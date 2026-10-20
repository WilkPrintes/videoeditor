#pragma once

// Immutable copy of what the engine needs from the project: one timeline, the timelines its
// compound clips refer to, and the media. The UI thread builds a new snapshot after every change and
// hands it to the engine threads, so they never read the live model.

#include "core/Model.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ve {

struct TimelineSnapshot {
    Timeline timeline;
    std::unordered_map<Id, Timeline> nested; // the project's other timelines (sources of compound clips)
    std::unordered_map<Id, MediaItem> media;

    const MediaItem* findMedia(const Id& id) const;
    const Timeline* findTimeline(const Id& id) const;

    static std::shared_ptr<const TimelineSnapshot> fromProject(const Project& project);
};

using TimelineSnapshotPtr = std::shared_ptr<const TimelineSnapshot>;

struct VideoSource {
    const MediaItem* media = nullptr;
    Time sourceTime; // media time, relative to the start of the media

    bool operator==(const VideoSource&) const = default;
};

// What the topmost enabled video track shows at timeline time t (nullopt = black). Multicam clips
// show their active angle; compound clips show their nested timeline (the parent renders the child
// as a source, spec §6.3), and where the child is empty the tracks below show through.
std::optional<VideoSource> resolveVideo(const TimelineSnapshot& snap, Time t);

// A stretch of one media file's sound in the timeline, with compound clips expanded: the media plays
// over [start, end), and at timeline time `origin` it is at media time `sourceOrigin`.
struct AudioSegment {
    const MediaItem* media = nullptr;
    std::string key; // the track path, so every track gets a decoder of its own
    Time start;
    Time end;
    Time origin;
    Time sourceOrigin;

    bool operator==(const AudioSegment&) const = default;
};

// The sound of every enabled audio track overlapping [from, to), in track order (the mixing
// order). Retimed clips are left out until audio time-stretching exists (spec §6.5).
std::vector<AudioSegment> audioSegments(const TimelineSnapshot& snap, Time from, Time to);

} // namespace ve
