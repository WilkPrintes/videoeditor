#include "core/Snapshot.h"

#include <algorithm>

namespace ve {

namespace {

constexpr int kMaxNesting = 16; // compound clips inside compound clips; also stops cycles

// The media shown by a clip-like item that is not a compound.
const MediaItem* mediaOf(const TimelineSnapshot& snap, const Item& item)
{
    if (const Clip* clip = std::get_if<Clip>(&item))
        return snap.findMedia(clip->mediaId);
    if (const MulticamClip* mc = std::get_if<MulticamClip>(&item);
        mc && mc->activeAngle >= 0 && std::size_t(mc->activeAngle) < mc->angles.size())
        return snap.findMedia(mc->angles[std::size_t(mc->activeAngle)]);
    return nullptr;
}

std::optional<VideoSource> resolveIn(const TimelineSnapshot& snap, const Timeline& tl, Time t, int depth)
{
    const auto& tracks = tl.tracks;
    // Later video tracks (V2, V3...) sit on top of earlier ones.
    for (auto it = tracks.rbegin(); it != tracks.rend(); ++it) {
        if (it->kind != TrackKind::Video || !it->enabled)
            continue;
        auto placed = it->clipAt(t);
        if (!placed || !placed->clip->enabled)
            continue;
        const Time offset = t - placed->start;
        if (const CompoundClip* cc = std::get_if<CompoundClip>(placed->item)) {
            const Timeline* child = snap.findTimeline(cc->timelineId);
            if (child && depth < kMaxNesting) {
                if (auto inner = resolveIn(snap, *child, cc->sourceRange.start + offset, depth + 1))
                    return inner;
            }
            continue; // an empty spot of the child shows what is below
        }
        const MediaItem* media = mediaOf(snap, *placed->item);
        if (!media || !media->hasVideo)
            continue;
        return VideoSource{media, placed->clip->sourceRange.start + sourceOffset(*placed->item, offset)};
    }
    return std::nullopt;
}

// `shift` turns times of `tl` into top-level timeline times; sound is kept inside [windowStart,
// windowEnd), the part of the enclosing compound clips that is visible.
void collectAudio(const TimelineSnapshot& snap, const Timeline& tl, Time shift, Time windowStart, Time windowEnd,
                  Time from, Time to, const std::string& prefix, int depth, std::vector<AudioSegment>& out)
{
    for (const Track& track : tl.tracks) {
        if (track.kind != TrackKind::Audio || !track.enabled)
            continue;
        for (const PlacedItem& placed : track.clips()) {
            if (!placed.clip->enabled)
                continue;
            const Time origin = placed.start + shift;
            const Time start = std::max(origin, windowStart);
            const Time end = std::min(placed.end() + shift, windowEnd);
            if (start >= end || end <= from || start >= to)
                continue;
            const Time sourceStart = placed.clip->sourceRange.start;
            if (const CompoundClip* cc = std::get_if<CompoundClip>(placed.item)) {
                const Timeline* child = snap.findTimeline(cc->timelineId);
                if (child && depth < kMaxNesting)
                    collectAudio(snap, *child, origin - sourceStart, start, end, from, to, prefix + track.id + "/",
                                 depth + 1, out);
                continue;
            }
            if (const Clip* clip = std::get_if<Clip>(placed.item); clip && !clip->retime.isIdentity())
                continue; // needs time-stretching (spec §6.5)
            const MediaItem* media = mediaOf(snap, *placed.item);
            if (!media || !media->hasAudio)
                continue;
            out.push_back({media, prefix + track.id + "|" + media->id, start, end, origin, sourceStart});
        }
    }
}

} // namespace

const MediaItem* TimelineSnapshot::findMedia(const Id& id) const
{
    auto it = media.find(id);
    return it == media.end() ? nullptr : &it->second;
}

const Timeline* TimelineSnapshot::findTimeline(const Id& id) const
{
    if (id == timeline.id)
        return &timeline;
    auto it = nested.find(id);
    return it == nested.end() ? nullptr : &it->second;
}

std::shared_ptr<const TimelineSnapshot> TimelineSnapshot::fromProject(const Project& project)
{
    auto snap = std::make_shared<TimelineSnapshot>();
    snap->timeline = project.timeline();
    for (const Timeline& tl : project.timelines) {
        if (tl.id != snap->timeline.id)
            snap->nested.emplace(tl.id, tl);
    }
    for (const MediaItem& m : project.media.items)
        snap->media.emplace(m.id, m);
    return snap;
}

std::optional<VideoSource> resolveVideo(const TimelineSnapshot& snap, Time t)
{
    return resolveIn(snap, snap.timeline, t, 0);
}

std::vector<AudioSegment> audioSegments(const TimelineSnapshot& snap, Time from, Time to)
{
    std::vector<AudioSegment> out;
    collectAudio(snap, snap.timeline, Time(0), Time(0), Time::max(), from, to, {}, 0, out);
    return out;
}

} // namespace ve
