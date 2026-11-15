// VideoEditorMCP: headless MCP server for the video editor.
//
//   VideoEditorMCP                 MCP over stdio (for Claude Agent SDK / Claude Code / Desktop)
//   VideoEditorMCP --http 8765     MCP over HTTP at http://127.0.0.1:8765/mcp
//   VideoEditorMCP --project file  open a project at startup
//
// All logging goes to stderr; stdout carries only JSON-RPC messages.

#include "api/EditorApi.h"
#include "api/McpServer.h"
#include "api/McpTransports.h"
#include "core/Project.h"
#include "media/FFmpegUtil.h"
#include "media/HwAccel.h"
#include "render/Composite.h"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QSettings>
#include <QSurfaceFormat>

#include <cstdio>

#ifdef _WIN32
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace {

// Headless host: no window, no playback; keeps a playhead and selection.
class HeadlessHost : public ve::EditorHost {
public:
    QString hostKind() const override { return QStringLiteral("headless"); }
    ve::TimeUs playhead() const override { return m_playhead; }
    void seek(ve::TimeUs t) override { m_playhead = t; }
    QString selectedClip() const override { return m_selected; }
    void selectClip(const QString& id) override { m_selected = id; }

private:
    ve::TimeUs m_playhead = 0;
    QString m_selected;
};

} // namespace

int main(int argc, char* argv[])
{
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(fmt);

    QGuiApplication app(argc, argv);   // GUI app for the offscreen OpenGL renderer; no windows are shown
    QGuiApplication::setApplicationName(QStringLiteral("VideoEditor"));
    QGuiApplication::setOrganizationName(QStringLiteral("VideoEditor"));
    QGuiApplication::setApplicationVersion(QStringLiteral("0.2.0"));
    qRegisterMetaType<ve::CompositeFramePtr>("ve::CompositeFramePtr");
    av_log_set_level(AV_LOG_ERROR);
    ve::hw::setDecodeMode(QSettings().value(QStringLiteral("gpu/decode"), QStringLiteral("auto")).toString());

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Headless MCP server for the video editor"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption httpOpt(QStringLiteral("http"), QStringLiteral("Serve MCP over HTTP on this port instead of stdio."),
                               QStringLiteral("port"));
    QCommandLineOption projectOpt(QStringLiteral("project"), QStringLiteral("Project to open at startup."),
                                  QStringLiteral("path"));
    parser.addOption(httpOpt);
    parser.addOption(projectOpt);
    parser.process(app);

    ve::Project project;
    HeadlessHost host;
    ve::EditorApi api(&project, &host);
    ve::McpServer server(&api);

    if (parser.isSet(projectOpt)) {
        try {
            api.projectOpen(parser.value(projectOpt));
        } catch (const ve::ApiError& e) {
            std::fprintf(stderr, "VideoEditorMCP: %s\n", qPrintable(e.message()));
            return 1;
        }
    }

    ve::McpHttpTransport http(&server);
    ve::McpStdioTransport stdio(&server);
    if (parser.isSet(httpOpt)) {
        QString err;
        const quint16 port = quint16(parser.value(httpOpt).toUInt());
        if (!http.listen(port, &err)) {
            std::fprintf(stderr, "VideoEditorMCP: cannot listen on port %u: %s\n", port, qPrintable(err));
            return 1;
        }
        std::fprintf(stderr, "VideoEditorMCP: %d tools at %s\n", server.toolCount(), qPrintable(http.url()));
    } else {
        stdio.start();
        std::fprintf(stderr, "VideoEditorMCP: %d tools on stdio\n", server.toolCount());
    }
    return app.exec();
}
