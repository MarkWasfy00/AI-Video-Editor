#pragma once

#include "render/Composite.h"

#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>

#include <memory>
#include <vector>

namespace ve {

// OpenGL 3.3 core compositor. Draws a CompositeFrame into the currently
// bound framebuffer. Used by the OpenGL preview and by the exporter
// (offscreen FBO), so preview and export look identical.
class GLCompositor : protected QOpenGLExtraFunctions {
public:
    GLCompositor() = default;
    ~GLCompositor();

    bool initialize(QString* error = nullptr);   // needs a current context
    void cleanup();                              // needs the same context current
    bool isInitialized() const { return m_program != nullptr; }

    // Draws into `viewport` (framebuffer pixels). Area outside is untouched.
    void render(const CompositeFrame& frame, const QRect& viewport, int framebufferHeight);

private:
    struct Slot {
        GLuint texture = 0;
        int width = 0;
        int height = 0;
        VideoFramePtr uploaded;
    };
    void upload(Slot& slot, const VideoFramePtr& frame);

    std::unique_ptr<QOpenGLShaderProgram> m_program;
    QOpenGLVertexArrayObject m_vao;
    std::vector<Slot> m_slots;
    int m_locMvp = -1, m_locColor = -1, m_locFx = -1, m_locTexel = -1, m_locCrop = -1, m_locTex = -1;
};

} // namespace ve
