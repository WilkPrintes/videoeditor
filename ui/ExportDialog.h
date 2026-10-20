#pragma once

#include "core/Model.h"
#include "engine/Exporter.h"

#include <QDialog>

class QComboBox;
class QLineEdit;
class QSpinBox;

// H.264 / MP4 export settings.
class ExportDialog : public QDialog {
    Q_OBJECT

public:
    ExportDialog(const ve::Timeline& timeline, const QString& suggestedPath, QWidget* parent = nullptr);

    ve::ExportSettings settings() const;

private:
    void browse();

    QLineEdit* m_path = nullptr;
    QComboBox* m_encoder = nullptr;
    QComboBox* m_resolution = nullptr;
    QSpinBox* m_videoBitrate = nullptr;
    QComboBox* m_audioBitrate = nullptr;
    int m_width = 0;
    int m_height = 0;
};
