#include "render/GLPreviewWidget.h"

#include <QOpenGLContext>
#include <QDebug>

namespace ve {

GLPreviewWidget::GLPreviewWidget(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setMinimumSize(320, 180);
}

GLPreviewWidget::~GLPreviewWidget()
{
    releaseGL();
}

void GLPreviewWidget::releaseGL()
{
    if (!m_compositor.isInitialized() || !context()) return;
    makeCurrent();
    m_compositor.cleanup();
    doneCurrent();
}

void GLPreviewWidget::setFrame(CompositeFramePtr frame)
{
    m_frame = std::move(frame);
    update();
}

void GLPreviewWidget::initializeGL()
{
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, &GLPreviewWidget::releaseGL, Qt::DirectConnection);
    QString err;
    if (!m_compositor.initialize(&err))
        qWarning() << "OpenGL compositor init failed:" << err;
    auto* f = context()->functions();
    const auto* renderer = reinterpret_cast<const char*>(f->glGetString(GL_RENDERER));
    const auto* version = reinterpret_cast<const char*>(f->glGetString(GL_VERSION));
    m_description = QStringLiteral("OpenGL %1 - %2").arg(QString::fromLatin1(version ? version : "?"),
                                                         QString::fromLatin1(renderer ? renderer : "?"));
}

void GLPreviewWidget::paintGL()
{
    auto* f = context()->functions();
    f->glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT);

    const qreal dpr = devicePixelRatioF();
    const QSize fb(int(width() * dpr), int(height() * dpr));
    CompositeFrame empty;
    const CompositeFrame& frame = m_frame ? *m_frame : empty;
    const QRect vp = letterbox(fb, frame.canvasWidth, frame.canvasHeight);
    m_compositor.render(frame, vp, fb.height());
}

} // namespace ve
