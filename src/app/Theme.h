#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

class QApplication;

namespace ve::theme {

// Surfaces, darkest first. Everything in the editor is a grade of black.
inline const QColor kBg0(8, 8, 10);       // wells: timeline, lists, text fields
inline const QColor kBg1(15, 15, 17);     // window and panels
inline const QColor kBg2(21, 21, 24);     // panel titles, headers, menus
inline const QColor kBg3(28, 28, 32);     // buttons
inline const QColor kBg4(38, 38, 43);     // hover
inline const QColor kLine(37, 37, 42);    // hairlines
inline const QColor kEdge(52, 52, 58);    // control borders
inline const QColor kText(228, 228, 231);
inline const QColor kTextDim(155, 155, 163);
inline const QColor kTextOff(90, 90, 98);
inline const QColor kAccent(76, 141, 255);
inline const QColor kSelection(27, 43, 71);

// Claude's own palette (used by the Claude panel only).
inline const QColor kClaudeBg(31, 30, 29);
inline const QColor kClaudeRaised(43, 42, 39);
inline const QColor kClaudeBubble(20, 20, 19);
inline const QColor kClaudeLine(58, 57, 54);
inline const QColor kClaudeText(250, 249, 245);
inline const QColor kClaudeDim(156, 154, 146);
inline const QColor kClaudeCoral(217, 119, 87);

enum class Glyph {
    Play, Pause, SkipBack, SkipForward, StepBack, StepForward,
    ArrowUp, Stop, Plus, Check, ChevronDown, ChevronUp, Close, Float, Spark,
};

QPixmap pixmap(Glyph g, const QColor& color, int size, qreal dpr = 1.0);
QIcon icon(Glyph g, const QColor& color = kText, int size = 20);

// Folder with the PNGs the stylesheet refers to (created by apply()).
QString assetDir();

// Fusion + black palette + application stylesheet.
void apply(QApplication& app);

} // namespace ve::theme
