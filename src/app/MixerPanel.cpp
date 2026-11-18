#include "app/MixerPanel.h"

#include "core/Project.h"

#include <QDial>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace ve {

namespace {

constexpr double kMeterTop = 6.0;      // dBFS at the top of the meter
constexpr double kMeterBottom = -60.0;
constexpr double kFallDbPerSec = 26.0;
constexpr double kHoldSeconds = 1.5;

double linToDb(float v) { return v > 0 ? 20.0 * std::log10(double(v)) : -90.0; }

} // namespace

// ---------------------------------------------------------------------------
// LevelMeter
// ---------------------------------------------------------------------------
LevelMeter::LevelMeter(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setToolTip(tr("Peak level (dBFS). Click to reset the clip indicator."));
}

double LevelMeter::yFor(double db) const
{
    const double t = (std::clamp(db, kMeterBottom, kMeterTop) - kMeterBottom) / (kMeterTop - kMeterBottom);
    return (height() - 2) * (1.0 - t) + 1;
}

void LevelMeter::feed(const ChannelLevels* lv, double dt)
{
    bool changed = false;
    for (int c = 0; c < 2; ++c) {
        const double in = lv && lv->frames > 0 ? linToDb(lv->peak[c]) : -90.0;
        const double fallen = m_level[c] - kFallDbPerSec * dt;
        const double next = std::max(in, std::max(fallen, -90.0));
        if (std::fabs(next - m_level[c]) > 0.05) changed = true;
        m_level[c] = next;

        m_holdAge[c] += dt;
        if (in >= m_hold[c]) {
            m_hold[c] = in;
            m_holdAge[c] = 0;
        } else if (m_holdAge[c] > kHoldSeconds) {
            const double h = std::max(m_hold[c] - kFallDbPerSec * dt, -90.0);
            if (h != m_hold[c]) changed = true;
            m_hold[c] = h;
        }
        if (lv && lv->peak[c] > 1.0f && !m_clip) {
            m_clip = true;
            changed = true;
            emit clipChanged(true);
        }
    }
    if (changed) update();
}

void LevelMeter::resetClip()
{
    if (!m_clip) return;
    m_clip = false;
    emit clipChanged(false);
    update();
}

void LevelMeter::mousePressEvent(QMouseEvent*)
{
    resetClip();
}

void LevelMeter::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const QRect r = rect();
    p.fillRect(r, QColor(6, 6, 7));

    QLinearGradient g(0, yFor(kMeterTop), 0, yFor(kMeterBottom));
    const auto stop = [&](double db) { return (kMeterTop - db) / (kMeterTop - kMeterBottom); };
    g.setColorAt(0.0, QColor(235, 50, 40));
    g.setColorAt(stop(0.0), QColor(235, 50, 40));
    g.setColorAt(stop(-0.1), QColor(240, 200, 40));
    g.setColorAt(stop(-9.0), QColor(220, 220, 50));
    g.setColorAt(stop(-18.0), QColor(70, 200, 90));
    g.setColorAt(1.0, QColor(40, 150, 70));

    const int gap = 2;
    const int bw = std::max(3, (r.width() - 3 * gap) / 2);
    for (int c = 0; c < 2; ++c) {
        const int x = gap + c * (bw + gap);
        const int top = int(yFor(m_level[c]));
        if (m_level[c] > kMeterBottom) p.fillRect(QRect(x, top, bw, r.height() - 1 - top), g);
        if (m_hold[c] > kMeterBottom) {
            const int hy = int(yFor(m_hold[c]));
            p.fillRect(QRect(x, hy, bw, 2), m_hold[c] > 0 ? QColor(255, 80, 70) : QColor(230, 230, 230));
        }
    }

    // Scale ticks
    p.setPen(QColor(255, 255, 255, 36));
    for (double db : {0.0, -6.0, -12.0, -24.0, -36.0, -48.0}) {
        const int y = int(yFor(db));
        p.drawLine(0, y, r.width(), y);
    }
    if (m_clip) p.fillRect(QRect(0, 0, r.width(), 4), QColor(255, 40, 30));
}

// ---------------------------------------------------------------------------
// ChannelStrip
// ---------------------------------------------------------------------------
int ChannelStrip::dbToPos(double db)
{
    if (db <= kMinGainDb) return 0;
    const double x = 1.0 - std::sqrt((kMaxGainDb - std::min(db, kMaxGainDb)) / 72.0);
    return std::clamp(int(std::lround(x * 1000.0)), 0, 1000);
}

double ChannelStrip::posToDb(int pos)
{
    if (pos <= 0) return kMinGainDb;
    const double x = pos / 1000.0;
    const double db = kMaxGainDb - 72.0 * (1.0 - x) * (1.0 - x);
    return std::clamp(std::round(db * 10.0) / 10.0, kMinGainDb, kMaxGainDb);
}

ChannelStrip::ChannelStrip(const QString& name, bool isMaster, QWidget* parent)
    : QFrame(parent)
    , m_master(isMaster)
{
    setFrameShape(QFrame::StyledPanel);
    setFixedWidth(isMaster ? 86 : 74);

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(3);

    m_name = new QLabel(this);
    m_name->setAlignment(Qt::AlignCenter);
    QFont f = m_name->font();
    f.setBold(true);
    m_name->setFont(f);
    setName(name);
    v->addWidget(m_name);

    if (!isMaster) {
        m_pan = new QDial(this);
        m_pan->setRange(-100, 100);
        m_pan->setValue(0);
        m_pan->setNotchesVisible(true);
        m_pan->setNotchTarget(25);
        m_pan->setFixedSize(38, 38);
        m_pan->setToolTip(tr("Pan (double-click to centre)"));
        m_pan->installEventFilter(this);
        m_panLabel = new QLabel(QStringLiteral("C"), this);
        m_panLabel->setAlignment(Qt::AlignCenter);
        v->addWidget(m_pan, 0, Qt::AlignHCenter);
        v->addWidget(m_panLabel);
        connect(m_pan, &QDial::valueChanged, this, [this](int val) {
            updatePanLabel(val);
            emit panChanged(val / 100.0);
        });
    } else {
        m_clipLed = new QLabel(tr("CLIP"), this);
        m_clipLed->setAlignment(Qt::AlignCenter);
        m_clipLed->setToolTip(tr("The mix went over 0 dBFS. Click the meter to reset."));
        m_clipLed->setStyleSheet(QStringLiteral("QLabel { color: #777; border: 1px solid #555; border-radius: 3px; "
                                                "font-size: 8pt; font-weight: bold; }"));
        m_clipLed->setFixedHeight(18);
        v->addWidget(m_clipLed);
        v->addSpacing(36); // line the fader up with the track strips (pan knob + label)
    }

    auto* h = new QHBoxLayout;
    h->setSpacing(3);
    m_fader = new QSlider(Qt::Vertical, this);
    m_fader->setRange(0, 1000);
    m_fader->setPageStep(50);
    m_fader->setValue(dbToPos(0));
    m_fader->setTickPosition(QSlider::TicksLeft);
    m_fader->setTickInterval(dbToPos(0)); // one tick at 0 dB
    m_fader->setToolTip(tr("Volume (double-click for 0 dB)"));
    m_fader->installEventFilter(this);
    m_meter = new LevelMeter(this);
    if (isMaster) m_meter->setFixedWidth(26);
    h->addWidget(m_fader, 0, Qt::AlignHCenter);
    h->addWidget(m_meter);
    v->addLayout(h, 1);

    m_gain = new QDoubleSpinBox(this);
    m_gain->setRange(kMinGainDb, kMaxGainDb);
    m_gain->setDecimals(1);
    m_gain->setSingleStep(0.5);
    m_gain->setSuffix(QStringLiteral(" dB"));
    m_gain->setSpecialValueText(QStringLiteral("-inf"));
    m_gain->setKeyboardTracking(false);
    m_gain->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_gain->setAlignment(Qt::AlignCenter);
    m_gain->setValue(0);
    v->addWidget(m_gain);

    auto* btns = new QHBoxLayout;
    btns->setSpacing(2);
    m_mute = new QToolButton(this);
    m_mute->setText(QStringLiteral("M"));
    m_mute->setCheckable(true);
    m_mute->setToolTip(isMaster ? tr("Mute the whole mix") : tr("Mute track"));
    m_mute->setStyleSheet(QStringLiteral("QToolButton:checked { background: #c0392b; color: white; }"));
    btns->addWidget(m_mute);
    if (!isMaster) {
        m_solo = new QToolButton(this);
        m_solo->setText(QStringLiteral("S"));
        m_solo->setCheckable(true);
        m_solo->setToolTip(tr("Solo: only soloed tracks are heard"));
        m_solo->setStyleSheet(QStringLiteral("QToolButton:checked { background: #d4ac0d; color: black; }"));
        btns->addWidget(m_solo);
        connect(m_solo, &QToolButton::toggled, this, &ChannelStrip::soloToggled);
    }
    v->addLayout(btns);

    connect(m_fader, &QSlider::valueChanged, this, [this](int pos) {
        const double db = posToDb(pos);
        const QSignalBlocker b(m_gain);
        m_gain->setValue(db);
        emit gainChanged(db);
    });
    connect(m_gain, &QDoubleSpinBox::valueChanged, this, [this](double db) {
        const QSignalBlocker b(m_fader);
        m_fader->setValue(dbToPos(db));
        emit gainChanged(db);
    });
    connect(m_mute, &QToolButton::toggled, this, &ChannelStrip::muteToggled);
    if (m_clipLed) {
        connect(m_meter, &LevelMeter::clipChanged, this, [this](bool on) {
            m_clipLed->setStyleSheet(on ? QStringLiteral("QLabel { background: #e02a1f; color: white; border-radius: 3px; "
                                                         "font-size: 8pt; font-weight: bold; }")
                                        : QStringLiteral("QLabel { color: #777; border: 1px solid #555; border-radius: 3px; "
                                                         "font-size: 8pt; font-weight: bold; }"));
        });
    }
}

void ChannelStrip::setName(const QString& name)
{
    m_name->setText(m_name->fontMetrics().elidedText(name, Qt::ElideRight, width() - 10));
    m_name->setToolTip(name);
}

void ChannelStrip::updatePanLabel(int v)
{
    if (!m_panLabel) return;
    m_panLabel->setText(v == 0 ? QStringLiteral("C") : (v < 0 ? QStringLiteral("L%1").arg(-v) : QStringLiteral("R%1").arg(v)));
}

void ChannelStrip::setValues(double gainDb, double pan, bool muted, bool solo, bool audible)
{
    if (!m_fader->isSliderDown()) {
        const QSignalBlocker b(m_fader);
        m_fader->setValue(dbToPos(gainDb));
    }
    if (!m_gain->hasFocus()) {
        const QSignalBlocker b(m_gain);
        m_gain->setValue(gainDb);
    }
    if (m_pan && !m_pan->isSliderDown()) {
        const QSignalBlocker b(m_pan);
        const int v = int(std::lround(pan * 100));
        m_pan->setValue(v);
        updatePanLabel(v);
    }
    {
        const QSignalBlocker b(m_mute);
        m_mute->setChecked(muted);
    }
    if (m_solo) {
        const QSignalBlocker b(m_solo);
        m_solo->setChecked(solo);
    }
    // Dim strips that are silenced by mute or by another track's solo.
    m_name->setEnabled(audible);
}

bool ChannelStrip::eventFilter(QObject* o, QEvent* e)
{
    if (e->type() == QEvent::MouseButtonDblClick) {
        if (o == m_fader) {
            m_fader->setValue(dbToPos(0));
            return true;
        }
        if (o == m_pan) {
            m_pan->setValue(0);
            return true;
        }
    }
    return QFrame::eventFilter(o, e);
}

// ---------------------------------------------------------------------------
// MixerPanel
// ---------------------------------------------------------------------------
MixerPanel::MixerPanel(Project* project, QWidget* parent)
    : QWidget(parent)
    , m_project(project)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    outer->setSpacing(4);

    auto* top = new QHBoxLayout;
    auto* resetBtn = new QPushButton(tr("Reset All"), this);
    resetBtn->setToolTip(tr("All faders to 0 dB, pan centred, mute and solo off"));
    top->addWidget(new QLabel(tr("Audio tracks and master bus"), this), 1);
    top->addWidget(resetBtn);
    outer->addLayout(top);

    auto* row = new QHBoxLayout;
    row->setSpacing(4);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_stripsHost = new QWidget(scroll);
    m_stripsLayout = new QHBoxLayout(m_stripsHost);
    m_stripsLayout->setContentsMargins(0, 0, 0, 0);
    m_stripsLayout->setSpacing(3);
    m_stripsLayout->addStretch(1);
    scroll->setWidget(m_stripsHost);
    row->addWidget(scroll, 1);

    m_masterStrip = new ChannelStrip(tr("Master"), true, this);
    row->addWidget(m_masterStrip);
    outer->addLayout(row, 1);
    setMinimumHeight(260);

    connect(m_masterStrip, &ChannelStrip::gainChanged, this, [this](double db) {
        MasterBus m = m_project->master();
        m.gainDb = db;
        m_project->edit(tr("Master volume"), [&] { m_project->setMaster(m); }, QStringLiteral("mix:master"));
    });
    connect(m_masterStrip, &ChannelStrip::muteToggled, this, [this](bool on) {
        MasterBus m = m_project->master();
        m.muted = on;
        m_project->edit(on ? tr("Mute master") : tr("Unmute master"), [&] { m_project->setMaster(m); });
    });
    connect(resetBtn, &QPushButton::clicked, this, [this] {
        m_project->edit(tr("Reset mixer"), [&] {
            for (int i = 0; i < m_project->trackCount(); ++i) {
                m_project->setTrackGain(i, 0.0);
                m_project->setTrackPan(i, 0.0);
                m_project->setTrackMuted(i, false);
                m_project->setTrackSolo(i, false);
            }
            m_project->setMaster({});
        });
    });

    connect(m_project, &Project::changed, this, &MixerPanel::syncFromProject);
    m_clock.start();
    rebuild();
}

int MixerPanel::trackIndex(const QString& id) const
{
    const auto& tracks = m_project->tracks();
    for (int i = 0; i < int(tracks.size()); ++i)
        if (tracks[i].id == id) return i;
    return -1;
}

void MixerPanel::rebuild()
{
    for (auto& [id, strip] : m_strips) delete strip;
    m_strips.clear();

    int pos = 0;
    for (const Track& t : m_project->tracks()) {
        if (!t.isAudio()) continue;
        auto* strip = new ChannelStrip(t.name, false, m_stripsHost);
        const QString id = t.id;
        connect(strip, &ChannelStrip::gainChanged, this, [this, id](double db) {
            const int i = trackIndex(id);
            if (i < 0) return;
            m_project->edit(tr("Track volume"), [&] { m_project->setTrackGain(i, db); }, QStringLiteral("mix:gain:") + id);
        });
        connect(strip, &ChannelStrip::panChanged, this, [this, id](double pan) {
            const int i = trackIndex(id);
            if (i < 0) return;
            m_project->edit(tr("Track pan"), [&] { m_project->setTrackPan(i, pan); }, QStringLiteral("mix:pan:") + id);
        });
        connect(strip, &ChannelStrip::muteToggled, this, [this, id](bool on) {
            const int i = trackIndex(id);
            if (i >= 0) m_project->edit(on ? tr("Mute track") : tr("Unmute track"), [&] { m_project->setTrackMuted(i, on); });
        });
        connect(strip, &ChannelStrip::soloToggled, this, [this, id](bool on) {
            const int i = trackIndex(id);
            if (i >= 0) m_project->edit(on ? tr("Solo track") : tr("Unsolo track"), [&] { m_project->setTrackSolo(i, on); });
        });
        m_stripsLayout->insertWidget(pos++, strip);
        m_strips.emplace_back(id, strip);
    }
    syncFromProject();
}

void MixerPanel::syncFromProject()
{
    std::vector<const Track*> audio;
    for (const Track& t : m_project->tracks())
        if (t.isAudio()) audio.push_back(&t);
    bool same = audio.size() == m_strips.size();
    for (size_t i = 0; same && i < audio.size(); ++i) same = audio[i]->id == m_strips[i].first;
    if (!same) {
        rebuild();
        return;
    }
    TimelineSnapshot s;
    s.tracks = m_project->tracks();
    for (size_t i = 0; i < audio.size(); ++i) {
        const Track& t = *audio[i];
        ChannelStrip* strip = m_strips[i].second;
        strip->setName(t.name);
        strip->setValues(t.gainDb, t.pan, t.muted, t.solo, trackAudible(s, t));
    }
    const MasterBus& m = m_project->master();
    m_masterStrip->setValues(m.gainDb, 0.0, m.muted, false, !m.muted);
}

void MixerPanel::updateMeters(const MixLevels* levels)
{
    const double dt = std::clamp(m_clock.restart() / 1000.0, 0.0, 0.25);
    for (auto& [id, strip] : m_strips) {
        const ChannelLevels* lv = nullptr;
        if (levels) {
            const auto it = levels->tracks.constFind(id);
            if (it != levels->tracks.constEnd()) lv = &it.value();
        }
        strip->meter()->feed(lv, dt);
    }
    m_masterStrip->meter()->feed(levels ? &levels->master : nullptr, dt);
}

} // namespace ve
