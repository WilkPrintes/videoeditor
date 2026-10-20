// ve-cli: headless access to the core and the engine, used to verify decoding, editing, export and
// the playback clock without the UI.
//
//   ve-cli probe <media>
//   ve-cli encoders
//   ve-cli make-test-clip <out.mp4> [seconds] [width] [height] [fps]
//   ve-cli selftest <media> <out.mp4>          import, cut, move, drag gesture, trim, undo, save, export
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

bool exportWithProgress(const TimelineSnapshot& snap, const ExportSettings& settings)
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
