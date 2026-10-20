#include "ui/TimelineWidget.h"

#include "core/Edit.h"
#include "ui/LabelColors.h"
#include "ui/MediaPoolWidget.h"

#include <QContextMenuEvent>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QToolTip>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kRulerHeight = 28;
constexpr int kHeaderWidth = 104;
constexpr int kTrackHeight = 50;
constexpr int kTrackSpacing = 2;
constexpr int kKindDivider = 8;
constexpr int kEdgeGrab = 6;
constexpr int kSnapPixels = 8;
constexpr int kMarkerGrab = 5;
constexpr int kFlagSize = 18;
constexpr int kLockX = 56;
constexpr int kSyncX = 80;
constexpr double kMinPxPerFrame = 0.01;
constexpr double kMaxPxPerFrame = 40.0;

const QColor kBackground(30, 31, 34);
const QColor kHeader(40, 42, 46);
const QColor kRuler(45, 47, 52);
const QColor kLaneVideo(36, 38, 43);
const QColor kLaneAudio(34, 39, 38);
const QColor kLaneLocked(44, 40, 40);
const QColor kText(214, 216, 220);
const QColor kTextDim(130, 134, 140);
const QColor kVideoClip(58, 98, 160);
const QColor kVideoClipSelected(90, 138, 212);
const QColor kAudioClip(48, 120, 86);
const QColor kAudioClipSelected(76, 168, 118);
const QColor kCompound(140, 96, 170);
const QColor kPlayhead(235, 70, 58);
const QColor kAccent(240, 196, 76);
const QColor kOutOfSync(230, 60, 60);
const QColor kFlagOn(214, 160, 60);

QColor markerColor(const std::string& name)
{
    return labels::colorOf(name, QColor(70, 130, 230));
}

void drawLock(QPainter& p, const QRectF& box, bool on)
{
    const QColor ink = on ? QColor(30, 31, 34) : kTextDim;
    const QRectF body(box.center().x() - 5, box.center().y() - 1, 10, 7);
    p.setPen(QPen(ink, 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(body.left() + 2, body.top() - 6, 6, 9), 0, 180 * 16);
    p.setBrush(ink);
    p.drawRect(body);
}

void drawChain(QPainter& p, const QRectF& box, bool on)
{
    const QColor ink = on ? QColor(30, 31, 34) : kTextDim;
    p.setPen(QPen(ink, 1.6));
    p.setBrush(Qt::NoBrush);
    const QPointF c = box.center();
    p.drawRoundedRect(QRectF(c.x() - 7, c.y() - 3, 8, 6), 3, 3);
    p.drawRoundedRect(QRectF(c.x() - 1, c.y() - 3, 8, 6), 3, 3);
}

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
    m_vScrollBar = new QScrollBar(Qt::Vertical, this);
    connect(m_vScrollBar, &QScrollBar::valueChanged, this, [this](int value) {
        if (m_vScroll != value) {
            m_vScroll = value;
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

ve::Time TimelineWidget::span(Frame frames) const
{
    return frames >= 0 ? timeOf(frames) : -timeOf(-frames);
}

void TimelineWidget::setProject(const ve::Project* project)
{
    m_project = project;
    m_selected.clear();
    m_selectedMarker.clear();
    m_focusedTrack.clear();
    m_drag = Drag::None;
    m_destVideo.clear();
    m_destAudio.clear();
    m_vScroll = 0;
    refresh();
}

void TimelineWidget::refresh()
{
    pruneSelection();
    validateDestinations();
    m_outOfSync.clear();
    if (const ve::Timeline* tl = timeline()) {
        for (const ve::Id& id : ve::edit::outOfSyncClips(*tl))
            m_outOfSync.insert(QString::fromStdString(id));
    }
    updateScrollBars();
    update();
}

void TimelineWidget::pruneSelection()
{
    const ve::Timeline* tl = timeline();
    QSet<QString> alive;
    bool markerAlive = false;
    if (tl) {
        for (const ve::Track& t : tl->tracks) {
            for (const ve::PlacedItem& p : t.clips()) {
                const QString id = QString::fromStdString(p.clip->id);
                if (m_selected.contains(id))
                    alive.insert(id);
            }
        }
        markerAlive = ve::edit::findMarker(*tl, m_selectedMarker.toStdString()) != nullptr;
        if (!m_focusedTrack.isEmpty() && !tl->track(m_focusedTrack.toStdString())) {
            m_focusedTrack.clear();
            emit selectionChanged();
        }
    }
    if (!markerAlive && !m_selectedMarker.isEmpty()) {
        m_selectedMarker.clear();
        emit selectionChanged();
    }
    if (alive != m_selected) {
        m_selected = alive;
        emit selectionChanged();
    }
}

void TimelineWidget::validateDestinations()
{
    const ve::Timeline* tl = timeline();
    if (!tl)
        return;
    auto valid = [&](const QString& id, ve::TrackKind kind) {
        const ve::Track* t = tl->track(id.toStdString());
        return t && t->kind == kind;
    };
    for (auto [dest, kind] : {std::pair{&m_destVideo, ve::TrackKind::Video}, std::pair{&m_destAudio, ve::TrackKind::Audio}}) {
        if (valid(*dest, kind))
            continue;
        const auto tracks = tl->tracksOfKind(kind);
        *dest = tracks.empty() ? QString() : QString::fromStdString(tracks.front()->id);
    }
}

ve::Id TimelineWidget::destinationTrack(ve::TrackKind kind) const
{
    return (kind == ve::TrackKind::Video ? m_destVideo : m_destAudio).toStdString();
}

void TimelineWidget::setTool(Tool tool)
{
    m_tool = tool;
    setCursor(Qt::ArrowCursor);
}

void TimelineWidget::setSnapping(bool on)
{
    m_snapping = on;
}

void TimelineWidget::setMarks(std::optional<ve::Time> in, std::optional<ve::Time> out)
{
    m_markIn = in ? std::optional<Frame>(frameOf(*in)) : std::nullopt;
    m_markOut = out ? std::optional<Frame>(frameOf(*out)) : std::nullopt;
    update();
}

void TimelineWidget::clearMarkerSelection()
{
    if (m_selectedMarker.isEmpty())
        return;
    m_selectedMarker.clear();
    emit selectionChanged();
    update();
}

// ---------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------

std::vector<TimelineWidget::Row> TimelineWidget::rows() const
{
    std::vector<Row> out;
    const ve::Timeline* tl = timeline();
    if (!tl)
        return out;
    int y = kRulerHeight + 4 - m_vScroll;
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

std::optional<TimelineWidget::Row> TimelineWidget::rowAt(int y) const
{
    for (const Row& r : rows()) {
        if (y >= r.top && y < r.top + r.height + kTrackSpacing)
            return r;
    }
    return std::nullopt;
}

int TimelineWidget::contentHeight() const
{
    const ve::Timeline* tl = timeline();
    if (!tl)
        return 0;
    return 4 + int(tl->tracks.size()) * (kTrackHeight + kTrackSpacing) + kKindDivider + 8;
}

int TimelineWidget::contentBottom() const
{
    return height() - m_scrollBar->sizeHint().height();
}

int TimelineWidget::contentRight() const
{
    return width() - (m_vScrollBar->isHidden() ? 0 : m_vScrollBar->sizeHint().width());
}

double TimelineWidget::xForFrame(double frame) const
{
    return kHeaderWidth + (frame - m_scroll) * m_pxPerFrame;
}

int TimelineWidget::xForTime(ve::Time t) const
{
    return int(std::lround(xForFrame(double(frameOf(t)))));
}

TimelineWidget::Frame TimelineWidget::frameAtX(double x) const
{
    return Frame(std::llround((x - kHeaderWidth) / m_pxPerFrame + m_scroll));
}

double TimelineWidget::visibleFrames() const
{
    return std::max(1.0, (contentRight() - kHeaderWidth) / m_pxPerFrame);
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
            return QRect(x0, row.top + 2, std::max(0, x1 - x0), row.height - 4)
                .intersected(QRect(kHeaderWidth, kRulerHeight, contentRight() - kHeaderWidth, contentBottom() - kRulerHeight));
        }
    }
    return {};
}

QRect TimelineWidget::trackHeaderRect(const ve::Id& trackId) const
{
    for (const Row& row : rows()) {
        if (row.track->id == trackId)
            return QRect(0, row.top, kHeaderWidth, row.height);
    }
    return {};
}

QRect TimelineWidget::trackFlagRect(const ve::Id& trackId, TrackFlag flag) const
{
    const QRect header = trackHeaderRect(trackId);
    if (header.isEmpty())
        return {};
    return QRect(flag == TrackFlag::Lock ? kLockX : kSyncX, header.top() + (header.height() - kFlagSize) / 2, kFlagSize,
                 kFlagSize);
}

QPoint TimelineWidget::markerPoint(const ve::Id& markerId) const
{
    const ve::Timeline* tl = timeline();
    const ve::Marker* m = tl ? ve::edit::findMarker(*tl, markerId) : nullptr;
    if (!m)
        return {};
    return QPoint(int(std::lround(xForFrame(double(frameOf(m->start))))), kRulerHeight - 8);
}

// ---------------------------------------------------------------------------------------------
// Scrolling and zoom
// ---------------------------------------------------------------------------------------------

void TimelineWidget::updateScrollBars()
{
    const int visibleTracks = contentBottom() - kRulerHeight;
    const int extra = std::max(0, contentHeight() - visibleTracks);
    m_vScrollBar->setVisible(extra > 0);
    {
        const QSignalBlocker block(m_vScrollBar);
        m_vScrollBar->setRange(0, extra);
        m_vScrollBar->setPageStep(std::max(1, visibleTracks));
        m_vScrollBar->setSingleStep(kTrackHeight / 2);
        m_vScroll = std::clamp(m_vScroll, 0, extra);
        m_vScrollBar->setValue(m_vScroll);
    }
    const int vw = m_vScrollBar->sizeHint().width();
    m_vScrollBar->setGeometry(width() - vw, kRulerHeight, vw, contentBottom() - kRulerHeight);
    const int h = m_scrollBar->sizeHint().height();
    m_scrollBar->setGeometry(kHeaderWidth, height() - h, contentRight() - kHeaderWidth, h);

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
    updateScrollBars();
    update();
}

void TimelineWidget::setVerticalScroll(int pixels)
{
    m_vScroll = pixels;
    updateScrollBars();
    update();
}

void TimelineWidget::zoomToFit()
{
    const ve::Timeline* tl = timeline();
    const Frame d = tl ? frameOf(tl->duration()) : 0;
    if (d > 0)
        m_pxPerFrame = std::clamp((contentRight() - kHeaderWidth - 24) / (double(d) * 1.02), kMinPxPerFrame, kMaxPxPerFrame);
    else
        m_pxPerFrame = 4.0;
    setScroll(0.0);
}

void TimelineWidget::zoomBy(double factor)
{
    const double anchorX = std::clamp(xForFrame(double(m_playhead)), double(kHeaderWidth), double(contentRight()));
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
    updateScrollBars();
}

void TimelineWidget::wheelEvent(QWheelEvent* event)
{
    const QPoint angle = event->angleDelta();
    if (event->modifiers() & Qt::ControlModifier) {
        const double x = std::max(double(kHeaderWidth), event->position().x());
        const double anchorFrame = (x - kHeaderWidth) / m_pxPerFrame + m_scroll;
        m_pxPerFrame = std::clamp(m_pxPerFrame * std::pow(1.2, angle.y() / 120.0), kMinPxPerFrame, kMaxPxPerFrame);
        setScroll(anchorFrame - (x - kHeaderWidth) / m_pxPerFrame);
    } else if (event->modifiers() & Qt::ShiftModifier) {
        const int steps = angle.y() != 0 ? angle.y() : angle.x();
        setVerticalScroll(m_vScroll - steps / 120 * kTrackHeight);
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
    const ve::Timeline* tl = timeline();
    if (pos.y() < kRulerHeight) {
        hit.kind = HitKind::Ruler;
        if (tl && pos.x() >= kHeaderWidth) {
            for (const ve::Marker& m : tl->markers) {
                if (std::abs(xForFrame(double(frameOf(m.start))) - pos.x()) <= kMarkerGrab) {
                    hit.kind = HitKind::Marker;
                    hit.markerId = QString::fromStdString(m.id);
                    hit.start = frameOf(m.start);
                }
            }
        }
        return hit;
    }
    if (pos.y() >= contentBottom() || pos.x() >= contentRight())
        return hit;
    const std::optional<Row> row = rowAt(pos.y());
    if (pos.x() < kHeaderWidth) {
        if (!row || pos.y() >= row->top + row->height)
            return hit;
        hit.trackId = QString::fromStdString(row->track->id);
        hit.kind = HitKind::Header;
        for (TrackFlag flag : {TrackFlag::Lock, TrackFlag::SyncLock}) {
            if (trackFlagRect(row->track->id, flag).contains(pos)) {
                hit.kind = HitKind::Flag;
                hit.flag = flag;
            }
        }
        return hit;
    }
    hit.kind = HitKind::Empty;
    if (!row || pos.y() >= row->top + row->height)
        return hit;
    hit.trackId = QString::fromStdString(row->track->id);
    if (row->track->locked)
        return hit; // clips on locked tracks cannot be picked
    for (const ve::PlacedItem& placed : row->track->clips()) {
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

QSet<QString> TimelineWidget::groupOf(const QString& clipId) const
{
    if (!m_linkedSelection)
        return {clipId};
    QSet<QString> group;
    for (const ve::Id& id : ve::edit::linkedGroup(*timeline(), clipId.toStdString()))
        group.insert(QString::fromStdString(id));
    return group;
}

void TimelineWidget::setLinkedSelection(bool on)
{
    m_linkedSelection = on;
}

void TimelineWidget::selectClips(const std::vector<ve::Id>& clipIds)
{
    QSet<QString> selected;
    for (const ve::Id& id : clipIds)
        selected += groupOf(QString::fromStdString(id));
    m_selected = selected;
    m_selectedMarker.clear();
    m_focusedTrack.clear();
    emit selectionChanged();
    update();
}

void TimelineWidget::selectMarker(const ve::Id& markerId)
{
    m_selected.clear();
    m_selectedMarker = QString::fromStdString(markerId);
    m_focusedTrack.clear();
    emit selectionChanged();
    update();
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

std::vector<TimelineWidget::Frame> TimelineWidget::snapTargets(const QSet<QString>& exclude,
                                                                const QString& excludeMarker) const
{
    std::vector<Frame> targets{0, m_playhead};
    if (m_markIn)
        targets.push_back(*m_markIn);
    if (m_markOut)
        targets.push_back(*m_markOut);
    if (const ve::Timeline* tl = timeline()) {
        for (const ve::Track& t : tl->tracks) {
            for (const ve::PlacedItem& p : t.clips()) {
                if (exclude.contains(QString::fromStdString(p.clip->id)))
                    continue;
                targets.push_back(frameOf(p.start));
                targets.push_back(frameOf(p.end()));
            }
        }
        for (const ve::Marker& m : tl->markers) {
            if (QString::fromStdString(m.id) == excludeMarker)
                continue;
            targets.push_back(frameOf(m.start));
            if (m.duration > ve::Time(0))
                targets.push_back(frameOf(m.start + m.duration));
        }
    }
    return targets;
}

TimelineWidget::Frame TimelineWidget::snap(Frame frame, const std::vector<Frame>& targets) const
{
    if (!m_snapping)
        return frame;
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

TimelineWidget::Frame TimelineWidget::snapEdges(const std::vector<Frame>& edges, Frame delta) const
{
    const double threshold = kSnapPixels / m_pxPerFrame;
    double best = threshold;
    Frame correction = 0;
    for (Frame edge : edges) {
        const Frame moved = edge + delta;
        const Frame snapped = snap(moved, m_snapTargets);
        if (snapped != moved && std::abs(double(snapped - moved)) < best) {
            best = std::abs(double(snapped - moved));
            correction = snapped - moved;
        }
    }
    return correction;
}

bool TimelineWidget::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const Hit hit = hitTest(help->pos());
        QString tip;
        if (hit.kind == HitKind::Flag)
            tip = hit.flag == TrackFlag::Lock ? tr("Lock the track: no edit changes it")
                                              : tr("Sync lock: ripple edits and inserts shift this track too");
        else if (hit.kind == HitKind::Header)
            tip = tr("Click to make this the destination of insert/overwrite edits");
        else if (hit.kind == HitKind::Marker) {
            if (const ve::Marker* m = ve::edit::findMarker(*timeline(), hit.markerId.toStdString()))
                tip = m->name.empty() ? tr("Marker") : QString::fromStdString(m->name);
        }
        if (tip.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(help->globalPos(), tip, this);
        return true;
    }
    return QWidget::event(event);
}

// ---------------------------------------------------------------------------------------------
// Mouse and keyboard: gestures
// ---------------------------------------------------------------------------------------------

void TimelineWidget::beginClipGesture(const Hit& hit, Qt::KeyboardModifiers modifiers)
{
    const bool shift = modifiers & Qt::ShiftModifier;
    const bool ctrl = modifiers & Qt::ControlModifier;
    const bool alt = modifiers & Qt::AltModifier;
    m_trimClip = hit.clipId;
    m_trimStart = hit.start;
    m_trimEnd = hit.end;
    m_trimHead = hit.kind == HitKind::Head;
    m_trimLinked = !alt && m_linkedSelection;
    m_dragSpans = {{hit.start, hit.end}};
    m_snapTargets = snapTargets(groupOf(hit.clipId));

    if (hit.kind == HitKind::Body) { // trim tool only: slip or slide
        selectGroup(hit.clipId, false);
        m_drag = shift ? Drag::Slide : Drag::Slip;
        emit gestureStarted(m_drag == Drag::Slide ? tr("Slide") : tr("Slip"));
        return;
    }
    selectGroup(hit.clipId, false);
    if (ctrl)
        m_drag = Drag::Roll;
    else if (m_tool == Tool::Trim || shift)
        m_drag = Drag::Ripple;
    else
        m_drag = Drag::Trim;
    m_trimEdge = m_trimHead ? hit.start : hit.end;
    emit gestureStarted(m_drag == Drag::Roll ? tr("Roll") : m_drag == Drag::Ripple ? tr("Ripple") : tr("Trim"));
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* event)
{
    if (!timeline() || m_drag != Drag::None)
        return;
    const QPoint pos = event->pos();
    const Hit hit = hitTest(pos);
    Context ctx;
    ctx.at = timeOf(std::max<Frame>(0, frameAtX(pos.x())));
    ctx.trackId = hit.trackId;
    switch (hit.kind) {
    case HitKind::Ruler:
        ctx.kind = Context::Kind::Ruler;
        break;
    case HitKind::Marker:
        ctx.kind = Context::Kind::Marker;
        ctx.markerId = hit.markerId;
        selectMarker(hit.markerId.toStdString());
        break;
    case HitKind::Header:
    case HitKind::Flag:
        ctx.kind = Context::Kind::TrackHeader;
        m_focusedTrack = hit.trackId;
        emit selectionChanged();
        break;
    case HitKind::Body:
    case HitKind::Head:
    case HitKind::Tail:
        ctx.kind = Context::Kind::Clip;
        ctx.clipId = hit.clipId;
        if (!m_selected.contains(hit.clipId))
            selectGroup(hit.clipId, false); // like other editors: the right-clicked clip becomes the selection
        clearMarkerSelection();
        break;
    case HitKind::Empty: {
        ctx.kind = Context::Kind::Empty;
        // Inside a track, between two clips: a gap.
        if (const ve::Track* t = timeline()->track(hit.trackId.toStdString()); t && ctx.at < t->duration())
            ctx.kind = Context::Kind::Gap;
        break;
    }
    case HitKind::None:
        return;
    }
    emit contextMenuRequested(ctx, event->globalPos());
    event->accept();
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !timeline())
        return;
    const Hit hit = hitTest(event->position().toPoint());
    if (hit.kind != HitKind::Body)
        return;
    for (const ve::Track& t : timeline()->tracks) {
        if (auto placed = t.findClip(hit.clipId.toStdString());
            placed && std::holds_alternative<ve::CompoundClip>(*placed->item))
            emit openCompoundRequested(hit.clipId);
    }
}

void TimelineWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !timeline() || m_drag != Drag::None)
        return;
    const QPoint pos = event->position().toPoint();
    const Hit hit = hitTest(pos);
    m_pressFrame = frameAtX(pos.x());
    m_dragDelta = 0;
    m_trackShift = 0;
    const bool toggle = event->modifiers() & Qt::ControlModifier;
    if (hit.kind != HitKind::Header && hit.kind != HitKind::Flag && !m_focusedTrack.isEmpty()) {
        m_focusedTrack.clear();
        emit selectionChanged();
    }

    switch (hit.kind) {
    case HitKind::Ruler:
        m_drag = Drag::Scrub;
        emit seekRequested(timeOf(std::max<Frame>(0, m_pressFrame)));
        break;
    case HitKind::Marker:
        // The last thing clicked is what Delete removes: a marker or clips, not both.
        m_selected.clear();
        m_selectedMarker = hit.markerId;
        emit selectionChanged();
        m_drag = Drag::Marker;
        m_dragMarker = hit.markerId;
        m_markerStart = hit.start;
        m_trimEdge = hit.start;
        m_snapTargets = snapTargets({}, hit.markerId);
        emit gestureStarted(tr("Move Marker"));
        break;
    case HitKind::Flag: {
        const ve::Track* t = timeline()->track(hit.trackId.toStdString());
        if (t)
            emit trackFlagRequested(hit.trackId, hit.flag, hit.flag == TrackFlag::Lock ? !t->locked : !t->syncLock);
        break;
    }
    case HitKind::Header: {
        const ve::Track* t = timeline()->track(hit.trackId.toStdString());
        if (t) {
            (t->kind == ve::TrackKind::Video ? m_destVideo : m_destAudio) = hit.trackId;
            m_focusedTrack = hit.trackId; // the inspector shows the track
            emit selectionChanged();
        }
        break;
    }
    case HitKind::Body:
        clearMarkerSelection();
        if (m_tool == Tool::Trim) {
            beginClipGesture(hit, event->modifiers());
            break;
        }
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
        if (const ve::Track* t = timeline()->track(hit.trackId.toStdString())) {
            m_pressKind = t->kind;
            m_pressKindIndex = ve::edit::kindIndex(*timeline(), t->id);
        }
        m_snapTargets = snapTargets(m_selected);
        emit gestureStarted(tr("Move"));
        break;
    case HitKind::Head:
    case HitKind::Tail:
        clearMarkerSelection();
        beginClipGesture(hit, event->modifiers());
        break;
    case HitKind::Empty:
    case HitKind::None:
        if (!toggle && !m_selected.isEmpty()) {
            m_selected.clear();
            emit selectionChanged();
        }
        clearMarkerSelection();
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
        Qt::CursorShape shape = Qt::ArrowCursor;
        if (hit.kind == HitKind::Head || hit.kind == HitKind::Tail || hit.kind == HitKind::Marker)
            shape = Qt::SizeHorCursor;
        else if (hit.kind == HitKind::Body && m_tool == Tool::Trim)
            shape = Qt::SplitHCursor;
        else if (hit.kind == HitKind::Flag || hit.kind == HitKind::Header)
            shape = Qt::PointingHandCursor;
        setCursor(shape);
        return;
    }
    const Frame frame = frameAtX(pos.x());
    const Frame moved = frame - m_pressFrame;
    switch (m_drag) {
    case Drag::Scrub:
        emit seekRequested(timeOf(std::max<Frame>(0, frame)));
        break;
    case Drag::Move: {
        Frame delta = std::max(moved, -m_dragMinStart);
        std::vector<Frame> edges;
        for (const auto& [s, e] : m_dragSpans) {
            edges.push_back(s);
            edges.push_back(e);
        }
        delta = std::max(delta + snapEdges(edges, delta), -m_dragMinStart);
        int shift = m_trackShift;
        if (const std::optional<Row> row = rowAt(pos.y()); row && row->track->kind == m_pressKind)
            shift = ve::edit::kindIndex(*timeline(), row->track->id) - m_pressKindIndex;
        if (delta != m_dragDelta || shift != m_trackShift) {
            m_dragDelta = delta;
            m_trackShift = shift;
            // Live edit: the step is relative to the press, the stack merges it into the previous one.
            emit moveRequested(m_dragClips, timeOf(m_dragMinStart + delta) - timeOf(m_dragMinStart), shift);
        }
        break;
    }
    case Drag::Trim: {
        Frame edge = snap(frame, m_snapTargets);
        if (m_trimHead)
            edge = std::clamp<Frame>(edge, 0, m_trimEnd - 1);
        else
            edge = std::max(edge, m_trimStart + 1);
        if (edge != m_trimEdge) {
            m_trimEdge = edge;
            emit trimRequested(m_trimClip, m_trimHead, timeOf(edge), m_trimLinked);
        }
        break;
    }
    case Drag::Ripple:
    case Drag::Roll: {
        const Frame base = m_trimHead ? m_trimStart : m_trimEnd;
        const Frame delta = snap(base + moved, m_snapTargets) - base;
        if (delta != m_dragDelta) {
            m_dragDelta = delta;
            emit trimEditRequested(m_drag == Drag::Roll ? TrimMode::Roll : TrimMode::Ripple, m_trimClip, m_trimHead,
                                   span(delta), m_trimLinked);
        }
        break;
    }
    case Drag::Slip:
        // The media follows the mouse: dragging right shows earlier frames.
        if (moved != m_dragDelta) {
            m_dragDelta = moved;
            emit trimEditRequested(TrimMode::Slip, m_trimClip, false, span(-moved), true);
        }
        break;
    case Drag::Slide: {
        const Frame delta = moved + snapEdges({m_trimStart, m_trimEnd}, moved);
        if (delta != m_dragDelta) {
            m_dragDelta = delta;
            emit trimEditRequested(TrimMode::Slide, m_trimClip, false, span(delta), true);
        }
        break;
    }
    case Drag::Marker: {
        const Frame to = snap(std::max<Frame>(0, m_markerStart + moved), m_snapTargets);
        if (to != m_trimEdge) {
            m_trimEdge = to;
            emit markerMoveRequested(m_dragMarker, timeOf(to));
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
    m_trackShift = 0;
    if (drag != Drag::None && drag != Drag::Scrub)
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
    const QPoint pos = event->position().toPoint();
    const Frame frame = m_dropFrame >= 0 ? m_dropFrame : std::max<Frame>(0, frameAtX(pos.x()));
    m_dropFrame = -1;
    const QMimeData* mime = event->mimeData();
    const QString mediaMime = QString::fromLatin1(MediaPoolWidget::kMediaMimeType);
    if (mime->hasFormat(mediaMime)) {
        std::optional<Row> row;
        if (pos.y() >= kRulerHeight)
            row = rowAt(pos.y());
        emit mediaDropped(QString::fromUtf8(mime->data(mediaMime)), timeOf(frame),
                          row ? QString::fromStdString(row->track->id) : QString());
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
    p.fillRect(QRect(kHeaderWidth, 0, contentRight() - kHeaderWidth, kRulerHeight), kRuler);
    const ve::Timeline* tl = timeline();
    if (!tl)
        return;

    // In/Out range
    if (m_markIn || m_markOut) {
        const double x0 = xForFrame(double(m_markIn.value_or(0)));
        const double x1 = xForFrame(double(m_markOut.value_or(std::max(frameOf(tl->duration()), m_markIn.value_or(0)))));
        QColor band = kAccent;
        band.setAlpha(60);
        p.fillRect(QRectF(x0, kRulerHeight - 9, std::max(1.0, x1 - x0), 9), band);
        p.setPen(QPen(kAccent, 2));
        if (m_markIn)
            p.drawLine(QPointF(x0, kRulerHeight - 12), QPointF(x0, kRulerHeight));
        if (m_markOut)
            p.drawLine(QPointF(x1, kRulerHeight - 12), QPointF(x1, kRulerHeight));
    }

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

    // Markers: a flag on the ruler (and a bar for markers with a duration).
    for (const ve::Marker& m : tl->markers) {
        const double x = xForFrame(double(frameOf(m.start)));
        if (x < kHeaderWidth - kMarkerGrab || x > contentRight() + kMarkerGrab)
            continue;
        const QColor color = markerColor(m.color);
        if (m.duration > ve::Time(0)) {
            QColor bar = color;
            bar.setAlpha(110);
            const double x1 = xForFrame(double(frameOf(m.start + m.duration)));
            p.fillRect(QRectF(x, kRulerHeight - 6, x1 - x, 4), bar);
        }
        const bool selected = QString::fromStdString(m.id) == m_selectedMarker;
        const QPointF flag[] = {{x - 5, kRulerHeight - 16.0}, {x + 5, kRulerHeight - 16.0}, {x + 5, kRulerHeight - 9.0},
                                {x, kRulerHeight - 4.0},      {x - 5, kRulerHeight - 9.0}};
        p.setPen(selected ? QPen(Qt::white, 1.5) : QPen(color.darker(140), 1));
        p.setBrush(color);
        p.drawPolygon(flag, int(std::size(flag)));
    }
}

void TimelineWidget::paintHeaders(QPainter& p, const std::vector<Row>& rows)
{
    p.fillRect(QRect(0, kRulerHeight, kHeaderWidth, contentBottom() - kRulerHeight), kHeader);
    QFont bold = font();
    bold.setBold(true);
    for (const Row& r : rows) {
        const QString id = QString::fromStdString(r.track->id);
        const bool destination = id == m_destVideo || id == m_destAudio;
        if (destination)
            p.fillRect(QRect(0, r.top, 4, r.height), kAccent);
        if (id == m_focusedTrack)
            p.fillRect(QRect(4, r.top, kHeaderWidth - 4, r.height), QColor(255, 255, 255, 18));
        if (!r.track->color.empty())
            p.fillRect(QRect(kHeaderWidth - 4, r.top, 3, r.height), labels::colorOf(r.track->color, kHeader));
        p.setFont(bold);
        p.setPen(r.track->enabled ? kText : kTextDim);
        p.drawText(QRect(10, r.top, kLockX - 12, r.height), Qt::AlignVCenter | Qt::AlignLeft,
                   QString::fromStdString(r.track->name));
        p.setFont(font());
        for (TrackFlag flag : {TrackFlag::Lock, TrackFlag::SyncLock}) {
            const QRectF box = trackFlagRect(r.track->id, flag);
            const bool on = flag == TrackFlag::Lock ? r.track->locked : r.track->syncLock;
            p.setPen(QPen(on ? kFlagOn : QColor(70, 73, 80), 1));
            p.setBrush(on ? kFlagOn : QColor(48, 50, 55));
            p.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
            if (flag == TrackFlag::Lock)
                drawLock(p, box, on);
            else
                drawChain(p, box, on);
        }
    }
}

void TimelineWidget::paintClips(QPainter& p, const Row& row)
{
    const bool video = row.track->kind == ve::TrackKind::Video;
    for (const ve::PlacedItem& placed : row.track->clips()) {
        const QString id = QString::fromStdString(placed.clip->id);
        const bool selected = m_selected.contains(id);
        const double x0 = xForFrame(double(frameOf(placed.start)));
        const double x1 = xForFrame(double(frameOf(placed.end())));
        if (x1 < kHeaderWidth || x0 > contentRight())
            continue;
        const QRectF r(x0, row.top + 2, std::max(2.0, x1 - x0), row.height - 4);

        QColor fill = video ? (selected ? kVideoClipSelected : kVideoClip) : (selected ? kAudioClipSelected : kAudioClip);
        if (!std::holds_alternative<ve::Clip>(*placed.item))
            fill = selected ? kCompound.lighter(130) : kCompound; // compound / multicam
        if (!placed.clip->color.empty()) {
            fill = labels::colorOf(placed.clip->color, fill).darker(video ? 115 : 135);
            if (selected)
                fill = fill.lighter(135);
        }
        if (!placed.clip->enabled)
            fill = fill.darker(180);
        if (row.track->locked)
            fill = fill.darker(130);
        p.setPen(QPen(selected ? QColor(235, 238, 245) : fill.darker(150), 1));
        p.setBrush(fill);
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        if (!video) { // stand-in for a waveform until the peak cache exists
            p.setPen(QPen(fill.lighter(140), 1));
            p.drawLine(QPointF(r.left() + 2, r.center().y()), QPointF(r.right() - 2, r.center().y()));
        }
        if (m_outOfSync.contains(id)) // like the red offset badges of other editors
            p.fillRect(QRectF(r.left() + 1, r.top() + 1, r.width() - 2, 3), kOutOfSync);
        // Flags: small coloured pennants in the top right corner.
        double fx = r.right() - 4;
        for (auto it = placed.clip->flags.rbegin(); it != placed.clip->flags.rend() && fx > r.left() + 30; ++it) {
            const QPointF pennant[] = {{fx - 7, r.top() + 3}, {fx, r.top() + 3}, {fx - 3.5, r.top() + 10}};
            p.setPen(Qt::NoPen);
            p.setBrush(labels::colorOf(*it, Qt::white));
            p.drawPolygon(pennant, 3);
            fx -= 9;
        }
        if (r.width() > 28) {
            QString label = QString::fromStdString(placed.clip->name);
            if (const ve::Clip* c = std::get_if<ve::Clip>(placed.item); c && !c->retime.isIdentity() && c->retime.isValid())
                label += QStringLiteral(" (%1%)").arg(100.0 * c->retime.speedNum / c->retime.speedDen, 0, 'f', 0);
            p.setPen(kText);
            const QRectF textRect = r.adjusted(6, 3, -6, -3);
            p.drawText(textRect, Qt::AlignLeft | Qt::AlignTop,
                       p.fontMetrics().elidedText(label, Qt::ElideRight, int(textRect.width())));
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
    const int right = contentRight();
    const QRect tracksArea(kHeaderWidth, kRulerHeight, right - kHeaderWidth, bottom - kRulerHeight);

    p.save();
    p.setClipRect(tracksArea);
    for (const Row& r : allRows) {
        const QColor lane = r.track->locked ? kLaneLocked : r.track->kind == ve::TrackKind::Video ? kLaneVideo : kLaneAudio;
        p.fillRect(QRect(kHeaderWidth, r.top, right - kHeaderWidth, r.height), lane);
    }
    for (const Row& r : allRows)
        paintClips(p, r);
    if (const ve::Timeline* tl = timeline()) { // marker lines through the tracks
        for (const ve::Marker& m : tl->markers) {
            QColor line = markerColor(m.color);
            line.setAlpha(110);
            p.setPen(QPen(line, 1, Qt::DashLine));
            const double x = xForFrame(double(frameOf(m.start)));
            p.drawLine(QPointF(x, kRulerHeight), QPointF(x, bottom));
        }
    }
    if (m_dropFrame >= 0) {
        const double x = xForFrame(double(m_dropFrame));
        p.setPen(QPen(kAccent, 2));
        p.drawLine(QPointF(x, kRulerHeight), QPointF(x, bottom));
    }
    if (!timeline() || !timeline()->hasClips()) {
        p.setPen(kTextDim);
        p.drawText(tracksArea, Qt::AlignCenter, tr("Import media (Ctrl+I) or drop files here"));
    }
    p.restore();

    p.save();
    p.setClipRect(QRect(0, kRulerHeight, kHeaderWidth, bottom - kRulerHeight));
    paintHeaders(p, allRows);
    p.restore();

    paintRuler(p);
    p.fillRect(QRect(0, 0, kHeaderWidth, kRulerHeight), kHeader);
    p.setPen(kTextDim);
    p.drawText(QRect(8, 0, kHeaderWidth - 12, kRulerHeight), Qt::AlignVCenter | Qt::AlignLeft,
               m_tool == Tool::Trim ? tr("Trim") : tr("Select"));

    // Playhead
    const double px = xForFrame(double(m_playhead));
    if (px >= kHeaderWidth && px <= right) {
        p.setPen(QPen(kPlayhead, 1.5));
        p.drawLine(QPointF(px, kRulerHeight - 6), QPointF(px, bottom));
        p.setPen(Qt::NoPen);
        p.setBrush(kPlayhead);
        const QPointF head[] = {{px - 6, 2}, {px + 6, 2}, {px + 6, kRulerHeight - 12.0}, {px, kRulerHeight - 5.0},
                                {px - 6, kRulerHeight - 12.0}};
        p.drawPolygon(head, int(std::size(head)));
    }
}
