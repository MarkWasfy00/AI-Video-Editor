#pragma once

#include "core/Project.h"
#include "media/VideoDecoder.h"

#include <QColor>
#include <QMatrix4x4>
#include <QMetaType>
#include <QRect>

#include <memory>
#include <vector>

namespace ve {

// One visible clip at a given time, ready to be drawn by a GPU backend.
struct Layer {
    VideoFramePtr frame;
    ClipTransform transform;
    ClipEffects effects;
    double opacity = 1.0;          // transform opacity * fade envelope
};

// Everything a backend needs to draw one output frame (bottom -> top).
struct CompositeFrame {
    int canvasWidth = 1920;
    int canvasHeight = 1080;
    QColor background = Qt::black;
    TimeUs time = 0;
    std::vector<Layer> layers;
};
using CompositeFramePtr = std::shared_ptr<const CompositeFrame>;

// Per-layer shader parameters. Identical layout for the OpenGL uniforms and
// the Vulkan push-constant block (128 bytes = the minimum every Vulkan device
// supports; std430/std140 compatible).
struct ShaderParams {
    float mvp[16];                 // unit quad -> clip space
    float color[4];                // brightness, contrast, saturation, opacity
    float fx[4];                   // hue (radians), blur, vignette, flags (1 = grayscale, 2 = invert)
    float texel[4];                // 1/w, 1/h, 0, 0
    float crop[4];                 // visible texture rect: u0, v0, u1, v1
};
static_assert(sizeof(ShaderParams) == 128, "ShaderParams must match the shader block");

enum class NdcConvention { OpenGL, Vulkan };

// Model matrix mapping the unit quad to canvas pixels (origin top-left, y down).
QMatrix4x4 layerModelMatrix(const Layer& layer, int canvasW, int canvasH);
QMatrix4x4 canvasToNdc(int canvasW, int canvasH, NdcConvention conv);
ShaderParams makeShaderParams(const Layer& layer, int canvasW, int canvasH, NdcConvention conv);

// Largest rect with the canvas aspect ratio that fits in `area` (centred).
QRect letterbox(const QSize& area, int canvasW, int canvasH);

} // namespace ve

Q_DECLARE_METATYPE(ve::CompositeFramePtr)
