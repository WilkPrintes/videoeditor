// GUI tests: drive the real MainWindow with synthetic mouse and key events, the paths a user takes,
// and check the model after each step.
// - editAndPlayThroughTheUi (phase 1): import -> blade -> drag clip -> trim edge -> cancelled drag ->
//   undo -> play.
// - multitrackTrimsAndMarkers (phase 2): add tracks -> ripple with the trim tool -> cancelled ripple ->
//   roll (Ctrl) -> L cut (Ctrl+Alt) -> slip -> move to V2 -> marker snapping -> lock from the track
//   header -> insert from the media pool (F9), each drag one undo step.
//
// Uses VE_TEST_MEDIA if set, otherwise generates a clapper clip with the engine's test pattern.
// VE_UI_SCREENSHOT=<file.png> saves the window after the edits (the phase 2 test adds "_phase2").

#include "core/Edit.h"
#include "media/TestPattern.h"
#include "ui/MainWindow.h"
#include "ui/MediaPoolWidget.h"
#include "ui/TimelineWidget.h"

#include <QAction>
#include <QTemporaryDir>
#include <QtTest>

using ve::Time;

class UiTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void editAndPlayThroughTheUi();
    void multitrackTrimsAndMarkers();

private:
    QTemporaryDir m_dir;
    QString m_media;
};

void UiTests::initTestCase()
{
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

QTEST_MAIN(UiTests)
#include "ui_tests.moc"
