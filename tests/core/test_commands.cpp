#include "core/Commands.h"

#include <catch_amalgamated.hpp>

#include "test_support.h"

using namespace ve;
using namespace ve::test;

namespace {

bool importMedia(CommandStack& stack, const MediaItem& media)
{
    stack.beginTransaction("Import");
    stack.push(std::make_unique<AddMediaCommand>(media));
    stack.edit("Place", [media](Timeline& tl, const MediaPool&) {
        tl.rate = kRate;
        return !edit::placeMedia(tl, media).empty();
    });
    return stack.commit();
}

} // namespace

TEST_CASE("undo and redo walk the history exactly", "[commands]")
{
    Project project = makeEmptyProject();
    CommandStack stack(project);
    int notifications = 0;
    stack.onChanged = [&] { ++notifications; };

    const MediaItem media = testMedia();
    REQUIRE(importMedia(stack, media));
    CHECK(project.media.items.size() == 1);
    CHECK(project.media.root.media.size() == 1);
    CHECK(project.timeline().duration() == F(300));
    const Project afterImport = project;

    CHECK(stack.edit("Split", [](Timeline& tl, const MediaPool&) { return edit::splitAt(tl, F(100)); }));
    const Project afterSplit = project;
    CHECK_FALSE(stack.edit("No-op", [](Timeline& tl, const MediaPool&) { return edit::splitAt(tl, F(100)); }));
    CHECK(stack.undoText() == "Split");

    stack.undo();
    CHECK(project == afterImport);
    stack.undo();
    CHECK(project.media.items.empty());
    CHECK(project.media.root.media.empty());
    CHECK(project.timeline().duration() == Time(0));
    CHECK_FALSE(stack.canUndo());
    stack.redo();
    CHECK(project == afterImport);
    stack.redo();
    CHECK(project == afterSplit);
    CHECK_FALSE(stack.canRedo());
    CHECK(notifications > 0);
}

TEST_CASE("an operation that fails half-way changes nothing", "[commands]")
{
    Project project = makeEmptyProject();
    CommandStack stack(project);
    REQUIRE(importMedia(stack, testMedia()));
    const Project before = project;
    CHECK_FALSE(stack.edit("Partial", [](Timeline& tl, const MediaPool&) {
        edit::splitAt(tl, F(100)); // modifies...
        return false;              // ...then refuses: the command restores the snapshot
    }));
    CHECK(project == before);
}

TEST_CASE("a transaction becomes one undo step; abort restores", "[commands]")
{
    Project project = makeEmptyProject();
    CommandStack stack(project);
    REQUIRE(importMedia(stack, testMedia()));
    const Project start = project;

    stack.beginTransaction("Two cuts");
    CHECK(stack.inTransaction());
    stack.edit("Split", [](Timeline& tl, const MediaPool&) { return edit::splitAt(tl, F(100)); });
    stack.edit("Split", [](Timeline& tl, const MediaPool&) { return edit::splitAt(tl, F(200)); });
    CHECK(project.timeline().tracks[0].clips().size() == 3); // visible while the transaction is open
    CHECK_FALSE(stack.canUndo());
    CHECK(stack.commit());
    CHECK(stack.undoText() == "Two cuts");
    stack.undo();
    CHECK(project == start);
    stack.redo();
    CHECK(project.timeline().tracks[0].clips().size() == 3);

    const Project beforeAbort = project;
    stack.beginTransaction("Abandoned");
    stack.edit("Delete", [](Timeline& tl, const MediaPool&) {
        return edit::deleteClips(tl, {tl.tracks[0].clips()[0].clip->id});
    });
    stack.abort();
    CHECK(project == beforeAbort);
    CHECK(stack.undoText() == "Two cuts");

    stack.beginTransaction("Empty");
    CHECK_FALSE(stack.commit());
    CHECK(stack.undoText() == "Two cuts");
}

TEST_CASE("steps of one gesture merge: every step starts from the gesture's start", "[commands]")
{
    Project project = makeEmptyProject();
    CommandStack stack(project);
    REQUIRE(importMedia(stack, testMedia()));
    REQUIRE(stack.edit("Split", [](Timeline& tl, const MediaPool&) { return edit::splitAt(tl, F(100)); }));
    const Project start = project;
    const Id second = project.timeline().tracks[0].clips()[1].clip->id;

    auto drag = [&](Time delta) {
        stack.edit("Move", [second, delta](Timeline& tl, const MediaPool&) { return edit::moveClips(tl, {second}, delta); },
                   "drag-move");
    };
    stack.beginTransaction("Move");
    drag(-F(60)); // over the first clip: overwrites its tail...
    CHECK(project.timeline().tracks[0].clips()[0].clip->sourceRange.duration == F(40));
    drag(F(20)); // ...and moving on restores it, because this step starts from the original state
    CHECK(project.timeline().tracks[0].clips()[0].clip->sourceRange.duration == F(100));
    drag(F(0)); // back to the start: no change at all
    CHECK(project == start);
    drag(F(45));
    CHECK(stack.commit());

    CHECK(project.timeline().tracks[0].clips()[1].start == F(145));
    CHECK(project.timeline().tracks[0].clips()[0].clip->sourceRange.duration == F(100));
    stack.undo(); // one step for the whole gesture
    CHECK(project == start);
}

TEST_CASE("clean state follows saves through undo and redo", "[commands]")
{
    Project project = makeEmptyProject();
    CommandStack stack(project);
    REQUIRE(importMedia(stack, testMedia()));
    REQUIRE(stack.edit("Split", [](Timeline& tl, const MediaPool&) { return edit::splitAt(tl, F(100)); }));
    stack.setClean();
    CHECK(stack.isClean());
    stack.undo();
    CHECK_FALSE(stack.isClean());
    stack.redo();
    CHECK(stack.isClean());
    stack.undo();
    CHECK(stack.edit("Move", [](Timeline& tl, const MediaPool&) {
        return edit::moveClips(tl, {tl.tracks[0].clips()[0].clip->id}, F(10));
    }));
    CHECK_FALSE(stack.isClean()); // the saved state was dropped from the redo history

    stack.beginTransaction("Open");
    CHECK_FALSE(stack.isClean());
    stack.undo(); // undo while a gesture is open aborts the gesture first
    CHECK_FALSE(stack.inTransaction());
}
