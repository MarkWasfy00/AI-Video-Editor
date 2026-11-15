#include "render/GLCompositor.h"

#include <QFile>

namespace ve {

GLCompositor::~GLCompositor()
{
    // Resources must be released with cleanup() while the context is current;
    // if that did not happen we leak rather than touch a dead context.
}

static QByteArray readResource(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool GLCompositor::initialize(QString* error)
{
    initializeOpenGLFunctions();
    m_program = std::make_unique<QOpenGLShaderProgram>();
    const bool ok =
        m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, readResource(QStringLiteral(":/shaders/gl/composite.vert"))) &&
        m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, readResource(QStringLiteral(":/shaders/gl/composite.frag"))) &&
        m_program->link();
    if (!ok) {
        if (error) *error = m_program->log();
        m_program.reset();
        return false;
    }
    m_locMvp = m_program->uniformLocation("uMvp");
    m_locColor = m_program->uniformLocation("uColor");
    m_locFx = m_program->uniformLocation("uFx");
    m_locTexel = m_program->uniformLocation("uTexel");
    m_locCrop = m_program->uniformLocation("uCrop");
    m_locTex = m_program->uniformLocation("uTex");
    m_vao.create(); // core profile requires a bound VAO, even without attributes
    return true;
}

void GLCompositor::cleanup()
{
    for (auto& s : m_slots)
        if (s.texture) glDeleteTextures(1, &s.texture);
    m_slots.clear();
    if (m_vao.isCreated()) m_vao.destroy();
    m_program.reset();
}

void GLCompositor::upload(Slot& slot, const VideoFramePtr& frame)
{
    if (slot.uploaded == frame) return; // same frame as last time
    if (!slot.texture) {
        glGenTextures(1, &slot.texture);
        glBindTexture(GL_TEXTURE_2D, slot.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, slot.texture);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (slot.width != frame->width || slot.height != frame->height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, frame->width, frame->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, frame->rgba.data());
        slot.width = frame->width;
        slot.height = frame->height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame->width, frame->height, GL_RGBA, GL_UNSIGNED_BYTE, frame->rgba.data());
    }
    slot.uploaded = frame;
}

void GLCompositor::render(const CompositeFrame& frame, const QRect& viewport, int framebufferHeight)
{
    if (!m_program || viewport.isEmpty()) return;

    // Qt rects are y-down, GL viewports are y-up.
    const int glY = framebufferHeight - viewport.y() - viewport.height();
    glViewport(viewport.x(), glY, viewport.width(), viewport.height());
    glEnable(GL_SCISSOR_TEST);
    glScissor(viewport.x(), glY, viewport.width(), viewport.height());
    glClearColor(frame.background.redF(), frame.background.greenF(), frame.background.blueF(), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    if (m_slots.size() < frame.layers.size()) m_slots.resize(frame.layers.size());

    m_program->bind();
    m_vao.bind();
    glActiveTexture(GL_TEXTURE0);
    m_program->setUniformValue(m_locTex, 0);

    for (size_t i = 0; i < frame.layers.size(); ++i) {
        const Layer& layer = frame.layers[i];
        if (!layer.frame || layer.opacity <= 0.0) continue;
        upload(m_slots[i], layer.frame);
        glBindTexture(GL_TEXTURE_2D, m_slots[i].texture);

        const ShaderParams p = makeShaderParams(layer, frame.canvasWidth, frame.canvasHeight, NdcConvention::OpenGL);
        glUniformMatrix4fv(m_locMvp, 1, GL_FALSE, p.mvp);
        glUniform4fv(m_locColor, 1, p.color);
        glUniform4fv(m_locFx, 1, p.fx);
        glUniform4fv(m_locTexel, 1, p.texel);
        glUniform4fv(m_locCrop, 1, p.crop);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    m_vao.release();
    m_program->release();
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
}

} // namespace ve
