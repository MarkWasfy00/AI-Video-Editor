#include "app/MainWindow.h"
#include "media/FFmpegUtil.h"
#include "media/HwAccel.h"
#include "render/Composite.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>
#include <QSurfaceFormat>

#ifdef _WIN32
// Ask hybrid-graphics laptops (NVIDIA Optimus / AMD PowerXpress) to run this
// process on the discrete GPU instead of the integrated one.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace {

void applyDarkTheme(QApplication& app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette p;
    const QColor window(45, 45, 50), base(32, 32, 36), text(225, 225, 230), accent(70, 130, 220);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::red);
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 125));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 125));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(120, 120, 125));
    app.setPalette(p);
}

} // namespace

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
    applyDarkTheme(app);

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
