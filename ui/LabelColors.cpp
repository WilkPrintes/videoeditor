#include "ui/LabelColors.h"

#include <QCoreApplication>
#include <QPainter>
#include <QPixmap>

namespace labels {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("labels", text);
}

} // namespace

const std::vector<Named>& clipColors()
{
    static const std::vector<Named> colors = {
        {"orange", tr("Orange"), QColor(232, 125, 50)},  {"apricot", tr("Apricot"), QColor(240, 160, 90)},
        {"yellow", tr("Yellow"), QColor(222, 196, 60)},  {"lime", tr("Lime"), QColor(160, 200, 60)},
        {"olive", tr("Olive"), QColor(120, 140, 50)},    {"green", tr("Green"), QColor(70, 160, 90)},
        {"teal", tr("Teal"), QColor(50, 150, 150)},      {"navy", tr("Navy"), QColor(50, 70, 140)},
        {"blue", tr("Blue"), QColor(70, 120, 210)},      {"purple", tr("Purple"), QColor(140, 90, 190)},
        {"violet", tr("Violet"), QColor(170, 110, 220)}, {"pink", tr("Pink"), QColor(220, 110, 170)},
        {"tan", tr("Tan"), QColor(190, 160, 120)},       {"beige", tr("Beige"), QColor(200, 185, 150)},
        {"brown", tr("Brown"), QColor(140, 90, 60)},     {"chocolate", tr("Chocolate"), QColor(100, 65, 45)},
    };
    return colors;
}

const std::vector<Named>& flagColors()
{
    static const std::vector<Named> colors = {
        {"blue", tr("Blue"), QColor(70, 130, 230)},   {"cyan", tr("Cyan"), QColor(70, 200, 210)},
        {"green", tr("Green"), QColor(80, 190, 110)}, {"yellow", tr("Yellow"), QColor(235, 200, 70)},
        {"red", tr("Red"), QColor(225, 70, 70)},      {"pink", tr("Pink"), QColor(235, 120, 180)},
        {"purple", tr("Purple"), QColor(165, 110, 220)}, {"cream", tr("Cream"), QColor(235, 225, 190)},
    };
    return colors;
}

const std::vector<Named>& markerColors()
{
    static const std::vector<Named> colors = {
        {"blue", tr("Blue"), QColor(70, 130, 230)},     {"cyan", tr("Cyan"), QColor(70, 200, 210)},
        {"green", tr("Green"), QColor(80, 190, 110)},   {"yellow", tr("Yellow"), QColor(235, 200, 70)},
        {"red", tr("Red"), QColor(225, 70, 70)},        {"pink", tr("Pink"), QColor(235, 120, 180)},
        {"purple", tr("Purple"), QColor(165, 110, 220)}, {"orange", tr("Orange"), QColor(240, 140, 60)},
    };
    return colors;
}

QColor colorOf(const std::string& id, const QColor& fallback)
{
    if (id.empty())
        return fallback;
    for (const auto* list : {&clipColors(), &flagColors(), &markerColors()}) {
        for (const Named& n : *list) {
            if (n.id == id)
                return n.color;
        }
    }
    const QColor parsed = QColor::fromString(QString::fromStdString(id));
    return parsed.isValid() ? parsed : fallback;
}

QIcon swatch(const QColor& color)
{
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(color.darker(160));
    p.setBrush(color);
    p.drawRoundedRect(QRectF(1.5, 1.5, 11, 11), 3, 3);
    return QIcon(pm);
}

} // namespace labels
