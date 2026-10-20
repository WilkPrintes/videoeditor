#include "ui/SourceViewerPanel.h"

#include "core/Edit.h"
#include "core/Snapshot.h"
#include "engine/PlaybackEngine.h"
#include "ui/ViewerWidget.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

const QColor kAccent(240, 196, 76);
const QColor kPlayhead(235, 70, 58);

QString timecode(ve::Time t, ve::Rate rate)
{
    return QString::fromStdString(ve::formatTimecode(t, rate));
}

} // namespace

// The whole media as a bar: In/Out range, playhead, click or drag to scrub.
class SourceScrubBar : public QWidget {
public:
    std::function<void(double)> onSeek; // fraction of the media

    explicit SourceScrubBar(QWidget* parent)
        : QWidget(parent)
    {
        setFixedHeight(16);
        setCursor(Qt::PointingHandCursor);
    }

    void setState(double playhead, std::optional<double> in, std::optional<double> out)
    {
        m_playhead = playhead;
        m_in = in;
        m_out = out;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const QRectF r = rect().adjusted(4, 4, -4, -4);
        p.fillRect(r, QColor(48, 50, 55));
        if (m_in || m_out) {
            QColor band = kAccent;
            band.setAlpha(110);
            const double a = r.left() + r.width() * m_in.value_or(0.0);
            const double b = r.left() + r.width() * m_out.value_or(1.0);
            p.fillRect(QRectF(a, r.top(), std::max(1.0, b - a), r.height()), band);
        }
        const double x = r.left() + r.width() * m_playhead;
        p.setPen(QPen(kPlayhead, 2));
        p.drawLine(QPointF(x, 0), QPointF(x, height()));
    }
    void mousePressEvent(QMouseEvent* e) override { seekAt(e->position().x()); }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (e->buttons() & Qt::LeftButton)
            seekAt(e->position().x());
    }

private:
    void seekAt(double x)
    {
        const double w = std::max(1.0, double(width() - 8));
        if (onSeek)
            onSeek(std::clamp((x - 4.0) / w, 0.0, 1.0));
    }

    double m_playhead = 0.0;
    std::optional<double> m_in;
    std::optional<double> m_out;
};

SourceViewerPanel::SourceViewerPanel(QWidget* parent)
    : QWidget(parent)
    , m_engine(std::make_unique<ve::PlaybackEngine>())
{
    m_viewer = new ViewerWidget;
    m_viewer->setObjectName(QStringLiteral("sourceViewer"));
    m_bar = new SourceScrubBar(this);
    m_bar->onSeek = [this](double fraction) {
        emit activated();
        seek(ve::Time(std::int64_t(std::llround(double(m_duration.flicks) * fraction))));
    };

    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(mono.pointSizeF() * 1.15);
    m_name = new QLabel(tr("No source clip"));
    m_name->setStyleSheet(QStringLiteral("color: #8a8f98;"));
    m_timecode = new QLabel(QStringLiteral("00:00:00:00"));
    m_timecode->setFont(mono);
    m_marks = new QLabel;
    m_marks->setFont(mono);
    m_marks->setStyleSheet(QStringLiteral("color: #f0c44c;"));

    m_playButton = new QToolButton;
    m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_playButton->setAutoRaise(true);
    m_playButton->setFocusPolicy(Qt::NoFocus);
    m_playButton->setToolTip(tr("Play / pause the source"));
    connect(m_playButton, &QToolButton::clicked, this, [this] {
        emit activated();
        togglePlay();
    });

    auto* header = new QHBoxLayout;
    header->setContentsMargins(8, 4, 8, 2);
    header->setSpacing(12); // nested layouts would inherit the outer spacing of 0
    header->addWidget(m_name, 1);
    header->addWidget(m_marks);
    auto* transport = new QHBoxLayout;
    transport->setContentsMargins(8, 2, 8, 4);
    transport->addWidget(m_timecode);
    transport->addStretch(1);
    transport->addWidget(m_playButton);
    transport->addStretch(1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(m_viewer, 1);
    layout->addWidget(m_bar);
    layout->addLayout(transport);

    for (QWidget* w : {static_cast<QWidget*>(m_viewer), static_cast<QWidget*>(m_bar)})
        w->installEventFilter(this);
    setFocusPolicy(Qt::ClickFocus);
    updateLabels();
}

SourceViewerPanel::~SourceViewerPanel() = default;

void SourceViewerPanel::setMedia(const ve::MediaItem* media, ve::Rate rate)
{
    m_engine->pause();
    m_in.reset();
    m_out.reset();
    m_rate = rate;
    if (!media) {
        m_media.reset();
        m_duration = ve::Time(0);
        m_engine->setTimeline(nullptr);
        m_viewer->setFrame(nullptr);
    } else {
        m_media = *media;
        // The source as a one-clip timeline on the project's frame grid: the engine plays it like any other.
        auto snap = std::make_shared<ve::TimelineSnapshot>();
        snap->timeline.id = "source";
        snap->timeline.rate = rate;
        snap->timeline.tracks = ve::makeDefaultTimeline().tracks;
        ve::edit::placeMedia(snap->timeline, *media, ve::Time(0));
        snap->media.emplace(media->id, *media);
        m_duration = snap->timeline.duration();
        m_engine->setTimeline(snap);
        m_engine->seek(ve::Time(0));
    }
    m_lastPosition = ve::Time(-1);
    updateLabels();
    emit marksChanged();
}

ve::Id SourceViewerPanel::mediaId() const
{
    return m_media ? m_media->id : ve::Id{};
}

void SourceViewerPanel::tick()
{
    bool changed = false;
    ve::VideoFramePtr frame = m_engine->currentVideoFrame(&changed);
    if (changed)
        m_viewer->setFrame(std::move(frame));
    if (m_engine->isPlaying() && m_engine->reachedEnd())
        m_engine->pause();
    const ve::Time pos = m_engine->position();
    const bool playing = m_engine->isPlaying();
    if (pos != m_lastPosition || playing != m_lastPlaying) {
        m_lastPosition = pos;
        m_lastPlaying = playing;
        m_playButton->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
        updateLabels();
    }
}

void SourceViewerPanel::play()
{
    if (m_media)
        m_engine->play();
}

void SourceViewerPanel::pause()
{
    m_engine->pause();
}

void SourceViewerPanel::togglePlay()
{
    if (m_engine->isPlaying())
        pause();
    else
        play();
}

bool SourceViewerPanel::isPlaying() const
{
    return m_engine->isPlaying();
}

void SourceViewerPanel::seek(ve::Time t)
{
    if (!m_media)
        return;
    const ve::Time last = ve::timeOfFrame(std::max<std::int64_t>(0, ve::frameAt(m_duration, m_rate) - 1), m_rate);
    m_engine->seek(std::clamp(t, ve::Time(0), last));
}

void SourceViewerPanel::stepFrames(std::int64_t frames)
{
    pause();
    seek(ve::timeOfFrame(std::max<std::int64_t>(0, ve::frameAt(position(), m_rate) + frames), m_rate));
}

void SourceViewerPanel::goToStart()
{
    seek(ve::Time(0));
}

void SourceViewerPanel::goToEnd()
{
    seek(m_duration);
}

ve::Time SourceViewerPanel::position() const
{
    return m_engine->position();
}

void SourceViewerPanel::markIn()
{
    if (!m_media)
        return;
    m_in = ve::floorToFrame(position(), m_rate);
    if (m_out && *m_out <= *m_in)
        m_out.reset();
    updateLabels();
    emit marksChanged();
}

void SourceViewerPanel::markOut()
{
    if (!m_media)
        return;
    m_out = ve::timeOfFrame(ve::frameAt(position(), m_rate) + 1, m_rate);
    if (m_in && *m_in >= *m_out)
        m_in.reset();
    updateLabels();
    emit marksChanged();
}

void SourceViewerPanel::clearMarks()
{
    m_in.reset();
    m_out.reset();
    updateLabels();
    emit marksChanged();
}

void SourceViewerPanel::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    update();
}

bool SourceViewerPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress)
        emit activated();
    return QWidget::eventFilter(watched, event);
}

void SourceViewerPanel::paintEvent(QPaintEvent*)
{
    if (!m_active)
        return;
    QPainter p(this);
    p.setPen(QPen(kAccent, 2));
    p.drawRect(rect().adjusted(1, 1, -1, -1));
}

void SourceViewerPanel::updateLabels()
{
    if (!m_media) {
        m_name->setText(tr("No source clip: double-click media in the pool"));
        m_timecode->setText(QStringLiteral("--:--:--:--"));
        m_marks->clear();
        m_bar->setState(0.0, std::nullopt, std::nullopt);
        return;
    }
    m_name->setText(QString::fromStdString(m_media->name));
    m_timecode->setText(timecode(position(), m_rate));
    QString marks;
    if (m_in)
        marks += tr("In %1").arg(timecode(*m_in, m_rate));
    if (m_out)
        marks += (marks.isEmpty() ? QString() : QStringLiteral("  ")) + tr("Out %1").arg(timecode(*m_out, m_rate));
    if (m_in || m_out) {
        const ve::Time length = m_out.value_or(m_duration) - m_in.value_or(ve::Time(0));
        marks += QStringLiteral("  (%1)").arg(timecode(length, m_rate));
    }
    m_marks->setText(marks);
    const double total = std::max<double>(1.0, double(m_duration.flicks));
    auto fraction = [&](const std::optional<ve::Time>& t) {
        return t ? std::optional<double>(double(t->flicks) / total) : std::nullopt;
    };
    m_bar->setState(double(position().flicks) / total, fraction(m_in), fraction(m_out));
}
