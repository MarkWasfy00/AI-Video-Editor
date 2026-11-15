#include "app/CaptionPanel.h"

#include "render/CaptionRenderer.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

namespace ve {

namespace {
QSlider* slider(int lo, int hi, QWidget* parent)
{
    auto* s = new QSlider(Qt::Horizontal, parent);
    s->setRange(lo, hi);
    return s;
}
} // namespace

CaptionPanel::CaptionPanel(Project* project, QWidget* parent)
    : QWidget(parent)
    , m_project(project)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);

    m_enabled = new QCheckBox(tr("Show captions on the video"), this);
    QFont bf = m_enabled->font();
    bf.setBold(true);
    m_enabled->setFont(bf);
    outer->addWidget(m_enabled);

    auto* presetRow = new QHBoxLayout;
    m_preset = new QComboBox(this);
    const QList<QPair<QString, QString>> presets = {
        {QStringLiteral("classic"), tr("Classic - white, outline, yellow word")},
        {QStringLiteral("bold"), tr("Bold - big caps in the middle")},
        {QStringLiteral("box"), tr("Box - text on a dark box")},
        {QStringLiteral("neon"), tr("Neon - glowing outline")},
        {QStringLiteral("minimal"), tr("Minimal - whole line, no highlight")},
        {QStringLiteral("karaoke"), tr("Karaoke - box behind the spoken word")},
        {QStringLiteral("arabic"), tr("Arabic - large, clear Arabic letters")}};
    for (const auto& [key, label] : presets) m_preset->addItem(label, key);
    presetRow->addWidget(new QLabel(tr("Style"), this));
    presetRow->addWidget(m_preset, 1);
    outer->addLayout(presetRow);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* body = new QWidget;
    auto* v = new QVBoxLayout(body);
    v->setContentsMargins(0, 0, 0, 0);

    auto* textBox = new QGroupBox(tr("Text"), body);
    auto* tf = new QFormLayout(textBox);
    m_font = new QFontComboBox(textBox);
    m_font->setToolTip(tr("Every font installed on this computer, shown in its own style"));
    m_font->setMaxVisibleItems(16);
    auto* fontFileRow = new QHBoxLayout;
    m_fontFileBtn = new QPushButton(tr("Add Font File..."), textBox);
    m_fontFileBtn->setToolTip(tr("Use a downloaded .ttf / .otf font without installing it (saved with the project)"));
    m_fontFileLabel = new QLabel(textBox);
    m_fontFileLabel->setStyleSheet(QStringLiteral("color: #9a9aa5;"));
    fontFileRow->addWidget(m_fontFileBtn);
    fontFileRow->addWidget(m_fontFileLabel, 1);
    auto* styleRow = new QHBoxLayout;
    m_bold = new QCheckBox(tr("Bold"), textBox);
    m_italic = new QCheckBox(tr("Italic"), textBox);
    m_upper = new QCheckBox(tr("ALL CAPS"), textBox);
    styleRow->addWidget(m_bold);
    styleRow->addWidget(m_italic);
    styleRow->addWidget(m_upper);
    m_size = slider(15, 200, textBox);   // per mille of the frame height
    m_color = colorButton(tr("Text color"));
    tf->addRow(tr("Font"), m_font);
    tf->addRow(QString(), fontFileRow);
    // Arabic words get their own face (fonts like Impact have no Arabic letters).
    m_arabicFont = new QFontComboBox(textBox);
    m_arabicFont->setWritingSystem(QFontDatabase::Arabic);   // only fonts with Arabic letters
    m_arabicFont->setToolTip(tr("Font for Arabic words. Popular caption fonts like Cairo or Tajawal can be added "
                                "as font files."));
    auto* arabicRow = new QHBoxLayout;
    m_arabicAuto = new QCheckBox(tr("Automatic"), textBox);
    m_arabicAuto->setToolTip(tr("Use the main font when it has Arabic letters, otherwise Segoe UI"));
    m_arabicFileBtn = new QPushButton(tr("Add Arabic Font File..."), textBox);
    arabicRow->addWidget(m_arabicAuto);
    arabicRow->addWidget(m_arabicFileBtn);
    arabicRow->addStretch(1);
    tf->addRow(tr("Arabic font"), m_arabicFont);
    tf->addRow(QString(), arabicRow);
    tf->addRow(QString(), styleRow);
    tf->addRow(tr("Size"), m_size);
    tf->addRow(tr("Color"), m_color);
    v->addWidget(textBox);

    auto* hlBox = new QGroupBox(tr("Spoken word"), body);
    auto* hf = new QFormLayout(hlBox);
    m_highlightMode = new QComboBox(hlBox);
    m_highlightMode->addItem(tr("Color the word"), QStringLiteral("color"));
    m_highlightMode->addItem(tr("Box behind the word"), QStringLiteral("box"));
    m_highlightMode->addItem(tr("No highlight"), QStringLiteral("none"));
    m_highlight = colorButton(tr("Highlight color"));
    hf->addRow(tr("Highlight"), m_highlightMode);
    hf->addRow(tr("Color"), m_highlight);
    v->addWidget(hlBox);

    auto* lookBox = new QGroupBox(tr("Outline, shadow, box"), body);
    auto* lf = new QFormLayout(lookBox);
    m_outline = colorButton(tr("Outline color"));
    m_outlineWidth = slider(0, 50, lookBox);   // percent of the font size
    m_shadow = new QCheckBox(tr("Drop shadow"), lookBox);
    m_box = new QCheckBox(tr("Background box"), lookBox);
    m_boxColor = colorButton(tr("Box color"));
    m_boxOpacity = slider(0, 100, lookBox);
    lf->addRow(tr("Outline"), m_outline);
    lf->addRow(tr("Thickness"), m_outlineWidth);
    lf->addRow(QString(), m_shadow);
    lf->addRow(QString(), m_box);
    lf->addRow(tr("Box color"), m_boxColor);
    lf->addRow(tr("Box opacity"), m_boxOpacity);
    v->addWidget(lookBox);

    auto* layoutBox = new QGroupBox(tr("Position && timing"), body);
    auto* pf = new QFormLayout(layoutBox);
    m_position = new QComboBox(layoutBox);
    m_position->addItem(tr("Bottom"), QStringLiteral("bottom"));
    m_position->addItem(tr("Middle"), QStringLiteral("middle"));
    m_position->addItem(tr("Top"), QStringLiteral("top"));
    m_offset = slider(0, 45, layoutBox);       // percent of the frame height
    m_words = new QSpinBox(layoutBox);
    m_words->setRange(0, 20);
    m_words->setSpecialValueText(tr("Whole line"));
    m_lines = new QSpinBox(layoutBox);
    m_lines->setRange(1, 4);
    m_animation = new QComboBox(layoutBox);
    m_animation->addItem(tr("Pop in"), QStringLiteral("pop"));
    m_animation->addItem(tr("Fade"), QStringLiteral("fade"));
    m_animation->addItem(tr("None"), QStringLiteral("none"));
    pf->addRow(tr("Position"), m_position);
    pf->addRow(tr("Distance from edge"), m_offset);
    pf->addRow(tr("Words at a time"), m_words);
    pf->addRow(tr("Max lines"), m_lines);
    pf->addRow(tr("Animation"), m_animation);
    v->addWidget(layoutBox);
    v->addStretch(1);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QStringLiteral("color: #9a9aa5;"));
    outer->addWidget(m_status);

    // Wire every control to apply()
    for (QCheckBox* c : {m_enabled, m_bold, m_italic, m_upper, m_shadow, m_box})
        connect(c, &QCheckBox::toggled, this, &CaptionPanel::apply);
    for (QSlider* s : {m_size, m_outlineWidth, m_boxOpacity, m_offset})
        connect(s, &QSlider::valueChanged, this, &CaptionPanel::apply);
    for (QComboBox* c : {m_highlightMode, m_position, m_animation})
        connect(c, &QComboBox::currentIndexChanged, this, &CaptionPanel::apply);
    for (QSpinBox* s : {m_words, m_lines}) connect(s, &QSpinBox::valueChanged, this, &CaptionPanel::apply);
    connect(m_font, &QFontComboBox::currentFontChanged, this, [this](const QFont& f) {
        if (m_loading) return;
        // Picking a different font from the list stops using the font file.
        if (!m_fontFile.isEmpty() && f.family() != CaptionRenderer::loadFontFile(m_fontFile)) m_fontFile.clear();
        apply();
    });
    connect(m_arabicAuto, &QCheckBox::toggled, this, [this](bool on) {
        m_arabicFont->setEnabled(!on);
        if (!m_loading) {
            if (on) m_arabicFontFile.clear();
            apply();
        }
    });
    connect(m_arabicFont, &QFontComboBox::currentFontChanged, this, [this](const QFont& f) {
        if (m_loading) return;
        if (!m_arabicFontFile.isEmpty() && f.family() != CaptionRenderer::loadFontFile(m_arabicFontFile)) m_arabicFontFile.clear();
        apply();
    });
    connect(m_arabicFileBtn, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Arabic Font File"), QString(),
                                                          tr("Fonts (*.ttf *.otf *.ttc);;All files (*)"));
        if (path.isEmpty()) return;
        const QString family = CaptionRenderer::loadFontFile(path);
        if (family.isEmpty() || !QFontDatabase::writingSystems(family).contains(QFontDatabase::Arabic)) {
            QMessageBox::warning(this, tr("Arabic Font File"), tr("This file is not a font with Arabic letters."));
            return;
        }
        m_arabicFontFile = path;
        m_loading = true;
        m_arabicAuto->setChecked(false);
        m_arabicFont->setEnabled(true);
        m_arabicFont->setCurrentFont(QFont(family));
        m_loading = false;
        apply();
    });
    connect(m_fontFileBtn, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Font File"), QString(),
                                                          tr("Fonts (*.ttf *.otf *.ttc);;All files (*)"));
        if (path.isEmpty()) return;
        const QString family = CaptionRenderer::loadFontFile(path);
        if (family.isEmpty()) {
            QMessageBox::warning(this, tr("Font File"), tr("This file is not a font Qt can use."));
            return;
        }
        m_fontFile = path;
        m_loading = true;
        m_font->setCurrentFont(QFont(family));   // now listed, since it is loaded
        m_loading = false;
        apply();
    });
    connect(m_preset, &QComboBox::activated, this, [this] {
        CaptionStyle s = captionPreset(m_preset->currentData().toString(), m_project->captionStyle());
        s.enabled = true;                        // picking a style shows it
        m_project->setCaptionStyle(s);
    });
    const QPushButton* buttons[4] = {m_color, m_highlight, m_outline, m_boxColor};
    for (int i = 0; i < 4; ++i) {
        connect(buttons[i], &QPushButton::clicked, this, [this, i] {
            const QColor c = QColorDialog::getColor(m_colors[i], this, tr("Caption color"));
            if (!c.isValid()) return;
            m_colors[i] = c;
            apply();
        });
    }
    connect(m_project, &Project::captionsChanged, this, &CaptionPanel::load);
    connect(m_project, &Project::transcriptsChanged, this, &CaptionPanel::load);
    load();
}

QPushButton* CaptionPanel::colorButton(const QString& tip)
{
    auto* b = new QPushButton(this);
    b->setToolTip(tip);
    b->setFixedSize(46, 22);
    return b;
}

void CaptionPanel::setButtonColor(QPushButton* b, const QColor& c)
{
    b->setStyleSheet(QStringLiteral("QPushButton { background: %1; border: 1px solid #666; border-radius: 3px; }").arg(c.name()));
}

void CaptionPanel::load()
{
    m_loading = true;
    const CaptionStyle& s = m_project->captionStyle();
    m_enabled->setChecked(s.enabled);
    const int pi = m_preset->findData(s.preset);
    if (pi >= 0) m_preset->setCurrentIndex(pi);
    m_fontFile = s.fontFile;
    if (!m_fontFile.isEmpty()) CaptionRenderer::loadFontFile(m_fontFile); // make it pickable
    m_font->setCurrentFont(QFont(s.font));
    m_fontFileLabel->setText(m_fontFile.isEmpty() ? tr("Installed font") : tr("From %1").arg(QFileInfo(m_fontFile).fileName()));
    m_fontFileLabel->setToolTip(m_fontFile);
    m_arabicFontFile = s.arabicFontFile;
    if (!m_arabicFontFile.isEmpty()) CaptionRenderer::loadFontFile(m_arabicFontFile);
    m_arabicAuto->setChecked(s.arabicFont.isEmpty() && s.arabicFontFile.isEmpty());
    m_arabicFont->setEnabled(!m_arabicAuto->isChecked());
    m_arabicFont->setCurrentFont(QFont(CaptionRenderer::arabicFamily(s)));
    m_bold->setChecked(s.bold);
    m_italic->setChecked(s.italic);
    m_upper->setChecked(s.uppercase);
    m_size->setValue(int(std::lround(s.size * 1000)));
    m_colors[0] = s.color;
    m_colors[1] = s.highlight;
    m_colors[2] = s.outline;
    m_colors[3] = s.boxColor;
    setButtonColor(m_color, s.color);
    setButtonColor(m_highlight, s.highlight);
    setButtonColor(m_outline, s.outline);
    setButtonColor(m_boxColor, s.boxColor);
    m_highlightMode->setCurrentIndex(std::max(0, m_highlightMode->findData(s.highlightMode)));
    m_outlineWidth->setValue(int(std::lround(s.outlineWidth * 100)));
    m_shadow->setChecked(s.shadow);
    m_box->setChecked(s.box);
    m_boxOpacity->setValue(int(std::lround(s.boxOpacity * 100)));
    m_position->setCurrentIndex(std::max(0, m_position->findData(s.position)));
    m_offset->setValue(int(std::lround(s.offset * 100)));
    m_words->setValue(s.wordsPerCaption);
    m_lines->setValue(s.maxLines);
    m_animation->setCurrentIndex(std::max(0, m_animation->findData(s.animation)));
    m_loading = false;

    const int n = int(m_project->captionChunks().size());
    m_status->setText(n == 0 ? tr("No transcript on the timeline yet: transcribe a clip in the Transcript panel first.")
                             : tr("%n caption(s) from the transcript. Edit the words in the Transcript panel; "
                                  "captions are included when you export.", nullptr, n));
}

void CaptionPanel::apply()
{
    if (m_loading) return;
    CaptionStyle s = m_project->captionStyle();
    s.enabled = m_enabled->isChecked();
    s.font = m_font->currentFont().family();
    s.fontFile = m_fontFile;
    s.arabicFont = m_arabicAuto->isChecked() ? QString() : m_arabicFont->currentFont().family();
    s.arabicFontFile = m_arabicAuto->isChecked() ? QString() : m_arabicFontFile;
    s.bold = m_bold->isChecked();
    s.italic = m_italic->isChecked();
    s.uppercase = m_upper->isChecked();
    s.size = m_size->value() / 1000.0;
    s.color = m_colors[0];
    s.highlight = m_colors[1];
    s.outline = m_colors[2];
    s.boxColor = m_colors[3];
    s.highlightMode = m_highlightMode->currentData().toString();
    s.outlineWidth = m_outlineWidth->value() / 100.0;
    s.shadow = m_shadow->isChecked();
    s.box = m_box->isChecked();
    s.boxOpacity = m_boxOpacity->value() / 100.0;
    s.position = m_position->currentData().toString();
    s.offset = m_offset->value() / 100.0;
    s.wordsPerCaption = m_words->value();
    s.maxLines = m_lines->value();
    s.animation = m_animation->currentData().toString();
    m_project->setCaptionStyle(s);
}

} // namespace ve
