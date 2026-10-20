// Phase 3.2: the edits that were missing. Append, ripple overwrite, inserting and deleting gaps, and
// swapping clips.

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

TEST_CASE("append puts the source after the end of the destination tracks", "[three-point][phase3]")
{
    MultiFixture f;
    auto p = f.points(f.b, 30); // the playhead does not matter
    p.recordIn = F(10);
    p.sourceOut = F(60);
    REQUIRE(edit::appendEdit(f.tl(), f.pool(), p).size() == 2);
    CHECK(spans(f.track("V1")).back() == Span{420, 480, 0});
    CHECK(spans(f.track("A1")).back() == Span{420, 480, 0});

    // Onto V2 only: after the title.
    p = f.points(f.title, 0);
    p.videoTrack = f.track("V2").id;
    p.audioTrack.clear();
    REQUIRE(edit::appendEdit(f.tl(), f.pool(), p).size() == 1);
    CHECK(spans(f.track("V2")).back() == Span{240, 330, 0});
    CHECK(healthy(f));
}

TEST_CASE("ripple overwrite swaps the clip under the playhead and ripples the difference", "[three-point][phase3]")
{
    MultiFixture f;
    auto p = f.points(f.b, 200); // Y [100,300) under the playhead
    p.sourceOut = F(60);
    // The title (V2, sync-locked) is inside Y's time: like a ripple delete, refused.
    const Timeline before = f.tl();
    CHECK(edit::rippleOverwriteEdit(f.tl(), f.pool(), p).empty());
    CHECK(f.tl() == before);

    f.track("V2").locked = true;
    f.track("A2").syncLock = false;
    REQUIRE(edit::rippleOverwriteEdit(f.tl(), f.pool(), p).size() == 2);
    const std::vector<Span> expected{{0, 100, 0}, {100, 160, 0}, {160, 280, 0}};
    CHECK(spans(f.track("V1")) == expected);
    CHECK(spans(f.track("A1")) == expected);
    CHECK(spans(f.track("V2")) == std::vector<Span>{{150, 240, 0}});
    CHECK(healthy(f));

    // Nothing under the playhead.
    p.playhead = F(500);
    CHECK(edit::rippleOverwriteEdit(f.tl(), f.pool(), p).empty());
}

TEST_CASE("insert gap opens time on every sync-locked track", "[gaps][phase3]")
{
    MultiFixture f;
    f.track("A2").syncLock = false;
    REQUIRE(edit::insertGap(f.tl(), F(50), F(30)));
    CHECK(spans(f.track("V1")) == std::vector<Span>{{0, 50, 0}, {80, 130, 50}, {130, 330, 100}, {330, 450, 0}});
    CHECK(spans(f.track("V2")) == std::vector<Span>{{180, 270, 0}});
    CHECK(spans(f.track("A2")) == std::vector<Span>{{0, 600, 0}});
    CHECK(healthy(f));
    CHECK_FALSE(edit::insertGap(f.tl(), F(50), Time(0)));
    CHECK_FALSE(edit::insertGap(f.tl(), F(5000), F(10))); // nothing after it
}

TEST_CASE("delete gap closes one gap when the other sync-locked tracks are empty there", "[gaps][phase3]")
{
    MultiFixture f;
    const Id v2 = f.track("V2").id;
    // V1, A1 and A2 have clips under V2's gap [0,150): closing it would break sync.
    CHECK_FALSE(edit::deleteGap(f.tl(), v2, F(10)));
    // Without sync lock, V2 closes its own gap.
    f.track("V2").syncLock = false;
    REQUIRE(edit::deleteGap(f.tl(), v2, F(10)));
    CHECK(spans(f.track("V2")) == std::vector<Span>{{0, 90, 0}});
    CHECK(spans(f.track("V1")).size() == 3);
    CHECK_FALSE(edit::deleteGap(f.tl(), v2, F(10))); // no gap there any more

    // A gap on V1/A1 after a lift, with the music and the title out of the way.
    MultiFixture g;
    g.track("V2").locked = true;
    g.track("A2").syncLock = false;
    REQUIRE(edit::deleteClips(g.tl(), {g.clipAt("V1", 200)}));
    REQUIRE(edit::deleteGap(g.tl(), g.track("V1").id, F(150)));
    CHECK(spans(g.track("V1")) == std::vector<Span>{{0, 100, 0}, {100, 220, 0}});
    CHECK(spans(g.track("A1")) == spans(g.track("V1")));
    CHECK(healthy(g));
}

TEST_CASE("delete gaps closes every stretch with no clip on the sync-locked tracks", "[gaps][phase3]")
{
    MultiFixture f;
    f.track("A2").syncLock = false;
    REQUIRE(edit::deleteClips(f.tl(), {f.clipAt("V1", 50)})); // gap [0,100) on V1 and A1
    REQUIRE(edit::deleteGaps(f.tl()));
    CHECK(spans(f.track("V1")) == std::vector<Span>{{0, 200, 100}, {200, 320, 0}});
    CHECK(spans(f.track("V2")) == std::vector<Span>{{50, 140, 0}});
    CHECK(spans(f.track("A2")) == std::vector<Span>{{0, 600, 0}}); // no sync lock: untouched
    CHECK_FALSE(edit::deleteGaps(f.tl())); // V2's gap [0,50) holds V1's clip: no common gap left
    CHECK(healthy(f));
}

TEST_CASE("swap moves a take past its neighbour, picture and sound together", "[order][phase3]")
{
    MultiFixture f;
    const Timeline start = f.tl();
    const Id y = f.clipAt("V1", 200);
    REQUIRE(edit::swapClips(f.tl(), y, 1));
    const std::vector<Span> right{{0, 100, 0}, {100, 220, 0}, {220, 420, 100}};
    CHECK(spans(f.track("V1")) == right);
    CHECK(spans(f.track("A1")) == right);
    CHECK(healthy(f));

    f.tl() = start;
    REQUIRE(edit::swapClips(f.tl(), y, -1));
    CHECK(spans(f.track("V1")) == std::vector<Span>{{0, 200, 100}, {200, 300, 0}, {300, 420, 0}});
    CHECK(healthy(f));

    // The last clip has no neighbour to its right; the title has none at all.
    f.tl() = start;
    CHECK_FALSE(edit::swapClips(f.tl(), f.clipAt("V1", 350), 1));
    CHECK_FALSE(edit::swapClips(f.tl(), f.clipAt("V2", 200), 1));

    // With an L cut the sound of X ends later than its picture: swapping would break sync.
    REQUIRE(edit::rollTrim(f.tl(), f.pool(), f.clipAt("A1", 50), edit::Edge::Tail, F(20), false));
    const Timeline lcut = f.tl();
    CHECK_FALSE(edit::swapClips(f.tl(), f.clipAt("V1", 50), 1));
    CHECK(f.tl() == lcut);
}
