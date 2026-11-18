#include "app/UpscaleDialog.h"

#include "app/AiModelsDialog.h"
#include "media/AiModels.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace ve {

UpscaleDialog::UpscaleDialog(int w, int h, int projectW, int projectH, QWidget* parent)
    : QDialog(parent), m_w(w), m_h(h), m_projectW(projectW), m_projectH(projectH)
{
    setWindowTitle(tr("Upscale Video (AI)"));
    setMinimumWidth(600);
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Increases the clip's resolution with a local AI model. Only the part of the video "
                                "the clip shows is processed. %1").arg(SystemProfile::current().summary()));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    m_model = new QComboBox;
    fillModelCombo(m_model, AiTask::Upscale, ai::selected(AiTask::Upscale));
    form->addRow(tr("Model:"), m_model);
    m_details = new QLabel;
    m_details->setWordWrap(true);
    m_details->setTextFormat(Qt::RichText);
    form->addRow(QString(), m_details);
    m_scale = new QComboBox;
    m_scale->addItem(tr("2x"), 2);
    m_scale->addItem(tr("4x"), 4);
    form->addRow(tr("Scale:"), m_scale);
    m_output = new QLabel;
    form->addRow(tr("Result:"), m_output);
    layout->addLayout(form);

    m_warning = new QLabel;
    m_warning->setWordWrap(true);
    m_warning->setStyleSheet(QStringLiteral("color:#ef6f6f"));
    m_useRecommended = new QPushButton;
    auto* warnRow = new QHBoxLayout;
    warnRow->addWidget(m_warning, 1);
    warnRow->addWidget(m_useRecommended);
    layout->addLayout(warnRow);
    connect(m_useRecommended, &QPushButton::clicked, this, [this] {
        m_model->setCurrentIndex(std::max(0, m_model->findData(ai::recommended(AiTask::Upscale))));
    });

    m_replace = new QCheckBox(tr("Use the upscaled video in the clip (undo restores the original)"));
    m_replace->setChecked(true);
    layout->addWidget(m_replace);
    m_projectRes = new QCheckBox;
    layout->addWidget(m_projectRes);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Upscale"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // Default scale for this computer and source.
    const UpscaleOptions def = ai::upscaleDefaults(model(), w, h);
    m_scale->setCurrentIndex(def.scale == 4 ? 1 : 0);
    connect(m_model, &QComboBox::currentIndexChanged, this, [this] {
        const UpscaleOptions d = ai::upscaleDefaults(model(), m_w, m_h);
        m_scale->setCurrentIndex(d.scale == 4 ? 1 : 0);
        refresh();
    });
    connect(m_scale, &QComboBox::currentIndexChanged, this, &UpscaleDialog::refresh);
    refresh();
}

void UpscaleDialog::refresh()
{
    m_details->setText(modelDetailsHtml(model()));
    const int ow = m_w * scale(), oh = m_h * scale();
    QString out = tr("%1 × %2  →  %3 × %4").arg(m_w).arg(m_h).arg(ow).arg(oh);
    const UpscaleOptions o = ai::upscaleDefaults(model(), m_w, m_h);
    if (o.tileDit || o.tileVae) out += tr("  (low-VRAM mode: processed in tiles)");
    if (std::max(ow, oh) > 4096) out += tr("  — larger than 4K: very slow, consider 2x");
    m_output->setText(out);

    const AiModel* m = ai::find(model());
    const AiAssessment a = m ? ai::assess(*m) : AiAssessment{};
    const QString rec = ai::recommended(AiTask::Upscale);
    const bool bad = a.fit == AiFit::TooHeavy || a.fit == AiFit::Unsupported;
    m_warning->setVisible(bad);
    m_useRecommended->setVisible(bad && rec != model());
    if (bad) {
        m_warning->setText(a.fit == AiFit::Unsupported
                               ? tr("%1 cannot run on this computer: %2.").arg(m->name, a.reason)
                               : tr("%1 is too heavy for this computer (%2). It will probably run out of memory.").arg(m->name, a.reason));
        if (const AiModel* r = ai::find(rec)) m_useRecommended->setText(tr("Use %1").arg(r->name));
    }

    const bool bigger = ow > m_projectW || oh > m_projectH;
    m_projectRes->setVisible(bigger);
    m_projectRes->setText(tr("Also raise the project resolution from %1 × %2 to %3 × %4 (so the export keeps the detail)")
                              .arg(m_projectW).arg(m_projectH).arg(ow).arg(oh));
}

QString UpscaleDialog::model() const { return m_model->currentData().toString(); }
int UpscaleDialog::scale() const { return m_scale->currentData().toInt(); }
bool UpscaleDialog::replaceClip() const { return m_replace->isChecked(); }
bool UpscaleDialog::setProjectResolution() const { return !m_projectRes->isHidden() && m_projectRes->isChecked(); }

} // namespace ve
