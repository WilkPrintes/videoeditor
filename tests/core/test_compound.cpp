// Compound clips (spec §6.3): composing and decomposing give back the same state, and a composed
// timeline renders the same pictures and sound as the original.

#include "core/Commands.h"
#include "core/Edit.h"
#include "core/Snapshot.h"

#include <catch_amalgamated.hpp>

#include "test_support.h"

#include <tuple>

using namespace ve;
using namespace ve::test;

namespace {

// What the renderer reads: the picture source of every frame...
std::vector<std::pair<Id, Time>> pictures(const Project& project, std::int64_t count)
{
    const auto snap = TimelineSnapshot::fromProject(project);
    std::vector<std::pair<Id, Time>> out;
    for (std::int64_t n = 0; n < count; ++n) {
        const auto src = resolveVideo(*snap, F(n));
        out.push_back(src ? std::pair(src->media->id, src->sourceTime) : std::pair(Id{}, Time(-1)));
    }
    return out;
}

// ...and the sound in mixing order: which media plays over which range, at which offset. Touching
// segments of the same media at the same offset sound the same as one (a split clip), so they merge.
// The decoder key is left out: it names the track path, which composing changes.
std::vector<std::tuple<Id, Time, Time, Time>> sound(const Project& project)
{
    const auto snap = TimelineSnapshot::fromProject(project);
    std::vector<std::tuple<Id, Time, Time, Time>> out;
    for (const AudioSegment& s : audioSegments(*snap, Time(0), snap->timeline.duration())) {
        const Time offset = s.sourceOrigin - s.origin;
        if (!out.empty()) {
            auto& [media, start, end, previousOffset] = out.back();
            if (media == s.media->id && previousOffset == offset && end == s.start) {
                end = s.end;
                continue;
            }
        }
        out.emplace_back(s.media->id, s.start, s.end, offset);
    }
    return out;
}

Id compoundOn(const Track& t)
{
    for (const PlacedItem& p : t.clips()) {
        if (std::holds_alternative<CompoundClip>(*p.item))
            return p.clip->id;
    }
    return {};
}

bool allHealthy(const Project& project)
{
    std::string problems;
    for (const Timeline& tl : project.timelines)
        problems += edit::checkInvariants(tl, project.media) + edit::checkSync(tl);
    INFO(problems);
    return problems.empty();
}

} // namespace

TEST_CASE("compose then decompose gives back exactly the same project", "[compound]")
{
    MultiFixture f;
    const Project original = f.project;
    const Id y = f.clipAt("V1", 200);
    const Id title = f.clipAt("V2", 200);

    const auto nestedId = edit::composeClips(f.project, {y, title}, "Scene");
    REQUIRE(nestedId);
    REQUIRE(f.project.timelines.size() == 2);
    const Timeline* nested = f.project.findTimeline(*nestedId);
    REQUIRE(nested);
    CHECK(nested->name == "Scene");
    // The nested timeline has the spanned tracks (V1, V2, A1) with the clips moved to zero.
    REQUIRE(nested->tracks.size() == 3);
    CHECK(nested->tracks[0].clips()[0].clip->id == y);
    CHECK(nested->tracks[0].clips()[0].start == Time(0));
    CHECK(nested->tracks[1].clips()[0].start == F(50));
    CHECK(nested->tracks[2].kind == TrackKind::Audio);

    // In the parent: a compound clip on V1 and one on A1, linked; V2 is empty now.
    const Id compound = compoundOn(f.track("V1"));
    REQUIRE_FALSE(compound.empty());
    CHECK(f.track("V1").findClip(compound)->start == F(100));
    CHECK(f.track("V1").findClip(compound)->end() == F(300));
    CHECK(edit::linkedGroup(f.tl(), compound).size() == 2);
    CHECK(f.track("V2").clips().empty());
    CHECK(allHealthy(f.project));

    // The parent renders the child as a source: same pictures, same sound in the same order.
    CHECK(pictures(f.project, 620) == pictures(original, 620));
    CHECK(sound(f.project) == sound(original));

    REQUIRE(edit::decomposeClip(f.project, compound));
    CHECK(f.project == original);
}

TEST_CASE("compose refuses a selection that would change what is shown", "[compound]")
{
    MultiFixture f;
    const Project original = f.project;
    // X and the title span [0,240), but Y (not selected) is on V1 inside that range.
    CHECK_FALSE(edit::composeClips(f.project, {f.clipAt("V1", 50), f.clipAt("V2", 200)}));
    CHECK(f.project == original);
    CHECK_FALSE(edit::composeClips(f.project, {}));

    // Just the picture and sound of Y: the title stays above the compound, so nothing changes on screen.
    REQUIRE(edit::composeClips(f.project, {f.clipAt("V1", 200)}));
    CHECK(f.track("V2").clips().size() == 1);
    CHECK(pictures(f.project, 620) == pictures(original, 620));
    CHECK(sound(f.project) == sound(original));
}

TEST_CASE("a trimmed compound decomposes into the part it shows", "[compound]")
{
    MultiFixture f;
    REQUIRE(edit::composeClips(f.project, {f.clipAt("V1", 200), f.clipAt("V2", 200)}));
    const Id compound = compoundOn(f.track("V1"));
    REQUIRE(edit::trimClip(f.tl(), f.pool(), compound, edit::Edge::Tail, F(250)));
    const Project trimmed = f.project;

    REQUIRE(edit::decomposeClip(f.project, compound));
    CHECK(f.project.timelines.size() == 1); // nobody shows the nested timeline any more
    const auto y = f.track("V1").clipAt(F(200));
    REQUIRE(y);
    CHECK(y->start == F(100));
    CHECK(y->end() == F(250));
    CHECK(y->clip->sourceRange.start == F(100));
    CHECK_FALSE(f.track("V1").clipAt(F(260)));
    CHECK(f.track("V2").clips()[0].start == F(150));
    CHECK(f.track("A1").clipAt(F(200))->end() == F(250));
    CHECK(allHealthy(f.project));
    CHECK(pictures(f.project, 620) == pictures(trimmed, 620));
    CHECK(sound(f.project) == sound(trimmed));
}

TEST_CASE("pieces of a split compound decompose with unique ids", "[compound]")
{
    MultiFixture f;
    const Project original = f.project;
    const auto nestedId = edit::composeClips(f.project, {f.clipAt("V1", 200), f.clipAt("V2", 200)});
    REQUIRE(nestedId);
    REQUIRE(edit::splitAt(f.tl(), F(200), {compoundOn(f.track("V1"))}));
    const Id left = f.clipAt("V1", 150);
    const Id right = f.clipAt("V1", 250);

    REQUIRE(edit::decomposeClip(f.project, left));
    CHECK(f.project.findTimeline(*nestedId)); // the right piece still shows it
    CHECK(allHealthy(f.project));
    REQUIRE(edit::decomposeClip(f.project, right));
    CHECK_FALSE(f.project.findTimeline(*nestedId));
    CHECK(allHealthy(f.project));
    CHECK(pictures(f.project, 620) == pictures(original, 620));
    CHECK(sound(f.project) == sound(original));
}

TEST_CASE("compounds nest, and compose/decompose are single undo steps", "[compound][commands]")
{
    MultiFixture f;
    const Project original = f.project;
    CommandStack stack(f.project);
    const std::vector<Id> inner{f.clipAt("V1", 200), f.clipAt("V2", 200)};
    REQUIRE(stack.editProject("Compose", [inner](Project& p) { return edit::composeClips(p, inner).has_value(); }));
    const Project composed = f.project;

    // A compound of X and the first compound: two levels deep, still the same render.
    const std::vector<Id> outer{f.clipAt("V1", 50), compoundOn(f.track("V1"))};
    REQUIRE(stack.editProject("Compose", [outer](Project& p) { return edit::composeClips(p, outer).has_value(); }));
    CHECK(f.project.timelines.size() == 3);
    CHECK(pictures(f.project, 620) == pictures(original, 620));
    CHECK(sound(f.project) == sound(original));
    CHECK(allHealthy(f.project));

    const Id top = compoundOn(f.track("V1"));
    REQUIRE(stack.editProject("Decompose", [top](Project& p) { return edit::decomposeClip(p, top); }));
    CHECK(f.project == composed);
    stack.undo();
    stack.undo();
    CHECK(f.project.timelines.size() == 2);
    stack.undo();
    CHECK(f.project == original);
    // A refused edit leaves no undo step.
    CHECK_FALSE(stack.editProject("Decompose", [](Project& p) { return edit::decomposeClip(p, "nothing"); }));
    CHECK_FALSE(stack.canUndo());
}
