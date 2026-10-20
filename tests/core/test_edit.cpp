#include "core/Edit.h"
#include "core/Snapshot.h"

#include <catch_amalgamated.hpp>

#include "test_support.h"

using namespace ve;
using namespace ve::test;

TEST_CASE("placing media creates linked V1/A1 clips on the frame grid", "[edit]")
{
    Fixture f;
    CHECK(f.tl().duration() == F(300));
    CHECK(f.v1().clips().size() == 1);
    CHECK(f.a1().clips().size() == 1);
    CHECK_FALSE(f.v1().clips()[0].clip->linkGroup.empty());
    CHECK(linkedTracksAligned(f.tl()));

    // Without a position: after the end. Each placement gets its own link group.
    edit::placeMedia(f.tl(), f.media);
    CHECK(f.tl().duration() == F(600));
    CHECK(f.v1().clips()[1].start == F(300));
    CHECK(f.v1().clips()[0].clip->linkGroup != f.v1().clips()[1].clip->linkGroup);

    // Media that does not end on a frame boundary is floored to the grid.
    MediaItem odd = testMedia(10.01);
    CHECK(edit::placeMedia(f.tl(), odd, F(1000)).size() == 2);
    CHECK(f.v1().clips().back().clip->sourceRange.duration == F(300));
    CHECK(edit::checkInvariants(f.tl(), f.pool()).empty());
}

TEST_CASE("audio-only media overwrites A1 only", "[edit]")
{
    Fixture f;
    const MediaItem music = testMedia(5.0, false, true);
    f.project.media.items.push_back(music);
    edit::placeMedia(f.tl(), music, Time(0));
    REQUIRE(f.a1().clips().size() == 2);
    CHECK(std::get<Clip>(*f.a1().clips()[0].item).mediaId == music.id);
    CHECK(f.a1().clips()[0].clip->linkGroup.empty());
    CHECK(f.a1().clips()[1].start == F(150));
    CHECK(f.a1().clips()[1].clip->sourceRange.start == F(150));
    CHECK(f.v1().clips().size() == 1);
}

TEST_CASE("blade splits linked clips and keeps both halves linked to each other", "[edit]")
{
    Fixture f;
    CHECK(edit::splitAt(f.tl(), F(100)));
    const auto v = f.v1().clips();
    REQUIRE(v.size() == 2);
    CHECK(v[0].clip->sourceRange.duration == F(100));
    CHECK(v[1].start == F(100));
    CHECK(v[1].clip->sourceRange.start == F(100));
    CHECK(v[1].clip->sourceRange.duration == F(200));
    CHECK(v[0].clip->id != v[1].clip->id);
    CHECK(v[0].clip->linkGroup != v[1].clip->linkGroup);
    CHECK(linkedTracksAligned(f.tl()));

    CHECK_FALSE(edit::splitAt(f.tl(), F(100))); // already a cut there
    CHECK_FALSE(edit::splitAt(f.tl(), Time(0)));
    CHECK_FALSE(edit::splitAt(f.tl(), F(5000)));

    // Restricted to one clip (and its audio).
    edit::placeMedia(f.tl(), f.media);
    const Id last = f.v1().clips().back().clip->id;
    CHECK(edit::splitAt(f.tl(), F(450), {last}));
    CHECK(f.v1().clips().size() == 4);
    CHECK(linkedTracksAligned(f.tl()));
}

TEST_CASE("move is an overwrite edit that never goes before zero", "[edit]")
{
    Fixture f;
    edit::splitAt(f.tl(), F(100));
    const Id second = f.v1().clips()[1].clip->id;

    CHECK(edit::moveClips(f.tl(), {second}, F(30)));
    auto v = f.v1().clips();
    REQUIRE(v.size() == 2);
    CHECK(v[1].start == F(130));
    CHECK(std::get<Gap>(f.v1().items[1]).duration == F(30));
    CHECK(f.tl().duration() == F(330));
    CHECK(linkedTracksAligned(f.tl()));

    // Over the first clip: its tail is overwritten.
    CHECK(edit::moveClips(f.tl(), {second}, -F(80)));
    v = f.v1().clips();
    REQUIRE(v.size() == 2);
    CHECK(v[0].clip->sourceRange.duration == F(50));
    CHECK(v[1].start == F(50));
    CHECK(v[1].clip->sourceRange.start == F(100));
    CHECK(linkedTracksAligned(f.tl()));

    // Clamped at zero; the first clip disappears completely under it.
    CHECK(edit::moveClips(f.tl(), {second}, -F(1000)));
    REQUIRE(f.v1().clips().size() == 1);
    CHECK(f.v1().clips()[0].start == Time(0));
    CHECK(f.v1().clips()[0].clip->id == second);
    CHECK_FALSE(edit::moveClips(f.tl(), {second}, Time(0)));
}

TEST_CASE("dropping a short clip into another one punches a hole with a relinked tail", "[edit]")
{
    Fixture f;
    edit::placeMedia(f.tl(), f.media); // [0,300) [300,600)
    edit::splitAt(f.tl(), F(590));
    const Id tiny = f.v1().clips()[2].clip->id; // [590,600)
    CHECK(edit::moveClips(f.tl(), {tiny}, F(100) - F(590)));
    const auto v = f.v1().clips();
    REQUIRE(v.size() == 4);
    CHECK(v[0].clip->sourceRange.duration == F(100));
    CHECK(v[1].clip->id == tiny);
    CHECK(v[2].start == F(110));
    CHECK(v[2].clip->sourceRange.start == F(110));
    CHECK(v[2].clip->id != v[0].clip->id);
    CHECK(linkedTracksAligned(f.tl()));
    CHECK(edit::linkedGroup(f.tl(), v[2].clip->id).size() == 2); // the new V/A tails share a group
}

TEST_CASE("trims respect media handles, neighbours and the one-frame minimum", "[edit]")
{
    Fixture f;
    const Id clip = f.v1().clips()[0].clip->id;
    CHECK_FALSE(edit::trimClip(f.tl(), f.pool(), clip, edit::Edge::Tail, F(400))); // no media left
    CHECK(edit::trimClip(f.tl(), f.pool(), clip, edit::Edge::Tail, F(250)));
    CHECK(f.v1().clips()[0].clip->sourceRange.duration == F(250));
    CHECK(linkedTracksAligned(f.tl()));
    CHECK(edit::trimClip(f.tl(), f.pool(), clip, edit::Edge::Tail, F(1000))); // clamped
    CHECK(f.v1().clips()[0].clip->sourceRange.duration == F(300));

    CHECK(edit::trimClip(f.tl(), f.pool(), clip, edit::Edge::Head, F(40)));
    auto p = f.v1().clips()[0];
    CHECK(p.start == F(40));
    CHECK(p.clip->sourceRange.start == F(40));
    CHECK(p.clip->sourceRange.duration == F(260));
    CHECK(edit::trimClip(f.tl(), f.pool(), clip, edit::Edge::Head, -F(50))); // back to the media start
    p = f.v1().clips()[0];
    CHECK(p.start == Time(0));
    CHECK(p.clip->sourceRange.start == Time(0));

    edit::splitAt(f.tl(), F(150));
    const Id left = f.v1().clips()[0].clip->id;
    CHECK_FALSE(edit::trimClip(f.tl(), f.pool(), left, edit::Edge::Tail, F(200))); // neighbour
    CHECK(edit::trimClip(f.tl(), f.pool(), left, edit::Edge::Tail, -F(10)));
    CHECK(f.v1().clips()[0].clip->sourceRange.duration == F(1));
    CHECK(edit::checkInvariants(f.tl(), f.pool()).empty());
}

TEST_CASE("lift delete leaves a gap and trims the track end", "[edit]")
{
    Fixture f;
    edit::splitAt(f.tl(), F(100));
    edit::splitAt(f.tl(), F(200));
    CHECK(edit::deleteClips(f.tl(), {f.v1().clips()[1].clip->id}));
    CHECK(f.v1().clips().size() == 2);
    CHECK(f.a1().clips().size() == 2);
    CHECK(std::get<Gap>(f.v1().items[1]).duration == F(100));
    CHECK(f.tl().duration() == F(300));
    CHECK(edit::deleteClips(f.tl(), {f.v1().clips()[1].clip->id}));
    CHECK(f.tl().duration() == F(100));
    CHECK(f.v1().items.size() == 1);
}

TEST_CASE("locked tracks are left alone", "[edit]")
{
    Fixture f;
    f.a1().locked = true;
    CHECK(edit::splitAt(f.tl(), F(100)));
    CHECK(f.v1().clips().size() == 2);
    CHECK(f.a1().clips().size() == 1);
    CHECK(edit::moveClips(f.tl(), {f.v1().clips()[1].clip->id}, F(10)));
    CHECK(f.a1().clips()[0].start == Time(0));
    CHECK(f.a1().clips()[0].clip->sourceRange.duration == F(300));
}

TEST_CASE("normalize merges gaps and drops dangling transitions", "[model]")
{
    Track t;
    IdRemap links;
    Clip c;
    c.id = newId();
    c.sourceRange = {Time(0), F(10)};
    t.items = {Gap{F(5)}, Gap{Time(0)}, Gap{F(5)}, Transition{}, c, Gap{F(3)}};
    t.normalize();
    REQUIRE(t.items.size() == 2);
    CHECK(std::get<Gap>(t.items[0]).duration == F(10));
    CHECK(t.duration() == F(20));

    Clip a = c, b = c;
    b.id = newId();
    t.items = {a, Transition{}, b};
    t.normalize();
    CHECK(t.items.size() == 3);
    t.liftRange(Time(0), F(10), links);
    CHECK(t.items.size() == 2);
}

TEST_CASE("compound and multicam items edit like clips", "[model]")
{
    Fixture f;
    MulticamClip mc;
    mc.id = newId();
    mc.name = "multicam";
    mc.sourceRange = {Time(0), F(60)};
    mc.angles = {f.media.id};
    CompoundClip cc;
    cc.id = newId();
    cc.sourceRange = {Time(0), F(40)};
    IdRemap links;
    f.v1().placeItem(F(300), mc, links);
    f.v1().placeItem(F(360), cc, links);
    CHECK(f.tl().duration() == F(400));
    CHECK(edit::splitAt(f.tl(), F(330)));
    CHECK(std::holds_alternative<MulticamClip>(f.v1().items[2]));
    CHECK(edit::checkInvariants(f.tl(), f.pool()).empty());

    // The active angle is what the viewer shows.
    auto snap = TimelineSnapshot::fromProject(f.project);
    auto src = resolveVideo(*snap, F(310));
    REQUIRE(src);
    CHECK(src->media->id == f.media.id);
    CHECK(src->sourceTime == F(10));
    CHECK_FALSE(resolveVideo(*snap, F(370))); // a compound whose nested timeline is missing shows nothing
}

TEST_CASE("snapshot resolves the picture under the playhead", "[engine-facing]")
{
    Fixture f;
    edit::splitAt(f.tl(), F(100));
    edit::moveClips(f.tl(), {f.v1().clips()[1].clip->id}, F(50)); // gap [100,150)
    auto snap = TimelineSnapshot::fromProject(f.project);
    auto a = resolveVideo(*snap, F(99));
    REQUIRE(a);
    CHECK(a->sourceTime == F(99));
    CHECK_FALSE(resolveVideo(*snap, F(120)));
    auto b = resolveVideo(*snap, F(150));
    REQUIRE(b);
    CHECK(b->sourceTime == F(100));
    CHECK_FALSE(resolveVideo(*snap, F(400)));
}

TEST_CASE("animated parameters interpolate linearly or hold", "[model]")
{
    AnimatedParam p;
    p.value = 3.0;
    CHECK(p.evaluate(F(10)) == 3.0);
    p.keys = {{F(0), 0.0, Interpolation::Linear}, {F(10), 1.0, Interpolation::Hold}, {F(20), 5.0, Interpolation::Linear}};
    CHECK(p.evaluate(-F(5)) == 0.0);
    CHECK(p.evaluate(F(5)) == Catch::Approx(0.5));
    CHECK(p.evaluate(F(15)) == 1.0); // hold until the next key
    CHECK(p.evaluate(F(20)) == 5.0);
    CHECK(p.evaluate(F(99)) == 5.0);
}
