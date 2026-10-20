// Trims (spec §6.2): ripple, roll, slip, slide, J/L cuts, ripple delete, and sync through ripples.

#include "core/Edit.h"

#include <catch_amalgamated.hpp>

#include "test_support.h"

using namespace ve;
using namespace ve::test;

namespace {

struct Span {
    std::int64_t start, end, source;
    bool operator==(const Span&) const = default;
};

std::vector<Span> spans(const Track& t)
{
    std::vector<Span> out;
    for (const PlacedItem& p : t.clips())
        out.push_back({frames(p.start), frames(p.end()), frames(p.clip->sourceRange.start)});
    return out;
}

bool healthy(MultiFixture& f)
{
    const std::string problems = edit::checkInvariants(f.tl(), f.pool()) + edit::checkSync(f.tl());
    INFO(problems << describeText(f.tl(), f.pool()));
    return problems.empty();
}

} // namespace

// The fixture: V1/A1 X [0,100) Y [100,300) Z [300,420); V2 title [150,240); A2 music [0,600).

TEST_CASE("ripple tail: everything after the cut follows on every sync-locked track", "[trim]")
{
    MultiFixture f;
    const Id x = f.clipAt("V1", 50);
    // The music on A2 spans the cut: a sync-locked track only gives up empty time.
    CHECK_FALSE(edit::rippleTrim(f.tl(), f.pool(), x, edit::Edge::Tail, -F(20)));

    f.track("A2").syncLock = false;
    REQUIRE(edit::rippleTrim(f.tl(), f.pool(), x, edit::Edge::Tail, -F(20)));
    const std::vector<Span> expected{{0, 80, 0}, {80, 280, 100}, {280, 400, 0}};
    CHECK(spans(f.track("V1")) == expected);
    CHECK(spans(f.track("A1")) == expected);
    CHECK(spans(f.track("V2")) == std::vector<Span>{{130, 220, 0}});
    CHECK(spans(f.track("A2")) == std::vector<Span>{{0, 600, 0}});
    CHECK(healthy(f));
    CHECK(strictAlignment(f.tl()).empty());
}

TEST_CASE("ripple tail extension pushes the rest and cuts through sync-locked clips", "[trim]")
{
    MultiFixture f;
    const Id x = f.clipAt("V1", 50);
    REQUIRE(edit::rippleTrim(f.tl(), f.pool(), x, edit::Edge::Tail, F(30)));
    CHECK(spans(f.track("V1")) == std::vector<Span>{{0, 130, 0}, {130, 330, 100}, {330, 450, 0}});
    CHECK(spans(f.track("V2")) == std::vector<Span>{{180, 270, 0}});
    CHECK(spans(f.track("A2")) == std::vector<Span>{{0, 100, 0}, {130, 630, 100}});
    // Clamped to the media: Y uses its media up to the end.
    const Id y = f.clipAt("V1", 200);
    CHECK_FALSE(edit::rippleTrim(f.tl(), f.pool(), y, edit::Edge::Tail, F(10)));
    CHECK(healthy(f));
}

TEST_CASE("ripple head keeps the clip's start and moves its content with what follows", "[trim]")
{
    MultiFixture f;
    f.track("A2").syncLock = false;
    const Id y = f.clipAt("V1", 200);
    const Timeline start = f.tl();

    REQUIRE(edit::rippleTrim(f.tl(), f.pool(), y, edit::Edge::Head, F(30)));
    CHECK(spans(f.track("V1")) == std::vector<Span>{{0, 100, 0}, {100, 270, 130}, {270, 390, 0}});
    CHECK(spans(f.track("A1")) == spans(f.track("V1")));
    CHECK(spans(f.track("V2")) == std::vector<Span>{{120, 210, 0}});
    CHECK(healthy(f));

    // V2 has only 50 empty frames after the cut (the title starts at 150): clamped there.
    f.tl() = start;
    REQUIRE(edit::rippleTrim(f.tl(), f.pool(), y, edit::Edge::Head, F(60)));
    CHECK(spans(f.track("V1"))[1] == Span{100, 250, 150});
    CHECK(spans(f.track("V2")) == std::vector<Span>{{100, 190, 0}});

    // Extending the head brings back earlier media and pushes everything after it.
    f.tl() = start;
    REQUIRE(edit::rippleTrim(f.tl(), f.pool(), y, edit::Edge::Head, -F(20)));
    CHECK(spans(f.track("V1")) == std::vector<Span>{{0, 100, 0}, {100, 320, 80}, {320, 440, 0}});
    CHECK(spans(f.track("V2")) == std::vector<Span>{{170, 260, 0}});
    CHECK(healthy(f));
    CHECK(strictAlignment(f.tl()).empty());
}

TEST_CASE("roll moves the cut and nothing else", "[trim]")
{
    MultiFixture f;
    const Timeline start = f.tl();
    const Id x = f.clipAt("V1", 50);
    const Id y = f.clipAt("V1", 200);
    REQUIRE(edit::rollTrim(f.tl(), f.pool(), x, edit::Edge::Tail, F(25)));
    const std::vector<Span> rolled{{0, 125, 0}, {125, 300, 125}, {300, 420, 0}};
    CHECK(spans(f.track("V1")) == rolled);
    CHECK(spans(f.track("A1")) == rolled);
    CHECK(f.track("V2") == start.tracks[1]);
    CHECK(f.tl().duration() == start.duration());

    // The head of the incoming clip is the same cut.
    f.tl() = start;
    REQUIRE(edit::rollTrim(f.tl(), f.pool(), y, edit::Edge::Head, F(25)));
    CHECK(spans(f.track("V1")) == rolled);

    // Clamped so both sides keep at least one frame (and within the media handles).
    f.tl() = start;
    REQUIRE(edit::rollTrim(f.tl(), f.pool(), x, edit::Edge::Tail, F(250)));
    CHECK(spans(f.track("V1"))[1] == Span{299, 300, 299});
    f.tl() = start;
    REQUIRE(edit::rollTrim(f.tl(), f.pool(), x, edit::Edge::Tail, -F(120)));
    CHECK(spans(f.track("V1"))[0] == Span{0, 1, 0});

    // At the end of the track the roll shortens the last clip; it has no media to grow into.
    f.tl() = start;
    const Id z = f.clipAt("V1", 350);
    CHECK_FALSE(edit::rollTrim(f.tl(), f.pool(), z, edit::Edge::Tail, F(20)));
    REQUIRE(edit::rollTrim(f.tl(), f.pool(), z, edit::Edge::Tail, -F(20)));
    CHECK(spans(f.track("V1")).back() == Span{300, 400, 0});

    // Against a gap: the gap takes the time.
    f.tl() = start;
    REQUIRE(edit::rollTrim(f.tl(), f.pool(), f.clipAt("V2", 200), edit::Edge::Head, F(20)));
    CHECK(spans(f.track("V2")) == std::vector<Span>{{170, 240, 20}});
    CHECK(healthy(f));
}

TEST_CASE("asymmetric trims make J and L cuts and keep picture and sound in sync", "[trim]")
{
    MultiFixture f;
    const Timeline start = f.tl();
    const Id xAudio = f.clipAt("A1", 50);

    // L cut: the sound of X goes on 20 frames under the picture of Y.
    REQUIRE(edit::rollTrim(f.tl(), f.pool(), xAudio, edit::Edge::Tail, F(20), false));
    CHECK(spans(f.track("A1")) == std::vector<Span>{{0, 120, 0}, {120, 300, 120}, {300, 420, 0}});
    CHECK(spans(f.track("V1")) == std::vector<Span>{{0, 100, 0}, {100, 300, 100}, {300, 420, 0}});
    CHECK(healthy(f));
    CHECK_FALSE(strictAlignment(f.tl()).empty()); // the edges differ on purpose

    // J cut: the sound of Y starts 20 frames before its picture.
    f.tl() = start;
    REQUIRE(edit::rollTrim(f.tl(), f.pool(), xAudio, edit::Edge::Tail, -F(20), false));
    CHECK(spans(f.track("A1"))[1] == Span{80, 300, 80});
    CHECK(healthy(f));

    // Ripple of the picture alone: A1 is sync-locked, so it gets the same 20 frames of space.
    f.tl() = start;
    f.track("A2").syncLock = false;
    REQUIRE(edit::rippleTrim(f.tl(), f.pool(), f.clipAt("V1", 50), edit::Edge::Tail, F(20), false));
    CHECK(spans(f.track("V1")) == std::vector<Span>{{0, 120, 0}, {120, 320, 100}, {320, 440, 0}});
    CHECK(spans(f.track("A1")) == std::vector<Span>{{0, 100, 0}, {120, 320, 100}, {320, 440, 0}});
    CHECK(healthy(f));

    // Shortening the head of the picture alone would remove frames from its own sound: refused.
    f.tl() = start;
    f.track("A2").syncLock = false;
    CHECK_FALSE(edit::rippleTrim(f.tl(), f.pool(), f.clipAt("V1", 200), edit::Edge::Head, F(10), false));
    // Extending it is fine: the sound moves with the content.
    REQUIRE(edit::rippleTrim(f.tl(), f.pool(), f.clipAt("V1", 200), edit::Edge::Head, -F(20), false));
    CHECK(spans(f.track("V1"))[1] == Span{100, 320, 80});
    CHECK(spans(f.track("A1"))[1] == Span{120, 320, 100});
    CHECK(healthy(f));
}

TEST_CASE("an insert through an L cut relinks the pieces that are still in sync", "[trim][three-point]")
{
    MultiFixture f;
    f.track("V2").locked = true;
    f.track("A2").syncLock = false;
    REQUIRE(edit::rollTrim(f.tl(), f.pool(), f.clipAt("A1", 50), edit::Edge::Tail, F(20), false));
    const Id x = f.clipAt("V1", 50);
    const Id y = f.clipAt("V1", 200);
    const Id yAudio = f.clipAt("A1", 200);

    auto p = f.points(f.b, 110); // inside the L cut: X's sound and Y's picture both span it
    p.sourceOut = F(60);
    REQUIRE(edit::insertEdit(f.tl(), f.pool(), p).size() == 2);
    CHECK(healthy(f));
    // X keeps the part of its sound before the insert...
    const auto xGroup = edit::linkedGroup(f.tl(), x);
    REQUIRE(xGroup.size() == 2);
    CHECK(f.track("A1").findClip(xGroup[1])->start == Time(0));
    // ...and Y's sound stays with the part of Y's picture after it.
    const Id yRight = f.clipAt("V1", 200);
    CHECK(yRight != y);
    const auto yGroup = edit::linkedGroup(f.tl(), yRight);
    CHECK(std::find(yGroup.begin(), yGroup.end(), yAudio) != yGroup.end());
    CHECK(edit::linkedGroup(f.tl(), y).size() == 1); // the 10 frames of Y before the insert
}

TEST_CASE("an insert that would push a take's sound away from its picture is refused", "[trim][three-point]")
{
    Project project = makeEmptyProject();
    Timeline& tl = project.timeline();
    tl.rate = kRate;
    const MediaItem m = testMedia();
    project.media.items.push_back(m);
    // Picture [0,50) and its sound [60,100), in sync (same media offset), as extreme trims can leave.
    Clip picture;
    picture.id = newId();
    picture.mediaId = m.id;
    picture.sourceRange = {Time(0), F(50)};
    picture.linkGroup = "take";
    Clip sound = picture;
    sound.id = newId();
    sound.sourceRange = {F(60), F(40)};
    tl.tracks[0].items = {picture};
    tl.tracks[1].items = {Gap{F(60)}, sound};
    REQUIRE(edit::checkSync(tl).empty());
    const Timeline before = tl;

    edit::EditPoints p;
    p.mediaId = m.id;
    p.playhead = F(55);
    p.sourceOut = F(10);
    p.videoTrack = tl.tracks[0].id;
    p.audioTrack = tl.tracks[1].id;
    CHECK(edit::insertEdit(tl, project.media, p).empty());
    CHECK(tl == before);
}

TEST_CASE("slip shows other media in place; slide moves the clip between its neighbours", "[trim]")
{
    MultiFixture f;
    const Timeline start = f.tl();
    const Id y = f.clipAt("V1", 200);

    REQUIRE(edit::slipClip(f.tl(), f.pool(), y, -F(40)));
    CHECK(spans(f.track("V1"))[1] == Span{100, 300, 60});
    CHECK(spans(f.track("A1"))[1] == Span{100, 300, 60});
    // Now there are 40 frames of media after it: a +50 slip stops there.
    REQUIRE(edit::slipClip(f.tl(), f.pool(), y, F(50)));
    CHECK(spans(f.track("V1"))[1] == Span{100, 300, 100});
    CHECK(f.tl() == start);
    CHECK_FALSE(edit::slipClip(f.tl(), f.pool(), y, F(10))); // no media after Y

    REQUIRE(edit::slideClip(f.tl(), f.pool(), y, F(20)));
    const std::vector<Span> slid{{0, 120, 0}, {120, 320, 100}, {320, 420, 20}};
    CHECK(spans(f.track("V1")) == slid);
    CHECK(spans(f.track("A1")) == slid);
    CHECK(f.tl().duration() == start.duration());
    // Sliding left would need media before Z's start.
    f.tl() = start;
    CHECK_FALSE(edit::slideClip(f.tl(), f.pool(), y, -F(20)));
    // Between gaps a slide is a move.
    REQUIRE(edit::slideClip(f.tl(), f.pool(), f.clipAt("V2", 200), -F(50)));
    CHECK(spans(f.track("V2")) == std::vector<Span>{{100, 190, 0}});
    CHECK(healthy(f));
}

TEST_CASE("ripple delete closes the hole where sync lock allows it", "[trim]")
{
    MultiFixture f;
    const Timeline start = f.tl();
    const Id y = f.clipAt("V1", 200);

    // The title (V2) and the music (A2) are inside the hole on sync-locked tracks.
    CHECK_FALSE(edit::rippleDelete(f.tl(), {y}));
    f.track("A2").syncLock = false;
    CHECK_FALSE(edit::rippleDelete(f.tl(), {y}));
    Timeline unchanged = start;
    unchanged.tracks[3].syncLock = false; // A2
    CHECK(f.tl() == unchanged);

    f.track("V2").locked = true;
    REQUIRE(edit::rippleDelete(f.tl(), {y}));
    const std::vector<Span> closed{{0, 100, 0}, {100, 220, 0}};
    CHECK(spans(f.track("V1")) == closed);
    CHECK(spans(f.track("A1")) == closed);
    CHECK(spans(f.track("V2")) == std::vector<Span>{{150, 240, 0}});
    CHECK(spans(f.track("A2")) == std::vector<Span>{{0, 600, 0}});

    // Two holes at once.
    f.tl() = start;
    f.track("V2").locked = true;
    f.track("A2").syncLock = false;
    REQUIRE(edit::rippleDelete(f.tl(), {f.clipAt("V1", 50), f.clipAt("A1", 350)}));
    CHECK(spans(f.track("V1")) == std::vector<Span>{{0, 200, 100}});
    CHECK(healthy(f));
}

TEST_CASE("ripple delete does not cut the L cut of a neighbour", "[trim]")
{
    MultiFixture f;
    f.track("V2").locked = true;
    f.track("A2").syncLock = false;
    REQUIRE(edit::rollTrim(f.tl(), f.pool(), f.clipAt("A1", 50), edit::Edge::Tail, F(20), false));
    const Timeline before = f.tl();
    CHECK_FALSE(edit::rippleDelete(f.tl(), {f.clipAt("V1", 200)})); // X's sound runs into Y's range
    CHECK(f.tl() == before);
}

TEST_CASE("trims leave locked tracks alone", "[trim]")
{
    MultiFixture f;
    f.track("A1").locked = true;
    f.track("A2").syncLock = false;
    const Track a1 = f.track("A1");
    const Id x = f.clipAt("V1", 50);
    REQUIRE(edit::rippleTrim(f.tl(), f.pool(), x, edit::Edge::Tail, -F(10)));
    REQUIRE(edit::rollTrim(f.tl(), f.pool(), x, edit::Edge::Tail, F(5)));
    REQUIRE(edit::slipClip(f.tl(), f.pool(), f.clipAt("V1", 200), -F(5)));
    CHECK(f.track("A1") == a1);
    CHECK_FALSE(edit::rippleTrim(f.tl(), f.pool(), f.clipAt("A1", 50), edit::Edge::Tail, -F(10)));
    CHECK_FALSE(edit::rollTrim(f.tl(), f.pool(), f.clipAt("A1", 50), edit::Edge::Tail, F(10)));
    CHECK(f.track("A1") == a1);
    CHECK(edit::checkInvariants(f.tl(), f.pool()).empty());
}
