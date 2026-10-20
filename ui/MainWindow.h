#pragma once

#include "core/Commands.h"
#include "core/Model.h"
#include "core/Snapshot.h"
#include "ui/TimelineWidget.h"

#include <QMainWindow>
#include <QPointer>
#include <QTimer>

#include <map>
#include <memory>
#include <optional>

namespace ve {
class PlaybackEngine;
struct ExportResult;
}

class EditIndexWidget;
class InspectorWidget;
class MediaPoolWidget;
class QAction;
class QDockWidget;
class QFrame;
class QLabel;
class QMenu;
class QProgressDialog;
class QTabBar;
class QToolButton;
class SourceViewerPanel;
class Updater;
class ViewerWidget;
struct ReleaseInfo;

// Owns the project, the command stack and the playback engines, and turns UI gestures into
// commands (timeline drags become transactions). Every model change funnels through
// onModelChanged().
//
// The window is laid out like the Edit page of other editors (spec phase 3.3): the timeline viewer
// in the middle and dockable panels around it (media pool, edit index, source viewer, inspector,
// timeline). Panels can be shown, hidden, moved and floated onto another monitor, and the layout can
// be saved as named presets. One of the two viewers is active at a time: it receives the transport
// keys and the In/Out marks.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    bool openProjectFile(const QString& path);
    void importFiles(const QStringList& paths, std::optional<ve::Time> at = std::nullopt);
    void seekTo(ve::Time t);
    void seekToFrame(std::int64_t frame);
    void play();

    // Loads pool media into the source viewer and makes it the active viewer.
    void openInSourceViewer(const QString& mediaId);
    bool sourceViewerActive() const { return m_sourceActive; }

    // Layout presets (stored with QSettings). The last layout is saved on close; the app restores it
    // at startup with restoreLastLayout() (tests start from the default layout).
    bool saveLayoutPreset(const QString& name);
    bool loadLayoutPreset(const QString& name);
    bool deleteLayoutPreset(const QString& name);
    QStringList layoutPresets() const;
    void resetLayout();
    void restoreLastLayout();

    // Help > Check for Updates: asks the release feed (GitHub, or VE_UPDATE_FEED) for a newer version
    // and, in the macOS app, downloads, verifies and installs it, then reopens.
    void checkForUpdates();
    Updater* updater();

    // The context menu for a right click on the timeline. The window execs it; it is built
    // separately so tests can trigger its actions.
    std::unique_ptr<QMenu> contextMenu(const TimelineWidget::Context& context);

    // Read-only access for tests and future panels.
    const ve::Project& project() const { return m_project; }
    ve::Time playheadPosition() const;
    bool isPlaying() const;

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    void buildDocks();
    void applyDefaultLayout();
    void sizeDefaultDocks();
    void buildActions();
    void connectPanels();
    void onModelChanged();
    void updateWindowState();
    void updateTransport();
    void refreshPanels();
    void refreshTimelineTabs();
    void refreshPresetsMenu();
    void tick();
    void setActiveViewer(bool source);

    bool edit(const QString& name, ve::TimelineEditCommand::Operation op, const char* mergeKey = nullptr);
    bool editProject(const QString& name, ve::ProjectEditCommand::Operation op);
    bool maybeSave();
    bool saveTo(const QString& path);

    void newProject();
    void openProject();
    bool save();
    bool saveAs();
    void importMedia();
    void exportTimeline();
    void onExportFinished(const ve::ExportResult& result, const QString& path);
    void onUpdateChecked(bool newer, const ReleaseInfo& release, const QString& error);
    void onUpdateDownloaded(const ReleaseInfo& release, const QString& zipPath);
    void closeUpdateProgress();

    // Transport of the active viewer (source or timeline).
    void transportToggle();
    void transportPlay();
    void transportPause();
    void transportStep(std::int64_t frames);
    void transportToStart();
    void transportToEnd();
    void togglePlay();
    void stepFrames(std::int64_t frames);

    void splitAtPlayhead();
    void deleteSelection();
    void rippleDeleteSelection();
    void placeMediaAt(const QString& mediaId, std::optional<ve::Time> at, const QString& trackId = {});

    // Three and four point edits of the source (the source viewer, or else the pool selection) onto
    // the destination tracks (spec §6.1).
    enum class SourceEdit { Insert, Overwrite, Replace, FitToFill, PlaceOnTop, Append, RippleOverwrite };
    void sourceEdit(SourceEdit kind);
    void composeSelection();
    void decomposeSelection();
    void openCompound(const QString& clipId);
    void addTrack(ve::TrackKind kind);
    void markIn();    // of the active viewer
    void markOut();
    void clearMarks();
    void clearTimelineMarks();
    void addMarker(std::optional<ve::Time> at = std::nullopt);
    void jumpToEdit(int direction);   // previous (-1) / next (+1) cut
    void jumpToMarker(int direction); // previous / next marker

    // Organization (phase 3.1) of the selection, and the timelines of the project (phase 3.2).
    void setSelectionColor(const QString& color);
    void setSelectionFlag(const QString& color, bool on);
    void clearSelectionFlags();
    void toggleSelectionEnabled();
    void linkSelection();
    void unlinkSelection();
    void swapSelection(int direction);
    void insertGapAtPlayhead();
    void deleteGapAt(const QString& trackId, ve::Time at);
    void deleteAllGaps();
    void newTimeline();
    void duplicateTimeline();
    void renameTimeline();
    void deleteTimeline();
    void openTimeline(const ve::Id& timelineId);
    void renameTrackInteractive(const QString& trackId);
    void renameSelectionInteractive();
    // Fills the clip colour and flag submenus for the current selection.
    void fillLabelMenus(QMenu* colorMenu, QMenu* flagMenu, const QString& objectPrefix);
    bool linkedSelection() const;
    std::vector<ve::Id> selection() const;

    ve::Project m_project;
    ve::CommandStack m_commands{m_project};
    ve::TimelineSnapshotPtr m_snapshot;
    std::unique_ptr<ve::PlaybackEngine> m_engine;

    ViewerWidget* m_viewer = nullptr;
    QFrame* m_viewerPanel = nullptr;
    QLabel* m_viewerTitle = nullptr;
    TimelineWidget* m_timeline = nullptr;
    MediaPoolWidget* m_pool = nullptr;
    SourceViewerPanel* m_source = nullptr;
    InspectorWidget* m_inspector = nullptr;
    EditIndexWidget* m_index = nullptr;
    QTabBar* m_timelineTabs = nullptr;
    QDockWidget* m_poolDock = nullptr;
    QDockWidget* m_indexDock = nullptr;
    QDockWidget* m_sourceDock = nullptr;
    QDockWidget* m_inspectorDock = nullptr;
    QDockWidget* m_timelineDock = nullptr;
    QLabel* m_timecode = nullptr;
    QLabel* m_duration = nullptr;
    QLabel* m_status = nullptr;
    QToolButton* m_playButton = nullptr;

    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;
    QAction* m_splitAction = nullptr;
    QAction* m_deleteAction = nullptr;
    QAction* m_rippleDeleteAction = nullptr;
    QAction* m_exportAction = nullptr;
    QList<QAction*> m_sourceEditActions;
    QAction* m_composeAction = nullptr;
    QAction* m_decomposeAction = nullptr;
    QAction* m_openCompoundAction = nullptr;
    QAction* m_linkedSelectionAction = nullptr;
    QList<QAction*> m_selectionActions; // need clips selected
    QMenu* m_presetsMenu = nullptr;
    QAction* m_updateAction = nullptr;
    Updater* m_updater = nullptr;
    QPointer<QProgressDialog> m_updateProgress;
    bool m_updating = false; // quitting so the update can replace the app

    std::optional<ve::Time> m_markIn;
    std::optional<ve::Time> m_markOut;
    bool m_sourceActive = false;
    ve::Id m_shownTimeline;
    std::map<ve::Id, ve::Time> m_playheads; // where each timeline was left
    bool m_sizeDocksOnShow = true;          // default layout, until a saved one is restored

    QTimer m_ticker;
    QString m_projectPath;
    QString m_gpuBackend;
    ve::Time m_lastPosition{-1};
    bool m_lastPlaying = false;
    bool m_exporting = false;
};
