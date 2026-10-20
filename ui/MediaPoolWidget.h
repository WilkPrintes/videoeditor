#pragma once

#include "core/Model.h"

#include <QListWidget>

// Media pool: imported files. Items drag onto the timeline; files dropped from the OS import.
class MediaPoolWidget : public QListWidget {
    Q_OBJECT

public:
    static constexpr const char* kMediaMimeType = "application/x-videoeditor-media-id";

    explicit MediaPoolWidget(QWidget* parent = nullptr);

    void setMedia(const ve::MediaPool& pool, ve::Rate timelineRate);
    // The source of three and four point edits: the selected media (the first of several), or the only
    // one in the pool. Empty when there is no obvious choice.
    QString sourceMediaId() const;
    void selectMedia(const QString& mediaId);

signals:
    void mediaActivated(const QString& mediaId);
    void filesDropped(const QStringList& paths);

protected:
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
};
