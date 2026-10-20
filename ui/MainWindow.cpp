#include "ui/MainWindow.h"

#include "core/Edit.h"
#include "core/ProjectFile.h"
#include "engine/Exporter.h"
#include "engine/PlaybackEngine.h"
#include "media/MediaProbe.h"
#include "ui/EditIndexWidget.h"
#include "ui/ExportDialog.h"
#include "ui/InspectorWidget.h"
#include "ui/LabelColors.h"
#include "ui/MediaPoolWidget.h"
#include "ui/SourceViewerPanel.h"
#include "ui/TimelineWidget.h"
#include "ui/Updater.h"
#include "ui/ViewerWidget.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QThread>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <set>

using ve::Time;

namespace {

constexpr const char* kGestureMergeKey = "timeline-gesture";
constexpr int kLayoutVersion = 3; // of saveState(); bump when the docks change

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

std::vector<ve::Id> ids(const QStringList& list)
{
    std::vector<ve::Id> out;
    for (const QString& id : list)
        out.push_back(utf8(id));
    return out;
}

// Layouts live in an ini file of their own (QStandardPaths test mode keeps the tests out of it).
std::unique_ptr<QSettings> layoutSettings()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return std::make_unique<QSettings>(dir + QStringLiteral("/layouts.ini"), QSettings::IniFormat);
}

// Preset names may hold any character; QSettings keys may not hold '/' or '\'.
QString presetKey(const QString& name)
{
    return QString::fromLatin1(name.toUtf8().toPercentEncoding());
}

QString presetName(const QString& key)
{
    return QString::fromUtf8(QByteArray::fromPercentEncoding(key.toLatin1()));
}

const ve::ClipBase* findClipBase(const ve::Timeline& tl, const ve::Id& clipId)
{
    for (const ve::Track& t : tl.tracks) {
        if (auto placed = t.findClip(clipId))
            return placed->clip;
    }
    return nullptr;
}

const ve::Item* findItem(const ve::Timeline& tl, const ve::Id& clipId)
{
    for (const ve::Track& t : tl.tracks) {
        if (auto placed = t.findClip(clipId))
            return placed->item;
    }
    return nullptr;
}

// A submenu with one checkable action per colour (and "None" first when `withNone`).
void addColorActions(QMenu* menu, const std::vector<labels::Named>& colors, const QString& current, bool withNone,
                     const QString& objectPrefix, const std::function<void(const QString&)>& choose)
{
    auto* group = new QActionGroup(menu);
    auto addOne = [&](const QString& id, const QString& name, const QIcon& icon) {
        QAction* a = menu->addAction(icon, name);
        a->setCheckable(true);
        a->setChecked(id == current);
        a->setObjectName(objectPrefix + id);
        group->addAction(a);
        QObject::connect(a, &QAction::triggered, menu, [choose, id] { choose(id); });
    };
    if (withNone)
        addOne({}, QObject::tr("None"), {});
    for (const labels::Named& c : colors)
        addOne(qs(c.id), c.name, labels::swatch(c.color));
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_engine(std::make_unique<ve::PlaybackEngine>())
{
    m_project = ve::makeEmptyProject();
    buildUi();
    buildDocks();
    buildActions();
    connectPanels();
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
    // The panels outlive the members (QWidget deletes children last); their last signals (a dock
    // hidden, a selection cleared) must not reach this window any more.
    m_viewer->removeEventFilter(this);
    m_timeline->removeEventFilter(this);
    for (QObject* child : findChildren<QObject*>())
        disconnect(child, nullptr, this, nullptr);
}

// ---------------------------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------------------------

void MainWindow::buildUi()
{
    m_pool = new MediaPoolWidget;
    m_viewer = new ViewerWidget;
    m_timeline = new TimelineWidget;
    m_source = new SourceViewerPanel;
    m_inspector = new InspectorWidget;
    m_index = new EditIndexWidget;

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
    // The buttons under the timeline viewer drive the timeline; the source viewer has its own.
    auto* toStart = makeButton(QStyle::SP_MediaSkipBackward, tr("Go to start (Home)"), [this] { seekTo(Time(0)); });
    auto* prev = makeButton(QStyle::SP_MediaSeekBackward, tr("Previous frame (Left)"), [this] { stepFrames(-1); });
    m_playButton = makeButton(QStyle::SP_MediaPlay, tr("Play / pause (Space)"), [this] {
        setActiveViewer(false);
        togglePlay();
    });
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

    // The timeline viewer is the central widget; every other panel is a dock around it.
    m_viewerTitle = new QLabel;
    m_viewerTitle->setContentsMargins(10, 4, 10, 2);
    m_viewerPanel = new QFrame;
    m_viewerPanel->setObjectName(QStringLiteral("timelineViewerPanel"));
    m_viewerPanel->setStyleSheet(QStringLiteral("QFrame#timelineViewerPanel { border: 2px solid transparent; }"
                                                "QFrame#timelineViewerPanel[active=\"true\"] { border: 2px solid rgb(240, 196, 76); }"));
    auto* viewerLayout = new QVBoxLayout(m_viewerPanel);
    viewerLayout->setContentsMargins(2, 2, 2, 2);
    viewerLayout->setSpacing(0);
    viewerLayout->addWidget(m_viewerTitle);
    viewerLayout->addWidget(m_viewer, 1);
    viewerLayout->addWidget(transport);
    setCentralWidget(m_viewerPanel);

    m_status = new QLabel;
    statusBar()->addPermanentWidget(m_status);
    // The update button (its action is made in buildActions).
    auto* updateButton = new QToolButton;
    updateButton->setObjectName(QStringLiteral("updateButton"));
    updateButton->setAutoRaise(true);
    updateButton->setFocusPolicy(Qt::NoFocus);
    updateButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    statusBar()->addPermanentWidget(updateButton);

    // A click in a viewer (or in the timeline) makes it the active viewer.
    m_viewer->installEventFilter(this);
    m_timeline->installEventFilter(this);
}

void MainWindow::buildDocks()
{
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks
                   | QMainWindow::GroupedDragging);
    // The timeline spans the whole width under the viewers, as on the Edit page of other editors.
    setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);

    // Timeline panel: one tab per timeline of the project, "+" for a new one, the track view.
    m_timelineTabs = new QTabBar;
    m_timelineTabs->setObjectName(QStringLiteral("timelineTabs"));
    m_timelineTabs->setDocumentMode(true);
    m_timelineTabs->setExpanding(false);
    m_timelineTabs->setDrawBase(false);
    m_timelineTabs->setFocusPolicy(Qt::NoFocus);
    m_timelineTabs->setContextMenuPolicy(Qt::CustomContextMenu);
    auto* newTab = new QToolButton;
    newTab->setObjectName(QStringLiteral("newTimelineButton"));
    newTab->setText(QStringLiteral("+"));
    newTab->setToolTip(tr("New Timeline (Ctrl+Alt+N)"));
    newTab->setAutoRaise(true);
    newTab->setFocusPolicy(Qt::NoFocus);
    connect(newTab, &QToolButton::clicked, this, [this] { newTimeline(); });
    auto* tabRow = new QHBoxLayout;
    tabRow->setContentsMargins(4, 0, 4, 0);
    tabRow->setSpacing(2);
    tabRow->addWidget(m_timelineTabs);
    tabRow->addWidget(newTab);
    tabRow->addStretch(1);
    auto* timelinePanel = new QWidget;
    auto* timelineLayout = new QVBoxLayout(timelinePanel);
    timelineLayout->setContentsMargins(0, 0, 0, 0);
    timelineLayout->setSpacing(0);
    timelineLayout->addLayout(tabRow);
    timelineLayout->addWidget(m_timeline, 1);

    auto dock = [this](const QString& title, const char* name, QWidget* widget) {
        auto* d = new QDockWidget(title, this);
        d->setObjectName(QLatin1String(name));
        d->setWidget(widget);
        return d;
    };
    m_poolDock = dock(tr("Media Pool"), "dockMediaPool", m_pool);
    m_indexDock = dock(tr("Edit Index"), "dockEditIndex", m_index);
    m_sourceDock = dock(tr("Source Viewer"), "dockSourceViewer", m_source);
    m_inspectorDock = dock(tr("Inspector"), "dockInspector", m_inspector);
    m_timelineDock = dock(tr("Timeline"), "dockTimeline", timelinePanel);
    applyDefaultLayout();
}

void MainWindow::applyDefaultLayout()
{
    const QList<QDockWidget*> docks = {m_poolDock, m_indexDock, m_sourceDock, m_inspectorDock, m_timelineDock};
    for (QDockWidget* d : docks) {
        d->setFloating(false);
        removeDockWidget(d);
    }
    // Media pool (with the edit index behind it) | source viewer | timeline viewer | inspector,
    // the timeline underneath.
    addDockWidget(Qt::LeftDockWidgetArea, m_poolDock);
    splitDockWidget(m_poolDock, m_sourceDock, Qt::Horizontal);
    tabifyDockWidget(m_poolDock, m_indexDock);
    addDockWidget(Qt::RightDockWidgetArea, m_inspectorDock);
    addDockWidget(Qt::BottomDockWidgetArea, m_timelineDock);
    for (QDockWidget* d : docks)
        d->show();
    if (isVisible())
        sizeDefaultDocks();
}

void MainWindow::sizeDefaultDocks()
{
    // The two viewers share what the pool and the inspector leave.
    const int pool = 280;
    const int inspector = 300;
    const int viewers = std::max(400, width() - pool - inspector);
    resizeDocks({m_poolDock, m_sourceDock, m_inspectorDock}, {pool, viewers / 2, inspector}, Qt::Horizontal);
    resizeDocks({m_timelineDock}, {std::max(260, int(height() * 0.42))}, Qt::Vertical);
    m_poolDock->raise(); // over the edit index (a raise before the window is shown is lost)
}

void MainWindow::connectPanels()
{
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
    connect(m_timeline, &TimelineWidget::moveRequested, this, [this](const QStringList& clipIds, Time delta, int shift) {
        const std::vector<ve::Id> clips = ids(clipIds);
        const bool linked = linkedSelection();
        edit(tr("Move"), [clips, delta, shift, linked](ve::Timeline& tl, const ve::MediaPool&) {
            return ve::edit::moveClips(tl, clips, delta, shift, linked);
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
    connect(m_timeline, &TimelineWidget::selectionChanged, this, [this] {
        updateWindowState();
        refreshPanels();
    });
    connect(m_timeline, &TimelineWidget::contextMenuRequested, this,
            [this](const TimelineWidget::Context& context, const QPoint& globalPos) {
        setActiveViewer(false);
        if (auto menu = contextMenu(context); menu && !menu->isEmpty())
            menu->exec(globalPos);
    });
    connect(m_timeline, &TimelineWidget::openCompoundRequested, this, &MainWindow::openCompound);

    // Media pool: a double-click opens the media in the source viewer.
    connect(m_pool, &MediaPoolWidget::mediaActivated, this, &MainWindow::openInSourceViewer);
    connect(m_pool, &MediaPoolWidget::filesDropped, this, [this](const QStringList& paths) { importFiles(paths); });
    connect(m_pool, &QListWidget::itemSelectionChanged, this, &MainWindow::updateWindowState);

    // Source viewer.
    connect(m_source, &SourceViewerPanel::activated, this, [this] { setActiveViewer(true); });
    connect(m_source, &SourceViewerPanel::marksChanged, this, &MainWindow::updateWindowState);
    connect(m_sourceDock->toggleViewAction(), &QAction::toggled, this, [this](bool shown) {
        if (!shown)
            setActiveViewer(false);
    });

    // Timeline tabs.
    connect(m_timelineTabs, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0)
            openTimeline(utf8(m_timelineTabs->tabData(index).toString()));
    });
    connect(m_timelineTabs, &QTabBar::customContextMenuRequested, this, [this](const QPoint& pos) {
        const int index = m_timelineTabs->tabAt(pos);
        if (index < 0)
            return;
        openTimeline(utf8(m_timelineTabs->tabData(index).toString()));
        QMenu menu;
        menu.addAction(tr("Rename Timeline..."), this, [this] { renameTimeline(); });
        menu.addAction(tr("Duplicate Timeline"), this, [this] { duplicateTimeline(); });
        menu.addSeparator();
        menu.addAction(tr("Delete Timeline"), this, [this] { deleteTimeline(); });
        menu.exec(m_timelineTabs->mapToGlobal(pos));
    });

    // Edit index: a row selects that clip or marker and moves the playhead to it.
    connect(m_indexDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible)
            m_index->setTimeline(m_project.timeline(), m_project.media);
    });
    connect(m_index, &EditIndexWidget::clipActivated, this, [this](const QString& id) {
        const ve::Id clip = utf8(id);
        m_timeline->selectClips({clip});
        for (const ve::Track& t : m_project.timeline().tracks) {
            if (auto placed = t.findClip(clip))
                seekTo(placed->start);
        }
        setActiveViewer(false);
    });
    connect(m_index, &EditIndexWidget::markerActivated, this, [this](const QString& id) {
        const ve::Id marker = utf8(id);
        m_timeline->selectMarker(marker);
        if (const ve::Marker* m = ve::edit::findMarker(m_project.timeline(), marker))
            seekTo(m->start);
        setActiveViewer(false);
    });

    // Inspector: every change is one undoable edit.
    connect(m_inspector, &InspectorWidget::timelineRenamed, this, [this](const QString& name) {
        const ve::Id id = m_project.currentTimeline;
        if (!editProject(tr("Rename Timeline"), [id, name](ve::Project& p) { return ve::edit::renameTimeline(p, id, utf8(name)); }))
            refreshPanels(); // back to the name it has
    });
    connect(m_inspector, &InspectorWidget::clipsRenamed, this, [this](const QStringList& clipIds, const QString& name) {
        const std::vector<ve::Id> clips = ids(clipIds);
        edit(tr("Rename Clip"), [clips, name](ve::Timeline& tl, const ve::MediaPool&) {
            bool changed = false;
            for (const ve::Id& id : clips)
                changed |= ve::edit::renameClip(tl, id, utf8(name));
            return changed;
        });
    });
    connect(m_inspector, &InspectorWidget::clipColorChosen, this, [this](const QStringList& clipIds, const QString& color) {
        const std::vector<ve::Id> clips = ids(clipIds);
        edit(tr("Clip Color"), [clips, color](ve::Timeline& tl, const ve::MediaPool&) {
            return ve::edit::setClipColor(tl, clips, utf8(color));
        });
    });
    connect(m_inspector, &InspectorWidget::clipFlagToggled, this,
            [this](const QStringList& clipIds, const QString& color, bool on) {
        const std::vector<ve::Id> clips = ids(clipIds);
        edit(on ? tr("Add Flag") : tr("Remove Flag"), [clips, color, on](ve::Timeline& tl, const ve::MediaPool&) {
            return ve::edit::setClipFlag(tl, clips, utf8(color), on);
        });
    });
    connect(m_inspector, &InspectorWidget::clipsEnabledChanged, this, [this](const QStringList& clipIds, bool on) {
        const std::vector<ve::Id> clips = ids(clipIds);
        edit(on ? tr("Enable Clip") : tr("Disable Clip"), [clips, on](ve::Timeline& tl, const ve::MediaPool&) {
            return ve::edit::setClipsEnabled(tl, clips, on);
        });
    });
    connect(m_inspector, &InspectorWidget::markerEdited, this, [this](const ve::Marker& marker) {
        if (!edit(tr("Edit Marker"), [marker](ve::Timeline& tl, const ve::MediaPool&) { return ve::edit::updateMarker(tl, marker); }))
            refreshPanels();
    });
    connect(m_inspector, &InspectorWidget::trackRenamed, this, [this](const QString& trackId, const QString& name) {
        const ve::Id track = utf8(trackId);
        if (!edit(tr("Rename Track"), [track, name](ve::Timeline& tl, const ve::MediaPool&) {
                return ve::edit::renameTrack(tl, track, utf8(name));
            })) {
            statusBar()->showMessage(tr("Another track already has that name"), 4000);
            refreshPanels();
        }
    });
    connect(m_inspector, &InspectorWidget::trackColorChosen, this, [this](const QString& trackId, const QString& color) {
        const ve::Id track = utf8(trackId);
        edit(tr("Track Color"), [track, color](ve::Timeline& tl, const ve::MediaPool&) {
            return ve::edit::setTrackColor(tl, track, utf8(color));
        });
    });
    connect(m_inspector, &InspectorWidget::trackLockChanged, this, [this](const QString& trackId, bool on) {
        const ve::Id track = utf8(trackId);
        edit(on ? tr("Lock Track") : tr("Unlock Track"),
             [track, on](ve::Timeline& tl, const ve::MediaPool&) { return ve::edit::setTrackLocked(tl, track, on); });
    });
    connect(m_inspector, &InspectorWidget::trackSyncLockChanged, this, [this](const QString& trackId, bool on) {
        const ve::Id track = utf8(trackId);
        edit(on ? tr("Sync Lock Track") : tr("Remove Sync Lock"),
             [track, on](ve::Timeline& tl, const ve::MediaPool&) { return ve::edit::setTrackSyncLock(tl, track, on); });
    });
    connect(m_inspector, &InspectorWidget::trackEnabledChanged, this, [this](const QString& trackId, bool on) {
        const ve::Id track = utf8(trackId);
        edit(on ? tr("Enable Track") : tr("Disable Track"),
             [track, on](ve::Timeline& tl, const ve::MediaPool&) { return ve::edit::setTrackEnabled(tl, track, on); });
    });
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
    m_linkedSelectionAction = add(editMenu, tr("&Linked Selection"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L)},
                                  [this](bool on) { m_timeline->setLinkedSelection(on); });
    m_linkedSelectionAction->setCheckable(true);
    m_linkedSelectionAction->setChecked(true);
    m_linkedSelectionAction->setToolTip(tr("Clicking a clip selects its linked picture or sound too (Ctrl+Shift+L)"));

    QMenu* clipMenu = menuBar()->addMenu(tr("&Clip"));
    QMenu* colorMenu = clipMenu->addMenu(tr("Clip &Color"));
    QMenu* flagMenu = clipMenu->addMenu(tr("&Flags"));
    connect(clipMenu, &QMenu::aboutToShow, this, [this, colorMenu, flagMenu] { fillLabelMenus(colorMenu, flagMenu, QStringLiteral("action")); });
    QAction* enableClip = add(clipMenu, tr("&Enable / Disable Clip"), {QKeySequence(Qt::Key_D)}, [this] { toggleSelectionEnabled(); });
    QAction* linkClips = add(clipMenu, tr("&Link / Unlink Clips"), {QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_L)}, [this] {
        // All selected clips in one group: unlink them; otherwise link them.
        const std::vector<ve::Id> selected = selection();
        std::set<ve::Id> groups;
        for (const ve::Id& id : selected) {
            if (const ve::ClipBase* c = findClipBase(m_project.timeline(), id))
                groups.insert(c->linkGroup);
        }
        if (groups.size() == 1 && !groups.begin()->empty())
            unlinkSelection();
        else
            linkSelection();
    });
    QAction* renameClip = add(clipMenu, tr("&Rename Clip..."), {QKeySequence(Qt::Key_F2)}, [this] { renameSelectionInteractive(); });
    clipMenu->addSeparator();
    QAction* swapLeft = add(clipMenu, tr("Swap with &Previous Clip"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Comma)},
                            [this] { swapSelection(-1); });
    QAction* swapRight = add(clipMenu, tr("Swap with &Next Clip"), {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Period)},
                             [this] { swapSelection(1); });
    clipMenu->addSeparator();
    m_composeAction = add(clipMenu, tr("New &Compound Clip"), {QKeySequence(Qt::ALT | Qt::Key_C)},
                          [this] { composeSelection(); });
    m_openCompoundAction = add(clipMenu, tr("&Open Compound Clip in Timeline"), {}, [this] {
        for (const ve::Id& id : selection()) {
            if (const ve::Item* item = findItem(m_project.timeline(), id); item && std::holds_alternative<ve::CompoundClip>(*item)) {
                openCompound(qs(id));
                return;
            }
        }
    });
    m_decomposeAction = add(clipMenu, tr("&Decompose Compound Clip"), {QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_C)},
                            [this] { decomposeSelection(); });
    m_selectionActions = {colorMenu->menuAction(), flagMenu->menuAction(), enableClip, linkClips, renameClip, swapLeft, swapRight};

    QMenu* timelineMenu = menuBar()->addMenu(tr("&Timeline"));
    const std::pair<SourceEdit, std::pair<QString, QList<QKeySequence>>> sourceEdits[] = {
        {SourceEdit::Insert, {tr("&Insert"), {QKeySequence(Qt::Key_F9)}}},
        {SourceEdit::Overwrite, {tr("&Overwrite"), {QKeySequence(Qt::Key_F10)}}},
        {SourceEdit::Replace, {tr("&Replace"), {QKeySequence(Qt::Key_F11)}}},
        {SourceEdit::FitToFill, {tr("&Fit to Fill"), {QKeySequence(Qt::SHIFT | Qt::Key_F11)}}},
        {SourceEdit::PlaceOnTop, {tr("Place on &Top"), {QKeySequence(Qt::Key_F12)}}},
        {SourceEdit::Append, {tr("&Append at End"), {QKeySequence(Qt::SHIFT | Qt::Key_F12)}}},
        {SourceEdit::RippleOverwrite, {tr("Ripple O&verwrite"), {QKeySequence(Qt::SHIFT | Qt::Key_F10)}}},
    };
    const char* sourceEditNames[] = {"actionInsert",    "actionOverwrite", "actionReplace",        "actionFitToFill",
                                     "actionPlaceOnTop", "actionAppend",    "actionRippleOverwrite"};
    for (std::size_t i = 0; i < std::size(sourceEdits); ++i) {
        const SourceEdit kind = sourceEdits[i].first;
        QAction* a = add(timelineMenu, sourceEdits[i].second.first, sourceEdits[i].second.second, [this, kind] { sourceEdit(kind); });
        a->setObjectName(QLatin1String(sourceEditNames[i]));
        a->setToolTip(tr("Edits the source viewer's clip (or the media selected in the pool) onto the destination "
                         "tracks, at the playhead or the In/Out"));
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
    QAction* insertGap = add(timelineMenu, tr("Insert &Gap (In to Out, or one second)"), {}, [this] { insertGapAtPlayhead(); });
    QAction* deleteGaps = add(timelineMenu, tr("Delete All &Gaps"), {}, [this] { deleteAllGaps(); });
    timelineMenu->addSeparator();
    QAction* addVideoTrack = add(timelineMenu, tr("Add &Video Track"), {QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_V)},
                                 [this] { addTrack(ve::TrackKind::Video); });
    QAction* addAudioTrack = add(timelineMenu, tr("Add &Audio Track"), {QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_A)},
                                 [this] { addTrack(ve::TrackKind::Audio); });
    timelineMenu->addSeparator();
    QAction* newTimelineAction = add(timelineMenu, tr("&New Timeline"), {QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_N)},
                                     [this] { newTimeline(); });
    QAction* duplicateTimelineAction = add(timelineMenu, tr("D&uplicate Timeline"), {}, [this] { duplicateTimeline(); });
    add(timelineMenu, tr("Rena&me Timeline..."), {}, [this] { renameTimeline(); });
    QAction* deleteTimelineAction = add(timelineMenu, tr("De&lete Timeline"), {}, [this] { deleteTimeline(); });

    QMenu* playback = menuBar()->addMenu(tr("&Playback"));
    // These go to the active viewer (the source or the timeline).
    QAction* playPause = add(playback, tr("Play / Pause"), {QKeySequence(Qt::Key_Space)}, [this] { transportToggle(); });
    add(playback, tr("Play"), {QKeySequence(Qt::Key_L)}, [this] { transportPlay(); });
    add(playback, tr("Pause"), {QKeySequence(Qt::Key_K)}, [this] { transportPause(); });
    playback->addSeparator();
    add(playback, tr("Previous Frame"), {QKeySequence(Qt::Key_Left)}, [this] { transportStep(-1); });
    add(playback, tr("Next Frame"), {QKeySequence(Qt::Key_Right)}, [this] { transportStep(1); });
    add(playback, tr("Back One Second"), {QKeySequence(Qt::SHIFT | Qt::Key_Left)},
        [this, oneSecond] { transportStep(-oneSecond()); });
    add(playback, tr("Forward One Second"), {QKeySequence(Qt::SHIFT | Qt::Key_Right)},
        [this, oneSecond] { transportStep(oneSecond()); });
    add(playback, tr("Go to Start"), {QKeySequence(Qt::Key_Home)}, [this] { transportToStart(); });
    add(playback, tr("Go to End"), {QKeySequence(Qt::Key_End)}, [this] { transportToEnd(); });

    QMenu* view = menuBar()->addMenu(tr("&View"));
    add(view, tr("Zoom In"), {QKeySequence(Qt::CTRL | Qt::Key_Equal), QKeySequence(Qt::CTRL | Qt::Key_Plus)},
        [this] { m_timeline->zoomBy(1.5); });
    add(view, tr("Zoom Out"), {QKeySequence(Qt::CTRL | Qt::Key_Minus)}, [this] { m_timeline->zoomBy(1.0 / 1.5); });
    add(view, tr("Zoom to Fit"), {QKeySequence(Qt::SHIFT | Qt::Key_Z)}, [this] { m_timeline->zoomToFit(); });

    // Workspace (spec phase 3.3): panels, the active viewer and layout presets.
    QMenu* workspace = menuBar()->addMenu(tr("&Workspace"));
    const std::pair<QDockWidget*, const char*> panels[] = {
        {m_poolDock, "actionShowMediaPool"},    {m_indexDock, "actionShowEditIndex"}, {m_sourceDock, "actionShowSourceViewer"},
        {m_inspectorDock, "actionShowInspector"}, {m_timelineDock, "actionShowTimeline"},
    };
    for (const auto& [dock, name] : panels) {
        QAction* a = dock->toggleViewAction();
        a->setObjectName(QLatin1String(name));
        workspace->addAction(a);
    }
    workspace->addSeparator();
    QAction* switchViewer = add(workspace, tr("Switch &Viewer (source / timeline)"), {QKeySequence(Qt::Key_Q)},
                                [this] { setActiveViewer(!m_sourceActive); });
    workspace->addSeparator();
    m_presetsMenu = workspace->addMenu(tr("Layout &Presets"));
    connect(m_presetsMenu, &QMenu::aboutToShow, this, &MainWindow::refreshPresetsMenu);
    QAction* resetLayoutAction = add(workspace, tr("&Reset Layout"), {}, [this] { resetLayout(); });

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    m_updateAction = add(help, tr("Check for &Updates..."), {}, [this] { checkForUpdates(); });
    m_updateAction->setObjectName(QStringLiteral("actionCheckForUpdates"));
    m_updateAction->setIconText(tr("Updates"));
    m_updateAction->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_updateAction->setToolTip(tr("Look for a newer VideoEditor release (version %1 here)").arg(QStringLiteral(VE_VERSION)));
    if (auto* button = findChild<QToolButton*>(QStringLiteral("updateButton")))
        button->setDefaultAction(m_updateAction);
    help->addSeparator();
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
    m_linkedSelectionAction->setObjectName(QStringLiteral("actionLinkedSelection"));
    enableClip->setObjectName(QStringLiteral("actionEnableClip"));
    linkClips->setObjectName(QStringLiteral("actionLinkClips"));
    renameClip->setObjectName(QStringLiteral("actionRenameClip"));
    swapLeft->setObjectName(QStringLiteral("actionSwapLeft"));
    swapRight->setObjectName(QStringLiteral("actionSwapRight"));
    markInAction->setObjectName(QStringLiteral("actionMarkIn"));
    markOutAction->setObjectName(QStringLiteral("actionMarkOut"));
    addMarkerAction->setObjectName(QStringLiteral("actionAddMarker"));
    insertGap->setObjectName(QStringLiteral("actionInsertGap"));
    deleteGaps->setObjectName(QStringLiteral("actionDeleteGaps"));
    addVideoTrack->setObjectName(QStringLiteral("actionAddVideoTrack"));
    addAudioTrack->setObjectName(QStringLiteral("actionAddAudioTrack"));
    newTimelineAction->setObjectName(QStringLiteral("actionNewTimeline"));
    duplicateTimelineAction->setObjectName(QStringLiteral("actionDuplicateTimeline"));
    deleteTimelineAction->setObjectName(QStringLiteral("actionDeleteTimeline"));
    m_composeAction->setObjectName(QStringLiteral("actionCompose"));
    m_openCompoundAction->setObjectName(QStringLiteral("actionOpenCompound"));
    m_decomposeAction->setObjectName(QStringLiteral("actionDecompose"));
    switchViewer->setObjectName(QStringLiteral("actionSwitchViewer"));
    resetLayoutAction->setObjectName(QStringLiteral("actionResetLayout"));

    QToolBar* bar = addToolBar(tr("Main"));
    bar->setObjectName(QStringLiteral("mainToolBar"));
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
    bar->addAction(m_linkedSelectionAction);
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

void MainWindow::fillLabelMenus(QMenu* colorMenu, QMenu* flagMenu, const QString& objectPrefix)
{
    // What the whole selection shares: its colour (empty when they differ) and the flags all have.
    const std::vector<ve::Id> selected = selection();
    std::set<std::string> colors;
    std::map<std::string, std::size_t> flagCount;
    for (const ve::Id& id : selected) {
        if (const ve::ClipBase* c = findClipBase(m_project.timeline(), id)) {
            colors.insert(c->color);
            for (const std::string& f : c->flags)
                ++flagCount[f];
        }
    }
    const QString current = colors.size() == 1 ? qs(*colors.begin()) : QStringLiteral("?");

    for (QMenu* menu : {colorMenu, flagMenu}) {
        qDeleteAll(menu->findChildren<QActionGroup*>(Qt::FindDirectChildrenOnly));
        menu->clear();
    }
    addColorActions(colorMenu, labels::clipColors(), current, true, objectPrefix + QStringLiteral("ClipColor:"),
                    [this](const QString& color) { setSelectionColor(color); });

    for (const labels::Named& f : labels::flagColors()) {
        const QString id = qs(f.id);
        const bool all = !selected.empty() && flagCount[f.id] == selected.size();
        QAction* a = flagMenu->addAction(labels::swatch(f.color), f.name);
        a->setCheckable(true);
        a->setChecked(all);
        a->setObjectName(objectPrefix + QStringLiteral("Flag:") + id);
        connect(a, &QAction::triggered, this, [this, id, all] { setSelectionFlag(id, !all); });
    }
    flagMenu->addSeparator();
    QAction* clear = flagMenu->addAction(tr("Clear All Flags"), this, [this] { clearSelectionFlags(); });
    clear->setObjectName(objectPrefix + QStringLiteral("ClearFlags"));
}

std::unique_ptr<QMenu> MainWindow::contextMenu(const TimelineWidget::Context& context)
{
    auto menu = std::make_unique<QMenu>(this);
    QMenu* m = menu.get();
    auto action = [m](const QString& text, const char* name, auto slot) {
        QAction* a = m->addAction(text);
        a->setObjectName(QLatin1String(name));
        QObject::connect(a, &QAction::triggered, m, slot);
        return a;
    };
    const ve::Timeline& tl = m_project.timeline();
    using Kind = TimelineWidget::Context::Kind;

    switch (context.kind) {
    case Kind::Clip: {
        const ve::Id clipId = utf8(context.clipId);
        const ve::ClipBase* clip = findClipBase(tl, clipId);
        const ve::Item* item = findItem(tl, clipId);
        if (!clip || !item)
            return {};
        action(tr("Rename Clip..."), "contextRenameClip", [this] { renameSelectionInteractive(); });
        QMenu* colorMenu = m->addMenu(tr("Clip Color"));
        QMenu* flagMenu = m->addMenu(tr("Flags"));
        fillLabelMenus(colorMenu, flagMenu, QStringLiteral("context"));
        QAction* enabled = action(tr("Enable Clip"), "contextEnableClip", [this] { toggleSelectionEnabled(); });
        enabled->setCheckable(true);
        enabled->setChecked(clip->enabled);
        const bool isLinked = !clip->linkGroup.empty() && ve::edit::linkedGroup(tl, clipId).size() > 1;
        QAction* link = action(tr("Link Clips"), "contextLinkClips", [this, isLinked] {
            if (isLinked)
                unlinkSelection();
            else
                linkSelection();
        });
        link->setCheckable(true);
        link->setChecked(isLinked);
        m->addSeparator();
        if (const auto* media = std::get_if<ve::Clip>(item)) {
            const QString mediaId = qs(media->mediaId);
            action(tr("Open in Source Viewer"), "contextOpenInSource", [this, mediaId] { openInSourceViewer(mediaId); });
        }
        if (std::holds_alternative<ve::CompoundClip>(*item)) {
            const QString id = context.clipId;
            action(tr("Open in Timeline"), "contextOpenCompound", [this, id] { openCompound(id); });
            action(tr("Decompose in Place"), "contextDecompose", [this] { decomposeSelection(); });
        } else {
            action(tr("New Compound Clip"), "contextCompose", [this] { composeSelection(); });
        }
        m->addSeparator();
        action(tr("Swap with Previous Clip"), "contextSwapLeft", [this] { swapSelection(-1); });
        action(tr("Swap with Next Clip"), "contextSwapRight", [this] { swapSelection(1); });
        m->addSeparator();
        action(tr("Delete"), "contextDelete", [this] { deleteSelection(); });
        action(tr("Ripple Delete"), "contextRippleDelete", [this] { rippleDeleteSelection(); });
        break;
    }
    case Kind::Gap: {
        const QString trackId = context.trackId;
        const Time at = context.at;
        action(tr("Ripple Delete Gap"), "contextDeleteGap", [this, trackId, at] { deleteGapAt(trackId, at); });
        action(tr("Delete All Gaps"), "contextDeleteGaps", [this] { deleteAllGaps(); });
        break;
    }
    case Kind::TrackHeader: {
        const ve::Track* track = tl.track(utf8(context.trackId));
        if (!track)
            return {};
        const QString trackId = context.trackId;
        const ve::Id id = track->id;
        action(tr("Rename Track..."), "contextRenameTrack", [this, trackId] { renameTrackInteractive(trackId); });
        QMenu* colorMenu = m->addMenu(tr("Track Color"));
        addColorActions(colorMenu, labels::clipColors(), qs(track->color), true, QStringLiteral("contextTrackColor:"),
                        [this, id](const QString& color) {
            edit(tr("Track Color"), [id, color](ve::Timeline& t, const ve::MediaPool&) {
                return ve::edit::setTrackColor(t, id, utf8(color));
            });
        });
        const bool enabled = track->enabled;
        const bool locked = track->locked;
        const bool syncLock = track->syncLock;
        QAction* enable = action(tr("Enable Track"), "contextEnableTrack", [this, id, enabled] {
            edit(enabled ? tr("Disable Track") : tr("Enable Track"),
                 [id, enabled](ve::Timeline& t, const ve::MediaPool&) { return ve::edit::setTrackEnabled(t, id, !enabled); });
        });
        enable->setCheckable(true);
        enable->setChecked(enabled);
        QAction* lock = action(tr("Lock Track"), "contextLockTrack", [this, id, locked] {
            edit(locked ? tr("Unlock Track") : tr("Lock Track"),
                 [id, locked](ve::Timeline& t, const ve::MediaPool&) { return ve::edit::setTrackLocked(t, id, !locked); });
        });
        lock->setCheckable(true);
        lock->setChecked(locked);
        QAction* sync = action(tr("Sync Lock"), "contextSyncLock", [this, id, syncLock] {
            edit(syncLock ? tr("Remove Sync Lock") : tr("Sync Lock Track"),
                 [id, syncLock](ve::Timeline& t, const ve::MediaPool&) { return ve::edit::setTrackSyncLock(t, id, !syncLock); });
        });
        sync->setCheckable(true);
        sync->setChecked(syncLock);
        m->addSeparator();
        action(tr("Add Video Track"), "contextAddVideoTrack", [this] { addTrack(ve::TrackKind::Video); });
        action(tr("Add Audio Track"), "contextAddAudioTrack", [this] { addTrack(ve::TrackKind::Audio); });
        break;
    }
    case Kind::Marker: {
        const ve::Marker* marker = ve::edit::findMarker(tl, utf8(context.markerId));
        if (!marker)
            return {};
        const ve::Marker copy = *marker;
        action(tr("Go to Marker"), "contextGoToMarker", [this, copy] { seekTo(copy.start); });
        QMenu* colorMenu = m->addMenu(tr("Marker Color"));
        addColorActions(colorMenu, labels::markerColors(), qs(copy.color), false, QStringLiteral("contextMarkerColor:"),
                        [this, copy](const QString& color) {
            ve::Marker changed = copy;
            changed.color = utf8(color);
            edit(tr("Marker Color"), [changed](ve::Timeline& t, const ve::MediaPool&) { return ve::edit::updateMarker(t, changed); });
        });
        m->addSeparator();
        const ve::Id id = copy.id;
        action(tr("Delete Marker"), "contextDeleteMarker", [this, id] {
            edit(tr("Delete Marker"), [id](ve::Timeline& t, const ve::MediaPool&) { return ve::edit::removeMarker(t, id); });
        });
        break;
    }
    case Kind::Ruler:
    case Kind::Empty: {
        const Time at = ve::floorToFrame(context.at, tl.rate);
        action(tr("Add Marker Here"), "contextAddMarker", [this, at] { addMarker(at); });
        action(tr("Delete All Gaps"), "contextDeleteGaps", [this] { deleteAllGaps(); });
        m->addSeparator();
        action(tr("Add Video Track"), "contextAddVideoTrack", [this] { addTrack(ve::TrackKind::Video); });
        action(tr("Add Audio Track"), "contextAddAudioTrack", [this] { addTrack(ve::TrackKind::Audio); });
        break;
    }
    }
    return menu;
}

// ---------------------------------------------------------------------------------------------
// Layouts
// ---------------------------------------------------------------------------------------------

bool MainWindow::saveLayoutPreset(const QString& name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return false;
    auto settings = layoutSettings();
    settings->setValue(QStringLiteral("presets/") + presetKey(trimmed), saveState(kLayoutVersion));
    settings->sync();
    return settings->status() == QSettings::NoError;
}

bool MainWindow::loadLayoutPreset(const QString& name)
{
    auto settings = layoutSettings();
    const QByteArray state = settings->value(QStringLiteral("presets/") + presetKey(name.trimmed())).toByteArray();
    if (state.isEmpty() || !restoreState(state, kLayoutVersion))
        return false;
    if (m_sourceDock->isHidden())
        setActiveViewer(false);
    return true;
}

bool MainWindow::deleteLayoutPreset(const QString& name)
{
    auto settings = layoutSettings();
    const QString key = QStringLiteral("presets/") + presetKey(name.trimmed());
    if (!settings->contains(key))
        return false;
    settings->remove(key);
    return true;
}

QStringList MainWindow::layoutPresets() const
{
    auto settings = layoutSettings();
    settings->beginGroup(QStringLiteral("presets"));
    QStringList names;
    for (const QString& key : settings->childKeys())
        names << presetName(key);
    names.sort(Qt::CaseInsensitive);
    return names;
}

void MainWindow::resetLayout()
{
    applyDefaultLayout();
    sizeDefaultDocks();
}

void MainWindow::restoreLastLayout()
{
    auto settings = layoutSettings();
    const QByteArray geometry = settings->value(QStringLiteral("last/geometry")).toByteArray();
    const QByteArray state = settings->value(QStringLiteral("last/state")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    if (!state.isEmpty() && restoreState(state, kLayoutVersion))
        m_sizeDocksOnShow = false;
}

void MainWindow::refreshPresetsMenu()
{
    m_presetsMenu->clear();
    m_presetsMenu->addAction(tr("Save Current Layout As..."), this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Save Layout"), tr("Preset name:"), QLineEdit::Normal, {}, &ok);
        if (ok && !name.trimmed().isEmpty()) {
            if (saveLayoutPreset(name))
                statusBar()->showMessage(tr("Layout \"%1\" saved").arg(name.trimmed()), 3000);
            else
                statusBar()->showMessage(tr("Could not save the layout"), 4000);
        }
    });
    const QStringList names = layoutPresets();
    if (names.isEmpty())
        return;
    m_presetsMenu->addSeparator();
    for (const QString& name : names)
        m_presetsMenu->addAction(name, this, [this, name] { loadLayoutPreset(name); });
    m_presetsMenu->addSeparator();
    QMenu* remove = m_presetsMenu->addMenu(tr("Delete Preset"));
    for (const QString& name : names)
        remove->addAction(name, this, [this, name] { deleteLayoutPreset(name); });
}

// ---------------------------------------------------------------------------------------------
// Model plumbing
// ---------------------------------------------------------------------------------------------

void MainWindow::onModelChanged()
{
    // Another timeline became current (tab, compound clip, undo of a new timeline...): each
    // timeline keeps its own playhead.
    const bool switched = m_project.currentTimeline != m_shownTimeline;
    const bool firstShown = m_shownTimeline.empty();
    if (switched && !firstShown)
        m_playheads[m_shownTimeline] = m_engine->position();

    auto snapshot = ve::TimelineSnapshot::fromProject(m_project);
    const bool changed = !m_snapshot || m_snapshot->timeline != snapshot->timeline
        || m_snapshot->nested != snapshot->nested || m_snapshot->media.size() != snapshot->media.size();
    if (changed) {
        m_snapshot = snapshot;
        m_engine->setTimeline(snapshot);
    }
    m_pool->setMedia(m_project.media, m_project.timeline().rate);
    // Media that left the pool (undone import, other project) leaves the source viewer too.
    if (m_source->hasMedia() && !m_project.media.find(m_source->mediaId())) {
        m_source->setMedia(nullptr, m_project.timeline().rate);
        setActiveViewer(false);
    }
    m_timeline->refresh();

    if (switched) {
        m_shownTimeline = m_project.currentTimeline;
        if (!firstShown) {
            m_markIn.reset();
            m_markOut.reset();
            m_timeline->clearMarkerSelection();
            m_timeline->zoomToFit();
            const auto it = m_playheads.find(m_shownTimeline);
            seekTo(it != m_playheads.end() ? it->second : Time(0));
        }
    }
    refreshTimelineTabs();
    // A tab behind another panel stays "visible" off screen; it is refreshed when its tab is picked.
    if (!m_indexDock->visibleRegion().isEmpty())
        m_index->setTimeline(m_project.timeline(), m_project.media);
    refreshPanels();
    updateWindowState();
}

void MainWindow::refreshPanels()
{
    // The inspector shows the last thing picked: a marker, clips, a track header, or else the timeline.
    const ve::Timeline& tl = m_project.timeline();
    const ve::Id marker = m_timeline->selectedMarker();
    if (!marker.empty()) {
        if (const ve::Marker* m = ve::edit::findMarker(tl, marker)) {
            m_inspector->showMarker(tl, *m);
            return;
        }
    }
    const std::vector<ve::Id> clips = selection();
    if (!clips.empty()) {
        m_inspector->showClips(tl, m_project.media, clips);
        return;
    }
    if (const ve::Track* track = tl.track(m_timeline->focusedTrack())) {
        m_inspector->showTrack(tl, *track);
        return;
    }
    m_inspector->showTimeline(tl);
}

void MainWindow::refreshTimelineTabs()
{
    const std::vector<ve::Timeline>& timelines = m_project.timelines;
    // Timelines a compound clip shows are marked as such.
    std::set<ve::Id> nested;
    for (const ve::Timeline& tl : timelines) {
        for (const ve::Track& t : tl.tracks) {
            for (const ve::PlacedItem& p : t.clips()) {
                if (const auto* compound = std::get_if<ve::CompoundClip>(p.item))
                    nested.insert(compound->timelineId);
            }
        }
    }

    const QSignalBlocker block(m_timelineTabs);
    bool same = m_timelineTabs->count() == int(timelines.size());
    for (int i = 0; same && i < m_timelineTabs->count(); ++i)
        same = m_timelineTabs->tabData(i).toString() == qs(timelines[std::size_t(i)].id)
            && m_timelineTabs->tabText(i) == qs(timelines[std::size_t(i)].name);
    if (!same) {
        while (m_timelineTabs->count() > 0)
            m_timelineTabs->removeTab(0);
        for (const ve::Timeline& tl : timelines) {
            const int index = m_timelineTabs->addTab(qs(tl.name));
            m_timelineTabs->setTabData(index, qs(tl.id));
        }
    }
    for (int i = 0; i < m_timelineTabs->count(); ++i) {
        const ve::Timeline& tl = timelines[std::size_t(i)];
        m_timelineTabs->setTabToolTip(i, nested.count(tl.id) ? tr("%1 (compound clip)").arg(qs(tl.name)) : qs(tl.name));
        if (tl.id == m_project.currentTimeline)
            m_timelineTabs->setCurrentIndex(i);
    }
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
    const std::vector<ve::Id> selected = selection();
    bool compoundSelected = false;
    for (const ve::Id& id : selected) {
        if (const ve::Item* item = findItem(tl, id))
            compoundSelected |= std::holds_alternative<ve::CompoundClip>(*item);
    }
    m_splitAction->setEnabled(hasClips);
    m_deleteAction->setEnabled(!selected.empty() || !m_timeline->selectedMarker().empty());
    m_rippleDeleteAction->setEnabled(!selected.empty());
    m_composeAction->setEnabled(!selected.empty());
    m_decomposeAction->setEnabled(compoundSelected);
    m_openCompoundAction->setEnabled(compoundSelected);
    for (QAction* a : m_selectionActions)
        a->setEnabled(!selected.empty());
    const bool hasSource = m_source->hasMedia() || !m_pool->sourceMediaId().isEmpty();
    for (QAction* a : m_sourceEditActions)
        a->setEnabled(hasSource);
    m_exportAction->setEnabled(hasClips && !m_exporting);
    m_linkedSelectionAction->setChecked(m_timeline->linkedSelection());
    m_timeline->setMarks(m_markIn, m_markOut);

    m_viewerTitle->setText(tr("Timeline: %1").arg(qs(tl.name)));
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
    m_source->tick();
}

bool MainWindow::edit(const QString& name, ve::TimelineEditCommand::Operation op, const char* mergeKey)
{
    return m_commands.edit(utf8(name), std::move(op), mergeKey ? mergeKey : "");
}

bool MainWindow::editProject(const QString& name, ve::ProjectEditCommand::Operation op)
{
    return m_commands.editProject(utf8(name), std::move(op));
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress && (watched == m_viewer || watched == m_timeline))
        setActiveViewer(false);
    return QMainWindow::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------------------------
// Viewers and transport
// ---------------------------------------------------------------------------------------------

void MainWindow::openInSourceViewer(const QString& mediaId)
{
    const ve::MediaItem* media = m_project.media.find(utf8(mediaId));
    if (!media)
        return;
    m_engine->pause();
    m_source->setMedia(media, m_project.timeline().rate);
    m_sourceDock->show();
    m_sourceDock->raise();
    m_pool->selectMedia(mediaId);
    setActiveViewer(true);
}

void MainWindow::setActiveViewer(bool source)
{
    m_sourceActive = source && m_source->hasMedia();
    m_source->setActive(m_sourceActive);
    // The accent marks the active viewer once there are two to choose from.
    const bool accent = !m_sourceActive && m_source->hasMedia();
    if (m_viewerPanel->property("active").toBool() != accent) {
        m_viewerPanel->setProperty("active", accent);
        m_viewerPanel->style()->unpolish(m_viewerPanel);
        m_viewerPanel->style()->polish(m_viewerPanel);
    }
    updateWindowState();
}

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
    if (m_project.timeline().hasClips()) {
        m_source->pause();
        m_engine->play();
    }
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

void MainWindow::transportToggle()
{
    if (!m_sourceActive) {
        togglePlay();
        return;
    }
    m_engine->pause();
    m_source->togglePlay();
}

void MainWindow::transportPlay()
{
    if (!m_sourceActive) {
        play();
        return;
    }
    m_engine->pause();
    m_source->play();
}

void MainWindow::transportPause()
{
    m_engine->pause();
    m_source->pause();
}

void MainWindow::transportStep(std::int64_t frames)
{
    if (m_sourceActive)
        m_source->stepFrames(frames);
    else
        stepFrames(frames);
}

void MainWindow::transportToStart()
{
    if (m_sourceActive)
        m_source->goToStart();
    else
        seekTo(Time(0));
}

void MainWindow::transportToEnd()
{
    if (m_sourceActive)
        m_source->goToEnd();
    else
        seekToFrame(ve::frameAt(m_project.timeline().duration(), m_project.timeline().rate) - 1);
}

// ---------------------------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------------------------

bool MainWindow::linkedSelection() const
{
    return m_timeline->linkedSelection();
}

std::vector<ve::Id> MainWindow::selection() const
{
    return m_timeline->selectedClips();
}

void MainWindow::splitAtPlayhead()
{
    const Time t = m_engine->position();
    const std::vector<ve::Id> selected = selection();
    const bool linked = linkedSelection();
    if (!edit(tr("Split"), [t, selected, linked](ve::Timeline& tl, const ve::MediaPool&) {
            return ve::edit::splitAt(tl, t, selected, linked);
        }))
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
    const std::vector<ve::Id> selected = selection();
    if (selected.empty())
        return;
    const bool linked = linkedSelection();
    edit(tr("Delete"), [selected, linked](ve::Timeline& tl, const ve::MediaPool&) {
        return ve::edit::deleteClips(tl, selected, linked);
    });
}

void MainWindow::rippleDeleteSelection()
{
    const std::vector<ve::Id> selected = selection();
    if (selected.empty())
        return;
    const bool linked = linkedSelection();
    if (!edit(tr("Ripple Delete"), [selected, linked](ve::Timeline& tl, const ve::MediaPool&) {
            return ve::edit::rippleDelete(tl, selected, linked);
        }))
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
    // The source is the source viewer's clip with its In/Out; with an empty viewer, the media
    // selected in the pool (all of it).
    const bool fromViewer = m_source->hasMedia();
    const ve::MediaItem* found = m_project.media.find(fromViewer ? m_source->mediaId() : utf8(m_pool->sourceMediaId()));
    if (!found) {
        statusBar()->showMessage(tr("Open the media in the source viewer (double-click it in the pool) or select it there first"),
                                 4000);
        return;
    }
    const ve::MediaItem media = *found;
    ve::edit::EditPoints p;
    p.mediaId = media.id;
    if (fromViewer) {
        p.sourceIn = m_source->sourceIn();
        p.sourceOut = m_source->sourceOut();
    }
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
    case SourceEdit::Append: name = tr("Append %1"); break;
    case SourceEdit::RippleOverwrite: name = tr("Ripple Overwrite with %1"); break;
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
        case SourceEdit::Append: ids = ve::edit::appendEdit(tl, pool, p); break;
        case SourceEdit::RippleOverwrite: ids = ve::edit::rippleOverwriteEdit(tl, pool, p); break;
        }
        if (!ids.empty()) {
            if (auto loc = tl.locateClip(ids.front()))
                *end = loc->end();
        }
        return !ids.empty();
    });
    if (!ok) {
        QString why;
        switch (kind) {
        case SourceEdit::FitToFill: why = tr("Fit to fill needs a timeline Out point (O) and a free destination track"); break;
        case SourceEdit::Replace:
            why = tr("Replace needs a clip under the playhead on a destination track, and enough media");
            break;
        case SourceEdit::RippleOverwrite:
            why = tr("Ripple overwrite needs a clip under the playhead (or the In) on a destination track, and the "
                     "sync-locked tracks free to ripple");
            break;
        default:
            why = tr("Edit refused: check the In/Out points, the media length and the destination tracks "
                     "(locked ones cannot be edited)");
            break;
        }
        statusBar()->showMessage(why, 6000);
        return;
    }
    clearTimelineMarks();
    if (conform)
        m_timeline->zoomToFit();
    if (*end && kind != SourceEdit::Replace)
        seekTo(**end); // like other editors, the playhead moves past the new clip
}

void MainWindow::composeSelection()
{
    const std::vector<ve::Id> selected = selection();
    if (selected.empty())
        return;
    if (!editProject(tr("New Compound Clip"), [selected](ve::Project& p) { return ve::edit::composeClips(p, selected).has_value(); }))
        statusBar()->showMessage(tr("Cannot make a compound clip: other clips on those tracks overlap the selection"), 6000);
}

void MainWindow::decomposeSelection()
{
    for (const ve::Id& id : selection()) {
        auto loc = m_project.timeline().locateClip(id);
        if (loc && std::holds_alternative<ve::CompoundClip>(loc->item)) {
            if (!editProject(tr("Decompose Compound Clip"), [id](ve::Project& p) { return ve::edit::decomposeClip(p, id); }))
                statusBar()->showMessage(tr("Cannot decompose: a track it needs is locked"), 5000);
            return;
        }
    }
}

void MainWindow::openCompound(const QString& clipId)
{
    const ve::Item* item = findItem(m_project.timeline(), utf8(clipId));
    if (const auto* compound = item ? std::get_if<ve::CompoundClip>(item) : nullptr)
        openTimeline(compound->timelineId);
}

void MainWindow::addTrack(ve::TrackKind kind)
{
    edit(kind == ve::TrackKind::Video ? tr("Add Video Track") : tr("Add Audio Track"),
         [kind](ve::Timeline& tl, const ve::MediaPool&) { return !ve::edit::addTrack(tl, kind).empty(); });
}

void MainWindow::markIn()
{
    if (m_sourceActive) {
        m_source->markIn();
        return;
    }
    m_markIn = ve::floorToFrame(m_engine->position(), m_project.timeline().rate);
    if (m_markOut && *m_markOut <= *m_markIn)
        m_markOut.reset();
    updateWindowState();
}

void MainWindow::markOut()
{
    if (m_sourceActive) {
        m_source->markOut();
        return;
    }
    // The Out includes the frame under the playhead.
    const ve::Rate rate = m_project.timeline().rate;
    m_markOut = ve::timeOfFrame(ve::frameAt(m_engine->position(), rate) + 1, rate);
    if (m_markIn && *m_markIn >= *m_markOut)
        m_markIn.reset();
    updateWindowState();
}

void MainWindow::clearMarks()
{
    if (m_sourceActive)
        m_source->clearMarks();
    else
        clearTimelineMarks();
}

void MainWindow::clearTimelineMarks()
{
    m_markIn.reset();
    m_markOut.reset();
    updateWindowState();
}

void MainWindow::addMarker(std::optional<Time> at)
{
    const Time where = at.value_or(m_engine->position());
    const QString name = tr("Marker %1").arg(m_project.timeline().markers.size() + 1);
    if (!edit(tr("Add Marker"), [where, name](ve::Timeline& tl, const ve::MediaPool&) {
            return !ve::edit::addMarker(tl, where, utf8(name)).empty();
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

// ---------------------------------------------------------------------------------------------
// Organization, gaps and timelines (phase 3)
// ---------------------------------------------------------------------------------------------

void MainWindow::setSelectionColor(const QString& color)
{
    const std::vector<ve::Id> selected = selection();
    edit(tr("Clip Color"), [selected, color](ve::Timeline& tl, const ve::MediaPool&) {
        return ve::edit::setClipColor(tl, selected, utf8(color));
    });
}

void MainWindow::setSelectionFlag(const QString& color, bool on)
{
    const std::vector<ve::Id> selected = selection();
    edit(on ? tr("Add Flag") : tr("Remove Flag"), [selected, color, on](ve::Timeline& tl, const ve::MediaPool&) {
        return ve::edit::setClipFlag(tl, selected, utf8(color), on);
    });
}

void MainWindow::clearSelectionFlags()
{
    const std::vector<ve::Id> selected = selection();
    edit(tr("Clear Flags"), [selected](ve::Timeline& tl, const ve::MediaPool&) { return ve::edit::clearClipFlags(tl, selected); });
}

void MainWindow::toggleSelectionEnabled()
{
    // Any enabled clip in the selection: disable them all; otherwise enable them.
    const std::vector<ve::Id> selected = selection();
    bool anyEnabled = false;
    for (const ve::Id& id : selected) {
        if (const ve::ClipBase* c = findClipBase(m_project.timeline(), id))
            anyEnabled |= c->enabled;
    }
    const bool on = !anyEnabled;
    edit(on ? tr("Enable Clip") : tr("Disable Clip"),
         [selected, on](ve::Timeline& tl, const ve::MediaPool&) { return ve::edit::setClipsEnabled(tl, selected, on); });
}

void MainWindow::linkSelection()
{
    const std::vector<ve::Id> selected = selection();
    if (!edit(tr("Link Clips"), [selected](ve::Timeline& tl, const ve::MediaPool&) { return ve::edit::linkClips(tl, selected); }))
        statusBar()->showMessage(tr("Select clips on different tracks (one per track) to link them"), 4000);
}

void MainWindow::unlinkSelection()
{
    const std::vector<ve::Id> selected = selection();
    edit(tr("Unlink Clips"), [selected](ve::Timeline& tl, const ve::MediaPool&) { return ve::edit::unlinkClips(tl, selected); });
}

void MainWindow::swapSelection(int direction)
{
    const std::vector<ve::Id> selected = selection();
    if (selected.empty())
        return;
    const ve::Id clip = selected.front();
    if (!edit(direction < 0 ? tr("Swap with Previous Clip") : tr("Swap with Next Clip"),
              [clip, direction](ve::Timeline& tl, const ve::MediaPool&) { return ve::edit::swapClips(tl, clip, direction); }))
        statusBar()->showMessage(tr("Cannot swap: no clip there, or its linked partners do not line up"), 4000);
}

void MainWindow::insertGapAtPlayhead()
{
    // The In->Out range when both are set, otherwise one second at the playhead.
    const ve::Rate rate = m_project.timeline().rate;
    Time at = ve::floorToFrame(m_engine->position(), rate);
    Time length = ve::timeOfFrame(std::llround(rate.toDouble()), rate);
    if (m_markIn && m_markOut) {
        at = *m_markIn;
        length = *m_markOut - *m_markIn;
    }
    if (!edit(tr("Insert Gap"), [at, length](ve::Timeline& tl, const ve::MediaPool&) { return ve::edit::insertGap(tl, at, length); }))
        statusBar()->showMessage(tr("Nothing to move: no unlocked sync-locked track has clips after that point"), 4000);
}

void MainWindow::deleteGapAt(const QString& trackId, Time at)
{
    const ve::Id track = utf8(trackId);
    if (!edit(tr("Delete Gap"), [track, at](ve::Timeline& tl, const ve::MediaPool&) { return ve::edit::deleteGap(tl, track, at); }))
        statusBar()->showMessage(tr("Cannot close that gap: another sync-locked track has clips in that time"), 5000);
}

void MainWindow::deleteAllGaps()
{
    if (!edit(tr("Delete Gaps"), [](ve::Timeline& tl, const ve::MediaPool&) { return ve::edit::deleteGaps(tl); }))
        statusBar()->showMessage(tr("No gap is empty on every sync-locked track"), 4000);
}

void MainWindow::openTimeline(const ve::Id& timelineId)
{
    // Not an edit: which timeline is open is not undone (each edit remembers its own timeline).
    if (timelineId == m_project.currentTimeline || !ve::edit::setCurrentTimeline(m_project, timelineId))
        return;
    m_engine->pause();
    onModelChanged();
}

void MainWindow::newTimeline()
{
    editProject(tr("New Timeline"), [](ve::Project& p) { return !ve::edit::newTimeline(p).empty(); });
}

void MainWindow::duplicateTimeline()
{
    const ve::Id id = m_project.currentTimeline;
    editProject(tr("Duplicate Timeline"), [id](ve::Project& p) { return !ve::edit::duplicateTimeline(p, id).empty(); });
}

void MainWindow::renameTimeline()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename Timeline"), tr("Name:"), QLineEdit::Normal,
                                               qs(m_project.timeline().name), &ok);
    if (!ok)
        return;
    const ve::Id id = m_project.currentTimeline;
    if (!editProject(tr("Rename Timeline"), [id, name](ve::Project& p) { return ve::edit::renameTimeline(p, id, utf8(name.trimmed())); }))
        statusBar()->showMessage(tr("Another timeline already has that name"), 4000);
}

void MainWindow::deleteTimeline()
{
    const ve::Id id = m_project.currentTimeline;
    if (!editProject(tr("Delete Timeline"), [id](ve::Project& p) { return ve::edit::deleteTimeline(p, id); }))
        statusBar()->showMessage(tr("The last timeline, and timelines a compound clip shows, cannot be deleted"), 5000);
}

void MainWindow::renameTrackInteractive(const QString& trackId)
{
    const ve::Track* track = m_project.timeline().track(utf8(trackId));
    if (!track)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename Track"), tr("Name:"), QLineEdit::Normal, qs(track->name), &ok);
    if (!ok)
        return;
    const ve::Id id = track->id;
    if (!edit(tr("Rename Track"), [id, name](ve::Timeline& tl, const ve::MediaPool&) {
            return ve::edit::renameTrack(tl, id, utf8(name.trimmed()));
        }))
        statusBar()->showMessage(tr("Another track already has that name"), 4000);
}

void MainWindow::renameSelectionInteractive()
{
    const std::vector<ve::Id> selected = selection();
    const ve::ClipBase* first = selected.empty() ? nullptr : findClipBase(m_project.timeline(), selected.front());
    if (!first)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename Clip"), tr("Name:"), QLineEdit::Normal, qs(first->name), &ok);
    if (!ok)
        return;
    edit(tr("Rename Clip"), [selected, name](ve::Timeline& tl, const ve::MediaPool&) {
        bool changed = false;
        for (const ve::Id& id : selected)
            changed |= ve::edit::renameClip(tl, id, utf8(name));
        return changed;
    });
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
    m_shownTimeline.clear();
    m_playheads.clear();
    m_markIn.reset();
    m_markOut.reset();
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
    m_shownTimeline.clear();
    m_playheads.clear();
    m_markIn.reset();
    m_markOut.reset();
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
    m_source->pause();

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

// ---------------------------------------------------------------------------------------------
// Updates (the latest GitHub release; the macOS app installs it by itself)
// ---------------------------------------------------------------------------------------------

Updater* MainWindow::updater()
{
    if (m_updater)
        return m_updater;
    QUrl feed = update::githubFeed(QStringLiteral(VE_UPDATE_REPOSITORY));
    if (const QString custom = qEnvironmentVariable("VE_UPDATE_FEED"); !custom.isEmpty())
        feed = QUrl::fromUserInput(custom); // a test feed: a local file or another server
    m_updater = new Updater(feed, update::versionOf(QStringLiteral(VE_VERSION)), this);
    connect(m_updater, &Updater::checked, this, &MainWindow::onUpdateChecked);
    connect(m_updater, &Updater::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (!m_updateProgress)
            return;
        m_updateProgress->setMaximum(total > 0 ? int(total / 1024) : 0);
        m_updateProgress->setValue(int(received / 1024));
    });
    connect(m_updater, &Updater::downloaded, this, &MainWindow::onUpdateDownloaded);
    connect(m_updater, &Updater::failed, this, [this](const QString& error) {
        closeUpdateProgress();
        m_updateAction->setEnabled(true);
        QMessageBox::warning(this, tr("Update"), error);
    });
    return m_updater;
}

void MainWindow::checkForUpdates()
{
    m_updateAction->setEnabled(false);
    statusBar()->showMessage(tr("Checking for updates..."));
    updater()->check();
}

void MainWindow::onUpdateChecked(bool newer, const ReleaseInfo& release, const QString& error)
{
    m_updateAction->setEnabled(true);
    statusBar()->clearMessage();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, tr("Check for Updates"), tr("Could not check for updates:\n%1").arg(error));
        return;
    }
    if (!newer) {
        QMessageBox::information(this, tr("Check for Updates"),
                                 tr("VideoEditor %1 is the latest version.").arg(QStringLiteral(VE_VERSION)));
        return;
    }

    QString why;
    bool canInstall = Updater::canInstallInPlace(&why);
    if (canInstall && release.assetName.isEmpty()) {
        canInstall = false;
        why = tr("This release has no download for this computer.");
    }
    QMessageBox box(this);
    box.setWindowTitle(tr("Update Available"));
    box.setIcon(QMessageBox::Information);
    box.setText(tr("VideoEditor %1 is available. You have %2.").arg(release.version.toString(), QStringLiteral(VE_VERSION)));
    QString details = release.notes.trimmed();
    if (details.size() > 1200)
        details = details.left(1200) + QStringLiteral("...");
    if (!canInstall)
        details += (details.isEmpty() ? QString() : QStringLiteral("\n\n")) + why;
    box.setInformativeText(details);
    QPushButton* install = canInstall ? box.addButton(tr("Download and Install"), QMessageBox::AcceptRole) : nullptr;
    QPushButton* page = box.addButton(tr("Open Release Page"), QMessageBox::ActionRole);
    box.addButton(tr("Later"), QMessageBox::RejectRole);
    box.setDefaultButton(install ? install : page);
    box.exec();

    if (install && box.clickedButton() == install) {
        m_updateAction->setEnabled(false);
        m_updateProgress = new QProgressDialog(tr("Downloading VideoEditor %1...").arg(release.version.toString()),
                                               tr("Cancel"), 0, 0, this);
        m_updateProgress->setWindowTitle(tr("Update"));
        m_updateProgress->setWindowModality(Qt::WindowModal);
        m_updateProgress->setMinimumDuration(0);
        m_updateProgress->setAutoClose(false);
        m_updateProgress->setAutoReset(false);
        connect(m_updateProgress, &QProgressDialog::canceled, this, [this] {
            updater()->cancel();
            closeUpdateProgress();
            m_updateAction->setEnabled(true);
        });
        updater()->download(release);
    } else if (box.clickedButton() == page) {
        QDesktopServices::openUrl(release.page);
    }
}

void MainWindow::onUpdateDownloaded(const ReleaseInfo& release, const QString& zipPath)
{
    closeUpdateProgress();
    m_updateAction->setEnabled(true);
    QString error;
    const auto installer = updater()->prepareInstall(release, zipPath, &error);
    if (!installer) {
        QMessageBox::warning(this, tr("Update"),
                             tr("VideoEditor %1 was downloaded and verified but cannot be installed:\n%2\n\nThe download "
                                "is in %3")
                                 .arg(release.version.toString(), error, QDir::toNativeSeparators(zipPath)));
        return;
    }
    if (QMessageBox::question(this, tr("Install Update"),
                              tr("VideoEditor %1 is ready. VideoEditor will close, replace itself and open again.\n\n"
                                 "Install now?")
                                  .arg(release.version.toString()))
        != QMessageBox::Yes)
        return;
    if (m_exporting) {
        QMessageBox::information(this, tr("Export running"), tr("Wait for the export to finish or cancel it first."));
        return;
    }
    if (!maybeSave())
        return;
    if (!QProcess::startDetached(installer->program, installer->arguments)) {
        QMessageBox::warning(this, tr("Update"), tr("Could not start the installer."));
        return;
    }
    m_updating = true; // the project was saved (or discarded) above
    close();
}

void MainWindow::closeUpdateProgress()
{
    if (m_updateProgress) {
        m_updateProgress->close();
        m_updateProgress->deleteLater();
    }
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (m_sizeDocksOnShow) {
        m_sizeDocksOnShow = false;
        sizeDefaultDocks();
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_exporting) {
        QMessageBox::information(this, tr("Export running"), tr("Wait for the export to finish or cancel it first."));
        event->ignore();
        return;
    }
    if (!m_updating && !maybeSave()) {
        event->ignore();
        return;
    }
    m_engine->pause();
    m_source->pause();
    // The layout is back next time (app/main.cpp calls restoreLastLayout()).
    auto settings = layoutSettings();
    settings->setValue(QStringLiteral("last/geometry"), saveGeometry());
    settings->setValue(QStringLiteral("last/state"), saveState(kLayoutVersion));
    event->accept();
}
