// GUI smoke test: drives the real MainWindow with synthetic mouse and key events, the path a user
// takes (import -> blade -> drag clip -> trim edge -> cancelled drag -> undo -> play), and checks the
// model after each step.
//
// Uses VE_TEST_MEDIA if set, otherwise generates a clapper clip with the engine's test pattern.
// VE_UI_SCREENSHOT=<file.png> saves the window after the edits.

#include "media/TestPattern.h"
#include "ui/MainWindow.h"
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

QTEST_MAIN(UiTests)
#include "ui_tests.moc"
