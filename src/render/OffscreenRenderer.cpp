#include "render/OffscreenRenderer.h"

#include <QOpenGLFunctions>

#include <cstring>

namespace ve {

OffscreenRenderer::~OffscreenRenderer()
{
    if (!m_ready) return;
    if (m_ctx.makeCurrent(&m_surface)) {
        m_compositor.cleanup();
        m_fbo.reset();
        m_ctx.doneCurrent();
    }
}

bool OffscreenRenderer::initialize(int width, int height, QString* error)
{
    m_width = width;
    m_height = height;
    m_ctx.setFormat(QSurfaceFormat::defaultFormat());
    if (!m_ctx.create()) {
        if (error) *error = QStringLiteral("Cannot create an OpenGL context.");
        return false;
    }
    m_surface.setFormat(m_ctx.format());
    m_surface.create();
    if (!m_ctx.makeCurrent(&m_surface)) {
        if (error) *error = QStringLiteral("Cannot activate the OpenGL context.");
        return false;
    }
    QOpenGLFramebufferObjectFormat fmt;
    fmt.setInternalTextureFormat(GL_RGBA8);
    m_fbo = std::make_unique<QOpenGLFramebufferObject>(width, height, fmt);
    QString glError;
    if (!m_fbo->isValid() || !m_compositor.initialize(&glError)) {
        if (error) *error = QStringLiteral("OpenGL setup failed: %1").arg(glError);
        m_fbo.reset();
        m_ctx.doneCurrent();
        return false;
    }
    m_pixels.resize(size_t(width) * height * 4);
    m_ready = true;
    return true;
}

const std::uint8_t* OffscreenRenderer::renderRaw(const CompositeFrame& frame)
{
    if (!m_ready) return nullptr;
    m_ctx.makeCurrent(&m_surface); // a UI repaint may have switched contexts
    m_fbo->bind();
    m_compositor.render(frame, QRect(0, 0, m_width, m_height), m_height);
    QOpenGLFunctions* f = m_ctx.functions();
    f->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    f->glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, m_pixels.data());
    m_fbo->release();
    return m_pixels.data();
}

QImage OffscreenRenderer::render(const CompositeFrame& frame)
{
    const std::uint8_t* px = renderRaw(frame);
    if (!px) return {};
    QImage img(m_width, m_height, QImage::Format_RGBA8888);
    const size_t row = size_t(m_width) * 4;
    for (int y = 0; y < m_height; ++y)
        std::memcpy(img.scanLine(y), px + size_t(m_height - 1 - y) * row, row);
    return img;
}

} // namespace ve
