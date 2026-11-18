#include "core/TextTemplates.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSettings>

#include <algorithm>

namespace ve {

QJsonObject TextTemplate::toJson() const
{
    return {{"id", id}, {"name", name}, {"category", category}, {"text", data.toJson()}, {"x", x}, {"y", y},
            {"rotation", rotation}, {"duration", duration}, {"custom", custom}};
}

TextTemplate TextTemplate::fromJson(const QJsonObject& o)
{
    TextTemplate t;
    t.id = o.value("id").toString();
    t.name = o.value("name").toString();
    t.category = o.value("category").toString(QStringLiteral("Custom"));
    t.data = TextData::fromJson(o.value("text").toObject());
    t.x = std::clamp(o.value("x").toDouble(), -1.0, 1.0);
    t.y = std::clamp(o.value("y").toDouble(), -1.0, 1.0);
    t.rotation = o.value("rotation").toDouble();
    t.duration = std::clamp(o.value("duration").toDouble(4.0), 0.1, 3600.0);
    t.custom = o.value("custom").toBool();
    return t;
}

ClipTransform TextTemplate::transform(int canvasW, int canvasH) const
{
    ClipTransform tr;
    tr.x = x * canvasW;
    tr.y = y * canvasH;
    tr.rotation = rotation;
    return tr;
}

namespace textTemplates {

namespace {

TextTemplate make(const char* id, const QString& name, const char* category, const QString& text,
                  const std::function<void(TextStyle&, TextAnimation&)>& setup, double x = 0, double y = 0,
                  double duration = 4.0, double rotation = 0)
{
    TextTemplate t;
    t.id = QLatin1String(id);
    t.name = name;
    t.category = QLatin1String(category);
    t.data.text = text;
    t.data.templateId = t.id;
    setup(t.data.style, t.data.animation);
    t.x = x;
    t.y = y;
    t.duration = duration;
    t.rotation = rotation;
    return t;
}

QString settingsKey() { return QStringLiteral("text/customTemplates"); }

void storeCustom(const QVector<TextTemplate>& list)
{
    QJsonArray a;
    for (const auto& t : list) a.append(t.toJson());
    QSettings().setValue(settingsKey(), QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)));
}

} // namespace

QVector<TextTemplate> builtIn()
{
    static const QVector<TextTemplate> list = [] {
        QVector<TextTemplate> v;
        v << make("title", QObject::tr("Title"), "Titles", QObject::tr("Your Title"), [](TextStyle& s, TextAnimation& a) {
            s.font = QStringLiteral("Segoe UI Black");
            s.size = 0.11;
            s.shadow = true;
            s.shadowBlur = 0.15;
            a.in = QStringLiteral("pop");
            a.inDuration = 0.45;
        });
        v << make("subtitle", QObject::tr("Subtitle"), "Titles", QObject::tr("Subtitle text"), [](TextStyle& s, TextAnimation&) {
            s.font = QStringLiteral("Segoe UI Semibold");
            s.bold = false;
            s.size = 0.05;
            s.outlineWidth = 0.08;
        }, 0, 0.36);
        v << make("cinematic", QObject::tr("Cinematic"), "Titles", QObject::tr("THE BEGINNING"), [](TextStyle& s, TextAnimation& a) {
            s.font = QStringLiteral("Segoe UI Light");
            s.bold = false;
            s.size = 0.07;
            s.letterSpacing = 0.5;
            a.in = QStringLiteral("blur");
            a.inDuration = 1.2;
            a.outDuration = 1.0;
        }, 0, 0, 5.0);
        v << make("gradient", QObject::tr("Gradient title"), "Titles", QObject::tr("Golden Hour"), [](TextStyle& s, TextAnimation& a) {
            s.font = QStringLiteral("Segoe UI Black");
            s.size = 0.1;
            s.gradient = true;
            s.color = QColor(255, 226, 89);
            s.color2 = QColor(255, 120, 60);
            s.outline = QColor(40, 20, 10);
            s.outlineWidth = 0.04;
            a.in = QStringLiteral("drop");
            a.inDuration = 0.6;
        });
        v << make("quote", QObject::tr("Quote"), "Titles", QObject::tr("“Make it simple,\nbut significant.”"),
                  [](TextStyle& s, TextAnimation& a) {
            s.font = QStringLiteral("Georgia");
            s.bold = false;
            s.italic = true;
            s.size = 0.07;
            s.shadow = true;
            a.in = QStringLiteral("words");
            a.inDuration = 1.5;
        }, 0, 0, 5.0);
        v << make("arabic-title", QObject::tr("Arabic title"), "Titles", QStringLiteral("عنوان رئيسي"),
                  [](TextStyle& s, TextAnimation& a) {
            s.font = QStringLiteral("Segoe UI");
            s.size = 0.1;
            s.outlineWidth = 0.08;
            s.shadow = true;
            a.in = QStringLiteral("pop");
        });
        v << make("lower-third", QObject::tr("Lower third"), "Lower thirds", QObject::tr("Jane Doe\nDirector"),
                  [](TextStyle& s, TextAnimation& a) {
            s.font = QStringLiteral("Segoe UI Semibold");
            s.bold = false;
            s.size = 0.045;
            s.align = QStringLiteral("left");
            s.background = true;
            s.backgroundColor = QColor(22, 48, 110);
            s.backgroundOpacity = 0.9;
            s.backgroundRadius = 0.12;
            s.backgroundPadding = 0.45;
            a.in = QStringLiteral("slide-right");
            a.out = QStringLiteral("slide-left");
        }, -0.3, 0.32, 5.0);
        v << make("breaking", QObject::tr("Breaking news"), "Lower thirds", QObject::tr("BREAKING NEWS"),
                  [](TextStyle& s, TextAnimation& a) {
            s.font = QStringLiteral("Arial Black");
            s.size = 0.05;
            s.align = QStringLiteral("left");
            s.background = true;
            s.backgroundColor = QColor(200, 16, 46);
            s.backgroundOpacity = 1.0;
            s.backgroundRadius = 0.0;
            a.in = QStringLiteral("slide-right");
            a.out = QStringLiteral("slide-left");
        }, -0.25, 0.33);
        v << make("highlight", QObject::tr("Highlight box"), "Lower thirds", QObject::tr("Highlight"), [](TextStyle& s, TextAnimation& a) {
            s.font = QStringLiteral("Segoe UI Black");
            s.size = 0.06;
            s.color = Qt::black;
            s.background = true;
            s.backgroundMode = QStringLiteral("lines");
            s.backgroundColor = QColor(255, 212, 0);
            s.backgroundOpacity = 1.0;
            s.backgroundRadius = 0.15;
            a.in = QStringLiteral("pop");
        }, 0, 0.3);
        v << make("subscribe", QObject::tr("Subscribe button"), "Social", QObject::tr("SUBSCRIBE"), [](TextStyle& s, TextAnimation& a) {
            s.font = QStringLiteral("Arial Black");
            s.size = 0.055;
            s.background = true;
            s.backgroundColor = QColor(230, 0, 0);
            s.backgroundOpacity = 1.0;
            s.backgroundRadius = 0.35;
            s.backgroundPadding = 0.5;
            a.in = QStringLiteral("bounce");
            a.inDuration = 0.8;
            a.loop = QStringLiteral("pulse");
        }, 0, 0.3);
        v << make("social-caption", QObject::tr("Viral caption"), "Social", QObject::tr("wait for it..."), [](TextStyle& s, TextAnimation& a) {
            s.font = QStringLiteral("Arial Black");
            s.uppercase = true;
            s.size = 0.07;
            s.outlineWidth = 0.18;
            s.shadow = true;
            a.in = QStringLiteral("words");
            a.inDuration = 0.9;
        }, 0, 0.15, 3.0);
        v << make("sticker", QObject::tr("Wiggle sticker"), "Social", QObject::tr("WOW!"), [](TextStyle& s, TextAnimation& a) {
            s.font = QStringLiteral("Comic Sans MS");
            s.size = 0.12;
            s.color = QColor(255, 225, 0);
            s.outlineWidth = 0.15;
            s.shadow = true;
            a.in = QStringLiteral("pop");
            a.loop = QStringLiteral("wiggle");
            a.loopSpeed = 1.5;
        }, 0.25, -0.25, 3.0, -8);
        v << make("neon", QObject::tr("Neon"), "Effects", QObject::tr("NEON"), [](TextStyle& s, TextAnimation& a) {
            s.font = QStringLiteral("Segoe UI Black");
            s.size = 0.12;
            s.color = QColor(240, 255, 255);
            s.outline = QColor(0, 229, 255);
            s.outlineWidth = 0.05;
            s.shadow = true;
            s.shadowColor = QColor(0, 229, 255, 230);
            s.shadowX = s.shadowY = 0;
            s.shadowBlur = 0.45;
            a.in = QStringLiteral("zoom");
            a.loop = QStringLiteral("blink");
            a.loopSpeed = 0.5;
        });
        v << make("outline", QObject::tr("Hollow outline"), "Effects", QObject::tr("OUTLINE"), [](TextStyle& s, TextAnimation& a) {
            s.font = QStringLiteral("Segoe UI Black");
            s.size = 0.13;
            s.color = QColor(0, 0, 0, 0);
            s.outline = Qt::white;
            s.outlineWidth = 0.035;
            a.in = QStringLiteral("slide-up");
        });
        v << make("typewriter", QObject::tr("Typewriter"), "Effects", QObject::tr("Once upon a time..."), [](TextStyle& s, TextAnimation& a) {
            s.font = QStringLiteral("Consolas");
            s.bold = false;
            s.size = 0.06;
            s.shadow = true;
            a.in = QStringLiteral("typewriter");
            a.inDuration = 2.0;
        }, 0, 0, 5.0);
        v << make("rainbow", QObject::tr("Rainbow"), "Effects", QObject::tr("PARTY"), [](TextStyle& s, TextAnimation& a) {
            s.font = QStringLiteral("Segoe UI Black");
            s.size = 0.12;
            s.color = QColor(255, 60, 60);
            s.gradient = true;
            s.color2 = QColor(255, 220, 0);
            s.outline = Qt::white;
            s.outlineWidth = 0.06;
            a.in = QStringLiteral("spin");
            a.loop = QStringLiteral("rainbow");
            a.loopSpeed = 0.5;
        });
        return v;
    }();
    return list;
}

QVector<TextTemplate> custom()
{
    QVector<TextTemplate> out;
    const QJsonArray a = QJsonDocument::fromJson(QSettings().value(settingsKey()).toString().toUtf8()).array();
    for (const auto& v : a) {
        TextTemplate t = TextTemplate::fromJson(v.toObject());
        t.custom = true;
        if (!t.id.isEmpty()) out << t;
    }
    return out;
}

QVector<TextTemplate> all() { return builtIn() + custom(); }

std::optional<TextTemplate> find(const QString& id)
{
    for (const auto& t : all())
        if (t.id == id) return t;
    return std::nullopt;
}

QString saveCustom(const QString& name, const TextData& data, double x, double y, double rotation, double duration)
{
    QVector<TextTemplate> list = custom();
    QString slug = name.trimmed().toLower();
    slug.replace(QRegularExpression(QStringLiteral("[^\\w]+")), QStringLiteral("-"));
    TextTemplate t;
    t.id = QStringLiteral("custom-") + (slug.isEmpty() ? newId().left(8) : slug);
    t.name = name.trimmed().isEmpty() ? QObject::tr("My template") : name.trimmed();
    t.category = QStringLiteral("Custom");
    t.data = data;
    t.data.templateId = t.id;
    t.x = x;
    t.y = y;
    t.rotation = rotation;
    t.duration = duration;
    t.custom = true;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const TextTemplate& o) { return o.id == t.id; }), list.end());
    list << t;
    storeCustom(list);
    return t.id;
}

bool removeCustom(const QString& id)
{
    QVector<TextTemplate> list = custom();
    const auto n = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const TextTemplate& o) { return o.id == id; }), list.end());
    if (list.size() == n) return false;
    storeCustom(list);
    return true;
}

} // namespace textTemplates
} // namespace ve
