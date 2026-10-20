#pragma once

#include "core/Model.h"

#include <QStringList>
#include <QWidget>

#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QStackedWidget;
class QToolButton;

// Properties of what is selected (spec phase 3.5): the timeline when nothing is, the clips, a marker
// or a track. Like the other panels it never edits the model; every change is a signal that the
// window turns into a command. Effects, transform and audio controls join it in phases 4 to 6.
class InspectorWidget : public QWidget {
    Q_OBJECT

public:
    explicit InspectorWidget(QWidget* parent = nullptr);

    void showTimeline(const ve::Timeline& tl);
    void showClips(const ve::Timeline& tl, const ve::MediaPool& pool, const std::vector<ve::Id>& clipIds);
    void showMarker(const ve::Timeline& tl, const ve::Marker& marker);
    void showTrack(const ve::Timeline& tl, const ve::Track& track);

signals:
    void timelineRenamed(const QString& name);
    void clipsRenamed(const QStringList& clipIds, const QString& name);
    void clipColorChosen(const QStringList& clipIds, const QString& color);
    void clipFlagToggled(const QStringList& clipIds, const QString& color, bool on);
    void clipsEnabledChanged(const QStringList& clipIds, bool enabled);
    void markerEdited(const ve::Marker& marker);
    void trackRenamed(const QString& trackId, const QString& name);
    void trackColorChosen(const QString& trackId, const QString& color);
    void trackLockChanged(const QString& trackId, bool on);
    void trackSyncLockChanged(const QString& trackId, bool on);
    void trackEnabledChanged(const QString& trackId, bool on);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* buildTimelinePage();
    QWidget* buildClipPage();
    QWidget* buildMarkerPage();
    QWidget* buildTrackPage();

    QStackedWidget* m_pages = nullptr;
    QLabel* m_title = nullptr;

    QLineEdit* m_timelineName = nullptr;
    QLabel* m_timelineInfo = nullptr;

    QLineEdit* m_clipName = nullptr;
    QLabel* m_clipInfo = nullptr;
    QComboBox* m_clipColor = nullptr;
    QCheckBox* m_clipEnabled = nullptr;
    std::vector<QToolButton*> m_flagButtons;
    QStringList m_clipIds;

    QLineEdit* m_markerName = nullptr;
    QPlainTextEdit* m_markerNote = nullptr;
    QComboBox* m_markerColor = nullptr;
    QLineEdit* m_markerStart = nullptr;
    QSpinBox* m_markerDuration = nullptr;
    ve::Marker m_marker;

    QLineEdit* m_trackName = nullptr;
    QComboBox* m_trackColor = nullptr;
    QCheckBox* m_trackLocked = nullptr;
    QCheckBox* m_trackSyncLock = nullptr;
    QCheckBox* m_trackEnabled = nullptr;
    QString m_trackId;

    ve::Rate m_rate{30, 1};
};
