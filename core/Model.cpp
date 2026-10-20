#include "core/Model.h"

#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ve {

const MediaItem* MediaPool::find(const Id& id) const
{
    for (const MediaItem& m : items) {
        if (m.id == id)
            return &m;
    }
    return nullptr;
}

double AnimatedParam::evaluate(Time t) const
{
    if (keys.empty())
        return value;
    if (t <= keys.front().time)
        return keys.front().value;
    if (t >= keys.back().time)
        return keys.back().value;
    auto next = std::upper_bound(keys.begin(), keys.end(), t,
                                 [](Time time, const Keyframe& k) { return time < k.time; });
    const Keyframe& b = *next;
    const Keyframe& a = *(next - 1);
    if (a.interpolation == Interpolation::Hold)
        return a.value;
    const double u = double((t - a.time).flicks) / double((b.time - a.time).flicks);
    return a.value + (b.value - a.value) * u;
}

// ---------------------------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------------------------

const ClipBase* clipBase(const Item& item)
{
    return std::visit([](const auto& v) -> const ClipBase* {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_base_of_v<ClipBase, T>)
            return &v;
        else
            return nullptr;
    }, item);
}

ClipBase* clipBase(Item& item)
{
    return const_cast<ClipBase*>(clipBase(std::as_const(item)));
}

Time durationOf(const Item& item)
{
    if (const ClipBase* c = clipBase(item))
        return c->sourceRange.duration;
    if (const Gap* g = std::get_if<Gap>(&item))
        return g->duration;
    return Time(0); // transitions overlap their neighbours
}

namespace {

const Retime* retimeOf(const Item& item)
{
    const Clip* c = std::get_if<Clip>(&item);
    return c && c->retime.isValid() && !c->retime.isIdentity() ? &c->retime : nullptr;
}

} // namespace

Time sourceOffset(const Item& item, Time clipTime)
{
    if (const Retime* r = retimeOf(item))
        return Time(mulDivFloor(clipTime.flicks, r->speedNum, r->speedDen));
    return clipTime;
}

Time clipTimeFor(const Item& item, Time sourceTime)
{
    if (const Retime* r = retimeOf(item))
        return Time(mulDivFloor(sourceTime.flicks, r->speedDen, r->speedNum));
    return sourceTime;
}

Time sourceEnd(const Item& item)
{
    const ClipBase* c = clipBase(item);
    return c ? c->sourceRange.start + sourceOffset(item, c->sourceRange.duration) : Time(0);
}

// ---------------------------------------------------------------------------------------------
// Track queries
// ---------------------------------------------------------------------------------------------

Time Track::duration() const
{
    Time d;
    for (const Item& item : items)
        d += durationOf(item);
    return d;
}

std::vector<PlacedItem> Track::clips() const
{
    std::vector<PlacedItem> out;
    Time pos;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (const ClipBase* c = clipBase(items[i]))
            out.push_back({c, &items[i], pos, i});
        pos += durationOf(items[i]);
    }
    return out;
}

std::optional<PlacedItem> Track::clipAt(Time t) const
{
    for (const PlacedItem& p : clips()) {
        if (p.start <= t && t < p.end())
            return p;
    }
    return std::nullopt;
}

std::optional<PlacedItem> Track::findClip(const Id& clipId) const
{
    for (const PlacedItem& p : clips()) {
        if (p.clip->id == clipId)
            return p;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// Track editing primitives
// ---------------------------------------------------------------------------------------------

namespace {

// The part of `item` starting `offset` into it and lasting `length` (timeline time).
Item slice(const Item& item, Time offset, Time length)
{
    Item out = item;
    if (ClipBase* c = clipBase(out)) {
        c->sourceRange.start += sourceOffset(item, offset);
        c->sourceRange.duration = length;
        return out;
    }
    return Gap{length};
}

} // namespace

void Track::liftRangeKeepTail(Time start, Time end, IdRemap& links)
{
    start = std::max(start, Time(0));
    if (end <= start)
        return;

    std::vector<Item> out;
    out.reserve(items.size() + 3);
    Time pos;
    for (const Item& item : items) {
        const Time d = durationOf(item);
        const Time s = pos;
        const Time e = pos + d;
        pos = e;
        if (d == Time(0) || e <= start || s >= end) {
            out.push_back(item);
            continue;
        }
        if (s < start)
            out.push_back(slice(item, Time(0), start - s));
        out.push_back(Gap{std::min(e, end) - std::max(s, start)});
        if (e > end) {
            Item tail = slice(item, end - s, e - end);
            ClipBase* c = clipBase(tail);
            if (c && s < start) {
                // A hole was punched in the middle: the tail becomes an item of its own.
                c->id = newId();
                c->linkGroup = links(c->linkGroup);
            }
            out.push_back(std::move(tail));
        }
    }
    if (pos < end)
        out.push_back(Gap{end - pos});
    items = std::move(out);
}

void Track::liftRange(Time start, Time end, IdRemap& links)
{
    liftRangeKeepTail(start, end, links);
    normalizeImpl(true);
}

void Track::placeItem(Time start, Item item, IdRemap& links)
{
    start = std::max(start, Time(0));
    const Time length = durationOf(item);
    if (length <= Time(0))
        return;
    const Time end = start + length;

    // Clear the destination; after merging gaps it lies inside a single gap.
    liftRangeKeepTail(start, end, links);
    normalizeImpl(false);

    Time pos;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Time d = durationOf(items[i]);
        if (std::holds_alternative<Gap>(items[i]) && pos <= start && end <= pos + d) {
            std::vector<Item> replacement;
            if (start > pos)
                replacement.push_back(Gap{start - pos});
            replacement.push_back(std::move(item));
            if (pos + d > end)
                replacement.push_back(Gap{pos + d - end});
            items.erase(items.begin() + std::ptrdiff_t(i));
            items.insert(items.begin() + std::ptrdiff_t(i), replacement.begin(), replacement.end());
            break;
        }
        pos += d;
    }
    normalizeImpl(true);
}

bool Track::splitAt(Time t, IdRemap& links)
{
    Time pos;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Time d = durationOf(items[i]);
        ClipBase* c = clipBase(items[i]);
        if (c && pos < t && t < pos + d) {
            Item right = slice(items[i], t - pos, pos + d - t);
            ClipBase* r = clipBase(right);
            r->id = newId();
            r->linkGroup = links(c->linkGroup);
            c->sourceRange.duration = t - pos;
            items.insert(items.begin() + std::ptrdiff_t(i) + 1, std::move(right));
            return true;
        }
        pos += d;
    }
    return false;
}

void Track::insertSpace(Time at, Time length, IdRemap& links)
{
    at = std::max(at, Time(0));
    if (length <= Time(0) || at >= duration())
        return;
    splitAt(at, links); // now `at` is a cut or lies inside a gap
    Time pos;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Time d = durationOf(items[i]);
        if (Gap* g = std::get_if<Gap>(&items[i]); g && pos < at && at < pos + d) {
            g->duration += length;
            break;
        }
        if (pos == at && d > Time(0)) {
            items.insert(items.begin() + std::ptrdiff_t(i), Gap{length});
            break;
        }
        pos += d;
    }
    normalizeImpl(true);
}

void Track::removeRange(Time start, Time end, IdRemap& links)
{
    start = std::max(start, Time(0));
    if (end <= start || start >= duration())
        return;
    // Lift the range, then take its (now empty) time out of the gaps that cover it.
    liftRangeKeepTail(start, end, links);
    Time pos;
    for (Item& item : items) {
        const Time d = durationOf(item);
        if (Gap* g = std::get_if<Gap>(&item)) {
            const Time overlap = std::min(pos + d, end) - std::max(pos, start);
            if (overlap > Time(0))
                g->duration -= overlap;
        }
        pos += d;
    }
    normalizeImpl(true);
}

void Track::normalize()
{
    normalizeImpl(true);
}

void Track::normalizeImpl(bool trimTrailingGaps)
{
    // 1. Drop empty gaps and clips.
    std::vector<Item> nonEmpty;
    nonEmpty.reserve(items.size());
    for (Item& item : items) {
        if (!std::holds_alternative<Transition>(item) && durationOf(item) <= Time(0))
            continue;
        nonEmpty.push_back(std::move(item));
    }

    // 2. Transitions must sit between two clip-like items. 3. Merge adjacent gaps.
    std::vector<Item> out;
    out.reserve(nonEmpty.size());
    for (std::size_t i = 0; i < nonEmpty.size(); ++i) {
        Item& item = nonEmpty[i];
        if (std::holds_alternative<Transition>(item)) {
            const bool between = i > 0 && i + 1 < nonEmpty.size() && clipBase(nonEmpty[i - 1])
                && clipBase(nonEmpty[i + 1]);
            if (!between)
                continue;
        }
        if (Gap* g = std::get_if<Gap>(&item); g && !out.empty()) {
            if (Gap* prev = std::get_if<Gap>(&out.back())) {
                prev->duration += g->duration;
                continue;
            }
        }
        out.push_back(std::move(item));
    }

    // 4. A track ends with its last item.
    if (trimTrailingGaps) {
        while (!out.empty() && std::holds_alternative<Gap>(out.back()))
            out.pop_back();
    }
    items = std::move(out);
}

// ---------------------------------------------------------------------------------------------
// Timeline and project
// ---------------------------------------------------------------------------------------------

Time Timeline::duration() const
{
    Time d;
    for (const Track& t : tracks)
        d = std::max(d, t.duration());
    return d;
}

Track* Timeline::track(const Id& trackId)
{
    for (Track& t : tracks) {
        if (t.id == trackId)
            return &t;
    }
    return nullptr;
}

const Track* Timeline::track(const Id& trackId) const
{
    return const_cast<Timeline*>(this)->track(trackId);
}

Track* Timeline::firstTrack(TrackKind kind)
{
    for (Track& t : tracks) {
        if (t.kind == kind && !t.locked)
            return &t;
    }
    return nullptr;
}

std::vector<const Track*> Timeline::tracksOfKind(TrackKind kind) const
{
    std::vector<const Track*> out;
    for (const Track& t : tracks) {
        if (t.kind == kind)
            out.push_back(&t);
    }
    return out;
}

bool Timeline::hasClips() const
{
    for (const Track& t : tracks) {
        for (const Item& item : t.items) {
            if (clipBase(item))
                return true;
        }
    }
    return false;
}

std::optional<Timeline::ClipLocation> Timeline::locateClip(const Id& clipId)
{
    for (Track& t : tracks) {
        if (auto placed = t.findClip(clipId))
            return ClipLocation{&t, *placed->item, placed->start};
    }
    return std::nullopt;
}

Timeline& Project::timeline()
{
    if (Timeline* tl = findTimeline(currentTimeline))
        return *tl;
    if (timelines.empty())
        throw std::logic_error("project without timelines");
    return timelines.front();
}

const Timeline& Project::timeline() const
{
    return const_cast<Project*>(this)->timeline();
}

Timeline* Project::findTimeline(const Id& id)
{
    for (Timeline& tl : timelines) {
        if (tl.id == id)
            return &tl;
    }
    return nullptr;
}

Timeline makeDefaultTimeline()
{
    Timeline t;
    t.id = newId();

    Track video;
    video.id = newId();
    video.kind = TrackKind::Video;
    video.name = "V1";

    Track audio;
    audio.id = newId();
    audio.kind = TrackKind::Audio;
    audio.name = "A1";

    t.tracks = {std::move(video), std::move(audio)};
    return t;
}

Project makeEmptyProject()
{
    Project p;
    p.timelines.push_back(makeDefaultTimeline());
    p.currentTimeline = p.timelines.front().id;
    p.media.root.id = newId();
    return p;
}

} // namespace ve
