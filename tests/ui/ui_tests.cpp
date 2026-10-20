// GUI tests: drive the real MainWindow with synthetic mouse and key events, the paths a user takes,
// and check the model after each step.
// - editAndPlayThroughTheUi (phase 1): import -> blade -> drag clip -> trim edge -> cancelled drag ->
//   undo -> play.
// - multitrackTrimsAndMarkers (phase 2): add tracks -> ripple with the trim tool -> cancelled ripple ->
//   roll (Ctrl) -> L cut (Ctrl+Alt) -> slip -> move to V2 -> marker snapping -> lock from the track
//   header -> insert from the media pool (F9), each drag one undo step.
// - panelsLayoutsAndSourceViewer (phase 3): docked panels -> layout preset save/reset/load/delete ->
//   source viewer from the pool (double-click) as the active viewer, its In/Out and transport ->
//   overwrite and append of the source range -> timeline tabs, each timeline with its own playhead.
// - inspectorIndexAndContextMenus (phase 3): rename, colour and disable from the inspector -> colour,
//   flag and unlink from the clip's context menu -> ripple delete of a gap -> track and marker menus
//   -> edit index row -> linked selection off -> compound clip opened with a double-click.
// - updatesFromAFeed (0.0.1): versions, checksums and a GitHub-shaped release feed on local files ->
//   check (newer / up to date / missing feed) -> download verified by SHA-256 -> tampered checksum.
//
// Uses VE_TEST_MEDIA if set, otherwise generates a clapper clip with the engine's test pattern.
// VE_UI_SCREENSHOT=<file.png> saves the window after the edits (the phase 2 test adds "_phase2").

#include "core/Edit.h"
#include "media/TestPattern.h"
#include "ui/MainWindow.h"
#include "ui/MediaPoolWidget.h"
#include "ui/SourceViewerPanel.h"
#include "ui/TimelineWidget.h"
#include "ui/Updater.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDockWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QStandardPaths>
#include <QTabBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest>

using ve::Time;

class UiTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void editAndPlayThroughTheUi();
    void multitrackTrimsAndMarkers();
    void panelsLayoutsAndSourceViewer();
    void inspectorIndexAndContextMenus();
    void updatesFromAFeed();

private:
    QTemporaryDir m_dir;
    QString m_media;
};

void UiTests::initTestCase()
{
    // Layout presets go to a test location, never to the user's settings.
    QStandardPaths::setTestModeEnabled(true);
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/layouts.ini"));
    m_media = qEnvironmentVariable("VE_TEST_MEDIA");
    if (m_media.isEmpty()) {
        QVERIFY(m_dir.isValid());
        m_media = m_dir.filePath(QStringLiteral("pattern.mp4"));
        std::string error;
        QVERIFY2(ve::writeTestClip(m_media.toStdString(), Time::fromSeconds(8), {30, 1}, 1280, 720, &error),
                 error.c_str());
    }
}

void UiTests::editAndPlayThroughTheUi()
{
    MainWindow w;
    w.resize(1400, 900);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));

    auto clipsOn = [&](std::size_t track) { return w.project().timeline().tracks.at(track).clips(); };
    auto action = [&](const char* name) {
        auto* a = w.findChild<QAction*>(QLatin1String(name));
        Q_ASSERT(a);
        return a;
    };
    auto* timeline = w.findChild<TimelineWidget*>();
    QVERIFY(timeline);
    const ve::Rate rate{30, 1};
    auto frame = [&](std::int64_t n) { return ve::timeOfFrame(n, rate); };

    // Import: one linked clip on V1 and A1, timeline conformed to 30 fps.
    w.importFiles({m_media});
    QVERIFY(w.project().timeline().hasClips());
    QCOMPARE(w.project().timeline().rate, rate);
    QCOMPARE(clipsOn(0).size(), std::size_t(1));
    QCOMPARE(clipsOn(1).size(), std::size_t(1));
    const Time length = w.project().timeline().duration();
    QCOMPARE(length, frame(240));

    // Blade at the playhead (Ctrl+B action).
    const Time cut = frame(120);
    w.seekTo(cut);
    QTRY_COMPARE(w.playheadPosition(), cut);
    action("actionSplit")->trigger();
    QCOMPARE(clipsOn(0).size(), std::size_t(2));
    QCOMPARE(clipsOn(1).size(), std::size_t(2));
    QCOMPARE(clipsOn(0)[1].start, cut);

    // Drag the second clip to the right: the model follows the mouse live, inside one transaction.
    QTest::qWait(50);
    const ve::Id second = clipsOn(0)[1].clip->id;
    QRect r = timeline->clipRect(second);
    QVERIFY(r.width() > 40);
    QPoint from = r.center();
    QTest::mousePress(timeline, Qt::LeftButton, {}, from);
    QTest::mouseMove(timeline, from + QPoint(30, 0));
    QVERIFY2(clipsOn(0)[1].start > cut, "the clip moves while dragging");
    QTest::mouseMove(timeline, from + QPoint(90, 0));
    QTest::mouseRelease(timeline, Qt::LeftButton, {}, from + QPoint(90, 0));
    auto v = clipsOn(0);
    QCOMPARE(v.size(), std::size_t(2));
    QVERIFY(v[1].start > cut);
    QCOMPARE(clipsOn(1)[1].start, v[1].start); // linked audio followed
    const Time movedTo = v[1].start;

    // Trim the first clip's tail by dragging its right edge to the left.
    const ve::Id first = v[0].clip->id;
    r = timeline->clipRect(first);
    from = QPoint(r.right() - 2, r.center().y());
    QTest::mousePress(timeline, Qt::LeftButton, {}, from);
    QTest::mouseMove(timeline, from - QPoint(20, 0));
    QTest::mouseMove(timeline, from - QPoint(50, 0));
    QTest::mouseRelease(timeline, Qt::LeftButton, {}, from - QPoint(50, 0));
    v = clipsOn(0);
    QVERIFY2(v[0].clip->sourceRange.duration < cut, "the first clip should be shorter");
    QCOMPARE(clipsOn(1)[0].clip->sourceRange.duration, v[0].clip->sourceRange.duration);
    const ve::Timeline afterTrim = w.project().timeline();

    // A drag cancelled with Escape leaves no trace.
    r = timeline->clipRect(second);
    from = r.center();
    QTest::mousePress(timeline, Qt::LeftButton, {}, from);
    QTest::mouseMove(timeline, from - QPoint(200, 0));
    QVERIFY(w.project().timeline() != afterTrim);
    QTest::keyClick(timeline, Qt::Key_Escape);
    QTest::mouseRelease(timeline, Qt::LeftButton, {}, from - QPoint(200, 0));
    QVERIFY(w.project().timeline() == afterTrim);

    if (const QString shot = qEnvironmentVariable("VE_UI_SCREENSHOT"); !shot.isEmpty()) {
        w.seekTo(movedTo);
        QTest::qWait(400);
        w.grab().save(shot);
    }

    // Each gesture is exactly one undo step.
    action("actionUndo")->trigger();
    QCOMPARE(clipsOn(0)[0].clip->sourceRange.duration, cut);
    action("actionUndo")->trigger();
    QCOMPARE(clipsOn(0)[1].start, cut);
    action("actionRedo")->trigger();
    QCOMPARE(clipsOn(0)[1].start, movedTo);

    // Playback advances the playhead on the audio clock (or the system clock without a device).
    w.seekTo(Time(0));
    QTRY_COMPARE(w.playheadPosition(), Time(0));
    action("actionPlayPause")->trigger();
    QTRY_VERIFY(w.isPlaying());
    QTest::qWait(1000);
    const std::int64_t played = ve::frameAt(w.playheadPosition(), rate);
    action("actionPlayPause")->trigger();
    QTRY_VERIFY(!w.isPlaying());
    QVERIFY2(played >= 15 && played <= 40, qPrintable(QStringLiteral("played %1 frames in ~1 s").arg(played)));
}

void UiTests::multitrackTrimsAndMarkers()
{
    MainWindow w;
    w.resize(1400, 900);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto* timeline = w.findChild<TimelineWidget*>();
    QVERIFY(timeline);
    const ve::Rate rate{30, 1};
    auto frame = [&](std::int64_t n) { return ve::timeOfFrame(n, rate); };
    auto tl = [&]() -> const ve::Timeline& { return w.project().timeline(); };
    auto track = [&](const char* name) -> const ve::Track& {
        const ve::Track* found = nullptr;
        for (const ve::Track& t : tl().tracks) {
            if (t.name == name)
                found = &t;
        }
        if (!found)
            qFatal("no track %s", name);
        return *found;
    };
    auto clips = [&](const char* name) { return track(name).clips(); };
    auto action = [&](const char* name) {
        auto* a = w.findChild<QAction*>(QLatin1String(name));
        Q_ASSERT(a);
        return a;
    };
    auto undoText = [&] { return action("actionUndo")->text(); };
    auto drag = [&](QPoint from, QPoint to, Qt::KeyboardModifiers modifiers = {}) {
        QTest::mousePress(timeline, Qt::LeftButton, modifiers, from);
        QTest::mouseMove(timeline, (from + to) / 2);
        QTest::mouseMove(timeline, to);
        QTest::mouseRelease(timeline, Qt::LeftButton, modifiers, to);
    };
    auto tailOf = [&](const ve::Id& clip) {
        const QRect r = timeline->clipRect(clip);
        return QPoint(r.right() - 2, r.center().y());
    };

    // Multitrack: one more video and one more audio track, undoable.
    w.importFiles({m_media});
    QCOMPARE(tl().duration(), frame(240));
    action("actionAddVideoTrack")->trigger();
    action("actionAddAudioTrack")->trigger();
    QCOMPARE(tl().tracks.size(), std::size_t(4));
    QVERIFY(undoText().contains(QStringLiteral("Add Audio Track")));
    action("actionUndo")->trigger();
    QCOMPARE(tl().tracks.size(), std::size_t(3));
    action("actionRedo")->trigger();
    QCOMPARE(track("A2").kind, ve::TrackKind::Audio);

    w.seekTo(frame(120));
    QTRY_COMPARE(w.playheadPosition(), frame(120));
    action("actionSplit")->trigger();
    QCOMPARE(clips("V1").size(), std::size_t(2));
    QTest::qWait(50);

    // Trim tool, edge: ripple. The second clip follows the shorter first one; no gap is left.
    action("actionTrimTool")->trigger();
    QCOMPARE(timeline->tool(), TimelineWidget::Tool::Trim);
    QPoint from = tailOf(clips("V1")[0].clip->id);
    drag(from, from - QPoint(40, 0));
    auto v = clips("V1");
    QCOMPARE(v.size(), std::size_t(2));
    const Time cut = v[0].end();
    QVERIFY(cut < frame(120));
    QCOMPARE(v[1].start, cut);
    QCOMPARE(clips("A1")[1].start, cut);
    QCOMPARE(tl().duration(), frame(240) - (frame(120) - cut));
    QVERIFY(undoText().contains(QStringLiteral("Ripple")));
    const ve::Timeline afterRipple = tl();

    // Escape in the middle of a ripple rolls it back.
    from = tailOf(clips("V1")[0].clip->id);
    QTest::mousePress(timeline, Qt::LeftButton, {}, from);
    QTest::mouseMove(timeline, from - QPoint(30, 0));
    QVERIFY(tl() != afterRipple);
    QTest::keyClick(timeline, Qt::Key_Escape);
    QTest::mouseRelease(timeline, Qt::LeftButton, {}, from - QPoint(30, 0));
    QVERIFY(tl() == afterRipple);

    // Ctrl+edge: roll. The cut moves later, the length of the timeline stays.
    drag(tailOf(clips("V1")[0].clip->id), tailOf(clips("V1")[0].clip->id) + QPoint(25, 0), Qt::ControlModifier);
    v = clips("V1");
    QVERIFY(v[0].end() > cut);
    QCOMPARE(v[1].start, v[0].end());
    QCOMPARE(tl().duration(), afterRipple.duration());
    QVERIFY(undoText().contains(QStringLiteral("Roll")));
    const Time rolledCut = v[0].end();

    // Ctrl+Alt on the sound's edge: roll that track only, an L cut that keeps picture and sound in sync.
    from = tailOf(clips("A1")[0].clip->id);
    drag(from, from + QPoint(25, 0), Qt::ControlModifier | Qt::AltModifier);
    QVERIFY(clips("A1")[0].end() > rolledCut);
    QCOMPARE(clips("V1")[0].end(), rolledCut);
    QVERIFY(ve::edit::checkSync(tl()).empty());
    action("actionUndo")->trigger();
    QCOMPARE(clips("A1")[0].end(), rolledCut);

    // Trim tool, body: slip. The media follows the mouse (earlier frames); place and length stay.
    const ve::Id second = clips("V1")[1].clip->id;
    const Time secondSource = clips("V1")[1].clip->sourceRange.start;
    from = timeline->clipRect(second).center();
    drag(from, from + QPoint(30, 0));
    QVERIFY(clips("V1")[1].clip->sourceRange.start < secondSource);
    QCOMPARE(clips("V1")[1].start, rolledCut);
    QCOMPARE(clips("A1")[1].clip->sourceRange.start, clips("V1")[1].clip->sourceRange.start);
    QVERIFY(undoText().contains(QStringLiteral("Slip")));

    // Selection tool, body dragged one row up: picture to V2, sound to A2.
    action("actionSelectTool")->trigger();
    const ve::Id secondSound = clips("A1")[1].clip->id;
    from = timeline->clipRect(second).center();
    drag(from, QPoint(from.x(), timeline->trackHeaderRect(track("V2").id).center().y()));
    QVERIFY(track("V2").findClip(second));
    QVERIFY(track("A2").findClip(secondSound));
    QCOMPARE(clips("V1").size(), std::size_t(1));

    // A marker 25 frames after the clip; a clip dragged near it snaps onto it.
    const Time clipStart = track("V2").findClip(second)->start;
    const Time markerAt = clipStart + frame(25);
    w.seekTo(markerAt);
    QTRY_COMPARE(w.playheadPosition(), markerAt);
    action("actionAddMarker")->trigger();
    QCOMPARE(tl().markers.size(), std::size_t(1));
    QCOMPARE(tl().markers[0].start, markerAt);
    w.seekTo(Time(0)); // keep the playhead out of the snapping
    QTRY_COMPARE(w.playheadPosition(), Time(0));
    from = timeline->clipRect(second).center();
    drag(from, from + QPoint(timeline->xForTime(markerAt) - timeline->xForTime(clipStart) + 3, 0));
    QCOMPARE(track("V2").findClip(second)->start, markerAt);
    QCOMPARE(track("A2").findClip(secondSound)->start, markerAt);

    // The marker dragged near the end of the first clip snaps onto that cut.
    const ve::Id marker = tl().markers[0].id;
    from = timeline->markerPoint(marker);
    drag(from, QPoint(timeline->xForTime(rolledCut) - 3, from.y()));
    QCOMPARE(tl().markers[0].start, rolledCut);
    QVERIFY(undoText().contains(QStringLiteral("Move Marker")));

    // The lock in A1's header: deleting the first take then leaves its sound alone.
    QTest::mouseClick(timeline, Qt::LeftButton, {}, timeline->trackFlagRect(track("A1").id, TimelineWidget::TrackFlag::Lock).center());
    QVERIFY(track("A1").locked);
    QTest::mouseClick(timeline, Qt::LeftButton, {}, timeline->clipRect(clips("V1")[0].clip->id).center());
    action("actionDelete")->trigger();
    QVERIFY(clips("V1").empty());
    QCOMPARE(clips("A1").size(), std::size_t(1));
    action("actionUndo")->trigger();
    QCOMPARE(clips("V1").size(), std::size_t(1));
    QTest::mouseClick(timeline, Qt::LeftButton, {}, timeline->trackFlagRect(track("A1").id, TimelineWidget::TrackFlag::Lock).center());
    QVERIFY(!track("A1").locked);

    // Insert (F9) of the pool's media at the start: everything moves right by its length, on every
    // sync-locked track, and the playhead lands after the new clip.
    const Time before = tl().duration();
    w.seekTo(Time(0));
    QTRY_COMPARE(w.playheadPosition(), Time(0));
    QVERIFY(action("actionInsert")->isEnabled());
    action("actionInsert")->trigger();
    QCOMPARE(tl().duration(), before + frame(240));
    QCOMPARE(clips("V1")[0].end(), frame(240));
    QCOMPARE(track("V2").findClip(second)->start, markerAt + frame(240));
    QTRY_COMPARE(w.playheadPosition(), frame(240));
    QCOMPARE(tl().markers[0].start, rolledCut); // markers belong to the timeline, not to clips

    QVERIFY(ve::edit::checkInvariants(tl(), w.project().media).empty());
    QVERIFY(ve::edit::checkSync(tl()).empty());
    if (const QString shot = qEnvironmentVariable("VE_UI_SCREENSHOT"); !shot.isEmpty()) {
        timeline->zoomToFit();
        QTest::qWait(300);
        QString path = shot;
        path.insert(path.lastIndexOf(QLatin1Char('.')), QStringLiteral("_phase2"));
        w.grab().save(path);
    }
}

void UiTests::panelsLayoutsAndSourceViewer()
{
    MainWindow w;
    w.resize(1400, 900);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto action = [&](const char* name) {
        auto* a = w.findChild<QAction*>(QLatin1String(name));
        Q_ASSERT(a);
        return a;
    };
    auto dock = [&](const char* name) {
        auto* d = w.findChild<QDockWidget*>(QLatin1String(name));
        Q_ASSERT(d);
        return d;
    };
    auto* timeline = w.findChild<TimelineWidget*>();
    auto* source = w.findChild<SourceViewerPanel*>();
    auto* pool = w.findChild<MediaPoolWidget*>();
    auto* tabs = w.findChild<QTabBar*>(QStringLiteral("timelineTabs"));
    QVERIFY(timeline && source && pool && tabs);
    const ve::Rate rate{30, 1};
    auto frame = [&](std::int64_t n) { return ve::timeOfFrame(n, rate); };
    auto clipsOn = [&](std::size_t track) { return w.project().timeline().tracks.at(track).clips(); };

    // Default layout: the panels are docked around the timeline viewer, the edit index behind the pool.
    for (const char* name : {"dockMediaPool", "dockSourceViewer", "dockInspector", "dockTimeline"})
        QTRY_VERIFY2(dock(name)->isVisible(), name);
    QVERIFY(!dock("dockEditIndex")->isHidden());
    QVERIFY(dock("dockEditIndex")->visibleRegion().isEmpty()); // a tab behind the pool
    QVERIFY(!dock("dockMediaPool")->visibleRegion().isEmpty());

    // A preset remembers which panels are shown and where they are, floating ones included.
    action("actionShowInspector")->trigger();
    QVERIFY(dock("dockInspector")->isHidden());
    dock("dockEditIndex")->setFloating(true);
    QVERIFY(w.saveLayoutPreset(QStringLiteral("Compact / review")));
    QVERIFY(w.layoutPresets().contains(QStringLiteral("Compact / review")));
    action("actionResetLayout")->trigger();
    QTRY_VERIFY(dock("dockInspector")->isVisible());
    QVERIFY(!dock("dockEditIndex")->isFloating());
    QVERIFY(w.loadLayoutPreset(QStringLiteral("Compact / review")));
    QTRY_VERIFY(dock("dockInspector")->isHidden());
    QVERIFY(dock("dockEditIndex")->isFloating());
    QVERIFY(w.deleteLayoutPreset(QStringLiteral("Compact / review")));
    QVERIFY(!w.layoutPresets().contains(QStringLiteral("Compact / review")));
    QVERIFY(!w.loadLayoutPreset(QStringLiteral("Compact / review")));
    action("actionResetLayout")->trigger();
    QTRY_VERIFY(dock("dockInspector")->isVisible());
    QVERIFY(!dock("dockEditIndex")->isFloating());

    // A double-click in the pool opens the media in the source viewer, which becomes the active viewer.
    w.importFiles({m_media});
    QCOMPARE(clipsOn(0).size(), std::size_t(1));
    QVERIFY(!w.sourceViewerActive());
    QTest::qWait(50);
    const QRect item = pool->visualItemRect(pool->item(0));
    QTest::mouseClick(pool->viewport(), Qt::LeftButton, {}, item.center()); // QTest's double-click event has no press
    QTest::mouseDClick(pool->viewport(), Qt::LeftButton, {}, item.center());
    QTRY_VERIFY(w.sourceViewerActive());
    QVERIFY(source->hasMedia());
    QCOMPARE(source->mediaId(), w.project().media.items.front().id);

    // I and O mark the active viewer: the source. Space plays it, not the timeline.
    source->seek(frame(30));
    QTRY_COMPARE(source->position(), frame(30));
    action("actionMarkIn")->trigger();
    source->seek(frame(89));
    QTRY_COMPARE(source->position(), frame(89));
    action("actionMarkOut")->trigger();
    QCOMPARE(source->sourceIn(), std::optional<Time>(frame(30)));
    QCOMPARE(source->sourceOut(), std::optional<Time>(frame(90)));
    action("actionPlayPause")->trigger();
    QTRY_VERIFY(source->isPlaying());
    QVERIFY(!w.isPlaying());
    action("actionPlayPause")->trigger();
    QTRY_VERIFY(!source->isPlaying());
    QTest::keyClick(&w, Qt::Key_Home); // the source's start, the timeline stays
    QTRY_COMPARE(source->position(), Time(0));

    // A click in the timeline makes the timeline viewer active; Q switches back and forth.
    QTest::mouseClick(timeline, Qt::LeftButton, {}, timeline->clipRect(clipsOn(0)[0].clip->id).center());
    QVERIFY(!w.sourceViewerActive());
    action("actionSwitchViewer")->trigger();
    QVERIFY(w.sourceViewerActive());
    action("actionSwitchViewer")->trigger();
    QVERIFY(!w.sourceViewerActive());

    // Overwrite (F10) of the source In -> Out at the timeline playhead. The source marks stay.
    w.seekTo(frame(60));
    QTRY_COMPARE(w.playheadPosition(), frame(60));
    QVERIFY(action("actionOverwrite")->isEnabled());
    action("actionOverwrite")->trigger();
    auto v = clipsOn(0);
    QCOMPARE(v.size(), std::size_t(3));
    QCOMPARE(v[1].start, frame(60));
    QCOMPARE(v[1].clip->sourceRange.start, frame(30));
    QCOMPARE(v[1].clip->sourceRange.duration, frame(60));
    QCOMPARE(clipsOn(1)[1].clip->sourceRange.start, frame(30));
    QTRY_COMPARE(w.playheadPosition(), frame(120));
    QCOMPARE(source->sourceIn(), std::optional<Time>(frame(30)));

    // Append (Shift+F12): the same range after the last clip, wherever the playhead is.
    w.seekTo(Time(0));
    QTRY_COMPARE(w.playheadPosition(), Time(0));
    action("actionAppend")->trigger();
    QCOMPARE(w.project().timeline().duration(), frame(300));
    QCOMPARE(clipsOn(0).back().start, frame(240));
    QTRY_COMPARE(w.playheadPosition(), frame(299)); // past the new clip: here the last frame

    // Timeline tabs: a new timeline becomes current; every timeline keeps its own playhead.
    QCOMPARE(tabs->count(), 1);
    const ve::Id first = w.project().currentTimeline;
    w.seekTo(frame(45));
    QTRY_COMPARE(w.playheadPosition(), frame(45));
    action("actionNewTimeline")->trigger();
    QCOMPARE(w.project().timelines.size(), std::size_t(2));
    QVERIFY(w.project().currentTimeline != first);
    QVERIFY(!w.project().timeline().hasClips());
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(tabs->currentIndex(), 1);
    QTRY_COMPARE(w.playheadPosition(), Time(0));
    QTest::mouseClick(tabs, Qt::LeftButton, {}, tabs->tabRect(0).center());
    QCOMPARE(w.project().currentTimeline, first);
    QTRY_COMPARE(w.playheadPosition(), frame(45));
    QCOMPARE(clipsOn(0).size(), std::size_t(4));
    action("actionUndo")->trigger(); // New Timeline
    QCOMPARE(w.project().timelines.size(), std::size_t(1));
    QCOMPARE(tabs->count(), 1);

    if (const QString shot = qEnvironmentVariable("VE_UI_SCREENSHOT"); !shot.isEmpty()) {
        timeline->zoomToFit();
        QTest::qWait(300);
        QString path = shot;
        path.insert(path.lastIndexOf(QLatin1Char('.')), QStringLiteral("_phase3"));
        w.grab().save(path);
    }
}

void UiTests::inspectorIndexAndContextMenus()
{
    MainWindow w;
    w.resize(1400, 900);
    w.show();
    QVERIFY(QTest::qWaitForWindowExposed(&w));
    auto action = [&](const char* name) {
        auto* a = w.findChild<QAction*>(QLatin1String(name));
        Q_ASSERT(a);
        return a;
    };
    auto undoText = [&] { return action("actionUndo")->text(); };
    auto* timeline = w.findChild<TimelineWidget*>();
    auto* tabs = w.findChild<QTabBar*>(QStringLiteral("timelineTabs"));
    QVERIFY(timeline && tabs);
    const ve::Rate rate{30, 1};
    auto frame = [&](std::int64_t n) { return ve::timeOfFrame(n, rate); };
    auto tl = [&]() -> const ve::Timeline& { return w.project().timeline(); };
    auto clipsOn = [&](std::size_t track) { return tl().tracks.at(track).clips(); };
    auto clip = [&](const ve::Id& id) -> const ve::ClipBase& {
        const ve::ClipBase* found = nullptr;
        for (const ve::Track& t : tl().tracks) {
            if (auto placed = t.findClip(id))
                found = placed->clip;
        }
        if (!found)
            qFatal("no clip %s", id.c_str());
        return *found;
    };
    auto click = [&](const ve::Id& clipId, Qt::KeyboardModifiers modifiers = {}) {
        QTest::mouseClick(timeline, Qt::LeftButton, modifiers, timeline->clipRect(clipId).center());
    };
    auto trigger = [&](const TimelineWidget::Context& context, const char* name) {
        std::unique_ptr<QMenu> menu = w.contextMenu(context);
        QVERIFY2(menu, name);
        auto* a = menu->findChild<QAction*>(QLatin1String(name));
        QVERIFY2(a, name);
        a->trigger();
    };
    using Kind = TimelineWidget::Context::Kind;

    // Two takes X | Y, picture and sound linked.
    w.importFiles({m_media});
    w.seekTo(frame(120));
    QTRY_COMPARE(w.playheadPosition(), frame(120));
    action("actionSplit")->trigger();
    QTest::qWait(50);
    const ve::Id x = clipsOn(0)[0].clip->id;
    const ve::Id xSound = clipsOn(1)[0].clip->id;
    const ve::Id y = clipsOn(0)[1].clip->id;
    const ve::Id ySound = clipsOn(1)[1].clip->id;

    // Inspector: a click shows the clip; renaming there renames picture and sound of the take.
    click(x);
    auto* name = w.findChild<QLineEdit*>(QStringLiteral("inspectorClipName"));
    QVERIFY(name);
    QTRY_VERIFY(name->isVisible());
    name->setFocus();
    name->selectAll();
    QTest::keyClicks(name, QStringLiteral("Opening shot")); // letters are typed, not taken as shortcuts
    QTest::keyClick(name, Qt::Key_Return);
    QCOMPARE(clip(x).name, std::string("Opening shot"));
    QCOMPARE(clip(xSound).name, std::string("Opening shot"));
    QVERIFY(undoText().contains(QStringLiteral("Rename Clip")));
    auto* color = w.findChild<QComboBox*>(QStringLiteral("inspectorClipColor"));
    QVERIFY(color);
    color->setCurrentIndex(color->findData(QStringLiteral("teal")));
    QCOMPARE(clip(x).color, std::string("teal"));
    QCOMPARE(clip(xSound).color, std::string("teal"));
    auto* enabled = w.findChild<QCheckBox*>(QStringLiteral("inspectorClipEnabled"));
    QVERIFY(enabled && enabled->isChecked());
    enabled->click();
    QVERIFY(!clip(x).enabled);
    action("actionEnableClip")->trigger(); // D toggles it back
    QVERIFY(clip(x).enabled);

    // The clip's context menu: colour, flag, unlink (and undo of each).
    click(y);
    TimelineWidget::Context ctx;
    ctx.kind = Kind::Clip;
    ctx.clipId = QString::fromStdString(y);
    trigger(ctx, "contextClipColor:orange");
    QCOMPARE(clip(y).color, std::string("orange"));
    QCOMPARE(clip(ySound).color, std::string("orange"));
    trigger(ctx, "contextFlag:red");
    QCOMPARE(clip(y).flags, std::vector<std::string>{"red"});
    trigger(ctx, "contextFlag:red"); // checked now: removes it
    QVERIFY(clip(y).flags.empty());
    trigger(ctx, "contextLinkClips");
    QVERIFY(clip(y).linkGroup.empty());
    QVERIFY(clip(ySound).linkGroup.empty());
    action("actionUndo")->trigger();
    QVERIFY(!clip(y).linkGroup.empty());
    QCOMPARE(clip(y).linkGroup, clip(ySound).linkGroup);

    // A gap: lift X, then ripple delete the gap from its context menu (sound follows: sync lock).
    click(x);
    action("actionDelete")->trigger();
    QCOMPARE(clipsOn(0).size(), std::size_t(1));
    ctx = {};
    ctx.kind = Kind::Gap;
    ctx.trackId = QString::fromStdString(tl().tracks[0].id);
    ctx.at = frame(60);
    trigger(ctx, "contextDeleteGap");
    QCOMPARE(clipsOn(0)[0].start, Time(0));
    QCOMPARE(clipsOn(1)[0].start, Time(0));
    QCOMPARE(tl().duration(), frame(120));
    action("actionUndo")->trigger();
    action("actionUndo")->trigger();
    QCOMPARE(clipsOn(0).size(), std::size_t(2));
    QCOMPARE(clipsOn(0)[1].start, frame(120));

    // Track header and marker menus.
    ctx = {};
    ctx.kind = Kind::TrackHeader;
    ctx.trackId = QString::fromStdString(tl().tracks[1].id);
    trigger(ctx, "contextEnableTrack");
    QVERIFY(!tl().tracks[1].enabled);
    trigger(ctx, "contextTrackColor:navy");
    QCOMPARE(tl().tracks[1].color, std::string("navy"));
    w.seekTo(frame(30));
    QTRY_COMPARE(w.playheadPosition(), frame(30));
    action("actionAddMarker")->trigger();
    ctx = {};
    ctx.kind = Kind::Marker;
    ctx.markerId = QString::fromStdString(tl().markers.at(0).id);
    trigger(ctx, "contextMarkerColor:red");
    QCOMPARE(tl().markers[0].color, std::string("red"));
    ctx = {};
    ctx.kind = Kind::Ruler;
    ctx.at = frame(200);
    trigger(ctx, "contextAddMarker");
    QCOMPARE(tl().markers.size(), std::size_t(2));
    QCOMPARE(tl().markers[1].start, frame(200));

    // Edit index: one row per clip; a click selects that clip and moves the playhead to it.
    auto* indexDock = w.findChild<QDockWidget*>(QStringLiteral("dockEditIndex"));
    indexDock->raise();
    QTRY_VERIFY(!indexDock->visibleRegion().isEmpty());
    auto* edits = w.findChild<QTableWidget*>(QStringLiteral("editIndexEdits"));
    QVERIFY(edits);
    QTRY_COMPARE(edits->rowCount(), 4);
    int row = -1;
    for (int r = 0; r < edits->rowCount(); ++r) {
        if (edits->item(r, 0)->data(Qt::UserRole).toString().toStdString() == y)
            row = r;
    }
    QVERIFY(row >= 0);
    QTest::mouseClick(edits->viewport(), Qt::LeftButton, {}, edits->visualItemRect(edits->item(row, 2)).center());
    const std::vector<ve::Id> selected = timeline->selectedClips();
    QVERIFY(std::find(selected.begin(), selected.end(), y) != selected.end());
    QTRY_COMPARE(w.playheadPosition(), frame(120));

    // Linked selection off: a click takes only the picture, Delete leaves the sound.
    action("actionLinkedSelection")->trigger();
    QVERIFY(!timeline->linkedSelection());
    click(x);
    QCOMPARE(timeline->selectedClips(), std::vector<ve::Id>{x});
    action("actionDelete")->trigger();
    QCOMPARE(clipsOn(0).size(), std::size_t(1));
    QCOMPARE(clipsOn(1).size(), std::size_t(2));
    action("actionUndo")->trigger();
    action("actionLinkedSelection")->trigger();
    QVERIFY(timeline->linkedSelection());

    // A compound clip opens in its own tab with a double-click; the first tab goes back.
    click(x);
    action("actionCompose")->trigger();
    const ve::Id parent = w.project().currentTimeline;
    QCOMPARE(w.project().timelines.size(), std::size_t(2));
    const ve::Id compound = clipsOn(0)[0].clip->id;
    QVERIFY(std::holds_alternative<ve::CompoundClip>(*clipsOn(0)[0].item));
    QTest::qWait(50);
    QTest::mouseDClick(timeline, Qt::LeftButton, {}, timeline->clipRect(compound).center());
    QVERIFY(w.project().currentTimeline != parent);
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(tabs->currentIndex(), 1);
    QCOMPARE(clipsOn(0).size(), std::size_t(1)); // X inside
    QTest::mouseClick(tabs, Qt::LeftButton, {}, tabs->tabRect(0).center());
    QCOMPARE(w.project().currentTimeline, parent);
    QVERIFY(ve::edit::checkInvariants(tl(), w.project().media).empty());
    QVERIFY(ve::edit::checkSync(tl()).empty());
}

void UiTests::updatesFromAFeed()
{
    // Versions and checksums.
    QCOMPARE(update::versionOf(QStringLiteral("v0.0.2")), QVersionNumber(0, 0, 2));
    QCOMPARE(update::versionOf(QStringLiteral("1.2.3-beta")), QVersionNumber(1, 2, 3));
    QVERIFY(update::versionOf(QStringLiteral("latest")).isNull());
    QVERIFY(update::versionOf(QStringLiteral("v0.0.10")) > update::versionOf(QStringLiteral("v0.0.9")));
    const QByteArray hash(64, 'a');
    QCOMPARE(update::checksumFrom(hash.toUpper() + "  VideoEditor-0.0.2-macos-arm64.zip\n"), hash);
    QVERIFY(update::checksumFrom("not a checksum").isEmpty());
    QCOMPARE(QString::fromLatin1(VE_VERSION), QStringLiteral("0.0.1"));

    // A local release feed shaped like GitHub's, with this platform's archive and its checksum.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString suffix = update::platformAssetSuffix();
    const QString assetName = QStringLiteral("VideoEditor-0.0.2-%1").arg(suffix);
    const QByteArray archive = QByteArray("pretend this is a zip ").repeated(5000);
    auto writeFile = [&](const QString& name, const QByteArray& data) {
        QFile f(dir.filePath(name));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(data);
    };
    writeFile(assetName, archive);
    const QByteArray digest = QCryptographicHash::hash(archive, QCryptographicHash::Sha256).toHex();
    writeFile(assetName + QStringLiteral(".sha256"), digest + "  " + assetName.toUtf8() + "\n");
    auto feedJson = [&](const QString& tag, qint64 size) {
        QJsonObject asset{{QStringLiteral("name"), assetName},
                          {QStringLiteral("size"), size},
                          {QStringLiteral("browser_download_url"), QUrl::fromLocalFile(dir.filePath(assetName)).toString()}};
        QJsonObject checksum{{QStringLiteral("name"), assetName + QStringLiteral(".sha256")},
                             {QStringLiteral("browser_download_url"),
                              QUrl::fromLocalFile(dir.filePath(assetName + QStringLiteral(".sha256"))).toString()}};
        QJsonObject other{{QStringLiteral("name"), QStringLiteral("VideoEditor-0.0.2-plan9-mips.zip")},
                          {QStringLiteral("browser_download_url"), QStringLiteral("https://example.invalid/x.zip")}};
        QJsonObject release{{QStringLiteral("tag_name"), tag},
                            {QStringLiteral("name"), QStringLiteral("VideoEditor ") + tag},
                            {QStringLiteral("body"), QStringLiteral("What changed")},
                            {QStringLiteral("html_url"), QStringLiteral("https://github.com/WilkPrintes/videoeditor/releases/tag/") + tag},
                            {QStringLiteral("assets"), QJsonArray{other, asset, checksum}}};
        return QJsonDocument(release).toJson();
    };

    QString error;
    auto parsed = update::parseRelease(feedJson(QStringLiteral("v0.0.2"), archive.size()), suffix, &error);
    QVERIFY2(parsed, qPrintable(error));
    QCOMPARE(parsed->version, QVersionNumber(0, 0, 2));
    QCOMPARE(parsed->assetName, assetName);
    QVERIFY(parsed->checksumUrl.isLocalFile());
    QVERIFY(update::parseRelease(feedJson(QStringLiteral("v0.0.2"), 0), QStringLiteral("plan9-arm.zip"))->assetName.isEmpty());
    QVERIFY(!update::parseRelease("<html>", suffix, &error));
    QVERIFY(!error.isEmpty());

    // Checking: newer than 0.0.1, not newer than itself.
    writeFile(QStringLiteral("latest.json"), feedJson(QStringLiteral("v0.0.2"), archive.size()));
    const QUrl feed = QUrl::fromLocalFile(dir.filePath(QStringLiteral("latest.json")));
    Updater updater(feed, QVersionNumber(0, 0, 1));
    QSignalSpy checked(&updater, &Updater::checked);
    updater.check();
    QTRY_COMPARE(checked.size(), 1);
    QCOMPARE(checked[0][0].toBool(), true);
    QVERIFY(checked[0][2].toString().isEmpty());
    const ReleaseInfo release = checked[0][1].value<ReleaseInfo>();
    QCOMPARE(release.version, QVersionNumber(0, 0, 2));
    Updater current(feed, QVersionNumber(0, 0, 2));
    QSignalSpy upToDate(&current, &Updater::checked);
    current.check();
    QTRY_COMPARE(upToDate.size(), 1);
    QCOMPARE(upToDate[0][0].toBool(), false);
    Updater missing(QUrl::fromLocalFile(dir.filePath(QStringLiteral("none.json"))), QVersionNumber(0, 0, 1));
    QSignalSpy noFeed(&missing, &Updater::checked);
    missing.check();
    QTRY_COMPARE(noFeed.size(), 1);
    QVERIFY(!noFeed[0][2].toString().isEmpty());

    // Downloading: the archive arrives and matches its SHA-256.
    QSignalSpy downloaded(&updater, &Updater::downloaded);
    QSignalSpy failed(&updater, &Updater::failed);
    updater.download(release);
    QTRY_VERIFY(downloaded.size() + failed.size() == 1);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed[0][0].toString()));
    const QString zip = downloaded[0][1].toString();
    QFile got(zip);
    QVERIFY(got.open(QIODevice::ReadOnly));
    QCOMPARE(got.readAll(), archive);
    got.close();

    // A checksum that does not match: the download is refused and deleted.
    writeFile(assetName + QStringLiteral(".sha256"), QByteArray(64, 'b') + "  " + assetName.toUtf8() + "\n");
    updater.download(release);
    QTRY_VERIFY(downloaded.size() + failed.size() == 2);
    QCOMPARE(failed.size(), 1);
    QVERIFY(failed[0][0].toString().contains(QStringLiteral("SHA-256")));
    QVERIFY(!QFile::exists(zip));

#if !defined(Q_OS_MACOS)
    QVERIFY(!Updater::canInstallInPlace()); // only the macOS app replaces itself
#endif
}

QTEST_MAIN(UiTests)
#include "ui_tests.moc"
