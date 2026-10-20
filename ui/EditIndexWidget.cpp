#include "ui/EditIndexWidget.h"

#include "ui/LabelColors.h"

#include <QHeaderView>
#include <QTableWidget>

#include <algorithm>

namespace {

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

QTableWidget* makeTable(const QStringList& headers, const QString& objectName)
{
    auto* table = new QTableWidget(0, int(headers.size()));
    table->setObjectName(objectName);
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->hide();
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setAlternatingRowColors(true);
    return table;
}

QTableWidgetItem* cell(const QString& text, const QString& id = {})
{
    auto* item = new QTableWidgetItem(text);
    if (!id.isEmpty())
        item->setData(Qt::UserRole, id);
    return item;
}

} // namespace

EditIndexWidget::EditIndexWidget(QWidget* parent)
    : QTabWidget(parent)
{
    m_edits = makeTable({tr("#"), tr("Track"), tr("Name"), tr("Record In"), tr("Record Out"), tr("Duration"), tr("Source In"),
                         tr("Source Out")},
                        QStringLiteral("editIndexEdits"));
    m_markers = makeTable({tr("#"), tr("Name"), tr("Start"), tr("Duration"), tr("Color"), tr("Note")},
                          QStringLiteral("editIndexMarkers"));
    addTab(m_edits, tr("Edits"));
    addTab(m_markers, tr("Markers"));
    connect(m_edits, &QTableWidget::cellClicked, this, [this](int row, int) {
        if (QTableWidgetItem* item = m_edits->item(row, 0))
            emit clipActivated(item->data(Qt::UserRole).toString());
    });
    connect(m_markers, &QTableWidget::cellClicked, this, [this](int row, int) {
        if (QTableWidgetItem* item = m_markers->item(row, 0))
            emit markerActivated(item->data(Qt::UserRole).toString());
    });
}

void EditIndexWidget::setTimeline(const ve::Timeline& tl, const ve::MediaPool& pool)
{
    struct Row {
        ve::Time start;
        int track;
        const ve::Track* owner;
        ve::PlacedItem placed;
    };
    std::vector<Row> rows;
    for (std::size_t t = 0; t < tl.tracks.size(); ++t) {
        for (const ve::PlacedItem& p : tl.tracks[t].clips())
            rows.push_back({p.start, int(t), &tl.tracks[t], p});
    }
    // An edit list reads in record order; picture before sound at the same time.
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.start != b.start)
            return a.start < b.start;
        if (a.owner->kind != b.owner->kind)
            return a.owner->kind == ve::TrackKind::Video;
        return a.track < b.track;
    });

    auto tc = [&](ve::Time t) { return qs(ve::formatTimecode(t, tl.rate)); };
    m_edits->setRowCount(int(rows.size()));
    for (int i = 0; i < int(rows.size()); ++i) {
        const Row& r = rows[std::size_t(i)];
        const ve::ClipBase& c = *r.placed.clip;
        QString name = qs(c.name);
        if (const ve::Clip* clip = std::get_if<ve::Clip>(r.placed.item)) {
            if (const ve::MediaItem* m = pool.find(clip->mediaId); m && name.isEmpty())
                name = qs(m->name);
        }
        const QString id = qs(c.id);
        m_edits->setItem(i, 0, cell(QString::number(i + 1), id));
        m_edits->setItem(i, 1, cell(qs(r.owner->name)));
        m_edits->setItem(i, 2, cell(name));
        m_edits->setItem(i, 3, cell(tc(r.placed.start)));
        m_edits->setItem(i, 4, cell(tc(r.placed.end())));
        m_edits->setItem(i, 5, cell(tc(c.sourceRange.duration)));
        m_edits->setItem(i, 6, cell(tc(c.sourceRange.start)));
        m_edits->setItem(i, 7, cell(tc(ve::sourceEnd(*r.placed.item))));
        if (!c.color.empty())
            m_edits->item(i, 2)->setIcon(labels::swatch(labels::colorOf(c.color, Qt::gray)));
    }

    m_markers->setRowCount(int(tl.markers.size()));
    for (int i = 0; i < int(tl.markers.size()); ++i) {
        const ve::Marker& m = tl.markers[std::size_t(i)];
        m_markers->setItem(i, 0, cell(QString::number(i + 1), qs(m.id)));
        m_markers->setItem(i, 1, cell(qs(m.name)));
        m_markers->setItem(i, 2, cell(tc(m.start)));
        m_markers->setItem(i, 3, cell(tc(m.duration)));
        m_markers->setItem(i, 4, cell(qs(m.color)));
        m_markers->item(i, 4)->setIcon(labels::swatch(labels::colorOf(m.color, Qt::gray)));
        m_markers->setItem(i, 5, cell(qs(m.note)));
    }
    setTabText(0, tr("Edits (%1)").arg(rows.size()));
    setTabText(1, tr("Markers (%1)").arg(tl.markers.size()));
}
