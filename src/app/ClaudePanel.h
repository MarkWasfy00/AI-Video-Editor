#pragma once

#include <QHash>
#include <QJsonObject>
#include <QProcess>
#include <QTextCursor>
#include <QWidget>

#include <functional>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;

namespace ve {

// Chat with Claude inside the editor. Runs claude-bridge/bridge.mjs (Claude
// Agent SDK) with Node; Claude edits the open project through the editor's
// own MCP endpoint, so every change shows up live and is undoable.
class ClaudePanel : public QWidget {
    Q_OBJECT
public:
    struct Host {
        std::function<QString()> mcpUrl;        // empty = MCP server could not be started
        std::function<QString()> editorState;   // one-line context sent with each message
    };

    explicit ClaudePanel(Host host, QWidget* parent = nullptr);
    ~ClaudePanel() override;

    void focusInput();
    bool isBusy() const { return m_busy; }

    // Locations searched for the bridge script / Node, for diagnostics.
    static QString findBridgeScript(QString* searched = nullptr);
    static QString findNode();

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void send();
    void stop();
    void newChat();
    bool ensureBridge();
    void writeCommand(const QJsonObject& cmd);
    void readBridge();
    void handle(const QJsonObject& m);
    void setBusy(bool busy);
    void setStatus(const QString& text, bool error = false);

    // transcript
    void appendUser(const QString& text);
    void appendNote(const QString& text, const QString& color);
    void appendToolUse(const QString& id, const QString& name, const QJsonObject& input);
    void appendToolResult(const QJsonObject& m);
    void renderAssistant();
    void finishAssistant();
    QTextCursor endCursor() const;
    void scrollToEnd();
    void showWelcome();

    Host m_host;
    QComboBox* m_model = nullptr;
    QPushButton* m_newChat = nullptr;
    QLabel* m_status = nullptr;
    QTextBrowser* m_log = nullptr;
    QPlainTextEdit* m_input = nullptr;
    QPushButton* m_sendBtn = nullptr;

    QProcess* m_proc = nullptr;
    QByteArray m_stdoutBuf;
    QStringList m_stderrTail;
    bool m_busy = false;
    bool m_subscription = true;     // Claude login (no per-token billing shown)

    int m_assistStart = -1;         // document position of the streaming reply
    QString m_assistText;
    QHash<QString, int> m_toolMarks;  // tool_use id -> block number of its line
    int m_imageCount = 0;
    bool m_welcome = false;         // the example text is showing
};

} // namespace ve
