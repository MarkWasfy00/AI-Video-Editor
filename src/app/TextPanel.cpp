#include "app/TextPanel.h"

#include "core/TextTemplates.h"
#include "render/CaptionRenderer.h"
#include "render/TextRenderer.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
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

QString niceName(const QString& id)
{
    QString s = id;
    s.replace(QLatin1Char('-'), QLatin1Char(' '));
    if (!s.isEmpty()) s[0] = s[0].toUpper();
    return s;
}

void fillCombo(QComboBox* c, const QStringList& ids)
{
    for (const QString& id : ids) c->addItem(niceName(id), id);
}

QIcon templateIcon(const TextTemplate& t)
{
    const QSize sz(168, 94);
    QImage canvas(sz, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(QColor(27, 27, 32));
    TextData d = t.data;
    d.style.maxWidth = 0.95;
    const QImage img = TextRenderer::render(d, 640, 360);
    if (!img.isNull()) {
        QPainter p(&canvas);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const double k = std::min({double(sz.width() - 8) / img.width(), double(sz.height() - 8) / img.height(), 0.5});
        const QSizeF s(img.width() * k, img.height() * k);
        p.drawImage(QRectF((sz.width() - s.width()) / 2, (sz.height() - s.height()) / 2, s.width(), s.height()), img);
    }
    return QIcon(QPixmap::fromImage(canvas));
}

} // namespace

TextPanel::TextPanel(Project* project, Host host, QWidget* parent)
    : QWidget(parent)
    , m_project(project)
    , m_host(std::move(host))
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);

    // Template gallery
    m_gallery = new QListWidget(this);
    m_gallery->setViewMode(QListView::IconMode);
    m_gallery->setIconSize(QSize(168, 94));
    m_gallery->setGridSize(QSize(180, 122));
    m_gallery->setResizeMode(QListView::Adjust);
    m_gallery->setMovement(QListView::Static);
    m_gallery->setWordWrap(true);
    m_gallery->setMinimumHeight(140);
    m_gallery->setMaximumHeight(270);
    m_gallery->setContextMenuPolicy(Qt::CustomContextMenu);
    m_gallery->setToolTip(tr("Double-click a template to add it at the playhead"));
    outer->addWidget(m_gallery);

    auto* row = new QHBoxLayout;
    m_add = new QPushButton(tr("Add Text"), this);
    m_add->setToolTip(tr("Add the selected template at the playhead (Ctrl+T)"));
    m_applyTemplate = new QPushButton(tr("Apply Style to Selected"), this);
    m_applyTemplate->setToolTip(tr("Give the selected text clip this template's look and animation (keeps its words)"));
    row->addWidget(m_add);
    row->addWidget(m_applyTemplate);
    row->addStretch(1);
    outer->addLayout(row);

    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet(QStringLiteral("color: #9a9aa5;"));
    outer->addWidget(m_hint);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_editor = new QWidget;
    auto* v = new QVBoxLayout(m_editor);
    v->setContentsMargins(0, 0, 0, 0);

    // Words + font
    auto* textBox = new QGroupBox(tr("Text"), m_editor);
    auto* tf = new QFormLayout(textBox);
    m_text = new QPlainTextEdit(textBox);
    m_text->setPlaceholderText(tr("Type the text (Enter for a new line)"));
    m_text->setMaximumHeight(80);
    m_textTimer = new QTimer(this);
    m_textTimer->setSingleShot(true);
    m_textTimer->setInterval(250);
    m_font = new QFontComboBox(textBox);
    m_font->setMaxVisibleItems(16);
    m_fontFileBtn = new QPushButton(tr("Font File..."), textBox);
    m_fontFileBtn->setToolTip(tr("Use a downloaded .ttf / .otf font without installing it (saved with the project)"));
    auto* fontRow = new QHBoxLayout;
    fontRow->addWidget(m_font, 1);
    fontRow->addWidget(m_fontFileBtn);
    m_size = slider(10, 400, textBox);   // per mille of the frame height
    auto* styleRow = new QHBoxLayout;
    m_bold = new QCheckBox(tr("Bold"), textBox);
    m_italic = new QCheckBox(tr("Italic"), textBox);
    m_underline = new QCheckBox(tr("Underline"), textBox);
    m_upper = new QCheckBox(tr("CAPS"), textBox);
    for (QCheckBox* c : {m_bold, m_italic, m_underline, m_upper}) styleRow->addWidget(c);
    styleRow->addStretch(1);
    m_align = new QComboBox(textBox);
    m_align->addItem(tr("Left"), QStringLiteral("left"));
    m_align->addItem(tr("Centre"), QStringLiteral("center"));
    m_align->addItem(tr("Right"), QStringLiteral("right"));
    m_letter = slider(-20, 150, textBox);       // percent of the font size
    m_lineSpacing = slider(60, 300, textBox);   // percent
    m_maxWidth = slider(10, 100, textBox);      // percent of the frame width
    auto* colorRow = new QHBoxLayout;
    m_colorBtns[0] = colorButton(tr("Text color"), 0);
    m_gradient = new QCheckBox(tr("Gradient to"), textBox);
    m_colorBtns[1] = colorButton(tr("Gradient end color (bottom)"), 1);
    colorRow->addWidget(m_colorBtns[0]);
    colorRow->addSpacing(10);
    colorRow->addWidget(m_gradient);
    colorRow->addWidget(m_colorBtns[1]);
    colorRow->addStretch(1);
    tf->addRow(m_text);
    tf->addRow(tr("Font"), fontRow);
    tf->addRow(QString(), styleRow);
    tf->addRow(tr("Size"), m_size);
    tf->addRow(tr("Color"), colorRow);
    tf->addRow(tr("Align"), m_align);
    tf->addRow(tr("Letter spacing"), m_letter);
    tf->addRow(tr("Line spacing"), m_lineSpacing);
    tf->addRow(tr("Wrap width"), m_maxWidth);
    v->addWidget(textBox);

    // Outline, shadow, background
    auto* lookBox = new QGroupBox(tr("Outline, shadow, background"), m_editor);
    auto* lf = new QFormLayout(lookBox);
    auto* outlineRow = new QHBoxLayout;
    m_colorBtns[2] = colorButton(tr("Outline color"), 2);
    m_outlineWidth = slider(0, 50, lookBox);    // percent of the font size
    outlineRow->addWidget(m_colorBtns[2]);
    outlineRow->addWidget(m_outlineWidth, 1);
    auto* shadowRow = new QHBoxLayout;
    m_shadow = new QCheckBox(tr("Shadow / glow"), lookBox);
    m_colorBtns[3] = colorButton(tr("Shadow color (a bright colour with no distance makes a glow)"), 3);
    shadowRow->addWidget(m_shadow);
    shadowRow->addWidget(m_colorBtns[3]);
    shadowRow->addStretch(1);
    m_shadowDistance = slider(0, 40, lookBox);  // percent of the font size
    m_shadowBlur = slider(0, 80, lookBox);
    auto* bgRow = new QHBoxLayout;
    m_background = new QCheckBox(tr("Background"), lookBox);
    m_colorBtns[4] = colorButton(tr("Background color"), 4);
    m_bgMode = new QComboBox(lookBox);
    m_bgMode->addItem(tr("One box"), QStringLiteral("block"));
    m_bgMode->addItem(tr("Box per line"), QStringLiteral("lines"));
    bgRow->addWidget(m_background);
    bgRow->addWidget(m_colorBtns[4]);
    bgRow->addWidget(m_bgMode);
    bgRow->addStretch(1);
    m_bgOpacity = slider(0, 100, lookBox);
    m_bgPadding = slider(0, 150, lookBox);
    m_bgRadius = slider(0, 150, lookBox);
    lf->addRow(tr("Outline"), outlineRow);
    lf->addRow(QString(), shadowRow);
    lf->addRow(tr("Shadow distance"), m_shadowDistance);
    lf->addRow(tr("Shadow softness"), m_shadowBlur);
    lf->addRow(QString(), bgRow);
    lf->addRow(tr("Opacity"), m_bgOpacity);
    lf->addRow(tr("Padding"), m_bgPadding);
    lf->addRow(tr("Corners"), m_bgRadius);
    v->addWidget(lookBox);

    // Animation
    auto* animBox = new QGroupBox(tr("Animation"), m_editor);
    auto* af = new QFormLayout(animBox);
    auto durSpin = [&](double max) {
        auto* s = new QDoubleSpinBox(animBox);
        s->setRange(0.0, max);
        s->setSingleStep(0.1);
        s->setDecimals(2);
        s->setSuffix(tr(" s"));
        return s;
    };
    m_animIn = new QComboBox(animBox);
    fillCombo(m_animIn, textAnimationsIn());
    m_animInDur = durSpin(10);
    m_animOut = new QComboBox(animBox);
    fillCombo(m_animOut, textAnimationsOut());
    m_animOutDur = durSpin(10);
    m_loop = new QComboBox(animBox);
    fillCombo(m_loop, textAnimationsLoop());
    m_loopSpeed = new QDoubleSpinBox(animBox);
    m_loopSpeed->setRange(0.05, 10);
    m_loopSpeed->setSingleStep(0.1);
    m_loopSpeed->setSuffix(tr(" /s"));
    auto pair = [](QWidget* a, QWidget* b) {
        auto* h = new QHBoxLayout;
        h->addWidget(a, 1);
        h->addWidget(b);
        return h;
    };
    af->addRow(tr("In"), pair(m_animIn, m_animInDur));
    af->addRow(tr("Out"), pair(m_animOut, m_animOutDur));
    af->addRow(tr("Loop"), pair(m_loop, m_loopSpeed));
    v->addWidget(animBox);

    auto* actions = new QHBoxLayout;
    auto* saveTpl = new QPushButton(tr("Save as Template..."), m_editor);
    saveTpl->setToolTip(tr("Keep this look, animation and position as a template of your own"));
    auto* speak = new QPushButton(tr("Read Aloud (Text to Speech)..."), m_editor);
    speak->setToolTip(tr("Add a voice-over that speaks this text"));
    actions->addWidget(saveTpl);
    actions->addWidget(speak);
    actions->addStretch(1);
    v->addLayout(actions);
    v->addStretch(1);
    scroll->setWidget(m_editor);
    outer->addWidget(scroll, 1);

    // --- wiring
    connect(m_add, &QPushButton::clicked, this, [this] { if (m_host.addText) m_host.addText(currentTemplate()); });
    connect(m_gallery, &QListWidget::itemDoubleClicked, this, [this] { if (m_host.addText) m_host.addText(currentTemplate()); });
    connect(m_applyTemplate, &QPushButton::clicked, this, [this] {
        const Clip* c = m_project->findClip(m_clipId);
        const auto tpl = textTemplates::find(currentTemplate());
        if (!c || !c->isText() || !tpl) return;
        Clip copy = *c;
        TextData d = tpl->data;
        d.text = c->text->text;
        copy.text = d;
        m_project->edit(tr("Apply text template"), [&] { m_project->updateClip(copy); });
    });
    connect(m_gallery, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* it = m_gallery->itemAt(pos);
        if (!it) return;
        const QString id = it->data(Qt::UserRole).toString();
        QMenu menu(this);
        menu.addAction(tr("Add at Playhead"), this, [this, id] { if (m_host.addText) m_host.addText(id); });
        if (id.startsWith(QLatin1String("custom-")))
            menu.addAction(tr("Delete Template"), this, [this, id] {
                textTemplates::removeCustom(id);
                reloadTemplates();
            });
        menu.exec(m_gallery->viewport()->mapToGlobal(pos));
    });
    connect(saveTpl, &QPushButton::clicked, this, [this] {
        const Clip* c = m_project->findClip(m_clipId);
        if (!c || !c->isText()) return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Save Text Template"), tr("Template name:"), QLineEdit::Normal,
                                                   c->name, &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        const double w = m_project->settings().width, h = m_project->settings().height;
        textTemplates::saveCustom(name, *c->text, c->transform.x / w, c->transform.y / h, c->transform.rotation,
                                  usToSeconds(c->duration));
        reloadTemplates();
    });
    connect(speak, &QPushButton::clicked, this, [this] { if (m_host.speak && !m_clipId.isEmpty()) m_host.speak(m_clipId); });

    connect(m_text, &QPlainTextEdit::textChanged, this, [this] { if (!m_loading) m_textTimer->start(); });
    connect(m_textTimer, &QTimer::timeout, this, [this] { apply(QStringLiteral("text")); });
    for (QCheckBox* c : {m_bold, m_italic, m_underline, m_upper, m_gradient, m_shadow, m_background})
        connect(c, &QCheckBox::toggled, this, [this, c] { apply(c->text()); });
    for (QSlider* s : {m_size, m_letter, m_lineSpacing, m_maxWidth, m_outlineWidth, m_shadowDistance, m_shadowBlur,
                       m_bgOpacity, m_bgPadding, m_bgRadius})
        connect(s, &QSlider::valueChanged, this, [this, s] { apply(QStringLiteral("slider%1").arg(quintptr(s))); });
    for (QComboBox* c : {m_align, m_bgMode, m_animIn, m_animOut, m_loop})
        connect(c, &QComboBox::currentIndexChanged, this, [this, c] { apply(QStringLiteral("combo%1").arg(quintptr(c))); });
    for (QDoubleSpinBox* s : {m_animInDur, m_animOutDur, m_loopSpeed})
        connect(s, &QDoubleSpinBox::valueChanged, this, [this, s] { apply(QStringLiteral("spin%1").arg(quintptr(s))); });
    connect(m_font, &QFontComboBox::currentFontChanged, this, [this](const QFont& f) {
        if (m_loading) return;
        if (!m_fontFile.isEmpty() && f.family() != CaptionRenderer::loadFontFile(m_fontFile)) m_fontFile.clear();
        apply(QStringLiteral("font"));
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
        m_font->setCurrentFont(QFont(family));
        m_loading = false;
        apply(QStringLiteral("font"));
    });

    connect(m_project, &Project::changed, this, &TextPanel::load);
    reloadTemplates();
    load();
}

QPushButton* TextPanel::colorButton(const QString& tip, int index)
{
    auto* b = new QPushButton(this);
    b->setToolTip(tip);
    b->setFixedSize(46, 22);
    connect(b, &QPushButton::clicked, this, [this, index] {
        const QColor c = QColorDialog::getColor(m_colors[index], this, tr("Text color"), QColorDialog::ShowAlphaChannel);
        if (!c.isValid()) return;
        m_colors[index] = c;
        setButtonColor(m_colorBtns[index], c);
        apply(QStringLiteral("color%1").arg(index));
    });
    return b;
}

void TextPanel::setButtonColor(QPushButton* b, const QColor& c)
{
    // Transparent colours show a checkerboard-ish grey so they stay visible.
    const QString fill = c.alpha() < 30 ? QStringLiteral("qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 #777, stop:0.5 #777, stop:0.51 #bbb, stop:1 #bbb)")
                                        : c.name(QColor::HexRgb);
    b->setStyleSheet(QStringLiteral("QPushButton { background: %1; border: 1px solid #666; border-radius: 3px; }").arg(fill));
}

void TextPanel::reloadTemplates()
{
    const QString keep = currentTemplate();
    m_gallery->clear();
    for (const TextTemplate& t : textTemplates::all()) {
        auto* it = new QListWidgetItem(templateIcon(t), t.name, m_gallery);
        it->setData(Qt::UserRole, t.id);
        it->setToolTip(tr("%1 (%2)\nIn: %3, out: %4%5")
                           .arg(t.name, t.category, niceName(t.data.animation.in), niceName(t.data.animation.out),
                                t.data.animation.loop == QLatin1String("none") ? QString()
                                                                               : tr(", loop: %1").arg(niceName(t.data.animation.loop))));
        if (t.id == keep) m_gallery->setCurrentItem(it);
    }
    if (!m_gallery->currentItem() && m_gallery->count() > 0) m_gallery->setCurrentRow(0);
}

QString TextPanel::currentTemplate() const
{
    const QListWidgetItem* it = m_gallery ? m_gallery->currentItem() : nullptr;
    return it ? it->data(Qt::UserRole).toString() : QStringLiteral("title");
}

void TextPanel::focusText()
{
    m_text->setFocus();
    m_text->selectAll();
}

void TextPanel::setClip(const QString& clipId)
{
    if (m_textTimer->isActive()) {   // finish typing into the previous clip first
        m_textTimer->stop();
        apply(QStringLiteral("text"));
    }
    m_clipId = clipId;
    load();
}

void TextPanel::load()
{
    const Clip* c = m_project->findClip(m_clipId);
    const bool text = c && c->isText();
    m_editor->setEnabled(text);
    m_applyTemplate->setEnabled(text);
    m_hint->setText(text ? tr("Editing “%1”. Move, scale and rotate it in the Inspector.").arg(c->name)
                         : tr("Pick a template and click Add Text, then edit it here. Select a text clip in the "
                              "timeline to change it."));
    if (!text || m_textTimer->isActive()) return;
    const TextData& d = *c->text;
    const TextStyle& s = d.style;
    m_loading = true;
    if (m_text->toPlainText() != d.text) m_text->setPlainText(d.text);
    m_fontFile = s.fontFile;
    if (!m_fontFile.isEmpty()) CaptionRenderer::loadFontFile(m_fontFile);
    m_font->setCurrentFont(QFont(s.font));
    m_fontFileBtn->setToolTip(m_fontFile.isEmpty() ? tr("Use a downloaded .ttf / .otf font without installing it")
                                                   : tr("From %1").arg(m_fontFile));
    m_size->setValue(int(std::lround(s.size * 1000)));
    m_bold->setChecked(s.bold);
    m_italic->setChecked(s.italic);
    m_underline->setChecked(s.underline);
    m_upper->setChecked(s.uppercase);
    m_align->setCurrentIndex(std::max(0, m_align->findData(s.align)));
    m_letter->setValue(int(std::lround(s.letterSpacing * 100)));
    m_lineSpacing->setValue(int(std::lround(s.lineSpacing * 100)));
    m_maxWidth->setValue(int(std::lround(s.maxWidth * 100)));
    m_gradient->setChecked(s.gradient);
    m_outlineWidth->setValue(int(std::lround(s.outlineWidth * 100)));
    m_shadow->setChecked(s.shadow);
    m_shadowDistance->setValue(int(std::lround(std::hypot(s.shadowX, s.shadowY) * 100)));
    m_shadowBlur->setValue(int(std::lround(s.shadowBlur * 100)));
    m_background->setChecked(s.background);
    m_bgMode->setCurrentIndex(std::max(0, m_bgMode->findData(s.backgroundMode)));
    m_bgOpacity->setValue(int(std::lround(s.backgroundOpacity * 100)));
    m_bgPadding->setValue(int(std::lround(s.backgroundPadding * 100)));
    m_bgRadius->setValue(int(std::lround(s.backgroundRadius * 100)));
    m_animIn->setCurrentIndex(std::max(0, m_animIn->findData(d.animation.in)));
    m_animInDur->setValue(d.animation.inDuration);
    m_animOut->setCurrentIndex(std::max(0, m_animOut->findData(d.animation.out)));
    m_animOutDur->setValue(d.animation.outDuration);
    m_loop->setCurrentIndex(std::max(0, m_loop->findData(d.animation.loop)));
    m_loopSpeed->setValue(d.animation.loopSpeed);
    const QColor cols[5] = {s.color, s.color2, s.outline, s.shadowColor, s.backgroundColor};
    for (int i = 0; i < 5; ++i) {
        m_colors[i] = cols[i];
        setButtonColor(m_colorBtns[i], cols[i]);
    }
    m_loading = false;
}

void TextPanel::apply(const QString& key)
{
    if (m_loading) return;
    const Clip* c = m_project->findClip(m_clipId);
    if (!c || !c->isText()) return;
    Clip copy = *c;
    TextData& d = *copy.text;
    TextStyle& s = d.style;
    const QString words = m_text->toPlainText();
    if (!words.trimmed().isEmpty()) {
        d.text = words;
        copy.name = words.section(QChar(u'\n'), 0, 0).trimmed().left(60);
    }
    s.font = m_font->currentFont().family();
    s.fontFile = m_fontFile;
    s.size = m_size->value() / 1000.0;
    s.bold = m_bold->isChecked();
    s.italic = m_italic->isChecked();
    s.underline = m_underline->isChecked();
    s.uppercase = m_upper->isChecked();
    s.align = m_align->currentData().toString();
    s.letterSpacing = m_letter->value() / 100.0;
    s.lineSpacing = m_lineSpacing->value() / 100.0;
    s.maxWidth = m_maxWidth->value() / 100.0;
    s.gradient = m_gradient->isChecked();
    s.outlineWidth = m_outlineWidth->value() / 100.0;
    s.shadow = m_shadow->isChecked();
    // Keep the shadow's direction; the slider sets its distance.
    const double dist = m_shadowDistance->value() / 100.0, old = std::hypot(s.shadowX, s.shadowY);
    if (old > 1e-6) {
        s.shadowX *= dist / old;
        s.shadowY *= dist / old;
    } else {
        s.shadowX = dist * 0.6;
        s.shadowY = dist * 0.8;
    }
    s.shadowBlur = m_shadowBlur->value() / 100.0;
    s.background = m_background->isChecked();
    s.backgroundMode = m_bgMode->currentData().toString();
    s.backgroundOpacity = m_bgOpacity->value() / 100.0;
    s.backgroundPadding = m_bgPadding->value() / 100.0;
    s.backgroundRadius = m_bgRadius->value() / 100.0;
    s.color = m_colors[0];
    s.color2 = m_colors[1];
    s.outline = m_colors[2];
    s.shadowColor = m_colors[3];
    s.backgroundColor = m_colors[4];
    d.animation.in = m_animIn->currentData().toString();
    d.animation.inDuration = m_animInDur->value();
    d.animation.out = m_animOut->currentData().toString();
    d.animation.outDuration = m_animOutDur->value();
    d.animation.loop = m_loop->currentData().toString();
    d.animation.loopSpeed = m_loopSpeed->value();
    if (copy.text == c->text && copy.name == c->name) return;
    m_loading = true;   // the change signal reloads; keep the controls as they are
    m_project->edit(tr("Edit text"), [&] { m_project->updateClip(copy); }, m_clipId + QStringLiteral("/text/") + key);
    m_loading = false;
}

} // namespace ve
