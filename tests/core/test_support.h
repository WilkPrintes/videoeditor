#pragma once

// Shared fixtures for the core tests.

#include "core/Edit.h"
#include "core/Model.h"

#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ve::test {

inline constexpr Rate kRate{30, 1};

// Frame n at 30 fps, in flicks.
inline Time F(std::int64_t n)
{
    return timeOfFrame(n, kRate);
}

inline std::int64_t frames(Time t)
{
    return frameAt(t, kRate);
}

inline MediaItem testMedia(double seconds = 10.0, bool video = true, bool audio = true, std::string name = "clip.mp4")
{
    MediaItem m;
    m.id = newId();
    m.name = name;
    m.path = "C:/media/" + name;
    m.duration = timeFromSeconds(seconds);
    m.hasVideo = video;
    m.width = video ? 1920 : 0;
    m.height = video ? 1080 : 0;
    m.frameRate = kRate;
    m.hasAudio = audio;
    m.sampleRate = audio ? 48000 : 0;
    m.channels = audio ? 2 : 0;
    return m;
}

// A project with one 10 s media placed at 0 on V1/A1 (300 frames).
struct Fixture {
    Project project = makeEmptyProject();
    MediaItem media = testMedia();

    Fixture()
    {
        project.timeline().rate = kRate;
        project.media.items.push_back(media);
        edit::placeMedia(project.timeline(), media);
    }
    Timeline& tl() { return project.timeline(); }
    Track& v1() { return tl().tracks[0]; }
    Track& a1() { return tl().tracks[1]; }
    const MediaPool& pool() const { return project.media; }
};

// Every link group present on V1 has an identical partner on A1 and vice versa.
inline bool linkedTracksAligned(const Timeline& tl)
{
    const auto v = tl.tracks[0].clips();
    const auto a = tl.tracks[1].clips();
    if (v.size() != a.size())
        return false;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i].start != a[i].start || !(v[i].clip->sourceRange == a[i].clip->sourceRange)
            || v[i].clip->linkGroup != a[i].clip->linkGroup)
            return false;
    }
    return true;
}

// The strict form of V/A alignment for any number of tracks: the members of every link group sit on
// tracks of the same index (V2 with A2), with the same position, source range and speed. Empty
// string = aligned.
inline std::string strictAlignment(const Timeline& tl)
{
    struct Member {
        std::string track;
        int index;
        Time start;
        TimeRange source;
        Retime speed;
    };
    std::map<Id, std::vector<Member>> groups;
    for (const Track& t : tl.tracks) {
        for (const PlacedItem& p : t.clips()) {
            if (p.clip->linkGroup.empty())
                continue;
            const Clip* c = std::get_if<Clip>(p.item);
            groups[p.clip->linkGroup].push_back(
                {t.name, edit::kindIndex(tl, t.id), p.start, p.clip->sourceRange, c ? c->retime : Retime{}});
        }
    }
    std::ostringstream err;
    for (const auto& [group, members] : groups) {
        for (const Member& m : members) {
            const Member& f = members.front();
            if (m.index != f.index || m.start != f.start || !(m.source == f.source) || !(m.speed == f.speed))
                err << "group " << group << ": " << m.track << " differs from " << f.track << "\n";
        }
    }
    return err.str();
}

// Four tracks and four media, for the phase 2 edits (frames at 30 fps):
//   V1/A1  take A [0,100) | take A [100,300) | take B [300,420)   linked picture and sound
//   V2     title (picture only) [150,240)
//   A2     music (sound only) [0,600)
// Ids come from a ScopedIdSequence, so golden states are reproducible.
struct MultiFixture {
    ScopedIdSequence ids{"id"};
    Project project = makeEmptyProject();
    MediaItem a = testMedia(10.0, true, true, "a.mp4");
    MediaItem b = testMedia(4.0, true, true, "b.mp4");
    MediaItem title = testMedia(3.0, true, false, "title.mov");
    MediaItem music = testMedia(20.0, false, true, "music.wav");

    MultiFixture()
    {
        Timeline& t = tl();
        t.rate = kRate;
        for (const MediaItem* m : {&a, &b, &title, &music})
            project.media.items.push_back(*m);
        edit::addTrack(t, TrackKind::Video);
        edit::addTrack(t, TrackKind::Audio);
        edit::placeMedia(t, a);
        edit::splitAt(t, F(100));
        edit::placeMedia(t, b);
        edit::EditPoints p = points(title, 150);
        p.audioTrack.clear();
        p.videoTrack = track("V2").id;
        edit::overwriteEdit(t, pool(), p);
        p = points(music, 0);
        p.videoTrack.clear();
        p.audioTrack = track("A2").id;
        edit::overwriteEdit(t, pool(), p);
    }

    Timeline& tl() { return project.timeline(); }
    const MediaPool& pool() const { return project.media; }
    Track& track(const std::string& name)
    {
        for (Track& t : tl().tracks) {
            if (t.name == name)
                return t;
        }
        throw std::logic_error("no track " + name);
    }
    // The clip on `trackName` under frame n.
    Id clipAt(const std::string& trackName, std::int64_t n)
    {
        auto placed = track(trackName).clipAt(F(n));
        return placed ? placed->clip->id : Id{};
    }
    // An edit of `media` at frame `at` onto V1/A1.
    edit::EditPoints points(const MediaItem& media, std::int64_t at)
    {
        edit::EditPoints p;
        p.mediaId = media.id;
        p.playhead = F(at);
        p.videoTrack = track("V1").id;
        p.audioTrack = track("A1").id;
        return p;
    }
};

// "[start,end) name src S" per clip in frames, one line per track: readable timeline summaries for
// failure messages and golden files.
inline std::vector<std::string> describe(const Timeline& tl, const MediaPool& pool)
{
    std::vector<std::string> out;
    for (const Track& t : tl.tracks) {
        std::ostringstream line;
        line << t.name;
        if (t.locked)
            line << " (locked)";
        if (!t.syncLock)
            line << " (sync lock off)";
        line << ":";
        Time pos;
        bool first = true;
        for (const Item& item : t.items) {
            const Time d = durationOf(item);
            line << (first ? " " : " | ");
            first = false;
            if (const ClipBase* c = clipBase(item)) {
                std::string what = c->name;
                if (const Clip* clip = std::get_if<Clip>(&item)) {
                    if (const MediaItem* m = pool.find(clip->mediaId))
                        what = m->name;
                } else if (std::holds_alternative<CompoundClip>(item)) {
                    what = "compound " + c->name;
                }
                line << "[" << frames(pos) << "," << frames(pos + d) << ") " << what << " src " << frames(c->sourceRange.start);
                if (const Clip* clip = std::get_if<Clip>(&item); clip && !clip->retime.isIdentity())
                    line << " x" << clip->retime.speedNum << "/" << clip->retime.speedDen;
                if (!c->linkGroup.empty())
                    line << " link " << c->linkGroup;
            } else if (std::holds_alternative<Gap>(item)) {
                line << "gap " << frames(d);
            } else {
                line << "transition";
            }
            pos += d;
        }
        out.push_back(line.str());
    }
    for (const Marker& m : tl.markers)
        out.push_back("marker " + m.name + " at " + std::to_string(frames(m.start)));
    return out;
}

inline std::string describeText(const Timeline& tl, const MediaPool& pool)
{
    std::string s;
    for (const std::string& line : describe(tl, pool))
        s += line + "\n";
    return s;
}

} // namespace ve::test
