#include "app/Inspector.h"

#include "core/Project.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>

#include <cmath>

namespace ve {

Inspector::Inspector(Project* project, QWidget* parent)
    : QWidget(parent)
    , m_project(project)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(6, 6, 6, 6);
    m_title = new QLabel(tr("No clip selected"), this);
    QFont f = m_title->font();
    f.setBold(true);
    m_title->setFont(f);
    m_title->setWordWrap(true);
    m_info = new QLabel(this);
    m_info->setStyleSheet(QStringLiteral("color: #9a9aa5;"));
    outer->addWidget(m_title);
    outer->addWidget(m_info);

    // Audio <-> video link of the selected clip
    auto* linkRow = new QHBoxLayout;
    m_linkLabel = new QLabel(this);
    m_linkLabel->setWordWrap(true);
    m_linkButton = new QPushButton(this);
    linkRow->addWidget(m_linkLabel, 1);
    linkRow->addWidget(m_linkButton);
    outer->addLayout(linkRow);
    connect(m_linkButton, &QPushButton::clicked, this, [this] {
        if (!m_project->findClip(m_clipId)) return;
        if (!m_project->linkedClips(m_clipId).isEmpty()) {
            m_project->edit(tr("Unlink audio and video"), [&] { m_project->unlinkClip(m_clipId); });
        } else if (const QString mate = m_project->linkCandidate(m_clipId); !mate.isEmpty()) {
            m_project->edit(tr("Link audio and video"), [&] { m_project->linkClips({m_clipId, mate}); });
        }
    });

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_body = new QWidget;
    auto* body = new QVBoxLayout(m_body);
    body->setContentsMargins(0, 0, 0, 0);

    // --- Transform
    auto* tg = new QGroupBox(tr("Transform"), m_body);
    m_transformBox = tg;
    auto* tf = new QFormLayout(tg);
    addRow(tf, tr("Position X"), "x", -4000, 4000, 1, 0,
           [](const Clip& c) { return c.transform.x; }, [](Clip& c, double v) { c.transform.x = v; });
    addRow(tf, tr("Position Y"), "y", -4000, 4000, 1, 0,
           [](const Clip& c) { return c.transform.y; }, [](Clip& c, double v) { c.transform.y = v; });
    addRow(tf, tr("Scale"), "scale", 0.05, 5, 0.01, 2,
           [](const Clip& c) { return c.transform.scale; }, [](Clip& c, double v) { c.transform.scale = v; });
    addRow(tf, tr("Rotation"), "rot", -180, 180, 0.5, 1,
           [](const Clip& c) { return c.transform.rotation; }, [](Clip& c, double v) { c.transform.rotation = v; });
    addRow(tf, tr("Opacity"), "opacity", 0, 1, 0.01, 2,
           [](const Clip& c) { return c.transform.opacity; }, [](Clip& c, double v) { c.transform.opacity = v; });
    // Quick fit / fill / rotate
    m_fitRow = new QWidget(tg);
    auto* fr = new QHBoxLayout(m_fitRow);
    fr->setContentsMargins(0, 0, 0, 0);
    auto addQuick = [&](const QString& text, const QString& tip, auto fn) {
        auto* b = new QPushButton(text, m_fitRow);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        fr->addWidget(b);
    };
    addQuick(tr("Fit"), tr("Whole picture visible inside the frame"), [this] { fitOrFill(false); });
    addQuick(tr("Fill"), tr("Cover the whole frame (edges may be cut off)"), [this] { fitOrFill(true); });
    addQuick(QStringLiteral("\u21BA 90\u00B0"), tr("Rotate 90\u00B0 counter-clockwise"), [this] { rotateBy(-90); });
    addQuick(QStringLiteral("\u21BB 90\u00B0"), tr("Rotate 90\u00B0 clockwise"), [this] { rotateBy(90); });
    tf->addRow(m_fitRow);
    body->addWidget(tg);

    // --- Crop
    auto* cg = new QGroupBox(tr("Crop"), m_body);
    m_cropBox = cg;
    auto* cf = new QFormLayout(cg);
    addRow(cf, tr("Left"), "cropl", 0, 0.95, 0.01, 2,
           [](const Clip& c) { return c.transform.cropLeft; }, [](Clip& c, double v) { c.transform.cropLeft = v; });
    addRow(cf, tr("Right"), "cropr", 0, 0.95, 0.01, 2,
           [](const Clip& c) { return c.transform.cropRight; }, [](Clip& c, double v) { c.transform.cropRight = v; });
    addRow(cf, tr("Top"), "cropt", 0, 0.95, 0.01, 2,
           [](const Clip& c) { return c.transform.cropTop; }, [](Clip& c, double v) { c.transform.cropTop = v; });
    addRow(cf, tr("Bottom"), "cropb", 0, 0.95, 0.01, 2,
           [](const Clip& c) { return c.transform.cropBottom; }, [](Clip& c, double v) { c.transform.cropBottom = v; });
    body->addWidget(cg);

    // --- Speed & time
    auto* sg = new QGroupBox(tr("Speed && Time"), m_body);
    m_speedBox = sg;
    auto* sf = new QFormLayout(sg);
    m_speed = new QDoubleSpinBox(sg);
    m_speed->setRange(0.05, 16.0);
    m_speed->setDecimals(2);
    m_speed->setSingleStep(0.25);
    m_speed->setSuffix(QStringLiteral(" x"));
    m_speed->setKeyboardTracking(false);
    m_speed->setToolTip(tr("Playback speed: 0.5 = slow motion, 2 = twice as fast. The clip keeps its footage, so "
                           "its length changes and later clips move."));
    m_curve = new QComboBox(sg);
    m_curve->addItem(tr("None (constant)"), QStringLiteral("none"));
    m_curve->addItem(tr("Ease in (speed up)"), QStringLiteral("ease-in"));
    m_curve->addItem(tr("Ease out (slow down)"), QStringLiteral("ease-out"));
    m_curve->addItem(tr("Slow - fast - slow"), QStringLiteral("ease-in-out"));
    m_curve->addItem(tr("Bullet (fast - slow-mo - fast)"), QStringLiteral("bullet"));
    m_curve->addItem(tr("Hero (slow-motion moment)"), QStringLiteral("hero"));
    m_curve->setToolTip(tr("Speed curve: speeds up and slows down within the clip"));
    m_reverse = new QCheckBox(tr("Play backwards"), sg);
    m_timeInfo = new QLabel(sg);
    m_timeInfo->setStyleSheet(QStringLiteral("color: #9a9aa5;"));
    sf->addRow(tr("Speed"), m_speed);
    sf->addRow(tr("Curve"), m_curve);
    sf->addRow(tr("Reverse"), m_reverse);
    sf->addRow(m_timeInfo);
    body->addWidget(sg);
    connect(m_speed, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (!m_updating) setSpeed(v, m_curve->currentData().toString());
    });
    connect(m_curve, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_updating) setSpeed(m_speed->value(), m_curve->currentData().toString());
    });
    connect(m_reverse, &QCheckBox::toggled, this, [this](bool on) {
        if (m_updating || !m_project->findClip(m_clipId)) return;
        m_project->edit(on ? tr("Reverse clip") : tr("Play forward"), [&] { m_project->setClipReverse(m_clipId, on); });
    });

    // --- Effects
    auto* eg = new QGroupBox(tr("Effects"), m_body);
    m_effectsBox = eg;
    auto* ef = new QFormLayout(eg);
    addRow(ef, tr("Brightness"), "bright", -1, 1, 0.01, 2,
           [](const Clip& c) { return c.effects.brightness; }, [](Clip& c, double v) { c.effects.brightness = v; });
    addRow(ef, tr("Contrast"), "contrast", 0, 3, 0.01, 2,
           [](const Clip& c) { return c.effects.contrast; }, [](Clip& c, double v) { c.effects.contrast = v; });
    addRow(ef, tr("Saturation"), "sat", 0, 3, 0.01, 2,
           [](const Clip& c) { return c.effects.saturation; }, [](Clip& c, double v) { c.effects.saturation = v; });
    addRow(ef, tr("Hue"), "hue", -180, 180, 1, 0,
           [](const Clip& c) { return c.effects.hue; }, [](Clip& c, double v) { c.effects.hue = v; });
    addRow(ef, tr("Blur"), "blur", 0, 10, 0.1, 1,
           [](const Clip& c) { return c.effects.blur; }, [](Clip& c, double v) { c.effects.blur = v; });
    addRow(ef, tr("Vignette"), "vignette", 0, 1, 0.01, 2,
           [](const Clip& c) { return c.effects.vignette; }, [](Clip& c, double v) { c.effects.vignette = v; });
    addCheck(ef, tr("Grayscale"), "gray",
             [](const Clip& c) { return c.effects.grayscale; }, [](Clip& c, bool v) { c.effects.grayscale = v; });
    addCheck(ef, tr("Invert"), "invert",
             [](const Clip& c) { return c.effects.invert; }, [](Clip& c, bool v) { c.effects.invert = v; });
    auto* reset = new QPushButton(tr("Reset Effects"), eg);
    connect(reset, &QPushButton::clicked, this, [this] {
        apply(QStringLiteral("reset"), [](Clip& c) { c.effects = {}; });
    });
    ef->addRow(reset);
    body->addWidget(eg);

    // --- Audio / fades
    auto* ag = new QGroupBox(tr("Audio && Fades"), m_body);
    auto* af = new QFormLayout(ag);
    m_volumeRow = addRow(af, tr("Volume"), "volume", 0, 2, 0.01, 2,
           [](const Clip& c) { return c.volume; }, [](Clip& c, double v) { c.volume = v; });
    addRow(af, tr("Fade in (s)"), "fadein", 0, 10, 0.05, 2,
           [](const Clip& c) { return usToSeconds(c.fadeIn); }, [](Clip& c, double v) { c.fadeIn = secondsToUs(v); });
    addRow(af, tr("Fade out (s)"), "fadeout", 0, 10, 0.05, 2,
           [](const Clip& c) { return usToSeconds(c.fadeOut); }, [](Clip& c, double v) { c.fadeOut = secondsToUs(v); });
    body->addWidget(ag);
    body->addStretch(1);

    scroll->setWidget(m_body);
    outer->addWidget(scroll, 1);

    connect(m_project, &Project::changed, this, &Inspector::reload);
    setClip({});
}

QWidget* Inspector::addRow(QFormLayout* form, const QString& label, const QString& key, double min, double max,
                           double step, int decimals, std::function<double(const Clip&)> get,
                           std::function<void(Clip&, double)> set)
{
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    Row row;
    row.scale = 1.0 / step;
    row.slider = new QSlider(Qt::Horizontal, w);
    row.slider->setRange(int(std::lround(min * row.scale)), int(std::lround(max * row.scale)));
    row.spin = new QDoubleSpinBox(w);
    row.spin->setRange(min, max);
    row.spin->setSingleStep(step);
    row.spin->setDecimals(decimals);
    row.spin->setKeyboardTracking(false);
    row.spin->setMinimumWidth(70);
    row.get = std::move(get);
    row.set = std::move(set);
    h->addWidget(row.slider, 1);
    h->addWidget(row.spin);
    form->addRow(label, w);

    const size_t idx = m_rows.size();
    m_rows.push_back(row);

    connect(row.slider, &QSlider::valueChanged, this, [this, idx, key](int v) {
        if (m_updating) return;
        const Row& r = m_rows[idx];
        const double value = v / r.scale;
        m_updating = true;
        r.spin->setValue(value);
        m_updating = false;
        apply(key, [&](Clip& c) { r.set(c, value); });
    });
    connect(row.spin, &QDoubleSpinBox::valueChanged, this, [this, idx, key](double value) {
        if (m_updating) return;
        const Row& r = m_rows[idx];
        m_updating = true;
        r.slider->setValue(int(std::lround(value * r.scale)));
        m_updating = false;
        apply(key, [&](Clip& c) { r.set(c, value); });
    });
    return w;
}

void Inspector::addCheck(QFormLayout* form, const QString& label, const QString& key,
                         std::function<bool(const Clip&)> get, std::function<void(Clip&, bool)> set)
{
    Check ch;
    ch.box = new QCheckBox;
    ch.get = std::move(get);
    ch.set = std::move(set);
    form->addRow(label, ch.box);
    const size_t idx = m_checks.size();
    m_checks.push_back(ch);
    connect(ch.box, &QCheckBox::toggled, this, [this, idx, key](bool on) {
        if (m_updating) return;
        apply(key, [&](Clip& c) { m_checks[idx].set(c, on); });
    });
}

void Inspector::setSpeed(double speed, const QString& curvePreset)
{
    const Clip* c = m_project->findClip(m_clipId);
    if (!c || c->freeze) return;
    const QString preset = curvePreset == QLatin1String("none") ? QString() : curvePreset;
    std::vector<SpeedPoint> curve = speedCurvePreset(curvePreset);
    if (curvePreset == QLatin1String("custom")) curve = c->speedCurve;
    m_updating = true;
    m_project->edit(tr("Change speed"), [&] { m_project->setClipSpeed(m_clipId, speed, curve, preset); },
                    m_clipId + QStringLiteral("/speed"));
    m_updating = false;
    reload();
}

void Inspector::fitOrFill(bool fill)
{
    const Clip* c = m_project->findClip(m_clipId);
    const MediaItem* m = c ? m_project->mediaById(c->mediaId) : nullptr;
    if (!c || !m) return;
    double scale = 1.0;
    if (fill && m->info.width > 0 && m->info.height > 0) {
        double w = m->info.width, h = m->info.height;
        if (std::fmod(std::fabs(c->transform.rotation), 180.0) == 90.0) std::swap(w, h); // turned sideways
        const double fx = m_project->settings().width / w, fy = m_project->settings().height / h;
        // scale 1 = fit the unrotated picture; correct for the rotation, then cover
        const double fitUnrotated = std::min(m_project->settings().width / double(m->info.width),
                                             m_project->settings().height / double(m->info.height));
        scale = std::max(fx, fy) / fitUnrotated;
    } else if (std::fmod(std::fabs(c->transform.rotation), 180.0) == 90.0 && m->info.width > 0) {
        const double cw = m_project->settings().width, ch = m_project->settings().height;
        const double fitUnrotated = std::min(cw / m->info.width, ch / m->info.height);
        scale = std::min(cw / m->info.height, ch / m->info.width) / fitUnrotated;
    }
    apply(QStringLiteral("fit"), [&](Clip& x) {
        x.transform.scale = scale;
        x.transform.x = x.transform.y = 0;
    });
    reload();
}

void Inspector::rotateBy(double degrees)
{
    apply(QStringLiteral("rot90"), [&](Clip& x) {
        double r = std::fmod(x.transform.rotation + degrees, 360.0);
        if (r > 180) r -= 360;
        if (r <= -180) r += 360;
        x.transform.rotation = r;
    });
    reload();
}

void Inspector::apply(const QString& key, const std::function<void(Clip&)>& fn)
{
    const Clip* c = m_project->findClip(m_clipId);
    if (!c) return;
    Clip copy = *c;
    fn(copy);
    m_updating = true;
    // Consecutive changes of the same property on the same clip merge into one undo step.
    m_project->edit(tr("Change clip property"), [&] { m_project->updateClip(copy); }, m_clipId + QLatin1Char('/') + key);
    m_updating = false;
}

void Inspector::setClip(const QString& clipId)
{
    m_clipId = clipId;
    reload();
}

void Inspector::reload()
{
    if (m_updating) return;
    static const Clip kDefaults;
    const Clip* c = m_project->findClip(m_clipId);
    m_body->setEnabled(c != nullptr);
    if (!c) {
        m_title->setText(tr("No clip selected"));
        m_info->clear();
        m_linkLabel->clear();
        m_linkButton->setVisible(false);
        c = &kDefaults; // show neutral values in the disabled controls
    } else {
        const double fps = m_project->settings().fps;
        int ti = -1;
        m_project->findClip(m_clipId, &ti);
        const bool audio = ti >= 0 && m_project->tracks()[ti].isAudio();
        const bool linked = !m_project->linkedClips(m_clipId).isEmpty();
        m_title->setText(c->name);
        if (c->isText())
            m_info->setText(tr("Text clip  |  Start %1  |  Length %2  |  Words, font and animation: Text panel")
                                .arg(formatTimecode(c->start, fps), formatTimecode(c->duration, fps)));
        else
            m_info->setText(tr("%1 clip%2  |  Start %3  |  Length %4  |  Source in %5")
                                .arg(audio ? tr("Audio") : tr("Video"), linked ? tr(" (linked)") : QString(),
                                     formatTimecode(c->start, fps), formatTimecode(c->duration, fps),
                                     formatTimecode(c->in, fps)));
        // Pictures are edited on the video clip, sound on the audio clip.
        m_transformBox->setEnabled(!audio);
        m_effectsBox->setEnabled(!audio);
        m_cropBox->setEnabled(!audio);
        m_volumeRow->setEnabled(audio);
        m_speedBox->setEnabled(!c->freeze && !c->isText());
        m_fitRow->setEnabled(!audio && !c->isText());   // text: no source picture to crop or fit
        if (c->isText()) m_cropBox->setEnabled(false);
        {
            QString info;
            if (c->isText()) info = tr("Text overlay");
            else if (c->freeze) info = tr("Freeze frame: holds the frame at %1").arg(formatTimecode(c->in, fps));
            else info = tr("Shows %1 of footage").arg(formatTimecode(c->sourceSpan(), fps));
            m_timeInfo->setText(info);
        }

        const QStringList partners = m_project->linkedClips(m_clipId);
        if (!partners.isEmpty()) {
            int pt = -1;
            m_project->findClip(partners.first(), &pt);
            m_linkLabel->setText(tr("Linked with its %1 on %2")
                                     .arg(audio ? tr("video") : tr("audio"), pt >= 0 ? m_project->tracks()[pt].name : QString()));
            m_linkButton->setText(tr("Unlink"));
            m_linkButton->setToolTip(tr("Separate the audio from the video so each can be edited on its own (Ctrl+L)"));
            m_linkButton->setVisible(true);
        } else if (!m_project->linkCandidate(m_clipId).isEmpty()) {
            m_linkLabel->setText(tr("Not linked to its %1").arg(audio ? tr("video") : tr("audio")));
            m_linkButton->setText(tr("Link"));
            m_linkButton->setToolTip(tr("Link it again with its %1 (Ctrl+L)").arg(audio ? tr("video") : tr("audio")));
            m_linkButton->setVisible(true);
        } else {
            m_linkLabel->setText(audio ? tr("Audio only") : tr("No audio linked"));
            m_linkButton->setVisible(false);
        }
    }
    m_updating = true;
    for (auto& r : m_rows) {
        const double v = r.get(*c);
        r.spin->setValue(v);
        r.slider->setValue(int(std::lround(v * r.scale)));
    }
    for (auto& ch : m_checks) ch.box->setChecked(ch.get(*c));
    m_speed->setValue(c->speed);
    {
        const QString preset = c->speedCurve.empty() ? QStringLiteral("none")
                               : c->curvePreset.isEmpty() ? QStringLiteral("custom") : c->curvePreset;
        int idx = m_curve->findData(preset);
        if (idx < 0) {
            m_curve->addItem(tr("Custom"), QStringLiteral("custom"));
            idx = m_curve->count() - 1;
        }
        m_curve->setCurrentIndex(idx);
    }
    m_reverse->setChecked(c->reverse);
    m_updating = false;
}

} // namespace ve
