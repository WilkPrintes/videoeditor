#include "ui/InspectorWidget.h"

#include "ui/LabelColors.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

namespace {

enum Page { TimelinePage, ClipPage, MarkerPage, TrackPage };

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

QString tc(ve::Time t, ve::Rate rate)
{
    return qs(ve::formatTimecode(t, rate));
}

// Model refreshes must not overwrite what the user is typing.
void setTextIfIdle(QLineEdit* edit, const QString& text)
{
    if (!edit->hasFocus() && edit->text() != text)
        edit->setText(text);
}

void fillColors(QComboBox* box, const std::vector<labels::Named>& colors, const QString& none)
{
    box->addItem(none, QString());
    for (const labels::Named& c : colors)
        box->addItem(labels::swatch(c.color), c.name, qs(c.id));
}

void selectColor(QComboBox* box, const std::string& id)
{
    const QSignalBlocker block(box);
    const int i = box->findData(qs(id));
    box->setCurrentIndex(i < 0 ? 0 : i);
}

} // namespace

InspectorWidget::InspectorWidget(QWidget* parent)
    : QWidget(parent)
{
    m_title = new QLabel;
    QFont bold = m_title->font();
    bold.setBold(true);
    m_title->setFont(bold);
    m_pages = new QStackedWidget;
    m_pages->addWidget(buildTimelinePage());
    m_pages->addWidget(buildClipPage());
    m_pages->addWidget(buildMarkerPage());
    m_pages->addWidget(buildTrackPage());

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->addWidget(m_title);
    layout->addWidget(m_pages);
    layout->addStretch(1);
}

QWidget* InspectorWidget::buildTimelinePage()
{
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);
    m_timelineName = new QLineEdit;
    m_timelineName->setObjectName(QStringLiteral("inspectorTimelineName"));
    m_timelineInfo = new QLabel;
    m_timelineInfo->setWordWrap(true);
    form->addRow(tr("Name"), m_timelineName);
    form->addRow(m_timelineInfo);
    connect(m_timelineName, &QLineEdit::editingFinished, this, [this] {
        if (!m_timelineName->text().trimmed().isEmpty())
            emit timelineRenamed(m_timelineName->text().trimmed());
    });
    return page;
}

QWidget* InspectorWidget::buildClipPage()
{
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);
    m_clipName = new QLineEdit;
    m_clipName->setObjectName(QStringLiteral("inspectorClipName"));
    m_clipInfo = new QLabel;
    m_clipInfo->setWordWrap(true);
    m_clipInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_clipColor = new QComboBox;
    m_clipColor->setObjectName(QStringLiteral("inspectorClipColor"));
    fillColors(m_clipColor, labels::clipColors(), tr("Default"));
    m_clipEnabled = new QCheckBox(tr("Enabled"));
    m_clipEnabled->setObjectName(QStringLiteral("inspectorClipEnabled"));

    auto* flags = new QWidget;
    auto* flagRow = new QHBoxLayout(flags);
    flagRow->setContentsMargins(0, 0, 0, 0);
    flagRow->setSpacing(2);
    for (const labels::Named& f : labels::flagColors()) {
        auto* b = new QToolButton;
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setIcon(labels::swatch(f.color));
        b->setToolTip(tr("Flag: %1").arg(f.name));
        b->setProperty("flag", qs(f.id));
        flagRow->addWidget(b);
        m_flagButtons.push_back(b);
        connect(b, &QToolButton::toggled, this, [this, b](bool on) {
            emit clipFlagToggled(m_clipIds, b->property("flag").toString(), on);
        });
    }
    flagRow->addStretch(1);

    form->addRow(tr("Name"), m_clipName);
    form->addRow(m_clipInfo);
    form->addRow(tr("Clip color"), m_clipColor);
    form->addRow(tr("Flags"), flags);
    form->addRow(m_clipEnabled);

    connect(m_clipName, &QLineEdit::editingFinished, this, [this] {
        if (!m_clipIds.isEmpty())
            emit clipsRenamed(m_clipIds, m_clipName->text()); // picture and sound of a take share the name
    });
    connect(m_clipColor, &QComboBox::currentIndexChanged, this,
            [this] { emit clipColorChosen(m_clipIds, m_clipColor->currentData().toString()); });
    connect(m_clipEnabled, &QCheckBox::toggled, this, [this](bool on) { emit clipsEnabledChanged(m_clipIds, on); });
    return page;
}

QWidget* InspectorWidget::buildMarkerPage()
{
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);
    m_markerName = new QLineEdit;
    m_markerName->setObjectName(QStringLiteral("inspectorMarkerName"));
    m_markerNote = new QPlainTextEdit;
    m_markerNote->setObjectName(QStringLiteral("inspectorMarkerNote"));
    m_markerNote->setFixedHeight(70);
    m_markerColor = new QComboBox;
    m_markerColor->setObjectName(QStringLiteral("inspectorMarkerColor"));
    for (const labels::Named& c : labels::markerColors())
        m_markerColor->addItem(labels::swatch(c.color), c.name, qs(c.id));
    m_markerStart = new QLineEdit;
    m_markerStart->setObjectName(QStringLiteral("inspectorMarkerStart"));
    m_markerDuration = new QSpinBox;
    m_markerDuration->setObjectName(QStringLiteral("inspectorMarkerDuration"));
    m_markerDuration->setRange(0, 24 * 3600 * 120);
    m_markerDuration->setSuffix(tr(" frames"));
    form->addRow(tr("Name"), m_markerName);
    form->addRow(tr("Note"), m_markerNote);
    form->addRow(tr("Color"), m_markerColor);
    form->addRow(tr("Start"), m_markerStart);
    form->addRow(tr("Duration"), m_markerDuration);

    auto commit = [this] { emit markerEdited(m_marker); };
    connect(m_markerName, &QLineEdit::editingFinished, this, [this, commit] {
        m_marker.name = m_markerName->text().toStdString();
        commit();
    });
    // The note commits when the field loses focus, so typing does not create one step per key.
    m_markerNote->installEventFilter(this);
    connect(m_markerColor, &QComboBox::currentIndexChanged, this, [this, commit] {
        m_marker.color = m_markerColor->currentData().toString().toStdString();
        commit();
    });
    connect(m_markerStart, &QLineEdit::editingFinished, this, [this, commit] {
        ve::Time t;
        if (ve::parseTimecode(m_markerStart->text().toStdString(), m_rate, &t)) {
            m_marker.start = t;
            commit();
        }
    });
    connect(m_markerDuration, &QSpinBox::editingFinished, this, [this, commit] {
        m_marker.duration = ve::timeOfFrame(m_markerDuration->value(), m_rate);
        commit();
    });
    connect(m_markerNote, &QPlainTextEdit::textChanged, this, [this] {
        m_marker.note = m_markerNote->toPlainText().toStdString();
    });
    return page;
}

QWidget* InspectorWidget::buildTrackPage()
{
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);
    m_trackName = new QLineEdit;
    m_trackName->setObjectName(QStringLiteral("inspectorTrackName"));
    m_trackColor = new QComboBox;
    m_trackColor->setObjectName(QStringLiteral("inspectorTrackColor"));
    fillColors(m_trackColor, labels::clipColors(), tr("Default"));
    m_trackLocked = new QCheckBox(tr("Locked"));
    m_trackSyncLock = new QCheckBox(tr("Sync lock"));
    m_trackEnabled = new QCheckBox(tr("Enabled"));
    form->addRow(tr("Name"), m_trackName);
    form->addRow(tr("Color"), m_trackColor);
    form->addRow(m_trackEnabled);
    form->addRow(m_trackLocked);
    form->addRow(m_trackSyncLock);
    connect(m_trackName, &QLineEdit::editingFinished, this, [this] {
        if (!m_trackName->text().trimmed().isEmpty())
            emit trackRenamed(m_trackId, m_trackName->text().trimmed());
    });
    connect(m_trackColor, &QComboBox::currentIndexChanged, this,
            [this] { emit trackColorChosen(m_trackId, m_trackColor->currentData().toString()); });
    connect(m_trackLocked, &QCheckBox::toggled, this, [this](bool on) { emit trackLockChanged(m_trackId, on); });
    connect(m_trackSyncLock, &QCheckBox::toggled, this, [this](bool on) { emit trackSyncLockChanged(m_trackId, on); });
    connect(m_trackEnabled, &QCheckBox::toggled, this, [this](bool on) { emit trackEnabledChanged(m_trackId, on); });
    return page;
}

bool InspectorWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_markerNote && event->type() == QEvent::FocusOut)
        emit markerEdited(m_marker);
    return QWidget::eventFilter(watched, event);
}

void InspectorWidget::showTimeline(const ve::Timeline& tl)
{
    m_rate = tl.rate;
    m_title->setText(tr("Timeline"));
    m_pages->setCurrentIndex(TimelinePage);
    setTextIfIdle(m_timelineName, qs(tl.name));
    int video = 0;
    int audio = 0;
    for (const ve::Track& t : tl.tracks)
        (t.kind == ve::TrackKind::Video ? video : audio)++;
    m_timelineInfo->setText(tr("%1x%2 @ %3 fps\nDuration %4\n%5 video and %6 audio tracks, %7 markers")
                                .arg(tl.width)
                                .arg(tl.height)
                                .arg(tl.rate.toDouble(), 0, 'f', 3)
                                .arg(tc(tl.duration(), tl.rate))
                                .arg(video)
                                .arg(audio)
                                .arg(tl.markers.size()));
}

void InspectorWidget::showClips(const ve::Timeline& tl, const ve::MediaPool& pool, const std::vector<ve::Id>& clipIds)
{
    m_rate = tl.rate;
    m_clipIds.clear();
    for (const ve::Id& id : clipIds)
        m_clipIds << qs(id);
    m_pages->setCurrentIndex(ClipPage);

    // The first selected clip (in track order) describes the selection; colours and switches apply to all.
    const ve::PlacedItem* first = nullptr;
    const ve::Track* firstTrack = nullptr;
    std::vector<ve::PlacedItem> all;
    std::vector<const ve::Track*> owners;
    for (const ve::Track& t : tl.tracks) {
        for (const ve::PlacedItem& p : t.clips()) {
            if (std::find(clipIds.begin(), clipIds.end(), p.clip->id) != clipIds.end()) {
                all.push_back(p);
                owners.push_back(&t);
            }
        }
    }
    if (all.empty()) {
        showTimeline(tl);
        return;
    }
    first = &all.front();
    firstTrack = owners.front();
    const ve::ClipBase& c = *first->clip;
    m_title->setText(all.size() == 1 ? tr("Clip") : tr("%1 clips").arg(all.size()));
    setTextIfIdle(m_clipName, qs(c.name));

    QString kind = tr("Clip");
    QString source;
    if (const ve::Clip* clip = std::get_if<ve::Clip>(first->item)) {
        const ve::MediaItem* media = pool.find(clip->mediaId);
        source = media ? qs(media->name) : tr("missing media");
        if (!clip->retime.isIdentity() && clip->retime.isValid())
            kind += tr(", speed %1%").arg(100.0 * clip->retime.speedNum / clip->retime.speedDen, 0, 'f', 1);
    } else if (std::holds_alternative<ve::CompoundClip>(*first->item)) {
        kind = tr("Compound clip");
    } else {
        kind = tr("Multicam clip");
    }
    m_clipInfo->setText(tr("%1 on %2\nSource: %3\nTimeline %4 - %5 (%6)\nSource %7 - %8%9")
                            .arg(kind, qs(firstTrack->name), source)
                            .arg(tc(first->start, tl.rate), tc(first->end(), tl.rate),
                                 tc(c.sourceRange.duration, tl.rate), tc(c.sourceRange.start, tl.rate),
                                 tc(ve::sourceEnd(*first->item), tl.rate))
                            .arg(c.linkGroup.empty() ? QString() : tr("\nLinked")));
    selectColor(m_clipColor, c.color);
    {
        const QSignalBlocker block(m_clipEnabled);
        m_clipEnabled->setChecked(c.enabled);
    }
    const std::set<std::string> flags(c.flags.begin(), c.flags.end());
    for (QToolButton* b : m_flagButtons) {
        const QSignalBlocker block(b);
        b->setChecked(flags.count(b->property("flag").toString().toStdString()) > 0);
    }
}

void InspectorWidget::showMarker(const ve::Timeline& tl, const ve::Marker& marker)
{
    m_rate = tl.rate;
    m_marker = marker;
    m_title->setText(tr("Marker"));
    m_pages->setCurrentIndex(MarkerPage);
    setTextIfIdle(m_markerName, qs(marker.name));
    if (!m_markerNote->hasFocus() && m_markerNote->toPlainText() != qs(marker.note)) {
        const QSignalBlocker block(m_markerNote);
        m_markerNote->setPlainText(qs(marker.note));
    }
    {
        const QSignalBlocker block(m_markerColor);
        const int i = m_markerColor->findData(qs(marker.color));
        m_markerColor->setCurrentIndex(i < 0 ? 0 : i);
    }
    setTextIfIdle(m_markerStart, tc(marker.start, tl.rate));
    if (!m_markerDuration->hasFocus()) {
        const QSignalBlocker block(m_markerDuration);
        m_markerDuration->setValue(int(ve::frameAt(marker.duration, tl.rate)));
    }
}

void InspectorWidget::showTrack(const ve::Timeline& tl, const ve::Track& track)
{
    m_rate = tl.rate;
    m_trackId = qs(track.id);
    m_title->setText(track.kind == ve::TrackKind::Video ? tr("Video track") : tr("Audio track"));
    m_pages->setCurrentIndex(TrackPage);
    setTextIfIdle(m_trackName, qs(track.name));
    selectColor(m_trackColor, track.color);
    const std::pair<QCheckBox*, bool> boxes[] = {
        {m_trackLocked, track.locked}, {m_trackSyncLock, track.syncLock}, {m_trackEnabled, track.enabled}};
    for (const auto& [box, value] : boxes) {
        const QSignalBlocker block(box);
        box->setChecked(value);
    }
}
