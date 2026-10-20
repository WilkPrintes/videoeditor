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

#include <atomic>
#include <cmath>

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
    connect(m_timeline, &TimelineWidget::moveRequested, this, [this](const QStringList& ids, Time delta) {
        std::vector<ve::Id> clips;
        for (const QString& id : ids)
            clips.push_back(utf8(id));
        edit(tr("Move"), [clips, delta](ve::Timeline& tl, const ve::MediaPool&) {
            return ve::edit::moveClips(tl, clips, delta);
        }, kGestureMergeKey);
    });
    connect(m_timeline, &TimelineWidget::trimRequested, this, [this](const QString& id, bool head, Time edge) {
        const ve::Id clip = utf8(id);
        edit(tr("Trim"), [clip, head, edge](ve::Timeline& tl, const ve::MediaPool& pool) {
            return ve::edit::trimClip(tl, pool, clip, head ? ve::edit::Edge::Head : ve::edit::Edge::Tail, edge);
        }, kGestureMergeKey);
    });
    connect(m_timeline, &TimelineWidget::mediaDropped, this,
            [this](const QString& mediaId, Time at) { placeMediaAt(mediaId, at); });
    connect(m_timeline, &TimelineWidget::filesDropped, this,
            [this](const QStringList& paths, Time at) { importFiles(paths, at); });
    connect(m_timeline, &TimelineWidget::selectionChanged, this, &MainWindow::updateWindowState);
    connect(m_pool, &MediaPoolWidget::mediaActivated, this,
            [this](const QString& id) { placeMediaAt(id, std::nullopt); });
    connect(m_pool, &MediaPoolWidget::filesDropped, this, [this](const QStringList& paths) { importFiles(paths); });
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
    playPause->setObjectName(QStringLiteral("actionPlayPause"));

    QToolBar* bar = addToolBar(tr("Main"));
    bar->setMovable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    bar->addAction(importAction);
    bar->addAction(m_exportAction);
    bar->addSeparator();
    bar->addAction(m_undoAction);
    bar->addAction(m_redoAction);
    bar->addSeparator();
    bar->addAction(m_splitAction);
    bar->addAction(m_deleteAction);
}

// ---------------------------------------------------------------------------------------------
// Model plumbing
// ---------------------------------------------------------------------------------------------

void MainWindow::onModelChanged()
{
    auto snapshot = ve::TimelineSnapshot::fromProject(m_project);
    const bool changed = !m_snapshot || m_snapshot->timeline != snapshot->timeline
        || m_snapshot->media.size() != snapshot->media.size();
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
    m_splitAction->setEnabled(hasClips);
    m_deleteAction->setEnabled(!m_timeline->selectedClips().empty());
    m_exportAction->setEnabled(hasClips && !m_exporting);

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
    const std::vector<ve::Id> selected = m_timeline->selectedClips();
    if (selected.empty())
        return;
    edit(tr("Delete"), [selected](ve::Timeline& tl, const ve::MediaPool&) { return ve::edit::deleteClips(tl, selected); });
}

void MainWindow::placeMediaAt(const QString& mediaId, std::optional<Time> at)
{
    const ve::MediaItem* found = m_project.media.find(utf8(mediaId));
    if (!found)
        return;
    const ve::MediaItem media = *found;
    const bool conform = !m_project.timeline().hasClips() && media.hasVideo;
    edit(tr("Place %1").arg(qs(media.name)), [media, at, conform](ve::Timeline& tl, const ve::MediaPool&) {
        if (conform)
            conformTimeline(tl, media);
        return !ve::edit::placeMedia(tl, media, at).empty();
    });
    if (conform)
        m_timeline->zoomToFit();
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
