#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>
#include <vector>

namespace ve {

class EditorApi;

// Model Context Protocol server (JSON-RPC 2.0) exposing every EditorApi
// operation as an MCP tool, so Claude (Agent SDK / Claude Code / Desktop)
// can drive the editor. Transport-agnostic: see McpTransports.h.
class McpServer : public QObject {
    Q_OBJECT
public:
    static constexpr const char* kLatestProtocol = "2025-06-18";

    explicit McpServer(EditorApi* api, QObject* parent = nullptr);

    // Handles one raw JSON-RPC message (request or notification). Returns the
    // serialized response, or an empty array for notifications.
    QByteArray handle(const QByteArray& message);

    // Handles one parsed message; returns {} for notifications.
    QJsonObject handleMessage(const QJsonObject& msg);

    QJsonArray toolDefinitions() const;
    int toolCount() const { return int(m_tools.size()); }

signals:
    // Emitted after every tools/call (for logging / UI status).
    void toolCalled(const QString& name, bool ok);

private:
    // Tools return a JSON result; they may append extra MCP content (images).
    using Handler = std::function<QJsonObject(const QJsonObject& args, QJsonArray& extraContent)>;
    struct Tool {
        QString name;
        QString title;
        QString description;
        QJsonObject inputSchema;
        bool readOnly = false;
        bool destructive = false;
        bool idempotent = false;
        Handler run;
    };

    void registerTools();
    void add(Tool t) { m_tools.push_back(std::move(t)); }
    QJsonObject callTool(const QString& name, const QJsonObject& args);

    EditorApi* m_api;
    std::vector<Tool> m_tools;
};

} // namespace ve
