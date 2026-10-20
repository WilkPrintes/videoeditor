// Golden states (spec §6.1 acceptance): every operation runs on the same multitrack timeline, built
// with deterministic ids, and the states before and after it (core/ProjectFile timelineToJson) must
// match tests/core/golden/<name>.json exactly. The "summary" lines (in frames) are there for review;
// the "timeline" objects are the exact state. Locked and non-sync-locked tracks are part of the cases.
//
// After an intended change, run the tests with VE_UPDATE_GOLDEN=1 to rewrite the files, and review the
// diff of the summaries before committing them.

#include "core/Edit.h"
#include "core/ProjectFile.h"

#include <catch_amalgamated.hpp>

#include "test_support.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

using namespace ve;
using namespace ve::test;
namespace fs = std::filesystem;

namespace {

json::Value stateOf(const Project& project)
{
    auto timelineState = [&](const Timeline& tl) {
        json::Value o = json::Value::object();
        json::Value summary = json::Value::array();
        for (const std::string& line : describe(tl, project.media))
            summary.push(line);
        o.set("summary", std::move(summary));
        o.set("timeline", timelineToJson(tl));
        return o;
    };
    json::Value o = timelineState(project.timeline());
    json::Value nested = json::Value::array();
    for (const Timeline& tl : project.timelines) {
        if (tl.id != project.currentTimeline)
            nested.push(timelineState(tl));
    }
    o.set("nested_timelines", std::move(nested));
    return o;
}

std::string readText(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string s = buffer.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end()); // tolerate CRLF checkouts
    return s;
}

void checkGolden(const std::string& name, const std::string& description, const Project& before,
                 const Project& after, bool applied)
{
    json::Value doc = json::Value::object();
    doc.set("operation", name);
    doc.set("description", description);
    doc.set("applied", applied);
    doc.set("before", stateOf(before));
    doc.set("after", stateOf(after));
    const std::string text = json::serialize(doc) + "\n";
    const fs::path path = fs::path(VE_GOLDEN_DIR) / (name + ".json");

    const char* update = std::getenv("VE_UPDATE_GOLDEN");
    if (update && std::string(update) == "1") {
        std::ofstream(path, std::ios::binary) << text;
        WARN("rewrote " << path.string());
        return;
    }
    const std::string expected = readText(path);
    INFO("golden file " << path.string() << " (VE_UPDATE_GOLDEN=1 rewrites it)");
    REQUIRE_FALSE(expected.empty());
    if (text == expected)
        return;
    // Show the first line that differs and keep the whole new state for a diff.
    std::istringstream a(text), b(expected);
    std::string la, lb;
    int line = 0;
    while (true) {
        const bool okA = bool(std::getline(a, la));
        const bool okB = bool(std::getline(b, lb));
        ++line;
        if (!okA || !okB || la != lb)
            break;
    }
    const fs::path actual = fs::temp_directory_path() / (name + ".actual.json");
    std::ofstream(actual, std::ios::binary) << text;
    FAIL("differs at line " << line << "\n  expected: " << lb << "\n  actual:   " << la << "\nfull output: "
                            << actual.string());
}

// Builds the fixture, lets `setup` adjust it (locks, earlier edits), applies `op` and compares.
void golden(const std::string& name, const std::string& description, const std::function<void(MultiFixture&)>& setup,
            const std::function<bool(MultiFixture&)>& op, bool inSync = true)
{
    MultiFixture f;
    if (setup)
        setup(f);
    const Project before = f.project;
    const bool applied = op(f);
    INFO(name);
    for (const Timeline& tl : f.project.timelines) {
        CHECK(edit::checkInvariants(tl, f.pool()).empty());
        CHECK(edit::checkSync(tl).empty() == inSync);
    }
    if (!applied)
        CHECK(f.project == before); // refused edits change nothing
    checkGolden(name, description, before, f.project, applied);
}

edit::EditPoints marked(MultiFixture& f, const MediaItem& m, std::int64_t at, std::optional<std::int64_t> in,
                        std::optional<std::int64_t> out)
{
    edit::EditPoints p = f.points(m, at);
    if (in)
        p.sourceIn = F(*in);
    if (out)
        p.sourceOut = F(*out);
    return p;
}

void lockV2SyncOffA2(MultiFixture& f)
{
    f.track("V2").locked = true;
    f.track("A2").syncLock = false;
}

void syncOffA2(MultiFixture& f)
{
    f.track("A2").syncLock = false;
}

} // namespace

TEST_CASE("golden states: three and four point edits", "[golden][three-point]")
{
    golden("insert_sync_lock", "Insert b [30,90) at frame 50 on V1/A1. V2 is locked and A2 has sync lock off: both stay.",
           lockV2SyncOffA2, [](MultiFixture& f) { return !edit::insertEdit(f.tl(), f.pool(), marked(f, f.b, 50, 30, 90)).empty(); });
    golden("insert_cut_through", "Insert b [0,60) at frame 50 with every track sync-locked: the title moves, the music is cut.",
           nullptr, [](MultiFixture& f) { return !edit::insertEdit(f.tl(), f.pool(), marked(f, f.b, 50, {}, 60)).empty(); });
    golden("insert_locked_destination", "Insert onto a locked V1: refused, nothing changes.",
           [](MultiFixture& f) { f.track("V1").locked = true; },
           [](MultiFixture& f) { return !edit::insertEdit(f.tl(), f.pool(), marked(f, f.b, 50, {}, 60)).empty(); });
    golden("overwrite", "Overwrite b [0,60) at frame 80 on V1/A1: the cut clips shrink, nothing moves.", nullptr,
           [](MultiFixture& f) { return !edit::overwriteEdit(f.tl(), f.pool(), marked(f, f.b, 80, {}, 60)).empty(); });
    golden("overwrite_backtimed", "Overwrite b [0,30) ending at the timeline Out 400 (three points: source In/Out, timeline Out).",
           nullptr, [](MultiFixture& f) {
               edit::EditPoints p = marked(f, f.b, 0, 0, 30);
               p.recordOut = F(400);
               return !edit::overwriteEdit(f.tl(), f.pool(), p).empty();
           });
    golden("replace", "Replace the clip under frame 40 with b, its source In 50 on the playhead (b plays from 10).", nullptr,
           [](MultiFixture& f) { return !edit::replaceEdit(f.tl(), f.pool(), marked(f, f.b, 40, 50, {})).empty(); });
    golden("fit_to_fill", "Fit all of b (120 frames) into the timeline In/Out [100,160): speed 2/1.", nullptr,
           [](MultiFixture& f) {
               edit::EditPoints p = marked(f, f.b, 0, {}, {});
               p.recordIn = F(100);
               p.recordOut = F(160);
               return !edit::fitToFillEdit(f.tl(), f.pool(), p).empty();
           });
    golden("place_on_top", "Place b on top at frame 160: V1/V2 and A1/A2 are busy there, so V3 and A3 are added.", nullptr,
           [](MultiFixture& f) { return !edit::placeOnTopEdit(f.tl(), f.pool(), marked(f, f.b, 160, {}, {})).empty(); });
}

TEST_CASE("golden states: trims", "[golden][trim]")
{
    auto clip = [](MultiFixture& f, const char* track, std::int64_t at) { return f.clipAt(track, at); };
    golden("ripple_trim_tail", "Ripple the tail of X (V1/A1 [0,100)) by -20; V2 follows, A2 has sync lock off.", syncOffA2,
           [clip](MultiFixture& f) { return edit::rippleTrim(f.tl(), f.pool(), clip(f, "V1", 50), edit::Edge::Tail, -F(20)); });
    golden("ripple_trim_tail_blocked", "The same ripple with A2 sync-locked: the music spans the cut, so it is refused.",
           nullptr, [clip](MultiFixture& f) {
               return edit::rippleTrim(f.tl(), f.pool(), clip(f, "V1", 50), edit::Edge::Tail, -F(20));
           });
    golden("ripple_trim_extend", "Ripple the tail of X by +30 with every track sync-locked: the music on A2 is cut through.",
           nullptr, [clip](MultiFixture& f) {
               return edit::rippleTrim(f.tl(), f.pool(), clip(f, "V1", 50), edit::Edge::Tail, F(30));
           });
    golden("ripple_trim_head", "Ripple the head of Y (V1/A1 [100,300)) by +30: Y keeps its start, shows from 130.", syncOffA2,
           [clip](MultiFixture& f) { return edit::rippleTrim(f.tl(), f.pool(), clip(f, "V1", 200), edit::Edge::Head, F(30)); });
    golden("roll", "Roll the cut between X and Y by +25: nothing else moves.", nullptr,
           [clip](MultiFixture& f) { return edit::rollTrim(f.tl(), f.pool(), clip(f, "V1", 50), edit::Edge::Tail, F(25)); });
    golden("l_cut", "Roll only the sound of the X/Y cut by +20 (asymmetric): X's sound runs under Y's picture.", nullptr,
           [clip](MultiFixture& f) {
               return edit::rollTrim(f.tl(), f.pool(), clip(f, "A1", 50), edit::Edge::Tail, F(20), false);
           });
    golden("j_cut", "Roll only the sound of the X/Y cut by -20 (asymmetric): Y's sound starts under X's picture.", nullptr,
           [clip](MultiFixture& f) {
               return edit::rollTrim(f.tl(), f.pool(), clip(f, "A1", 50), edit::Edge::Tail, -F(20), false);
           });
    golden("slip", "Slip Y by -40: same place and length, it shows media 60-260.", nullptr,
           [clip](MultiFixture& f) { return edit::slipClip(f.tl(), f.pool(), clip(f, "V1", 200), -F(40)); });
    golden("slide", "Slide Y by +20: X gets 20 frames longer, Z loses its first 20.", nullptr,
           [clip](MultiFixture& f) { return edit::slideClip(f.tl(), f.pool(), clip(f, "V1", 200), F(20)); });
    golden("ripple_delete", "Ripple delete Y: V1/A1 close the hole; V2 is locked and A2 has sync lock off.", lockV2SyncOffA2,
           [clip](MultiFixture& f) { return edit::rippleDelete(f.tl(), {clip(f, "V1", 200)}); });
    golden("ripple_delete_blocked", "Ripple delete Y with every track sync-locked: the title and the music are in the hole, refused.",
           nullptr, [clip](MultiFixture& f) { return edit::rippleDelete(f.tl(), {clip(f, "V1", 200)}); });
}

TEST_CASE("golden states: phase 3 edits and organization", "[golden][phase3]")
{
    golden("append", "Append b [0,60) after the end of V1/A1; the playhead and the timeline In are ignored.", nullptr,
           [](MultiFixture& f) {
               edit::EditPoints p = marked(f, f.b, 30, {}, 60);
               p.recordIn = F(10);
               return !edit::appendEdit(f.tl(), f.pool(), p).empty();
           });
    golden("ripple_overwrite", "Ripple overwrite of Y (under frame 200) with b [0,60): Z moves up 140 frames. V2 locked, A2 without sync lock.",
           lockV2SyncOffA2,
           [](MultiFixture& f) { return !edit::rippleOverwriteEdit(f.tl(), f.pool(), marked(f, f.b, 200, {}, 60)).empty(); });
    golden("insert_gap", "Insert a 30-frame gap at frame 50: X is cut, everything after moves; A2 has sync lock off.", syncOffA2,
           [](MultiFixture& f) { return edit::insertGap(f.tl(), F(50), F(30)); });
    golden("delete_gaps", "Lift X, then delete gaps: the empty [0,100) closes on V1, A1 and V2; A2 has sync lock off.", syncOffA2,
           [](MultiFixture& f) {
               edit::deleteClips(f.tl(), {f.clipAt("V1", 50)});
               return edit::deleteGaps(f.tl());
           });
    golden("swap_clips", "Swap Y with Z (to the right), picture and sound together.", nullptr,
           [](MultiFixture& f) { return edit::swapClips(f.tl(), f.clipAt("V1", 200), 1); });
    golden("labels", "Colour X orange, flag it red and green, rename V2 and colour it purple.", nullptr, [](MultiFixture& f) {
        const Id x = f.clipAt("V1", 50);
        const Id v2 = f.track("V2").id;
        return edit::setClipColor(f.tl(), {x, f.clipAt("A1", 50)}, "orange") && edit::setClipFlag(f.tl(), {x}, "red", true)
            && edit::setClipFlag(f.tl(), {x}, "green", true) && edit::renameTrack(f.tl(), v2, "Titles")
            && edit::setTrackColor(f.tl(), v2, "purple");
    });
    golden("unlinked_move", "With linked selection off, move only the picture of Y 10 frames later: its sound stays (out of sync on purpose).",
           nullptr, [](MultiFixture& f) { return edit::moveClips(f.tl(), {f.clipAt("V1", 200)}, F(10), 0, false); },
           false);
}

TEST_CASE("golden states: multitrack and compound clips", "[golden][compound]")
{
    golden("move_to_track_above", "Move X up one track: picture to V2, sound to A2 (over the start of the music).", nullptr,
           [](MultiFixture& f) { return edit::moveClips(f.tl(), {f.clipAt("V1", 50)}, Time(0), 1); });
    golden("compose", "Compound clip of Y and the title: a nested timeline with V1, V2 and A1; compounds on V1 and A1.",
           nullptr, [](MultiFixture& f) {
               return edit::composeClips(f.project, {f.clipAt("V1", 200), f.clipAt("V2", 200)}, "Scene").has_value();
           });

    // Decompose starts from the composed state and must land on the original fixture.
    MultiFixture original;
    golden("decompose", "Decompose the compound clip made above: the state before composing comes back.",
           [](MultiFixture& f) { edit::composeClips(f.project, {f.clipAt("V1", 200), f.clipAt("V2", 200)}, "Scene"); },
           [&original](MultiFixture& f) {
               const Id compound = f.clipAt("V1", 200);
               const bool ok = edit::decomposeClip(f.project, compound);
               CHECK(f.project.timelines == original.project.timelines);
               return ok;
           });
}
