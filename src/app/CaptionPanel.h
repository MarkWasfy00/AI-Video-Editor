#pragma once

#include "core/Project.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;

namespace ve {

// Style of the on-screen captions (from the transcript): presets plus font,
// size, colours, outline, box, position, words per caption and animation.
// Every change shows immediately in the preview.
class CaptionPanel : public QWidget {
    Q_OBJECT
public:
    explicit CaptionPanel(Project* project, QWidget* parent = nullptr);

private:
    void load();                       // project -> controls
    void apply();                      // controls -> project
    QPushButton* colorButton(const QString& tip);
    void setButtonColor(QPushButton* b, const QColor& c);

    Project* m_project;
    bool m_loading = false;
    QCheckBox* m_enabled = nullptr;
    QComboBox* m_preset = nullptr;
    QFontComboBox* m_font = nullptr;
    QPushButton* m_fontFileBtn = nullptr;
    QLabel* m_fontFileLabel = nullptr;
    QString m_fontFile;                // .ttf / .otf in use (empty = installed font)
    QCheckBox* m_arabicAuto = nullptr;
    QFontComboBox* m_arabicFont = nullptr;
    QPushButton* m_arabicFileBtn = nullptr;
    QString m_arabicFontFile;
    QCheckBox* m_bold = nullptr;
    QCheckBox* m_italic = nullptr;
    QCheckBox* m_upper = nullptr;
    QSlider* m_size = nullptr;
    QPushButton* m_color = nullptr;
    QPushButton* m_highlight = nullptr;
    QComboBox* m_highlightMode = nullptr;
    QPushButton* m_outline = nullptr;
    QSlider* m_outlineWidth = nullptr;
    QCheckBox* m_shadow = nullptr;
    QCheckBox* m_box = nullptr;
    QPushButton* m_boxColor = nullptr;
    QSlider* m_boxOpacity = nullptr;
    QComboBox* m_position = nullptr;
    QSlider* m_offset = nullptr;
    QSpinBox* m_words = nullptr;
    QSpinBox* m_lines = nullptr;
    QComboBox* m_animation = nullptr;
    QLabel* m_status = nullptr;
    QColor m_colors[4];                // text, highlight, outline, box
};

} // namespace ve
