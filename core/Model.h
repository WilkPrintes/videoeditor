#pragma once

// Editorial data model (spec §5.2), modelled after OpenTimelineIO:
//
//   Project ─ MediaPool ─ Folder* ─ MediaItem
//           └ Timeline* ─ Track* (video/audio) ─ Item*
//   Item = Clip | Gap | Transition | CompoundClip | MulticamClip
//
// A track is a *sequence*: an item's timeline start is the sum of the durations before it, so
// empty space is an explicit Gap and two items can never overlap. Transitions take no time of
// their own (they overlap their neighbours, as in OTIO). All times are flicks (core/Time.h).

#include "core/Id.h"
#include "core/Time.h"

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ve {

// ---------------------------------------------------------------------------------------------
// Media
// ---------------------------------------------------------------------------------------------

struct MediaItem {
    Id id;
    std::string path; // UTF-8, absolute
    std::string name;
    Time duration;
    Time startTimecode; // source timecode of the first frame (zero when the file has none)

    bool hasVideo = false;
    int width = 0;
    int height = 0;
    Rate frameRate{};
    std::string videoCodec;

    bool hasAudio = false;
    int sampleRate = 0;
    int channels = 0;
    std::string audioCodec;

    std::map<std::string, std::string> metadata;
    std::string proxyPath; // empty until a proxy exists

    bool operator==(const MediaItem&) const = default;
};

struct Folder {
    Id id;
    std::string name;
    std::vector<Id> media; // MediaItem ids
    std::vector<Folder> folders;

    bool operator==(const Folder&) const = default;
};

struct MediaPool {
    std::vector<MediaItem> items; // storage; folders refer to items by id
    Folder root{"", "Master", {}, {}};

    const MediaItem* find(const Id& id) const;
    bool operator==(const MediaPool&) const = default;
};

// ---------------------------------------------------------------------------------------------
// Animation and effects
// ---------------------------------------------------------------------------------------------

enum class Interpolation { Linear, Hold, Bezier };

struct Keyframe {
    Time time; // relative to the start of the item that owns the parameter
    double value = 0.0;
    Interpolation interpolation = Interpolation::Linear;

    bool operator==(const Keyframe&) const = default;
};

// A parameter that is either constant or a keyframe curve (spec §5.2, §6.6).
struct AnimatedParam {
    double value = 0.0;          // used when there are no keyframes
    std::vector<Keyframe> keys;  // sorted by time

    bool isAnimated() const { return !keys.empty(); }
    // Linear and hold today; Bézier is evaluated as linear until the curve editor lands.
    double evaluate(Time t) const;
    bool operator==(const AnimatedParam&) const = default;
};

struct Effect {
    Id id;
    std::string type; // e.g. "transform", "opacity"
    bool enabled = true;
    std::map<std::string, AnimatedParam> params;

    bool operator==(const Effect&) const = default;
};

// Colour grade of a clip: the node list of its grading graph (spec §8). Empty = no grade.
struct Grade {
    std::vector<Effect> nodes;
    bool operator==(const Grade&) const = default;
};

// Constant speed for now (num/den, 1/1 = normal). Keyframed TimeMaps come with spec §6.5; edit
// operations currently assume 1/1.
struct Retime {
    int speedNum = 1;
    int speedDen = 1;
    bool operator==(const Retime&) const = default;
};

// ---------------------------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------------------------

// What every clip-like item has. Its timeline duration is sourceRange.duration.
struct ClipBase {
    Id id;
    std::string name;
    TimeRange sourceRange; // media time (for compounds: time in the nested timeline)
    bool enabled = true;
    Id linkGroup;          // items sharing a non-empty group edit together (picture + sound)

    bool operator==(const ClipBase&) const = default;
};

struct Clip : ClipBase {
    Id mediaId;
    std::vector<Effect> effects;
    Grade grade;
    Retime retime;

    bool operator==(const Clip&) const = default;
};

// A clip whose source is another timeline of the project (spec §6.3).
struct CompoundClip : ClipBase {
    Id timelineId;
    bool operator==(const CompoundClip&) const = default;
};

// Synchronized angles; activeAngle selects the picture/sound (spec §6.4).
struct MulticamClip : ClipBase {
    std::vector<Id> angles; // media ids
    int activeAngle = 0;
    bool operator==(const MulticamClip&) const = default;
};

struct Gap {
    Time duration;
    bool operator==(const Gap&) const = default;
};

enum class TransitionAlignment { Centered, StartOnCut, EndOnCut };

struct Transition {
    Id id;
    std::string kind = "dissolve";
    Time inOffset;  // taken from the outgoing item
    Time outOffset; // taken from the incoming item
    TransitionAlignment alignment = TransitionAlignment::Centered;

    bool operator==(const Transition&) const = default;
};

using Item = std::variant<Clip, Gap, Transition, CompoundClip, MulticamClip>;

Time durationOf(const Item& item);
const ClipBase* clipBase(const Item& item); // nullptr for gaps and transitions
ClipBase* clipBase(Item& item);

// ---------------------------------------------------------------------------------------------
// Tracks and timelines
// ---------------------------------------------------------------------------------------------

enum class TrackKind { Video, Audio };

struct PlacedItem {
    const ClipBase* clip = nullptr; // the clip-like item
    const Item* item = nullptr;
    Time start;
    std::size_t index = 0; // position in Track::items

    Time end() const { return start + clip->sourceRange.duration; }
};

struct Track {
    Id id;
    TrackKind kind = TrackKind::Video;
    std::string name;
    std::vector<Item> items;
    bool enabled = true;
    bool locked = false;  // edits skip locked tracks
    bool syncLock = true; // ripple edits shift this track too (spec §6.1)

    Time duration() const;
    std::vector<PlacedItem> clips() const; // clip-like items with their timeline start
    std::optional<PlacedItem> clipAt(Time t) const;
    std::optional<PlacedItem> findClip(const Id& clipId) const;

    // Editing primitives; all leave the track normalized.
    // Replaces [start, end) with empty space, extending the track with a gap if needed.
    void liftRange(Time start, Time end, IdRemap& links);
    // Overwrite: whatever was under [start, start + duration) is replaced by `item`.
    void placeItem(Time start, Item item, IdRemap& links);
    // Splits the clip-like item strictly spanning t. False when nothing was split.
    bool splitAt(Time t, IdRemap& links);
    // Merges adjacent gaps, drops empty items, dangling transitions and trailing gaps.
    void normalize();

    bool operator==(const Track&) const = default;

private:
    void liftRangeKeepTail(Time start, Time end, IdRemap& links);
    void normalizeImpl(bool trimTrailingGaps);
};

struct Timeline {
    Id id;
    std::string name = "Timeline 1";
    Rate rate{30, 1};
    int width = 1920;
    int height = 1080;
    int sampleRate = 48000;
    std::vector<Track> tracks; // any order; the UI shows video above audio

    Time duration() const;
    Track* track(const Id& trackId);
    const Track* track(const Id& trackId) const;
    Track* firstTrack(TrackKind kind);
    std::vector<const Track*> tracksOfKind(TrackKind kind) const;
    bool hasClips() const;

    struct ClipLocation {
        Track* track = nullptr;
        Item item;
        Time start;
        const ClipBase& clip() const { return *clipBase(item); }
        Time end() const { return start + clip().sourceRange.duration; }
    };
    std::optional<ClipLocation> locateClip(const Id& clipId);

    bool operator==(const Timeline&) const = default;
};

struct Project {
    std::string name = "Untitled";
    MediaPool media;
    std::vector<Timeline> timelines;
    Id currentTimeline;

    Timeline& timeline();
    const Timeline& timeline() const;
    Timeline* findTimeline(const Id& id);

    bool operator==(const Project&) const = default;
};

Timeline makeDefaultTimeline(); // V1 + A1
Project makeEmptyProject();

} // namespace ve
