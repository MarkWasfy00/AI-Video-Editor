#include "app/AiModelsDialog.h"

#include "app/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

namespace ve {

namespace {

QString fitColor(AiFit fit)
{
    switch (fit) {
    case AiFit::Good: return QStringLiteral("#5cc98a");
    case AiFit::Slow: return QStringLiteral("#e0b450");
    case AiFit::TooHeavy:
    case AiFit::Unsupported: return QStringLiteral("#ef6f6f");
    }
    return theme::kText.name();
}

QString sizeText(double gb)
{
    if (gb < 0.1) return QObject::tr("%1 MB").arg(std::max(1, int(gb * 1000 + 0.5)));
    return QObject::tr("%1 GB").arg(gb, 0, 'f', 1);
}

const char* kPromptKey = "ai/promptedHardware";

} // namespace

void fillModelCombo(QComboBox* combo, AiTask task, const QString& current, bool downloadedOnly)
{
    const QSignalBlocker block(combo);
    combo->clear();
    const QString rec = ai::recommended(task);
    for (const AiModel& m : ai::models(task)) {
        if (downloadedOnly && m.id != current && !ai::isInstalled(m.id)) continue;
        const AiAssessment a = ai::assess(m);
        QString label = m.name;
        if (m.id == rec) label += QObject::tr("  ★ Recommended for this computer");
        else label += QStringLiteral("  — ") + ai::fitLabel(a.fit);
        if (ai::isInstalled(m.id)) label += QObject::tr(" (installed)");
        combo->addItem(label, m.id);
        combo->setItemData(combo->count() - 1, QColor(fitColor(a.fit)), Qt::ForegroundRole);
    }
    const int i = combo->findData(current);
    combo->setCurrentIndex(i >= 0 ? i : 0);
}

QString modelDetailsHtml(const QString& modelId)
{
    const AiModel* m = ai::find(modelId);
    if (!m) return {};
    const AiAssessment a = ai::assess(*m);
    QString html = QStringLiteral("<div>%1</div>").arg(m->description.toHtmlEscaped());
    html += QStringLiteral("<div style='color:%1'>%2</div>").arg(theme::kTextDim.name(),
        QObject::tr("Needs: %1. Download: %2%3.").arg(m->requirements.toHtmlEscaped(), sizeText(m->downloadGB),
                                                      ai::isInstalled(m->id) ? QObject::tr(" (installed)") : QString()));
    QString fit = ai::fitLabel(a.fit);
    if (!a.reason.isEmpty()) fit += QStringLiteral(": ") + a.reason;
    html += QStringLiteral("<div style='color:%1'><b>%2</b></div>").arg(fitColor(a.fit), fit.toHtmlEscaped());
    return html;
}

AiModelsDialog::AiModelsDialog(Mode mode, QWidget* parent) : QDialog(parent), m_mode(mode)
{
    setWindowTitle(mode == Mode::Startup ? tr("Recommended AI Models for This Computer") : tr("AI Models"));
    setMinimumWidth(640);
    auto* layout = new QVBoxLayout(this);
    const SystemProfile& hw = SystemProfile::current();

    if (mode == Mode::Startup) {
        QString text = tr("<p><b>This computer has limited resources for some of the editor's AI models.</b> "
                          "They run locally, so the hardware decides what works well. These models are recommended "
                          "for this computer:</p><ul>");
        for (const ai::Advice& a : ai::limitedTasks()) {
            const AiModel* best = ai::find(a.best);
            const AiModel* rec = ai::find(a.recommended);
            text += QStringLiteral("<li><b>%1:</b> %2 &rarr; <b>%3</b><br><span style='color:%4'>%5: %6</span></li>")
                        .arg(ai::taskName(a.task).toHtmlEscaped(), best->name.toHtmlEscaped(),
                             rec ? rec->name.toHtmlEscaped() : QString(), theme::kTextDim.name(),
                             best->name.toHtmlEscaped(), (a.bestFit.reason.isEmpty() ? ai::fitLabel(a.bestFit.fit) : a.bestFit.reason).toHtmlEscaped());
        }
        text += tr("</ul><p>You can change these at any time in <b>Tools &gt; AI Models</b>.</p>");
        auto* banner = new QLabel(text);
        banner->setWordWrap(true);
        banner->setTextFormat(Qt::RichText);
        layout->addWidget(banner);
    }

    auto* hwBox = new QGroupBox(tr("This computer"));
    auto* hwLayout = new QVBoxLayout(hwBox);
    QString hwText = tr("RAM: %1 GB · CPU threads: %2 · Free disk: %3 GB")
                         .arg(hw.ramGB, 0, 'f', 1).arg(hw.cpuThreads).arg(hw.diskFreeGB, 0, 'f', 0);
    if (hw.nvidia.isEmpty()) hwText += QStringLiteral("\n") + hw.gpuText();
    for (const NvidiaGpu& g : hw.nvidia)
        hwText += tr("\nGPU: %1, %2 GB VRAM, CUDA compute %3").arg(g.name).arg(g.vramGB, 0, 'f', 1).arg(g.computeCapability, 0, 'f', 1);
    if (hw.simulated) hwText += tr("\n(simulated with VE_SIMULATE_HARDWARE)");
    hwLayout->addWidget(new QLabel(hwText));
    layout->addWidget(hwBox);

    // Switch between the models already on this computer (no download needed).
    auto* downloaded = new QCheckBox(tr("Show only downloaded models (switch between them without downloading)"));
    layout->addWidget(downloaded);
    connect(downloaded, &QCheckBox::toggled, this, [this](bool on) {
        m_downloadedOnly = on;
        refill();
    });

    for (AiTask task : ai::tasks()) {
        auto* box = new QGroupBox(ai::taskName(task));
        auto* v = new QVBoxLayout(box);
        auto* combo = new QComboBox;
        fillModelCombo(combo, task, mode == Mode::Startup ? ai::recommended(task) : ai::selected(task));
        auto* details = new QLabel;
        details->setWordWrap(true);
        details->setTextFormat(Qt::RichText);
        details->setMinimumHeight(details->fontMetrics().height() * 4);
        v->addWidget(combo);
        v->addWidget(details);
        layout->addWidget(box);
        m_combos[task] = combo;
        m_details[task] = details;
        connect(combo, &QComboBox::currentIndexChanged, this, [this, task] { updateDetails(task); });
        updateDetails(task);
    }

    auto* buttons = new QDialogButtonBox;
    QPushButton* ok = buttons->addButton(mode == Mode::Startup ? tr("Use These Models") : tr("Save"), QDialogButtonBox::AcceptRole);
    ok->setDefault(true);
    buttons->addButton(mode == Mode::Startup ? tr("Not Now") : tr("Cancel"), QDialogButtonBox::RejectRole);
    QPushButton* recBtn = buttons->addButton(tr("Use Recommended"), QDialogButtonBox::ResetRole);
    connect(recBtn, &QPushButton::clicked, this, &AiModelsDialog::useRecommended);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        apply();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void AiModelsDialog::refill()
{
    for (AiTask task : ai::tasks()) {
        fillModelCombo(m_combos[task], task, m_combos[task]->currentData().toString(), m_downloadedOnly);
        updateDetails(task);
    }
}

void AiModelsDialog::updateDetails(AiTask task)
{
    m_details[task]->setText(modelDetailsHtml(m_combos[task]->currentData().toString()));
}

void AiModelsDialog::useRecommended()
{
    for (AiTask task : ai::tasks()) {
        if (m_combos[task]->findData(ai::recommended(task)) < 0)   // hidden by the filter
            fillModelCombo(m_combos[task], task, ai::recommended(task), false);
        const int i = m_combos[task]->findData(ai::recommended(task));
        if (i >= 0) m_combos[task]->setCurrentIndex(i);
    }
}

void AiModelsDialog::apply()
{
    for (AiTask task : ai::tasks()) ai::setSelected(task, m_combos[task]->currentData().toString());
    QSettings().setValue(QLatin1String(kPromptKey), SystemProfile::current().fingerprint());
}

void AiModelsDialog::promptIfLimited(QWidget* parent)
{
    const QString fp = SystemProfile::current().fingerprint();
    if (QSettings().value(QLatin1String(kPromptKey)).toString() == fp) return;   // already asked on this hardware
    if (ai::limitedTasks().isEmpty()) {
        QSettings().setValue(QLatin1String(kPromptKey), fp);
        return;
    }
    AiModelsDialog dlg(Mode::Startup, parent);
    dlg.exec();
    QSettings().setValue(QLatin1String(kPromptKey), fp);   // "Not Now" also counts: ask again only on new hardware
}

} // namespace ve
