// Multitrack (adding tracks, moving clips between tracks, lock and sync lock switches) and markers.

#include "core/Edit.h"
#include "core/ProjectFile.h"

#include <catch_amalgamated.hpp>

#include "test_support.h"

using namespace ve;
using namespace ve::test;

TEST_CASE("tracks are added above the others of their kind with unique names", "[tracks]")
{
    Timeline tl = makeDefaultTimeline(); // V1 A1
    const Id v2 = edit::addTrack(tl, TrackKind::Video);
    const Id a2 = edit::addTrack(tl, TrackKind::Audio);
    const Id v3 = edit::addTrack(tl, TrackKind::Video);
    REQUIRE(tl.tracks.size() == 5);
    CHECK(tl.tracks[1].id == v2);
    CHECK(tl.tracks[2].id == v3);
    CHECK(tl.tracks[4].id == a2);
    CHECK(tl.track(v3)->name == "V3");
    CHECK(tl.track(a2)->name == "A2");
    CHECK(edit::kindIndex(tl, v3) == 2);
    CHECK(edit::kindIndex(tl, a2) == 1);
    CHECK(edit::trackOfKind(tl, TrackKind::Audio, 1)->id == a2);
    CHECK_FALSE(edit::trackOfKind(tl, TrackKind::Audio, 2));

    tl.track(v3)->name = "V4"; // a renamed track keeps its name free of clashes
    CHECK(tl.track(edit::addTrack(tl, TrackKind::Video))->name == "V5");

    CHECK(edit::setTrackLocked(tl, v2, true));
    CHECK_FALSE(edit::setTrackLocked(tl, v2, true));
    CHECK(edit::setTrackSyncLock(tl, a2, false));
    CHECK_FALSE(edit::setTrackSyncLock(tl, "missing", false));
    CHECK(tl.track(v2)->locked);
    CHECK_FALSE(tl.track(a2)->syncLock);
}

TEST_CASE("clips move between tracks, picture and sound together", "[tracks][edit]")
{
    MultiFixture f;
    const Id x = f.clipAt("V1", 50);
    REQUIRE(edit::moveClips(f.tl(), {x}, Time(0), 1)); // V1 -> V2 and A1 -> A2
    CHECK(f.track("V2").findClip(x));
    CHECK(f.track("V1").clips().size() == 2);
    const auto music = f.track("A2").clips();
    REQUIRE(music.size() == 2); // the take overwrote the start of the music
    CHECK(music[1].start == F(100));
    CHECK(music[1].clip->sourceRange.start == F(100));
    CHECK(edit::checkSync(f.tl()).empty());
    CHECK(strictAlignment(f.tl()).empty());

    // There is no V3: the shift is reduced to what exists, here none, so only the time move happens.
    REQUIRE(edit::moveClips(f.tl(), {x}, F(10), 5));
    CHECK(f.track("V2").findClip(x)->start == F(10));
    // A locked destination stops the vertical move too.
    f.track("V1").locked = true;
    CHECK_FALSE(edit::moveClips(f.tl(), {x}, Time(0), -1));
    CHECK(edit::checkInvariants(f.tl(), f.pool()).empty());
}

TEST_CASE("markers sit on the frame grid, one per frame, and survive the project file", "[markers]")
{
    MultiFixture f;
    Timeline& tl = f.tl();
    const Id first = edit::addMarker(tl, F(90) + Time(5), "Pico", "red");
    REQUIRE_FALSE(first.empty());
    CHECK(tl.markers[0].start == F(90));
    CHECK(edit::addMarker(tl, F(90)).empty()); // one marker per frame
    const Id second = edit::addMarker(tl, F(30), "", "", F(15));
    REQUIRE(tl.markers.size() == 2);
    CHECK(tl.markers[0].id == second); // sorted
    CHECK(tl.markers[0].color == "blue");
    CHECK(tl.markers[0].duration == F(15));

    CHECK(edit::moveMarker(tl, second, F(120)));
    CHECK(tl.markers.back().id == second);
    CHECK_FALSE(edit::moveMarker(tl, second, F(90))); // taken
    CHECK_FALSE(edit::moveMarker(tl, "missing", F(10)));
    CHECK(edit::findMarker(tl, first)->name == "Pico");

    // Clips moving or rippling leave markers where they are.
    f.track("A2").syncLock = false;
    REQUIRE(edit::rippleTrim(tl, f.pool(), f.clipAt("V1", 50), edit::Edge::Tail, -F(20)));
    CHECK(tl.markers[0].start == F(90));
    CHECK(edit::checkInvariants(tl, f.pool()).empty());

    std::string error;
    const auto back = projectFromJson(*json::parse(json::serialize(projectToJson(f.project))), "", &error);
    REQUIRE(back);
    CHECK(back->timeline().markers == tl.markers);

    CHECK(edit::removeMarker(tl, first));
    CHECK_FALSE(edit::removeMarker(tl, first));
    CHECK(tl.markers.size() == 1);
}

TEST_CASE("edit points list every cut once", "[edit]")
{
    MultiFixture f;
    const auto points = edit::editPoints(f.tl());
    std::vector<std::int64_t> n;
    for (Time t : points)
        n.push_back(frames(t));
    CHECK(n == std::vector<std::int64_t>{0, 100, 150, 240, 300, 420, 600});
}
