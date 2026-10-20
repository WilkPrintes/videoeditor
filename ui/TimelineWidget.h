#pragma once

#include "core/Model.h"

#include <QSet>
#include <QWidget>

#include <optional>
#include <vector>

class QScrollBar;

// Track view of the current timeline: ruler with markers and In/Out, any number of video and audio
// tracks with their headers (edit destination, lock, sync lock), clips and the playhead.
//
// It never edits the model itself. Drags are *gestures*: press emits gestureStarted (MainWindow
// opens a transaction), every mouse move emits the edit for the current position relative to the
// press (merged into the previous step, so the model and the viewer update live), release emits
// gestureFinished(true) and Escape gestureFinished(false), which rolls everything back.
//
// What a drag does depends on the tool and the modifiers (spec §6.2):
//   Selection tool (A)   body: move (also onto other tracks)   edge: trim into empty space
//                        Shift+edge: ripple
//   Trim tool (T)        body: slip    Shift+body: slide       edge: ripple
//   both                 Ctrl+edge: roll; Alt on any trim: this track only (J/L cuts)
//   ruler                drag a marker to move it, anywhere else to scrub
// Edges, clips and markers snap to cuts, markers, the playhead and In/Out (setSnapping).
// The UI works in frames of the timeline rate; the model in flicks.
class TimelineWidget : public QWidget {
    Q_OBJECT

public:
    enum class Tool { Select, Trim };
    enum class TrimMode { Ripple, Roll, Slip, Slide };
    enum class TrackFlag { Lock, SyncLock };
    Q_ENUM(Tool)
    Q_ENUM(TrimMode)
    Q_ENUM(TrackFlag)

    // What a right click landed on; the window builds the context menu from it.
    struct Context {
        enum class Kind { Empty, Ruler, Marker, TrackHeader, Clip, Gap };
        Kind kind = Kind::Empty;
        QString clipId;
        QString trackId;
        QString markerId;
        ve::Time at;
    };

    explicit TimelineWidget(QWidget* parent = nullptr);

    void setProject(const ve::Project* project);
    void refresh(); // model changed
    void setPlayhead(ve::Time t, bool follow);
    void setTool(Tool tool);
    Tool tool() const { return m_tool; }
    void setSnapping(bool on);
    bool snapping() const { return m_snapping; }
    void setMarks(std::optional<ve::Time> in, std::optional<ve::Time> out);

    std::vector<ve::Id> selectedClips() const;
    ve::Id selectedMarker() const { return m_selectedMarker.toStdString(); }
    void clearMarkerSelection();
    // Selection from elsewhere (edit index): the clips (with their partners when linked) or a marker.
    void selectClips(const std::vector<ve::Id>& clipIds);
    void selectMarker(const ve::Id& markerId);
    // The track whose header was clicked last (empty after a click elsewhere): the inspector shows it.
    ve::Id focusedTrack() const { return m_focusedTrack.toStdString(); }
    // Linked selection (on by default): clicking a clip selects its linked partners too.
    void setLinkedSelection(bool on);
    bool linkedSelection() const { return m_linkedSelection; }
    // Destination tracks of three and four point edits, picked in the track headers (by default the
    // lowest video and audio tracks).
    ve::Id destinationTrack(ve::TrackKind kind) const;

    // Where things are drawn, in widget coordinates (empty when not on screen). For tests and tools.
    QRect clipRect(const ve::Id& clipId) const;
    QRect trackHeaderRect(const ve::Id& trackId) const;
    QRect trackFlagRect(const ve::Id& trackId, TrackFlag flag) const;
    QPoint markerPoint(const ve::Id& markerId) const;
    int xForTime(ve::Time t) const;

    void zoomToFit();
    void zoomBy(double factor);

signals:
    void seekRequested(ve::Time t);
    void gestureStarted(const QString& name);
    void moveRequested(const QStringList& clipIds, ve::Time delta, int trackShift);
    void trimRequested(const QString& clipId, bool head, ve::Time newEdge, bool linked);
    void trimEditRequested(TimelineWidget::TrimMode mode, const QString& clipId, bool head, ve::Time delta, bool linked);
    void markerMoveRequested(const QString& markerId, ve::Time to);
    void gestureFinished(bool commit);
    void trackFlagRequested(const QString& trackId, TimelineWidget::TrackFlag flag, bool on);
    void mediaDropped(const QString& mediaId, ve::Time at, const QString& trackId);
    void filesDropped(const QStringList& paths, ve::Time at);
    void selectionChanged();
    void contextMenuRequested(const TimelineWidget::Context& context, const QPoint& globalPos);
    void openCompoundRequested(const QString& clipId);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
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
    enum class HitKind { None, Ruler, Marker, Header, Flag, Empty, Body, Head, Tail };
    struct Hit {
        HitKind kind = HitKind::None;
        QString clipId;
        QString trackId;
        QString markerId;
        TrackFlag flag = TrackFlag::Lock;
        Frame start = 0;
        Frame end = 0;
    };
    enum class Drag { None, Scrub, Move, Trim, Ripple, Roll, Slip, Slide, Marker };

    const ve::Timeline* timeline() const;
    ve::Rate rate() const;
    Frame frameOf(ve::Time t) const;
    ve::Time timeOf(Frame f) const;
    ve::Time span(Frame frames) const; // a signed number of frames as time
    std::vector<Row> rows() const;
    std::optional<Row> rowAt(int y) const;
    int contentBottom() const;
    int contentRight() const;
    int contentHeight() const;
    double xForFrame(double frame) const;
    Frame frameAtX(double x) const;
    double visibleFrames() const;
    Hit hitTest(const QPoint& pos) const;
    QSet<QString> groupOf(const QString& clipId) const;
    void selectGroup(const QString& clipId, bool toggle);
    std::vector<Frame> snapTargets(const QSet<QString>& exclude, const QString& excludeMarker = {}) const;
    Frame snap(Frame frame, const std::vector<Frame>& targets) const;
    // The correction that snaps the closest of `edges` (moved by `delta`) to a target.
    Frame snapEdges(const std::vector<Frame>& edges, Frame delta) const;
    void beginClipGesture(const Hit& hit, Qt::KeyboardModifiers modifiers);
    void finishGesture(bool commit);
    void setScroll(double frames);
    void setVerticalScroll(int pixels);
    void updateScrollBars();
    void pruneSelection();
    void validateDestinations();

    void paintRuler(QPainter& p);
    void paintHeaders(QPainter& p, const std::vector<Row>& rows);
    void paintClips(QPainter& p, const Row& row);

    const ve::Project* m_project = nullptr;
    QScrollBar* m_scrollBar = nullptr;
    QScrollBar* m_vScrollBar = nullptr;
    double m_pxPerFrame = 4.0;
    double m_scroll = 0.0; // first visible frame
    int m_vScroll = 0;     // pixels of tracks scrolled away at the top
    Frame m_playhead = 0;
    Tool m_tool = Tool::Select;
    bool m_snapping = true;
    std::optional<Frame> m_markIn;
    std::optional<Frame> m_markOut;
    QSet<QString> m_selected;
    QString m_selectedMarker;
    QString m_focusedTrack;
    bool m_linkedSelection = true;
    QSet<QString> m_outOfSync;
    QString m_destVideo;
    QString m_destAudio;

    // Gesture state, captured at mouse press (every step is computed from it).
    Drag m_drag = Drag::None;
    Frame m_pressFrame = 0;
    QStringList m_dragClips;
    std::vector<std::pair<Frame, Frame>> m_dragSpans; // start/end of the dragged clips at press
    Frame m_dragMinStart = 0;
    Frame m_dragDelta = 0;
    int m_trackShift = 0;
    ve::TrackKind m_pressKind = ve::TrackKind::Video;
    int m_pressKindIndex = 0;
    QString m_trimClip;
    bool m_trimHead = false;
    bool m_trimLinked = true;
    Frame m_trimStart = 0;
    Frame m_trimEnd = 0;
    Frame m_trimEdge = 0;
    QString m_dragMarker;
    Frame m_markerStart = 0;
    std::vector<Frame> m_snapTargets;
    Frame m_dropFrame = -1;
};
