// Phase 3.1: organization in the core. Clip and track labels, links, editing without the linked
// partners, marker editing and the timelines of a project.

#include "core/Commands.h"
#include "core/Edit.h"
#include "core/ProjectFile.h"

#include <catch_amalgamated.hpp>

#include "test_support.h"

using namespace ve;
using namespace ve::test;

// The fixture: V1/A1 X [0,100) Y [100,300) Z [300,420); V2 title [150,240); A2 music [0,600).

TEST_CASE("clip labels: colour, flags, name and enabled, never on locked tracks", "[organization]")
{
    MultiFixture f;
    const Id x = f.clipAt("V1", 50);
    const Id xSound = f.clipAt("A1", 50);

    CHECK(edit::setClipColor(f.tl(), {x, xSound}, "orange"));
    CHECK_FALSE(edit::setClipColor(f.tl(), {x, xSound}, "orange")); // nothing changes
    CHECK(f.track("V1").findClip(x)->clip->color == "orange");

    CHECK(edit::setClipFlag(f.tl(), {x}, "red", true));
    CHECK(edit::setClipFlag(f.tl(), {x}, "blue", true));
    CHECK_FALSE(edit::setClipFlag(f.tl(), {x}, "red", true));
    CHECK(f.track("V1").findClip(x)->clip->flags == std::vector<std::string>{"blue", "red"}); // sorted
    CHECK(edit::setClipFlag(f.tl(), {x}, "red", false));
    CHECK(edit::clearClipFlags(f.tl(), {x}));
    CHECK(f.track("V1").findClip(x)->clip->flags.empty());
    CHECK_FALSE(edit::setClipFlag(f.tl(), {x}, "", true));

    CHECK(edit::renameClip(f.tl(), x, "Plano geral"));
    CHECK(f.track("V1").findClip(x)->clip->name == "Plano geral");
    CHECK(edit::setClipsEnabled(f.tl(), {x}, false));
    CHECK_FALSE(f.track("V1").findClip(x)->clip->enabled);

    f.track("A1").locked = true;
    CHECK_FALSE(edit::setClipColor(f.tl(), {xSound}, "green"));
    CHECK_FALSE(edit::renameClip(f.tl(), xSound, "x"));
    CHECK(edit::checkInvariants(f.tl(), f.pool()).empty());
}

TEST_CASE("track labels: names stay unique, colour and enabled switch", "[organization][tracks]")
{
    MultiFixture f;
    const Id v2 = f.track("V2").id;
    CHECK(edit::renameTrack(f.tl(), v2, "Títulos"));
    CHECK(f.tl().track(v2)->name == "Títulos");
    CHECK_FALSE(edit::renameTrack(f.tl(), v2, "V1")); // taken
    CHECK_FALSE(edit::renameTrack(f.tl(), v2, ""));
    CHECK(edit::setTrackColor(f.tl(), v2, "purple"));
    CHECK_FALSE(edit::setTrackColor(f.tl(), v2, "purple"));
    CHECK(edit::setTrackEnabled(f.tl(), v2, false));
    CHECK_FALSE(f.tl().track(v2)->enabled);
    CHECK_FALSE(edit::renameTrack(f.tl(), "missing", "x"));
}

TEST_CASE("labels survive the project file; unset ones are not written", "[organization][project]")
{
    MultiFixture f;
    const std::string plain = json::serialize(timelineToJson(f.tl()));
    CHECK(plain.find("\"flags\"") == std::string::npos);
    CHECK(plain.find("\"color\"") == std::string::npos);

    edit::setClipColor(f.tl(), {f.clipAt("V1", 50)}, "orange");
    edit::setClipFlag(f.tl(), {f.clipAt("V1", 50)}, "green", true);
    edit::setTrackColor(f.tl(), f.track("A2").id, "teal");
    std::string error;
    const auto back = projectFromJson(*json::parse(json::serialize(projectToJson(f.project))), "", &error);
    REQUIRE(back);
    CHECK(back->timeline() == f.tl());
}

TEST_CASE("link and unlink takes; a link out of sync is reported", "[organization][links]")
{
    MultiFixture f;
    const Id title = f.clipAt("V2", 200);
    const Id music = f.clipAt("A2", 200);
    const Id x = f.clipAt("V1", 50);
    const Id xSound = f.clipAt("A1", 50);

    // The title starts at 150 but the music at 0: linked, they are out of sync, and that shows.
    CHECK(edit::linkClips(f.tl(), {title, music}));
    CHECK(edit::linkedGroup(f.tl(), title).size() == 2);
    CHECK_FALSE(edit::checkSync(f.tl()).empty());
    const auto outOfSync = edit::outOfSyncClips(f.tl());
    CHECK(std::find(outOfSync.begin(), outOfSync.end(), title) != outOfSync.end());
    CHECK_FALSE(edit::linkClips(f.tl(), {title, music})); // already that group

    // Two clips of one track cannot be one take.
    CHECK_FALSE(edit::linkClips(f.tl(), {x, f.clipAt("V1", 200)}));

    // Unlinking the picture leaves its sound alone, so the sound is unlinked too.
    CHECK(edit::unlinkClips(f.tl(), {x}));
    CHECK(edit::linkedGroup(f.tl(), x).size() == 1);
    CHECK(f.track("A1").findClip(xSound)->clip->linkGroup.empty());
    CHECK_FALSE(edit::unlinkClips(f.tl(), {x}));

    // Linking the picture of X with the sound of Y moves Y's picture out of its old group.
    const Id ySound = f.clipAt("A1", 200);
    const Id y = f.clipAt("V1", 200);
    CHECK(edit::linkClips(f.tl(), {x, ySound}));
    CHECK(f.track("V1").findClip(y)->clip->linkGroup.empty());
    CHECK(edit::checkInvariants(f.tl(), f.pool()).empty());
}

TEST_CASE("with linked selection off, edits leave the partners where they are", "[organization][links]")
{
    MultiFixture f;
    const Id x = f.clipAt("V1", 50);
    const Id xSound = f.clipAt("A1", 50);
    const Track a1 = f.track("A1");

    REQUIRE(edit::splitAt(f.tl(), F(40), {x}, false));
    CHECK(f.track("V1").clips().size() == 4);
    CHECK(f.track("A1") == a1);
    CHECK(edit::checkSync(f.tl()).empty()); // the left part keeps the link, the right one is alone

    REQUIRE(edit::moveClips(f.tl(), {f.clipAt("V1", 200)}, F(5), 0, false));
    CHECK(f.track("A1") == a1);
    CHECK_FALSE(edit::outOfSyncClips(f.tl()).empty()); // the picture of Y moved without its sound

    REQUIRE(edit::deleteClips(f.tl(), {x}, false));
    CHECK(f.track("A1").findClip(xSound));
    CHECK(edit::checkInvariants(f.tl(), f.pool()).empty());
}

TEST_CASE("markers are edited in place, one per frame", "[organization][markers]")
{
    MultiFixture f;
    const Id a = edit::addMarker(f.tl(), F(30), "A");
    const Id b = edit::addMarker(f.tl(), F(90), "B");
    Marker m = *edit::findMarker(f.tl(), a);
    m.name = "Começo";
    m.note = "trocar a música";
    m.color = "green";
    m.duration = F(15) + Time(3);
    m.start = F(120);
    REQUIRE(edit::updateMarker(f.tl(), m));
    const Marker& now = f.tl().markers.back(); // moved after B
    CHECK(now.id == a);
    CHECK(now.name == "Começo");
    CHECK(now.note == "trocar a música");
    CHECK(now.duration == F(15));
    CHECK_FALSE(edit::updateMarker(f.tl(), now)); // unchanged
    Marker clash = now;
    clash.start = F(90);
    CHECK_FALSE(edit::updateMarker(f.tl(), clash)); // B is there
    CHECK(f.tl().markers.front().id == b);
    CHECK(edit::checkInvariants(f.tl(), f.pool()).empty());
}

TEST_CASE("timelines: new, duplicate, switch, rename and delete, undoable", "[organization][timelines]")
{
    MultiFixture f;
    const Project original = f.project;
    const Id first = f.tl().id;
    CommandStack stack(f.project);

    REQUIRE(stack.editProject("New", [](Project& p) { return !edit::newTimeline(p).empty(); }));
    CHECK(f.project.timelines.size() == 2);
    CHECK(f.project.currentTimeline != first);
    CHECK(f.tl().rate == kRate);
    CHECK(f.tl().tracks.size() == 2);
    CHECK(f.tl().name == "Timeline 2");

    REQUIRE(stack.editProject("Duplicate", [first](Project& p) { return !edit::duplicateTimeline(p, first).empty(); }));
    CHECK(f.tl().name == "Timeline 1 copy");
    CHECK(f.tl().tracks == original.timeline().tracks);

    const Id copy = f.tl().id;
    CHECK_FALSE(edit::renameTimeline(f.project, copy, "Timeline 2")); // taken
    REQUIRE(stack.editProject("Rename", [copy](Project& p) { return edit::renameTimeline(p, copy, "Corte 2"); }));
    REQUIRE(stack.editProject("Switch", [first](Project& p) { return edit::setCurrentTimeline(p, first); }));
    CHECK(f.tl().id == first);
    REQUIRE(stack.editProject("Delete", [copy](Project& p) { return edit::deleteTimeline(p, copy); }));
    CHECK(f.project.timelines.size() == 2);

    // A timeline a compound clip shows, and the last timeline, cannot be deleted.
    const auto nested = edit::composeClips(f.project, {f.clipAt("V1", 200)});
    REQUIRE(nested);
    CHECK_FALSE(edit::deleteTimeline(f.project, *nested));
    Project single = original;
    CHECK_FALSE(edit::deleteTimeline(single, single.timeline().id));

    // Opening the nested timeline of the compound for editing is just switching to it.
    CHECK(edit::setCurrentTimeline(f.project, *nested));
    CHECK(f.tl().id == *nested);
    CHECK(edit::setCurrentTimeline(f.project, first));

    while (stack.canUndo())
        stack.undo();
    CHECK(f.project.currentTimeline == original.currentTimeline);
}
