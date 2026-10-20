#include "core/Snapshot.h"

namespace ve {

const MediaItem* TimelineSnapshot::findMedia(const Id& id) const
{
    auto it = media.find(id);
    return it == media.end() ? nullptr : &it->second;
}

std::shared_ptr<const TimelineSnapshot> TimelineSnapshot::fromProject(const Project& project)
{
    auto snap = std::make_shared<TimelineSnapshot>();
    snap->timeline = project.timeline();
    for (const MediaItem& m : project.media.items)
        snap->media.emplace(m.id, m);
    return snap;
}

std::optional<VideoSource> resolveVideo(const TimelineSnapshot& snap, Time t)
{
    const auto& tracks = snap.timeline.tracks;
    // Later video tracks (V2, V3...) sit on top of earlier ones.
    for (auto it = tracks.rbegin(); it != tracks.rend(); ++it) {
        if (it->kind != TrackKind::Video || !it->enabled)
            continue;
        auto placed = it->clipAt(t);
        if (!placed || !placed->clip->enabled)
            continue;
        Id mediaId;
        if (const Clip* clip = std::get_if<Clip>(placed->item))
            mediaId = clip->mediaId;
        else if (const MulticamClip* mc = std::get_if<MulticamClip>(placed->item);
                 mc && mc->activeAngle >= 0 && std::size_t(mc->activeAngle) < mc->angles.size())
            mediaId = mc->angles[std::size_t(mc->activeAngle)];
        const MediaItem* media = snap.findMedia(mediaId);
        if (!media || !media->hasVideo)
            continue;
        return VideoSource{media, placed->clip->sourceRange.start + (t - placed->start)};
    }
    return std::nullopt;
}

} // namespace ve
