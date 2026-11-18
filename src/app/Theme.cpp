#include "app/Theme.h"

#include <QApplication>
#include <QDir>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStandardPaths>
#include <QStyleFactory>

#include <cmath>
#include <utility>

namespace ve::theme {

namespace {

// Glyphs are drawn on a 24 x 24 grid.
void paintGlyph(QPainter& p, Glyph g, const QColor& c)
{
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto stroke = [&](qreal w) {
        p.setPen(QPen(c, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
    };
    const auto solid = [&] {
        p.setPen(QPen(c, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(c);
    };
    const auto triangle = [&](QPointF a, QPointF b, QPointF d) {
        QPainterPath t;
        t.moveTo(a);
        t.lineTo(b);
        t.lineTo(d);
        t.closeSubpath();
        p.drawPath(t);
    };

    switch (g) {
    case Glyph::SkipForward:
    case Glyph::StepForward:
        // mirrored versions of the backward glyphs
        p.translate(24, 0);
        p.scale(-1, 1);
        paintGlyph(p, g == Glyph::SkipForward ? Glyph::SkipBack : Glyph::StepBack, c);
        return;
    case Glyph::Play:
        solid();
        triangle({8, 5.5}, {19, 12}, {8, 18.5});
        break;
    case Glyph::Pause:
        solid();
        p.drawRoundedRect(QRectF(6.5, 5.5, 3.6, 13), 1, 1);
        p.drawRoundedRect(QRectF(13.9, 5.5, 3.6, 13), 1, 1);
        break;
    case Glyph::SkipBack: // |<
        solid();
        p.drawRoundedRect(QRectF(5, 6, 2, 12), 0.8, 0.8);
        triangle({18.5, 6.5}, {9.5, 12}, {18.5, 17.5});
        break;
    case Glyph::StepBack: // <|
        solid();
        triangle({14, 6.5}, {5, 12}, {14, 17.5});
        p.drawRoundedRect(QRectF(16.5, 6, 2, 12), 0.8, 0.8);
        break;
    case Glyph::ArrowUp:
        stroke(2.3);
        p.drawLine(QPointF(12, 18.5), QPointF(12, 6));
        p.drawPolyline(QPolygonF({{6.5, 11.5}, {12, 6}, {17.5, 11.5}}));
        break;
    case Glyph::Stop:
        solid();
        p.drawRoundedRect(QRectF(7.5, 7.5, 9, 9), 1.5, 1.5);
        break;
    case Glyph::Plus:
        stroke(2);
        p.drawLine(QPointF(12, 5.5), QPointF(12, 18.5));
        p.drawLine(QPointF(5.5, 12), QPointF(18.5, 12));
        break;
    case Glyph::Check:
        stroke(2.8);
        p.drawPolyline(QPolygonF({{5.5, 12.5}, {10, 17}, {18.5, 7.5}}));
        break;
    case Glyph::ChevronDown:
        stroke(2.6);
        p.drawPolyline(QPolygonF({{5.5, 9}, {12, 15.5}, {18.5, 9}}));
        break;
    case Glyph::ChevronUp:
        stroke(2.6);
        p.drawPolyline(QPolygonF({{5.5, 15}, {12, 8.5}, {18.5, 15}}));
        break;
    case Glyph::Close:
        stroke(2.2);
        p.drawLine(QPointF(6.5, 6.5), QPointF(17.5, 17.5));
        p.drawLine(QPointF(17.5, 6.5), QPointF(6.5, 17.5));
        break;
    case Glyph::Float:
        stroke(2);
        p.drawRect(QRectF(5, 9.5, 9.5, 9.5));
        p.drawPolyline(QPolygonF({{9.5, 9.5}, {9.5, 5}, {19, 5}, {19, 14.5}, {14.5, 14.5}}));
        break;
    case Glyph::Spark: {
        // Claude's spark: uneven rounded spokes around a centre.
        static const qreal len[12] = {10.6, 8.6, 10.2, 9.0, 10.6, 8.4, 10.4, 9.2, 10.6, 8.6, 10.0, 9.0};
        stroke(2.3);
        p.translate(12, 12);
        for (int i = 0; i < 12; ++i) {
            p.save();
            p.rotate(i * 30.0 + 8.0);
            p.drawLine(QPointF(0, -2.6), QPointF(0, -len[i]));
            p.restore();
        }
        break;
    }
    }
}

QString hex(const QColor& c)
{
    return c.name(QColor::HexRgb);
}

// Stylesheet images cannot be painted, so the few we need are written as PNGs.
void writeAssets()
{
    struct Asset {
        const char* name;
        Glyph glyph;
        QColor color;
        int size;
    };
    const Asset assets[] = {
        {"check", Glyph::Check, Qt::white, 12},
        {"down", Glyph::ChevronDown, kTextDim, 10},
        {"up", Glyph::ChevronUp, kTextDim, 10},
        {"close", Glyph::Close, kTextDim, 12},
        {"float", Glyph::Float, kTextDim, 12},
        {"claude-down", Glyph::ChevronDown, kClaudeDim, 10},
        {"spark", Glyph::Spark, kClaudeCoral, 44},
    };
    const QDir dir(assetDir());
    dir.mkpath(QStringLiteral("."));
    for (const Asset& a : assets) {
        pixmap(a.glyph, a.color, a.size, 1.0).save(dir.filePath(QStringLiteral("%1.png").arg(QLatin1String(a.name))));
        pixmap(a.glyph, a.color, a.size, 2.0).save(dir.filePath(QStringLiteral("%1@2x.png").arg(QLatin1String(a.name))));
    }
}

const char* const kStyleSheet = R"(
* { outline: 0; }

QMainWindow, QDialog { background: @bg1; }
QMainWindow::separator { background: @bg0; width: 4px; height: 4px; }
QMainWindow::separator:hover { background: @accent; }
QSplitter::handle { background: @bg0; }

QMenuBar { background: @bg1; border-bottom: 1px solid @line; padding: 2px 4px; }
QMenuBar::item { background: transparent; padding: 5px 10px; border-radius: 4px; }
QMenuBar::item:selected, QMenuBar::item:pressed { background: @bg4; }
QMenu { background: @bg2; border: 1px solid @edge; padding: 5px; }
QMenu::item { padding: 6px 28px 6px 26px; border-radius: 4px; }
QMenu::item:selected { background: @sel; color: white; }
QMenu::item:disabled { color: @off; }
QMenu::separator { height: 1px; background: @line; margin: 5px 8px; }
QMenu::indicator { width: 12px; height: 12px; left: 8px; }
QMenu::indicator:checked { image: url("@dir/check.png"); }

QToolTip { background: @bg3; color: @text; border: 1px solid @edge; padding: 5px 8px; }

QStatusBar { background: @bg0; border-top: 1px solid @line; color: @dim; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: @dim; padding: 0 6px; }

QDockWidget { color: @dim; titlebar-close-icon: url("@dir/close.png"); titlebar-normal-icon: url("@dir/float.png"); }
QDockWidget::title { background: @bg2; padding: 7px 10px; border-bottom: 1px solid @line; text-align: left; }
QDockWidget::close-button, QDockWidget::float-button { border: none; background: transparent; border-radius: 3px; padding: 2px; }
QDockWidget::close-button:hover, QDockWidget::float-button:hover { background: @bg4; }

QTabBar { background: @bg0; qproperty-drawBase: 0; }
QTabBar::tab { background: transparent; color: @dim; padding: 7px 14px; border: none; border-bottom: 2px solid transparent; }
QTabBar::tab:bottom { border-bottom: none; border-top: 2px solid transparent; }
QTabBar::tab:hover { color: @text; background: @bg2; }
QTabBar::tab:selected { color: @text; background: @bg1; border-color: @accent; }
QTabWidget::pane { border: 1px solid @line; }

QPushButton { background: @bg3; color: @text; border: 1px solid @edge; border-radius: 5px; padding: 5px 12px; }
QPushButton:hover { background: @bg4; border-color: #4a4a52; }
QPushButton:pressed { background: @bg2; }
QPushButton:checked { background: @sel; border-color: @accent; }
QPushButton:default { border-color: @accent; }
QPushButton:disabled { background: @bg2; color: @off; border-color: @line; }

QToolButton { background: transparent; color: @text; border: 1px solid transparent; border-radius: 5px; padding: 4px; }
QToolButton:hover { background: @bg4; }
QToolButton:pressed { background: @bg2; }
QToolButton:checked { background: @sel; border-color: @accent; color: white; }
QToolButton:disabled { color: @off; }

/* Spin boxes are styled only when they show their arrows: Qt's stylesheet style gives a
   boxed spin box without arrows an empty text area, so those keep the Fusion look. */
QLineEdit, QPlainTextEdit, QTextEdit, QAbstractSpinBox[buttonSymbols="0"], QComboBox {
    background: @bg0; color: @text; border: 1px solid @edge; border-radius: 5px; padding: 4px 7px;
    selection-background-color: @accent; selection-color: white;
}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QAbstractSpinBox[buttonSymbols="0"]:focus, QComboBox:focus { border-color: @accent; }
QLineEdit:disabled, QPlainTextEdit:disabled, QTextEdit:disabled, QAbstractSpinBox[buttonSymbols="0"]:disabled, QComboBox:disabled {
    color: @off; border-color: @line;
}

/* the editor inside a spin box or combo box must not draw a second frame */
QAbstractSpinBox QLineEdit, QComboBox QLineEdit { background: transparent; border: none; padding: 0; }

QComboBox { padding-right: 24px; }
QComboBox:hover { border-color: #4a4a52; }
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 22px; border: none; }
QComboBox::down-arrow { image: url("@dir/down.png"); width: 10px; height: 10px; }
QComboBox QAbstractItemView {
    background: @bg2; color: @text; border: 1px solid @edge; padding: 3px;
    selection-background-color: @sel; selection-color: white;
}

QAbstractSpinBox[buttonSymbols="0"]::up-button, QAbstractSpinBox[buttonSymbols="0"]::down-button {
    subcontrol-origin: border; width: 18px; border: none; border-left: 1px solid @line; background: transparent;
}
QAbstractSpinBox[buttonSymbols="0"]::up-button { subcontrol-position: top right; }
QAbstractSpinBox[buttonSymbols="0"]::down-button { subcontrol-position: bottom right; }
QAbstractSpinBox[buttonSymbols="0"]::up-button:hover, QAbstractSpinBox[buttonSymbols="0"]::down-button:hover { background: @bg4; }
QAbstractSpinBox[buttonSymbols="0"]::up-arrow { image: url("@dir/up.png"); width: 8px; height: 8px; }
QAbstractSpinBox[buttonSymbols="0"]::down-arrow { image: url("@dir/down.png"); width: 8px; height: 8px; }

QCheckBox, QRadioButton { color: @text; spacing: 7px; }
QCheckBox::indicator, QGroupBox::indicator {
    width: 14px; height: 14px; border: 1px solid #4a4a52; border-radius: 3px; background: @bg0;
}
QCheckBox::indicator:hover, QGroupBox::indicator:hover { border-color: @accent; }
QCheckBox::indicator:checked, QGroupBox::indicator:checked {
    background: @accent; border-color: @accent; image: url("@dir/check.png");
}
QCheckBox::indicator:disabled, QGroupBox::indicator:disabled { border-color: @line; background: @bg2; }
QRadioButton::indicator { width: 14px; height: 14px; border: 1px solid #4a4a52; border-radius: 8px; background: @bg0; }
QRadioButton::indicator:hover { border-color: @accent; }
QRadioButton::indicator:checked { width: 6px; height: 6px; border: 5px solid @accent; background: white; }

QGroupBox { border: 1px solid @line; border-radius: 6px; margin-top: 18px; padding: 8px 6px 6px 6px; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 8px; padding: 0 4px; color: @dim; }

QSlider::groove:horizontal { height: 4px; background: @bg4; border-radius: 2px; }
QSlider::sub-page:horizontal { background: @accent; border-radius: 2px; }
QSlider::handle:horizontal { background: #dcdce0; width: 12px; height: 12px; margin: -4px 0; border-radius: 6px; }
QSlider::handle:horizontal:hover { background: white; }
QSlider::handle:horizontal:disabled { background: @off; }
QSlider::sub-page:horizontal:disabled { background: @edge; }

QScrollBar:vertical { background: @bg0; width: 10px; margin: 0; }
QScrollBar:horizontal { background: @bg0; height: 10px; margin: 0; }
QScrollBar::handle { background: @edge; border-radius: 3px; }
QScrollBar::handle:vertical { min-height: 28px; margin: 2px; }
QScrollBar::handle:horizontal { min-width: 28px; margin: 2px; }
QScrollBar::handle:hover { background: #50505a; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QAbstractScrollArea::corner { background: @bg0; }

QListView, QTreeView, QTableView {
    background: @bg0; color: @text; border: 1px solid @line; border-radius: 5px;
    alternate-background-color: @bg1; gridline-color: @line;
    selection-background-color: @sel; selection-color: white;
}
QListView::item:hover, QTreeView::item:hover { background: @bg2; }
QListView::item:selected, QTreeView::item:selected, QTableView::item:selected { background: @sel; color: white; }
QHeaderView { background: @bg2; }
QHeaderView::section {
    background: @bg2; color: @dim; border: none; border-right: 1px solid @line; border-bottom: 1px solid @line;
    padding: 5px 8px;
}
QTableCornerButton::section { background: @bg2; border: none; }

QProgressBar { background: @bg0; color: @text; border: 1px solid @edge; border-radius: 4px; text-align: center; }
QProgressBar::chunk { background: @accent; border-radius: 3px; }
)";

} // namespace

QPixmap pixmap(Glyph g, const QColor& color, int size, qreal dpr)
{
    const int px = int(std::lround(size * dpr));
    QPixmap pm(px, px);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        p.scale(px / 24.0, px / 24.0);
        paintGlyph(p, g, color);
    }
    pm.setDevicePixelRatio(dpr);
    return pm;
}

QIcon icon(Glyph g, const QColor& color, int size)
{
    QIcon ic;
    for (qreal dpr : {1.0, 2.0, 3.0}) {
        ic.addPixmap(pixmap(g, color, size, dpr), QIcon::Normal);
        ic.addPixmap(pixmap(g, kTextOff, size, dpr), QIcon::Disabled);
    }
    return ic;
}

QString assetDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/theme");
}

void apply(QApplication& app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette p;
    p.setColor(QPalette::Window, kBg1);
    p.setColor(QPalette::WindowText, kText);
    p.setColor(QPalette::Base, kBg0);
    p.setColor(QPalette::AlternateBase, kBg1);
    p.setColor(QPalette::ToolTipBase, kBg3);
    p.setColor(QPalette::ToolTipText, kText);
    p.setColor(QPalette::Text, kText);
    p.setColor(QPalette::PlaceholderText, kTextOff);
    p.setColor(QPalette::Button, kBg3);
    p.setColor(QPalette::ButtonText, kText);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Link, kAccent);
    p.setColor(QPalette::Highlight, kAccent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Light, kBg4);
    p.setColor(QPalette::Midlight, kBg3);
    p.setColor(QPalette::Mid, kBg2);
    p.setColor(QPalette::Dark, QColor(4, 4, 5));
    p.setColor(QPalette::Shadow, Qt::black);
    for (QPalette::ColorRole r : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText})
        p.setColor(QPalette::Disabled, r, kTextOff);
    p.setColor(QPalette::Disabled, QPalette::Highlight, kEdge);
    app.setPalette(p);

    writeAssets();
    QString qss = QString::fromLatin1(kStyleSheet);
    const std::pair<const char*, QString> tokens[] = {
        {"@dir", assetDir()},        {"@bg0", hex(kBg0)},   {"@bg1", hex(kBg1)},     {"@bg2", hex(kBg2)},
        {"@bg3", hex(kBg3)},         {"@bg4", hex(kBg4)},   {"@line", hex(kLine)},   {"@edge", hex(kEdge)},
        {"@text", hex(kText)},       {"@dim", hex(kTextDim)}, {"@off", hex(kTextOff)}, {"@accent", hex(kAccent)},
        {"@sel", hex(kSelection)},
    };
    for (const auto& [key, value] : tokens) qss.replace(QLatin1String(key), value);
    app.setStyleSheet(qss);
}

} // namespace ve::theme
