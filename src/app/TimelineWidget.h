#pragma once

#include "core/Project.h"

#include <QAbstractScrollArea>
#include <QJsonArray>

namespace ve {

// Multi-track timeline: ruler, a video section (V tracks, thumbnails) above an
// audio section (A tracks, waveforms), clips with move / trim / snapping,
// playhead scrubbing, zoom and drag & drop. Linked clips (a video and its
// audio) are edited together; hold Alt to edit one of them on its own.
class TimelineWidget : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit TimelineWidget(Project* project, QWidget* parent = nullptr);

    QString selectedClip() const { return m_selected; }
    void setSelectedClip(const QString& id);
    TimeUs playhead() const { return m_playhead; }
    int trackAtY(int y) const;          // viewport y -> track index or -1

    // Linked selection off: every edit acts on the clicked clip only (as if Alt
    // were held). Link on add off: a video's sound is added unlinked.
    void setLinkedSelection(bool on);
    bool linkedSelection() const { return m_linkedSelection; }
    void setLinkOnAdd(bool on) { m_linkOnAdd = on; }
    bool linkOnAdd() const { return m_linkOnAdd; }

    // In / out marks for range edits (Cut Range). -1 = not set.
    TimeUs markIn() const { return m_markIn; }
    TimeUs markOut() const { return m_markOut; }
    void setMarkIn(TimeUs t);
    void setMarkOut(TimeUs t);
    void clearMarks();
    bool hasRange() const { return m_markIn >= 0 && m_markOut > m_markIn; }

public slots:
    void setPlayhead(ve::TimeUs t);
    void zoomIn();
    void zoomOut();
    void zoomToFit();

signals:
    void marksChanged();
    void separateAudioRequested(const QString& clipId);
    void transcribeRequested(const QString& clipId);
    void autoCaptionsRequested(const QString& clipId);
    void seekRequested(ve::TimeUs t);
    void selectionChanged(const QString& clipId);
    void filesDropped(const QStringList& paths, int track, ve::TimeUs at);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    enum class Part { None, Body, LeftEdge, RightEdge };
    enum class Drag { None, Scrub, Move, TrimLeft, TrimRight };
    struct Hit {
        int track = -1;
        QString clipId;
        Part part = Part::None;
    };

    double xForTime(TimeUs t) const;
    TimeUs timeForX(double x) const;
    int trackY(int track) const;
    QRectF clipRect(int track, const Clip& c) const;
    Hit hitTest(const QPoint& p) const;
    TimeUs snap(TimeUs t, const QStringList& ignoreClips, bool* snapped = nullptr) const;
    int contentHeight() const;
    void paintWaveform(QPainter& p, const QRectF& r, const Clip& c, const MediaItem* m, const QColor& col);
    void updateScrollBars();
    void zoomAround(double factor, int anchorX);
    QRect muteRect(int track) const;
    QRect hideRect(int track) const;
    QRect soloRect(int track) const;

    void paintRuler(QPainter& p);
    void paintTracks(QPainter& p);
    void paintHeaders(QPainter& p);

    Project* m_project;
    double m_pxPerSec = 60.0;
    TimeUs m_playhead = 0;
    QString m_selected;

    // drag state
    Drag m_drag = Drag::None;
    bool m_dragActive = false;
    QPoint m_pressPos;
    QJsonArray m_dragBefore;
    QString m_dragClip;
    QStringList m_dragGroup;          // the dragged clip + linked clips that move with it
    bool m_dragLinked = true;         // false while Alt is held
    bool m_linkedSelection = true;
    TimeUs m_markIn = -1;
    TimeUs m_markOut = -1;
    bool m_linkOnAdd = true;
    int m_dragOrigTrack = 0;
    TimeUs m_dragOrigStart = 0;
    TimeUs m_dragOrigIn = 0;
    TimeUs m_dragOrigDur = 0;
    TimeUs m_snapLine = -1;

    // external drop preview
    int m_dropTrack = -1;
    TimeUs m_dropTime = 0;
    TimeUs m_dropDuration = 0;
};

} // namespace ve
