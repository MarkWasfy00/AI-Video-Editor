#pragma once

#include "media/AudioMixer.h"

#include <QElapsedTimer>
#include <QFrame>
#include <QWidget>

#include <vector>

class QDial;
class QDoubleSpinBox;
class QHBoxLayout;
class QLabel;
class QSlider;
class QToolButton;

namespace ve {

class Project;

// Vertical stereo peak meter, -60..+6 dBFS, with ballistics, peak hold and a
// clip indicator (click to reset).
class LevelMeter : public QWidget {
    Q_OBJECT
public:
    explicit LevelMeter(QWidget* parent = nullptr);

    // Feed one block (linear peaks per channel) or nothing (silence/decay).
    void feed(const ChannelLevels* levels, double dtSeconds);
    void resetClip();
    bool isClipped() const { return m_clip; }

    QSize sizeHint() const override { return {22, 160}; }
    QSize minimumSizeHint() const override { return {14, 80}; }

signals:
    void clipChanged(bool clipped);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    double yFor(double db) const;

    double m_level[2] = {-90, -90};     // displayed peak (dB)
    double m_hold[2] = {-90, -90};
    double m_holdAge[2] = {0, 0};       // seconds since the hold was set
    bool m_clip = false;
};

// One channel strip: name, pan knob, fader + meter, gain readout, mute/solo.
class ChannelStrip : public QFrame {
    Q_OBJECT
public:
    ChannelStrip(const QString& name, bool isMaster, QWidget* parent = nullptr);

    void setName(const QString& name);
    void setValues(double gainDb, double pan, bool muted, bool solo, bool audible);
    LevelMeter* meter() const { return m_meter; }

    // Fader taper: position 0..1000 <-> dB (finer resolution near 0 dB).
    static int dbToPos(double db);
    static double posToDb(int pos);

signals:
    void gainChanged(double db);
    void panChanged(double pan);
    void muteToggled(bool on);
    void soloToggled(bool on);

protected:
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void updatePanLabel(int v);

    bool m_master;
    QLabel* m_name = nullptr;
    QDial* m_pan = nullptr;
    QLabel* m_panLabel = nullptr;
    QSlider* m_fader = nullptr;
    LevelMeter* m_meter = nullptr;
    QDoubleSpinBox* m_gain = nullptr;
    QToolButton* m_mute = nullptr;
    QToolButton* m_solo = nullptr;
    QLabel* m_clipLed = nullptr;
};

// The audio mixer dock: one strip per track (same order as the timeline,
// top track first) plus the master bus. Every change is an undoable edit.
class MixerPanel : public QWidget {
    Q_OBJECT
public:
    explicit MixerPanel(Project* project, QWidget* parent = nullptr);

    // Called ~30x per second: levels of the audio played since the last call,
    // or nullptr when nothing is playing (meters fall back).
    void updateMeters(const MixLevels* levels);

private:
    void syncFromProject();
    void rebuild();
    int trackIndex(const QString& id) const;

    Project* m_project;
    QHBoxLayout* m_stripsLayout = nullptr;
    QWidget* m_stripsHost = nullptr;
    ChannelStrip* m_masterStrip = nullptr;
    std::vector<std::pair<QString, ChannelStrip*>> m_strips;   // (track id, strip)
    QElapsedTimer m_clock;
};

} // namespace ve
