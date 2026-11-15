#pragma once

#include "render/Composite.h"

#include <QString>

class QWidget;

namespace ve {

enum class RenderBackend { OpenGL, Vulkan };

// A preview surface that displays composited frames. Implemented by the
// OpenGL (QOpenGLWidget) and Vulkan (QVulkanWindow) backends.
class PreviewView {
public:
    virtual ~PreviewView() = default;
    virtual QWidget* widget() = 0;
    virtual void setFrame(CompositeFramePtr frame) = 0;
    virtual RenderBackend backend() const = 0;
    virtual QString backendDescription() const = 0;
};

} // namespace ve
