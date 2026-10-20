#pragma once

// Immutable copy of what the engine needs from the project: one timeline plus the media it
// references. The UI thread builds a new snapshot after every change and hands it to the engine
// threads, so they never read the live model.

#include "core/Model.h"

#include <memory>
#include <optional>
#include <unordered_map>

namespace ve {

struct TimelineSnapshot {
    Timeline timeline;
    std::unordered_map<Id, MediaItem> media;

    const MediaItem* findMedia(const Id& id) const;

    static std::shared_ptr<const TimelineSnapshot> fromProject(const Project& project);
};

using TimelineSnapshotPtr = std::shared_ptr<const TimelineSnapshot>;

struct VideoSource {
    const MediaItem* media = nullptr;
    Time sourceTime; // media time, relative to the start of the media
};

// What the topmost enabled video track shows at timeline time t (nullopt = black). Multicam clips
// show their active angle; compound clips are not rendered yet.
std::optional<VideoSource> resolveVideo(const TimelineSnapshot& snap, Time t);

} // namespace ve
