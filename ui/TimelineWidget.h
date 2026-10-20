#pragma once

#include "core/Model.h"

#include <QSet>
#include <QWidget>

#include <vector>

class QScrollBar;

// Track view of the current timeline: ruler, V/A lanes, clips, playhead.
//
// It never edits the model itself. Drags are *gestures*: press emits gestureStarted (MainWindow
// opens a transaction), every mouse move emits the edit for the current position relative to the
// press (merged into the previous step, so the model and the viewer update live), release emits
// gestureFinished(true) and Escape gestureFinished(false), which rolls everything back.
// The UI works in frames of the timeline rate; the model in flicks.
class TimelineWidget : public QWidget {
    Q_OBJECT

public:
    explicit TimelineWidget(QWidget* parent = nullptr);

    void setProject(const ve::Project* project);
    void refresh(); // model changed
    void setPlayhead(ve::Time t, bool follow);
    std::vector<ve::Id> selectedClips() const;
    // Where a clip is drawn, in widget coordinates (empty if not on screen).
    QRect clipRect(const ve::Id& clipId) const;
    void zoomToFit();
    void zoomBy(double factor);

signals:
    void seekRequested(ve::Time t);
    void gestureStarted(const QString& name);
    void moveRequested(const QStringList& clipIds, ve::Time delta);
    void trimRequested(const QString& clipId, bool head, ve::Time newEdge);
    void gestureFinished(bool commit);
    void mediaDropped(const QString& mediaId, ve::Time at);
    void filesDropped(const QStringList& paths, ve::Time at);
    void selectionChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    using Frame = std::int64_t;

    struct Row {
        const ve::Track* track = nullptr;
        int top = 0;
        int height = 0;
    };
    enum class HitKind { None, Ruler, Empty, Body, Head, Tail };
    struct Hit {
        HitKind kind = HitKind::None;
        QString clipId;
        Frame start = 0;
        Frame end = 0;
    };
    enum class Drag { None, Scrub, Move, TrimHead, TrimTail };

    const ve::Timeline* timeline() const;
    ve::Rate rate() const;
    Frame frameOf(ve::Time t) const;
    ve::Time timeOf(Frame f) const;
    std::vector<Row> rows() const;
    int contentBottom() const;
    double xForFrame(double frame) const;
    Frame frameAtX(double x) const;
    double visibleFrames() const;
    Hit hitTest(const QPoint& pos) const;
    QSet<QString> groupOf(const QString& clipId) const;
    void selectGroup(const QString& clipId, bool toggle);
    std::vector<Frame> snapTargets(const QSet<QString>& exclude) const;
    Frame snap(Frame frame, const std::vector<Frame>& targets) const;
    void finishGesture(bool commit);
    void setScroll(double frames);
    void updateScrollBar();
    void pruneSelection();

    void paintRuler(QPainter& p);
    void paintClips(QPainter& p, const Row& row);

    const ve::Project* m_project = nullptr;
    QScrollBar* m_scrollBar = nullptr;
    double m_pxPerFrame = 4.0;
    double m_scroll = 0.0; // first visible frame
    Frame m_playhead = 0;
    QSet<QString> m_selected;

    // Gesture state, captured at mouse press (every step is computed from it).
    Drag m_drag = Drag::None;
    Frame m_pressFrame = 0;
    QStringList m_dragClips;
    std::vector<std::pair<Frame, Frame>> m_dragSpans; // start/end of the dragged clips at press
    Frame m_dragMinStart = 0;
    Frame m_dragDelta = 0;
    QString m_trimClip;
    Frame m_trimStart = 0;
    Frame m_trimEnd = 0;
    Frame m_trimEdge = 0;
    std::vector<Frame> m_snapTargets;
    Frame m_dropFrame = -1;
};
