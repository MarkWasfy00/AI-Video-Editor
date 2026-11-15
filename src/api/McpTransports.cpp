#include "api/McpTransports.h"

#include "api/McpServer.h"

#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUuid>

#include <cstdio>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace ve {

// ===========================================================================
// HTTP
// ===========================================================================
namespace {

const char* reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    default: return "Error";
    }
}

constexpr qsizetype kMaxBody = 32 * 1024 * 1024;

// Rejects browser requests from non-local origins (DNS-rebinding protection).
bool originAllowed(const QByteArray& origin)
{
    if (origin.isEmpty() || origin == "null") return true;
    const QString host = QUrl(QString::fromLatin1(origin)).host();
    return host == QLatin1String("localhost") || host == QLatin1String("127.0.0.1") || host == QLatin1String("::1");
}

} // namespace

McpHttpTransport::McpHttpTransport(McpServer* server, QObject* parent)
    : QObject(parent)
    , m_server(server)
    , m_tcp(new QTcpServer(this))
    , m_sessionId(QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1())
{
    connect(m_tcp, &QTcpServer::newConnection, this, &McpHttpTransport::onNewConnection);
}

McpHttpTransport::~McpHttpTransport() = default;

bool McpHttpTransport::listen(quint16 port, QString* error)
{
    if (m_tcp->isListening()) m_tcp->close();
    if (!m_tcp->listen(QHostAddress::LocalHost, port)) {
        if (error) *error = m_tcp->errorString();
        return false;
    }
    return true;
}

void McpHttpTransport::close()
{
    m_tcp->close();
    for (auto it = m_conns.begin(); it != m_conns.end(); ++it) it.key()->disconnectFromHost();
}

bool McpHttpTransport::isListening() const { return m_tcp->isListening(); }
quint16 McpHttpTransport::port() const { return m_tcp->serverPort(); }
QString McpHttpTransport::url() const { return QStringLiteral("http://127.0.0.1:%1/mcp").arg(port()); }

void McpHttpTransport::onNewConnection()
{
    while (QTcpSocket* s = m_tcp->nextPendingConnection()) {
        m_conns.insert(s, {});
        connect(s, &QTcpSocket::readyRead, this, [this, s] { onReadyRead(s); });
        connect(s, &QTcpSocket::disconnected, this, [this, s] {
            m_conns.remove(s);
            s->deleteLater();
        });
    }
}

void McpHttpTransport::respond(QTcpSocket* s, int status, const QByteArray& body, const QByteArray& contentType,
                               const QList<QPair<QByteArray, QByteArray>>& extraHeaders)
{
    QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason(status) + "\r\n";
    if (!body.isEmpty()) out += "Content-Type: " + contentType + "\r\n";
    out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\n";
    for (const auto& [k, v] : extraHeaders) out += k + ": " + v + "\r\n";
    out += "\r\n";
    out += body;
    s->write(out);
}

void McpHttpTransport::onReadyRead(QTcpSocket* s)
{
    auto it = m_conns.find(s);
    if (it == m_conns.end()) return;
    QByteArray& buf = it->buffer;
    buf += s->readAll();

    // A connection may carry several keep-alive requests.
    for (;;) {
        const qsizetype headerEnd = buf.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            if (buf.size() > 64 * 1024) { respond(s, 400, {}); s->disconnectFromHost(); }
            return;
        }
        const QList<QByteArray> lines = buf.left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
        const QByteArray method = requestLine.value(0);
        QByteArray path = requestLine.value(1);
        if (const qsizetype q = path.indexOf('?'); q >= 0) path.truncate(q);

        QHash<QByteArray, QByteArray> headers;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray l = lines[i].trimmed();
            const qsizetype c = l.indexOf(':');
            if (c > 0) headers.insert(l.left(c).trimmed().toLower(), l.mid(c + 1).trimmed());
        }
        const qsizetype length = headers.value("content-length", "0").toLongLong();
        if (length < 0 || length > kMaxBody) { respond(s, 413, {}); s->disconnectFromHost(); return; }
        if (buf.size() < headerEnd + 4 + length) return; // wait for the full body
        const QByteArray body = buf.mid(headerEnd + 4, length);
        buf.remove(0, headerEnd + 4 + length);

        if (!originAllowed(headers.value("origin"))) {
            respond(s, 403, R"({"error":"origin not allowed"})");
            continue;
        }
        if (path == "/health" && method == "GET") {
            respond(s, 200, QByteArray(R"({"ok":true,"server":"video-editor","tools":)") +
                                QByteArray::number(m_server->toolCount()) + "}");
            continue;
        }
        if (path != "/mcp" && path != "/") {
            respond(s, 404, R"({"error":"use POST /mcp"})");
            continue;
        }
        if (method == "POST") {
            const QByteArray response = m_server->handle(body);
            const QList<QPair<QByteArray, QByteArray>> extra = {{"Mcp-Session-Id", m_sessionId}};
            if (response.isEmpty()) respond(s, 202, {}, {}, extra);   // notification
            else respond(s, 200, response, "application/json", extra);
        } else if (method == "DELETE") {
            respond(s, 200, {});
        } else if (method == "OPTIONS") {
            respond(s, 204, {}, {}, {{"Allow", "POST, DELETE, OPTIONS"}});
        } else {
            // No server-initiated SSE stream is offered.
            respond(s, 405, {}, {}, {{"Allow", "POST, DELETE, OPTIONS"}});
        }
    }
}

// ===========================================================================
// stdio
// ===========================================================================
namespace {

class StdinReader : public QThread {
    Q_OBJECT
public:
    using QThread::QThread;
signals:
    void line(const QByteArray& line);
    void eof();

protected:
    void run() override
    {
        std::string l;
        while (std::getline(std::cin, l)) {
            QByteArray b = QByteArray::fromStdString(l).trimmed(); // also strips \r
            if (!b.isEmpty()) emit line(b);
        }
        emit eof();
    }
};

} // namespace

McpStdioTransport::McpStdioTransport(McpServer* server, QObject* parent)
    : QObject(parent)
    , m_server(server)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY); // no \r\n translation in JSON-RPC output
    _setmode(_fileno(stdin), _O_BINARY);
#endif
}

McpStdioTransport::~McpStdioTransport()
{
    if (m_reader) {
        m_reader->terminate(); // blocked in getline; process is exiting anyway
        m_reader->wait(500);
    }
}

void McpStdioTransport::start()
{
    auto* reader = new StdinReader(this);
    m_reader = reader;
    connect(reader, &StdinReader::line, this, &McpStdioTransport::onLine, Qt::QueuedConnection);
    connect(reader, &StdinReader::eof, qApp, &QCoreApplication::quit, Qt::QueuedConnection);
    reader->start();
}

void McpStdioTransport::onLine(const QByteArray& line)
{
    const QByteArray response = m_server->handle(line);
    if (response.isEmpty()) return;
    std::fwrite(response.constData(), 1, size_t(response.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

} // namespace ve

#include "McpTransports.moc"
