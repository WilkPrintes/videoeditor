#pragma once

#include "core/Model.h"

#include <QTabWidget>

class QTableWidget;

// The edit index (spec phase 3.5): every clip of the timeline as an edit list (record and source
// in/out), and every marker. Clicking a row selects that clip or marker and moves the playhead to it.
class EditIndexWidget : public QTabWidget {
    Q_OBJECT

public:
    explicit EditIndexWidget(QWidget* parent = nullptr);

    void setTimeline(const ve::Timeline& tl, const ve::MediaPool& pool);

signals:
    void clipActivated(const QString& clipId);
    void markerActivated(const QString& markerId);

private:
    QTableWidget* m_edits = nullptr;
    QTableWidget* m_markers = nullptr;
};
