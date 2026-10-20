#include "core/ProjectFile.h"

#include <catch_amalgamated.hpp>

#include "test_support.h"

#include <filesystem>
#include <fstream>
#include <random>

using namespace ve;
using namespace ve::test;
namespace fs = std::filesystem;

namespace {

// A fresh, empty directory under the system temp folder, removed at the end of the test.
struct TempDir {
    fs::path path;
    TempDir()
    {
        std::random_device rd;
        path = fs::temp_directory_path() / ("ve_tests_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string file(const std::u8string& name) const
    {
        const std::u8string s = (path / fs::path(name)).generic_u8string();
        return std::string(s.begin(), s.end());
    }
};

Project richProject()
{
    Fixture f;
    Project p = f.project;
    p.name = "Projeto de teste";
    p.timeline().rate = {30000, 1001};
    p.media.items[0].metadata["title"] = "Claquete";
    p.media.items[0].startTimecode = timeOfFrame(3600 * 30, {30000, 1001});
    p.media.root.folders.push_back(Folder{newId(), "Cenas", {p.media.items[0].id}, {}});

    Timeline& tl = p.timeline();
    edit::splitAt(tl, F(100));
    Clip& clip = std::get<Clip>(tl.tracks[0].items[0]);
    Effect opacity;
    opacity.id = newId();
    opacity.type = "opacity";
    opacity.params["amount"].keys = {{Time(0), 0.0, Interpolation::Linear}, {F(10), 1.0, Interpolation::Bezier}};
    clip.effects.push_back(opacity);
    clip.grade.nodes.push_back(Effect{newId(), "lift_gamma_gain", true, {{"gain", AnimatedParam{1.25, {}}}}});
    clip.retime = {1, 2};
    tl.tracks[0].items.insert(tl.tracks[0].items.begin() + 1,
                              Transition{newId(), "dissolve", F(5), F(5), TransitionAlignment::Centered});

    Timeline nested = makeDefaultTimeline();
    nested.name = "Composto";
    p.timelines.push_back(nested); // reallocates: re-fetch the main timeline below
    Timeline& main = p.timeline();
    CompoundClip cc;
    cc.id = newId();
    cc.name = "Composto";
    cc.timelineId = nested.id;
    cc.sourceRange = {Time(0), F(20)};
    MulticamClip mc;
    mc.id = newId();
    mc.sourceRange = {F(5), F(20)};
    mc.angles = {p.media.items[0].id, "outro-angulo"};
    mc.activeAngle = 1;
    IdRemap links;
    main.tracks[0].placeItem(F(400), cc, links);
    main.tracks[0].placeItem(F(420), mc, links);
    main.tracks[1].locked = true;
    main.tracks[1].syncLock = false;
    return p;
}

} // namespace

TEST_CASE("project JSON round trip preserves every item kind", "[project]")
{
    const Project p = richProject();
    const json::Value doc = projectToJson(p);
    std::string error;
    auto back = projectFromJson(*json::parse(json::serialize(doc)), "", &error);
    INFO(error);
    REQUIRE(back.has_value());
    CHECK(back->timelines == p.timelines);
    CHECK(back->media == p.media);
    CHECK(back->name == p.name);
    CHECK(back->currentTimeline == p.currentTimeline);
}

TEST_CASE("save and load through a UTF-8 path", "[project]")
{
    TempDir dir;
    const Project p = richProject();
    const std::string path = dir.file(u8"edição ção.veproj");
    std::string error;
    REQUIRE(saveProject(p, path, &error));
    auto back = loadProject(path, &error);
    INFO(error);
    REQUIRE(back.has_value());
    CHECK(back->timelines == p.timelines);
    CHECK(back->media == p.media);
    // Saving again over the same file works (atomic replace).
    REQUIRE(saveProject(*back, path, &error));
}

TEST_CASE("media is relinked relative to the project when the absolute path is gone", "[project]")
{
    TempDir dir;
    fs::create_directories(dir.path / "media");
    std::ofstream(dir.path / "media" / "clip.mp4") << "x";

    Project p = makeEmptyProject();
    MediaItem m = testMedia();
    m.path = dir.file(u8"media/clip.mp4");
    p.media.items.push_back(m);
    json::Value doc = projectToJson(p, dir.file(u8""));
    // Pretend the project folder was moved: the stored absolute path no longer exists.
    json::Value items = json::Value::array();
    json::Value moved = doc["media_pool"]["items"].asArray()[0];
    moved.set("path", "Z:/somewhere/else/clip.mp4");
    items.push(moved);
    json::Value pool = doc["media_pool"];
    pool.set("items", items);
    doc.set("media_pool", pool);

    std::string error;
    auto back = projectFromJson(doc, dir.file(u8""), &error);
    REQUIRE(back.has_value());
    CHECK(back->media.items[0].path == m.path);
}

TEST_CASE("unreadable projects are reported, not half-loaded", "[project]")
{
    TempDir dir;
    std::string error;
    CHECK_FALSE(loadProject(dir.file(u8"missing.veproj"), &error));
    CHECK_FALSE(error.empty());

    std::ofstream(dir.path / "bad.veproj") << "{ not json";
    CHECK_FALSE(loadProject(dir.file(u8"bad.veproj"), &error));

    std::ofstream(dir.path / "old.veproj") << R"({"schema": "Project.1", "format_version": 1})";
    CHECK_FALSE(loadProject(dir.file(u8"old.veproj"), &error));
    CHECK(error.find("old format") != std::string::npos);

    std::ofstream(dir.path / "other.veproj") << R"({"schema": "Something.1"})";
    CHECK_FALSE(loadProject(dir.file(u8"other.veproj"), &error));
}
