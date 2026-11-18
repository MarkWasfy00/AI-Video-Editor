#include "app/PreviewInteractor.h"

#include "core/Project.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ve {

namespace {
constexpr double kPi = 3.14159265358979323846;
const QPointF kCorners[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
double len(const QPointF& p) { return std::hypot(p.x(), p.y()); }
} // namespace

PreviewInteractor::PreviewInteractor(Project* project, Host host, QObject* parent)
    : QObject(parent)
    , m_project(project)
    , m_host(std::move(host))
{
}

void PreviewInteractor::attach(PreviewView* view)
{
    m_view = view;
    m_mode = Mode::None;
    if (!view) return;
    if (auto* w = qobject_cast<QWidget*>(view->inputObject())) w->setMouseTracking(true);
    view->inputObject()->installEventFilter(this);
}

QRect PreviewInteractor::viewRect(const CompositeFrame& f) const
{
    return letterbox(m_view->widget()->size(), f.canvasWidth, f.canvasHeight);
}

QPointF PreviewInteractor::toCanvas(const QPointF& p, const CompositeFrame& f) const
{
    const QRect r = viewRect(f);
    const double k = double(f.canvasWidth) / std::max(1, r.width());
    return (p - QPointF(r.topLeft())) * k;
}

const Layer* PreviewInteractor::layerOf(const CompositeFrame& f, const QString& clipId) const
{
    if (clipId.isEmpty()) return nullptr;
    for (const Layer& l : f.layers)
        if (l.clipId == clipId) return &l;
    return nullptr;
}

QString PreviewInteractor::hitTest(const CompositeFrame& f, const QPointF& c) const
{
    for (auto it = f.layers.rbegin(); it != f.layers.rend(); ++it) {   // top first
        if (it->clipId.isEmpty() || !it->frame) continue;
        bool ok = false;
        const QMatrix4x4 inv = layerModelMatrix(*it, f.canvasWidth, f.canvasHeight).inverted(&ok);
        if (!ok) continue;
        const QPointF u = inv.map(c);
        if (u.x() >= 0 && u.x() <= 1 && u.y() >= 0 && u.y() <= 1) return it->clipId;
    }
    return {};
}

int PreviewInteractor::cornerAt(const CompositeFrame& f, const Layer& l, const QPointF& viewPos) const
{
    const QRect r = viewRect(f);
    const double k = double(r.width()) / std::max(1, f.canvasWidth);
    const QMatrix4x4 m = layerModelMatrix(l, f.canvasWidth, f.canvasHeight);
    for (int i = 0; i < 4; ++i) {
        const QPointF v = QPointF(r.topLeft()) + m.map(kCorners[i]) * k;
        if (len(v - viewPos) <= 10.0) return i;
    }
    return -1;
}

QPointF PreviewInteractor::layerCentre(const CompositeFrame& f, const Layer& l) const
{
    return layerModelMatrix(l, f.canvasWidth, f.canvasHeight).map(QPointF(0.5, 0.5));
}

void PreviewInteractor::applyTransform(const QString& clipId, const ClipTransform& t)
{
    const Clip* c = m_project->findClip(clipId);
    if (!c || c->transform == t) return;
    Clip copy = *c;
    copy.transform = t;
    m_project->edit(tr("Move / resize in the preview"), [&] { m_project->updateClip(copy); },
                    clipId + QStringLiteral("/preview/") + QString::number(m_dragSerial));
}

void PreviewInteractor::updateCursor(const QPointF& viewPos)
{
    const CompositeFramePtr f = m_host.lastFrame ? m_host.lastFrame() : nullptr;
    Qt::CursorShape shape = Qt::ArrowCursor;
    if (f && !(m_host.isPlaying && m_host.isPlaying())) {
        const QString sel = m_host.selectedClip ? m_host.selectedClip() : QString();
        const Layer* l = layerOf(*f, sel);
        const int corner = l ? cornerAt(*f, *l, viewPos) : -1;
        const QString hit = hitTest(*f, toCanvas(viewPos, *f));
        if (corner >= 0) shape = (corner % 2 == 0) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
        else if (!hit.isEmpty()) shape = hit == sel ? Qt::SizeAllCursor : Qt::PointingHandCursor;
    }
    m_view->setCursorShape(shape);
}

bool PreviewInteractor::eventFilter(QObject* watched, QEvent* event)
{
    if (!m_view || watched != m_view->inputObject()) return QObject::eventFilter(watched, event);
    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove && type != QEvent::MouseButtonRelease &&
        type != QEvent::MouseButtonDblClick && type != QEvent::Wheel)
        return QObject::eventFilter(watched, event);
    const CompositeFramePtr frame = m_host.lastFrame ? m_host.lastFrame() : nullptr;
    if (!frame || frame->canvasWidth <= 0) return QObject::eventFilter(watched, event);
    const CompositeFrame& f = *frame;
    const QString sel = m_host.selectedClip ? m_host.selectedClip() : QString();

    if (type == QEvent::Wheel) {
        auto* we = static_cast<QWheelEvent*>(event);
        const QPointF canvas = toCanvas(we->position(), f);
        const Layer* l = layerOf(f, sel);
        if (!l || hitTest(f, canvas).isEmpty() || !m_project->findClip(sel)) return false;
        const int delta = we->angleDelta().y() != 0 ? we->angleDelta().y() : we->angleDelta().x();
        if (delta == 0) return true;
        ClipTransform t = m_project->findClip(sel)->transform;
        const double steps = delta / 120.0;
        if (we->modifiers() & Qt::ShiftModifier) t.rotation = std::fmod(t.rotation + 5.0 * steps, 360.0);
        else t.scale = std::clamp(t.scale * std::pow(1.08, steps), 0.02, 20.0);
        applyTransform(sel, t);   // consecutive wheel steps merge into one undo step
        return true;
    }

    auto* me = static_cast<QMouseEvent*>(event);
    const QPointF pos = me->position();
    const QPointF canvas = toCanvas(pos, f);

    if (type == QEvent::MouseButtonDblClick && me->button() == Qt::LeftButton) {
        const QString hit = hitTest(f, canvas);
        const Clip* c = m_project->findClip(hit);
        if (c && c->isText() && m_host.editText) m_host.editText(hit);
        return true;
    }
    if (type == QEvent::MouseButtonPress && me->button() == Qt::LeftButton) {
        if (m_host.isPlaying && m_host.isPlaying()) return false;
        const Layer* selLayer = layerOf(f, sel);
        QString target;
        m_mode = Mode::None;
        if (selLayer && cornerAt(f, *selLayer, pos) >= 0) {
            target = sel;
            m_mode = Mode::Scale;
        } else {
            target = hitTest(f, canvas);
            if (!target.isEmpty()) m_mode = (me->modifiers() & Qt::AltModifier) ? Mode::Rotate : Mode::Move;
        }
        if (target.isEmpty()) return false;
        if (target != sel && m_host.selectClip) m_host.selectClip(target);
        const Clip* c = m_project->findClip(target);
        const Layer* l = layerOf(f, target);
        if (!c || !l) {
            m_mode = Mode::None;
            return true;
        }
        m_dragClip = target;
        m_start = c->transform;
        m_pressCanvas = canvas;
        m_centre = layerCentre(f, *l);
        ++m_dragSerial;
        return true;
    }
    if (type == QEvent::MouseMove) {
        if (m_mode == Mode::None) {
            updateCursor(pos);
            return false;
        }
        ClipTransform t = m_start;
        const bool free = me->modifiers() & Qt::ControlModifier;
        if (m_mode == Mode::Move) {
            const QPointF d = canvas - m_pressCanvas;
            t.x = m_start.x + d.x();
            t.y = m_start.y + d.y();
            if (!free) {   // snap to the centre lines
                const QRect r = viewRect(f);
                const double snap = 8.0 * f.canvasWidth / std::max(1, r.width());
                if (std::abs(t.x) < snap) t.x = 0;
                if (std::abs(t.y) < snap) t.y = 0;
            }
        } else if (m_mode == Mode::Scale) {
            const double d0 = len(m_pressCanvas - m_centre), d1 = len(canvas - m_centre);
            if (d0 > 1) t.scale = std::clamp(m_start.scale * d1 / d0, 0.02, 20.0);
        } else if (m_mode == Mode::Rotate) {
            const QPointF a = m_pressCanvas - m_centre, b = canvas - m_centre;
            double r = m_start.rotation + (std::atan2(b.y(), b.x()) - std::atan2(a.y(), a.x())) * 180.0 / kPi;
            r = std::fmod(r + 540.0, 360.0) - 180.0;
            if (!free) {   // snap to 0 / 45 / 90 ...
                const double q = std::round(r / 45.0) * 45.0;
                if (std::abs(r - q) < 3.0) r = q;
            }
            t.rotation = r;
        }
        applyTransform(m_dragClip, t);
        return true;
    }
    if (type == QEvent::MouseButtonRelease && me->button() == Qt::LeftButton && m_mode != Mode::None) {
        m_mode = Mode::None;
        ++m_dragSerial;
        updateCursor(pos);
        return true;
    }
    return QObject::eventFilter(watched, event);
}

CompositeFramePtr PreviewInteractor::decorate(const CompositeFramePtr& frame)
{
    if (!frame || !m_view || (m_host.isPlaying && m_host.isPlaying())) return frame;
    const QString sel = m_host.selectedClip ? m_host.selectedClip() : QString();
    const Layer* l = layerOf(*frame, sel);
    if (!l || !l->frame || l->frame->width <= 0) return frame;
    const int cw = frame->canvasWidth, ch = frame->canvasHeight;
    const QRect r = viewRect(*frame);
    if (r.width() <= 0) return frame;
    const double canvasPerView = double(cw) / r.width();

    // Size of the clip on the canvas, and an outline image of the same proportions.
    const double fit = std::min(double(cw) / l->frame->width, double(ch) / l->frame->height);
    const double W = l->frame->width * fit * l->transform.scale, H = l->frame->height * fit * l->transform.scale;
    if (W < 1 || H < 1) return frame;
    const double k = std::min(1.0, 4096.0 / std::max(W, H));   // image pixels per canvas pixel
    const QSize size(std::max(8, int(std::lround(W * k))), std::max(8, int(std::lround(H * k))));
    const int stroke = std::max(1, int(std::lround(2.0 * canvasPerView * k)));
    if (!m_outline || size != m_outlineSize || stroke != m_outlineStroke) {
        QImage img(size, QImage::Format_RGBA8888_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, false);
        const QColor gold(255, 200, 60);
        p.setPen(Qt::NoPen);
        p.setBrush(gold);
        p.drawRect(0, 0, size.width(), stroke);
        p.drawRect(0, size.height() - stroke, size.width(), stroke);
        p.drawRect(0, 0, stroke, size.height());
        p.drawRect(size.width() - stroke, 0, stroke, size.height());
        const int hs = std::min({int(std::lround(9.0 * canvasPerView * k)), size.width() / 3, size.height() / 3});
        p.setPen(QPen(gold, stroke));
        p.setBrush(Qt::white);
        for (const QPoint& c : {QPoint(0, 0), QPoint(size.width() - hs, 0), QPoint(0, size.height() - hs),
                                QPoint(size.width() - hs, size.height() - hs)})
            p.drawRect(QRect(c, QSize(hs, hs)).adjusted(stroke / 2, stroke / 2, -stroke / 2 - 1, -stroke / 2 - 1));
        p.end();
        const QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
        auto vf = std::make_shared<VideoFrame>();
        vf->width = rgba.width();
        vf->height = rgba.height();
        vf->rgba.resize(size_t(vf->width) * vf->height * 4);
        for (int y = 0; y < rgba.height(); ++y)
            std::memcpy(vf->rgba.data() + size_t(y) * vf->width * 4, rgba.constScanLine(y), size_t(vf->width) * 4);
        m_outline = vf;
        m_outlineSize = size;
        m_outlineStroke = stroke;
    }
    Layer o;
    o.frame = m_outline;
    o.transform.x = l->transform.x;
    o.transform.y = l->transform.y;
    o.transform.rotation = l->transform.rotation;
    const double fitO = std::min(double(cw) / size.width(), double(ch) / size.height());
    o.transform.scale = W / (size.width() * fitO);
    auto out = std::make_shared<CompositeFrame>(*frame);
    out->layers.push_back(std::move(o));
    return out;
}

} // namespace ve
