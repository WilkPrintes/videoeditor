#pragma once

// Timeline edit operations (spec §6). Plain functions over the model, testable without UI; undo and
// transactions wrap them in TimelineEditCommand / ProjectEditCommand (core/Commands.h).
//
// Rules every operation follows:
// - Validated and atomic: everything is checked before the model is touched, so a refused edit
//   (false / empty result) changes nothing. Interactive trims clamp to what is possible instead.
// - Locked tracks are never modified.
// - Sync lock: edits that add or remove time (insert, ripple trims, ripple delete) shift every
//   unlocked track with syncLock on, not only the edited ones, so what is after the edit stays in
//   sync. A sync-locked track can always receive empty time (what spans the edit point is cut), but
//   it only gives up empty time: when it holds media there, the edit is refused or clamped.
// - Linked clips (same linkGroup: picture and sound of one take) edit together unless the call asks
//   for one track only, which is how J and L cuts are made.
//
// Positions passed in are flicks and are expected on the timeline's frame grid (the UI snaps);
// deltas are rounded towards zero to whole frames.

#include "core/Model.h"

#include <optional>
#include <string>
#include <vector>

namespace ve::edit {

enum class Edge { Head, Tail };

// ---------------------------------------------------------------------------------------------
// Tracks
// ---------------------------------------------------------------------------------------------

// Adds an empty track above the others of its kind (V2 over V1; A2 after A1) and returns its id.
Id addTrack(Timeline& tl, TrackKind kind);
// Track switches. False when the track does not exist or already has that value.
bool setTrackLocked(Timeline& tl, const Id& trackId, bool locked);
bool setTrackSyncLock(Timeline& tl, const Id& trackId, bool syncLock);
// Position of a track among those of its kind (V1 = 0, V2 = 1...), -1 when absent; and back.
int kindIndex(const Timeline& tl, const Id& trackId);
Track* trackOfKind(Timeline& tl, TrackKind kind, int index);
const Track* trackOfKind(const Timeline& tl, TrackKind kind, int index);

// ---------------------------------------------------------------------------------------------
// Placing media, blade, move, lift
// ---------------------------------------------------------------------------------------------

// Places the media on the first unlocked video/audio tracks at `at` (overwrite) as linked clips.
// Without `at` the media goes after the last item of those tracks. Returns the new clip ids.
std::vector<Id> placeMedia(Timeline& tl, const MediaItem& media, std::optional<Time> at = std::nullopt);

// In the calls below, `linked` = false is the UI's linked selection switched off: the clips are
// edited without their linked partners (which may then fall out of sync, as in other editors).

// Blade at t. With an empty `only`, every clip under t on unlocked tracks is split; otherwise just
// those clips and (when `linked`) their linked partners.
bool splitAt(Timeline& tl, Time t, const std::vector<Id>& only = {}, bool linked = true);

// Overwrite move of the clips (and their linked partners) by `delta` in time and `trackShift`
// tracks (+1: V1 to V2 and A1 to A2). The vertical shift is reduced until every clip lands on an
// unlocked track of its kind.
bool moveClips(Timeline& tl, const std::vector<Id>& clipIds, Time delta, int trackShift = 0, bool linked = true);

// Moves one edge of a clip (and, when `linked`, the same edge of its linked partners) to `newEdge`.
// Clamped to the media handles and to neighbouring clips: this trim only consumes empty space.
bool trimClip(Timeline& tl, const MediaPool& pool, const Id& clipId, Edge edge, Time newEdge, bool linked = true);

// Lift delete (leaves a gap). Linked partners go too.
bool deleteClips(Timeline& tl, const std::vector<Id>& clipIds, bool linked = true);

// ---------------------------------------------------------------------------------------------
// Organization (spec §6.8, phase 3). Clips on locked tracks are left alone; each returns false when
// nothing changed.
// ---------------------------------------------------------------------------------------------

bool renameClip(Timeline& tl, const Id& clipId, const std::string& name);
// Colour label of the clips; an empty colour goes back to the default.
bool setClipColor(Timeline& tl, const std::vector<Id>& clipIds, const std::string& color);
// Adds or removes one flag colour on the clips; clearClipFlags removes them all.
bool setClipFlag(Timeline& tl, const std::vector<Id>& clipIds, const std::string& color, bool on);
bool clearClipFlags(Timeline& tl, const std::vector<Id>& clipIds);
bool setClipsEnabled(Timeline& tl, const std::vector<Id>& clipIds, bool enabled);
// Track names stay unique: a name another track already uses is refused.
bool renameTrack(Timeline& tl, const Id& trackId, const std::string& name);
bool setTrackColor(Timeline& tl, const Id& trackId, const std::string& color);
bool setTrackEnabled(Timeline& tl, const Id& trackId, bool enabled);
// Links the clips (at most one per track) into a new group: from then on they edit together, in
// sync or not (checkSync reports a link that is out of sync). Unlink takes them out of their groups;
// a partner left alone in its group is unlinked too.
bool linkClips(Timeline& tl, const std::vector<Id>& clipIds);
bool unlinkClips(Timeline& tl, const std::vector<Id>& clipIds);

// ---------------------------------------------------------------------------------------------
// Three and four point edits (spec §6.1)
// ---------------------------------------------------------------------------------------------

// The four points of an edit and where it goes. Missing points come from the others:
// - duration: timeline In->Out when both are set, otherwise source In->Out (by default the start
//   and the end of the media);
// - source start: source In; or source Out minus the duration (backtiming); or the media start;
// - timeline start: timeline In; or timeline Out minus the duration; or the playhead.
// With all four set the timeline range wins and the source Out is ignored (fit to fill honours all
// four by changing the speed). Points are floored to the timeline's frame grid.
struct EditPoints {
    Id mediaId;
    std::optional<Time> sourceIn;
    std::optional<Time> sourceOut;
    std::optional<Time> recordIn;
    std::optional<Time> recordOut;
    Time playhead;
    Id videoTrack; // where the picture goes (empty: no picture)
    Id audioTrack; // where the sound goes (empty: no sound)
};

struct ResolvedPoints {
    Time sourceStart;
    Time recordStart;
    Time duration;
};
// Nothing when the points are inconsistent or ask for media the item does not have.
std::optional<ResolvedPoints> resolvePoints(const EditPoints& points, const MediaItem& media, Rate rate);

// Each edit returns the ids of the new clips (linked when there is picture and sound) or nothing
// when it is refused: unknown media, invalid points, not enough media, or a destination that is
// locked, missing or of the wrong kind.

// Insert: splits what is under the timeline start and pushes everything after it right by the
// duration, on the destination tracks and on every other unlocked sync-locked track.
std::vector<Id> insertEdit(Timeline& tl, const MediaPool& pool, const EditPoints& points);
// Overwrite: replaces the range on the destination tracks; nothing else moves.
std::vector<Id> overwriteEdit(Timeline& tl, const MediaPool& pool, const EditPoints& points);
// Replace: swaps the clip under the timeline start (In or playhead) on each destination track for
// the source, keeping that clip's position and duration. With a source In, that source frame lands
// on the timeline start; without one, the media starts where the (earliest) replaced clip starts.
std::vector<Id> replaceEdit(Timeline& tl, const MediaPool& pool, const EditPoints& points);
// Fit to fill: the source In->Out (by default the whole media) fills the timeline In->Out (the Out
// is required) by changing the clip's speed, as an overwrite.
std::vector<Id> fitToFillEdit(Timeline& tl, const MediaPool& pool, const EditPoints& points);
// Place on top: the picture goes on the first unlocked video track above every video clip in the
// range, the sound on the first unlocked audio track after every audio clip in it; tracks are added
// when there is none. Only whether a destination is set matters here.
std::vector<Id> placeOnTopEdit(Timeline& tl, const MediaPool& pool, const EditPoints& points);
// Append: the source goes right after the last item of the destination tracks; the playhead and
// the timeline In/Out are ignored.
std::vector<Id> appendEdit(Timeline& tl, const MediaPool& pool, const EditPoints& points);
// Ripple overwrite: the clip under the timeline start (In or playhead) on the first destination
// track is replaced by the source and the timeline ripples by the difference in length. It is a
// ripple delete of that clip followed by an insert, as one atomic edit with the same sync rules.
std::vector<Id> rippleOverwriteEdit(Timeline& tl, const MediaPool& pool, const EditPoints& points);

// ---------------------------------------------------------------------------------------------
// Gaps and order (phase 3)
// ---------------------------------------------------------------------------------------------

// Opens `length` of empty time at `at` on every unlocked sync-locked track (what spans `at` is cut).
bool insertGap(Timeline& tl, Time at, Time length);
// Closes the gap under `at` on the track. On a sync-locked track the other unlocked sync-locked
// tracks close the same time, so they must be empty there; otherwise the edit is refused.
bool deleteGap(Timeline& tl, const Id& trackId, Time at);
// Closes every stretch of time with no clip on any unlocked sync-locked track.
bool deleteGaps(Timeline& tl);
// Swaps the clip (and its linked partners) with the next clip to the left (direction < 0) or to
// the right (direction > 0); what lies between them stays between them. Partners must swap with the
// partners of that neighbour, so that picture and sound stay in sync; otherwise it is refused.
bool swapClips(Timeline& tl, const Id& clipId, int direction);

// ---------------------------------------------------------------------------------------------
// Trims (spec §6.2). `delta` is timeline time, positive = later. Each clamps to what the media
// handles, the neighbours and the sync-locked tracks allow and is false when nothing can move.
// ---------------------------------------------------------------------------------------------

// Ripple: the edge moves and the clip changes length; everything after the edit shifts by the same
// amount, on the clip's track and on every unlocked sync-locked track. A head ripple keeps the clip's
// start in place (the clip gains or loses frames at its head and what follows moves). With
// `linked` false only this track's clip is trimmed (J/L cuts).
bool rippleTrim(Timeline& tl, const MediaPool& pool, const Id& clipId, Edge edge, Time delta, bool linked = true);
// Roll: the cut between the clip and its neighbour on that side moves; the neighbour's edge follows,
// so nothing else moves. With `linked` false only this track's cut moves (J/L cuts).
bool rollTrim(Timeline& tl, const MediaPool& pool, const Id& clipId, Edge edge, Time delta, bool linked = true);
// Slip: the clip shows other media (source start += delta, scaled by its speed); position and
// duration stay. Linked partners slip too, so picture and sound stay in sync.
bool slipClip(Timeline& tl, const MediaPool& pool, const Id& clipId, Time delta);
// Slide: the clip moves between its neighbours, which give or take the time (left neighbour's
// tail and right neighbour's head move with it). Linked partners slide too.
bool slideClip(Timeline& tl, const MediaPool& pool, const Id& clipId, Time delta);
// Ripple delete: removes the clips (and linked partners) and closes the hole. Every unlocked
// sync-locked track loses the same time ranges, and those ranges may only hold selected clips or
// empty space there (otherwise the delete is refused). Tracks without sync lock only close the holes
// of their own clips.
bool rippleDelete(Timeline& tl, const std::vector<Id>& clipIds, bool linked = true);

// ---------------------------------------------------------------------------------------------
// Compound clips (spec §6.3). They need the project: the nested timeline lives in it.
// ---------------------------------------------------------------------------------------------

// Moves the selected clips (and linked partners) of the current timeline into a new timeline and
// puts a compound clip in their place: on the lowest selected video track and on the lowest selected
// audio track, linked to each other. The nested timeline keeps the tracks between the lowest and the
// highest selected one, so the stacking order does not change. Refused when an unselected clip on
// those tracks overlaps the selection's range. Returns the new timeline's id.
std::optional<Id> composeClips(Project& project, const std::vector<Id>& clipIds, std::string name = {});
// The opposite: the compound clip (and its linked compound partner) is replaced by the part of the
// nested timeline it shows, on its track and the ones above it (added when missing). The nested
// timeline is dropped when nothing refers to it any more, so compose + decompose gives back exactly
// the same project.
bool decomposeClip(Project& project, const Id& compoundClipId);

// ---------------------------------------------------------------------------------------------
// Timelines of the project (phase 3)
// ---------------------------------------------------------------------------------------------

// A new empty timeline (V1 + A1, same rate and size as the current one) that becomes current.
Id newTimeline(Project& project, std::string name = {});
// A copy of the timeline (same clips) that becomes current.
Id duplicateTimeline(Project& project, const Id& timelineId, std::string name = {});
bool setCurrentTimeline(Project& project, const Id& timelineId);
// Names stay unique; an empty or taken name is refused.
bool renameTimeline(Project& project, const Id& timelineId, const std::string& name);
// Refused for the last timeline and for one a compound clip shows.
bool deleteTimeline(Project& project, const Id& timelineId);

// ---------------------------------------------------------------------------------------------
// Markers (spec §6.8)
// ---------------------------------------------------------------------------------------------

// Adds a marker on the frame at `at`. Empty result when there is already one starting there.
Id addMarker(Timeline& tl, Time at, std::string name = {}, std::string color = "blue", Time duration = Time(0));
bool moveMarker(Timeline& tl, const Id& markerId, Time to);
bool removeMarker(Timeline& tl, const Id& markerId);
// Replaces the name, note, colour, start and duration of the marker with that id (start and
// duration go to the frame grid; a start another marker uses is refused).
bool updateMarker(Timeline& tl, const Marker& marker);
const Marker* findMarker(const Timeline& tl, const Id& markerId);

// ---------------------------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------------------------

// The clip plus every clip sharing its link group.
std::vector<Id> linkedGroup(const Timeline& tl, const Id& clipId);
std::vector<Id> expandLinked(const Timeline& tl, const std::vector<Id>& clipIds);

// Every clip start and end in the timeline, sorted and unique (navigation, snapping).
std::vector<Time> editPoints(const Timeline& tl);

// Invariants every edit must keep; used by the property tests. Empty string = all good.
std::string checkInvariants(const Timeline& tl, const MediaPool& pool);
// Linked clips in sync: every member of a link group maps timeline time to media time the same way
// (same speed, same offset) and a group has at most one clip per track. J/L cuts are in sync (only
// the edges differ); out of sync happens only when a locked or non-sync-locked track stays behind.
std::string checkSync(const Timeline& tl);
// The clips of link groups that are out of sync (the UI marks them).
std::vector<Id> outOfSyncClips(const Timeline& tl);

} // namespace ve::edit
