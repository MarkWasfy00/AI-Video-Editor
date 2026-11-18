#pragma once

#include "core/Project.h"

#include <QColor>
#include <QWidget>

#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QGroupBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QTimer;

namespace ve {

// Text overlays: a gallery of templates (built-in and saved) to add titles,
// lower thirds and stickers, and every setting of the selected text clip:
// words, font, size, colours / gradient, outline, shadow, background box,
// spacing, alignment and in / out / loop animations. Edits are undoable.
class TextPanel : public QWidget {
    Q_OBJECT
public:
    struct Host {
        std::function<void(const QString& templateId)> addText;      // at the playhead
        std::function<void(const QString& clipId)> speak;            // text to speech for a text clip
    };

    TextPanel(Project* project, Host host, QWidget* parent = nullptr);

    QString currentTemplate() const;   // the template picked in the gallery ("title" if none)
    void focusText();                  // put the cursor in the words box (all selected)

public slots:
    void setClip(const QString& clipId);
    void reloadTemplates();

private:
    void load();                       // clip -> controls
    void apply(const QString& key);    // controls -> clip (merged undo per property)
    QPushButton* colorButton(const QString& tip, int index);
    void setButtonColor(QPushButton* b, const QColor& c);

    Project* m_project;
    Host m_host;
    QString m_clipId;
    bool m_loading = false;

    QListWidget* m_gallery = nullptr;
    QPushButton* m_add = nullptr;
    QPushButton* m_applyTemplate = nullptr;
    QLabel* m_hint = nullptr;
    QWidget* m_editor = nullptr;

    QPlainTextEdit* m_text = nullptr;
    QTimer* m_textTimer = nullptr;
    QFontComboBox* m_font = nullptr;
    QPushButton* m_fontFileBtn = nullptr;
    QString m_fontFile;
    QSlider* m_size = nullptr;
    QCheckBox* m_bold = nullptr;
    QCheckBox* m_italic = nullptr;
    QCheckBox* m_underline = nullptr;
    QCheckBox* m_upper = nullptr;
    QComboBox* m_align = nullptr;
    QSlider* m_letter = nullptr;
    QSlider* m_lineSpacing = nullptr;
    QSlider* m_maxWidth = nullptr;
    QCheckBox* m_gradient = nullptr;
    QSlider* m_outlineWidth = nullptr;
    QCheckBox* m_shadow = nullptr;
    QSlider* m_shadowBlur = nullptr;
    QSlider* m_shadowDistance = nullptr;
    QCheckBox* m_background = nullptr;
    QComboBox* m_bgMode = nullptr;
    QSlider* m_bgOpacity = nullptr;
    QSlider* m_bgPadding = nullptr;
    QSlider* m_bgRadius = nullptr;
    QComboBox* m_animIn = nullptr;
    QDoubleSpinBox* m_animInDur = nullptr;
    QComboBox* m_animOut = nullptr;
    QDoubleSpinBox* m_animOutDur = nullptr;
    QComboBox* m_loop = nullptr;
    QDoubleSpinBox* m_loopSpeed = nullptr;
    QPushButton* m_colorBtns[5] = {};  // text, gradient end, outline, shadow, background
    QColor m_colors[5];
};

} // namespace ve
