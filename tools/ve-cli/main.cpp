// ve-cli: headless access to the core and the engine, used to verify decoding, editing, export and
// the playback clock without the UI.
//
//   ve-cli probe <media>
//   ve-cli encoders
//   ve-cli make-test-clip <out.mp4> [seconds] [width] [height] [fps]
//   ve-cli selftest <media> <out.mp4>          import, cut, move, drag gesture, trim, undo, save, export,
//                                              then the phase 2 edits and the compound render hash
//   ve-cli render <project.veproj> <out.mp4> [encoder]
//   ve-cli play <media> [seconds]              plays through the audio device, reports A/V sync

#include "core/Commands.h"
#include "core/Edit.h"
#include "core/ProjectFile.h"
#include "core/Snapshot.h"
#include "engine/Exporter.h"
#include "engine/PlaybackEngine.h"
#include "media/MediaProbe.h"
#include "media/TestPattern.h"

#include <QCoreApplication>
#include <QFile>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace ve;

namespace {

int usage()
{
    std::puts("usage:\n"
              "  ve-cli probe <media>\n"
              "  ve-cli encoders\n"
              "  ve-cli make-test-clip <out.mp4> [seconds] [width] [height] [fps]\n"
              "  ve-cli selftest <media> <out.mp4>\n"
              "  ve-cli render <project.veproj> <out.mp4> [encoder]\n"
              "  ve-cli play <media> [seconds]");
    return 2;
}

void printMedia(const MediaItem& m)
{
    std::printf("%s\n  duration %.3f s (%lld flicks)\n", m.name.c_str(), m.duration.toSeconds(),
                static_cast<long long>(m.duration.flicks));
    if (m.hasVideo)
        std::printf("  video %s %dx%d @ %d/%d (%.3f fps)\n", m.videoCodec.c_str(), m.width, m.height,
                    m.frameRate.num, m.frameRate.den, m.frameRate.toDouble());
    if (m.hasAudio)
        std::printf("  audio %s %d Hz, %d ch\n", m.audioCodec.c_str(), m.sampleRate, m.channels);
}

std::optional<MediaItem> probeOrFail(const std::string& path)
{
    std::string error;
    auto media = probeMedia(path, &error);
    if (!media)
        std::fprintf(stderr, "probe failed: %s\n", error.c_str());
    return media;
}

void conform(Timeline& tl, const MediaItem& media)
{
    if (media.hasVideo) {
        tl.rate = media.frameRate;
        tl.width = media.width & ~1;
        tl.height = media.height & ~1;
    }
}

void dumpTimeline(const Timeline& tl)
{
    for (const Track& t : tl.tracks) {
        std::printf("  %s:", t.name.c_str());
        for (const Item& item : t.items) {
            if (const ClipBase* c = clipBase(item))
                std::printf(" [clip in=%lld dur=%lld]", static_cast<long long>(frameAt(c->sourceRange.start, tl.rate)),
                            static_cast<long long>(frameAt(c->sourceRange.duration, tl.rate)));
            else if (const Gap* g = std::get_if<Gap>(&item))
                std::printf(" (gap %lld)", static_cast<long long>(frameAt(g->duration, tl.rate)));
            else
                std::printf(" <transition>");
        }
        std::printf("   (frames)\n");
    }
}

bool exportWithProgress(const TimelineSnapshot& snap, const ExportSettings& settings, ExportResult* result = nullptr)
{
    int lastTenth = -1;
    const auto t0 = std::chrono::steady_clock::now();
    const ExportResult res = exportTimeline(snap, settings, [&](double p) {
        const int tenth = int(p * 10);
        if (tenth != lastTenth) {
            lastTenth = tenth;
            std::printf("  export %3d%%\n", tenth * 10);
            std::fflush(stdout);
        }
        return true;
    });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (result)
        *result = res;
    if (!res.ok) {
        std::fprintf(stderr, "export failed: %s\n", res.error.c_str());
        return false;
    }
    std::printf("  exported %lld frames with %s in %.2f s\n", static_cast<long long>(res.framesWritten),
                res.videoEncoder.c_str(), secs);
    return true;
}

int cmdSelftest(const std::string& mediaPath, const std::string& outPath)
{
    auto media = probeOrFail(mediaPath);
    if (!media)
        return 1;
    printMedia(*media);
    // The edits below assume this much media (shorter clips leave gaps where the checks expect clips).
    if (media->duration < timeFromSeconds(6.0)) {
        std::fprintf(stderr, "selftest needs media of at least 6 s (try: ve-cli make-test-clip clip.mp4 8)\n");
        return 1;
    }

    Project project = makeEmptyProject();
    CommandStack stack(project);
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        std::printf("  %-52s %s\n", what, cond ? "ok" : "FAILED");
        ok = ok && cond;
    };
    auto invariantsHold = [&] {
        const std::string problems = edit::checkInvariants(project.timeline(), project.media);
        if (!problems.empty())
            std::printf("%s", problems.c_str());
        return problems.empty();
    };

    // Import = add to pool + conform the timeline + place on V1/A1, as one undo step.
    stack.beginTransaction("Import");
    stack.push(std::make_unique<AddMediaCommand>(*media));
    const MediaItem m = *media;
    stack.edit("Place", [m](Timeline& tl, const MediaPool&) {
        conform(tl, m);
        return !edit::placeMedia(tl, m).empty();
    });
    check(stack.commit(), "import places the clip (one transaction)");
    const Timeline& tl = project.timeline();
    const Rate rate = tl.rate;
    const Time length = tl.duration();
    const Time frame = timeOfFrame(1, rate);
    std::printf("  timeline: %lld frames @ %.3f fps\n", static_cast<long long>(frameAt(length, rate)), rate.toDouble());

    const Time cut = floorToFrame(Time(length.flicks / 3), rate);
    check(stack.edit("Split", [cut](Timeline& t, const MediaPool&) { return edit::splitAt(t, cut); }), "blade at 1/3");

    // A drag gesture: many intermediate positions (one passing over the first clip and back) merge
    // into a single undo step, and the final state only reflects the last position.
    const Id second = project.timeline().tracks[0].clipAt(cut)->clip->id;
    const Time oneSecond = timeOfFrame(std::llround(rate.toDouble()), rate);
    const Timeline beforeDrag = project.timeline();
    stack.beginTransaction("Move");
    for (Time delta : {Time(0) - timeOfFrame(30, rate), Time(0) - timeOfFrame(10, rate), timeOfFrame(5, rate), oneSecond}) {
        stack.edit("Move", [second, delta](Timeline& t, const MediaPool&) { return edit::moveClips(t, {second}, delta); },
                   "drag");
    }
    check(stack.commit(), "drag gesture commits");
    check(project.timeline().duration() == length + oneSecond, "only the last drag position applies (+1 s)");
    check(project.timeline().tracks[0].clips().size() == 2, "the dragged-over clip is intact");
    stack.undo();
    check(project.timeline() == beforeDrag, "one undo reverts the whole gesture");
    stack.redo();

    const Timeline beforeTrim = project.timeline();
    const Id first = project.timeline().tracks[0].clips().front().clip->id;
    check(stack.edit("Trim", [first, cut, frame](Timeline& t, const MediaPool& pool) {
              return edit::trimClip(t, pool, first, edit::Edge::Tail, cut - Time(frame.flicks * 10));
          }),
          "trim first part -10 frames");
    stack.undo();
    check(project.timeline() == beforeTrim, "undo restores the timeline");
    stack.redo();
    check(project.timeline() != beforeTrim, "redo re-applies the trim");
    check(invariantsHold(), "invariants (frame grid, media handles, no gaps left over)");
    dumpTimeline(project.timeline());

    // Linked clips must still line up on V1 and A1.
    if (media->hasVideo && media->hasAudio) {
        const auto v = project.timeline().tracks[0].clips();
        const auto a = project.timeline().tracks[1].clips();
        bool aligned = v.size() == a.size();
        for (std::size_t i = 0; aligned && i < v.size(); ++i)
            aligned = v[i].start == a[i].start && v[i].clip->sourceRange == a[i].clip->sourceRange
                && v[i].clip->linkGroup == a[i].clip->linkGroup;
        check(aligned, "V1/A1 clips stay aligned and linked");
    }

    // Save and reload.
    const std::string projectPath = outPath + ".veproj";
    std::string error;
    check(saveProject(project, projectPath, &error), "save project");
    auto loaded = loadProject(projectPath, &error);
    check(loaded && loaded->timelines == project.timelines, "reload gives the same timeline");

    // Export and verify the result.
    auto snap = TimelineSnapshot::fromProject(project);
    ExportSettings settings;
    settings.outputPath = outPath;
    const bool exported = exportWithProgress(*snap, settings);
    check(exported, "export H.264/AAC MP4");
    if (exported) {
        auto out = probeOrFail(outPath);
        check(out.has_value(), "output is readable");
        if (out) {
            printMedia(*out);
            const double drift = std::abs((out->duration - project.timeline().duration()).toSeconds());
            check(drift < 0.1, "output duration matches timeline");
            check(out->hasVideo && out->videoCodec == "h264", "output video is H.264");
            check(!media->hasAudio || (out->hasAudio && out->audioCodec == "aac"), "output audio is AAC");
        }
    }

    // Phase 2 (spec §6.1-6.3) on real media: every edit keeps the invariants and picture/sound in sync,
    // undo brings everything back, and a compound clip renders exactly the frames and samples of what
    // it replaced (hashed before encoding, so the encoder plays no part).
    std::printf("phase 2:\n");
    const Project beforePhase2 = project;
    int phase2Steps = 0;
    auto syncHolds = [&] {
        const std::string problems = edit::checkSync(project.timeline());
        if (!problems.empty())
            std::printf("%s", problems.c_str());
        return problems.empty();
    };
    auto step = [&](const char* what, TimelineEditCommand::Operation op) {
        const bool applied = stack.edit(what, std::move(op));
        phase2Steps += applied ? 1 : 0;
        check(applied && invariantsHold() && syncHolds(), what);
    };
    auto trackId = [&](TrackKind kind, int index) {
        const Track* t = edit::trackOfKind(project.timeline(), kind, index);
        return t ? t->id : Id{};
    };
    auto clipOn = [&](TrackKind kind, int index, Time at) {
        const Track* t = edit::trackOfKind(project.timeline(), kind, index);
        const auto placed = t ? t->clipAt(at) : std::nullopt;
        return placed ? placed->clip->id : Id{};
    };
    const Time sec = oneSecond;
    step("add V2 and A2", [](Timeline& t, const MediaPool&) {
        return !edit::addTrack(t, TrackKind::Video).empty() && !edit::addTrack(t, TrackKind::Audio).empty();
    });
    edit::EditPoints points;
    points.mediaId = media->id;
    points.videoTrack = trackId(TrackKind::Video, 0);
    points.audioTrack = trackId(TrackKind::Audio, 0);
    points.playhead = sec;
    points.sourceIn = Time(0);
    points.sourceOut = sec + sec;
    step("insert 2 s at 1 s (all tracks sync-locked)", [points](Timeline& t, const MediaPool& pool) {
        return !edit::insertEdit(t, pool, points).empty();
    });
    check(project.timeline().duration() == beforePhase2.timeline().duration() + sec + sec, "insert made the timeline 2 s longer");
    const Id head = clipOn(TrackKind::Video, 0, Time(0));
    const Id inserted = clipOn(TrackKind::Video, 0, sec);
    const Id headSound = clipOn(TrackKind::Audio, 0, Time(0));
    step("ripple trim the first clip by -10 frames", [head, frame](Timeline& t, const MediaPool& pool) {
        return edit::rippleTrim(t, pool, head, edit::Edge::Tail, Time(0) - Time(frame.flicks * 10));
    });
    step("roll its cut by +5 frames", [head, frame](Timeline& t, const MediaPool& pool) {
        return edit::rollTrim(t, pool, head, edit::Edge::Tail, Time(frame.flicks * 5));
    });
    step("L cut: roll only the sound by +5 frames", [headSound, frame](Timeline& t, const MediaPool& pool) {
        return edit::rollTrim(t, pool, headSound, edit::Edge::Tail, Time(frame.flicks * 5), false);
    });
    step("slip the inserted clip by +10 frames", [inserted, frame](Timeline& t, const MediaPool& pool) {
        return edit::slipClip(t, pool, inserted, Time(frame.flicks * 10));
    });
    step("slide the inserted clip by +5 frames", [inserted, frame](Timeline& t, const MediaPool& pool) {
        return edit::slideClip(t, pool, inserted, Time(frame.flicks * 5));
    });
    edit::EditPoints top = points;
    top.playhead = Time(0);
    top.sourceOut = sec;
    step("place 1 s on top (lands on V2/A2)", [top](Timeline& t, const MediaPool& pool) {
        return !edit::placeOnTopEdit(t, pool, top).empty();
    });
    check(!clipOn(TrackKind::Video, 1, Time(0)).empty(), "the clip on top is on V2");
    edit::EditPoints fit = top;
    fit.videoTrack = trackId(TrackKind::Video, 1);
    fit.audioTrack = trackId(TrackKind::Audio, 1);
    fit.recordIn = Time(sec.flicks * 4);
    fit.recordOut = Time(sec.flicks * 6);
    step("fit 1 s into 2 s on V2/A2 (speed 1/2)", [fit](Timeline& t, const MediaPool& pool) {
        return !edit::fitToFillEdit(t, pool, fit).empty();
    });
    const Id v2 = trackId(TrackKind::Video, 1);
    const Id a2 = trackId(TrackKind::Audio, 1);
    step("sync lock off on V2 and A2", [v2, a2](Timeline& t, const MediaPool&) {
        return edit::setTrackSyncLock(t, v2, false) && edit::setTrackSyncLock(t, a2, false);
    });
    check(!stack.edit("Ripple delete", [inserted](Timeline& t, const MediaPool&) { return edit::rippleDelete(t, {inserted}); }),
          "ripple delete refused where it would cut the L cut");
    const auto insertedNow = edit::trackOfKind(project.timeline(), TrackKind::Video, 0)->findClip(inserted);
    const Id next = insertedNow ? clipOn(TrackKind::Video, 0, insertedNow->end()) : Id{};
    step("ripple delete the clip after it (V2/A2 stay)", [next](Timeline& t, const MediaPool&) {
        return edit::rippleDelete(t, {next});
    });
    dumpTimeline(project.timeline());

    ExportSettings hashed = settings;
    hashed.renderHash = true;
    hashed.outputPath = outPath + ".phase2.mp4";
    ExportResult plain;
    const bool plainOk = exportWithProgress(*TimelineSnapshot::fromProject(project), hashed, &plain);
    check(plainOk, "export the edited multitrack timeline");
    std::vector<Id> everything;
    for (const Track& t : project.timeline().tracks) {
        for (const PlacedItem& p : t.clips())
            everything.push_back(p.clip->id);
    }
    const auto beforeCompose = project.timelines;
    const bool composed = stack.editProject("Compose", [everything](Project& p) {
        return edit::composeClips(p, everything, "Everything").has_value();
    });
    phase2Steps += composed ? 1 : 0;
    check(composed && project.timelines.size() == 2, "compound clip of every clip");
    hashed.outputPath = outPath + ".compound.mp4";
    ExportResult nested;
    const bool nestedOk = exportWithProgress(*TimelineSnapshot::fromProject(project), hashed, &nested);
    check(nestedOk, "export the compound clip");
    std::printf("  render hashes  plain %016llx/%016llx  compound %016llx/%016llx\n",
                static_cast<unsigned long long>(plain.videoHash), static_cast<unsigned long long>(plain.audioHash),
                static_cast<unsigned long long>(nested.videoHash), static_cast<unsigned long long>(nested.audioHash));
    check(plainOk && nestedOk && plain.framesWritten == nested.framesWritten && plain.videoHash == nested.videoHash,
          "compound renders the same frames (hash)");
    check(plainOk && nestedOk && plain.audioHash == nested.audioHash, "compound renders the same samples (hash)");
    const Id compound = clipOn(TrackKind::Video, 0, Time(0));
    const bool decomposed = stack.editProject("Decompose", [compound](Project& p) { return edit::decomposeClip(p, compound); });
    phase2Steps += decomposed ? 1 : 0;
    check(decomposed && project.timelines == beforeCompose, "decompose gives back the same timeline");
    for (int i = 0; i < phase2Steps; ++i)
        stack.undo();
    check(project.timelines == beforePhase2.timelines, "undoing phase 2 restores the timeline");
    QFile::remove(QString::fromStdString(outPath + ".phase2.mp4"));
    QFile::remove(QString::fromStdString(outPath + ".compound.mp4"));

    std::printf("%s\n", ok ? "SELFTEST PASSED" : "SELFTEST FAILED");
    return ok ? 0 : 1;
}

int cmdRender(const std::string& projectPath, const std::string& outPath, const std::string& encoder)
{
    std::string error;
    auto project = loadProject(projectPath, &error);
    if (!project) {
        std::fprintf(stderr, "cannot load project: %s\n", error.c_str());
        return 1;
    }
    ExportSettings settings;
    settings.outputPath = outPath;
    if (!encoder.empty())
        settings.videoEncoder = encoder;
    return exportWithProgress(*TimelineSnapshot::fromProject(*project), settings) ? 0 : 1;
}

int cmdPlay(const std::string& mediaPath, double seconds)
{
    auto media = probeOrFail(mediaPath);
    if (!media)
        return 1;
    printMedia(*media);
    Project project = makeEmptyProject();
    conform(project.timeline(), *media);
    project.media.items.push_back(*media);
    edit::placeMedia(project.timeline(), *media);
    auto snap = TimelineSnapshot::fromProject(project);
    const Rate rate = snap->timeline.rate;

    PlaybackEngine engine;
    engine.setTimeline(snap);
    const auto t0 = std::chrono::steady_clock::now();
    engine.play();
    const auto t1 = std::chrono::steady_clock::now();

    // Poll like the UI does and compare the picture with the clock.
    std::int64_t maxSkew = 0;
    std::int64_t samples = 0;
    std::int64_t blackWhilePlaying = 0;
    while (std::chrono::steady_clock::now() - t1 < std::chrono::duration<double>(seconds)) {
        bool changed = false;
        VideoFramePtr picture = engine.currentVideoFrame(&changed);
        const Time pos = engine.position();
        if (changed) {
            ++samples;
            const auto expected = resolveVideo(*snap, pos);
            if (picture && expected) {
                const double skew = (picture->sourceTime - expected->sourceTime).toSeconds() * rate.toDouble();
                maxSkew = std::max<std::int64_t>(maxSkew, std::llround(std::abs(skew)));
            } else if (!picture && expected) {
                ++blackWhilePlaying;
            }
        }
        if (engine.reachedEnd())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    const Time endPos = engine.position();
    engine.pause();

    const auto st = engine.stats();
    const double clockSeconds = endPos.toSeconds();
    std::printf("  start latency        %.1f ms\n", std::chrono::duration<double, std::milli>(t1 - t0).count());
    std::printf("  clock                %s (%s)\n", st.audioClock ? "audio device" : "system", st.audioDevice.c_str());
    std::printf("  played               %.3f s of timeline in %.3f s wall (ratio %.4f)\n", clockSeconds, wall,
                wall > 0 ? clockSeconds / wall : 0.0);
    std::printf("  frames shown/dropped %lld / %lld\n", static_cast<long long>(st.framesShown),
                static_cast<long long>(st.framesDropped));
    std::printf("  audio underrun       %lld frames\n", static_cast<long long>(st.audioUnderrunFrames));
    std::printf("  max picture skew     %lld frame(s) from the clock (%lld samples)\n", static_cast<long long>(maxSkew),
                static_cast<long long>(samples));
    std::printf("  black while playing  %lld\n", static_cast<long long>(blackWhilePlaying));
    const bool ok = maxSkew <= 1 && blackWhilePlaying == 0 && samples > 0;
    std::printf("%s\n", ok ? "PLAYBACK OK" : "PLAYBACK CHECK FAILED");
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv); // gives correctly decoded (UTF-8) arguments on Windows
    const QStringList args = app.arguments();
    auto arg = [&](int i) { return i < args.size() ? args[i].toStdString() : std::string(); };
    const std::string cmd = arg(1);

    if (cmd == "probe" && args.size() >= 3) {
        auto media = probeOrFail(arg(2));
        if (!media)
            return 1;
        printMedia(*media);
        return 0;
    }
    if (cmd == "encoders") {
        std::puts("H.264 encoders in this FFmpeg build (usable ones marked *):");
        const auto usable = availableH264Encoders(true);
        for (const std::string& name : availableH264Encoders(false)) {
            const bool ok = std::find(usable.begin(), usable.end(), name) != usable.end();
            std::printf("  %s %s\n", ok ? "*" : " ", name.c_str());
        }
        return 0;
    }
    if (cmd == "make-test-clip" && args.size() >= 3) {
        const double seconds = args.size() >= 4 ? args[3].toDouble() : 10.0;
        const int width = args.size() >= 5 ? args[4].toInt() : 1920;
        const int height = args.size() >= 6 ? args[5].toInt() : 1080;
        const Rate rate = args.size() >= 7 ? rateFromDouble(args[6].toDouble()) : Rate{30, 1};
        std::string error;
        if (!writeTestClip(arg(2), timeFromSeconds(seconds), rate, width, height, &error)) {
            std::fprintf(stderr, "failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("wrote %s\n", arg(2).c_str());
        return 0;
    }
    if (cmd == "selftest" && args.size() >= 4)
        return cmdSelftest(arg(2), arg(3));
    if (cmd == "render" && args.size() >= 4)
        return cmdRender(arg(2), arg(3), arg(4));
    if (cmd == "play" && args.size() >= 3)
        return cmdPlay(arg(2), args.size() >= 4 ? args[3].toDouble() : 5.0);
    return usage();
}
