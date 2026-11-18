#include "app/MainWindow.h"
#include "app/Theme.h"
#include "media/FFmpegUtil.h"
#include "media/HwAccel.h"
#include "render/Composite.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QSettings>
#include <QSurfaceFormat>

#ifdef _WIN32
// Ask hybrid-graphics laptops (NVIDIA Optimus / AMD PowerXpress) to run this
// process on the discrete GPU instead of the integrated one.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

int main(int argc, char* argv[])
{
    // OpenGL 3.3 core for the preview and the export renderer.
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("VideoEditor"));
    QApplication::setOrganizationName(QStringLiteral("VideoEditor"));
    QApplication::setApplicationVersion(QStringLiteral("0.2.0"));
    ve::theme::apply(app);

    qRegisterMetaType<ve::CompositeFramePtr>("ve::CompositeFramePtr");
    qRegisterMetaType<ve::TimeUs>("ve::TimeUs");
    av_log_set_level(AV_LOG_ERROR);
    ve::hw::setDecodeMode(QSettings().value(QStringLiteral("gpu/decode"), QStringLiteral("auto")).toString());

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Basic GPU-accelerated video editor"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption rendererOpt(QStringLiteral("renderer"),
                                   QStringLiteral("Preview renderer: opengl or vulkan."), QStringLiteral("name"));
    parser.addOption(rendererOpt);
    QCommandLineOption mcpPortOpt(QStringLiteral("mcp-port"), QStringLiteral("Port for the MCP (Claude) HTTP endpoint."),
                                  QStringLiteral("port"));
    QCommandLineOption noMcpOpt(QStringLiteral("no-mcp"), QStringLiteral("Disable the MCP endpoint."));
    parser.addOption(mcpPortOpt);
    parser.addOption(noMcpOpt);
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("Project (.veproj/.json) or media files to open."), QStringLiteral("[files...]"));
    parser.process(app);

    QString renderer = QSettings().value(QStringLiteral("renderer"), QStringLiteral("opengl")).toString();
    if (parser.isSet(rendererOpt)) renderer = parser.value(rendererOpt).toLower();
    const ve::RenderBackend backend = renderer == QLatin1String("vulkan") ? ve::RenderBackend::Vulkan : ve::RenderBackend::OpenGL;

    const int mcpPort = parser.isSet(noMcpOpt) ? -1 : (parser.isSet(mcpPortOpt) ? parser.value(mcpPortOpt).toInt() : 0);
    ve::MainWindow window(backend, mcpPort);
    window.show();

    const QStringList files = parser.positionalArguments();
    if (!files.isEmpty()) {
        const QString first = files.first();
        const QString suffix = QFileInfo(first).suffix().toLower();
        if (suffix == QLatin1String("veproj") || suffix == QLatin1String("json"))
            window.openProject(first);
        else
            window.importFiles(files);
    }
    return app.exec();
}
