// Three and four point edits (spec §6.1): insert, overwrite, replace, fit to fill, place on top.

#include "core/Edit.h"
#include "core/Snapshot.h"

#include <catch_amalgamated.hpp>

#include "test_support.h"

using namespace ve;
using namespace ve::test;

namespace {

// Start, end (frames) and source start of every clip on a track.
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

} // namespace

TEST_CASE("the missing points of an edit come from the others", "[three-point]")
{
    const MediaItem b = testMedia(4.0); // 120 frames
    edit::EditPoints p;
    p.mediaId = b.id;
    p.playhead = F(50);

    auto r = edit::resolvePoints(p, b, kRate); // nothing marked: the whole media at the playhead
    REQUIRE(r);
    CHECK(r->duration == F(120));
    CHECK(r->sourceStart == Time(0));
    CHECK(r->recordStart == F(50));

    p.sourceIn = F(30);
    p.sourceOut = F(90);
    r = edit::resolvePoints(p, b, kRate);
    REQUIRE(r);
    CHECK(r->duration == F(60));
    CHECK(r->sourceStart == F(30));

    // Timeline Out only: backtimed from it.
    p.recordOut = F(400);
    r = edit::resolvePoints(p, b, kRate);
    REQUIRE(r);
    CHECK(r->recordStart == F(340));

    // Timeline In and Out set the duration; a source Out alone backtimes the source.
    p = {};
    p.mediaId = b.id;
    p.recordIn = F(0);
    p.recordOut = F(30);
    p.sourceOut = F(120);
    r = edit::resolvePoints(p, b, kRate);
    REQUIRE(r);
    CHECK(r->sourceStart == F(90));
    CHECK(r->duration == F(30));

    // Four points: the timeline range wins, the source Out is ignored.
    p.sourceIn = F(10);
    r = edit::resolvePoints(p, b, kRate);
    REQUIRE(r);
    CHECK(r->sourceStart == F(10));
    CHECK(r->duration == F(30));

    // Not enough media, or an Out before the In.
    p.sourceIn = F(100);
    CHECK_FALSE(edit::resolvePoints(p, b, kRate));
    p = {};
    p.mediaId = b.id;
    p.sourceIn = F(60);
    p.sourceOut = F(50);
    CHECK_FALSE(edit::resolvePoints(p, b, kRate));
}

TEST_CASE("insert splits and pushes the destination and the sync-locked tracks only", "[three-point]")
{
    MultiFixture f;
    f.track("V2").locked = true;
    f.track("A2").syncLock = false;
    auto p = f.points(f.b, 50);
    p.sourceIn = F(30);
    p.sourceOut = F(90);
    const auto ids = edit::insertEdit(f.tl(), f.pool(), p);
    REQUIRE(ids.size() == 2);
    const std::vector<Span> expected{{0, 50, 0}, {50, 110, 30}, {110, 160, 50}, {160, 360, 100}, {360, 480, 0}};
    CHECK(spans(f.track("V1")) == expected);
    CHECK(spans(f.track("A1")) == expected);
    CHECK(spans(f.track("V2")) == std::vector<Span>{{150, 240, 0}}); // locked: stays
    CHECK(spans(f.track("A2")) == std::vector<Span>{{0, 600, 0}});   // no sync lock: stays
    CHECK(edit::linkedGroup(f.tl(), ids[0]).size() == 2);
    CHECK(edit::checkInvariants(f.tl(), f.pool()).empty());
    CHECK(edit::checkSync(f.tl()).empty());
    CHECK(strictAlignment(f.tl()).empty());
}

TEST_CASE("insert cuts through clips on sync-locked tracks", "[three-point]")
{
    MultiFixture f;
    auto p = f.points(f.b, 50);
    p.sourceOut = F(60);
    REQUIRE(edit::insertEdit(f.tl(), f.pool(), p).size() == 2);
    CHECK(spans(f.track("V2")) == std::vector<Span>{{210, 300, 0}});
    CHECK(spans(f.track("A2")) == std::vector<Span>{{0, 50, 0}, {110, 660, 50}});
    CHECK(edit::checkInvariants(f.tl(), f.pool()).empty());
}

TEST_CASE("an edit onto a locked or missing destination changes nothing", "[three-point]")
{
    MultiFixture f;
    const Timeline before = f.tl();
    f.track("V1").locked = true;
    Timeline locked = f.tl();
    auto p = f.points(f.b, 50);
    CHECK(edit::insertEdit(f.tl(), f.pool(), p).empty());
    CHECK(edit::overwriteEdit(f.tl(), f.pool(), p).empty());
    CHECK(edit::replaceEdit(f.tl(), f.pool(), p).empty());
    p.recordOut = F(80);
    CHECK(edit::fitToFillEdit(f.tl(), f.pool(), p).empty());
    CHECK(f.tl() == locked);

    f.track("V1").locked = false;
    p = f.points(f.b, 50);
    p.videoTrack = f.track("A2").id; // wrong kind
    CHECK(edit::overwriteEdit(f.tl(), f.pool(), p).empty());
    p.videoTrack = "nope";
    CHECK(edit::insertEdit(f.tl(), f.pool(), p).empty());
    p.mediaId = "unknown";
    CHECK(edit::overwriteEdit(f.tl(), f.pool(), p).empty());
    CHECK(f.tl() == before);
}

TEST_CASE("overwrite replaces the range without moving anything", "[three-point]")
{
    MultiFixture f;
    auto p = f.points(f.b, 80);
    p.sourceOut = F(60);
    REQUIRE(edit::overwriteEdit(f.tl(), f.pool(), p).size() == 2);
    const std::vector<Span> expected{{0, 80, 0}, {80, 140, 0}, {140, 300, 140}, {300, 420, 0}};
    CHECK(spans(f.track("V1")) == expected);
    CHECK(spans(f.track("A1")) == expected);
    CHECK(spans(f.track("V2")) == std::vector<Span>{{150, 240, 0}});
    CHECK(f.tl().duration() == F(600));

    // Backtimed from a timeline Out, punching a hole: the tail on V1 and A1 is relinked as a pair.
    p = f.points(f.b, 0);
    p.sourceOut = F(30);
    p.recordOut = F(400);
    REQUIRE(edit::overwriteEdit(f.tl(), f.pool(), p).size() == 2);
    CHECK(spans(f.track("V1")).back() == Span{400, 420, 100});
    const Id tail = f.clipAt("V1", 410);
    CHECK(edit::linkedGroup(f.tl(), tail).size() == 2);
    CHECK(edit::checkSync(f.tl()).empty());
    CHECK(strictAlignment(f.tl()).empty());
}

TEST_CASE("replace keeps the clip's place and lines the source In up with the playhead", "[three-point]")
{
    MultiFixture f;
    auto p = f.points(f.b, 40);
    p.sourceIn = F(50); // lands on frame 40 of the clip at [0,100): the source starts at 10
    const auto ids = edit::replaceEdit(f.tl(), f.pool(), p);
    REQUIRE(ids.size() == 2);
    CHECK(spans(f.track("V1")).front() == Span{0, 100, 10});
    CHECK(std::get<Clip>(f.track("A1").items[0]).mediaId == f.b.id);
    CHECK(edit::linkedGroup(f.tl(), ids[0]).size() == 2);
    CHECK(f.tl().duration() == F(600));

    // Without a source In the media starts with the clip.
    p = f.points(f.b, 320);
    REQUIRE(edit::replaceEdit(f.tl(), f.pool(), p).size() == 2);
    CHECK(spans(f.track("V1")).back() == Span{300, 420, 0});

    // The clip at [100,300) is longer than b (120 frames): refused.
    const Timeline before = f.tl();
    p = f.points(f.b, 150);
    CHECK(edit::replaceEdit(f.tl(), f.pool(), p).empty());
    // Nothing under the playhead.
    p = f.points(f.b, 500);
    p.audioTrack.clear();
    CHECK(edit::replaceEdit(f.tl(), f.pool(), p).empty());
    CHECK(f.tl() == before);
}

TEST_CASE("fit to fill changes the speed so the source fills the timeline range", "[three-point]")
{
    MultiFixture f;
    auto p = f.points(f.b, 0);
    p.recordIn = F(100);
    p.recordOut = F(160); // 120 source frames into 60
    const auto ids = edit::fitToFillEdit(f.tl(), f.pool(), p);
    REQUIRE(ids.size() == 2);
    const Clip& clip = std::get<Clip>(*f.track("V1").findClip(ids[0])->item);
    CHECK(clip.retime == Retime{2, 1});
    CHECK(clip.sourceRange == TimeRange{Time(0), F(60)});
    CHECK(sourceEnd(Item(clip)) == F(120));
    CHECK(spans(f.track("V1"))[2] == Span{160, 300, 160});
    CHECK(edit::checkInvariants(f.tl(), f.pool()).empty());

    // The viewer reads the media twice as fast; retimed sound is left out until time-stretching.
    auto snap = TimelineSnapshot::fromProject(f.project);
    const auto src = resolveVideo(*snap, F(130));
    REQUIRE(src);
    CHECK(src->media->id == f.b.id);
    CHECK(src->sourceTime == F(60));
    for (const AudioSegment& s : audioSegments(*snap, F(100), F(160)))
        CHECK(s.media->id != f.b.id);

    // Slow motion: 30 frames over 90, and splitting a retimed clip keeps its timing.
    p = f.points(f.b, 0);
    p.sourceIn = F(10);
    p.sourceOut = F(40);
    p.recordIn = F(450);
    p.recordOut = F(540);
    const auto slow = edit::fitToFillEdit(f.tl(), f.pool(), p);
    REQUIRE(slow.size() == 2);
    CHECK(std::get<Clip>(*f.track("V1").findClip(slow[0])->item).retime == Retime{1, 3});
    REQUIRE(edit::splitAt(f.tl(), F(480)));
    const auto right = f.track("V1").clipAt(F(480));
    REQUIRE(right);
    CHECK(right->clip->sourceRange.start == F(20)); // 30 timeline frames at 1/3 speed = 10 source frames
    CHECK(edit::checkSync(f.tl()).empty());

    // A timeline Out is required.
    p.recordOut.reset();
    CHECK(edit::fitToFillEdit(f.tl(), f.pool(), p).empty());
}

TEST_CASE("place on top uses the first free track above the clips there, adding tracks", "[three-point]")
{
    MultiFixture f;
    auto p = f.points(f.b, 160); // [160,280): V1 and V2 busy, A1 and A2 busy
    const auto ids = edit::placeOnTopEdit(f.tl(), f.pool(), p);
    REQUIRE(ids.size() == 2);
    CHECK(f.tl().tracksOfKind(TrackKind::Video).size() == 3);
    CHECK(f.tl().tracksOfKind(TrackKind::Audio).size() == 3);
    CHECK(spans(f.track("V3")) == std::vector<Span>{{160, 280, 0}});
    CHECK(spans(f.track("A3")) == std::vector<Span>{{160, 280, 0}});
    CHECK(spans(f.track("V1")).size() == 3);

    // Past the end of V1 and V2 the picture goes to V1; the music keeps A1 free there too.
    p = f.points(f.b, 450);
    REQUIRE(edit::placeOnTopEdit(f.tl(), f.pool(), p).size() == 2);
    CHECK(spans(f.track("V1")).back() == Span{450, 570, 0});
    CHECK(spans(f.track("A3")).back() == Span{450, 570, 0});

    // Locked tracks are skipped.
    f.track("V3").locked = true;
    p = f.points(f.b, 200);
    p.audioTrack.clear();
    REQUIRE(edit::placeOnTopEdit(f.tl(), f.pool(), p).size() == 1);
    CHECK(spans(f.track("V4")) == std::vector<Span>{{200, 320, 0}});
    CHECK(edit::checkInvariants(f.tl(), f.pool()).empty());
}
