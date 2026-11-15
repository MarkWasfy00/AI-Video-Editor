#pragma once

#include <QHash>
#include <QObject>
#include <QThread>

class QTcpServer;
class QTcpSocket;

namespace ve {

class McpServer;

// MCP "Streamable HTTP" transport (JSON responses, no SSE stream), bound to
// 127.0.0.1 only. Endpoint: POST http://127.0.0.1:<port>/mcp
class McpHttpTransport : public QObject {
    Q_OBJECT
public:
    explicit McpHttpTransport(McpServer* server, QObject* parent = nullptr);
    ~McpHttpTransport() override;

    bool listen(quint16 port, QString* error = nullptr);
    void close();
    bool isListening() const;
    quint16 port() const;
    QString url() const;

private:
    struct Conn {
        QByteArray buffer;
    };
    void onNewConnection();
    void onReadyRead(QTcpSocket* s);
    void respond(QTcpSocket* s, int status, const QByteArray& body, const QByteArray& contentType = "application/json",
                 const QList<QPair<QByteArray, QByteArray>>& extraHeaders = {});

    McpServer* m_server;
    QTcpServer* m_tcp = nullptr;
    QHash<QTcpSocket*, Conn> m_conns;
    QByteArray m_sessionId;
};

// MCP stdio transport: newline-delimited JSON-RPC on stdin/stdout. Reads on a
// background thread, handles messages on the main thread. Quits the app on EOF.
class McpStdioTransport : public QObject {
    Q_OBJECT
public:
    explicit McpStdioTransport(McpServer* server, QObject* parent = nullptr);
    ~McpStdioTransport() override;
    void start();

private:
    void onLine(const QByteArray& line);

    McpServer* m_server;
    QThread* m_reader = nullptr;
};

} // namespace ve
