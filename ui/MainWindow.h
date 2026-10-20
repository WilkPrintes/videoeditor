#pragma once

#include "core/Commands.h"
#include "core/Model.h"
#include "core/Snapshot.h"

#include <QMainWindow>
#include <QTimer>

#include <memory>
#include <optional>

namespace ve {
class PlaybackEngine;
struct ExportResult;
}

class MediaPoolWidget;
class QAction;
class QLabel;
class QToolButton;
class TimelineWidget;
class ViewerWidget;

// Owns the project, the command stack and the playback engine, and turns UI gestures into
// commands (timeline drags become transactions). Every model change funnels through
// onModelChanged().
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

    // Read-only access for tests and future panels.
    const ve::Project& project() const { return m_project; }
    ve::Time playheadPosition() const;
    bool isPlaying() const;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    void buildActions();
    void onModelChanged();
    void updateWindowState();
    void updateTransport();
    void tick();

    bool edit(const QString& name, ve::TimelineEditCommand::Operation op, const char* mergeKey = nullptr);
    bool maybeSave();
    bool saveTo(const QString& path);

    void newProject();
    void openProject();
    bool save();
    bool saveAs();
    void importMedia();
    void exportTimeline();
    void onExportFinished(const ve::ExportResult& result, const QString& path);

    void togglePlay();
    void stepFrames(std::int64_t frames);
    void splitAtPlayhead();
    void deleteSelection();
    void placeMediaAt(const QString& mediaId, std::optional<ve::Time> at);

    ve::Project m_project;
    ve::CommandStack m_commands{m_project};
    ve::TimelineSnapshotPtr m_snapshot;
    std::unique_ptr<ve::PlaybackEngine> m_engine;

    ViewerWidget* m_viewer = nullptr;
    TimelineWidget* m_timeline = nullptr;
    MediaPoolWidget* m_pool = nullptr;
    QLabel* m_timecode = nullptr;
    QLabel* m_duration = nullptr;
    QLabel* m_status = nullptr;
    QToolButton* m_playButton = nullptr;

    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;
    QAction* m_splitAction = nullptr;
    QAction* m_deleteAction = nullptr;
    QAction* m_exportAction = nullptr;

    QTimer m_ticker;
    QString m_projectPath;
    QString m_gpuBackend;
    ve::Time m_lastPosition{-1};
    bool m_lastPlaying = false;
    bool m_exporting = false;
};
