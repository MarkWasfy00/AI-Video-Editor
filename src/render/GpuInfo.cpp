#include "render/GpuInfo.h"

#include "media/HwAccel.h"

#ifdef VE_HAVE_VULKAN
#include "render/VulkanPreview.h"
#endif

#include <QJsonArray>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>

namespace ve {

static QJsonObject openGlInfo()
{
    static QJsonObject cached;
    if (!cached.isEmpty()) return cached;
    QOpenGLContext* previous = QOpenGLContext::currentContext();
    QSurface* previousSurface = previous ? previous->surface() : nullptr;

    QOpenGLContext ctx;
    QOffscreenSurface surface;
    if (ctx.create()) {
        surface.setFormat(ctx.format());
        surface.create();
        if (ctx.makeCurrent(&surface)) {
            auto str = [&](GLenum e) {
                const auto* s = reinterpret_cast<const char*>(ctx.functions()->glGetString(e));
                return QString::fromLatin1(s ? s : "");
            };
            cached = {{"vendor", str(GL_VENDOR)}, {"renderer", str(GL_RENDERER)}, {"version", str(GL_VERSION)}};
            ctx.doneCurrent();
        }
    }
    if (previous && previousSurface) previous->makeCurrent(previousSurface);
    if (cached.isEmpty()) cached = {{"error", "no OpenGL context"}};
    return cached;
}

QJsonObject collectGpuInfo()
{
    QJsonObject o;
    o["opengl"] = openGlInfo();

    QJsonObject vk;
#ifdef VE_HAVE_VULKAN
    vk["built"] = true;
    QJsonArray devices;
    for (const auto& d : vulkanDevices())
        devices.append(QJsonObject{{"name", d.name}, {"type", d.type}, {"apiVersion", d.apiVersion}});
    vk["available"] = !devices.isEmpty();
    vk["devices"] = devices;
#else
    vk["built"] = false;
    vk["available"] = false;
#endif
    o["vulkan"] = vk;

    QJsonArray decoders;
    for (const QString& t : hw::availableDecodeDevices())
        decoders.append(QJsonObject{{"name", t}, {"label", hw::deviceLabel(t)}});
    o["decode"] = QJsonObject{{"mode", hw::decodeMode()}, {"available", decoders}};

    QJsonArray enc;
    for (const auto& e : hw::availableEncoders())
        enc.append(QJsonObject{{"name", e.name}, {"label", e.label}, {"hardware", e.hardware}});
    o["encoders"] = enc;
    return o;
}

} // namespace ve
