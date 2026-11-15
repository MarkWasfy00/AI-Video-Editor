#pragma once

#include "render/GLCompositor.h"

#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>

#include <cstdint>
#include <memory>
#include <vector>

namespace ve {

// Renders CompositeFrames with the OpenGL compositor into an offscreen FBO
// (GPU), then reads the pixels back. Used by export and by the API's
// render_frame. GUI thread only.
class OffscreenRenderer {
public:
    OffscreenRenderer() = default;
    ~OffscreenRenderer();
    OffscreenRenderer(const OffscreenRenderer&) = delete;
    OffscreenRenderer& operator=(const OffscreenRenderer&) = delete;

    bool initialize(int width, int height, QString* error = nullptr);

    // Bottom-up RGBA rows (OpenGL order), width * height * 4 bytes; valid
    // until the next call.
    const std::uint8_t* renderRaw(const CompositeFrame& frame);
    QImage render(const CompositeFrame& frame);   // top-down RGBA8888 copy

    int width() const { return m_width; }
    int height() const { return m_height; }

private:
    QOpenGLContext m_ctx;
    QOffscreenSurface m_surface;
    std::unique_ptr<QOpenGLFramebufferObject> m_fbo;
    GLCompositor m_compositor;
    std::vector<std::uint8_t> m_pixels;
    int m_width = 0;
    int m_height = 0;
    bool m_ready = false;
};

} // namespace ve
