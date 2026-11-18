#include "app/ClaudePanel.h"

#include "app/Theme.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
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
#include <QTextFrame>
#include <QUrl>
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

const char kError[] = "#e5786b";

// The Claude app sets replies in a serif face; use the closest one installed.
QFont serifFont()
{
    QFont f;
    f.setFamilies({QStringLiteral("Anthropic Serif"), QStringLiteral("Tiempos Text"), QStringLiteral("Source Serif 4"),
                   QStringLiteral("Georgia"), QStringLiteral("Cambria")});
    f.setStyleHint(QFont::Serif);
    return f;
}

// The interface face at a size relative to the conversation's serif face.
QFont sansFont(const QFont& base, qreal scale)
{
    QFont f = QGuiApplication::font();
    f.setPointSizeF(base.pointSizeF() * scale);
    return f;
}

QString panelStyleSheet()
{
    QString qss = QString::fromLatin1(R"(
#ClaudePanel { background: @bg; }
#ClaudePanel QLabel { color: @text; background: transparent; }
#claudeLog { background: transparent; border: none; padding: 0; color: @text;
             selection-background-color: @coral; selection-color: white; }
#claudeComposer { background: @raised; border: 1px solid @line; border-radius: 14px; }
#claudeInput { background: transparent; border: none; padding: 0; color: @text;
               selection-background-color: @coral; selection-color: white; }
#claudeModel { background: transparent; border: none; border-radius: 6px; color: @dim; padding: 4px 22px 4px 6px; }
#claudeModel:hover { background: @bg; color: @text; }
#claudeModel::drop-down { border: none; width: 18px; }
#claudeModel::down-arrow { image: url("@dir/claude-down.png"); width: 9px; height: 9px; }
#claudeModel QAbstractItemView { background: @raised; color: @text; border: 1px solid @line; padding: 4px;
                                 selection-background-color: @line; selection-color: @text; }
#claudeSend { background: @coral; border: none; border-radius: 8px; padding: 0; }
#claudeSend:hover { background: #e58a6c; }
#claudeSend:pressed { background: #c4633f; }
#claudeGhost { background: transparent; border: 1px solid transparent; border-radius: 7px; color: @dim; padding: 4px 9px; }
#claudeGhost:hover { background: @raised; border-color: @line; color: @text; }
#claudeGhost:disabled { color: #5e5d59; background: transparent; border-color: transparent; }
#claudeStatus { font-size: 8pt; }
#ClaudePanel QScrollBar:vertical { background: transparent; width: 8px; }
#ClaudePanel QScrollBar::handle:vertical { background: @line; border-radius: 3px; margin: 1px; }
#ClaudePanel QScrollBar::handle:vertical:hover { background: #55534e; }
)");
    const std::pair<const char*, QString> tokens[] = {
        {"@dir", theme::assetDir()},          {"@bg", theme::kClaudeBg.name()},     {"@raised", theme::kClaudeRaised.name()},
        {"@line", theme::kClaudeLine.name()}, {"@text", theme::kClaudeText.name()}, {"@dim", theme::kClaudeDim.name()},
        {"@coral", theme::kClaudeCoral.name()},
    };
    for (const auto& [key, value] : tokens) qss.replace(QLatin1String(key), value);
    return qss;
}

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
    setObjectName(QStringLiteral("ClaudePanel"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(panelStyleSheet());

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(14, 10, 14, 10);
    v->setSpacing(8);

    // Header: spark + name, new chat on the right
    auto* top = new QHBoxLayout;
    top->setSpacing(8);
    auto* spark = new QLabel(this);
    spark->setPixmap(theme::pixmap(theme::Glyph::Spark, theme::kClaudeCoral, 20, devicePixelRatioF()));
    auto* title = new QLabel(QStringLiteral("Claude"), this);
    QFont titleFont = serifFont();
    titleFont.setPointSizeF(13);
    title->setFont(titleFont);
    m_newChat = new QPushButton(tr("New chat"), this);
    m_newChat->setObjectName(QStringLiteral("claudeGhost"));
    m_newChat->setIcon(theme::icon(theme::Glyph::Plus, theme::kClaudeDim, 14));
    m_newChat->setIconSize(QSize(14, 14));
    m_newChat->setCursor(Qt::PointingHandCursor);
    m_newChat->setToolTip(tr("Start a new conversation (the project is not changed)"));
    top->addWidget(spark);
    top->addWidget(title);
    top->addStretch(1);
    top->addWidget(m_newChat);
    v->addLayout(top);

    // Conversation: Claude's replies are set in a serif face, as in the Claude app
    m_log = new QTextBrowser(this);
    m_log->setObjectName(QStringLiteral("claudeLog"));
    m_log->setFrameShape(QFrame::NoFrame);
    m_log->setAccessibleName(tr("Claude conversation"));
    m_log->setOpenExternalLinks(true);
    QFont logFont = serifFont();
    logFont.setPointSizeF(10.5);
    m_log->setFont(logFont);
    QPalette logPal = m_log->palette();
    logPal.setColor(QPalette::Text, theme::kClaudeText);
    logPal.setColor(QPalette::Link, theme::kClaudeCoral);
    logPal.setColor(QPalette::Highlight, theme::kClaudeCoral);
    m_log->setPalette(logPal);
    m_log->document()->setDocumentMargin(2);
    v->addWidget(m_log, 1);
    showWelcome();

    // Composer: one rounded box holding the message, the model picker and send
    auto* composer = new QFrame(this);
    composer->setObjectName(QStringLiteral("claudeComposer"));
    auto* cv = new QVBoxLayout(composer);
    cv->setContentsMargins(12, 8, 8, 8);
    cv->setSpacing(4);
    m_input = new QPlainTextEdit(composer);
    m_input->setObjectName(QStringLiteral("claudeInput"));
    m_input->setFrameShape(QFrame::NoFrame);
    m_input->setPlaceholderText(tr("How can I help you edit?"));
    m_input->setToolTip(tr("Enter to send, Shift+Enter for a new line"));
    m_input->setFixedHeight(m_input->fontMetrics().lineSpacing() * 3 + 10);
    m_input->installEventFilter(this);
    m_input->setAccessibleName(tr("Message Claude"));
    cv->addWidget(m_input);

    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(6);
    m_model = new QComboBox(composer);
    m_model->setObjectName(QStringLiteral("claudeModel"));
    m_model->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_model->setCursor(Qt::PointingHandCursor);
    const QString saved = QSettings().value(QStringLiteral("claude/model"), QString::fromLatin1(kModels[0].id)).toString();
    for (const auto& mc : kModels) {
        m_model->addItem(QString::fromLatin1(mc.label), QString::fromLatin1(mc.id));
        if (saved == QLatin1String(mc.id)) m_model->setCurrentIndex(m_model->count() - 1);
    }
    m_model->setToolTip(tr("Claude model"));
    m_sendBtn = new QPushButton(composer);
    m_sendBtn->setObjectName(QStringLiteral("claudeSend"));
    m_sendBtn->setFixedSize(32, 32);
    m_sendBtn->setIconSize(QSize(18, 18));
    m_sendBtn->setCursor(Qt::PointingHandCursor);
    m_sendBtn->setAccessibleName(tr("Send to Claude"));
    bottom->addWidget(m_model);
    bottom->addStretch(1);
    bottom->addWidget(m_sendBtn);
    cv->addLayout(bottom);
    v->addWidget(composer);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("claudeStatus"));
    m_status->setWordWrap(true);
    m_status->setAlignment(Qt::AlignHCenter);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_status);
    setBusy(false);

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
    m_status->setStyleSheet(QStringLiteral("color: %1;").arg(error ? QLatin1String(kError) : theme::kClaudeDim.name()));
}

void ClaudePanel::setBusy(bool busy)
{
    m_busy = busy;
    m_sendBtn->setIcon(theme::icon(busy ? theme::Glyph::Stop : theme::Glyph::ArrowUp, Qt::white, 18));
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
        appendNote(tr("Node.js was not found. Install Node.js 18 or newer (nodejs.org), then try again."), kError);
        return false;
    }
    if (script.isEmpty()) {
        appendNote(tr("The Claude bridge is not installed. Run <code>npm install</code> in the claude-bridge "
                      "folder next to the editor. Looked in:<br>%1").arg(searched.toHtmlEscaped().replace("\n", "<br>")),
                   kError);
        return false;
    }
    const QString url = m_host.mcpUrl ? m_host.mcpUrl() : QString();
    if (url.isEmpty()) {
        appendNote(tr("The editor's MCP server is not running (Tools > Enable MCP Server). Claude needs it to "
                      "control the editor."), kError);
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
                appendNote(msg, kError);
            }
            setBusy(false);
        });
        connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
            if (err == QProcess::FailedToStart) {
                appendNote(tr("Could not start Node.js: %1").arg(m_proc->errorString().toHtmlEscaped()), kError);
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
            appendNote(tr("Claude could not connect to the editor's MCP server (status: %1).").arg(editor), kError);
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
            appendNote(tr("Stopped: %1").arg(why.toHtmlEscaped()), kError);
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
        appendNote(msg.toHtmlEscaped().replace("\n", "<br>"), kError);
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
    const QString spark = QUrl::fromLocalFile(theme::assetDir() + QStringLiteral("/spark@2x.png")).toString();
    const QString sans = QGuiApplication::font().family();
    QString html = QStringLiteral("<p align='center' style='margin-top:26px'><img src='%1' width='40' height='40'></p>").arg(spark);
    html += QStringLiteral("<p align='center' style='font-size:16pt; margin-top:6px'>%1</p>").arg(tr("What should we edit?"));
    html += QStringLiteral("<p align='center' style='font-family:\"%1\"; font-size:9pt; color:%2'>%3</p>")
                .arg(sans, theme::kClaudeDim.name(),
                     tr("Claude works on the open project with the editor's own tools, "
                        "so you see every change live and can undo it."));
    html += QStringLiteral("<p align='center' style='font-family:\"%1\"; font-size:9pt; color:%2; margin-top:14px'>")
                .arg(sans, theme::kClaudeText.name());
    const QStringList examples = {tr("Split the selected clip at the playhead"),
                                  tr("Make the second clip black and white with a 1 second fade in"),
                                  tr("Balance the audio so nothing clips"), tr("Export a 720p MP4 to my Desktop")};
    for (const QString& e : examples)
        html += QStringLiteral("<span style='color:%1'>&#8250;</span>&nbsp; %2<br>").arg(theme::kClaudeCoral.name(), e.toHtmlEscaped());
    html += QStringLiteral("</p>");
    m_log->setHtml(html);
    m_welcome = true;
}

void ClaudePanel::appendUser(const QString& text)
{
    if (m_welcome) {
        m_log->clear();
        m_welcome = false;
    }
    finishAssistant();
    // The message sits in a dark bubble, indented from the left like in the Claude app.
    QTextCursor c = endCursor();
    QTextFrameFormat ff;
    ff.setBackground(theme::kClaudeBubble);
    ff.setBorder(0);
    ff.setPadding(9);
    ff.setLeftMargin(34);
    ff.setTopMargin(10);
    ff.setBottomMargin(8);
    c.insertFrame(ff);
    QTextCharFormat cf;
    cf.setForeground(theme::kClaudeText);
    cf.setFont(sansFont(m_log->font(), 0.93));
    c.setBlockFormat(QTextBlockFormat());
    c.setCharFormat(cf);
    c.insertText(text);
    scrollToEnd();
}

void ClaudePanel::appendNote(const QString& html, const QString& color)
{
    finishAssistant();
    QTextCursor c = endCursor();
    if (!m_log->document()->isEmpty()) c.insertBlock(QTextBlockFormat(), QTextCharFormat());
    c.insertHtml(QStringLiteral("<span style='font-family:\"%1\"; font-size:9pt; color:%2'>%3</span>")
                     .arg(QGuiApplication::font().family(), color, html));
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
    f.setFont(sansFont(m_log->font(), 0.82));
    f.setForeground(theme::kClaudeDim);
    QTextCharFormat dot = f;
    dot.setForeground(theme::kClaudeCoral);
    c.insertText(QStringLiteral("● "), dot);
    QTextCharFormat nameF = f;
    nameF.setFontWeight(QFont::DemiBold);
    nameF.setForeground(QColor(194, 192, 182));
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
        f.setFont(sansFont(m_log->font(), 0.82));
        f.setForeground(err ? QColor(QLatin1String(kError)) : QColor(132, 176, 120));
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
