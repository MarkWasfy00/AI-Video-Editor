#pragma once

#include "core/Project.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <optional>

namespace ve {

// A ready-made text look: style + animation + default words + placement.
// Built-in templates ship with the editor; custom ones are saved by the user
// (QSettings, shared by the GUI and the MCP server).
struct TextTemplate {
    QString id;
    QString name;
    QString category;              // Titles, Lower thirds, Social, Effects, Custom
    TextData data;                 // data.text = the default words
    double x = 0.0;                // centre offset, fraction of the canvas width
    double y = 0.0;                // fraction of the canvas height (+ = down)
    double rotation = 0.0;
    double duration = 4.0;         // seconds
    bool custom = false;

    QJsonObject toJson() const;
    static TextTemplate fromJson(const QJsonObject& o);
    // Transform for a canvas of this size.
    ClipTransform transform(int canvasW, int canvasH) const;
};

namespace textTemplates {

QVector<TextTemplate> builtIn();
QVector<TextTemplate> custom();
QVector<TextTemplate> all();                 // built-in first
std::optional<TextTemplate> find(const QString& id);
// Saves (or replaces, same name) a custom template. Returns its id ("custom-...").
QString saveCustom(const QString& name, const TextData& data, double x, double y, double rotation, double duration);
bool removeCustom(const QString& id);

} // namespace textTemplates
} // namespace ve
