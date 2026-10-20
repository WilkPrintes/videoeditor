#include "ui/ExportDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

namespace {

QString describeEncoder(const std::string& name)
{
    if (name == "h264_nvenc")
        return QObject::tr("NVIDIA NVENC (GPU)");
    if (name == "h264_amf")
        return QObject::tr("AMD AMF (GPU)");
    if (name == "h264_qsv")
        return QObject::tr("Intel Quick Sync (GPU)");
    if (name == "h264_mf")
        return QObject::tr("Media Foundation (GPU MFT if present, else software)");
    if (name == "h264_videotoolbox")
        return QObject::tr("VideoToolbox (Apple)");
    if (name == "libopenh264")
        return QObject::tr("OpenH264 (software, BSD)");
    if (name == "libx264")
        return QObject::tr("x264 (software, GPL build only)");
    return QString::fromStdString(name);
}

} // namespace

ExportDialog::ExportDialog(const ve::Timeline& timeline, const QString& suggestedPath, QWidget* parent)
    : QDialog(parent)
    , m_width(timeline.width)
    , m_height(timeline.height)
{
    setWindowTitle(tr("Export H.264 / MP4"));
    setMinimumWidth(520);

    m_path = new QLineEdit(suggestedPath);
    auto* browseButton = new QPushButton(tr("Browse..."));
    connect(browseButton, &QPushButton::clicked, this, &ExportDialog::browse);
    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(browseButton);

    m_encoder = new QComboBox;
    m_encoder->addItem(tr("Automatic (best available)"), QStringLiteral("auto"));
    for (const std::string& name : ve::availableH264Encoders(false))
        m_encoder->addItem(describeEncoder(name), QString::fromStdString(name));

    m_resolution = new QComboBox;
    m_resolution->addItem(tr("Timeline (%1x%2)").arg(m_width).arg(m_height), QSize(m_width, m_height));
    for (const QSize& s : {QSize(3840, 2160), QSize(1920, 1080), QSize(1280, 720)}) {
        if (s != QSize(m_width, m_height))
            m_resolution->addItem(QStringLiteral("%1x%2").arg(s.width()).arg(s.height()), s);
    }

    m_videoBitrate = new QSpinBox;
    m_videoBitrate->setRange(1, 400);
    m_videoBitrate->setSuffix(tr(" Mbit/s"));
    m_videoBitrate->setValue(m_height > 1080 ? 40 : 16);

    m_audioBitrate = new QComboBox;
    for (int kbps : {128, 192, 256, 320})
        m_audioBitrate->addItem(tr("%1 kbit/s").arg(kbps), kbps);
    m_audioBitrate->setCurrentIndex(1);

    auto* form = new QFormLayout;
    form->addRow(tr("File:"), pathRow);
    form->addRow(tr("Video encoder:"), m_encoder);
    form->addRow(tr("Resolution:"), m_resolution);
    form->addRow(tr("Frame rate:"), new QLabel(QStringLiteral("%1 fps").arg(timeline.rate.toDouble(), 0, 'f', 3)));
    form->addRow(tr("Video bitrate:"), m_videoBitrate);
    form->addRow(tr("Audio (AAC):"), m_audioBitrate);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Export"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (!m_path->text().trimmed().isEmpty())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

void ExportDialog::browse()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Export to"), m_path->text(), tr("MPEG-4 video (*.mp4)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive))
        path += QStringLiteral(".mp4");
    m_path->setText(path);
}

ve::ExportSettings ExportDialog::settings() const
{
    ve::ExportSettings s;
    QString path = m_path->text().trimmed();
    if (!path.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive))
        path += QStringLiteral(".mp4");
    s.outputPath = path.toStdString();
    s.videoEncoder = m_encoder->currentData().toString().toStdString();
    const QSize size = m_resolution->currentData().toSize();
    s.width = size.width();
    s.height = size.height();
    s.videoBitrateKbps = m_videoBitrate->value() * 1000;
    s.audioBitrateKbps = m_audioBitrate->currentData().toInt();
    return s;
}
