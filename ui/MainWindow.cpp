#include "ui/MainWindow.h"

#include "core/Edit.h"
#include "core/ProjectFile.h"
#include "engine/Exporter.h"
#include "engine/PlaybackEngine.h"
#include "media/MediaProbe.h"
#include "ui/ExportDialog.h"
#include "ui/MediaPoolWidget.h"
#include "ui/TimelineWidget.h"
#include "ui/ViewerWidget.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QThread>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

using ve::Time;

namespace {

constexpr const char* kGestureMergeKey = "timeline-gesture";

QString projectFilter()
{
    return QObject::tr("VideoEditor project (*.veproj)");
}

QString mediaFilter()
{
    return QObject::tr("Media files (*.mp4 *.mov *.mkv *.m4v *.avi *.mxf *.webm *.mts *.m2ts *.wav *.mp3 *.m4a *.aac "
                       "*.flac);;All files (*)");
}

// The first video placed on an empty timeline sets its frame rate and size.
void conformTimeline(ve::Timeline& tl, const ve::MediaItem& media)
{
    tl.rate = media.frameRate;
    tl.width = media.width & ~1;
    tl.height = media.height & ~1;
}

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

std::string utf8(const QString& s)
{
    return s.toStdString();
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_engine(std::make_unique<ve::PlaybackEngine>())
{
    m_project = ve::makeEmptyProject();
    buildUi();
    buildActions();
    m_commands.onChanged = [this] { onModelChanged(); };
    m_timeline->setProject(&m_project);
    onModelChanged();

    m_ticker.setTimerType(Qt::PreciseTimer);
    m_ticker.setInterval(8);
    connect(&m_ticker, &QTimer::timeout, this, &MainWindow::tick);
    m_ticker.start();
}

MainWindow::~MainWindow()
{
    m_ticker.stop();
    m_commands.onChanged = nullptr;
}

// ---------------------------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------------------------

void MainWindow::buildUi()
{
    m_pool = new MediaPoolWidget;
    m_viewer = new ViewerWidget;
    m_timeline = new TimelineWidget;

    auto makeButton = [this](QStyle::StandardPixmap icon, const QString& tip, auto slot) {
        auto* b = new QToolButton;
        b->setIcon(style()->standardIcon(icon));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setIconSize(QSize(22, 22));
        connect(b, &QToolButton::clicked, this, slot);
        return b;
    };
    auto* toStart = makeButton(QStyle::SP_MediaSkipBackward, tr("Go to start (Home)"), [this] { seekTo(Time(0)); });
    auto* prev = makeButton(QStyle::SP_MediaSeekBackward, tr("Previous frame (Left)"), [this] { stepFrames(-1); });
    m_playButton = makeButton(QStyle::SP_MediaPlay, tr("Play / pause (Space)"), [this] { togglePlay(); });
    auto* next = makeButton(QStyle::SP_MediaSeekForward, tr("Next frame (Right)"), [this] { stepFrames(1); });
    auto* toEnd = makeButton(QStyle::SP_MediaSkipForward, tr("Go to end (End)"), [this] {
        seekToFrame(ve::frameAt(m_project.timeline().duration(), m_project.timeline().rate) - 1);
    });

    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(mono.pointSizeF() * 1.35);
    m_timecode = new QLabel(QStringLiteral("00:00:00:00"));
    m_timecode->setFont(mono);
    m_duration = new QLabel;
    m_duration->setFont(mono);
    m_duration->setStyleSheet(QStringLiteral("color: #8a8f98;"));

    auto* transport = new QWidget;
    auto* transportLayout = new QHBoxLayout(transport);
    transportLayout->setContentsMargins(10, 4, 10, 4);
    transportLayout->addWidget(m_timecode);
    transportLayout->addStretch(1);
    for (QToolButton* b : {toStart, prev, m_playButton, next, toEnd})
        transportLayout->addWidget(b);
    transportLayout->addStretch(1);
    transportLayout->addWidget(m_duration);

    auto* viewerPanel = new QWidget;
    auto* viewerLayout = new QVBoxLayout(viewerPanel);
    viewerLayout->setContentsMargins(0, 0, 0, 0);
    viewerLayout->setSpacing(0);
    viewerLayout->addWidget(m_viewer, 1);
    viewerLayout->addWidget(transport);

    auto* top = new QSplitter(Qt::Horizontal);
    top->addWidget(m_pool);
    top->addWidget(viewerPanel);
    top->setStretchFactor(0, 0);
    top->setStretchFactor(1, 1);
    top->setSizes({300, 1300});

    auto* main = new QSplitter(Qt::Vertical);
    main->addWidget(top);
    main->addWidget(m_timeline);
    main->setStretchFactor(0, 3);
    main->setStretchFactor(1, 2);
    main->setSizes({600, 300});
    setCentralWidget(main);

    m_status = new QLabel;
    statusBar()->addPermanentWidget(m_status);

    connect(m_viewer, &ViewerWidget::backendReady, this, [this](const QString& name) {
        m_gpuBackend = name;
        updateWindowState();
    });
    connect(m_timeline, &TimelineWidget::seekRequested, this, &MainWindow::seekTo);

    // Timeline gestures: one transaction per drag; every step replaces the previous one.
    connect(m_timeline, &TimelineWidget::gestureStarted, this,
            [this](const QString& name) { m_commands.beginTransaction(utf8(name)); });
    connect(m_timeline, &TimelineWidget::gestureFinished, this, [this](bool commit) {
        if (!m_commands.inTransaction())
            return;
        if (commit)
            m_commands.commit();
        else
            m_commands.abort();
    });
    connect(m_timeline, &TimelineWidget::moveRequested, this, [this](const QStringList& ids, Time delta, int shift) {
        std::vector<ve::Id> clips;
        for (const QString& id : ids)
            clips.push_back(utf8(id));
        edit(tr("Move"), [clips, delta, shift](ve::Timeline& tl, const ve::MediaPool&) {
            return ve::edit::moveClips(tl, clips, delta, shift);
        }, kGestureMergeKey);
    });
    connect(m_timeline, &TimelineWidget::trimRequested, this, [this](const QString& id, bool head, Time edge, bool linked) {
        const ve::Id clip = utf8(id);
        edit(tr("Trim"), [clip, head, edge, linked](ve::Timeline& tl, const ve::MediaPool& pool) {
            return ve::edit::trimClip(tl, pool, clip, head ? ve::edit::Edge::Head : ve::edit::Edge::Tail, edge, linked);
        }, kGestureMergeKey);
    });
    connect(m_timeline, &TimelineWidget::trimEditRequested, this,
            [this](TimelineWidget::TrimMode mode, const QString& id, bool head, Time delta, bool linked) {
        const ve::Id clip = utf8(id);
        const auto edge = head ? ve::edit::Edge::Head : ve::edit::Edge::Tail;
        switch (mode) {
        case TimelineWidget::TrimMode::Ripple:
            edit(tr("Ripple"), [=](ve::Timeline& tl, const ve::MediaPool& pool) {
                return ve::edit::rippleTrim(tl, pool, clip, edge, delta, linked);
            }, kGestureMergeKey);
            break;
        case TimelineWidget::TrimMode::Roll:
            edit(tr("Roll"), [=](ve::Timeline& tl, const ve::MediaPool& pool) {
                return ve::edit::rollTrim(tl, pool, clip, edge, delta, linked);
            }, kGestureMergeKey);
            break;
        case TimelineWidget::TrimMode::Slip:
            edit(tr("Slip"), [=](ve::Timeline& tl, const ve::MediaPool& pool) {
                return ve::edit::slipClip(tl, pool, clip, delta);
            }, kGestureMergeKey);
            break;
        case TimelineWidget::TrimMode::Slide:
            edit(tr("Slide"), [=](ve::Timeline& tl, const ve::MediaPool& pool) {
                return ve::edit::slideClip(tl, pool, clip, delta);
            }, kGestureMergeKey);
            break;
        }
    });
    connect(m_timeline, &TimelineWidget::markerMoveRequested, this, [this](const QString& id, Time to) {
        const ve::Id marker = utf8(id);
        edit(tr("Move Marker"), [marker, to](ve::Timeline& tl, const ve::MediaPool&) {
            return ve::edit::moveMarker(tl, marker, to);
        }, kGestureMergeKey);
    });
    connect(m_timeline, &TimelineWidget::trackFlagRequested, this,
            [this](const QString& id, TimelineWidget::TrackFlag flag, bool on) {
        const ve::Id track = utf8(id);
        if (flag == TimelineWidget::TrackFlag::Lock)
            edit(on ? tr("Lock Track") : tr("Unlock Track"), [track, on](ve::Timeline& tl, const ve::MediaPool&) {
                return ve::edit::setTrackLocked(tl, track, on);
            });
        else
            edit(on ? tr("Sync Lock Track") : tr("Remove Sync Lock"), [track, on](ve::Timeline& tl, const ve::MediaPool&) {
                return ve::edit::setTrackSyncLock(tl, track, on);
            });
    });
    connect(m_timeline, &TimelineWidget::mediaDropped, this,
            [this](const QString& mediaId, Time at, const QString& trackId) { placeMediaAt(mediaId, at, trackId); });
    connect(m_timeline, &TimelineWidget::filesDropped, this,
            [this](const QStringList& paths, Time at) { importFiles(paths, at); });
    connect(m_timeline, &TimelineWidget::selectionChanged, this, &MainWindow::updateWindowState);
    connect(m_pool, &MediaPoolWidget::mediaActivated, this,
            [this](const QString& id) { placeMediaAt(id, std::nullopt); });
    connect(m_pool, &MediaPoolWidget::filesDropped, this, [this](const QStringList& paths) { importFiles(paths); });
    connect(m_pool, &QListWidget::itemSelectionChanged, this, &MainWindow::updateWindowState);
}

void MainWindow::buildActions()
{
    auto add = [this](QMenu* menu, const QString& text, const QList<QKeySequence>& keys, auto slot) {
        QAction* a = menu->addAction(text);
        a->setShortcuts(keys);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };
    const auto oneSecond = [this] { return std::int64_t(std::llround(m_project.timeline().rate.toDouble())); };

    QMenu* file = menuBar()->addMenu(tr("&File"));
    add(file, tr("&New Project"), {QKeySequence::New}, [this] { newProject(); });
    add(file, tr("&Open Project..."), {QKeySequence::Open}, [this] { openProject(); });
    add(file, tr("&Save Project"), {QKeySequence::Save}, [this] { save(); });
    add(file, tr("Save Project &As..."), {QKeySequence::SaveAs}, [this] { saveAs(); });
    file->addSeparator();
    QAction* importAction = add(file, tr("&Import Media..."), {QKeySequence(Qt::CTRL | Qt::Key_I)}, [this] { importMedia(); });
    m_exportAction = add(file, tr("&Export H.264 / MP4..."), {QKeySequence(Qt::CTRL | Qt::Key_E)}, [this] { exportTimeline(); });
    file->addSeparator();
    add(file, tr("&Quit"), {QKeySequence::Quit}, [this] { close(); });

    QMenu* editMenu = menuBar()->addMenu(tr("&Edit"));
    m_undoAction = add(editMenu, tr("&Undo"), {QKeySequence::Undo}, [this] { m_commands.undo(); });
    m_redoAction = add(editMenu, tr("&Redo"),
                       {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), QKeySequence(Qt::CTRL | Qt::Key_Y)},
                       [this] { m_commands.redo(); });
    editMenu->addSeparator();
    m_splitAction = add(editMenu, tr("&Split at Playhead"), {QKeySequence(Qt::CTRL | Qt::Key_B), QKeySequence(Qt::Key_B)},
                        [this] { splitAtPlayhead(); });
    m_deleteAction = add(editMenu, tr("&Delete Clips"), {QKeySequence::Delete, QKeySequence(Qt::Key_Backspace)},
                         [this] { deleteSelection(); });
    m_rippleDeleteAction = add(editMenu, tr("R&ipple Delete"),
                               {QKeySequence(Qt::SHIFT | Qt::Key_Delete), QKeySequence(Qt::SHIFT | Qt::Key_Backspace)},
                               [this] { rippleDeleteSelection(); });
    editMenu->addSeparator();
    // Tools (spec §6.2): what a drag on a clip does. Modifiers are listed in TimelineWidget.h.
    auto* tools = new QActionGroup(this);
    QAction* selectTool = add(editMenu, tr("&Selection Tool"), {QKeySequence(Qt::Key_A)},
                              [this] { m_timeline->setTool(TimelineWidget::Tool::Select); });
    QAction* trimTool = add(editMenu, tr("&Trim Tool (ripple, slip, slide)"), {QKeySequence(Qt::Key_T)},
                            [this] { m_timeline->setTool(TimelineWidget::Tool::Trim); });
    for (QAction* a : {selectTool, trimTool}) {
        a->setCheckable(true);
        tools->addAction(a);
    }
    selectTool->setChecked(true);
    QAction* snapping = add(editMenu, tr("S&napping"), {QKeySequence(Qt::Key_N)},
                            [this](bool on) { m_timeline->setSnapping(on); });
    snapping->setCheckable(true);
    snapping->setChecked(true);

    QMenu* timelineMenu = menuBar()->addMenu(tr("&Timeline"));
    const std::pair<SourceEdit, std::pair<QString, QList<QKeySequence>>> sourceEdits[] = {
        {SourceEdit::Insert, {tr("&Insert"), {QKeySequence(Qt::Key_F9)}}},
        {SourceEdit::Overwrite, {tr("&Overwrite"), {QKeySequence(Qt::Key_F10)}}},
        {SourceEdit::Replace, {tr("&Replace"), {QKeySequence(Qt::Key_F11)}}},
        {SourceEdit::FitToFill, {tr("&Fit to Fill"), {QKeySequence(Qt::SHIFT | Qt::Key_F11)}}},
        {SourceEdit::PlaceOnTop, {tr("Place on &Top"), {QKeySequence(Qt::Key_F12)}}},
    };
    const char* sourceEditNames[] = {"actionInsert", "actionOverwrite", "actionReplace", "actionFitToFill", "actionPlaceOnTop"};
    for (std::size_t i = 0; i < std::size(sourceEdits); ++i) {
        const SourceEdit kind = sourceEdits[i].first;
        QAction* a = add(timelineMenu, sourceEdits[i].second.first, sourceEdits[i].second.second, [this, kind] { sourceEdit(kind); });
        a->setObjectName(QLatin1String(sourceEditNames[i]));
        a->setToolTip(tr("Edits the media selected in the pool onto the destination tracks, at the playhead or the In/Out"));
        m_sourceEditActions << a;
    }
    timelineMenu->addSeparator();
    QAction* markInAction = add(timelineMenu, tr("Mark &In"), {QKeySequence(Qt::Key_I)}, [this] { markIn(); });
    QAction* markOutAction = add(timelineMenu, tr("Mark O&ut"), {QKeySequence(Qt::Key_O)}, [this] { markOut(); });
    add(timelineMenu, tr("&Clear In and Out"), {QKeySequence(Qt::ALT | Qt::Key_X)}, [this] { clearMarks(); });
    timelineMenu->addSeparator();
    QAction* addMarkerAction = add(timelineMenu, tr("Add &Marker"), {QKeySequence(Qt::Key_M)}, [this] { addMarker(); });
    add(timelineMenu, tr("Previous Marker"), {QKeySequence(Qt::SHIFT | Qt::Key_Up)}, [this] { jumpToMarker(-1); });
    add(timelineMenu, tr("Next Marker"), {QKeySequence(Qt::SHIFT | Qt::Key_Down)}, [this] { jumpToMarker(1); });
    add(timelineMenu, tr("Previous Edit"), {QKeySequence(Qt::Key_Up)}, [this] { jumpToEdit(-1); });
    add(timelineMenu, tr("Next Edit"), {QKeySequence(Qt::Key_Down)}, [this] { jumpToEdit(1); });
    timelineMenu->addSeparator();
    QAction* addVideoTrack = add(timelineMenu, tr("Add &Video Track"), {QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_V)},
                                 [this] { addTrack(ve::TrackKind::Video); });
    QAction* addAudioTrack = add(timelineMenu, tr("Add &Audio Track"), {QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_A)},
                                 [this] { addTrack(ve::TrackKind::Audio); });
    timelineMenu->addSeparator();
    m_composeAction = add(timelineMenu, tr("New &Compound Clip"), {QKeySequence(Qt::ALT | Qt::Key_C)},
                          [this] { composeSelection(); });
    m_decomposeAction = add(timelineMenu, tr("&Decompose Compound Clip"), {QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_C)},
                            [this] { decomposeSelection(); });

    QMenu* playback = menuBar()->addMenu(tr("&Playback"));
    QAction* playPause = add(playback, tr("Play / Pause"), {QKeySequence(Qt::Key_Space)}, [this] { togglePlay(); });
    add(playback, tr("Play"), {QKeySequence(Qt::Key_L)}, [this] { play(); });
    add(playback, tr("Pause"), {QKeySequence(Qt::Key_K)}, [this] { m_engine->pause(); });
    playback->addSeparator();
    add(playback, tr("Previous Frame"), {QKeySequence(Qt::Key_Left)}, [this] { stepFrames(-1); });
    add(playback, tr("Next Frame"), {QKeySequence(Qt::Key_Right)}, [this] { stepFrames(1); });
    add(playback, tr("Back One Second"), {QKeySequence(Qt::SHIFT | Qt::Key_Left)},
        [this, oneSecond] { stepFrames(-oneSecond()); });
    add(playback, tr("Forward One Second"), {QKeySequence(Qt::SHIFT | Qt::Key_Right)},
        [this, oneSecond] { stepFrames(oneSecond()); });
    add(playback, tr("Go to Start"), {QKeySequence(Qt::Key_Home)}, [this] { seekTo(Time(0)); });
    add(playback, tr("Go to End"), {QKeySequence(Qt::Key_End)}, [this] {
        seekToFrame(ve::frameAt(m_project.timeline().duration(), m_project.timeline().rate) - 1);
    });

    QMenu* view = menuBar()->addMenu(tr("&View"));
    add(view, tr("Zoom In"), {QKeySequence(Qt::CTRL | Qt::Key_Equal), QKeySequence(Qt::CTRL | Qt::Key_Plus)},
        [this] { m_timeline->zoomBy(1.5); });
    add(view, tr("Zoom Out"), {QKeySequence(Qt::CTRL | Qt::Key_Minus)}, [this] { m_timeline->zoomBy(1.0 / 1.5); });
    add(view, tr("Zoom to Fit"), {QKeySequence(Qt::SHIFT | Qt::Key_Z)}, [this] { m_timeline->zoomToFit(); });

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    add(help, tr("&About"), {}, [this] {
        QMessageBox::about(this, tr("About VideoEditor"),
                           tr("<b>VideoEditor %1</b> (case study)<br>Qt %2 (RHI: %3), FFmpeg (LGPL, dynamically "
                              "linked), miniaudio.")
                               .arg(QApplication::applicationVersion(), QString::fromLatin1(qVersion()), m_gpuBackend));
    });

    // Stable names for tests and future scripting.
    importAction->setObjectName(QStringLiteral("actionImport"));
    m_exportAction->setObjectName(QStringLiteral("actionExport"));
    m_undoAction->setObjectName(QStringLiteral("actionUndo"));
    m_redoAction->setObjectName(QStringLiteral("actionRedo"));
    m_splitAction->setObjectName(QStringLiteral("actionSplit"));
    m_deleteAction->setObjectName(QStringLiteral("actionDelete"));
    m_rippleDeleteAction->setObjectName(QStringLiteral("actionRippleDelete"));
    playPause->setObjectName(QStringLiteral("actionPlayPause"));
    selectTool->setObjectName(QStringLiteral("actionSelectTool"));
    trimTool->setObjectName(QStringLiteral("actionTrimTool"));
    snapping->setObjectName(QStringLiteral("actionSnapping"));
    markInAction->setObjectName(QStringLiteral("actionMarkIn"));
    markOutAction->setObjectName(QStringLiteral("actionMarkOut"));
    addMarkerAction->setObjectName(QStringLiteral("actionAddMarker"));
    addVideoTrack->setObjectName(QStringLiteral("actionAddVideoTrack"));
    addAudioTrack->setObjectName(QStringLiteral("actionAddAudioTrack"));
    m_composeAction->setObjectName(QStringLiteral("actionCompose"));
    m_decomposeAction->setObjectName(QStringLiteral("actionDecompose"));

    QToolBar* bar = addToolBar(tr("Main"));
    bar->setMovable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    bar->addAction(importAction);
    bar->addAction(m_exportAction);
    bar->addSeparator();
    bar->addAction(m_undoAction);
    bar->addAction(m_redoAction);
    bar->addSeparator();
    bar->addAction(selectTool);
    bar->addAction(trimTool);
    bar->addAction(snapping);
    bar->addSeparator();
    bar->addAction(m_splitAction);
    bar->addAction(m_deleteAction);
    bar->addAction(m_rippleDeleteAction);
    bar->addSeparator();
    for (QAction* a : m_sourceEditActions)
        bar->addAction(a);
    bar->addSeparator();
    bar->addAction(addMarkerAction);
}

// ---------------------------------------------------------------------------------------------
// Model plumbing
// ---------------------------------------------------------------------------------------------

void MainWindow::onModelChanged()
{
    auto snapshot = ve::TimelineSnapshot::fromProject(m_project);
    const bool changed = !m_snapshot || m_snapshot->timeline != snapshot->timeline
        || m_snapshot->nested != snapshot->nested || m_snapshot->media.size() != snapshot->media.size();
    if (changed) {
        m_snapshot = snapshot;
        m_engine->setTimeline(snapshot);
    }
    m_pool->setMedia(m_project.media, m_project.timeline().rate);
    m_timeline->refresh();
    updateWindowState();
}

void MainWindow::updateWindowState()
{
    const QString name = m_projectPath.isEmpty() ? tr("Untitled") : QFileInfo(m_projectPath).completeBaseName();
    setWindowTitle(QStringLiteral("%1[*] - VideoEditor").arg(name));
    setWindowModified(!m_commands.isClean());

    m_undoAction->setEnabled(m_commands.canUndo());
    m_undoAction->setText(m_commands.canUndo() ? tr("&Undo %1").arg(qs(m_commands.undoText())) : tr("&Undo"));
    m_redoAction->setEnabled(m_commands.canRedo());
    m_redoAction->setText(m_commands.canRedo() ? tr("&Redo %1").arg(qs(m_commands.redoText())) : tr("&Redo"));

    const ve::Timeline& tl = m_project.timeline();
    const bool hasClips = tl.hasClips();
    const std::vector<ve::Id> selected = m_timeline->selectedClips();
    bool compoundSelected = false;
    for (const ve::Track& t : tl.tracks) {
        for (const ve::PlacedItem& p : t.clips())
            compoundSelected |= std::holds_alternative<ve::CompoundClip>(*p.item)
                && std::find(selected.begin(), selected.end(), p.clip->id) != selected.end();
    }
    m_splitAction->setEnabled(hasClips);
    m_deleteAction->setEnabled(!selected.empty() || !m_timeline->selectedMarker().empty());
    m_rippleDeleteAction->setEnabled(!selected.empty());
    m_composeAction->setEnabled(!selected.empty());
    m_decomposeAction->setEnabled(compoundSelected);
    const bool hasSource = !m_pool->sourceMediaId().isEmpty();
    for (QAction* a : m_sourceEditActions)
        a->setEnabled(hasSource);
    m_exportAction->setEnabled(hasClips && !m_exporting);
    m_timeline->setMarks(m_markIn, m_markOut);

    m_duration->setText(qs(ve::formatTimecode(tl.duration(), tl.rate)));
    m_status->setText(tr("Timeline %1x%2 @ %3 fps    GPU: %4")
                          .arg(tl.width)
                          .arg(tl.height)
                          .arg(tl.rate.toDouble(), 0, 'f', 3)
                          .arg(m_gpuBackend.isEmpty() ? QStringLiteral("-") : m_gpuBackend));
}

void MainWindow::updateTransport()
{
    m_playButton->setIcon(style()->standardIcon(m_lastPlaying ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
}

void MainWindow::tick()
{
    bool changed = false;
    ve::VideoFramePtr frame = m_engine->currentVideoFrame(&changed);
    if (changed)
        m_viewer->setFrame(std::move(frame));
    if (m_engine->isPlaying() && m_engine->reachedEnd())
        m_engine->pause();

    const bool playing = m_engine->isPlaying();
    const Time pos = m_engine->position();
    if (pos != m_lastPosition) {
        m_lastPosition = pos;
        m_timeline->setPlayhead(pos, playing);
        m_timecode->setText(qs(ve::formatTimecode(pos, m_project.timeline().rate)));
    }
    if (playing != m_lastPlaying) {
        m_lastPlaying = playing;
        updateTransport();
    }
}

bool MainWindow::edit(const QString& name, ve::TimelineEditCommand::Operation op, const char* mergeKey)
{
    return m_commands.edit(utf8(name), std::move(op), mergeKey ? mergeKey : "");
}

// ---------------------------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------------------------

void MainWindow::seekTo(Time t)
{
    m_engine->seek(std::max(t, Time(0)));
}

void MainWindow::seekToFrame(std::int64_t frame)
{
    seekTo(ve::timeOfFrame(std::max<std::int64_t>(0, frame), m_project.timeline().rate));
}

Time MainWindow::playheadPosition() const
{
    return m_engine->position();
}

bool MainWindow::isPlaying() const
{
    return m_engine->isPlaying();
}

void MainWindow::play()
{
    if (m_project.timeline().hasClips())
        m_engine->play();
}

void MainWindow::togglePlay()
{
    if (m_engine->isPlaying())
        m_engine->pause();
    else
        play();
}

void MainWindow::stepFrames(std::int64_t frames)
{
    if (m_engine->isPlaying())
        m_engine->pause();
    const ve::Rate rate = m_project.timeline().rate;
    seekToFrame(ve::frameAt(m_engine->position(), rate) + frames);
}

// ---------------------------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------------------------

void MainWindow::splitAtPlayhead()
{
    const Time t = m_engine->position();
    const std::vector<ve::Id> selected = m_timeline->selectedClips();
    if (!edit(tr("Split"), [t, selected](ve::Timeline& tl, const ve::MediaPool&) { return ve::edit::splitAt(tl, t, selected); }))
        statusBar()->showMessage(tr("Nothing to split under the playhead"), 3000);
}

void MainWindow::deleteSelection()
{
    // A selected marker goes first; clips are deleted when no marker is selected.
    const ve::Id marker = m_timeline->selectedMarker();
    if (!marker.empty()) {
        edit(tr("Delete Marker"), [marker](ve::Timeline& tl, const ve::MediaPool&) { return ve::edit::removeMarker(tl, marker); });
        return;
    }
    const std::vector<ve::Id> selected = m_timeline->selectedClips();
    if (selected.empty())
        return;
    edit(tr("Delete"), [selected](ve::Timeline& tl, const ve::MediaPool&) { return ve::edit::deleteClips(tl, selected); });
}

void MainWindow::rippleDeleteSelection()
{
    const std::vector<ve::Id> selected = m_timeline->selectedClips();
    if (selected.empty())
        return;
    if (!edit(tr("Ripple Delete"), [selected](ve::Timeline& tl, const ve::MediaPool&) { return ve::edit::rippleDelete(tl, selected); }))
        statusBar()->showMessage(tr("Ripple delete refused: a sync-locked track has other clips in that time. "
                                    "Turn off its sync lock (or lock it) to leave it as it is."),
                                 6000);
}

void MainWindow::placeMediaAt(const QString& mediaId, std::optional<Time> at, const QString& trackId)
{
    const ve::MediaItem* found = m_project.media.find(utf8(mediaId));
    if (!found)
        return;
    const ve::MediaItem media = *found;
    const bool conform = !m_project.timeline().hasClips() && media.hasVideo;
    const ve::Track* row = m_project.timeline().track(utf8(trackId));
    if (row && at) {
        // Dropped on a track: overwrite there, the other kind on the track of the same number.
        ve::edit::EditPoints p;
        p.mediaId = media.id;
        p.playhead = *at;
        const int index = ve::edit::kindIndex(m_project.timeline(), row->id);
        for (ve::TrackKind kind : {ve::TrackKind::Video, ve::TrackKind::Audio}) {
            const ve::Track* t = kind == row->kind ? row : ve::edit::trackOfKind(m_project.timeline(), kind, index);
            if (!t)
                t = m_project.timeline().track(m_timeline->destinationTrack(kind));
            (kind == ve::TrackKind::Video ? p.videoTrack : p.audioTrack) = t && !t->locked ? t->id : ve::Id{};
        }
        if (!edit(tr("Place %1").arg(qs(media.name)), [media, p, conform](ve::Timeline& tl, const ve::MediaPool& pool) {
                if (conform)
                    conformTimeline(tl, media);
                return !ve::edit::overwriteEdit(tl, pool, p).empty();
            }))
            statusBar()->showMessage(tr("That track is locked"), 3000);
    } else {
        edit(tr("Place %1").arg(qs(media.name)), [media, at, conform](ve::Timeline& tl, const ve::MediaPool&) {
            if (conform)
                conformTimeline(tl, media);
            return !ve::edit::placeMedia(tl, media, at).empty();
        });
    }
    if (conform)
        m_timeline->zoomToFit();
}

void MainWindow::sourceEdit(SourceEdit kind)
{
    const ve::MediaItem* found = m_project.media.find(utf8(m_pool->sourceMediaId()));
    if (!found) {
        statusBar()->showMessage(tr("Select the media to edit in the media pool first"), 4000);
        return;
    }
    const ve::MediaItem media = *found;
    ve::edit::EditPoints p;
    p.mediaId = media.id;
    p.playhead = m_engine->position();
    p.recordIn = m_markIn;
    p.recordOut = m_markOut;
    p.videoTrack = m_timeline->destinationTrack(ve::TrackKind::Video);
    p.audioTrack = m_timeline->destinationTrack(ve::TrackKind::Audio);
    const bool conform = !m_project.timeline().hasClips() && media.hasVideo;

    QString name;
    switch (kind) {
    case SourceEdit::Insert: name = tr("Insert %1"); break;
    case SourceEdit::Overwrite: name = tr("Overwrite %1"); break;
    case SourceEdit::Replace: name = tr("Replace with %1"); break;
    case SourceEdit::FitToFill: name = tr("Fit to Fill %1"); break;
    case SourceEdit::PlaceOnTop: name = tr("Place %1 on Top"); break;
    }
    auto end = std::make_shared<std::optional<Time>>(); // where the new clips end, for the playhead
    const bool ok = edit(name.arg(qs(media.name)), [media, p, kind, conform, end](ve::Timeline& tl, const ve::MediaPool& pool) {
        if (conform)
            conformTimeline(tl, media);
        std::vector<ve::Id> ids;
        switch (kind) {
        case SourceEdit::Insert: ids = ve::edit::insertEdit(tl, pool, p); break;
        case SourceEdit::Overwrite: ids = ve::edit::overwriteEdit(tl, pool, p); break;
        case SourceEdit::Replace: ids = ve::edit::replaceEdit(tl, pool, p); break;
        case SourceEdit::FitToFill: ids = ve::edit::fitToFillEdit(tl, pool, p); break;
        case SourceEdit::PlaceOnTop: ids = ve::edit::placeOnTopEdit(tl, pool, p); break;
        }
        if (!ids.empty()) {
            if (auto loc = tl.locateClip(ids.front()))
                *end = loc->end();
        }
        return !ids.empty();
    });
    if (!ok) {
        statusBar()->showMessage(kind == SourceEdit::FitToFill
                                     ? tr("Fit to fill needs a timeline Out point (O) and a free destination track")
                                 : kind == SourceEdit::Replace
                                     ? tr("Replace needs a clip under the playhead on a destination track, and enough media")
                                     : tr("Edit refused: check the In/Out points, the media length and the destination tracks "
                                          "(locked ones cannot be edited)"),
                                 6000);
        return;
    }
    clearMarks();
    if (conform)
        m_timeline->zoomToFit();
    if (*end && kind != SourceEdit::Replace)
        seekTo(**end); // like other editors, the playhead moves past the new clip
}

bool MainWindow::editProject(const QString& name, ve::ProjectEditCommand::Operation op)
{
    return m_commands.editProject(utf8(name), std::move(op));
}

void MainWindow::composeSelection()
{
    const std::vector<ve::Id> selected = m_timeline->selectedClips();
    if (selected.empty())
        return;
    if (!editProject(tr("New Compound Clip"), [selected](ve::Project& p) { return ve::edit::composeClips(p, selected).has_value(); }))
        statusBar()->showMessage(tr("Cannot make a compound clip: other clips on those tracks overlap the selection"), 6000);
}

void MainWindow::decomposeSelection()
{
    for (const ve::Id& id : m_timeline->selectedClips()) {
        auto loc = m_project.timeline().locateClip(id);
        if (loc && std::holds_alternative<ve::CompoundClip>(loc->item)) {
            if (!editProject(tr("Decompose Compound Clip"), [id](ve::Project& p) { return ve::edit::decomposeClip(p, id); }))
                statusBar()->showMessage(tr("Cannot decompose: a track it needs is locked"), 5000);
            return;
        }
    }
}

void MainWindow::addTrack(ve::TrackKind kind)
{
    edit(kind == ve::TrackKind::Video ? tr("Add Video Track") : tr("Add Audio Track"),
         [kind](ve::Timeline& tl, const ve::MediaPool&) { return !ve::edit::addTrack(tl, kind).empty(); });
}

void MainWindow::markIn()
{
    m_markIn = ve::floorToFrame(m_engine->position(), m_project.timeline().rate);
    if (m_markOut && *m_markOut <= *m_markIn)
        m_markOut.reset();
    updateWindowState();
}

void MainWindow::markOut()
{
    // The Out includes the frame under the playhead.
    const ve::Rate rate = m_project.timeline().rate;
    m_markOut = ve::timeOfFrame(ve::frameAt(m_engine->position(), rate) + 1, rate);
    if (m_markIn && *m_markIn >= *m_markOut)
        m_markIn.reset();
    updateWindowState();
}

void MainWindow::clearMarks()
{
    m_markIn.reset();
    m_markOut.reset();
    updateWindowState();
}

void MainWindow::addMarker()
{
    const Time at = m_engine->position();
    const QString name = tr("Marker %1").arg(m_project.timeline().markers.size() + 1);
    if (!edit(tr("Add Marker"), [at, name](ve::Timeline& tl, const ve::MediaPool&) {
            return !ve::edit::addMarker(tl, at, utf8(name)).empty();
        }))
        statusBar()->showMessage(tr("There is already a marker on this frame"), 3000);
}

void MainWindow::jumpToEdit(int direction)
{
    const ve::Timeline& tl = m_project.timeline();
    const Time now = ve::floorToFrame(m_engine->position(), tl.rate);
    const std::vector<Time> points = ve::edit::editPoints(tl);
    if (direction > 0) {
        auto it = std::upper_bound(points.begin(), points.end(), now);
        if (it != points.end())
            seekTo(*it);
    } else {
        auto it = std::lower_bound(points.begin(), points.end(), now);
        if (it != points.begin())
            seekTo(*std::prev(it));
    }
}

void MainWindow::jumpToMarker(int direction)
{
    const ve::Timeline& tl = m_project.timeline();
    const Time now = ve::floorToFrame(m_engine->position(), tl.rate);
    if (direction > 0) {
        for (const ve::Marker& m : tl.markers) {
            if (m.start > now) {
                seekTo(m.start);
                return;
            }
        }
    } else {
        for (auto it = tl.markers.rbegin(); it != tl.markers.rend(); ++it) {
            if (it->start < now) {
                seekTo(it->start);
                return;
            }
        }
    }
}

void MainWindow::importFiles(const QStringList& paths, std::optional<Time> at)
{
    if (paths.isEmpty())
        return;
    QStringList errors;
    std::vector<ve::MediaItem> probed;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (const QString& path : paths) {
        std::string error;
        if (auto media = ve::probeMedia(utf8(QFileInfo(path).absoluteFilePath()), &error))
            probed.push_back(std::move(*media));
        else
            errors << QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), qs(error));
    }
    QApplication::restoreOverrideCursor();

    if (!probed.empty()) {
        const bool wasEmpty = !m_project.timeline().hasClips();
        const QString name = probed.size() == 1 ? tr("Import %1").arg(qs(probed.front().name))
                                                : tr("Import %1 files").arg(probed.size());
        // One transaction: add to the pool, conform the timeline once, place every file.
        m_commands.beginTransaction(utf8(name));
        auto cursor = std::make_shared<std::optional<Time>>(at);
        bool conformPending = wasEmpty;
        for (const ve::MediaItem& media : probed) {
            m_commands.push(std::make_unique<ve::AddMediaCommand>(media));
            const bool conform = conformPending && media.hasVideo;
            if (conform)
                conformPending = false;
            m_commands.edit("Place", [media, cursor, conform](ve::Timeline& tl, const ve::MediaPool&) {
                if (conform)
                    conformTimeline(tl, media);
                const auto ids = ve::edit::placeMedia(tl, media, *cursor);
                if (ids.empty())
                    return false;
                if (*cursor)
                    *cursor = **cursor + ve::floorToFrame(media.duration, tl.rate);
                return true;
            });
        }
        m_commands.commit();
        if (wasEmpty && m_project.timeline().hasClips()) {
            m_timeline->zoomToFit();
            seekTo(Time(0));
        }
    }
    if (!errors.isEmpty())
        QMessageBox::warning(this, tr("Import"), tr("Some files could not be imported:\n\n%1").arg(errors.join('\n')));
}

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------

bool MainWindow::maybeSave()
{
    if (m_commands.isClean())
        return true;
    const auto answer = QMessageBox::warning(this, tr("Unsaved changes"), tr("Save the changes to this project?"),
                                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Save)
        return save();
    return answer == QMessageBox::Discard;
}

void MainWindow::newProject()
{
    if (!maybeSave())
        return;
    m_engine->pause();
    m_project = ve::makeEmptyProject();
    m_projectPath.clear();
    m_snapshot.reset();
    m_timeline->setProject(&m_project);
    m_commands.clear();
    seekTo(Time(0));
}

void MainWindow::openProject()
{
    if (!maybeSave())
        return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Open project"), QFileInfo(m_projectPath).absolutePath(),
                                                      projectFilter());
    if (!path.isEmpty())
        openProjectFile(path);
}

bool MainWindow::openProjectFile(const QString& path)
{
    std::string error;
    auto loaded = ve::loadProject(utf8(QFileInfo(path).absoluteFilePath()), &error);
    if (!loaded) {
        QMessageBox::critical(this, tr("Open project"), tr("Cannot open %1:\n%2").arg(path, qs(error)));
        return false;
    }
    m_engine->pause();
    m_project = std::move(*loaded);
    m_projectPath = QFileInfo(path).absoluteFilePath();
    m_snapshot.reset();
    m_timeline->setProject(&m_project);
    m_commands.clear();
    m_timeline->zoomToFit();
    seekTo(Time(0));

    QStringList missing;
    for (const ve::MediaItem& m : m_project.media.items) {
        if (!QFileInfo::exists(qs(m.path)))
            missing << qs(m.path);
    }
    if (!missing.isEmpty())
        QMessageBox::warning(this, tr("Missing media"), tr("These files were not found:\n\n%1").arg(missing.join('\n')));
    return true;
}

bool MainWindow::save()
{
    return m_projectPath.isEmpty() ? saveAs() : saveTo(m_projectPath);
}

bool MainWindow::saveAs()
{
    const QString start = m_projectPath.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/Untitled.veproj")
        : m_projectPath;
    QString path = QFileDialog::getSaveFileName(this, tr("Save project"), start, projectFilter());
    if (path.isEmpty())
        return false;
    if (!path.endsWith(QStringLiteral(".veproj"), Qt::CaseInsensitive))
        path += QStringLiteral(".veproj");
    return saveTo(path);
}

bool MainWindow::saveTo(const QString& path)
{
    std::string error;
    if (!ve::saveProject(m_project, utf8(QFileInfo(path).absoluteFilePath()), &error)) {
        QMessageBox::critical(this, tr("Save project"), tr("Cannot save %1:\n%2").arg(path, qs(error)));
        return false;
    }
    m_projectPath = QFileInfo(path).absoluteFilePath();
    m_commands.setClean();
    statusBar()->showMessage(tr("Saved %1").arg(path), 4000);
    return true;
}

void MainWindow::importMedia()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, tr("Import media"), QStandardPaths::writableLocation(QStandardPaths::MoviesLocation), mediaFilter());
    importFiles(paths);
}

// ---------------------------------------------------------------------------------------------
// Export (runs on a worker thread; the engine renders from a snapshot)
// ---------------------------------------------------------------------------------------------

void MainWindow::exportTimeline()
{
    if (m_exporting || !m_project.timeline().hasClips())
        return;
    m_engine->pause();

    const QString suggested = m_projectPath.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + QStringLiteral("/export.mp4")
        : QFileInfo(m_projectPath).absolutePath() + QLatin1Char('/') + QFileInfo(m_projectPath).completeBaseName()
            + QStringLiteral(".mp4");
    ExportDialog dialog(m_project.timeline(), suggested, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const ve::ExportSettings settings = dialog.settings();
    const QString outPath = qs(settings.outputPath);
    if (QFileInfo::exists(outPath)
        && QMessageBox::question(this, tr("Export"), tr("%1 already exists. Replace it?").arg(outPath)) != QMessageBox::Yes)
        return;

    auto snapshot = ve::TimelineSnapshot::fromProject(m_project);
    auto* progress = new QProgressDialog(tr("Exporting %1...").arg(QFileInfo(outPath).fileName()), tr("Cancel"), 0, 1000, this);
    progress->setWindowTitle(tr("Export"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    connect(progress, &QProgressDialog::canceled, this, [cancel] { cancel->store(true); });

    m_exporting = true;
    updateWindowState();
    QThread* worker = QThread::create([this, snapshot, settings, cancel, progress, outPath] {
        int lastPermille = -1;
        const ve::ExportResult result = ve::exportTimeline(*snapshot, settings, [&](double p) {
            const int permille = int(p * 1000);
            if (permille != lastPermille) { // throttle UI updates
                lastPermille = permille;
                QMetaObject::invokeMethod(progress, [progress, permille] { progress->setValue(permille); },
                                          Qt::QueuedConnection);
            }
            return !cancel->load();
        });
        QMetaObject::invokeMethod(this, [this, result, progress, outPath] {
            progress->close();
            progress->deleteLater();
            onExportFinished(result, outPath);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MainWindow::onExportFinished(const ve::ExportResult& result, const QString& path)
{
    m_exporting = false;
    updateWindowState();
    if (result.ok) {
        QMessageBox::information(this, tr("Export finished"),
                                 tr("Wrote %1 frames to\n%2\n\nVideo encoder: %3")
                                     .arg(result.framesWritten)
                                     .arg(path, qs(result.videoEncoder)));
    } else if (result.cancelled) {
        statusBar()->showMessage(tr("Export cancelled"), 4000);
    } else {
        QMessageBox::critical(this, tr("Export failed"), qs(result.error));
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_exporting) {
        QMessageBox::information(this, tr("Export running"), tr("Wait for the export to finish or cancel it first."));
        event->ignore();
        return;
    }
    if (!maybeSave()) {
        event->ignore();
        return;
    }
    m_engine->pause();
    event->accept();
}
