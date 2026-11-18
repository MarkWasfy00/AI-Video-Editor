// Drives the preview's direct manipulation (PreviewInteractor) with synthetic
// mouse / wheel events: select, move (with centre snapping), resize from a
// corner, rotate, wheel-scale, double-click to edit text, one undo step per drag.
#include "app/PreviewInteractor.h"
#include "core/Project.h"
#include "media/FrameSource.h"
#include "render/OffscreenRenderer.h"

#include <QApplication>
#include <QMouseEvent>
#include <QUndoStack>
#include <QWheelEvent>
#include <QWidget>

#include <cmath>
#include <cstdio>

using namespace ve;

static int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        } else {                                                                       \
            std::printf("  ok   %s\n", #cond);                                         \
        }                                                                              \
    } while (0)

class FakeView : public PreviewView {
public:
    FakeView() { m_w.resize(640, 360); }
    QWidget* widget() override { return &m_w; }
    void setFrame(CompositeFramePtr f) override { last = std::move(f); }
    RenderBackend backend() const override { return RenderBackend::OpenGL; }
    QString backendDescription() const override { return QStringLiteral("fake"); }
    CompositeFramePtr last;

private:
    QWidget m_w;
};

static void mouse(QWidget* w, QEvent::Type t, QPointF p, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    const Qt::MouseButton b = t == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const Qt::MouseButtons bs = t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent e(t, p, w->mapToGlobal(p.toPoint()), b, bs, mods);
    QApplication::sendEvent(w, &e);
}

static void drag(QWidget* w, QPointF from, QPointF to, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    mouse(w, QEvent::MouseButtonPress, from, mods);
    for (int i = 1; i <= 4; ++i) mouse(w, QEvent::MouseMove, from + (to - from) * (i / 4.0), mods);
    mouse(w, QEvent::MouseButtonRelease, to, mods);
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    Project project;                                       // 1920x1080 canvas
    TextData d;
    d.text = QStringLiteral("DRAG ME");
    d.animation.in = d.animation.out = QStringLiteral("none");
    QString id;
    project.edit(QStringLiteral("Add text"), [&] { id = project.addTextClip(-1, 0, 4 * kUsPerSecond, d); });
    const TimeUs t = kUsPerSecond;

    FrameSource source;
    QString selected, edited;
    PreviewInteractor ia(&project,
                         {[&] { return selected; }, [&](const QString& s) { selected = s; },
                          [&](const QString& s) { edited = s; }, [] { return false; },
                          [&] { return source.build(project.snapshot(), t); }});
    FakeView view;
    ia.attach(&view);
    QWidget* w = view.widget();
    const auto tr = [&] { return project.findClip(id)->transform; };
    const QPointF centre(320, 180);                        // view is 1/3 of the canvas

    std::printf("== select + move\n");
    const int undoBefore = project.undoStack()->count();
    drag(w, centre, centre + QPointF(64, -20));
    CHECK(selected == id);
    CHECK(std::abs(tr().x - 192) < 1 && std::abs(tr().y + 60) < 1);
    CHECK(project.undoStack()->count() == undoBefore + 1);   // one drag = one undo step

    std::printf("== snap to the centre lines (Ctrl = free)\n");
    drag(w, centre + QPointF(64, -20), centre + QPointF(2, 1));
    CHECK(tr().x == 0 && tr().y == 0);
    drag(w, centre, centre + QPointF(2, 1), Qt::ControlModifier);
    CHECK(std::abs(tr().x - 6) < 1 && std::abs(tr().y - 3) < 1);
    drag(w, centre + QPointF(2, 1), centre);               // back (snaps)
    CHECK(tr().x == 0 && tr().y == 0);

    std::printf("== resize from a corner\n");
    {
        const CompositeFramePtr f = source.build(project.snapshot(), t);
        const Layer* l = nullptr;
        for (const Layer& x : f->layers)
            if (x.clipId == id) l = &x;
        CHECK(l != nullptr);
        if (l) {
            const QPointF corner = layerModelMatrix(*l, f->canvasWidth, f->canvasHeight).map(QPointF(1, 1)) / 3.0;
            drag(w, corner, centre + (corner - centre) * 2.0);
            CHECK(std::abs(tr().scale - 2.0) < 0.05);
        }
    }

    std::printf("== rotate (Alt+drag, snaps to 45 degrees)\n");
    drag(w, centre + QPointF(100, 0), centre + QPointF(0, 100), Qt::AltModifier);
    CHECK(std::abs(tr().rotation - 90) < 0.01);

    std::printf("== wheel: scale, Shift+wheel: rotate\n");
    {
        const double s0 = tr().scale;
        QWheelEvent we(centre, w->mapToGlobal(centre.toPoint()), {}, QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                       Qt::NoScrollPhase, false);
        QApplication::sendEvent(w, &we);
        CHECK(std::abs(tr().scale - s0 * 1.08) < 1e-6);
        QWheelEvent we2(centre, w->mapToGlobal(centre.toPoint()), {}, QPoint(0, 120), Qt::NoButton, Qt::ShiftModifier,
                        Qt::NoScrollPhase, false);
        QApplication::sendEvent(w, &we2);
        CHECK(std::abs(tr().rotation - 95) < 0.01);
    }

    std::printf("== click on empty canvas does nothing; double-click edits text\n");
    {
        const ClipTransform before = tr();
        drag(w, QPointF(5, 5), QPointF(40, 40));
        CHECK(tr() == before);
        mouse(w, QEvent::MouseButtonDblClick, centre);
        CHECK(edited == id);
    }

    std::printf("== selection outline\n");
    {
        const CompositeFramePtr f = source.build(project.snapshot(), t);
        const CompositeFramePtr deco = ia.decorate(f);
        CHECK(deco->layers.size() == f->layers.size() + 1 && deco->layers.back().clipId.isEmpty());
        if (argc > 1) {   // optional: save the outlined frame for a visual check
            OffscreenRenderer r;
            if (r.initialize(f->canvasWidth, f->canvasHeight)) r.render(*deco).save(QString::fromLocal8Bit(argv[1]));
        }
        selected.clear();
        CHECK(ia.decorate(f) == f);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
