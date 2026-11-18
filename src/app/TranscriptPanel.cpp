#include "app/TranscriptPanel.h"

#include "media/StemSeparator.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace ve {

namespace {
enum Column { ColStart, ColEnd, ColText, ColTranslation };

QString clock(TimeUs t)
{
    const qint64 cs = std::max<TimeUs>(0, t) / 10000;
    return QStringLiteral("%1:%2.%3").arg(cs / 6000).arg(cs / 100 % 60, 2, 10, QLatin1Char('0')).arg(cs % 100, 2, 10, QLatin1Char('0'));
}

// "1:02.50", "0:01:02.5", "62.5" -> microseconds (-1 if not a time)
TimeUs parseClock(const QString& s)
{
    static const QRegularExpression re(QStringLiteral("^\\s*(?:(\\d+):)?(?:(\\d+):)?(\\d+(?:[.,]\\d+)?)\\s*$"));
    const auto m = re.match(s);
    if (!m.hasMatch()) return -1;
    QStringList parts;
    for (int i = 1; i <= 3; ++i)
        if (!m.captured(i).isEmpty()) parts << m.captured(i);
    double sec = 0;
    for (QString p : parts) sec = sec * 60 + p.replace(QLatin1Char(','), QLatin1Char('.')).toDouble();
    return secondsToUs(sec);
}
} // namespace

TranscriptPanel::TranscriptPanel(Project* project, Host host, QWidget* parent)
    : QWidget(parent)
    , m_project(project)
    , m_host(std::move(host))
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);

    auto* top = new QHBoxLayout;
    m_transcribe = new QPushButton(tr("Transcribe Selected Clip"), this);
    m_transcribe->setToolTip(tr("Turn the selected clip's speech or song lyrics into text (Whisper, on your GPU)"));
    m_language = new QComboBox(this);
    m_language->addItem(tr("Detect language"), QStringLiteral("auto"));
    for (const auto& [code, name] : subtitleLanguages()) m_language->addItem(name, code);
    m_hints = new QLineEdit(this);
    m_hints->setPlaceholderText(tr("Hints: names, slang"));
    m_hints->setToolTip(tr("Words Whisper should expect, e.g. names, slang or song titles (helps lyrics and dialects)"));
    m_hints->setMaximumWidth(170);
    m_isolate = new QCheckBox(tr("Voice only"), this);
    m_isolate->setChecked(true);
    m_isolate->setToolTip(tr("Separate the voice from the music first: much better for song lyrics"));
    m_export = new QToolButton(this);
    m_export->setText(tr("Export"));
    m_export->setPopupMode(QToolButton::InstantPopup);
    auto* exportMenu = new QMenu(m_export);
    for (const auto& [fmt, label] : QList<QPair<QString, QString>>{{QStringLiteral("srt"), tr("Subtitles (.srt)")},
                                                                   {QStringLiteral("vtt"), tr("Web subtitles (.vtt)")},
                                                                   {QStringLiteral("lrc"), tr("Synced lyrics (.lrc)")},
                                                                   {QStringLiteral("txt"), tr("Plain text (.txt)")}})
        exportMenu->addAction(label, this, [this, fmt] { if (m_host.exportAs) m_host.exportAs(fmt); });
    m_export->setMenu(exportMenu);
    top->addWidget(m_transcribe);
    top->addWidget(m_language);
    top->addWidget(m_hints);
    top->addWidget(m_isolate);
    top->addStretch(1);
    top->addWidget(m_export);
    v->addLayout(top);

    // Subtitle tools: import, timing, translation, voice-over
    auto* tools = new QHBoxLayout;
    auto* importBtn = new QPushButton(tr("Import Subtitles..."), this);
    importBtn->setToolTip(tr("Read an .srt / .vtt / .lrc file into the selected clip's transcript"));
    auto* shiftBtn = new QPushButton(tr("Shift Timing..."), this);
    shiftBtn->setToolTip(tr("Move every line earlier or later (fix subtitles that are out of sync)"));
    auto* translateBtn = new QPushButton(tr("Translate..."), this);
    translateBtn->setToolTip(tr("Translate the subtitles on this computer (NLLB-200)"));
    auto* voiceBtn = new QPushButton(tr("Voice-over..."), this);
    voiceBtn->setToolTip(tr("Speak every line with a voice (text to speech), e.g. a dub of a translation"));
    m_showLang = new QComboBox(this);
    m_showLang->setToolTip(tr("Translation shown next to each line (double-click it to edit)"));
    for (QPushButton* b : {importBtn, shiftBtn, translateBtn, voiceBtn}) tools->addWidget(b);
    tools->addStretch(1);
    tools->addWidget(new QLabel(tr("Translation:"), this));
    tools->addWidget(m_showLang);
    v->addLayout(tools);
    connect(importBtn, &QPushButton::clicked, this, [this] { if (m_host.importSubtitles) m_host.importSubtitles(); });
    connect(shiftBtn, &QPushButton::clicked, this, [this] { shiftDialog(-1); });
    connect(translateBtn, &QPushButton::clicked, this, [this] { if (m_host.translate) m_host.translate(); });
    connect(voiceBtn, &QPushButton::clicked, this, [this] { if (m_host.voiceOver) m_host.voiceOver(); });
    connect(m_showLang, &QComboBox::activated, this, [this] {
        m_showLang->setProperty("chosen", m_showLang->currentData());
        rebuild();
    });

    // Karaoke view of the line being heard
    m_lyric = new QLabel(this);
    m_lyric->setWordWrap(true);
    m_lyric->setAlignment(Qt::AlignCenter);
    m_lyric->setMinimumHeight(70);
    m_lyric->setTextFormat(Qt::RichText);
    m_lyric->setStyleSheet(QStringLiteral("QLabel { background: #08080a; border: 1px solid #25252a; border-radius: 6px; padding: 8px; }"));
    v->addWidget(m_lyric);

    m_table = new QTableWidget(0, 4, this);
    m_table->setHorizontalHeaderLabels({tr("Start"), tr("End"), tr("Text (double-click to edit)"), tr("Translation")});
    m_table->horizontalHeader()->setSectionResizeMode(ColStart, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColEnd, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColText, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColTranslation, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setWordWrap(true);
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                             QAbstractItemView::AnyKeyPressed);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    v->addWidget(m_table, 1);

    auto* bottom = new QHBoxLayout;
    m_follow = new QCheckBox(tr("Follow playback"), this);
    m_follow->setChecked(true);
    m_captions = new QCheckBox(tr("Captions on video"), this);
    m_captions->setToolTip(tr("Show the transcript as CapCut-style text over the video (style it in the Captions panel)"));
    m_captions->setChecked(m_project->captionStyle().enabled);
    connect(m_captions, &QCheckBox::toggled, this, [this](bool on) {
        CaptionStyle s = m_project->captionStyle();
        s.enabled = on;
        m_project->setCaptionStyle(s);
    });
    connect(m_project, &Project::captionsChanged, this, [this] {
        const QSignalBlocker b(m_captions);
        m_captions->setChecked(m_project->captionStyle().enabled);
    });
    m_hint = new QLabel(this);
    m_hint->setStyleSheet(QStringLiteral("color: #9a9aa5;"));
    bottom->addWidget(m_follow);
    bottom->addWidget(m_captions);
    bottom->addStretch(1);
    bottom->addWidget(m_hint);
    v->addLayout(bottom);

    connect(m_transcribe, &QPushButton::clicked, this, [this] {
        if (!m_host.transcribe) return;
        const QString clip = m_host.selectedClip ? m_host.selectedClip() : QString();
        m_host.transcribe(clip, m_language->currentData().toString(), m_isolate->isChecked(), m_hints->text().trimmed());
    });
    // Click a time to jump there; edit times, text and translation in place.
    connect(m_table, &QTableWidget::cellClicked, this, [this](int row, int col) {
        if (row < 0 || row >= int(m_lines.size()) || !m_host.seek) return;
        if (col == ColStart) m_host.seek(m_lines[size_t(row)].start);
        else if (col == ColEnd) m_host.seek(std::max<TimeUs>(0, m_lines[size_t(row)].end - 1));
    });
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (m_rebuilding) return;
        const int row = item->row();
        if (row < 0 || row >= int(m_lines.size())) return;
        const TimelineLine l = m_lines[size_t(row)];
        const QString value = item->text().trimmed();
        switch (item->column()) {
        case ColStart:
        case ColEnd: {
            const TimeUs t = parseClock(value);
            if (t < 0) {
                QTimer::singleShot(0, this, &TranscriptPanel::rebuild);   // not a time: put it back
                return;
            }
            if (item->column() == ColStart) setLineTimes(row, t, -1);
            else setLineTimes(row, -1, t);
            break;
        }
        case ColText:
            if (value == l.original) return;
            if (value.isEmpty()) m_project->removeTranscriptSegment(l.mediaId, l.segment);
            else m_project->updateTranscriptSegment(l.mediaId, l.segment, value);
            break;
        case ColTranslation:
            if (!translationLanguage().isEmpty())
                m_project->setSegmentTranslation(l.mediaId, l.segment, translationLanguage(), value);
            break;
        }
    });
    connect(m_table, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const int row = m_table->rowAt(pos.y());
        QMenu menu(this);
        if (row >= 0 && row < int(m_lines.size())) {
            const TimelineLine l = m_lines[size_t(row)];
            menu.addAction(tr("Go to Line"), this, [this, l] { if (m_host.seek) m_host.seek(l.start); });
            menu.addAction(tr("Edit Text"), this, [this, row] { m_table->editItem(m_table->item(row, ColText)); });
            menu.addSeparator();
            menu.addAction(tr("Set Start to Playhead"), this, [this, row] { setLineTimes(row, m_playhead, -1); });
            menu.addAction(tr("Set End to Playhead"), this, [this, row] { setLineTimes(row, -1, m_playhead); });
            menu.addAction(tr("Nudge Earlier (-0.1 s)"), this, [this, l] {
                m_project->shiftTranscript(l.mediaId, -100'000, l.segment, l.segment);
            });
            menu.addAction(tr("Nudge Later (+0.1 s)"), this, [this, l] {
                m_project->shiftTranscript(l.mediaId, 100'000, l.segment, l.segment);
            });
            menu.addAction(tr("Shift This and Later Lines..."), this, [this, row] { shiftDialog(row); });
            menu.addSeparator();
            QAction* split = menu.addAction(tr("Split at Playhead"), this, [this, l] {
                const Clip* c = m_project->findClip(l.clipId);
                if (c && c->contains(m_playhead))
                    m_project->splitTranscriptSegment(l.mediaId, l.segment, c->sourceTimeAt(m_playhead));
            });
            split->setEnabled(m_playhead > l.start && m_playhead < l.end);
            menu.addAction(tr("Merge with Next Line"), this, [this, l] { m_project->mergeTranscriptSegments(l.mediaId, l.segment); });
            menu.addAction(tr("Delete Line"), this, [this, l] { m_project->removeTranscriptSegment(l.mediaId, l.segment); });
            menu.addSeparator();
            menu.addAction(tr("Add Line at Playhead"), this, [this, l] {
                // Same media as this line, at the source time playing at the playhead.
                const Clip* c = m_project->findClip(l.clipId);
                if (!c || !c->contains(m_playhead)) return;
                const TimeUs src = c->sourceTimeAt(m_playhead);
                m_project->insertTranscriptSegment(l.mediaId, src, src + 2 * kUsPerSecond, tr("New line"));
            });
        }
        if (!menu.isEmpty()) menu.exec(m_table->viewport()->mapToGlobal(pos));
    });

    connect(m_project, &Project::transcriptsChanged, this, &TranscriptPanel::rebuild);
    connect(m_project, &Project::changed, this, &TranscriptPanel::rebuild); // clips moved / trimmed / sped up
    connect(m_project, &Project::captionsChanged, this, [this] {
        // Follow the captions language in the Translation column.
        const QString lang = m_project->captionStyle().language;
        if (!lang.isEmpty() && lang != translationLanguage()) {
            m_showLang->setProperty("chosen", lang);
            rebuild();
        }
    });
    rebuild();
}

QString TranscriptPanel::translationLanguage() const { return m_showLang->currentData().toString(); }

void TranscriptPanel::setLineTimes(int row, TimeUs start, TimeUs end)
{
    if (row < 0 || row >= int(m_lines.size())) return;
    const TimelineLine l = m_lines[size_t(row)];
    const Clip* c = m_project->findClip(l.clipId);
    if (!c) return;
    TimeUs a = start >= 0 ? start : l.start, b = end >= 0 ? end : l.end;
    if (b <= a) b = a + 200'000;
    a = std::clamp(a, c->start, c->end() - 1);
    b = std::clamp(b, a + 1, c->end());
    TimeUs sa = c->sourceTimeAt(a), sb = c->sourceTimeAt(b - 1) + 1;
    if (sb < sa) std::swap(sa, sb);            // reversed clip
    const Transcript* t = m_project->transcript(l.mediaId);
    if (!t || l.segment >= int(t->segments.size())) return;
    m_project->updateTranscriptSegment(l.mediaId, l.segment, t->segments[size_t(l.segment)].text, sa, sb);
}

void TranscriptPanel::shiftDialog(int row)
{
    bool ok = false;
    const QString what = row < 0 ? tr("every line") : tr("this line and the ones after it");
    const double s = QInputDialog::getDouble(this, tr("Shift Subtitle Timing"),
                                             tr("Move %1 by (seconds, negative = earlier):").arg(what), 0.0, -600.0, 600.0, 2, &ok, {},
                                             0.1);
    if (!ok || s == 0.0) return;
    if (row < 0) {
        for (const QString& m : m_project->transcribedMedia()) m_project->shiftTranscript(m, secondsToUs(s));
    } else if (row < int(m_lines.size())) {
        const TimelineLine& l = m_lines[size_t(row)];
        m_project->shiftTranscript(l.mediaId, secondsToUs(s), l.segment, -1);
    }
}

void TranscriptPanel::rebuild()
{
    if (isEditing()) { // don't yank the editor away; retry shortly
        if (!m_pendingRebuild) {
            m_pendingRebuild = true;
            QTimer::singleShot(400, this, [this] { m_pendingRebuild = false; rebuild(); });
        }
        return;
    }
    m_rebuilding = true;
    // Translation column: the captions language, else what was picked, else the first translation.
    const QStringList langs = m_project->translationLanguages();
    const QVariant chosen = m_showLang->property("chosen");
    QString lang = chosen.toString();
    const QString capLang = m_project->captionStyle().language;
    if (!chosen.isValid() || (!lang.isEmpty() && !langs.contains(lang)))
        lang = langs.contains(capLang) ? capLang : langs.isEmpty() ? QString() : langs.first();
    {
        const QSignalBlocker b(m_showLang);
        m_showLang->clear();
        m_showLang->addItem(tr("None"), QString());
        for (const QString& l : langs) m_showLang->addItem(languageName(l), l);
        m_showLang->setCurrentIndex(std::max(0, m_showLang->findData(lang)));
        m_showLang->setEnabled(!langs.isEmpty());
    }
    const QString showLang = m_showLang->currentData().toString();
    m_table->setColumnHidden(ColTranslation, showLang.isEmpty());

    m_lines = m_project->transcriptLines();
    std::vector<TimelineLine> translated;
    if (!showLang.isEmpty()) translated = m_project->transcriptLines(showLang);
    m_table->setRowCount(int(m_lines.size()));
    for (int i = 0; i < int(m_lines.size()); ++i) {
        const auto& l = m_lines[size_t(i)];
        auto* start = new QTableWidgetItem(clock(l.start));
        auto* end = new QTableWidgetItem(clock(l.end));
        for (QTableWidgetItem* it : {start, end}) {
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
            it->setForeground(QColor(140, 180, 230));
            it->setToolTip(tr("Click to jump there; double-click to type a new time (m:ss.cc)"));
        }
        auto* text = new QTableWidgetItem(l.text);
        text->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
        if (l.text.isRightToLeft()) text->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter); // Arabic
        QString trText;
        if (i < int(translated.size()) && translated[size_t(i)].segment == l.segment && translated[size_t(i)].mediaId == l.mediaId &&
            translated[size_t(i)].text != l.original)
            trText = translated[size_t(i)].text;
        auto* trItem = new QTableWidgetItem(trText);
        trItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
        if (trText.isRightToLeft()) trItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        trItem->setForeground(QColor(200, 200, 150));
        m_table->setItem(i, ColStart, start);
        m_table->setItem(i, ColEnd, end);
        m_table->setItem(i, ColText, text);
        m_table->setItem(i, ColTranslation, trItem);
    }
    m_table->resizeRowsToContents();
    m_rebuilding = false;
    m_current = -2;
    m_hint->setText(m_lines.empty() ? QString() : tr("%n line(s)", nullptr, int(m_lines.size())));
    m_export->setEnabled(!m_lines.empty());
    m_isolate->setEnabled(StemSeparator::isInstalled());
    updateCurrent();
}

bool TranscriptPanel::isEditing() const
{
    // A cell editor is a child of the table's viewport and has the focus while open.
    const QWidget* f = QApplication::focusWidget();
    return f && m_table->viewport()->isAncestorOf(f);
}

int TranscriptPanel::lineAt(TimeUs t) const
{
    int best = -1;
    for (int i = 0; i < int(m_lines.size()); ++i) {
        if (m_lines[size_t(i)].start <= t) best = i;
        else break;
    }
    return best;
}

void TranscriptPanel::setPlayhead(TimeUs t)
{
    m_playhead = t;
    updateCurrent();
}

void TranscriptPanel::updateCurrent()
{
    if (m_lines.empty()) {
        m_lyric->setText(tr("<div style='color:#8a8a95; font-size:12pt'>No transcript yet.<br>Select a clip with "
                            "speech or singing and click <b>Transcribe Selected Clip</b>, or <b>Import Subtitles</b>.</div>"));
        m_current = -1;
        return;
    }
    const int cur = lineAt(m_playhead);
    const bool inside = cur >= 0 && m_playhead < m_lines[size_t(cur)].end;

    // Karaoke: sung words white, the current word gold, the rest grey.
    QString html;
    if (cur >= 0) {
        const auto& l = m_lines[size_t(cur)];
        QString body;
        if (!l.words.empty() && inside) {
            for (const auto& w : l.words) {
                const QString color = m_playhead >= w.end ? QStringLiteral("#ffffff")
                                      : m_playhead >= w.start ? QStringLiteral("#ffc83d") : QStringLiteral("#8a8a95");
                body += QStringLiteral("<span style='color:%1'>%2</span> ").arg(color, w.text.toHtmlEscaped());
            }
        } else {
            body = QStringLiteral("<span style='color:%1'>%2</span>")
                       .arg(inside ? QStringLiteral("#ffffff") : QStringLiteral("#8a8a95"), l.text.toHtmlEscaped());
        }
        const QString dir = l.text.isRightToLeft() ? QStringLiteral("rtl") : QStringLiteral("ltr");
        html = QStringLiteral("<div dir='%1' style='font-size:15pt; font-weight:600'>%2</div>").arg(dir, body);
        if (auto* cell = m_table->item(cur, ColTranslation); cell && !cell->text().isEmpty() && !m_table->isColumnHidden(ColTranslation))
            html += QStringLiteral("<div dir='%1' style='font-size:11pt; color:#d8d8a0; margin-top:3px'>%2</div>")
                        .arg(cell->text().isRightToLeft() ? QStringLiteral("rtl") : QStringLiteral("ltr"), cell->text().toHtmlEscaped());
    }
    if (cur + 1 < int(m_lines.size()))
        html += QStringLiteral("<div dir='%1' style='font-size:10pt; color:#6f6f7a; margin-top:6px'>%2</div>")
                    .arg(m_lines[size_t(cur + 1)].text.isRightToLeft() ? QStringLiteral("rtl") : QStringLiteral("ltr"),
                         m_lines[size_t(cur + 1)].text.toHtmlEscaped());
    m_lyric->setText(html);

    if (cur != m_current) {
        m_current = cur;
        m_rebuilding = true;
        for (int i = 0; i < m_table->rowCount(); ++i) {
            QFont f = m_table->font();
            f.setBold(i == cur);
            for (int c = 0; c < m_table->columnCount(); ++c)
                if (auto* it = m_table->item(i, c)) {
                    it->setFont(f);
                    it->setBackground(i == cur ? QColor(27, 43, 71) : QBrush());
                }
        }
        m_rebuilding = false;
        if (cur >= 0 && m_follow->isChecked() && !isEditing())
            m_table->scrollToItem(m_table->item(cur, ColStart), QAbstractItemView::PositionAtCenter);
    }
}

} // namespace ve
