#include "render/Composite.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ve {

QMatrix4x4 layerModelMatrix(const Layer& layer, int canvasW, int canvasH)
{
    QMatrix4x4 m;
    if (!layer.frame || layer.frame->width <= 0 || layer.frame->height <= 0) return m;
    const double fw = layer.frame->width;
    const double fh = layer.frame->height;
    // Fit the source into the canvas, then apply the user scale.
    const double fit = std::min(canvasW / fw, canvasH / fh);
    const double w = fw * fit * layer.transform.scale;
    const double h = fh * fit * layer.transform.scale;
    const double cx = canvasW * 0.5 + layer.transform.x;
    const double cy = canvasH * 0.5 + layer.transform.y;

    m.translate(static_cast<float>(cx), static_cast<float>(cy));
    m.rotate(static_cast<float>(layer.transform.rotation), 0.0f, 0.0f, 1.0f); // y-down => clockwise
    m.scale(static_cast<float>(w), static_cast<float>(h));
    m.translate(-0.5f, -0.5f);
    return m;
}

QMatrix4x4 canvasToNdc(int canvasW, int canvasH, NdcConvention conv)
{
    QMatrix4x4 p;
    if (conv == NdcConvention::OpenGL)
        p.ortho(0.0f, float(canvasW), float(canvasH), 0.0f, -1.0f, 1.0f);   // y up in NDC
    else
        p.ortho(0.0f, float(canvasW), 0.0f, float(canvasH), -1.0f, 1.0f);   // y down in NDC
    return p;
}

ShaderParams makeShaderParams(const Layer& layer, int canvasW, int canvasH, NdcConvention conv)
{
    ShaderParams sp{};
    const QMatrix4x4 mvp = canvasToNdc(canvasW, canvasH, conv) * layerModelMatrix(layer, canvasW, canvasH);
    std::memcpy(sp.mvp, mvp.constData(), sizeof(sp.mvp)); // column-major, as GLSL expects

    const auto& e = layer.effects;
    sp.color[0] = static_cast<float>(e.brightness);
    sp.color[1] = static_cast<float>(e.contrast);
    sp.color[2] = static_cast<float>(e.saturation);
    sp.color[3] = static_cast<float>(std::clamp(layer.opacity, 0.0, 1.0));
    sp.fx[0] = static_cast<float>(e.hue * M_PI / 180.0);
    sp.fx[1] = static_cast<float>(std::max(0.0, e.blur));
    sp.fx[2] = static_cast<float>(std::clamp(e.vignette, 0.0, 1.0));
    sp.fx[3] = static_cast<float>((e.grayscale ? 1 : 0) + (e.invert ? 2 : 0));
    if (layer.frame) {
        sp.texel[0] = 1.0f / std::max(1, layer.frame->width);
        sp.texel[1] = 1.0f / std::max(1, layer.frame->height);
    }
    const auto& t = layer.transform;
    sp.crop[0] = static_cast<float>(std::clamp(t.cropLeft, 0.0, 0.95));
    sp.crop[1] = static_cast<float>(std::clamp(t.cropTop, 0.0, 0.95));
    sp.crop[2] = static_cast<float>(1.0 - std::clamp(t.cropRight, 0.0, 0.95));
    sp.crop[3] = static_cast<float>(1.0 - std::clamp(t.cropBottom, 0.0, 0.95));
    return sp;
}

QRect letterbox(const QSize& area, int canvasW, int canvasH)
{
    if (area.isEmpty() || canvasW <= 0 || canvasH <= 0) return {};
    const double s = std::min(double(area.width()) / canvasW, double(area.height()) / canvasH);
    const int w = std::max(1, int(std::lround(canvasW * s)));
    const int h = std::max(1, int(std::lround(canvasH * s)));
    return QRect((area.width() - w) / 2, (area.height() - h) / 2, w, h);
}

} // namespace ve
