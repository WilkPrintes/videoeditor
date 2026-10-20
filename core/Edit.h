#pragma once

// Timeline edit operations (spec §6). Plain functions over the model, testable without UI; undo and
// transactions wrap them in TimelineEditCommand (core/Commands.h). Each one is atomic: it either
// applies completely or returns false, and the command then restores the previous state.
//
// Positions passed in are flicks and are expected on the timeline's frame grid (the UI snaps).

#include "core/Model.h"

#include <optional>
#include <vector>

namespace ve::edit {

enum class Edge { Head, Tail };

// Places the media on the first unlocked video/audio tracks at `at` (overwrite) as linked clips.
// Without `at` the media goes after the last item of those tracks. Returns the new clip ids.
std::vector<Id> placeMedia(Timeline& tl, const MediaItem& media, std::optional<Time> at = std::nullopt);

// Blade at t. With an empty `only`, every clip under t on unlocked tracks is split; otherwise just
// those clips and their linked partners.
bool splitAt(Timeline& tl, Time t, const std::vector<Id>& only = {});

// Overwrite move of the clips (and their linked partners) by `delta`.
bool moveClips(Timeline& tl, const std::vector<Id>& clipIds, Time delta);

// Moves one edge of a clip, and the same edge of its linked partners, to `newEdge`. Clamped to the
// media handles and to neighbouring clips: this trim only consumes empty space.
bool trimClip(Timeline& tl, const MediaPool& pool, const Id& clipId, Edge edge, Time newEdge);

// Lift delete (leaves a gap). Linked partners go too.
bool deleteClips(Timeline& tl, const std::vector<Id>& clipIds);

// The clip plus every clip sharing its link group.
std::vector<Id> linkedGroup(const Timeline& tl, const Id& clipId);
std::vector<Id> expandLinked(const Timeline& tl, const std::vector<Id>& clipIds);

// Invariants every edit must keep; used by the property tests. Empty string = all good.
std::string checkInvariants(const Timeline& tl, const MediaPool& pool);

} // namespace ve::edit
