#include "core/ProjectFile.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace ve {

namespace {

using json::Value;
namespace fs = std::filesystem;

constexpr int kFormatVersion = 2; // 2: times in flicks

fs::path u8path(const std::string& s)
{
    return fs::path(std::u8string(s.begin(), s.end()));
}

std::string toUtf8(const fs::path& p)
{
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

Value timeValue(Time t) { return Value(t.flicks); }
Time timeOf(const Value& v) { return Time(v.asInt()); }

Value rangeValue(const TimeRange& r)
{
    Value o = Value::object();
    o.set("start", timeValue(r.start));
    o.set("duration", timeValue(r.duration));
    return o;
}

TimeRange rangeOf(const Value& v)
{
    return {timeOf(v["start"]), timeOf(v["duration"])};
}

Value rateValue(Rate r)
{
    Value o = Value::object();
    o.set("num", r.num);
    o.set("den", r.den);
    return o;
}

Rate rateOf(const Value& v, Rate fallback)
{
    Rate r{int(v["num"].asInt(fallback.num)), int(v["den"].asInt(fallback.den))};
    return r.isValid() ? r : fallback;
}

const char* interpolationName(Interpolation i)
{
    switch (i) {
    case Interpolation::Hold: return "hold";
    case Interpolation::Bezier: return "bezier";
    case Interpolation::Linear:
    default: return "linear";
    }
}

Interpolation interpolationOf(const std::string& s)
{
    if (s == "hold")
        return Interpolation::Hold;
    if (s == "bezier")
        return Interpolation::Bezier;
    return Interpolation::Linear;
}

Value paramValue(const AnimatedParam& p)
{
    Value o = Value::object();
    o.set("value", p.value);
    if (p.isAnimated()) {
        Value keys = Value::array();
        for (const Keyframe& k : p.keys) {
            Value kv = Value::object();
            kv.set("time", timeValue(k.time));
            kv.set("value", k.value);
            kv.set("interpolation", interpolationName(k.interpolation));
            keys.push(std::move(kv));
        }
        o.set("keyframes", std::move(keys));
    }
    return o;
}

AnimatedParam paramOf(const Value& v)
{
    AnimatedParam p;
    p.value = v["value"].asDouble();
    for (const Value& kv : v["keyframes"].asArray())
        p.keys.push_back({timeOf(kv["time"]), kv["value"].asDouble(), interpolationOf(kv["interpolation"].asString())});
    return p;
}

Value effectValue(const Effect& e)
{
    Value o = Value::object();
    o.set("id", e.id);
    o.set("type", e.type);
    o.set("enabled", e.enabled);
    Value params = Value::object();
    for (const auto& [name, p] : e.params)
        params.set(name, paramValue(p));
    o.set("params", std::move(params));
    return o;
}

Effect effectOf(const Value& v)
{
    Effect e;
    e.id = v["id"].asString();
    e.type = v["type"].asString();
    e.enabled = v["enabled"].asBool(true);
    for (const auto& [name, p] : v["params"].asObject())
        e.params.emplace(name, paramOf(p));
    return e;
}

Value effectsValue(const std::vector<Effect>& effects)
{
    Value a = Value::array();
    for (const Effect& e : effects)
        a.push(effectValue(e));
    return a;
}

std::vector<Effect> effectsOf(const Value& v)
{
    std::vector<Effect> out;
    for (const Value& e : v.asArray())
        out.push_back(effectOf(e));
    return out;
}

void writeClipBase(Value& o, const ClipBase& c)
{
    o.set("id", c.id);
    o.set("name", c.name);
    o.set("source_range", rangeValue(c.sourceRange));
    o.set("enabled", c.enabled);
    o.set("link_group", c.linkGroup);
}

void readClipBase(const Value& o, ClipBase& c)
{
    c.id = o["id"].asString();
    c.name = o["name"].asString();
    c.sourceRange = rangeOf(o["source_range"]);
    c.enabled = o["enabled"].asBool(true);
    c.linkGroup = o["link_group"].asString();
}

const char* alignmentName(TransitionAlignment a)
{
    switch (a) {
    case TransitionAlignment::StartOnCut: return "start_on_cut";
    case TransitionAlignment::EndOnCut: return "end_on_cut";
    case TransitionAlignment::Centered:
    default: return "centered";
    }
}

Value itemValue(const Item& item)
{
    Value o = Value::object();
    if (const Clip* c = std::get_if<Clip>(&item)) {
        o.set("schema", "Clip.1");
        writeClipBase(o, *c);
        o.set("media_id", c->mediaId);
        o.set("effects", effectsValue(c->effects));
        o.set("grade", effectsValue(c->grade.nodes));
        Value retime = Value::object();
        retime.set("speed_num", c->retime.speedNum);
        retime.set("speed_den", c->retime.speedDen);
        o.set("retime", std::move(retime));
    } else if (const Gap* g = std::get_if<Gap>(&item)) {
        o.set("schema", "Gap.1");
        o.set("duration", timeValue(g->duration));
    } else if (const Transition* t = std::get_if<Transition>(&item)) {
        o.set("schema", "Transition.1");
        o.set("id", t->id);
        o.set("kind", t->kind);
        o.set("in_offset", timeValue(t->inOffset));
        o.set("out_offset", timeValue(t->outOffset));
        o.set("alignment", alignmentName(t->alignment));
    } else if (const CompoundClip* cc = std::get_if<CompoundClip>(&item)) {
        o.set("schema", "CompoundClip.1");
        writeClipBase(o, *cc);
        o.set("timeline_id", cc->timelineId);
    } else if (const MulticamClip* mc = std::get_if<MulticamClip>(&item)) {
        o.set("schema", "MulticamClip.1");
        writeClipBase(o, *mc);
        Value angles = Value::array();
        for (const Id& a : mc->angles)
            angles.push(a);
        o.set("angles", std::move(angles));
        o.set("active_angle", mc->activeAngle);
    }
    return o;
}

std::optional<Item> itemOf(const Value& o)
{
    const std::string& schema = o["schema"].asString();
    if (schema.starts_with("Clip.")) {
        Clip c;
        readClipBase(o, c);
        c.mediaId = o["media_id"].asString();
        c.effects = effectsOf(o["effects"]);
        c.grade.nodes = effectsOf(o["grade"]);
        c.retime.speedNum = int(o["retime"]["speed_num"].asInt(1));
        c.retime.speedDen = int(o["retime"]["speed_den"].asInt(1));
        return c;
    }
    if (schema.starts_with("Gap."))
        return Gap{timeOf(o["duration"])};
    if (schema.starts_with("Transition.")) {
        Transition t;
        t.id = o["id"].asString();
        t.kind = o["kind"].asString();
        t.inOffset = timeOf(o["in_offset"]);
        t.outOffset = timeOf(o["out_offset"]);
        const std::string& a = o["alignment"].asString();
        t.alignment = a == "start_on_cut" ? TransitionAlignment::StartOnCut
            : a == "end_on_cut"           ? TransitionAlignment::EndOnCut
                                          : TransitionAlignment::Centered;
        return t;
    }
    if (schema.starts_with("CompoundClip.")) {
        CompoundClip c;
        readClipBase(o, c);
        c.timelineId = o["timeline_id"].asString();
        return c;
    }
    if (schema.starts_with("MulticamClip.")) {
        MulticamClip c;
        readClipBase(o, c);
        for (const Value& a : o["angles"].asArray())
            c.angles.push_back(a.asString());
        c.activeAngle = int(o["active_angle"].asInt());
        return c;
    }
    return std::nullopt;
}

Value mediaValue(const MediaItem& m, const std::string& projectDir)
{
    Value o = Value::object();
    o.set("schema", "MediaItem.1");
    o.set("id", m.id);
    o.set("name", m.name);
    o.set("path", m.path);
    if (!projectDir.empty())
        o.set("relative_path", toUtf8(u8path(m.path).lexically_relative(u8path(projectDir))));
    o.set("duration", timeValue(m.duration));
    o.set("start_timecode", timeValue(m.startTimecode));
    o.set("has_video", m.hasVideo);
    o.set("width", m.width);
    o.set("height", m.height);
    o.set("frame_rate", rateValue(m.frameRate));
    o.set("video_codec", m.videoCodec);
    o.set("has_audio", m.hasAudio);
    o.set("sample_rate", m.sampleRate);
    o.set("channels", m.channels);
    o.set("audio_codec", m.audioCodec);
    Value meta = Value::object();
    for (const auto& [k, v] : m.metadata)
        meta.set(k, v);
    o.set("metadata", std::move(meta));
    o.set("proxy_path", m.proxyPath);
    return o;
}

MediaItem mediaOf(const Value& o, const std::string& projectDir)
{
    MediaItem m;
    m.id = o["id"].asString();
    m.name = o["name"].asString();
    m.path = o["path"].asString();
    m.duration = timeOf(o["duration"]);
    m.startTimecode = timeOf(o["start_timecode"]);
    m.hasVideo = o["has_video"].asBool();
    m.width = int(o["width"].asInt());
    m.height = int(o["height"].asInt());
    m.frameRate = rateOf(o["frame_rate"], Rate{30, 1});
    m.videoCodec = o["video_codec"].asString();
    m.hasAudio = o["has_audio"].asBool();
    m.sampleRate = int(o["sample_rate"].asInt());
    m.channels = int(o["channels"].asInt());
    m.audioCodec = o["audio_codec"].asString();
    for (const auto& [k, v] : o["metadata"].asObject())
        m.metadata.emplace(k, v.asString());
    m.proxyPath = o["proxy_path"].asString();

    // Relink: if the absolute path is gone, try the one relative to the project file.
    const std::string& relative = o["relative_path"].asString();
    std::error_code ec;
    if (!fs::exists(u8path(m.path), ec) && !relative.empty() && !projectDir.empty()) {
        const fs::path candidate = (u8path(projectDir) / u8path(relative)).lexically_normal();
        if (fs::exists(candidate, ec))
            m.path = toUtf8(candidate);
    }
    return m;
}

Value folderValue(const Folder& f)
{
    Value o = Value::object();
    o.set("id", f.id);
    o.set("name", f.name);
    Value media = Value::array();
    for (const Id& id : f.media)
        media.push(id);
    o.set("media", std::move(media));
    Value folders = Value::array();
    for (const Folder& child : f.folders)
        folders.push(folderValue(child));
    o.set("folders", std::move(folders));
    return o;
}

Folder folderOf(const Value& o)
{
    Folder f;
    f.id = o["id"].asString();
    f.name = o["name"].asString();
    for (const Value& id : o["media"].asArray())
        f.media.push_back(id.asString());
    for (const Value& child : o["folders"].asArray())
        f.folders.push_back(folderOf(child));
    return f;
}

} // namespace

json::Value timelineToJson(const Timeline& tl)
{
    Value tracks = Value::array();
    for (const Track& t : tl.tracks) {
        Value children = Value::array();
        for (const Item& item : t.items)
            children.push(itemValue(item));
        Value to = Value::object();
        to.set("schema", "Track.1");
        to.set("id", t.id);
        to.set("name", t.name);
        to.set("kind", t.kind == TrackKind::Video ? "video" : "audio");
        to.set("enabled", t.enabled);
        to.set("locked", t.locked);
        to.set("sync_lock", t.syncLock);
        to.set("children", std::move(children));
        tracks.push(std::move(to));
    }
    Value o = Value::object();
    o.set("schema", "Timeline.1");
    o.set("id", tl.id);
    o.set("name", tl.name);
    o.set("rate", rateValue(tl.rate));
    o.set("width", tl.width);
    o.set("height", tl.height);
    o.set("sample_rate", tl.sampleRate);
    o.set("tracks", std::move(tracks));
    return o;
}

Timeline timelineFromJson(const json::Value& o)
{
    Timeline tl;
    tl.id = o["id"].asString();
    tl.name = o["name"].asString();
    tl.rate = rateOf(o["rate"], Rate{30, 1});
    tl.width = int(o["width"].asInt(1920));
    tl.height = int(o["height"].asInt(1080));
    tl.sampleRate = int(o["sample_rate"].asInt(48000));
    for (const Value& to : o["tracks"].asArray()) {
        Track t;
        t.id = to["id"].asString();
        t.name = to["name"].asString();
        t.kind = to["kind"].asString() == "audio" ? TrackKind::Audio : TrackKind::Video;
        t.enabled = to["enabled"].asBool(true);
        t.locked = to["locked"].asBool(false);
        t.syncLock = to["sync_lock"].asBool(true);
        for (const Value& iv : to["children"].asArray()) {
            if (auto item = itemOf(iv))
                t.items.push_back(std::move(*item));
        }
        t.normalize();
        tl.tracks.push_back(std::move(t));
    }
    return tl;
}

json::Value projectToJson(const Project& project, const std::string& projectDir)
{
    Value items = Value::array();
    for (const MediaItem& m : project.media.items)
        items.push(mediaValue(m, projectDir));
    Value pool = Value::object();
    pool.set("items", std::move(items));
    pool.set("root", folderValue(project.media.root));

    Value timelines = Value::array();
    for (const Timeline& tl : project.timelines)
        timelines.push(timelineToJson(tl));

    Value o = Value::object();
    o.set("schema", "Project.1");
    o.set("format_version", kFormatVersion);
    o.set("time_unit", "flicks (1/705600000 s)");
    o.set("name", project.name);
    o.set("media_pool", std::move(pool));
    o.set("timelines", std::move(timelines));
    o.set("current_timeline", project.currentTimeline);
    return o;
}

std::optional<Project> projectFromJson(const json::Value& o, const std::string& projectDir, std::string* error)
{
    auto fail = [&](const char* message) -> std::optional<Project> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (!o["schema"].asString().starts_with("Project."))
        return fail("Not a project file");
    const std::int64_t version = o["format_version"].asInt();
    if (version > kFormatVersion)
        return fail("Project was saved by a newer version");
    if (version < kFormatVersion)
        return fail("Project uses an old format (frames instead of flicks) that this version does not read");

    Project p;
    p.name = o["name"].asString();
    for (const Value& m : o["media_pool"]["items"].asArray())
        p.media.items.push_back(mediaOf(m, projectDir));
    p.media.root = folderOf(o["media_pool"]["root"]);
    for (const Value& tl : o["timelines"].asArray())
        p.timelines.push_back(timelineFromJson(tl));
    if (p.timelines.empty())
        p.timelines.push_back(makeDefaultTimeline());
    p.currentTimeline = o["current_timeline"].asString();
    if (!p.findTimeline(p.currentTimeline))
        p.currentTimeline = p.timelines.front().id;
    return p;
}

bool saveProject(const Project& project, const std::string& path, std::string* error)
{
    const fs::path target = u8path(path);
    const fs::path dir = target.parent_path();
    const std::string text = json::serialize(projectToJson(project, toUtf8(fs::absolute(dir))));
    fs::path temp = target;
    temp += u8".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(text.data(), std::streamsize(text.size())) || !out.flush()) {
            if (error)
                *error = "Cannot write " + path;
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, target, ec);
    if (ec) {
        fs::remove(temp, ec);
        if (error)
            *error = "Cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

std::optional<Project> loadProject(const std::string& path, std::string* error)
{
    std::ifstream in(u8path(path), std::ios::binary);
    if (!in) {
        if (error)
            *error = "Cannot open " + path;
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string parseError;
    auto doc = json::parse(buffer.str(), &parseError);
    if (!doc) {
        if (error)
            *error = "Invalid JSON: " + parseError;
        return std::nullopt;
    }
    std::error_code ec;
    const fs::path dir = fs::absolute(u8path(path), ec).parent_path();
    return projectFromJson(*doc, toUtf8(dir), error);
}

} // namespace ve
