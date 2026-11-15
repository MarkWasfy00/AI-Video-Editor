#include "app/TranscriptPanel.h"

#include "media/StemSeparator.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace ve {

namespace {
QString clock(TimeUs t)
{
    const qint64 cs = std::max<TimeUs>(0, t) / 10000;
    return QStringLiteral("%1:%2.%3").arg(cs / 6000).arg(cs / 100 % 60, 2, 10, QLatin1Char('0')).arg(cs % 100, 2, 10, QLatin1Char('0'));
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
    const QList<QPair<QString, QString>> langs = {
        {QStringLiteral("auto"), tr("Detect language")}, {QStringLiteral("en"), tr("English")},
        {QStringLiteral("ar-EG"), tr("Arabic (Egyptian)")}, {QStringLiteral("ar"), tr("Arabic (Standard)")},
        {QStringLiteral("es"), tr("Spanish")}, {QStringLiteral("fr"), tr("French")},
        {QStringLiteral("de"), tr("German")}, {QStringLiteral("it"), tr("Italian")}, {QStringLiteral("pt"), tr("Portuguese")},
        {QStringLiteral("ru"), tr("Russian")}, {QStringLiteral("tr"), tr("Turkish")}, {QStringLiteral("hi"), tr("Hindi")},
        {QStringLiteral("ja"), tr("Japanese")}, {QStringLiteral("ko"), tr("Korean")}, {QStringLiteral("zh"), tr("Chinese")}};
    for (const auto& [code, name] : langs) m_language->addItem(name, code);
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

    // Karaoke view of the line being heard
    m_lyric = new QLabel(this);
    m_lyric->setWordWrap(true);
    m_lyric->setAlignment(Qt::AlignCenter);
    m_lyric->setMinimumHeight(70);
    m_lyric->setTextFormat(Qt::RichText);
    m_lyric->setStyleSheet(QStringLiteral("QLabel { background: #17171b; border-radius: 6px; padding: 8px; }"));
    v->addWidget(m_lyric);

    m_table = new QTableWidget(0, 2, this);
    m_table->setHorizontalHeaderLabels({tr("Time"), tr("Text (double-click to edit)")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
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
    // Click a time to jump there; edit the text in place.
    connect(m_table, &QTableWidget::cellClicked, this, [this](int row, int col) {
        if (col == 0 && row >= 0 && row < int(m_lines.size()) && m_host.seek) m_host.seek(m_lines[size_t(row)].start);
    });
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (m_rebuilding || item->column() != 1) return;
        const int row = item->row();
        if (row < 0 || row >= int(m_lines.size())) return;
        const TimelineLine l = m_lines[size_t(row)];
        const QString text = item->text().trimmed();
        if (text == l.text) return;
        if (text.isEmpty()) m_project->removeTranscriptSegment(l.mediaId, l.segment);
        else m_project->updateTranscriptSegment(l.mediaId, l.segment, text);
    });
    connect(m_table, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const int row = m_table->rowAt(pos.y());
        QMenu menu(this);
        if (row >= 0 && row < int(m_lines.size())) {
            const TimelineLine l = m_lines[size_t(row)];
            menu.addAction(tr("Go to Line"), this, [this, l] { if (m_host.seek) m_host.seek(l.start); });
            menu.addAction(tr("Edit Text"), this, [this, row] { m_table->editItem(m_table->item(row, 1)); });
            menu.addAction(tr("Delete Line"), this, [this, l] { m_project->removeTranscriptSegment(l.mediaId, l.segment); });
            menu.addSeparator();
            menu.addAction(tr("Add Line at Playhead"), this, [this, l] {
                // Same media as this line, at the source time playing at the playhead.
                const Clip* c = m_project->findClip(l.clipId);
                if (!c || !c->contains(m_playhead)) return;
                const TimeUs src = c->sourceTimeAt(m_playhead);
                const int idx = m_project->insertTranscriptSegment(l.mediaId, src, src + 2 * kUsPerSecond, tr("New line"));
                Q_UNUSED(idx);
            });
        }
        if (!menu.isEmpty()) menu.exec(m_table->viewport()->mapToGlobal(pos));
    });

    connect(m_project, &Project::transcriptsChanged, this, &TranscriptPanel::rebuild);
    connect(m_project, &Project::changed, this, &TranscriptPanel::rebuild); // clips moved / trimmed / sped up
    rebuild();
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
    m_lines = m_project->transcriptLines();
    m_table->setRowCount(int(m_lines.size()));
    for (int i = 0; i < int(m_lines.size()); ++i) {
        const auto& l = m_lines[size_t(i)];
        auto* time = new QTableWidgetItem(clock(l.start));
        time->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        time->setToolTip(tr("%1 - %2 (click to jump)").arg(clock(l.start), clock(l.end)));
        time->setForeground(QColor(140, 180, 230));
        auto* text = new QTableWidgetItem(l.text);
        text->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
        if (l.text.isRightToLeft()) text->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter); // Arabic
        m_table->setItem(i, 0, time);
        m_table->setItem(i, 1, text);
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
                            "speech or singing and click <b>Transcribe Selected Clip</b>.</div>"));
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
            for (int c = 0; c < 2; ++c)
                if (auto* it = m_table->item(i, c)) {
                    it->setFont(f);
                    it->setBackground(i == cur ? QColor(60, 75, 100) : QBrush());
                }
        }
        m_rebuilding = false;
        if (cur >= 0 && m_follow->isChecked() && !isEditing())
            m_table->scrollToItem(m_table->item(cur, 0), QAbstractItemView::PositionAtCenter);
    }
}

} // namespace ve
