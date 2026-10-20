#include "ui/TimelineWidget.h"

#include "core/Edit.h"
#include "ui/MediaPoolWidget.h"

#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kRulerHeight = 28;
constexpr int kHeaderWidth = 64;
constexpr int kTrackHeight = 58;
constexpr int kTrackSpacing = 2;
constexpr int kKindDivider = 8;
constexpr int kEdgeGrab = 6;
constexpr int kSnapPixels = 8;
constexpr double kMinPxPerFrame = 0.01;
constexpr double kMaxPxPerFrame = 40.0;

const QColor kBackground(30, 31, 34);
const QColor kHeader(40, 42, 46);
const QColor kRuler(45, 47, 52);
const QColor kLaneVideo(36, 38, 43);
const QColor kLaneAudio(34, 39, 38);
const QColor kText(214, 216, 220);
const QColor kTextDim(130, 134, 140);
const QColor kVideoClip(58, 98, 160);
const QColor kVideoClipSelected(90, 138, 212);
const QColor kAudioClip(48, 120, 86);
const QColor kAudioClipSelected(76, 168, 118);
const QColor kCompound(140, 96, 170);
const QColor kPlayhead(235, 70, 58);
const QColor kAccent(240, 196, 76);

} // namespace

TimelineWidget::TimelineWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setAcceptDrops(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumHeight(kRulerHeight + 2 * kTrackHeight + kKindDivider + 40);
    m_scrollBar = new QScrollBar(Qt::Horizontal, this);
    connect(m_scrollBar, &QScrollBar::valueChanged, this, [this](int value) {
        if (int(m_scroll) != value) {
            m_scroll = value;
            update();
        }
    });
}

// ---------------------------------------------------------------------------------------------
// Model access and units
// ---------------------------------------------------------------------------------------------

const ve::Timeline* TimelineWidget::timeline() const
{
    return m_project && !m_project->timelines.empty() ? &m_project->timeline() : nullptr;
}

ve::Rate TimelineWidget::rate() const
{
    const ve::Timeline* tl = timeline();
    return tl ? tl->rate : ve::Rate{30, 1};
}

TimelineWidget::Frame TimelineWidget::frameOf(ve::Time t) const
{
    return ve::frameAt(t, rate());
}

ve::Time TimelineWidget::timeOf(Frame f) const
{
    return ve::timeOfFrame(f, rate());
}

void TimelineWidget::setProject(const ve::Project* project)
{
    m_project = project;
    m_selected.clear();
    m_drag = Drag::None;
    refresh();
}

void TimelineWidget::refresh()
{
    pruneSelection();
    updateScrollBar();
    update();
}

void TimelineWidget::pruneSelection()
{
    const ve::Timeline* tl = timeline();
    QSet<QString> alive;
    if (tl) {
        for (const ve::Track& t : tl->tracks) {
            for (const ve::PlacedItem& p : t.clips()) {
                const QString id = QString::fromStdString(p.clip->id);
                if (m_selected.contains(id))
                    alive.insert(id);
            }
        }
    }
    if (alive != m_selected) {
        m_selected = alive;
        emit selectionChanged();
    }
}

std::vector<TimelineWidget::Row> TimelineWidget::rows() const
{
    std::vector<Row> out;
    const ve::Timeline* tl = timeline();
    if (!tl)
        return out;
    int y = kRulerHeight + 4;
    const auto video = tl->tracksOfKind(ve::TrackKind::Video);
    for (auto it = video.rbegin(); it != video.rend(); ++it) { // V2 above V1
        out.push_back({*it, y, kTrackHeight});
        y += kTrackHeight + kTrackSpacing;
    }
    y += kKindDivider;
    for (const ve::Track* t : tl->tracksOfKind(ve::TrackKind::Audio)) {
        out.push_back({t, y, kTrackHeight});
        y += kTrackHeight + kTrackSpacing;
    }
    return out;
}

int TimelineWidget::contentBottom() const
{
    return height() - m_scrollBar->sizeHint().height();
}

double TimelineWidget::xForFrame(double frame) const
{
    return kHeaderWidth + (frame - m_scroll) * m_pxPerFrame;
}

TimelineWidget::Frame TimelineWidget::frameAtX(double x) const
{
    return Frame(std::llround((x - kHeaderWidth) / m_pxPerFrame + m_scroll));
}

double TimelineWidget::visibleFrames() const
{
    return std::max(1.0, (width() - kHeaderWidth) / m_pxPerFrame);
}

std::vector<ve::Id> TimelineWidget::selectedClips() const
{
    std::vector<ve::Id> out;
    for (const QString& id : m_selected)
        out.push_back(id.toStdString());
    return out;
}

QRect TimelineWidget::clipRect(const ve::Id& clipId) const
{
    for (const Row& row : rows()) {
        if (auto placed = row.track->findClip(clipId)) {
            const int x0 = int(std::ceil(xForFrame(double(frameOf(placed->start)))));
            const int x1 = int(std::floor(xForFrame(double(frameOf(placed->end())))));
            return QRect(x0, row.top + 2, std::max(0, x1 - x0), row.height - 4).intersected(rect());
        }
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// Scrolling and zoom
// ---------------------------------------------------------------------------------------------

void TimelineWidget::updateScrollBar()
{
    const ve::Timeline* tl = timeline();
    const double total = tl ? double(frameOf(tl->duration())) : 0.0;
    const double visible = visibleFrames();
    const int maxValue = int(std::max({0.0, total + visible * 0.5 - visible, m_scroll}));
    const QSignalBlocker block(m_scrollBar);
    m_scrollBar->setRange(0, maxValue);
    m_scrollBar->setPageStep(std::max(1, int(visible)));
    m_scrollBar->setSingleStep(std::max(1, int(visible / 20)));
    m_scrollBar->setValue(int(m_scroll));
}

void TimelineWidget::setScroll(double frames)
{
    m_scroll = std::max(0.0, frames);
    updateScrollBar();
    update();
}

void TimelineWidget::zoomToFit()
{
    const ve::Timeline* tl = timeline();
    const Frame d = tl ? frameOf(tl->duration()) : 0;
    if (d > 0)
        m_pxPerFrame = std::clamp((width() - kHeaderWidth - 24) / (double(d) * 1.02), kMinPxPerFrame, kMaxPxPerFrame);
    else
        m_pxPerFrame = 4.0;
    setScroll(0.0);
}

void TimelineWidget::zoomBy(double factor)
{
    const double anchorX = std::clamp(xForFrame(double(m_playhead)), double(kHeaderWidth), double(width()));
    const double anchorFrame = (anchorX - kHeaderWidth) / m_pxPerFrame + m_scroll;
    m_pxPerFrame = std::clamp(m_pxPerFrame * factor, kMinPxPerFrame, kMaxPxPerFrame);
    setScroll(anchorFrame - (anchorX - kHeaderWidth) / m_pxPerFrame);
}

void TimelineWidget::setPlayhead(ve::Time t, bool follow)
{
    const Frame frame = frameOf(t);
    if (frame == m_playhead)
        return;
    m_playhead = frame;
    if (follow) {
        const double visible = visibleFrames();
        if (double(frame) < m_scroll || double(frame) > m_scroll + visible * 0.95)
            setScroll(double(frame) - visible * 0.05);
    }
    update();
}

void TimelineWidget::resizeEvent(QResizeEvent*)
{
    const int h = m_scrollBar->sizeHint().height();
    m_scrollBar->setGeometry(kHeaderWidth, height() - h, width() - kHeaderWidth, h);
    updateScrollBar();
}

void TimelineWidget::wheelEvent(QWheelEvent* event)
{
    const QPoint angle = event->angleDelta();
    if (event->modifiers() & Qt::ControlModifier) {
        const double x = std::max(double(kHeaderWidth), event->position().x());
        const double anchorFrame = (x - kHeaderWidth) / m_pxPerFrame + m_scroll;
        m_pxPerFrame = std::clamp(m_pxPerFrame * std::pow(1.2, angle.y() / 120.0), kMinPxPerFrame, kMaxPxPerFrame);
        setScroll(anchorFrame - (x - kHeaderWidth) / m_pxPerFrame);
    } else {
        const int steps = angle.y() != 0 ? angle.y() : angle.x();
        setScroll(m_scroll - steps / 120.0 * 80.0 / m_pxPerFrame);
    }
    event->accept();
}

// ---------------------------------------------------------------------------------------------
// Hit testing, selection, snapping
// ---------------------------------------------------------------------------------------------

TimelineWidget::Hit TimelineWidget::hitTest(const QPoint& pos) const
{
    Hit hit;
    if (pos.y() < kRulerHeight) {
        hit.kind = HitKind::Ruler;
        return hit;
    }
    if (pos.x() < kHeaderWidth || pos.y() >= contentBottom())
        return hit;
    hit.kind = HitKind::Empty;
    for (const Row& row : rows()) {
        if (pos.y() < row.top || pos.y() >= row.top + row.height)
            continue;
        for (const ve::PlacedItem& placed : row.track->clips()) {
            const Frame start = frameOf(placed.start);
            const Frame end = frameOf(placed.end());
            const double x0 = xForFrame(double(start));
            const double x1 = xForFrame(double(end));
            if (pos.x() < x0 || pos.x() > x1)
                continue;
            hit.clipId = QString::fromStdString(placed.clip->id);
            hit.start = start;
            hit.end = end;
            const bool edgesGrabbable = x1 - x0 > 3 * kEdgeGrab;
            if (edgesGrabbable && pos.x() - x0 <= kEdgeGrab)
                hit.kind = HitKind::Head;
            else if (edgesGrabbable && x1 - pos.x() <= kEdgeGrab)
                hit.kind = HitKind::Tail;
            else
                hit.kind = HitKind::Body;
            return hit;
        }
        return hit;
    }
    return hit;
}

QSet<QString> TimelineWidget::groupOf(const QString& clipId) const
{
    QSet<QString> group;
    for (const ve::Id& id : ve::edit::linkedGroup(*timeline(), clipId.toStdString()))
        group.insert(QString::fromStdString(id));
    return group;
}

void TimelineWidget::selectGroup(const QString& clipId, bool toggle)
{
    const QSet<QString> group = groupOf(clipId);
    if (toggle) {
        if (m_selected.contains(clipId))
            m_selected -= group;
        else
            m_selected += group;
    } else if (!m_selected.contains(clipId)) {
        m_selected = group;
    }
    emit selectionChanged();
    update();
}

std::vector<TimelineWidget::Frame> TimelineWidget::snapTargets(const QSet<QString>& exclude) const
{
    std::vector<Frame> targets{0, m_playhead};
    if (const ve::Timeline* tl = timeline()) {
        for (const ve::Track& t : tl->tracks) {
            for (const ve::PlacedItem& p : t.clips()) {
                if (exclude.contains(QString::fromStdString(p.clip->id)))
                    continue;
                targets.push_back(frameOf(p.start));
                targets.push_back(frameOf(p.end()));
            }
        }
    }
    return targets;
}

TimelineWidget::Frame TimelineWidget::snap(Frame frame, const std::vector<Frame>& targets) const
{
    const double threshold = kSnapPixels / m_pxPerFrame;
    Frame best = frame;
    double bestDistance = threshold;
    for (Frame t : targets) {
        const double d = std::abs(double(t - frame));
        if (d <= bestDistance) {
            bestDistance = d;
            best = t;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------------------------
// Mouse and keyboard: gestures
// ---------------------------------------------------------------------------------------------

void TimelineWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !timeline() || m_drag != Drag::None)
        return;
    const QPoint pos = event->position().toPoint();
    const Hit hit = hitTest(pos);
    m_pressFrame = frameAtX(pos.x());
    m_dragDelta = 0;
    const bool toggle = event->modifiers() & Qt::ControlModifier;

    switch (hit.kind) {
    case HitKind::Ruler:
        m_drag = Drag::Scrub;
        emit seekRequested(timeOf(std::max<Frame>(0, m_pressFrame)));
        break;
    case HitKind::Body: {
        selectGroup(hit.clipId, toggle);
        if (!m_selected.contains(hit.clipId))
            break;
        m_drag = Drag::Move;
        m_dragClips = QStringList(m_selected.begin(), m_selected.end());
        m_dragSpans.clear();
        m_dragMinStart = std::numeric_limits<Frame>::max();
        for (const ve::Track& t : timeline()->tracks) {
            for (const ve::PlacedItem& p : t.clips()) {
                if (m_selected.contains(QString::fromStdString(p.clip->id))) {
                    m_dragSpans.push_back({frameOf(p.start), frameOf(p.end())});
                    m_dragMinStart = std::min(m_dragMinStart, frameOf(p.start));
                }
            }
        }
        m_snapTargets = snapTargets(m_selected);
        emit gestureStarted(tr("Move"));
        break;
    }
    case HitKind::Head:
    case HitKind::Tail:
        selectGroup(hit.clipId, false);
        m_drag = hit.kind == HitKind::Head ? Drag::TrimHead : Drag::TrimTail;
        m_trimClip = hit.clipId;
        m_trimStart = hit.start;
        m_trimEnd = hit.end;
        m_trimEdge = hit.kind == HitKind::Head ? hit.start : hit.end;
        m_snapTargets = snapTargets(groupOf(hit.clipId));
        emit gestureStarted(tr("Trim"));
        break;
    case HitKind::Empty:
    case HitKind::None:
        if (!toggle && !m_selected.isEmpty()) {
            m_selected.clear();
            emit selectionChanged();
        }
        m_drag = Drag::Scrub;
        emit seekRequested(timeOf(std::max<Frame>(0, m_pressFrame)));
        break;
    }
    update();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event)
{
    const QPoint pos = event->position().toPoint();
    if (m_drag == Drag::None) {
        const Hit hit = hitTest(pos);
        setCursor(hit.kind == HitKind::Head || hit.kind == HitKind::Tail ? Qt::SizeHorCursor : Qt::ArrowCursor);
        return;
    }
    const Frame frame = frameAtX(pos.x());
    switch (m_drag) {
    case Drag::Scrub:
        emit seekRequested(timeOf(std::max<Frame>(0, frame)));
        break;
    case Drag::Move: {
        Frame delta = std::max(frame - m_pressFrame, -m_dragMinStart);
        // Snap whichever dragged edge is closest to a target.
        const double threshold = kSnapPixels / m_pxPerFrame;
        double best = threshold;
        Frame correction = 0;
        for (const auto& [s, e] : m_dragSpans) {
            for (Frame edge : {s + delta, e + delta}) {
                const Frame snapped = snap(edge, m_snapTargets);
                if (snapped != edge && std::abs(double(snapped - edge)) < best) {
                    best = std::abs(double(snapped - edge));
                    correction = snapped - edge;
                }
            }
        }
        delta = std::max(delta + correction, -m_dragMinStart);
        if (delta != m_dragDelta) {
            m_dragDelta = delta;
            // Live edit: the step is relative to the press, the stack merges it into the previous one.
            emit moveRequested(m_dragClips, timeOf(m_dragMinStart + delta) - timeOf(m_dragMinStart));
        }
        break;
    }
    case Drag::TrimHead:
    case Drag::TrimTail: {
        Frame edge = snap(frame, m_snapTargets);
        if (m_drag == Drag::TrimHead)
            edge = std::clamp<Frame>(edge, 0, m_trimEnd - 1);
        else
            edge = std::max(edge, m_trimStart + 1);
        if (edge != m_trimEdge) {
            m_trimEdge = edge;
            emit trimRequested(m_trimClip, m_drag == Drag::TrimHead, timeOf(edge));
        }
        break;
    }
    case Drag::None:
        break;
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        finishGesture(true);
}

void TimelineWidget::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && m_drag != Drag::None) {
        finishGesture(false); // roll the whole gesture back
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TimelineWidget::finishGesture(bool commit)
{
    const Drag drag = m_drag;
    m_drag = Drag::None;
    m_dragDelta = 0;
    if (drag == Drag::Move || drag == Drag::TrimHead || drag == Drag::TrimTail)
        emit gestureFinished(commit);
    update();
}

// ---------------------------------------------------------------------------------------------
// Drag and drop (media pool items and files from the OS)
// ---------------------------------------------------------------------------------------------

void TimelineWidget::dragEnterEvent(QDragEnterEvent* event)
{
    const QMimeData* mime = event->mimeData();
    if (mime->hasFormat(QString::fromLatin1(MediaPoolWidget::kMediaMimeType)) || mime->hasUrls())
        event->acceptProposedAction();
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* event)
{
    m_dropFrame = snap(std::max<Frame>(0, frameAtX(event->position().x())), snapTargets({}));
    update();
    event->acceptProposedAction();
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent*)
{
    m_dropFrame = -1;
    update();
}

void TimelineWidget::dropEvent(QDropEvent* event)
{
    const Frame frame = m_dropFrame >= 0 ? m_dropFrame : std::max<Frame>(0, frameAtX(event->position().x()));
    m_dropFrame = -1;
    const QMimeData* mime = event->mimeData();
    const QString mediaMime = QString::fromLatin1(MediaPoolWidget::kMediaMimeType);
    if (mime->hasFormat(mediaMime)) {
        emit mediaDropped(QString::fromUtf8(mime->data(mediaMime)), timeOf(frame));
    } else {
        QStringList paths;
        for (const QUrl& url : mime->urls()) {
            if (url.isLocalFile())
                paths << url.toLocalFile();
        }
        if (!paths.isEmpty())
            emit filesDropped(paths, timeOf(frame));
    }
    event->acceptProposedAction();
    update();
}

// ---------------------------------------------------------------------------------------------
// Painting (always the live model: gestures edit it directly)
// ---------------------------------------------------------------------------------------------

void TimelineWidget::paintRuler(QPainter& p)
{
    p.fillRect(QRect(kHeaderWidth, 0, width() - kHeaderWidth, kRulerHeight), kRuler);
    const ve::Timeline* tl = timeline();
    if (!tl)
        return;
    const Frame fps = std::max<Frame>(1, Frame(std::llround(tl->rate.toDouble())));
    const Frame steps[] = {1, 2, 5, 10, fps, 2 * fps, 5 * fps, 10 * fps, 15 * fps, 30 * fps, 60 * fps,
                           120 * fps, 300 * fps, 600 * fps, 1800 * fps, 3600 * fps};
    Frame major = steps[std::size(steps) - 1];
    for (Frame s : steps) {
        if (double(s) * m_pxPerFrame >= 96.0) {
            major = s;
            break;
        }
    }
    Frame minor = major;
    for (Frame div : {10, 5, 2}) {
        if (major % div == 0 && double(major / div) * m_pxPerFrame >= 7.0) {
            minor = major / div;
            break;
        }
    }

    QFont small = font();
    small.setPointSizeF(font().pointSizeF() * 0.85);
    p.setFont(small);
    const Frame first = Frame(std::floor(m_scroll / double(minor))) * minor;
    const double last = m_scroll + visibleFrames();
    for (Frame f = first; double(f) <= last; f += minor) {
        const double x = xForFrame(double(f));
        if (x < kHeaderWidth)
            continue;
        const bool isMajor = f % major == 0;
        p.setPen(isMajor ? kText : kTextDim);
        p.drawLine(QPointF(x, kRulerHeight - (isMajor ? 11 : 5)), QPointF(x, kRulerHeight - 1));
        if (isMajor)
            p.drawText(QPointF(x + 3, 12), QString::fromStdString(ve::formatTimecode(timeOf(f), tl->rate)));
    }
    p.setFont(font());
}

void TimelineWidget::paintClips(QPainter& p, const Row& row)
{
    const bool video = row.track->kind == ve::TrackKind::Video;
    for (const ve::PlacedItem& placed : row.track->clips()) {
        const QString id = QString::fromStdString(placed.clip->id);
        const bool selected = m_selected.contains(id);
        const double x0 = xForFrame(double(frameOf(placed.start)));
        const double x1 = xForFrame(double(frameOf(placed.end())));
        const QRectF r(x0, row.top + 2, std::max(2.0, x1 - x0), row.height - 4);

        QColor fill = video ? (selected ? kVideoClipSelected : kVideoClip) : (selected ? kAudioClipSelected : kAudioClip);
        if (!std::holds_alternative<ve::Clip>(*placed.item))
            fill = selected ? kCompound.lighter(130) : kCompound; // compound / multicam
        if (!placed.clip->enabled)
            fill = fill.darker(180);
        p.setPen(QPen(selected ? QColor(235, 238, 245) : fill.darker(150), 1));
        p.setBrush(fill);
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        if (!video) { // stand-in for a waveform until the peak cache exists
            p.setPen(QPen(fill.lighter(140), 1));
            p.drawLine(QPointF(r.left() + 2, r.center().y()), QPointF(r.right() - 2, r.center().y()));
        }
        if (r.width() > 28) {
            p.setPen(kText);
            const QRectF textRect = r.adjusted(6, 3, -6, -3);
            p.drawText(textRect, Qt::AlignLeft | Qt::AlignTop,
                       p.fontMetrics().elidedText(QString::fromStdString(placed.clip->name), Qt::ElideRight,
                                                  int(textRect.width())));
        }
    }
}

void TimelineWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), kBackground);
    const auto allRows = rows();
    const int bottom = contentBottom();

    for (const Row& r : allRows)
        p.fillRect(QRect(kHeaderWidth, r.top, width() - kHeaderWidth, r.height),
                   r.track->kind == ve::TrackKind::Video ? kLaneVideo : kLaneAudio);

    p.save();
    p.setClipRect(QRect(kHeaderWidth, kRulerHeight, width() - kHeaderWidth, bottom - kRulerHeight));
    for (const Row& r : allRows)
        paintClips(p, r);
    if (m_dropFrame >= 0) {
        const double x = xForFrame(double(m_dropFrame));
        p.setPen(QPen(kAccent, 2));
        p.drawLine(QPointF(x, kRulerHeight), QPointF(x, bottom));
    }
    if (!timeline() || !timeline()->hasClips()) {
        p.setPen(kTextDim);
        p.drawText(QRect(kHeaderWidth, kRulerHeight, width() - kHeaderWidth, bottom - kRulerHeight), Qt::AlignCenter,
                   tr("Import media (Ctrl+I) or drop files here"));
    }
    p.restore();

    // Track headers (a lock marker for locked tracks)
    p.fillRect(QRect(0, kRulerHeight, kHeaderWidth, bottom - kRulerHeight), kHeader);
    QFont bold = font();
    bold.setBold(true);
    p.setFont(bold);
    for (const Row& r : allRows) {
        p.setPen(r.track->enabled ? kText : kTextDim);
        QString label = QString::fromStdString(r.track->name);
        if (r.track->locked)
            label += QStringLiteral(" ■");
        p.drawText(QRect(10, r.top, kHeaderWidth - 14, r.height), Qt::AlignVCenter | Qt::AlignLeft, label);
    }
    p.setFont(font());

    paintRuler(p);
    p.fillRect(QRect(0, 0, kHeaderWidth, kRulerHeight), kHeader);

    // Playhead
    const double px = xForFrame(double(m_playhead));
    if (px >= kHeaderWidth && px <= width()) {
        p.setPen(QPen(kPlayhead, 1.5));
        p.drawLine(QPointF(px, kRulerHeight - 6), QPointF(px, bottom));
        p.setPen(Qt::NoPen);
        p.setBrush(kPlayhead);
        const QPointF head[] = {{px - 6, 2}, {px + 6, 2}, {px + 6, kRulerHeight - 12.0}, {px, kRulerHeight - 5.0},
                                {px - 6, kRulerHeight - 12.0}};
        p.drawPolygon(head, int(std::size(head)));
    }
}
