#include "app/ClaudePanel.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace ve {

namespace {

struct ModelChoice {
    const char* id;
    const char* label;
};
constexpr ModelChoice kModels[] = {
    {"claude-opus-5-5", "Opus 5.5"},
    {"claude-sonnet-5-5", "Sonnet 5.5"},
    {"claude-haiku-5-5", "Haiku 5.5 (fast)"},
};

QString shortValue(const QJsonValue& v)
{
    QString s;
    if (v.isString()) {
        s = v.toString();
        // ids are long UUIDs: show the first block only
        if (s.size() == 36 && s.count(QLatin1Char('-')) == 4) s = s.left(8) + QStringLiteral("…");
        else if (s.contains(QLatin1Char('/')) || s.contains(QLatin1Char('\\'))) s = QFileInfo(s).fileName();
    } else if (v.isObject() || v.isArray()) {
        s = QString::fromUtf8(QJsonDocument(v.isObject() ? QJsonDocument(v.toObject()) : QJsonDocument(v.toArray()))
                                  .toJson(QJsonDocument::Compact));
    } else if (v.isBool()) {
        s = v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    } else {
        s = QString::number(v.toDouble());
    }
    if (s.size() > 60) s = s.left(57) + QStringLiteral("…");
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// discovery
// ---------------------------------------------------------------------------
QString ClaudePanel::findBridgeScript(QString* searched)
{
    QStringList dirs;
    const QString custom = QSettings().value(QStringLiteral("claude/bridgeDir")).toString();
    if (!custom.isEmpty()) dirs << custom;
    dirs << QCoreApplication::applicationDirPath() + QStringLiteral("/claude-bridge");
#ifdef VE_SOURCE_DIR
    dirs << QStringLiteral(VE_SOURCE_DIR "/claude-bridge");
#endif
    if (searched) *searched = dirs.join(QStringLiteral("\n"));
    for (const QString& d : dirs) {
        const QString script = QDir(d).filePath(QStringLiteral("bridge.mjs"));
        if (QFileInfo::exists(script) &&
            QFileInfo::exists(QDir(d).filePath(QStringLiteral("node_modules/@anthropic-ai/claude-agent-sdk/package.json"))))
            return script;
    }
    return {};
}

QString ClaudePanel::findNode()
{
    const QString custom = QSettings().value(QStringLiteral("claude/node")).toString();
    if (!custom.isEmpty() && QFileInfo::exists(custom)) return custom;
    QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
    if (!node.isEmpty()) return node;
#ifdef Q_OS_WIN
    for (const QString& p : {QStringLiteral("C:/Program Files/nodejs/node.exe"),
                             QStringLiteral("C:/Program Files (x86)/nodejs/node.exe")})
        if (QFileInfo::exists(p)) return p;
#else
    for (const QString& p : {QStringLiteral("/usr/local/bin/node"), QStringLiteral("/opt/homebrew/bin/node")})
        if (QFileInfo::exists(p)) return p;
#endif
    return {};
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
ClaudePanel::ClaudePanel(Host host, QWidget* parent)
    : QWidget(parent)
    , m_host(std::move(host))
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);

    auto* top = new QHBoxLayout;
    m_model = new QComboBox(this);
    const QString saved = QSettings().value(QStringLiteral("claude/model"), QString::fromLatin1(kModels[0].id)).toString();
    for (const auto& mc : kModels) {
        m_model->addItem(QString::fromLatin1(mc.label), QString::fromLatin1(mc.id));
        if (saved == QLatin1String(mc.id)) m_model->setCurrentIndex(m_model->count() - 1);
    }
    m_model->setToolTip(tr("Claude model"));
    m_newChat = new QPushButton(tr("New Chat"), this);
    m_newChat->setToolTip(tr("Start a new conversation (the project is not changed)"));
    top->addWidget(m_model, 1);
    top->addWidget(m_newChat);
    v->addLayout(top);

    m_log = new QTextBrowser(this);
    m_log->setAccessibleName(tr("Claude conversation"));
    m_log->setOpenExternalLinks(true);
    m_log->document()->setDocumentMargin(6);
    v->addWidget(m_log, 1);
    showWelcome();

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_status);

    auto* bottom = new QHBoxLayout;
    m_input = new QPlainTextEdit(this);
    m_input->setPlaceholderText(tr("Message Claude… (Enter to send, Shift+Enter for a new line)"));
    m_input->setFixedHeight(m_input->fontMetrics().lineSpacing() * 3 + 14);
    m_input->installEventFilter(this);
    m_input->setAccessibleName(tr("Message Claude"));
    m_sendBtn = new QPushButton(tr("Send"), this);
    m_sendBtn->setFixedHeight(m_input->height());
    m_sendBtn->setAccessibleName(tr("Send to Claude"));
    bottom->addWidget(m_input, 1);
    bottom->addWidget(m_sendBtn);
    v->addLayout(bottom);

    connect(m_sendBtn, &QPushButton::clicked, this, [this] { m_busy ? stop() : send(); });
    connect(m_newChat, &QPushButton::clicked, this, &ClaudePanel::newChat);
    connect(m_model, &QComboBox::currentIndexChanged, this, [this] {
        const QString id = m_model->currentData().toString();
        QSettings().setValue(QStringLiteral("claude/model"), id);
        if (m_proc && m_proc->state() == QProcess::Running) writeCommand({{"type", "model"}, {"model", id}});
    });

    QString searched;
    if (findNode().isEmpty())
        setStatus(tr("Node.js was not found. Install it from nodejs.org to use Claude here."), true);
    else if (findBridgeScript(&searched).isEmpty())
        setStatus(tr("Claude bridge not installed. Run 'npm install' in the claude-bridge folder. Looked in:\n%1")
                      .arg(searched), true);
    else
        setStatus(tr("Uses your Claude Code login. Claude can only use the editor's tools."));
}

ClaudePanel::~ClaudePanel()
{
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->closeWriteChannel();
        if (!m_proc->waitForFinished(1500)) m_proc->kill();
    }
}

void ClaudePanel::focusInput()
{
    m_input->setFocus();
}

bool ClaudePanel::eventFilter(QObject* o, QEvent* e)
{
    if (o == m_input && e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && !(k->modifiers() & Qt::ShiftModifier)) {
            if (!m_busy) send();
            return true;
        }
        if (k->key() == Qt::Key_Escape && m_busy) {
            stop();
            return true;
        }
    }
    return QWidget::eventFilter(o, e);
}

void ClaudePanel::setStatus(const QString& text, bool error)
{
    m_status->setText(text);
    m_status->setStyleSheet(error ? QStringLiteral("color: #e57373;") : QStringLiteral("color: #a8b0bb;"));
}

void ClaudePanel::setBusy(bool busy)
{
    m_busy = busy;
    m_sendBtn->setText(busy ? tr("Stop") : tr("Send"));
    m_sendBtn->setToolTip(busy ? tr("Stop Claude (Esc)") : tr("Send (Enter)"));
    m_newChat->setEnabled(!busy);
}

// ---------------------------------------------------------------------------
// bridge process
// ---------------------------------------------------------------------------
bool ClaudePanel::ensureBridge()
{
    if (m_proc && m_proc->state() != QProcess::NotRunning) return true;

    const QString node = findNode();
    QString searched;
    const QString script = findBridgeScript(&searched);
    if (node.isEmpty()) {
        appendNote(tr("Node.js was not found. Install Node.js 18 or newer (nodejs.org), then try again."), "#e57373");
        return false;
    }
    if (script.isEmpty()) {
        appendNote(tr("The Claude bridge is not installed. Run <code>npm install</code> in the claude-bridge "
                      "folder next to the editor. Looked in:<br>%1").arg(searched.toHtmlEscaped().replace("\n", "<br>")),
                   "#e57373");
        return false;
    }
    const QString url = m_host.mcpUrl ? m_host.mcpUrl() : QString();
    if (url.isEmpty()) {
        appendNote(tr("The editor's MCP server is not running (Tools > Enable MCP Server). Claude needs it to "
                      "control the editor."), "#e57373");
        return false;
    }

    if (!m_proc) {
        m_proc = new QProcess(this);
        connect(m_proc, &QProcess::readyReadStandardOutput, this, &ClaudePanel::readBridge);
        connect(m_proc, &QProcess::readyReadStandardError, this, [this] {
            for (const QByteArray& l : m_proc->readAllStandardError().split('\n')) {
                const QString s = QString::fromUtf8(l).trimmed();
                if (s.isEmpty()) continue;
                m_stderrTail << s;
                while (m_stderrTail.size() > 12) m_stderrTail.removeFirst();
            }
        });
        connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
            finishAssistant();
            if (m_busy || st == QProcess::CrashExit || code != 0) {
                QString msg = tr("Claude bridge stopped (exit code %1).").arg(code);
                if (!m_stderrTail.isEmpty()) msg += QStringLiteral("<br><code>%1</code>")
                                                       .arg(m_stderrTail.join("\n").toHtmlEscaped().replace("\n", "<br>"));
                appendNote(msg, "#e57373");
            }
            setBusy(false);
        });
        connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
            if (err == QProcess::FailedToStart) {
                appendNote(tr("Could not start Node.js: %1").arg(m_proc->errorString().toHtmlEscaped()), "#e57373");
                setBusy(false);
            }
        });
#ifdef Q_OS_WIN
        m_proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
            a->flags |= CREATE_NO_WINDOW;
        });
#endif
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("VE_MCP_URL"), url);
    env.insert(QStringLiteral("VE_CLAUDE_MODEL"), m_model->currentData().toString());
    const QString cwd = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/claude");
    QDir().mkpath(cwd);
    env.insert(QStringLiteral("VE_CLAUDE_CWD"), cwd);
    // Node must find claude.exe etc.: make sure the Node directory is on PATH.
    env.insert(QStringLiteral("PATH"), QFileInfo(node).absolutePath() + QDir::listSeparator() + env.value(QStringLiteral("PATH")));
    m_proc->setProcessEnvironment(env);
    m_proc->setWorkingDirectory(QFileInfo(script).absolutePath());
    m_stdoutBuf.clear();
    m_stderrTail.clear();
    m_proc->start(node, {script});
    setStatus(tr("Starting Claude…"));
    return true;
}

void ClaudePanel::writeCommand(const QJsonObject& cmd)
{
    if (!m_proc || m_proc->state() == QProcess::NotRunning) return;
    m_proc->write(QJsonDocument(cmd).toJson(QJsonDocument::Compact) + '\n');
}

void ClaudePanel::send()
{
    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty() || m_busy) return;
    if (!ensureBridge()) return;
    m_input->clear();
    appendUser(text);

    QString payload = text;
    if (m_host.editorState) {
        const QString state = m_host.editorState();
        if (!state.isEmpty()) payload = QStringLiteral("[Editor state: %1]\n\n%2").arg(state, text);
    }
    setBusy(true);
    setStatus(tr("Claude is working… (Esc to stop)"));
    writeCommand({{"type", "user"}, {"text", payload}});
}

void ClaudePanel::stop()
{
    writeCommand({{"type", "interrupt"}});
    setStatus(tr("Stopping…"));
}

void ClaudePanel::newChat()
{
    writeCommand({{"type", "reset"}});
    m_log->clear();
    showWelcome();
    m_toolMarks.clear();
    m_assistStart = -1;
    m_assistText.clear();
    setStatus(tr("New conversation. Claude doesn't remember earlier messages."));
}

void ClaudePanel::readBridge()
{
    m_stdoutBuf += m_proc->readAllStandardOutput();
    int nl;
    while ((nl = m_stdoutBuf.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuf.left(nl).trimmed();
        m_stdoutBuf.remove(0, nl + 1);
        if (line.isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) handle(doc.object());
    }
}

void ClaudePanel::handle(const QJsonObject& m)
{
    const QString type = m.value("type").toString();
    if (type == QLatin1String("hello")) {
        setStatus(m_busy ? tr("Claude is working… (Esc to stop)") : tr("Connected"));
    } else if (type == QLatin1String("ready")) {
        m_subscription = m.value("apiKeySource").toString() == QLatin1String("none");
        const QString editor = m.value("editor").toString();
        if (editor != QLatin1String("connected"))
            appendNote(tr("Claude could not connect to the editor's MCP server (status: %1).").arg(editor), "#e57373");
    } else if (type == QLatin1String("message_start")) {
        finishAssistant();
    } else if (type == QLatin1String("text_delta")) {
        m_assistText += m.value("text").toString();
        renderAssistant();
    } else if (type == QLatin1String("assistant_text")) {
        m_assistText = m.value("text").toString();
        renderAssistant();
        finishAssistant();
    } else if (type == QLatin1String("tool_use")) {
        finishAssistant();
        appendToolUse(m.value("id").toString(), m.value("name").toString(), m.value("input").toObject());
    } else if (type == QLatin1String("tool_result")) {
        appendToolResult(m);
    } else if (type == QLatin1String("result")) {
        finishAssistant();
        const QString sub = m.value("subtype").toString();
        if (m.value("isError").toBool() || sub != QLatin1String("success")) {
            QString why = m.value("text").toString();
            if (why.isEmpty()) why = sub;
            appendNote(tr("Stopped: %1").arg(why.toHtmlEscaped()), "#e57373");
        }
        const int turns = m.value("turns").toInt();
        QString info = (turns == 1 ? tr("1 step · %1 s") : tr("%1 steps · %2 s").arg(turns))
                           .arg(m.value("durationMs").toDouble() / 1000.0, 0, 'f', 1);
        if (!m_subscription) info += QStringLiteral(" · $%1").arg(m.value("costUsd").toDouble(), 0, 'f', 4);
        setStatus(info);
        setBusy(false);
    } else if (type == QLatin1String("error")) {
        finishAssistant();
        QString msg = m.value("message").toString();
        if (msg.contains(QLatin1String("login"), Qt::CaseInsensitive) || msg.contains(QLatin1String("API key"), Qt::CaseInsensitive))
            msg += tr("\nSign in by running 'claude' once in a terminal, or set ANTHROPIC_API_KEY.");
        appendNote(msg.toHtmlEscaped().replace("\n", "<br>"), "#e57373");
        setStatus(tr("Error"), true);
        setBusy(false);
    } else if (type == QLatin1String("status")) {
        if (!m_busy) setStatus(m.value("text").toString());
    } else if (type == QLatin1String("reset")) {
        setBusy(false);
    }
}

// ---------------------------------------------------------------------------
// transcript rendering
// ---------------------------------------------------------------------------
QTextCursor ClaudePanel::endCursor() const
{
    QTextCursor c(m_log->document());
    c.movePosition(QTextCursor::End);
    return c;
}

void ClaudePanel::scrollToEnd()
{
    m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
}

void ClaudePanel::showWelcome()
{
    m_log->setHtml(tr("<div style='color:#9aa3ad'><p><b>Ask Claude to edit your video.</b> Claude works on the open "
                      "project with the editor's own tools, so you see every change live and can undo it.</p>"
                      "<p>For example:</p><ul>"
                      "<li>Split the selected clip at the playhead</li>"
                      "<li>Make the second clip black and white with a 1 second fade in</li>"
                      "<li>Balance the audio so nothing clips</li>"
                      "<li>Export a 720p MP4 to my Desktop</li></ul></div>"));
    m_welcome = true;
}

void ClaudePanel::appendUser(const QString& text)
{
    if (m_welcome) {
        m_log->clear();
        m_welcome = false;
    }
    finishAssistant();
    QTextCursor c = endCursor();
    if (!m_log->document()->isEmpty()) c.insertBlock();
    QTextBlockFormat bf;
    bf.setBackground(QColor(55, 75, 105));
    bf.setTopMargin(8);
    bf.setBottomMargin(4);
    bf.setLeftMargin(30);
    QTextCharFormat cf;
    cf.setForeground(QColor(235, 240, 248));
    c.setBlockFormat(bf);
    c.setCharFormat(cf);
    c.insertText(text);
    c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    scrollToEnd();
}

void ClaudePanel::appendNote(const QString& html, const QString& color)
{
    finishAssistant();
    QTextCursor c = endCursor();
    if (!m_log->document()->isEmpty()) c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    c.insertHtml(QStringLiteral("<span style='color:%1'>%2</span>").arg(color, html));
    c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    scrollToEnd();
}

void ClaudePanel::appendToolUse(const QString& id, const QString& name, const QJsonObject& input)
{
    QStringList args;
    for (auto it = input.begin(); it != input.end(); ++it)
        args << QStringLiteral("%1=%2").arg(it.key(), shortValue(it.value()));
    QTextCursor c = endCursor();
    if (!c.block().text().isEmpty()) c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    QTextCharFormat f;
    f.setForeground(QColor(150, 160, 175));
    f.setFontPointSize(m_log->font().pointSizeF() * 0.9);
    c.insertText(QStringLiteral("▸ "), f);
    QTextCharFormat nameF = f;
    nameF.setFontWeight(QFont::DemiBold);
    nameF.setForeground(QColor(130, 180, 230));
    c.insertText(name, nameF);
    if (!args.isEmpty()) c.insertText(QStringLiteral("  ") + args.join(QStringLiteral(", ")), f);
    m_toolMarks.insert(id, c.blockNumber());  // the result mark goes at the end of this line
    c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    scrollToEnd();
}

void ClaudePanel::appendToolResult(const QJsonObject& m)
{
    const QString id = m.value("id").toString();
    const bool err = m.value("isError").toBool();
    auto it = m_toolMarks.find(id);
    if (it != m_toolMarks.end()) {
        const QTextBlock block = m_log->document()->findBlockByNumber(it.value());
        QTextCursor c(m_log->document());
        c.setPosition(block.position() + block.length() - 1);
        QTextCharFormat f;
        f.setForeground(err ? QColor(229, 115, 115) : QColor(110, 200, 120));
        c.insertText(err ? QStringLiteral("  ✗") : QStringLiteral("  ✓"), f);
        if (err) {
            QString text = m.value("text").toString().simplified();
            if (text.size() > 160) text = text.left(157) + QStringLiteral("…");
            c.insertText(QStringLiteral(" ") + text, f);
        }
        m_toolMarks.erase(it);
    }
    // Show what Claude saw (render_frame returns the composited frame).
    for (const QJsonValue& v : m.value("images").toArray()) {
        const QByteArray data = QByteArray::fromBase64(v.toObject().value("data").toString().toLatin1());
        QImage img;
        if (!img.loadFromData(data)) continue;
        const QUrl url(QStringLiteral("claude-image://%1").arg(++m_imageCount));
        m_log->document()->addResource(QTextDocument::ImageResource, url, img);
        QTextCursor c = endCursor();
        if (!c.block().text().isEmpty()) c.insertBlock(QTextBlockFormat(), QTextCharFormat());
        QTextImageFormat imf;
        imf.setName(url.toString());
        const double w = std::min(260.0, double(img.width()));
        imf.setWidth(w);
        imf.setHeight(w * img.height() / std::max(1, img.width()));
        imf.setToolTip(tr("Frame Claude looked at"));
        c.insertImage(imf);
        c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    }
    scrollToEnd();
}

void ClaudePanel::renderAssistant()
{
    QTextCursor c = endCursor();
    if (m_assistStart < 0) {
        if (!c.block().text().isEmpty()) c.insertBlock(QTextBlockFormat(), QTextCharFormat());
        m_assistStart = c.position();
    } else {
        c.setPosition(m_assistStart);
        c.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        c.removeSelectedText();
    }
    c.setPosition(m_assistStart);
    c.insertMarkdown(m_assistText);
    scrollToEnd();
}

void ClaudePanel::finishAssistant()
{
    if (m_assistStart >= 0) {
        QTextCursor c = endCursor();
        c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    }
    m_assistStart = -1;
    m_assistText.clear();
}

} // namespace ve
