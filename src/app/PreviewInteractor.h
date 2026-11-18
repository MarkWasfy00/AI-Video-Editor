#pragma once

#include "render/Composite.h"
#include "render/PreviewView.h"

#include <QObject>
#include <QPointF>

#include <functional>

namespace ve {

class Project;

// Direct manipulation in the preview: click a picture or text to select it,
// drag to move (snaps to the centre lines; hold Ctrl to place freely), drag a
// corner to resize, Alt+drag to rotate, wheel to scale, Shift+wheel to rotate,
// double-click text to edit it. Draws the selection outline into preview frames.
class PreviewInteractor : public QObject {
    Q_OBJECT
public:
    struct Host {
        std::function<QString()> selectedClip;
        std::function<void(const QString&)> selectClip;
        std::function<void(const QString&)> editText;     // double-click on a text clip
        std::function<bool()> isPlaying;
        std::function<CompositeFramePtr()> lastFrame;     // as rendered (no outline)
    };

    PreviewInteractor(Project* project, Host host, QObject* parent = nullptr);

    void attach(PreviewView* view);        // after the preview (backend) changes
    // The frame with the selected clip's outline and corner handles on top.
    CompositeFramePtr decorate(const CompositeFramePtr& frame);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class Mode { None, Move, Scale, Rotate };
    QRect viewRect(const CompositeFrame& f) const;        // where the canvas sits in the view
    QPointF toCanvas(const QPointF& viewPos, const CompositeFrame& f) const;
    const Layer* layerOf(const CompositeFrame& f, const QString& clipId) const;
    QString hitTest(const CompositeFrame& f, const QPointF& canvasPos) const;
    int cornerAt(const CompositeFrame& f, const Layer& l, const QPointF& viewPos) const;   // -1 = none
    QPointF layerCentre(const CompositeFrame& f, const Layer& l) const;
    void applyTransform(const QString& clipId, const ClipTransform& t);
    void updateCursor(const QPointF& viewPos);

    Project* m_project;
    Host m_host;
    PreviewView* m_view = nullptr;
    Mode m_mode = Mode::None;
    QString m_dragClip;
    ClipTransform m_start;
    QPointF m_pressCanvas;
    QPointF m_centre;                      // canvas
    int m_dragSerial = 0;                  // one undo step per drag
    VideoFramePtr m_outline;               // cached outline image
    QSize m_outlineSize;
    int m_outlineStroke = 0;
};

} // namespace ve
