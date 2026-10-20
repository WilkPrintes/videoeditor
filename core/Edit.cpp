#include "core/Edit.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <map>
#include <numeric>
#include <set>
#include <sstream>

namespace ve::edit {

namespace {

using Range = std::pair<Time, Time>; // [first, second)

Time oneFrame(const Timeline& tl)
{
    return timeOfFrame(1, tl.rate);
}

// Rounded towards zero to whole frames.
Time gridDelta(Time d, Rate rate)
{
    return d < Time(0) ? -floorToFrame(-d, rate) : floorToFrame(d, rate);
}

Time gridPoint(Time t, Rate rate)
{
    return floorToFrame(std::max(t, Time(0)), rate);
}

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

// Media length available to a clip (unbounded for compounds and multicam: not one file).
Time sourceLength(const Item& item, const MediaPool& pool)
{
    if (const Clip* c = std::get_if<Clip>(&item)) {
        if (const MediaItem* m = pool.find(c->mediaId))
            return m->duration;
    }
    return Time::max();
}

// Unused media before / after what the item shows, as timeline time on the frame grid.
Time handleBefore(const Item& item, Rate rate)
{
    return floorToFrame(clipTimeFor(item, clipBase(item)->sourceRange.start), rate);
}

Time handleAfter(const Item& item, const MediaPool& pool, Rate rate)
{
    const Time length = sourceLength(item, pool);
    if (length == Time::max())
        return Time::max();
    return floorToFrame(clipTimeFor(item, std::max(Time(0), length - sourceEnd(item))), rate);
}

bool hasClipIn(const Track& track, Time from, Time to)
{
    for (const PlacedItem& p : track.clips()) {
        if (p.start < to && p.end() > from)
            return true;
    }
    return false;
}

// Empty time on the track right before / right after `at` (unbounded after the last clip).
Time freeBefore(const Track& track, Time at)
{
    Time lastEnd;
    for (const PlacedItem& p : track.clips()) {
        if (p.start < at && at < p.end())
            return Time(0);
        if (p.end() <= at)
            lastEnd = std::max(lastEnd, p.end());
    }
    return at - lastEnd;
}

Time freeAfter(const Track& track, Time at)
{
    Time next = Time::max();
    for (const PlacedItem& p : track.clips()) {
        if (p.start <= at && at < p.end())
            return Time(0);
        if (p.start >= at)
            next = std::min(next, p.start);
    }
    return next == Time::max() ? next : next - at;
}

// Neighbours of the item at `index`, skipping transitions (they take no time).
std::optional<std::size_t> previousItem(const Track& track, std::size_t index)
{
    for (std::size_t i = index; i-- > 0;) {
        if (!std::holds_alternative<Transition>(track.items[i]))
            return i;
    }
    return std::nullopt;
}

std::optional<std::size_t> nextItem(const Track& track, std::size_t index)
{
    for (std::size_t i = index + 1; i < track.items.size(); ++i) {
        if (!std::holds_alternative<Transition>(track.items[i]))
            return i;
    }
    return std::nullopt;
}

// The clips an edit works on: with `linked`, their linked partners too; never clips on locked tracks.
std::vector<Id> selectionOf(const Timeline& tl, const std::vector<Id>& clipIds, bool linked)
{
    if (linked)
        return expandLinked(tl, clipIds);
    std::vector<Id> out;
    std::set<Id> seen;
    for (const Id& id : clipIds) {
        const Track* owner = nullptr;
        if (findClip(tl, id, &owner) && !owner->locked && seen.insert(id).second)
            out.push_back(id);
    }
    return out;
}

// Clip-like items of the selection on unlocked tracks, for in-place changes of their fields.
std::vector<ClipBase*> editableClips(Timeline& tl, const std::vector<Id>& clipIds)
{
    const std::set<Id> wanted(clipIds.begin(), clipIds.end());
    std::vector<ClipBase*> out;
    for (Track& t : tl.tracks) {
        if (t.locked)
            continue;
        for (Item& item : t.items) {
            ClipBase* c = clipBase(item);
            if (c && wanted.count(c->id))
                out.push_back(c);
        }
    }
    return out;
}

// A clip an edit works on, as it was before the edit.
struct Target {
    Id trackId;
    std::size_t index = 0; // in Track::items
    Item item;
    Time start;
    Time end;
};

// The clip and, with `linked`, its linked partners: on unlocked tracks, at most one per track, the
// clip itself first. Empty when the clip is missing or on a locked track.
std::vector<Target> targetsFor(const Timeline& tl, const Id& clipId, bool linked)
{
    std::vector<Id> ids{clipId};
    if (linked) {
        for (const Id& id : linkedGroup(tl, clipId)) {
            if (id != clipId)
                ids.push_back(id);
        }
    }
    std::vector<Target> out;
    std::set<Id> tracks;
    for (const Id& id : ids) {
        for (const Track& t : tl.tracks) {
            auto placed = t.findClip(id);
            if (!placed)
                continue;
            if (!t.locked && tracks.insert(t.id).second)
                out.push_back({t.id, placed->index, *placed->item, placed->start, placed->end()});
            break;
        }
    }
    if (out.empty() || clipBase(out.front().item)->id != clipId)
        return {};
    return out;
}

bool targetsTrack(const std::vector<Target>& targets, const Id& trackId)
{
    return std::any_of(targets.begin(), targets.end(), [&](const Target& t) { return t.trackId == trackId; });
}

// Limits [lo, hi] of moving the tail / the head of a clip or gap by a delta.
void limitTail(const Item& item, const MediaPool& pool, const Timeline& tl, Time& lo, Time& hi)
{
    if (clipBase(item)) {
        hi = std::min(hi, handleAfter(item, pool, tl.rate));
        lo = std::max(lo, oneFrame(tl) - durationOf(item));
    } else {
        lo = std::max(lo, -durationOf(item)); // a gap may close completely
    }
}

void limitHead(const Item& item, const Timeline& tl, Time& lo, Time& hi)
{
    if (clipBase(item)) {
        hi = std::min(hi, durationOf(item) - oneFrame(tl));
        lo = std::max(lo, -handleBefore(item, tl.rate));
    } else {
        hi = std::min(hi, durationOf(item));
    }
}

void moveTail(Item& item, Time delta)
{
    if (ClipBase* c = clipBase(item))
        c->sourceRange.duration += delta;
    else if (Gap* g = std::get_if<Gap>(&item))
        g->duration += delta;
}

void moveHead(Item& item, Time delta)
{
    if (ClipBase* c = clipBase(item)) {
        c->sourceRange.start += sourceOffset(item, delta);
        c->sourceRange.duration -= delta;
    } else if (Gap* g = std::get_if<Gap>(&item)) {
        g->duration -= delta;
    }
}

std::vector<Range> mergeRanges(std::vector<Range> ranges)
{
    std::sort(ranges.begin(), ranges.end());
    std::vector<Range> out;
    for (const Range& r : ranges) {
        if (!out.empty() && r.first <= out.back().second)
            out.back().second = std::max(out.back().second, r.second);
        else
            out.push_back(r);
    }
    return out;
}

// The destination tracks the media can use, picture first. Nothing when a requested destination is
// unusable (missing, of the other kind, locked) or when no destination is left.
std::optional<std::vector<Id>> destinationTracks(const Timeline& tl, const MediaItem& media, const EditPoints& p)
{
    std::vector<Id> out;
    const std::pair<const Id*, TrackKind> wanted[] = {{&p.videoTrack, TrackKind::Video}, {&p.audioTrack, TrackKind::Audio}};
    for (const auto& [id, kind] : wanted) {
        const bool has = kind == TrackKind::Video ? media.hasVideo : media.hasAudio;
        if (!has || id->empty())
            continue;
        const Track* t = tl.track(*id);
        if (!t || t->kind != kind || t->locked)
            return std::nullopt;
        out.push_back(*id);
    }
    if (out.empty())
        return std::nullopt;
    return out;
}

// New clips of the media on the tracks (linked when more than one), placed as an overwrite.
std::vector<Id> placeNewClips(Timeline& tl, const MediaItem& media, const std::vector<Id>& trackIds, Time sourceStart,
                              Time recordStart, Time duration, Retime retime = {})
{
    const Id link = trackIds.size() > 1 ? newId() : Id{};
    IdRemap links;
    std::vector<Id> placed;
    for (const Id& trackId : trackIds) {
        Clip clip;
        clip.id = newId();
        clip.mediaId = media.id;
        clip.name = media.name;
        clip.sourceRange = {sourceStart, duration};
        clip.linkGroup = link;
        clip.retime = retime;
        placed.push_back(clip.id);
        tl.track(trackId)->placeItem(recordStart, std::move(clip), links);
    }
    return placed;
}

int countReferences(const Project& project, const Id& timelineId, const std::set<Id>& except)
{
    int n = 0;
    for (const Timeline& tl : project.timelines) {
        for (const Track& t : tl.tracks) {
            for (const Item& item : t.items) {
                const CompoundClip* cc = std::get_if<CompoundClip>(&item);
                if (cc && cc->timelineId == timelineId && !except.count(cc->id))
                    ++n;
            }
        }
    }
    return n;
}

// Keeping linked clips in sync through ripples. A ripple moves, on each track it touches, everything
// at or after a point. A link group whose members end up on both sides would go out of sync, so the
// edit is refused. A member that spans the point is split: by default the piece before the point
// keeps the group (the new piece gets a fresh one), but when the other members move, the piece after
// the point is the one in sync with them and takes the group instead.
enum class Side { Before, Across, After };
using ShiftPoints = std::map<Id, Time>; // track -> where its content starts moving

std::optional<std::set<Id>> planSync(const Timeline& tl, const ShiftPoints& points, const std::map<Id, Side>& fixed)
{
    std::map<Id, std::set<Side>> sides;
    for (const Track& t : tl.tracks) {
        const auto point = points.find(t.id);
        if (point == points.end())
            continue; // a track that does not move (locked, sync lock off) may fall out of sync by design
        for (const PlacedItem& p : t.clips()) {
            if (p.clip->linkGroup.empty())
                continue;
            Side side = p.start >= point->second ? Side::After : p.end() <= point->second ? Side::Before : Side::Across;
            if (auto f = fixed.find(p.clip->id); f != fixed.end())
                side = f->second;
            sides[p.clip->linkGroup].insert(side);
        }
    }
    std::set<Id> afterKeeps;
    for (const auto& [group, s] : sides) {
        if (s.count(Side::Before) && s.count(Side::After))
            return std::nullopt;
        if (s.count(Side::Across) && s.count(Side::After))
            afterKeeps.insert(group);
    }
    return afterKeeps;
}

void applySync(Timeline& tl, const ShiftPoints& points, const std::set<Id>& afterKeeps, const IdRemap& links)
{
    for (const Id& group : afterKeeps) {
        const Id* split = links.find(group);
        if (!split)
            continue;
        const Id fresh = *split;
        for (Track& t : tl.tracks) {
            const auto point = points.find(t.id);
            if (point == points.end())
                continue;
            Time pos;
            for (Item& item : t.items) {
                const Time d = durationOf(item);
                if (ClipBase* c = clipBase(item)) {
                    if (c->linkGroup == fresh)
                        c->linkGroup = group; // the piece after the point stays with the others
                    else if (c->linkGroup == group && pos + d <= point->second)
                        c->linkGroup = fresh; // the piece before it goes its own way
                }
                pos += d;
            }
        }
    }
}

// Unlocked tracks with sync lock: the ones every ripple shifts.
std::set<Id> syncLockedTracks(const Timeline& tl)
{
    std::set<Id> out;
    for (const Track& t : tl.tracks) {
        if (!t.locked && t.syncLock)
            out.insert(t.id);
    }
    return out;
}

// Whether closing `ranges` (sorted, holding no clip that stays) on `tracks` moves every link group
// by one amount, i.e. keeps the takes on those tracks in sync. `leaving` are clips being removed.
bool groupsMoveTogether(const Timeline& tl, const std::vector<Range>& ranges, const std::set<Id>& tracks,
                        const std::set<Id>& leaving)
{
    std::map<Id, Time> shift;
    for (const Track& t : tl.tracks) {
        if (!tracks.count(t.id))
            continue;
        for (const PlacedItem& p : t.clips()) {
            if (p.clip->linkGroup.empty() || leaving.count(p.clip->id))
                continue;
            Time s;
            for (const Range& r : ranges) {
                if (r.second <= p.start)
                    s += r.second - r.first;
            }
            auto [it, first] = shift.try_emplace(p.clip->linkGroup, s);
            if (!first && it->second != s)
                return false;
        }
    }
    return true;
}

// The part [from, to) of the track, moved to start at zero.
Track cropped(const Track& track, Time from, Time to)
{
    Track out = track;
    IdRemap unused; // nothing is cut in the middle: the crop only removes what is outside
    out.liftRange(to, std::max(to, out.duration()), unused);
    out.removeRange(Time(0), from, unused);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Tracks
// ---------------------------------------------------------------------------------------------

Id addTrack(Timeline& tl, TrackKind kind)
{
    const char prefix = kind == TrackKind::Video ? 'V' : 'A';
    int n = int(tl.tracksOfKind(kind).size()) + 1;
    auto taken = [&](const std::string& name) {
        return std::any_of(tl.tracks.begin(), tl.tracks.end(), [&](const Track& t) { return t.name == name; });
    };
    std::string name;
    do {
        name = prefix + std::to_string(n++);
    } while (taken(name));

    Track track;
    track.id = newId();
    track.kind = kind;
    track.name = name;
    // After the last track of its kind; a first video track goes before the audio ones.
    std::size_t pos = kind == TrackKind::Video ? 0 : tl.tracks.size();
    for (std::size_t i = 0; i < tl.tracks.size(); ++i) {
        if (tl.tracks[i].kind == kind)
            pos = i + 1;
    }
    const Id id = track.id;
    tl.tracks.insert(tl.tracks.begin() + std::ptrdiff_t(pos), std::move(track));
    return id;
}

bool setTrackLocked(Timeline& tl, const Id& trackId, bool locked)
{
    Track* t = tl.track(trackId);
    if (!t || t->locked == locked)
        return false;
    t->locked = locked;
    return true;
}

bool setTrackSyncLock(Timeline& tl, const Id& trackId, bool syncLock)
{
    Track* t = tl.track(trackId);
    if (!t || t->syncLock == syncLock)
        return false;
    t->syncLock = syncLock;
    return true;
}

int kindIndex(const Timeline& tl, const Id& trackId)
{
    const Track* target = tl.track(trackId);
    if (!target)
        return -1;
    int index = 0;
    for (const Track& t : tl.tracks) {
        if (t.kind != target->kind)
            continue;
        if (t.id == trackId)
            return index;
        ++index;
    }
    return -1;
}

Track* trackOfKind(Timeline& tl, TrackKind kind, int index)
{
    if (index < 0)
        return nullptr;
    for (Track& t : tl.tracks) {
        if (t.kind == kind && index-- == 0)
            return &t;
    }
    return nullptr;
}

const Track* trackOfKind(const Timeline& tl, TrackKind kind, int index)
{
    return trackOfKind(const_cast<Timeline&>(tl), kind, index);
}

// ---------------------------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------------------------

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

std::vector<Time> editPoints(const Timeline& tl)
{
    std::vector<Time> out;
    for (const Track& t : tl.tracks) {
        for (const PlacedItem& p : t.clips()) {
            out.push_back(p.start);
            out.push_back(p.end());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ---------------------------------------------------------------------------------------------
// Placing media, blade, move, lift
// ---------------------------------------------------------------------------------------------

std::vector<Id> placeMedia(Timeline& tl, const MediaItem& media, std::optional<Time> at)
{
    Track* video = media.hasVideo ? tl.firstTrack(TrackKind::Video) : nullptr;
    Track* audio = media.hasAudio ? tl.firstTrack(TrackKind::Audio) : nullptr;
    const Time length = floorToFrame(media.duration, tl.rate); // clips stay on the frame grid
    if ((!video && !audio) || length <= Time(0))
        return {};

    const Time start = at ? gridPoint(*at, tl.rate)
                          : std::max(video ? video->duration() : Time(0), audio ? audio->duration() : Time(0));
    std::vector<Id> tracks;
    for (Track* track : {video, audio}) {
        if (track)
            tracks.push_back(track->id);
    }
    return placeNewClips(tl, media, tracks, Time(0), start, length);
}

bool splitAt(Timeline& tl, Time t, const std::vector<Id>& only, bool linked)
{
    std::set<Id> allowed;
    if (!only.empty()) {
        for (Id& id : selectionOf(tl, only, linked))
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

bool moveClips(Timeline& tl, const std::vector<Id>& clipIds, Time delta, int trackShift, bool linked)
{
    struct Moving {
        Id trackId;
        TrackKind kind;
        int index;
        Item item;
        Time start;
    };
    std::vector<Moving> moving;
    for (const Id& id : selectionOf(tl, clipIds, linked)) {
        if (auto loc = tl.locateClip(id))
            moving.push_back({loc->track->id, loc->track->kind, kindIndex(tl, loc->track->id), loc->item, loc->start});
    }
    if (moving.empty())
        return false;

    Time minStart = Time::max();
    for (const Moving& m : moving)
        minStart = std::min(minStart, m.start);
    delta = std::max(delta, -minStart); // nothing moves before zero

    auto fits = [&](int shift) {
        for (const Moving& m : moving) {
            const Track* t = trackOfKind(tl, m.kind, m.index + shift);
            if (!t || t->locked)
                return false;
        }
        return true;
    };
    while (trackShift != 0 && !fits(trackShift))
        trackShift += trackShift > 0 ? -1 : 1;
    if (delta == Time(0) && trackShift == 0)
        return false;

    IdRemap links;
    for (const Moving& m : moving)
        tl.track(m.trackId)->liftRange(m.start, m.start + durationOf(m.item), links);
    for (const Moving& m : moving)
        trackOfKind(tl, m.kind, m.index + trackShift)->placeItem(m.start + delta, m.item, links);
    return true;
}

bool trimClip(Timeline& tl, const MediaPool& pool, const Id& clipId, Edge edge, Time newEdge, bool linked)
{
    std::vector<Target> targets = targetsFor(tl, clipId, linked);
    if (targets.empty())
        return false;
    Time delta = edge == Edge::Head ? newEdge - targets.front().start : newEdge - targets.front().end;
    if (delta == Time(0))
        return false;

    Time lo = -Time::max();
    Time hi = Time::max();
    for (const Target& t : targets) {
        const FreeSpace free = freeAround(*tl.track(t.trackId), t.index);
        if (edge == Edge::Head) {
            lo = std::max(lo, -std::min(free.before, handleBefore(t.item, tl.rate)));
            hi = std::min(hi, durationOf(t.item) - oneFrame(tl));
        } else {
            lo = std::max(lo, oneFrame(tl) - durationOf(t.item));
            hi = std::min(hi, std::min(free.after, handleAfter(t.item, pool, tl.rate)));
        }
    }
    if (lo > hi)
        return false;
    delta = std::clamp(delta, lo, hi);
    if (delta == Time(0))
        return false;

    IdRemap links;
    for (Target& t : targets) {
        Track* track = tl.track(t.trackId);
        const Time oldDuration = durationOf(t.item);
        Time start = t.start;
        if (edge == Edge::Head) {
            start += delta;
            moveHead(t.item, delta);
        } else {
            moveTail(t.item, delta);
        }
        track->liftRange(t.start, t.start + oldDuration, links);
        track->placeItem(start, std::move(t.item), links);
    }
    return true;
}

bool deleteClips(Timeline& tl, const std::vector<Id>& clipIds, bool linked)
{
    IdRemap links;
    bool any = false;
    for (const Id& id : selectionOf(tl, clipIds, linked)) {
        if (auto loc = tl.locateClip(id)) {
            loc->track->liftRange(loc->start, loc->end(), links);
            any = true;
        }
    }
    return any;
}

// ---------------------------------------------------------------------------------------------
// Organization
// ---------------------------------------------------------------------------------------------

namespace {

// Clears the group of members left alone in it (a group of one links nothing).
void dropLonelyGroups(Timeline& tl, const std::set<Id>& groups)
{
    std::map<Id, int> members;
    for (const Track& t : tl.tracks) {
        for (const Item& item : t.items) {
            const ClipBase* c = clipBase(item);
            if (c && groups.count(c->linkGroup))
                ++members[c->linkGroup];
        }
    }
    for (Track& t : tl.tracks) {
        if (t.locked)
            continue;
        for (Item& item : t.items) {
            ClipBase* c = clipBase(item);
            if (c && groups.count(c->linkGroup) && members[c->linkGroup] == 1)
                c->linkGroup.clear();
        }
    }
}

} // namespace

bool renameClip(Timeline& tl, const Id& clipId, const std::string& name)
{
    const auto clips = editableClips(tl, {clipId});
    if (clips.empty() || clips.front()->name == name)
        return false;
    clips.front()->name = name;
    return true;
}

bool setClipColor(Timeline& tl, const std::vector<Id>& clipIds, const std::string& color)
{
    bool any = false;
    for (ClipBase* c : editableClips(tl, clipIds)) {
        if (c->color != color) {
            c->color = color;
            any = true;
        }
    }
    return any;
}

bool setClipFlag(Timeline& tl, const std::vector<Id>& clipIds, const std::string& color, bool on)
{
    if (color.empty())
        return false;
    bool any = false;
    for (ClipBase* c : editableClips(tl, clipIds)) {
        auto it = std::lower_bound(c->flags.begin(), c->flags.end(), color);
        const bool has = it != c->flags.end() && *it == color;
        if (on && !has) {
            c->flags.insert(it, color);
            any = true;
        } else if (!on && has) {
            c->flags.erase(it);
            any = true;
        }
    }
    return any;
}

bool clearClipFlags(Timeline& tl, const std::vector<Id>& clipIds)
{
    bool any = false;
    for (ClipBase* c : editableClips(tl, clipIds)) {
        if (!c->flags.empty()) {
            c->flags.clear();
            any = true;
        }
    }
    return any;
}

bool setClipsEnabled(Timeline& tl, const std::vector<Id>& clipIds, bool enabled)
{
    bool any = false;
    for (ClipBase* c : editableClips(tl, clipIds)) {
        if (c->enabled != enabled) {
            c->enabled = enabled;
            any = true;
        }
    }
    return any;
}

bool renameTrack(Timeline& tl, const Id& trackId, const std::string& name)
{
    Track* t = tl.track(trackId);
    if (!t || name.empty() || t->name == name)
        return false;
    for (const Track& other : tl.tracks) {
        if (other.id != trackId && other.name == name)
            return false;
    }
    t->name = name;
    return true;
}

bool setTrackColor(Timeline& tl, const Id& trackId, const std::string& color)
{
    Track* t = tl.track(trackId);
    if (!t || t->color == color)
        return false;
    t->color = color;
    return true;
}

bool setTrackEnabled(Timeline& tl, const Id& trackId, bool enabled)
{
    Track* t = tl.track(trackId);
    if (!t || t->enabled == enabled)
        return false;
    t->enabled = enabled;
    return true;
}

bool linkClips(Timeline& tl, const std::vector<Id>& clipIds)
{
    const std::set<Id> wanted(clipIds.begin(), clipIds.end());
    const auto clips = editableClips(tl, clipIds);
    if (clips.size() < 2 || clips.size() != wanted.size())
        return false; // a clip is missing or on a locked track
    std::set<Id> tracks;
    std::set<Id> oldGroups;
    for (const Track& t : tl.tracks) {
        for (const PlacedItem& p : t.clips()) {
            if (!wanted.count(p.clip->id))
                continue;
            if (!tracks.insert(t.id).second)
                return false; // two clips of one track cannot be one take
            oldGroups.insert(p.clip->linkGroup);
        }
    }
    // Already exactly one group made of these clips: nothing to do.
    if (oldGroups.size() == 1 && !oldGroups.begin()->empty() && linkedGroup(tl, clipIds.front()).size() == wanted.size())
        return false;
    const Id group = newId();
    for (ClipBase* c : clips)
        c->linkGroup = group;
    oldGroups.erase(Id{});
    dropLonelyGroups(tl, oldGroups);
    return true;
}

bool unlinkClips(Timeline& tl, const std::vector<Id>& clipIds)
{
    std::set<Id> groups;
    for (ClipBase* c : editableClips(tl, clipIds)) {
        if (!c->linkGroup.empty()) {
            groups.insert(c->linkGroup);
            c->linkGroup.clear();
        }
    }
    dropLonelyGroups(tl, groups);
    return !groups.empty();
}

// ---------------------------------------------------------------------------------------------
// Three and four point edits
// ---------------------------------------------------------------------------------------------

std::optional<ResolvedPoints> resolvePoints(const EditPoints& p, const MediaItem& media, Rate rate)
{
    auto grid = [&](const std::optional<Time>& t) {
        return t ? std::optional<Time>(gridPoint(*t, rate)) : std::nullopt;
    };
    const Time mediaEnd = floorToFrame(media.duration, rate);
    const auto sourceIn = grid(p.sourceIn);
    const auto sourceOut = grid(p.sourceOut);
    const auto recordIn = grid(p.recordIn);
    const auto recordOut = grid(p.recordOut);

    ResolvedPoints r;
    r.duration = recordIn && recordOut ? *recordOut - *recordIn : sourceOut.value_or(mediaEnd) - sourceIn.value_or(Time(0));
    r.sourceStart = sourceIn ? *sourceIn : sourceOut ? *sourceOut - r.duration : Time(0);
    r.recordStart = recordIn ? *recordIn : recordOut ? *recordOut - r.duration : gridPoint(p.playhead, rate);
    if (r.duration < timeOfFrame(1, rate) || r.sourceStart < Time(0) || r.sourceStart + r.duration > mediaEnd
        || r.recordStart < Time(0))
        return std::nullopt;
    return r;
}

std::vector<Id> overwriteEdit(Timeline& tl, const MediaPool& pool, const EditPoints& points)
{
    const MediaItem* media = pool.find(points.mediaId);
    if (!media)
        return {};
    const auto r = resolvePoints(points, *media, tl.rate);
    const auto dest = destinationTracks(tl, *media, points);
    if (!r || !dest)
        return {};
    return placeNewClips(tl, *media, *dest, r->sourceStart, r->recordStart, r->duration);
}

std::vector<Id> insertEdit(Timeline& tl, const MediaPool& pool, const EditPoints& points)
{
    const MediaItem* media = pool.find(points.mediaId);
    if (!media)
        return {};
    const auto r = resolvePoints(points, *media, tl.rate);
    const auto dest = destinationTracks(tl, *media, points);
    if (!r || !dest)
        return {};
    ShiftPoints shifted;
    for (const Track& t : tl.tracks) {
        const bool destination = std::find(dest->begin(), dest->end(), t.id) != dest->end();
        if (!t.locked && (destination || t.syncLock))
            shifted[t.id] = r->recordStart;
    }
    const auto sync = planSync(tl, shifted, {});
    if (!sync)
        return {}; // it would push the sound of a take away from its picture
    IdRemap links;
    for (const auto& [trackId, at] : shifted)
        tl.track(trackId)->insertSpace(at, r->duration, links);
    applySync(tl, shifted, *sync, links);
    return placeNewClips(tl, *media, *dest, r->sourceStart, r->recordStart, r->duration);
}

std::vector<Id> replaceEdit(Timeline& tl, const MediaPool& pool, const EditPoints& points)
{
    const MediaItem* media = pool.find(points.mediaId);
    if (!media)
        return {};
    const auto dest = destinationTracks(tl, *media, points);
    if (!dest)
        return {};
    const Time at = gridPoint(points.recordIn.value_or(points.playhead), tl.rate);
    const Time mediaEnd = floorToFrame(media->duration, tl.rate);

    struct Replaced {
        Id trackId;
        std::size_t index;
        Time sourceStart;
        Time duration;
        bool enabled;
    };
    std::vector<Replaced> replaced;
    std::vector<Time> starts;
    for (const Id& trackId : *dest) {
        const auto placed = tl.track(trackId)->clipAt(at);
        if (!placed)
            continue;
        replaced.push_back({trackId, placed->index, Time(0), placed->clip->sourceRange.duration, placed->clip->enabled});
        starts.push_back(placed->start);
    }
    if (replaced.empty())
        return {};
    // The source In lands on `at`; without one the media starts with the earliest replaced clip. Either
    // way picture and sound get the same offset, so a J/L cut stays in sync.
    const Time anchor = points.sourceIn ? at - gridPoint(*points.sourceIn, tl.rate)
                                        : *std::min_element(starts.begin(), starts.end());
    for (std::size_t i = 0; i < replaced.size(); ++i) {
        Replaced& r = replaced[i];
        r.sourceStart = starts[i] - anchor;
        if (r.sourceStart < Time(0) || r.sourceStart + r.duration > mediaEnd)
            return {};
    }

    const Id link = replaced.size() > 1 ? newId() : Id{};
    std::vector<Id> ids;
    for (const Replaced& r : replaced) {
        Clip clip;
        clip.id = newId();
        clip.mediaId = media->id;
        clip.name = media->name;
        clip.sourceRange = {r.sourceStart, r.duration};
        clip.enabled = r.enabled;
        clip.linkGroup = link;
        ids.push_back(clip.id);
        tl.track(r.trackId)->items[r.index] = std::move(clip); // same duration: nothing else moves
    }
    return ids;
}

std::vector<Id> fitToFillEdit(Timeline& tl, const MediaPool& pool, const EditPoints& points)
{
    const MediaItem* media = pool.find(points.mediaId);
    if (!media || !points.recordOut)
        return {};
    const Rate rate = tl.rate;
    const Time mediaEnd = floorToFrame(media->duration, rate);
    const Time sourceIn = gridPoint(points.sourceIn.value_or(Time(0)), rate);
    const Time sourceOut = std::min(gridPoint(points.sourceOut.value_or(mediaEnd), rate), mediaEnd);
    const Time recordIn = gridPoint(points.recordIn.value_or(points.playhead), rate);
    const Time recordOut = gridPoint(*points.recordOut, rate);
    const std::int64_t sourceFrames = frameAt(sourceOut, rate) - frameAt(sourceIn, rate);
    const std::int64_t recordFrames = frameAt(recordOut, rate) - frameAt(recordIn, rate);
    if (sourceFrames <= 0 || recordFrames <= 0)
        return {};
    const std::int64_t g = std::gcd(sourceFrames, recordFrames);
    if (sourceFrames / g > INT_MAX || recordFrames / g > INT_MAX)
        return {};
    const Retime speed{int(sourceFrames / g), int(recordFrames / g)};
    const Time duration = recordOut - recordIn;
    if (sourceIn + Time(mulDivFloor(duration.flicks, speed.speedNum, speed.speedDen)) > mediaEnd)
        return {};
    const auto dest = destinationTracks(tl, *media, points);
    if (!dest)
        return {};
    return placeNewClips(tl, *media, *dest, sourceIn, recordIn, duration, speed);
}

std::vector<Id> placeOnTopEdit(Timeline& tl, const MediaPool& pool, const EditPoints& points)
{
    const MediaItem* media = pool.find(points.mediaId);
    if (!media)
        return {};
    const auto r = resolvePoints(points, *media, tl.rate);
    if (!r)
        return {};
    const Time end = r->recordStart + r->duration;
    std::vector<Id> trackIds;
    for (TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const bool wanted = kind == TrackKind::Video ? media->hasVideo && !points.videoTrack.empty()
                                                     : media->hasAudio && !points.audioTrack.empty();
        if (!wanted)
            continue;
        const auto tracks = tl.tracksOfKind(kind);
        int top = -1; // highest track of this kind with a clip in the range
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (hasClipIn(*tracks[i], r->recordStart, end))
                top = int(i);
        }
        Id chosen;
        for (std::size_t i = std::size_t(top + 1); i < tracks.size(); ++i) {
            if (!tracks[i]->locked) {
                chosen = tracks[i]->id;
                break;
            }
        }
        if (chosen.empty())
            chosen = addTrack(tl, kind);
        trackIds.push_back(chosen);
    }
    if (trackIds.empty())
        return {};
    return placeNewClips(tl, *media, trackIds, r->sourceStart, r->recordStart, r->duration);
}

std::vector<Id> appendEdit(Timeline& tl, const MediaPool& pool, const EditPoints& points)
{
    const MediaItem* media = pool.find(points.mediaId);
    if (!media)
        return {};
    const auto dest = destinationTracks(tl, *media, points);
    if (!dest)
        return {};
    Time end;
    for (const Id& id : *dest)
        end = std::max(end, tl.track(id)->duration());
    EditPoints p = points;
    p.recordIn = end;
    p.recordOut.reset();
    return overwriteEdit(tl, pool, p);
}

std::vector<Id> rippleOverwriteEdit(Timeline& tl, const MediaPool& pool, const EditPoints& points)
{
    const MediaItem* media = pool.find(points.mediaId);
    if (!media)
        return {};
    const auto dest = destinationTracks(tl, *media, points);
    if (!dest)
        return {};
    const Time at = gridPoint(points.recordIn.value_or(points.playhead), tl.rate);
    const auto replaced = tl.track(dest->front())->clipAt(at);
    if (!replaced)
        return {};
    const Time start = replaced->start;
    const Id replacedId = replaced->clip->id;
    const Timeline before = tl; // the two steps are one edit: undo the first if the second fails
    if (!rippleDelete(tl, {replacedId}))
        return {};
    EditPoints p = points;
    p.recordIn = start;
    p.recordOut.reset();
    std::vector<Id> ids = insertEdit(tl, pool, p);
    if (ids.empty())
        tl = before;
    return ids;
}

// ---------------------------------------------------------------------------------------------
// Trims
// ---------------------------------------------------------------------------------------------

bool rippleTrim(Timeline& tl, const MediaPool& pool, const Id& clipId, Edge edge, Time delta, bool linked)
{
    const std::vector<Target> targets = targetsFor(tl, clipId, linked);
    if (targets.empty())
        return false;
    delta = gridDelta(delta, tl.rate);

    Time lo = -Time::max();
    Time hi = Time::max();
    for (const Target& t : targets) {
        if (edge == Edge::Tail) {
            hi = std::min(hi, handleAfter(t.item, pool, tl.rate));
            lo = std::max(lo, oneFrame(tl) - durationOf(t.item));
        } else {
            hi = std::min(hi, durationOf(t.item) - oneFrame(tl));
            lo = std::max(lo, -handleBefore(t.item, tl.rate));
        }
    }
    // Sync-locked tracks without an edge of their own only give up empty time.
    const Target& primary = targets.front();
    for (const Track& t : tl.tracks) {
        if (t.locked || !t.syncLock || targetsTrack(targets, t.id))
            continue;
        if (edge == Edge::Tail)
            lo = std::max(lo, -freeBefore(t, primary.end)); // shortening removes [end + delta, end)
        else
            hi = std::min(hi, freeAfter(t, primary.start)); // shortening removes [start, start + delta)
    }
    if (lo > hi)
        return false;
    delta = std::clamp(delta, lo, hi);
    if (delta == Time(0))
        return false;

    // Where each track starts moving. A tail trim leaves its clip where it is; a head trim moves the
    // clip's content with what follows.
    ShiftPoints shifted;
    std::map<Id, Side> fixed;
    for (const Target& t : targets) {
        shifted[t.trackId] = edge == Edge::Tail ? t.end : t.start;
        fixed[clipBase(t.item)->id] = edge == Edge::Tail ? Side::Before : Side::After;
    }
    for (const Track& t : tl.tracks) {
        if (!t.locked && t.syncLock && !targetsTrack(targets, t.id))
            shifted[t.id] = edge == Edge::Tail ? primary.end : primary.start;
    }
    const auto sync = planSync(tl, shifted, fixed);
    if (!sync)
        return false;

    IdRemap links;
    for (const Target& t : targets) {
        Track& track = *tl.track(t.trackId);
        Item item = t.item;
        if (edge == Edge::Tail && delta > Time(0)) {
            track.insertSpace(t.end, delta, links);
            moveTail(item, delta);
            track.placeItem(t.start, std::move(item), links);
        } else if (edge == Edge::Tail) {
            track.removeRange(t.end + delta, t.end, links);
        } else if (delta > Time(0)) {
            track.removeRange(t.start, t.start + delta, links); // the clip's head goes, it stays at start
        } else {
            track.insertSpace(t.start, -delta, links);
            moveHead(item, delta);
            track.placeItem(t.start, std::move(item), links);
        }
    }
    // Everything else after the edit follows on the sync-locked tracks.
    const Time shift = edge == Edge::Tail ? delta : -delta;
    const Time at = edge == Edge::Tail ? primary.end : primary.start;
    for (Track& t : tl.tracks) {
        if (t.locked || !t.syncLock || targetsTrack(targets, t.id))
            continue;
        if (shift > Time(0))
            t.insertSpace(at, shift, links);
        else if (edge == Edge::Tail)
            t.removeRange(at + shift, at, links);
        else
            t.removeRange(at, at - shift, links);
    }
    applySync(tl, shifted, *sync, links);
    return true;
}

bool rollTrim(Timeline& tl, const MediaPool& pool, const Id& clipId, Edge edge, Time delta, bool linked)
{
    const std::vector<Target> targets = targetsFor(tl, clipId, linked);
    if (targets.empty())
        return false;
    delta = gridDelta(delta, tl.rate);

    struct Cut {
        Id trackId;
        std::optional<std::size_t> left;  // the item whose tail moves
        std::optional<std::size_t> right; // the item whose head moves
    };
    std::vector<Cut> cuts;
    Time lo = -Time::max();
    Time hi = Time::max();
    for (const Target& t : targets) {
        const Track& track = *tl.track(t.trackId);
        Cut cut{t.trackId, {}, {}};
        if (edge == Edge::Tail) {
            cut.left = t.index;
            cut.right = nextItem(track, t.index);
        } else {
            cut.left = previousItem(track, t.index);
            cut.right = t.index;
        }
        if (cut.left)
            limitTail(track.items[*cut.left], pool, tl, lo, hi);
        else
            lo = std::max(lo, Time(0)); // the cut is at the start of the track
        if (cut.right)
            limitHead(track.items[*cut.right], tl, lo, hi);
        cuts.push_back(cut);
    }
    if (lo > hi)
        return false;
    delta = std::clamp(delta, lo, hi);
    if (delta == Time(0))
        return false;

    for (const Cut& cut : cuts) {
        Track& track = *tl.track(cut.trackId);
        if (cut.right)
            moveHead(track.items[*cut.right], delta);
        if (cut.left)
            moveTail(track.items[*cut.left], delta);
        else
            track.items.insert(track.items.begin(), Gap{delta});
        track.normalize();
    }
    return true;
}

bool slipClip(Timeline& tl, const MediaPool& pool, const Id& clipId, Time delta)
{
    const std::vector<Target> targets = targetsFor(tl, clipId, true);
    if (targets.empty())
        return false;
    delta = gridDelta(delta, tl.rate);
    Time lo = -Time::max();
    Time hi = Time::max();
    for (const Target& t : targets) {
        lo = std::max(lo, -handleBefore(t.item, tl.rate));
        hi = std::min(hi, handleAfter(t.item, pool, tl.rate));
    }
    if (lo > hi)
        return false;
    delta = std::clamp(delta, lo, hi);
    if (delta == Time(0))
        return false;
    for (const Target& t : targets) {
        Item& item = tl.track(t.trackId)->items[t.index];
        clipBase(item)->sourceRange.start += sourceOffset(item, delta);
    }
    return true;
}

bool slideClip(Timeline& tl, const MediaPool& pool, const Id& clipId, Time delta)
{
    const std::vector<Target> targets = targetsFor(tl, clipId, true);
    if (targets.empty())
        return false;
    delta = gridDelta(delta, tl.rate);

    struct Sides {
        Id trackId;
        std::optional<std::size_t> left;
        std::optional<std::size_t> right;
    };
    std::vector<Sides> sides;
    Time lo = -Time::max();
    Time hi = Time::max();
    for (const Target& t : targets) {
        const Track& track = *tl.track(t.trackId);
        Sides s{t.trackId, previousItem(track, t.index), nextItem(track, t.index)};
        if (s.left)
            limitTail(track.items[*s.left], pool, tl, lo, hi);
        else
            lo = std::max(lo, Time(0)); // already at the start of the track
        if (s.right)
            limitHead(track.items[*s.right], tl, lo, hi);
        sides.push_back(s);
    }
    if (lo > hi)
        return false;
    delta = std::clamp(delta, lo, hi);
    if (delta == Time(0))
        return false;

    for (const Sides& s : sides) {
        Track& track = *tl.track(s.trackId);
        if (s.right)
            moveHead(track.items[*s.right], delta);
        if (s.left)
            moveTail(track.items[*s.left], delta);
        else
            track.items.insert(track.items.begin(), Gap{delta});
        track.normalize();
    }
    return true;
}

bool rippleDelete(Timeline& tl, const std::vector<Id>& clipIds, bool linked)
{
    const std::vector<Id> ids = selectionOf(tl, clipIds, linked);
    if (ids.empty())
        return false;
    const std::set<Id> selected(ids.begin(), ids.end());

    std::map<Id, std::vector<Range>> own;
    std::vector<Range> all;
    for (const Track& t : tl.tracks) {
        for (const PlacedItem& p : t.clips()) {
            if (selected.count(p.clip->id)) {
                own[t.id].push_back({p.start, p.end()});
                all.push_back({p.start, p.end()});
            }
        }
    }
    const std::vector<Range> everywhere = mergeRanges(all);

    std::vector<std::pair<Id, std::vector<Range>>> plan;
    for (const Track& t : tl.tracks) {
        if (t.locked)
            continue;
        if (t.syncLock) {
            for (const PlacedItem& p : t.clips()) {
                if (selected.count(p.clip->id))
                    continue;
                for (const Range& r : everywhere) {
                    if (p.start < r.second && p.end() > r.first)
                        return false; // closing the hole would cut a clip of a sync-locked track
                }
            }
            plan.push_back({t.id, everywhere});
        } else if (own.count(t.id)) {
            plan.push_back({t.id, mergeRanges(own[t.id])});
        }
    }
    // Linked clips that stay must move by the same amount on the sync-locked tracks, or a hole
    // closing between the picture and the sound of a take would put them out of sync.
    if (!groupsMoveTogether(tl, everywhere, syncLockedTracks(tl), selected))
        return false;

    IdRemap links;
    for (const auto& [trackId, ranges] : plan) {
        Track& track = *tl.track(trackId);
        for (auto r = ranges.rbegin(); r != ranges.rend(); ++r) // from the end: earlier ranges stay put
            track.removeRange(r->first, r->second, links);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Gaps and order
// ---------------------------------------------------------------------------------------------

bool insertGap(Timeline& tl, Time at, Time length)
{
    at = gridPoint(at, tl.rate);
    length = floorToFrame(std::max(length, Time(0)), tl.rate);
    if (length <= Time(0))
        return false;
    ShiftPoints shifted;
    for (const Track& t : tl.tracks) {
        if (!t.locked && t.syncLock && at < t.duration())
            shifted[t.id] = at;
    }
    if (shifted.empty())
        return false;
    const auto sync = planSync(tl, shifted, {});
    if (!sync)
        return false;
    IdRemap links;
    for (const auto& [trackId, point] : shifted)
        tl.track(trackId)->insertSpace(point, length, links);
    applySync(tl, shifted, *sync, links);
    return true;
}

bool deleteGap(Timeline& tl, const Id& trackId, Time at)
{
    const Track* track = tl.track(trackId);
    if (!track || track->locked)
        return false;
    std::optional<Range> gap;
    Time pos;
    for (const Item& item : track->items) {
        const Time d = durationOf(item);
        if (std::holds_alternative<Gap>(item) && pos <= at && at < pos + d)
            gap = Range{pos, pos + d};
        pos += d;
    }
    if (!gap)
        return false;
    std::set<Id> affected{trackId};
    if (track->syncLock) {
        for (const Track& t : tl.tracks) {
            if (t.locked || !t.syncLock || t.id == trackId)
                continue;
            if (hasClipIn(t, gap->first, gap->second))
                return false; // that track would lose sync
            affected.insert(t.id);
        }
    }
    if (!groupsMoveTogether(tl, {*gap}, affected, {}))
        return false;
    IdRemap links;
    for (const Id& id : affected)
        tl.track(id)->removeRange(gap->first, gap->second, links);
    return true;
}

bool deleteGaps(Timeline& tl)
{
    const std::set<Id> affected = syncLockedTracks(tl);
    std::vector<Range> busy;
    for (const Track& t : tl.tracks) {
        if (!affected.count(t.id))
            continue;
        for (const PlacedItem& p : t.clips())
            busy.push_back({p.start, p.end()});
    }
    std::vector<Range> gaps;
    Time pos;
    for (const Range& r : mergeRanges(busy)) {
        if (r.first > pos)
            gaps.push_back({pos, r.first});
        pos = std::max(pos, r.second);
    }
    if (gaps.empty() || !groupsMoveTogether(tl, gaps, affected, {}))
        return false;
    IdRemap links;
    for (const Id& id : affected) {
        Track& track = *tl.track(id);
        for (auto r = gaps.rbegin(); r != gaps.rend(); ++r)
            track.removeRange(r->first, r->second, links);
    }
    return true;
}

bool swapClips(Timeline& tl, const Id& clipId, int direction)
{
    if (direction == 0)
        return false;
    const std::vector<Target> targets = targetsFor(tl, clipId, true);
    if (targets.empty())
        return false;

    struct Swap {
        Id trackId;
        std::size_t self;     // index of the clip
        std::size_t other;    // index of the neighbour clip
        Time selfShift;       // how far each one moves
        Time otherShift;
    };
    std::vector<Swap> swaps;
    Id neighbourGroup;
    std::set<Id> neighbours;
    for (const Target& t : targets) {
        const Track& track = *tl.track(t.trackId);
        std::optional<std::size_t> other;
        for (std::size_t i = t.index; direction > 0 ? i + 1 < track.items.size() : i > 0;) {
            i = direction > 0 ? i + 1 : i - 1;
            if (clipBase(track.items[i])) {
                other = i;
                break;
            }
        }
        if (!other)
            return false;
        const ClipBase* n = clipBase(track.items[*other]);
        if (&t == &targets.front())
            neighbourGroup = n->linkGroup;
        else if (neighbourGroup.empty() || n->linkGroup != neighbourGroup)
            return false; // a partner would swap with something unrelated
        neighbours.insert(n->id);
        // Positions: the clip, what lies between, and the neighbour exchange places in that span.
        const auto self = track.findClip(clipBase(t.item)->id);
        const auto placed = track.findClip(n->id);
        Swap s{t.trackId, t.index, *other, Time(0), Time(0)};
        if (direction > 0) {
            s.selfShift = placed->end() - self->end();
            s.otherShift = Time(0) - (placed->start - self->start);
        } else {
            s.selfShift = Time(0) - (self->start - placed->start);
            s.otherShift = self->end() - placed->end();
        }
        swaps.push_back(s);
    }
    // The neighbour's own partners must all be among the swapped neighbours, and each take must
    // move by one amount on every track.
    if (!neighbourGroup.empty()) {
        for (const Id& id : linkedGroup(tl, *neighbours.begin())) {
            if (!neighbours.count(id))
                return false;
        }
    }
    for (const Swap& s : swaps) {
        if (s.selfShift != swaps.front().selfShift || s.otherShift != swaps.front().otherShift)
            return false;
    }

    for (const Swap& s : swaps) {
        Track& track = *tl.track(s.trackId);
        std::swap(track.items[s.self], track.items[s.other]);
        track.normalize(); // transitions that no longer sit between their clips go away
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Compound clips
// ---------------------------------------------------------------------------------------------

std::optional<Id> composeClips(Project& project, const std::vector<Id>& clipIds, std::string name)
{
    Timeline& tl = project.timeline();
    const std::vector<Id> ids = expandLinked(tl, clipIds);
    if (ids.empty())
        return std::nullopt;
    const std::set<Id> selected(ids.begin(), ids.end());

    Time from = Time::max();
    Time to;
    std::map<TrackKind, std::pair<int, int>> span; // lowest and highest track with selected clips
    for (const Track& t : tl.tracks) {
        for (const PlacedItem& p : t.clips()) {
            if (!selected.count(p.clip->id))
                continue;
            from = std::min(from, p.start);
            to = std::max(to, p.end());
            const int index = kindIndex(tl, t.id);
            auto it = span.try_emplace(t.kind, index, index).first;
            it->second.first = std::min(it->second.first, index);
            it->second.second = std::max(it->second.second, index);
        }
    }
    // Every clip of the spanned tracks inside the range must be selected, or the nested timeline
    // would change what is shown (stacking) or cut clips that stay outside.
    for (const auto& [kind, s] : span) {
        bool anyEnabled = false;
        for (int i = s.first; i <= s.second; ++i) {
            const Track& t = *trackOfKind(tl, kind, i);
            anyEnabled |= t.enabled;
            for (const PlacedItem& p : t.clips()) {
                if (p.start < to && p.end() > from && !selected.count(p.clip->id))
                    return std::nullopt;
            }
        }
        // The compound sits on the lowest track: if that one is off, it would hide the others.
        if (anyEnabled && !trackOfKind(tl, kind, s.first)->enabled)
            return std::nullopt;
    }

    Timeline nested;
    nested.id = newId();
    nested.name = name.empty() ? "Compound Clip " + std::to_string(project.timelines.size()) : std::move(name);
    nested.rate = tl.rate;
    nested.width = tl.width;
    nested.height = tl.height;
    nested.sampleRate = tl.sampleRate;
    for (const auto& [kind, s] : span) { // video first (enum order)
        for (int i = s.first; i <= s.second; ++i) {
            const Track& source = *trackOfKind(tl, kind, i);
            Track copy = cropped(source, from, to);
            copy.id = newId();
            copy.locked = false;
            nested.tracks.push_back(std::move(copy));
        }
    }

    IdRemap links;
    for (const auto& [kind, s] : span) {
        for (int i = s.first; i <= s.second; ++i) {
            Track& t = *trackOfKind(tl, kind, i);
            if (!t.locked)
                t.liftRange(from, to, links);
        }
    }
    const Id link = span.size() > 1 ? newId() : Id{};
    for (const auto& [kind, s] : span) {
        CompoundClip cc;
        cc.id = newId();
        cc.name = nested.name;
        cc.timelineId = nested.id;
        cc.sourceRange = {Time(0), to - from};
        cc.linkGroup = link;
        trackOfKind(tl, kind, s.first)->placeItem(from, std::move(cc), links);
    }
    const Id nestedId = nested.id;
    project.timelines.push_back(std::move(nested)); // `tl` is invalid from here on
    return nestedId;
}

bool decomposeClip(Project& project, const Id& compoundClipId)
{
    Timeline& tl = project.timeline();
    struct Piece {
        Id trackId;
        TrackKind kind;
        int index;
        CompoundClip clip;
        Time start;
    };
    std::vector<Piece> pieces;
    for (const Target& t : targetsFor(tl, compoundClipId, true)) {
        if (const CompoundClip* cc = std::get_if<CompoundClip>(&t.item)) {
            const Track& track = *tl.track(t.trackId);
            pieces.push_back({t.trackId, track.kind, kindIndex(tl, t.trackId), *cc, t.start});
        }
    }
    if (pieces.empty() || pieces.front().clip.id != compoundClipId)
        return false;

    // What each piece shows of its nested timeline, track by track, from the piece's track up.
    struct Placement {
        TrackKind kind;
        int index;
        Time start;
        Id timelineId;
        Track content;
    };
    std::vector<Placement> placements;
    for (const Piece& piece : pieces) {
        const Timeline* nested = project.findTimeline(piece.clip.timelineId);
        if (!nested || nested->id == tl.id)
            return false;
        const TimeRange shown = piece.clip.sourceRange;
        int k = 0;
        for (const Track* nt : nested->tracksOfKind(piece.kind)) {
            Track content = cropped(*nt, shown.start, shown.end());
            const Track* target = trackOfKind(tl, piece.kind, piece.index + k);
            if (target && target->locked && !content.clips().empty())
                return false;
            placements.push_back({piece.kind, piece.index + k, piece.start, nested->id, std::move(content)});
            ++k;
        }
    }

    std::set<Id> pieceIds;
    for (const Piece& piece : pieces)
        pieceIds.insert(piece.clip.id);
    std::set<Id> existing; // clip ids that stay in the timeline: placed clips must not repeat them
    for (const Track& t : tl.tracks) {
        for (const PlacedItem& p : t.clips()) {
            if (!pieceIds.count(p.clip->id))
                existing.insert(p.clip->id);
        }
    }

    IdRemap links;
    IdRemap fresh;
    for (const Piece& piece : pieces)
        tl.track(piece.trackId)->liftRange(piece.start, piece.start + piece.clip.sourceRange.duration, links);
    for (const Placement& pl : placements) {
        // The clips keep their ids, unless another compound still shows the same nested timeline.
        const bool keepIds = countReferences(project, pl.timelineId, pieceIds) == 0;
        Track* target = trackOfKind(tl, pl.kind, pl.index);
        while (!target) {
            addTrack(tl, pl.kind);
            target = trackOfKind(tl, pl.kind, pl.index);
        }
        std::map<Id, Id> renamed;
        for (const PlacedItem& p : pl.content.clips()) {
            Item item = *p.item;
            ClipBase* c = clipBase(item);
            const Id old = c->id;
            if (!keepIds || existing.count(c->id)) {
                c->id = newId();
                c->linkGroup = fresh(c->linkGroup);
            }
            existing.insert(c->id);
            renamed[old] = c->id;
            target->placeItem(pl.start + p.start, std::move(item), links);
        }
        // Transitions between two clips that are neighbours again.
        const auto& items = pl.content.items;
        for (std::size_t i = 1; i + 1 < items.size(); ++i) {
            const Transition* tr = std::get_if<Transition>(&items[i]);
            const ClipBase* left = clipBase(items[i - 1]);
            const ClipBase* right = clipBase(items[i + 1]);
            if (!tr || !left || !right)
                continue;
            const auto placedLeft = target->findClip(renamed[left->id]);
            if (!placedLeft || placedLeft->index + 1 >= target->items.size())
                continue;
            const ClipBase* next = clipBase(target->items[placedLeft->index + 1]);
            if (next && next->id == renamed[right->id]) {
                Transition copy = *tr;
                if (!keepIds)
                    copy.id = newId();
                target->items.insert(target->items.begin() + std::ptrdiff_t(placedLeft->index + 1), copy);
            }
        }
    }

    // Nested timelines nothing refers to any more go away (`tl` is invalid after an erase).
    const Id current = tl.id;
    for (const Piece& piece : pieces) {
        const Id& nestedId = piece.clip.timelineId;
        if (nestedId == current || countReferences(project, nestedId, {}) > 0)
            continue;
        auto& timelines = project.timelines;
        timelines.erase(std::remove_if(timelines.begin(), timelines.end(), [&](const Timeline& t) { return t.id == nestedId; }),
                        timelines.end());
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Timelines of the project
// ---------------------------------------------------------------------------------------------

namespace {

bool timelineNameTaken(const Project& project, const std::string& name, const Id& except = {})
{
    return std::any_of(project.timelines.begin(), project.timelines.end(),
                       [&](const Timeline& t) { return t.id != except && t.name == name; });
}

std::string uniqueTimelineName(const Project& project, const std::string& wanted)
{
    if (!timelineNameTaken(project, wanted))
        return wanted;
    for (int n = 2;; ++n) {
        const std::string candidate = wanted + " " + std::to_string(n);
        if (!timelineNameTaken(project, candidate))
            return candidate;
    }
}

} // namespace

Id newTimeline(Project& project, std::string name)
{
    const Timeline& current = project.timeline();
    Timeline tl = makeDefaultTimeline();
    tl.rate = current.rate;
    tl.width = current.width;
    tl.height = current.height;
    tl.sampleRate = current.sampleRate;
    tl.name = uniqueTimelineName(project, name.empty() ? "Timeline " + std::to_string(project.timelines.size() + 1) : name);
    const Id id = tl.id;
    project.timelines.push_back(std::move(tl));
    project.currentTimeline = id;
    return id;
}

Id duplicateTimeline(Project& project, const Id& timelineId, std::string name)
{
    const Timeline* source = project.findTimeline(timelineId);
    if (!source)
        return {};
    Timeline copy = *source;
    copy.id = newId();
    copy.name = uniqueTimelineName(project, name.empty() ? source->name + " copy" : name);
    const Id id = copy.id;
    project.timelines.push_back(std::move(copy)); // `source` is invalid from here on
    project.currentTimeline = id;
    return id;
}

bool setCurrentTimeline(Project& project, const Id& timelineId)
{
    if (!project.findTimeline(timelineId) || project.currentTimeline == timelineId)
        return false;
    project.currentTimeline = timelineId;
    return true;
}

bool renameTimeline(Project& project, const Id& timelineId, const std::string& name)
{
    Timeline* tl = project.findTimeline(timelineId);
    if (!tl || name.empty() || tl->name == name || timelineNameTaken(project, name, timelineId))
        return false;
    tl->name = name;
    return true;
}

bool deleteTimeline(Project& project, const Id& timelineId)
{
    if (project.timelines.size() <= 1 || !project.findTimeline(timelineId) || countReferences(project, timelineId, {}) > 0)
        return false;
    auto& timelines = project.timelines;
    timelines.erase(std::remove_if(timelines.begin(), timelines.end(), [&](const Timeline& t) { return t.id == timelineId; }),
                    timelines.end());
    if (project.currentTimeline == timelineId)
        project.currentTimeline = timelines.front().id;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Markers
// ---------------------------------------------------------------------------------------------

namespace {

void insertSorted(std::vector<Marker>& markers, Marker m)
{
    auto pos = std::upper_bound(markers.begin(), markers.end(), m.start,
                                [](Time t, const Marker& other) { return t < other.start; });
    markers.insert(pos, std::move(m));
}

} // namespace

Id addMarker(Timeline& tl, Time at, std::string name, std::string color, Time duration)
{
    const Time start = gridPoint(at, tl.rate);
    for (const Marker& m : tl.markers) {
        if (m.start == start)
            return {};
    }
    Marker m;
    m.id = newId();
    m.start = start;
    m.duration = duration > Time(0) ? floorToFrame(duration, tl.rate) : Time(0);
    m.name = std::move(name);
    m.color = color.empty() ? "blue" : std::move(color);
    const Id id = m.id;
    insertSorted(tl.markers, std::move(m));
    return id;
}

bool moveMarker(Timeline& tl, const Id& markerId, Time to)
{
    const Time start = gridPoint(to, tl.rate);
    auto it = std::find_if(tl.markers.begin(), tl.markers.end(), [&](const Marker& m) { return m.id == markerId; });
    if (it == tl.markers.end() || it->start == start)
        return false;
    for (const Marker& m : tl.markers) {
        if (m.start == start)
            return false; // one marker per frame
    }
    Marker m = *it;
    tl.markers.erase(it);
    m.start = start;
    insertSorted(tl.markers, std::move(m));
    return true;
}

bool removeMarker(Timeline& tl, const Id& markerId)
{
    auto it = std::find_if(tl.markers.begin(), tl.markers.end(), [&](const Marker& m) { return m.id == markerId; });
    if (it == tl.markers.end())
        return false;
    tl.markers.erase(it);
    return true;
}

bool updateMarker(Timeline& tl, const Marker& marker)
{
    auto it = std::find_if(tl.markers.begin(), tl.markers.end(), [&](const Marker& m) { return m.id == marker.id; });
    if (it == tl.markers.end())
        return false;
    Marker m = marker;
    m.start = gridPoint(m.start, tl.rate);
    m.duration = m.duration > Time(0) ? floorToFrame(m.duration, tl.rate) : Time(0);
    if (m.color.empty())
        m.color = "blue";
    if (*it == m)
        return false;
    for (const Marker& other : tl.markers) {
        if (other.id != m.id && other.start == m.start)
            return false; // one marker per frame
    }
    tl.markers.erase(it);
    insertSorted(tl.markers, std::move(m));
    return true;
}

const Marker* findMarker(const Timeline& tl, const Id& markerId)
{
    for (const Marker& m : tl.markers) {
        if (m.id == markerId)
            return &m;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Invariants
// ---------------------------------------------------------------------------------------------

std::string checkInvariants(const Timeline& tl, const MediaPool& pool)
{
    std::ostringstream err;
    std::set<Id> ids;
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
            if (!ids.insert(p.clip->id).second)
                err << track.name << ": clip id " << p.clip->id << " used twice\n";
            if (p.start != floorToFrame(p.start, tl.rate) || p.end() != floorToFrame(p.end(), tl.rate))
                err << track.name << ": clip " << p.clip->id << " off the frame grid\n";
            if (p.clip->sourceRange.start < Time(0))
                err << track.name << ": clip " << p.clip->id << " starts before its media\n";
            if (sourceEnd(*p.item) > sourceLength(*p.item, pool))
                err << track.name << ": clip " << p.clip->id << " uses media past its end\n";
            if (const Clip* c = std::get_if<Clip>(p.item); c && !c->retime.isValid())
                err << track.name << ": clip " << p.clip->id << " has an invalid speed\n";
        }
    }
    for (std::size_t i = 0; i < tl.markers.size(); ++i) {
        const Marker& m = tl.markers[i];
        if (m.start < Time(0) || m.start != floorToFrame(m.start, tl.rate) || m.duration < Time(0))
            err << "marker " << m.id << " off the frame grid\n";
        if (i > 0 && !(tl.markers[i - 1].start < m.start))
            err << "markers not sorted at " << i << "\n";
    }
    return err.str();
}

namespace {

struct SyncProblem {
    Id clipId;
    Id otherId;
    bool sameTrack = false; // otherwise out of sync
};

std::vector<SyncProblem> syncProblems(const Timeline& tl)
{
    // Up to a sample at 192 kHz of disagreement for retimed clips, whose source times are floored.
    constexpr std::int64_t kRetimedTolerance = kFlicksPerSecond / 192000;
    struct Member {
        Id trackId;
        Id clipId;
        std::int64_t num;
        std::int64_t den;
        Time origin; // media time at timeline zero
    };
    std::map<Id, std::vector<Member>> groups;
    for (const Track& t : tl.tracks) {
        for (const PlacedItem& p : t.clips()) {
            if (p.clip->linkGroup.empty())
                continue;
            std::int64_t num = 1;
            std::int64_t den = 1;
            if (const Clip* c = std::get_if<Clip>(p.item); c && c->retime.isValid()) {
                const std::int64_t g = std::gcd(c->retime.speedNum, c->retime.speedDen);
                num = c->retime.speedNum / g;
                den = c->retime.speedDen / g;
            }
            const Time origin = p.clip->sourceRange.start - Time(mulDivFloor(p.start.flicks, num, den));
            groups[p.clip->linkGroup].push_back({t.id, p.clip->id, num, den, origin});
        }
    }
    std::vector<SyncProblem> out;
    for (const auto& [group, members] : groups) {
        std::set<Id> tracks;
        const Member& first = members.front();
        for (const Member& m : members) {
            if (!tracks.insert(m.trackId).second)
                out.push_back({m.clipId, first.clipId, true});
            const std::int64_t tolerance = m.num == m.den ? 0 : kRetimedTolerance;
            if (m.num != first.num || m.den != first.den || std::llabs((m.origin - first.origin).flicks) > tolerance)
                out.push_back({m.clipId, first.clipId, false});
        }
    }
    return out;
}

} // namespace

std::string checkSync(const Timeline& tl)
{
    std::ostringstream err;
    for (const SyncProblem& p : syncProblems(tl)) {
        if (p.sameTrack)
            err << "clip " << p.clipId << " shares a track with its linked clip " << p.otherId << "\n";
        else
            err << "clip " << p.clipId << " is out of sync with " << p.otherId << "\n";
    }
    return err.str();
}

std::vector<Id> outOfSyncClips(const Timeline& tl)
{
    std::set<Id> ids;
    for (const SyncProblem& p : syncProblems(tl)) {
        if (!p.sameTrack) {
            ids.insert(p.clipId);
            ids.insert(p.otherId);
        }
    }
    return {ids.begin(), ids.end()};
}

} // namespace ve::edit
