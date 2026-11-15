#pragma once

#include "render/GLCompositor.h"
#include "render/PreviewView.h"

#include <QOpenGLWidget>

namespace ve {

class GLPreviewWidget : public QOpenGLWidget, public PreviewView {
    Q_OBJECT
public:
    explicit GLPreviewWidget(QWidget* parent = nullptr);
    ~GLPreviewWidget() override;

    QWidget* widget() override { return this; }
    void setFrame(CompositeFramePtr frame) override;
    RenderBackend backend() const override { return RenderBackend::OpenGL; }
    QString backendDescription() const override { return m_description; }

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    void releaseGL();

    GLCompositor m_compositor;
    CompositeFramePtr m_frame;
    QString m_description = QStringLiteral("OpenGL");
};

} // namespace ve
