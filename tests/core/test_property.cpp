// Property test (spec §6.2): after any random sequence of edits, the timeline keeps its invariants
// (frame grid, media handles, no overlaps or leftover gaps, linked picture and sound aligned), and
// undoing everything returns exactly to the initial state.

#include "core/Commands.h"

#include <catch_amalgamated.hpp>

#include "test_support.h"

#include <random>

using namespace ve;
using namespace ve::test;

namespace {

Id randomClip(const Timeline& tl, std::mt19937_64& rng)
{
    std::vector<Id> ids;
    for (const Track& t : tl.tracks) {
        for (const PlacedItem& p : t.clips())
            ids.push_back(p.clip->id);
    }
    if (ids.empty())
        return {};
    return ids[std::uniform_int_distribution<std::size_t>(0, ids.size() - 1)(rng)];
}

void runRandomEdits(std::uint64_t seed, int steps)
{
    std::mt19937_64 rng(seed);
    Project project = makeEmptyProject();
    project.timeline().rate = kRate;
    const std::vector<MediaItem> media = {testMedia(10.0), testMedia(4.0), testMedia(7.5)};
    for (const MediaItem& m : media)
        project.media.items.push_back(m);
    const Project initial = project;

    CommandStack stack(project);
    auto frames = [&](int lo, int hi) { return F(std::uniform_int_distribution<int>(lo, hi)(rng)); };
    int applied = 0;

    for (int step = 0; step < steps; ++step) {
        const int op = std::uniform_int_distribution<int>(0, 99)(rng);
        const Id clip = randomClip(project.timeline(), rng);
        bool changed = false;
        if (op < 10 || clip.empty()) {
            const MediaItem m = media[std::size_t(step) % media.size()];
            const Time at = frames(0, 900);
            changed = stack.edit("Place", [m, at](Timeline& tl, const MediaPool&) { return !edit::placeMedia(tl, m, at).empty(); });
        } else if (op < 30) {
            const Time t = frames(0, 900);
            changed = stack.edit("Split", [t](Timeline& tl, const MediaPool&) { return edit::splitAt(tl, t); });
        } else if (op < 50) {
            const Time delta = frames(-120, 120);
            changed = stack.edit("Move", [clip, delta](Timeline& tl, const MediaPool&) { return edit::moveClips(tl, {clip}, delta); });
        } else if (op < 90) {
            const auto edge = op < 70 ? edit::Edge::Head : edit::Edge::Tail;
            const Time to = frames(0, 900);
            changed = stack.edit("Trim", [clip, edge, to](Timeline& tl, const MediaPool& pool) {
                return edit::trimClip(tl, pool, clip, edge, to);
            });
        } else if (op < 95) {
            changed = stack.edit("Delete", [clip](Timeline& tl, const MediaPool&) { return edit::deleteClips(tl, {clip}); });
        } else if (stack.canUndo()) {
            stack.undo();
            changed = true;
        }
        applied += changed ? 1 : 0;

        const std::string problems = edit::checkInvariants(project.timeline(), project.media);
        INFO("seed " << seed << " step " << step << " op " << op);
        REQUIRE(problems.empty());
        REQUIRE(linkedTracksAligned(project.timeline()));
    }
    CHECK(applied > steps / 4); // the sequence really edited things

    while (stack.canUndo())
        stack.undo();
    CHECK(project.timelines == initial.timelines);
}

} // namespace

TEST_CASE("10 000 random edits keep the timeline valid and fully undoable", "[property]")
{
    for (std::uint64_t seed : {1u, 2u, 3u, 4u, 5u})
        runRandomEdits(seed, 2000);
}
