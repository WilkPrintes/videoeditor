#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

#include <string>
#include <vector>

// Colour labels of clips, tracks, flags and markers (spec §6.8). The model stores the id ("orange");
// the UI turns it into a colour and a translated name.
namespace labels {

struct Named {
    std::string id;
    QString name;
    QColor color;
};

const std::vector<Named>& clipColors();   // clip and track labels
const std::vector<Named>& flagColors();
const std::vector<Named>& markerColors();

// The colour for an id from any of the lists, or `fallback` for an empty or unknown id.
QColor colorOf(const std::string& id, const QColor& fallback);
QIcon swatch(const QColor& color);

} // namespace labels
