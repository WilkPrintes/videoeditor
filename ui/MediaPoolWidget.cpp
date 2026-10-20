#include "ui/MediaPoolWidget.h"

#include <QDragEnterEvent>
#include <QMimeData>
#include <QStyle>
#include <QUrl>

MediaPoolWidget::MediaPoolWidget(QWidget* parent)
    : QListWidget(parent)
{
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);
    setIconSize(QSize(32, 32));
    setSpacing(2);
    setWordWrap(true);
    setFocusPolicy(Qt::ClickFocus);
    connect(this, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        emit mediaActivated(item->data(Qt::UserRole).toString());
    });
}

void MediaPoolWidget::setMedia(const ve::MediaPool& pool, ve::Rate timelineRate)
{
    QStringList selectedIds;
    for (QListWidgetItem* item : selectedItems())
        selectedIds << item->data(Qt::UserRole).toString();

    clear();
    for (const ve::MediaItem& m : pool.items) {
        QString details;
        if (m.hasVideo)
            details = QStringLiteral("%1x%2  %3 fps").arg(m.width).arg(m.height).arg(m.frameRate.toDouble(), 0, 'f', 2);
        else
            details = tr("audio only");
        details += QStringLiteral("  ") + QString::fromStdString(
            ve::formatTimecode(m.duration, timelineRate));

        auto* item = new QListWidgetItem(QString::fromStdString(m.name) + QLatin1Char('\n') + details, this);
        item->setData(Qt::UserRole, QString::fromStdString(m.id));
        item->setIcon(style()->standardIcon(m.hasVideo ? QStyle::SP_FileDialogContentsView : QStyle::SP_MediaVolume));
        QString tip = QString::fromStdString(m.path);
        if (m.hasVideo)
            tip += QStringLiteral("\nvideo: %1").arg(QString::fromStdString(m.videoCodec));
        if (m.hasAudio)
            tip += QStringLiteral("\naudio: %1, %2 Hz, %3 ch")
                       .arg(QString::fromStdString(m.audioCodec))
                       .arg(m.sampleRate)
                       .arg(m.channels);
        item->setToolTip(tip);
        if (selectedIds.contains(item->data(Qt::UserRole).toString()))
            item->setSelected(true);
    }
}

QStringList MediaPoolWidget::mimeTypes() const
{
    return {QString::fromLatin1(kMediaMimeType), QStringLiteral("text/uri-list")};
}

QMimeData* MediaPoolWidget::mimeData(const QList<QListWidgetItem*>& items) const
{
    if (items.isEmpty())
        return nullptr;
    auto* mime = new QMimeData;
    // The timeline places one media per drop: the first selected one.
    mime->setData(QString::fromLatin1(kMediaMimeType), items.first()->data(Qt::UserRole).toString().toUtf8());
    return mime;
}

void MediaPoolWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
    else
        event->ignore(); // internal reordering is not supported
}

void MediaPoolWidget::dragMoveEvent(QDragMoveEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
    else
        event->ignore();
}

void MediaPoolWidget::dropEvent(QDropEvent* event)
{
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            paths << url.toLocalFile();
    }
    if (paths.isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    emit filesDropped(paths);
}
