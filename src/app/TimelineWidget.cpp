#include "app/TimelineWidget.h"

#include "app/MediaBin.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace ve {

namespace {
constexpr int kHeaderW = 120;
constexpr int kRulerH = 28;
constexpr int kTrackH = 60;
constexpr int kSectionGap = 10;   // space between the video and audio sections
constexpr int kEdgeGrab = 6;
constexpr int kSnapPx = 8;
constexpr double kMinPxPerSec = 2.0;
constexpr double kMaxPxPerSec = 2000.0;

const QColor kBg(30, 30, 34);
const QColor kTrackA(38, 38, 43);
const QColor kTrackB(43, 43, 49);
const QColor kHeaderBg(50, 50, 57);
const QColor kRulerBg(46, 46, 52);
const QColor kPlayhead(235, 70, 70);

const QColor kVideoTag(70, 120, 200);
const QColor kAudioTag(60, 160, 105);

QColor clipColor(const MediaItem* m, bool audioTrack)
{
    if (!m) return QColor(110, 110, 110);
    if (audioTrack) return QColor(46, 128, 84);
    if (m->info.isImage) return QColor(138, 92, 184);
    return QColor(61, 111, 182);
}
} // namespace

TimelineWidget::TimelineWidget(Project* project, QWidget* parent)
    : QAbstractScrollArea(parent)
    , m_project(project)
{
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(kRulerH + kTrackH * 2 + 20);
    horizontalScrollBar()->setSingleStep(20);
    verticalScrollBar()->setSingleStep(kTrackH / 2);

    connect(m_project, &Project::changed, this, [this] {
        if (!m_selected.isEmpty() && !m_project->findClip(m_selected)) {
            m_selected.clear();
            emit selectionChanged({});
        }
        updateScrollBars();
        viewport()->update();
    });
    connect(m_project, &Project::mediaChanged, viewport(), qOverload<>(&QWidget::update));
    connect(m_project, &Project::transcriptsChanged, viewport(), qOverload<>(&QWidget::update)); // lyric text
    updateScrollBars();
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
double TimelineWidget::xForTime(TimeUs t) const
{
    return kHeaderW + usToSeconds(t) * m_pxPerSec - horizontalScrollBar()->value();
}

TimeUs TimelineWidget::timeForX(double x) const
{
    return std::max<TimeUs>(0, secondsToUs((x - kHeaderW + horizontalScrollBar()->value()) / m_pxPerSec));
}

int TimelineWidget::trackY(int track) const
{
    const int firstAudio = m_project->firstAudioTrack();
    const int gap = (track >= firstAudio && firstAudio > 0) ? kSectionGap : 0;
    return kRulerH + track * kTrackH + gap - verticalScrollBar()->value();
}

int TimelineWidget::trackAtY(int y) const
{
    if (y < kRulerH) return -1;
    for (int i = 0; i < m_project->trackCount(); ++i) {
        const int ty = trackY(i);
        if (y >= ty && y < ty + kTrackH) return i;
    }
    return -1;
}

int TimelineWidget::contentHeight() const
{
    const int firstAudio = m_project->firstAudioTrack();
    const bool both = firstAudio > 0 && firstAudio < m_project->trackCount();
    return m_project->trackCount() * kTrackH + (both ? kSectionGap : 0);
}

QRectF TimelineWidget::clipRect(int track, const Clip& c) const
{
    const double x0 = xForTime(c.start);
    const double x1 = xForTime(c.end());
    return QRectF(x0, trackY(track) + 4, std::max(2.0, x1 - x0), kTrackH - 8);
}

QRect TimelineWidget::muteRect(int track) const { return QRect(kHeaderW - 50, trackY(track) + 30, 20, 18); }
QRect TimelineWidget::hideRect(int track) const { return QRect(kHeaderW - 26, trackY(track) + 30, 20, 18); }
QRect TimelineWidget::soloRect(int track) const { return QRect(kHeaderW - 26, trackY(track) + 30, 20, 18); }

TimelineWidget::Hit TimelineWidget::hitTest(const QPoint& p) const
{
    Hit h;
    h.track = trackAtY(p.y());
    if (h.track < 0 || p.x() < kHeaderW) return h;
    for (const auto& c : m_project->tracks()[h.track].clips) {
        const QRectF r = clipRect(h.track, c);
        if (p.x() < r.left() - 1 || p.x() > r.right() + 1) continue;
        h.clipId = c.id;
        const int grab = std::min<int>(kEdgeGrab, int(r.width() / 3));
        if (p.x() <= r.left() + grab) h.part = Part::LeftEdge;
        else if (p.x() >= r.right() - grab) h.part = Part::RightEdge;
        else h.part = Part::Body;
        return h;
    }
    return h;
}

TimeUs TimelineWidget::snap(TimeUs t, const QStringList& ignoreClips, bool* snapped) const
{
    const TimeUs threshold = secondsToUs(kSnapPx / m_pxPerSec);
    TimeUs best = t;
    TimeUs bestDist = threshold + 1;
    auto consider = [&](TimeUs cand) {
        const TimeUs d = std::llabs(cand - t);
        if (d < bestDist) { bestDist = d; best = cand; }
    };
    consider(0);
    consider(m_playhead);
    for (const auto& tr : m_project->tracks())
        for (const auto& c : tr.clips) {
            if (ignoreClips.contains(c.id)) continue;
            consider(c.start);
            consider(c.end());
        }
    if (snapped) *snapped = bestDist <= threshold;
    return bestDist <= threshold ? best : snapToFrame(t, m_project->settings().fps);
}

void TimelineWidget::updateScrollBars()
{
    const double contentW = (usToSeconds(m_project->duration()) + 30.0) * m_pxPerSec;
    const int visibleW = std::max(1, viewport()->width() - kHeaderW);
    horizontalScrollBar()->setPageStep(visibleW);
    horizontalScrollBar()->setRange(0, std::max(0, int(contentW) - visibleW));
    const int contentH = contentHeight();
    const int visibleH = std::max(1, viewport()->height() - kRulerH);
    verticalScrollBar()->setPageStep(visibleH);
    verticalScrollBar()->setRange(0, std::max(0, contentH - visibleH));
}

void TimelineWidget::zoomAround(double factor, int anchorX)
{
    anchorX = std::max(anchorX, kHeaderW);
    const TimeUs anchorT = timeForX(anchorX);
    m_pxPerSec = std::clamp(m_pxPerSec * factor, kMinPxPerSec, kMaxPxPerSec);
    updateScrollBars();
    horizontalScrollBar()->setValue(int(usToSeconds(anchorT) * m_pxPerSec - (anchorX - kHeaderW)));
    viewport()->update();
}

void TimelineWidget::zoomIn() { zoomAround(1.5, int(xForTime(m_playhead))); }
void TimelineWidget::zoomOut() { zoomAround(1.0 / 1.5, int(xForTime(m_playhead))); }

void TimelineWidget::zoomToFit()
{
    const double secs = std::max(5.0, usToSeconds(m_project->duration()) * 1.05);
    m_pxPerSec = std::clamp((viewport()->width() - kHeaderW) / secs, kMinPxPerSec, kMaxPxPerSec);
    updateScrollBars();
    horizontalScrollBar()->setValue(0);
    viewport()->update();
}

void TimelineWidget::setPlayhead(TimeUs t)
{
    if (t == m_playhead) return;
    m_playhead = t;
    // Keep the playhead visible while playing.
    const double x = xForTime(t);
    if (m_drag == Drag::None && (x > viewport()->width() - 20 || x < kHeaderW)) {
        horizontalScrollBar()->setValue(int(usToSeconds(t) * m_pxPerSec - (viewport()->width() - kHeaderW) * 0.1));
    }
    viewport()->update();
}

void TimelineWidget::setMarkIn(TimeUs t)
{
    m_markIn = std::max<TimeUs>(0, t);
    if (m_markOut >= 0 && m_markOut <= m_markIn) m_markOut = -1;
    viewport()->update();
    emit marksChanged();
}

void TimelineWidget::setMarkOut(TimeUs t)
{
    m_markOut = std::max<TimeUs>(0, t);
    if (m_markIn >= 0 && m_markIn >= m_markOut) m_markIn = -1;
    viewport()->update();
    emit marksChanged();
}

void TimelineWidget::clearMarks()
{
    m_markIn = m_markOut = -1;
    viewport()->update();
    emit marksChanged();
}

void TimelineWidget::setLinkedSelection(bool on)
{
    m_linkedSelection = on;
    viewport()->update();
}

void TimelineWidget::setSelectedClip(const QString& id)
{
    if (id == m_selected) return;
    m_selected = id;
    viewport()->update();
    emit selectionChanged(id);
}

void TimelineWidget::resizeEvent(QResizeEvent* e)
{
    QAbstractScrollArea::resizeEvent(e);
    updateScrollBars();
}

void TimelineWidget::scrollContentsBy(int, int)
{
    viewport()->update();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
void TimelineWidget::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), kBg);
    paintTracks(p);
    paintRuler(p);
    paintHeaders(p);

    // Corner
    p.fillRect(QRect(0, 0, kHeaderW, kRulerH), kHeaderBg.darker(115));
    p.setPen(QColor(220, 220, 220));
    p.drawText(QRect(6, 0, kHeaderW - 6, kRulerH), Qt::AlignVCenter | Qt::AlignLeft,
               formatTimecode(m_playhead, m_project->settings().fps));

    // Playhead
    const double px = xForTime(m_playhead);
    if (px >= kHeaderW) {
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(kPlayhead, 1.5));
        p.drawLine(QPointF(px, kRulerH - 8), QPointF(px, viewport()->height()));
        QPainterPath tri;
        tri.moveTo(px - 6, kRulerH - 12);
        tri.lineTo(px + 6, kRulerH - 12);
        tri.lineTo(px, kRulerH - 2);
        tri.closeSubpath();
        p.fillPath(tri, kPlayhead);
    }

    // In / out range
    if (m_markIn >= 0 || m_markOut >= 0) {
        const double x0 = m_markIn >= 0 ? xForTime(m_markIn) : kHeaderW;
        const double x1 = m_markOut >= 0 ? xForTime(m_markOut) : viewport()->width();
        p.save();
        p.setClipRect(QRect(kHeaderW, 0, viewport()->width() - kHeaderW, viewport()->height()));
        p.fillRect(QRectF(x0, 0, x1 - x0, viewport()->height()), QColor(90, 160, 255, 38));
        p.fillRect(QRectF(x0, 0, x1 - x0, 5), QColor(90, 160, 255, 200));
        p.setPen(QPen(QColor(90, 160, 255), 1.5));
        if (m_markIn >= 0) p.drawLine(QPointF(x0, 0), QPointF(x0, viewport()->height()));
        if (m_markOut >= 0) p.drawLine(QPointF(x1, 0), QPointF(x1, viewport()->height()));
        p.restore();
    }

    // Snap guide
    if (m_snapLine >= 0 && m_dragActive) {
        const double sx = xForTime(m_snapLine);
        p.setPen(QPen(QColor(255, 210, 60), 1, Qt::DashLine));
        p.drawLine(QPointF(sx, kRulerH), QPointF(sx, viewport()->height()));
    }
}

void TimelineWidget::paintRuler(QPainter& p)
{
    const int w = viewport()->width();
    p.fillRect(QRect(kHeaderW, 0, w - kHeaderW, kRulerH), kRulerBg);

    static const double steps[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
    double major = steps[std::size(steps) - 1];
    for (double s : steps)
        if (s * m_pxPerSec >= 80) { major = s; break; }
    const double minor = major / 5.0;

    const double t0 = std::max(0.0, usToSeconds(timeForX(kHeaderW)) - major);
    const double t1 = usToSeconds(timeForX(w)) + major;
    p.setPen(QColor(150, 150, 160));
    for (double t = std::floor(t0 / minor) * minor; t <= t1; t += minor) {
        const double x = xForTime(secondsToUs(t));
        if (x < kHeaderW) continue;
        const bool isMajor = std::fabs(std::remainder(t, major)) < minor * 0.01;
        p.drawLine(QPointF(x, isMajor ? 10 : 20), QPointF(x, kRulerH));
        if (isMajor) {
            const int total = int(std::lround(t * 10));
            const int mins = total / 600;
            const double secs = (total % 600) / 10.0;
            const QString label = major < 1 ? QString::asprintf("%d:%04.1f", mins, secs)
                                            : QString::asprintf("%d:%02d", mins, int(secs));
            p.drawText(QPointF(x + 3, 11), label);
        }
    }
    p.setPen(QColor(20, 20, 24));
    p.drawLine(kHeaderW, kRulerH - 1, w, kRulerH - 1);
}

void TimelineWidget::paintTracks(QPainter& p)
{
    const int w = viewport()->width();
    p.save();
    p.setClipRect(QRect(kHeaderW, kRulerH, w - kHeaderW, viewport()->height() - kRulerH));

    const auto& tracks = m_project->tracks();
    const QStringList partners =
        (m_selected.isEmpty() || !m_linkedSelection) ? QStringList() : m_project->linkedClips(m_selected);
    // Transcript lines, drawn as text along the audio clips
    QHash<QString, std::vector<TimelineLine>> lyrics;
    for (auto& l : m_project->transcriptLines()) lyrics[l.clipId].push_back(std::move(l));
    const int firstAudio = m_project->firstAudioTrack();
    if (firstAudio > 0 && firstAudio < int(tracks.size())) // divider between the video and audio sections
        p.fillRect(QRect(kHeaderW, trackY(firstAudio) - kSectionGap, w - kHeaderW, kSectionGap), QColor(22, 22, 25));
    for (int ti = 0; ti < int(tracks.size()); ++ti) {
        const int y = trackY(ti);
        const bool audio = tracks[ti].isAudio();
        QColor bg = ti % 2 ? kTrackB : kTrackA;
        if (audio) bg = QColor(bg.red() - 2, bg.green() + 3, bg.blue() - 1);
        p.fillRect(QRect(kHeaderW, y, w - kHeaderW, kTrackH), bg);
        p.setPen(QColor(25, 25, 28));
        p.drawLine(kHeaderW, y + kTrackH - 1, w, y + kTrackH - 1);

        for (const auto& c : tracks[ti].clips) {
            const QRectF r = clipRect(ti, c);
            if (r.right() < kHeaderW || r.left() > w) continue;
            const MediaItem* m = m_project->mediaById(c.mediaId);
            QColor col = clipColor(m, audio);
            if ((!audio && tracks[ti].hidden) || (audio && tracks[ti].muted)) col = col.darker(170);

            p.setRenderHint(QPainter::Antialiasing, true);
            QPainterPath path;
            path.addRoundedRect(r, 4, 4);
            p.fillPath(path, col);

            // Waveform (audio) / thumbnail strip (video)
            if (audio) paintWaveform(p, r, c, m, col);
            if (audio && lyrics.contains(c.id) && r.height() > 30) {
                p.save();
                QFont f = p.font();
                f.setPointSizeF(f.pointSizeF() * 0.85);
                p.setFont(f);
                const QRectF band(r.left(), r.bottom() - 15, r.width(), 14);
                p.setClipRect(band.intersected(QRectF(kHeaderW, 0, w - kHeaderW, viewport()->height())));
                for (const auto& l : lyrics[c.id]) {
                    const double x0 = std::max(r.left() + 2, xForTime(l.start)), x1 = std::min(r.right() - 2, xForTime(l.end));
                    if (x1 - x0 < 12) continue;
                    const QRectF tr(x0, band.top(), x1 - x0, band.height());
                    p.fillRect(tr, QColor(0, 0, 0, 120));
                    p.setPen(QColor(235, 235, 235));
                    p.drawText(tr.adjusted(3, 0, -2, 0), Qt::AlignVCenter | (l.text.isRightToLeft() ? Qt::AlignRight : Qt::AlignLeft),
                               p.fontMetrics().elidedText(l.text, Qt::ElideRight, int(tr.width() - 5)));
                }
                p.restore();
            }
            if (!audio && m && !m->thumbnail.isNull() && m->info.hasVideo && r.width() > 40) {
                const int th = int(r.height()) - 18;
                const int tw = th * m->thumbnail.width() / std::max(1, m->thumbnail.height());
                const QRectF tr(r.left() + 2, r.top() + 16, std::min<double>(tw, r.width() - 4), th);
                p.save();
                p.setClipPath(path, Qt::IntersectClip);
                p.drawImage(tr, m->thumbnail, QRectF(0, 0, tr.width() * m->thumbnail.width() / std::max(1, tw), m->thumbnail.height()));
                p.restore();
            }

            // Fades
            p.setPen(QPen(QColor(255, 255, 255, 160), 1));
            if (c.fadeIn > 0) {
                const double fx = xForTime(c.start + c.fadeIn);
                p.drawLine(QPointF(r.left(), r.bottom()), QPointF(fx, r.top()));
            }
            if (c.fadeOut > 0) {
                const double fx = xForTime(c.end() - c.fadeOut);
                p.drawLine(QPointF(fx, r.top()), QPointF(r.right(), r.bottom()));
            }

            // Border + label (the selected clip in gold, clips linked to it lighter)
            const bool sel = c.id == m_selected;
            const bool partner = !sel && partners.contains(c.id);
            p.setPen(QPen(sel ? QColor(255, 200, 60) : partner ? QColor(255, 225, 150) : col.lighter(140),
                          sel ? 2.0 : partner ? 1.5 : 1.0, partner ? Qt::DashLine : Qt::SolidLine));
            p.drawPath(path);
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setPen(Qt::white);
            const QRectF textR = r.adjusted(5, 1, -4, 0);
            QString label = c.linkId.isEmpty() ? c.name : QStringLiteral("\u26AD ") + c.name; // linked mark
            if (c.freeze) label += tr("  [freeze]");
            if (c.reverse) label += tr("  [reverse]");
            if (c.speed != 1.0) label += QStringLiteral("  [%1x]").arg(c.speed, 0, 'g', 3);
            if (!c.speedCurve.empty()) label += tr("  [ramp]");
            p.drawText(textR, Qt::AlignTop | Qt::AlignLeft,
                       p.fontMetrics().elidedText(label, Qt::ElideRight, int(textR.width())));
        }
    }

    // Drop preview
    if (m_dropTrack >= 0) {
        const double x0 = xForTime(m_dropTime);
        const double x1 = xForTime(m_dropTime + m_dropDuration);
        p.setPen(QPen(QColor(255, 255, 255, 200), 1, Qt::DashLine));
        p.setBrush(QColor(255, 255, 255, 40));
        p.drawRect(QRectF(x0, trackY(m_dropTrack) + 4, std::max(2.0, x1 - x0), kTrackH - 8));
        p.setBrush(Qt::NoBrush);
    }
    p.restore();
}

void TimelineWidget::paintHeaders(QPainter& p)
{
    p.save();
    p.setClipRect(QRect(0, kRulerH, kHeaderW, viewport()->height() - kRulerH));
    const auto& tracks = m_project->tracks();
    const int firstAudio = m_project->firstAudioTrack();
    if (firstAudio > 0 && firstAudio < int(tracks.size())) {
        const QRect gap(0, trackY(firstAudio) - kSectionGap, kHeaderW, kSectionGap);
        p.fillRect(gap, QColor(22, 22, 25));
    }
    for (int ti = 0; ti < int(tracks.size()); ++ti) {
        const int y = trackY(ti);
        const bool audio = tracks[ti].isAudio();
        p.fillRect(QRect(0, y, kHeaderW, kTrackH), kHeaderBg);
        p.fillRect(QRect(0, y, 4, kTrackH - 1), audio ? kAudioTag : kVideoTag); // section colour tag
        p.setPen(QColor(25, 25, 28));
        p.drawLine(0, y + kTrackH - 1, kHeaderW, y + kTrackH - 1);
        p.drawLine(kHeaderW - 1, y, kHeaderW - 1, y + kTrackH);
        p.setPen(QColor(225, 225, 230));
        p.drawText(QRect(10, y + 6, kHeaderW - 16, 20), Qt::AlignLeft | Qt::AlignVCenter, tracks[ti].name);
        p.setPen(QColor(140, 140, 150));
        p.drawText(QRect(10, y + 30, 40, 18), Qt::AlignLeft | Qt::AlignVCenter, audio ? tr("Audio") : tr("Video"));

        auto toggle = [&](const QRect& r, const QString& label, bool on, const QColor& onColor) {
            p.fillRect(r, on ? onColor : QColor(70, 70, 78));
            p.setPen(on ? Qt::black : QColor(200, 200, 205));
            p.drawText(r, Qt::AlignCenter, label);
        };
        if (audio) {
            toggle(muteRect(ti), QStringLiteral("M"), tracks[ti].muted, QColor(240, 180, 60));
            toggle(soloRect(ti), QStringLiteral("S"), tracks[ti].solo, QColor(212, 172, 13));
        } else {
            toggle(hideRect(ti), QStringLiteral("H"), tracks[ti].hidden, QColor(120, 170, 240));
        }
    }
    p.restore();
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------
void TimelineWidget::mousePressEvent(QMouseEvent* e)
{
    setFocus();
    if (e->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(e);
        return;
    }
    const QPoint pos = e->position().toPoint();
    m_pressPos = pos;
    m_dragActive = false;

    if (pos.y() < kRulerH && pos.x() >= kHeaderW) {
        m_drag = Drag::Scrub;
        emit seekRequested(snapToFrame(timeForX(pos.x()), m_project->settings().fps));
        return;
    }

    if (pos.x() < kHeaderW) {
        const int ti = trackAtY(pos.y());
        if (ti < 0) return;
        const Track& t = m_project->tracks()[ti];
        if (t.isAudio() && muteRect(ti).contains(pos))
            m_project->edit(tr("Toggle mute"), [&] { m_project->setTrackMuted(ti, !t.muted); });
        else if (t.isAudio() && soloRect(ti).contains(pos))
            m_project->edit(tr("Toggle solo"), [&] { m_project->setTrackSolo(ti, !t.solo); });
        else if (!t.isAudio() && hideRect(ti).contains(pos))
            m_project->edit(tr("Toggle visibility"), [&] { m_project->setTrackHidden(ti, !t.hidden); });
        return;
    }

    const Hit h = hitTest(pos);
    if (h.clipId.isEmpty()) {
        setSelectedClip({});
        m_drag = Drag::Scrub;
        emit seekRequested(snapToFrame(timeForX(pos.x()), m_project->settings().fps));
        return;
    }

    setSelectedClip(h.clipId);
    const Clip* c = m_project->findClip(h.clipId, &m_dragOrigTrack);
    if (!c) return;
    m_dragClip = h.clipId;
    m_dragLinked = m_linkedSelection && !(e->modifiers() & Qt::AltModifier); // Alt: edit only this clip
    m_dragGroup = QStringList{h.clipId} + (m_dragLinked ? m_project->linkedClips(h.clipId) : QStringList());
    m_dragOrigStart = c->start;
    m_dragOrigIn = c->in;
    m_dragOrigDur = c->duration;
    m_dragBefore = m_project->captureState();
    m_drag = h.part == Part::LeftEdge ? Drag::TrimLeft : h.part == Part::RightEdge ? Drag::TrimRight : Drag::Move;
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();

    if (m_drag == Drag::None) {
        const Hit h = hitTest(pos);
        viewport()->setCursor(h.part == Part::LeftEdge || h.part == Part::RightEdge ? Qt::SizeHorCursor
                              : h.part == Part::Body ? Qt::OpenHandCursor : Qt::ArrowCursor);
        return;
    }

    if (m_drag == Drag::Scrub) {
        emit seekRequested(snapToFrame(timeForX(pos.x()), m_project->settings().fps));
        return;
    }

    if (!m_dragActive) {
        if ((pos - m_pressPos).manhattanLength() < QApplication::startDragDistance()) return;
        m_dragActive = true;
    }

    const TimeUs dt = timeForX(pos.x()) - timeForX(m_pressPos.x());
    // Re-apply from the pre-drag state so passing over other clips is non-destructive.
    m_project->restoreState(m_dragBefore);
    m_snapLine = -1;
    bool snapped = false;

    if (m_drag == Drag::Move) {
        viewport()->setCursor(Qt::ClosedHandCursor);
        TimeUs start = std::max<TimeUs>(0, m_dragOrigStart + dt);
        const TimeUs s1 = snap(start, m_dragGroup, &snapped);
        if (snapped) {
            m_snapLine = s1;
            start = s1;
        } else {
            const TimeUs e1 = snap(start + m_dragOrigDur, m_dragGroup, &snapped);
            if (snapped) {
                m_snapLine = e1;
                start = e1 - m_dragOrigDur;
            } else {
                start = s1;
            }
        }
        // Stay within the clip's own section: video clips on video tracks, audio on audio.
        int track = trackAtY(pos.y());
        const bool audio = m_project->tracks()[m_dragOrigTrack].isAudio();
        if (track < 0 || m_project->tracks()[track].isAudio() != audio) track = m_dragOrigTrack;
        m_project->moveClip(m_dragClip, track, std::max<TimeUs>(0, start), m_dragLinked);
    } else if (m_drag == Drag::TrimLeft) {
        TimeUs start = snap(std::max<TimeUs>(0, m_dragOrigStart + dt), m_dragGroup, &snapped);
        start = std::min(start, m_dragOrigStart + m_dragOrigDur - frameDurationUs(m_project->settings().fps));
        if (snapped) m_snapLine = start;
        m_project->trimEdges(m_dragClip, start, m_dragOrigStart + m_dragOrigDur, m_dragLinked);
    } else if (m_drag == Drag::TrimRight) {
        TimeUs end = snap(m_dragOrigStart + m_dragOrigDur + dt, m_dragGroup, &snapped);
        end = std::max(end, m_dragOrigStart + frameDurationUs(m_project->settings().fps));
        if (snapped) m_snapLine = end;
        m_project->trimEdges(m_dragClip, m_dragOrigStart, end, m_dragLinked);
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    if (m_dragActive && (m_drag == Drag::Move || m_drag == Drag::TrimLeft || m_drag == Drag::TrimRight)) {
        const QString text = m_drag == Drag::Move ? tr("Move clip") : tr("Trim clip");
        m_project->commitUndo(text, m_dragBefore);
    }
    m_drag = Drag::None;
    m_dragActive = false;
    m_snapLine = -1;
    m_dragBefore = {};
    m_dragGroup.clear();
    viewport()->setCursor(Qt::ArrowCursor);
    viewport()->update();
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    // Double-click on a clip jumps the playhead to it.
    const Hit h = hitTest(e->position().toPoint());
    if (const Clip* c = h.clipId.isEmpty() ? nullptr : m_project->findClip(h.clipId))
        emit seekRequested(c->start);
}

void TimelineWidget::wheelEvent(QWheelEvent* e)
{
    const int delta = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
    if (e->modifiers() & Qt::ControlModifier) {
        zoomAround(std::pow(1.0015, delta), int(e->position().x()));
        e->accept();
        return;
    }
    const bool horizontal = (e->modifiers() & Qt::ShiftModifier) || e->angleDelta().x() != 0 ||
                            verticalScrollBar()->maximum() == 0;
    if (horizontal) {
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta);
        e->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(e);
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* e)
{
    const QPoint pos = e->pos();
    QMenu menu(this);

    if (pos.x() < kHeaderW) {
        const int ti = trackAtY(pos.y());
        menu.addAction(tr("Add Video Track"), this, [this] {
            m_project->edit(tr("Add video track"), [this] { m_project->addTrack({}, TrackKind::Video); });
        });
        menu.addAction(tr("Add Audio Track"), this, [this] {
            m_project->edit(tr("Add audio track"), [this] { m_project->addTrack({}, TrackKind::Audio); });
        });
        if (ti >= 0 && m_project->trackCount() > 1) {
            menu.addAction(tr("Remove Track"), this, [this, ti] {
                m_project->edit(tr("Remove track"), [this, ti] { m_project->removeTrack(ti); });
            });
        }
        menu.exec(e->globalPos());
        return;
    }

    const Hit h = hitTest(pos);
    if (h.clipId.isEmpty()) {
        if (h.track < 0) return;
        const int track = h.track;
        const TimeUs at = timeForX(pos.x());
        bool clipAfter = false;
        for (const auto& c : m_project->tracks()[track].clips) clipAfter |= c.start > at;
        QAction* close = menu.addAction(tr("Close Gap"), this, [this, track, at] {
            m_project->edit(tr("Close gap"), [&] { m_project->closeGap(track, at); });
        });
        close->setEnabled(clipAfter);
        if (hasRange()) {
            menu.addAction(tr("Cut Marked Range (Close Gap)"), this, [this] {
                m_project->edit(tr("Cut range"), [&] { m_project->removeRange(m_markIn, m_markOut, true); });
            });
        }
        menu.exec(e->globalPos());
        return;
    }
    setSelectedClip(h.clipId);
    const QString id = h.clipId;
    const bool linked = !m_project->linkedClips(id).isEmpty();
    const bool group = linked && m_linkedSelection;
    menu.addAction(tr("Split at Playhead"), this, [this, id, group] {
        m_project->edit(tr("Split clip"), [&] { m_project->splitClip(id, m_playhead, group); });
    });
    if (linked) {
        menu.addAction(tr("Unlink Audio and Video") + QStringLiteral("\tCtrl+L"), this, [this, id] {
            m_project->edit(tr("Unlink audio and video"), [&] { m_project->unlinkClip(id); });
            viewport()->update();
        });
    } else if (const QString mate = m_project->linkCandidate(id); !mate.isEmpty()) {
        menu.addAction(tr("Link Audio and Video") + QStringLiteral("\tCtrl+L"), this, [this, id, mate] {
            m_project->edit(tr("Link audio and video"), [&] { m_project->linkClips({id, mate}); });
        });
    }
    menu.addAction(tr("Duplicate") + QStringLiteral("\tCtrl+D"), this, [this, id, group] {
        QString copy;
        m_project->edit(tr("Duplicate clip"), [&] { copy = m_project->duplicateClip(id, group); });
        if (!copy.isEmpty()) setSelectedClip(copy);
    });
    int clipTrack = -1;
    const Clip* clip = m_project->findClip(id, &clipTrack);
    if (clip && !m_project->tracks()[clipTrack].isAudio() && clip->contains(m_playhead) && !clip->freeze) {
        menu.addAction(tr("Freeze Frame at Playhead") + QStringLiteral("\tShift+F"), this, [this, id] {
            QString still;
            m_project->edit(tr("Freeze frame"), [&] { still = m_project->freezeFrame(id, m_playhead, 2 * kUsPerSecond); });
            if (!still.isEmpty()) setSelectedClip(still);
        });
    }
    if (clip && !clip->freeze) {
        QAction* rev = menu.addAction(tr("Reverse") + QStringLiteral("\tCtrl+R"), this, [this, id, group] {
            const Clip* c = m_project->findClip(id);
            const bool on = c && !c->reverse;
            m_project->edit(on ? tr("Reverse clip") : tr("Play forward"), [&] { m_project->setClipReverse(id, on, group); });
        });
        rev->setCheckable(true);
        rev->setChecked(clip->reverse);
        QMenu* speed = menu.addMenu(tr("Speed"));
        for (double s : {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 4.0, 8.0}) {
            QAction* a = speed->addAction(QStringLiteral("%1x").arg(s), this, [this, id, s, group] {
                const Clip* c = m_project->findClip(id);
                if (!c) return;
                const auto curve = c->speedCurve;
                const QString preset = c->curvePreset;
                m_project->edit(tr("Change speed"), [&] { m_project->setClipSpeed(id, s, curve, preset, group); });
            });
            a->setCheckable(true);
            a->setChecked(std::fabs(clip->speed - s) < 1e-6);
        }
        QMenu* curve = menu.addMenu(tr("Speed Curve"));
        const QList<QPair<QString, QString>> curves = {
            {QStringLiteral("none"), tr("None (constant)")}, {QStringLiteral("ease-in"), tr("Ease In (speed up)")},
            {QStringLiteral("ease-out"), tr("Ease Out (slow down)")}, {QStringLiteral("ease-in-out"), tr("Slow - Fast - Slow")},
            {QStringLiteral("bullet"), tr("Bullet (fast - slow-mo - fast)")}, {QStringLiteral("hero"), tr("Hero (slow-motion moment)")}};
        for (const auto& [key, label] : curves) {
            QAction* a = curve->addAction(label, this, [this, id, key, group] {
                const Clip* c = m_project->findClip(id);
                if (!c) return;
                const double sp = c->speed;
                m_project->edit(tr("Speed curve"), [&] {
                    m_project->setClipSpeed(id, sp, speedCurvePreset(key), key == QLatin1String("none") ? QString() : key, group);
                });
            });
            a->setCheckable(true);
            a->setChecked(key == QLatin1String("none") ? clip->speedCurve.empty() : clip->curvePreset == key);
        }
    }
    {
        // Voice / music separation for audio clips and videos with linked sound
        bool hasSound = clip && m_project->tracks()[clipTrack].isAudio() && !clip->freeze;
        if (clip && !hasSound)
            for (const QString& p : m_project->linkedClips(id)) {
                int pt = -1;
                if (m_project->findClip(p, &pt) && m_project->tracks()[pt].isAudio()) hasSound = true;
            }
        if (hasSound) {
            menu.addSeparator();
            menu.addAction(tr("Separate Voice and Music..."), this, [this, id] { emit separateAudioRequested(id); });
            menu.addAction(tr("Transcribe Speech / Lyrics..."), this, [this, id] { emit transcribeRequested(id); });
            menu.addAction(tr("Auto Captions"), this, [this, id] { emit autoCaptionsRequested(id); });
        }
    }
    menu.addSeparator();
    menu.addAction(tr("Reset Effects"), this, [this, id] {
        if (const Clip* c = m_project->findClip(id)) {
            Clip copy = *c;
            copy.effects = {};
            copy.transform = {};
            m_project->edit(tr("Reset effects"), [&] { m_project->updateClip(copy); });
        }
    });
    menu.addSeparator();
    menu.addAction(group ? tr("Delete (with Linked Clip)") : tr("Delete"), this, [this, id, group] {
        m_project->edit(tr("Delete clip"), [&] { m_project->removeClip(id, group); });
    });
    menu.addAction(tr("Ripple Delete (Close Gap)") + QStringLiteral("\tShift+Del"), this, [this, id, group] {
        m_project->edit(tr("Ripple delete"), [&] { m_project->rippleDelete(id, group); });
    });
    if (group)
        menu.addAction(tr("Delete Only This Clip"), this, [this, id] {
            m_project->edit(tr("Delete clip"), [&] { m_project->removeClip(id, false); });
        });
    menu.exec(e->globalPos());
}

// ---------------------------------------------------------------------------
// Drag & drop from the media bin / file manager
// ---------------------------------------------------------------------------
void TimelineWidget::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasFormat(QString::fromLatin1(kMediaMimeType)) || e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* e)
{
    const QPoint pos = e->position().toPoint();
    int track = trackAtY(pos.y());
    if (track < 0) track = m_project->trackCount() - 1;
    m_dropTrack = track;
    m_dropTime = snap(timeForX(pos.x()), QStringList());
    m_dropDuration = 5 * kUsPerSecond;
    const QByteArray id = e->mimeData()->data(QString::fromLatin1(kMediaMimeType));
    if (const MediaItem* m = id.isEmpty() ? nullptr : m_project->mediaById(QString::fromUtf8(id)))
        if (!m->info.isImage && m->info.duration > 0) m_dropDuration = m->info.duration;
    e->acceptProposedAction();
    viewport()->update();
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent*)
{
    m_dropTrack = -1;
    viewport()->update();
}

void TimelineWidget::dropEvent(QDropEvent* e)
{
    const int track = m_dropTrack >= 0 ? m_dropTrack : std::max(0, m_project->trackCount() - 1);
    const TimeUs at = m_dropTime;
    m_dropTrack = -1;

    const QByteArray id = e->mimeData()->data(QString::fromLatin1(kMediaMimeType));
    if (!id.isEmpty()) {
        QString clipId;
        m_project->edit(tr("Add clip"), [&] {
            clipId = m_project->addClipFromMedia(QString::fromUtf8(id), track, at, m_linkOnAdd);
        });
        if (!clipId.isEmpty()) setSelectedClip(clipId);
        e->acceptProposedAction();
    } else if (e->mimeData()->hasUrls()) {
        QStringList paths;
        for (const QUrl& u : e->mimeData()->urls())
            if (u.isLocalFile()) paths << u.toLocalFile();
        emit filesDropped(paths, track, at);
        e->acceptProposedAction();
    }
    viewport()->update();
}

// Audio clips: the source's peaks (dB scale), following the clip volume and
// fades, mirrored around the middle of the clip.
void TimelineWidget::paintWaveform(QPainter& p, const QRectF& r, const Clip& c, const MediaItem* m, const QColor& col)
{
    if (!m || !m->waveform || m->waveform->empty() || r.width() < 3 || c.freeze) return;
    const auto& peaks = *m->waveform;
    const QRectF area = r.adjusted(1, 15, -1, -2);
    const double mid = area.center().y();
    const double half = area.height() / 2.0;
    const int x0 = std::max(int(std::ceil(area.left())), kHeaderW);
    const int x1 = std::min(int(area.right()), viewport()->width());
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(col.lighter(175));
    for (int x = x0; x <= x1; ++x) {
        const TimeUs ta = timeForX(x), tb = timeForX(x + 1);
        double sa = usToSeconds(c.sourceTimeAt(ta)) * kWaveformBucketsPerSecond;
        double sb = usToSeconds(c.sourceTimeAt(tb)) * kWaveformBucketsPerSecond;
        if (sb < sa) std::swap(sa, sb);                          // reversed clip
        const auto b0 = std::clamp<std::int64_t>(std::int64_t(sa), 0, std::int64_t(peaks.size()) - 1);
        const auto b1 = std::clamp<std::int64_t>(std::int64_t(std::ceil(sb)), b0 + 1, std::int64_t(peaks.size()));
        float peak = 0.0f;
        for (auto b = b0; b < b1; ++b) peak = std::max(peak, peaks[size_t(b)]);
        // Decibel scale (-48..0 dBFS) so quiet passages stay visible, as in most editors.
        const double level = peak * std::min(1.0, c.volume * c.fadeFactor(ta));
        const double db = level > 0 ? 20.0 * std::log10(level) : -100.0;
        const double h = std::max(0.5, std::clamp((db + 48.0) / 48.0, 0.0, 1.0) * half);
        p.drawLine(QPointF(x, mid - h), QPointF(x, mid + h));
    }
    p.restore();
}

} // namespace ve
