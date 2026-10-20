#include "core/Edit.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace ve::edit {

namespace {

struct FreeSpace {
    Time before;
    Time after;
};

// Empty time directly before/after the item at `index`. Past the last item the track is open.
FreeSpace freeAround(const Track& track, std::size_t index)
{
    FreeSpace fs;
    if (index > 0) {
        if (const Gap* g = std::get_if<Gap>(&track.items[index - 1]))
            fs.before = g->duration;
    }
    if (index + 1 < track.items.size()) {
        if (const Gap* g = std::get_if<Gap>(&track.items[index + 1]))
            fs.after = g->duration;
    } else {
        fs.after = Time::max();
    }
    return fs;
}

const ClipBase* findClip(const Timeline& tl, const Id& clipId, const Track** owner = nullptr)
{
    for (const Track& t : tl.tracks) {
        if (auto placed = t.findClip(clipId)) {
            if (owner)
                *owner = &t;
            return placed->clip;
        }
    }
    return nullptr;
}

// Media length available to a clip (unbounded for compounds and multicam until they are rendered).
Time sourceLength(const Item& item, const MediaPool& pool)
{
    if (const Clip* c = std::get_if<Clip>(&item)) {
        if (const MediaItem* m = pool.find(c->mediaId))
            return m->duration;
    }
    return Time::max();
}

} // namespace

std::vector<Id> linkedGroup(const Timeline& tl, const Id& clipId)
{
    const ClipBase* clip = findClip(tl, clipId);
    if (!clip)
        return {};
    if (clip->linkGroup.empty())
        return {clipId};
    std::vector<Id> out;
    for (const Track& t : tl.tracks) {
        for (const PlacedItem& p : t.clips()) {
            if (p.clip->linkGroup == clip->linkGroup)
                out.push_back(p.clip->id);
        }
    }
    return out;
}

std::vector<Id> expandLinked(const Timeline& tl, const std::vector<Id>& clipIds)
{
    std::vector<Id> out;
    std::set<Id> seen;
    for (const Id& id : clipIds) {
        for (Id& member : linkedGroup(tl, id)) {
            const Track* owner = nullptr;
            if (!findClip(tl, member, &owner) || owner->locked)
                continue;
            if (seen.insert(member).second)
                out.push_back(std::move(member));
        }
    }
    return out;
}

std::vector<Id> placeMedia(Timeline& tl, const MediaItem& media, std::optional<Time> at)
{
    Track* video = media.hasVideo ? tl.firstTrack(TrackKind::Video) : nullptr;
    Track* audio = media.hasAudio ? tl.firstTrack(TrackKind::Audio) : nullptr;
    const Time length = floorToFrame(media.duration, tl.rate); // clips stay on the frame grid
    if ((!video && !audio) || length <= Time(0))
        return {};

    const Time start = at ? floorToFrame(std::max(*at, Time(0)), tl.rate)
                          : std::max(video ? video->duration() : Time(0), audio ? audio->duration() : Time(0));
    const Id link = (video && audio) ? newId() : Id{};
    IdRemap links;
    std::vector<Id> placed;
    for (Track* track : {video, audio}) {
        if (!track)
            continue;
        Clip clip;
        clip.id = newId();
        clip.mediaId = media.id;
        clip.name = media.name;
        clip.sourceRange = {Time(0), length};
        clip.linkGroup = link;
        placed.push_back(clip.id);
        track->placeItem(start, std::move(clip), links);
    }
    return placed;
}

bool splitAt(Timeline& tl, Time t, const std::vector<Id>& only)
{
    std::set<Id> allowed;
    if (!only.empty()) {
        for (Id& id : expandLinked(tl, only))
            allowed.insert(std::move(id));
    }
    IdRemap links;
    bool any = false;
    for (Track& track : tl.tracks) {
        if (track.locked)
            continue;
        auto placed = track.clipAt(t);
        if (!placed || placed->start == t)
            continue;
        if (!allowed.empty() && !allowed.count(placed->clip->id))
            continue;
        any |= track.splitAt(t, links);
    }
    return any;
}

bool moveClips(Timeline& tl, const std::vector<Id>& clipIds, Time delta)
{
    if (delta == Time(0))
        return false;
    struct Moving {
        Id trackId;
        Item item;
        Time start;
    };
    std::vector<Moving> moving;
    for (const Id& id : expandLinked(tl, clipIds)) {
        if (auto loc = tl.locateClip(id))
            moving.push_back({loc->track->id, loc->item, loc->start});
    }
    if (moving.empty())
        return false;

    Time minStart = Time::max();
    for (const Moving& m : moving)
        minStart = std::min(minStart, m.start);
    delta = std::max(delta, -minStart); // nothing moves before zero
    if (delta == Time(0))
        return false;

    IdRemap links;
    for (const Moving& m : moving)
        tl.track(m.trackId)->liftRange(m.start, m.start + durationOf(m.item), links);
    for (const Moving& m : moving)
        tl.track(m.trackId)->placeItem(m.start + delta, m.item, links);
    return true;
}

bool trimClip(Timeline& tl, const MediaPool& pool, const Id& clipId, Edge edge, Time newEdge)
{
    auto primary = tl.locateClip(clipId);
    if (!primary || primary->track->locked)
        return false;
    Time delta = edge == Edge::Head ? newEdge - primary->start : newEdge - primary->end();
    if (delta == Time(0))
        return false;

    struct Target {
        Id trackId;
        Item item;
        Time start;
    };
    std::vector<Target> targets;
    Time lo = -Time::max();
    Time hi = Time::max();
    const Time oneFrame = timeOfFrame(1, tl.rate);

    for (const Id& id : expandLinked(tl, {clipId})) {
        auto loc = tl.locateClip(id);
        if (!loc)
            continue;
        const Track& track = *loc->track;
        const ClipBase& c = loc->clip();
        const FreeSpace free = freeAround(track, track.findClip(id)->index);
        const Time length = sourceLength(loc->item, pool);
        if (edge == Edge::Head) {
            lo = std::max(lo, -floorToFrame(std::min(free.before, c.sourceRange.start), tl.rate));
            hi = std::min(hi, c.sourceRange.duration - oneFrame);
        } else {
            const Time mediaLeft = length == Time::max() ? Time::max()
                                                          : std::max(Time(0), length - c.sourceRange.end());
            lo = std::max(lo, -(c.sourceRange.duration - oneFrame));
            hi = std::min(hi, std::min(free.after, mediaLeft == Time::max() ? mediaLeft : floorToFrame(mediaLeft, tl.rate)));
        }
        targets.push_back({track.id, loc->item, loc->start});
    }
    if (targets.empty() || lo > hi)
        return false;
    delta = std::clamp(delta, lo, hi);
    if (delta == Time(0))
        return false;

    IdRemap links;
    for (Target& t : targets) {
        Track* track = tl.track(t.trackId);
        const Time oldDuration = durationOf(t.item);
        ClipBase* c = clipBase(t.item);
        Time start = t.start;
        if (edge == Edge::Head) {
            start += delta;
            c->sourceRange.start += delta;
            c->sourceRange.duration -= delta;
        } else {
            c->sourceRange.duration += delta;
        }
        track->liftRange(t.start, t.start + oldDuration, links);
        track->placeItem(start, std::move(t.item), links);
    }
    return true;
}

bool deleteClips(Timeline& tl, const std::vector<Id>& clipIds)
{
    IdRemap links;
    bool any = false;
    for (const Id& id : expandLinked(tl, clipIds)) {
        if (auto loc = tl.locateClip(id)) {
            loc->track->liftRange(loc->start, loc->end(), links);
            any = true;
        }
    }
    return any;
}

std::string checkInvariants(const Timeline& tl, const MediaPool& pool)
{
    std::ostringstream err;
    for (const Track& track : tl.tracks) {
        const auto& items = track.items;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const bool transition = std::holds_alternative<Transition>(items[i]);
            if (!transition && durationOf(items[i]) <= Time(0))
                err << track.name << ": empty item at " << i << "\n";
            if (i > 0 && std::holds_alternative<Gap>(items[i]) && std::holds_alternative<Gap>(items[i - 1]))
                err << track.name << ": adjacent gaps at " << i << "\n";
            if (transition && (i == 0 || i + 1 == items.size() || !clipBase(items[i - 1]) || !clipBase(items[i + 1])))
                err << track.name << ": dangling transition at " << i << "\n";
        }
        if (!items.empty() && std::holds_alternative<Gap>(items.back()))
            err << track.name << ": trailing gap\n";
        for (const PlacedItem& p : track.clips()) {
            if (p.start != floorToFrame(p.start, tl.rate) || p.end() != floorToFrame(p.end(), tl.rate))
                err << track.name << ": clip " << p.clip->id << " off the frame grid\n";
            if (p.clip->sourceRange.start < Time(0))
                err << track.name << ": clip " << p.clip->id << " starts before its media\n";
            if (p.clip->sourceRange.end() > sourceLength(*p.item, pool))
                err << track.name << ": clip " << p.clip->id << " uses media past its end\n";
        }
    }
    return err.str();
}

} // namespace ve::edit
