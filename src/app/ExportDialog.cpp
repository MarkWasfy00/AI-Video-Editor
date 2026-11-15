#include "app/ExportDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>

#include "media/HwAccel.h"

namespace ve {

namespace {
QString extensionFor(const QString& encoder)
{
    if (encoder == QLatin1String("libvpx-vp9")) return QStringLiteral("webm");
    if (encoder == QLatin1String("prores_ks")) return QStringLiteral("mov");
    return QStringLiteral("mp4");
}

} // namespace

ExportDialog::ExportDialog(const ProjectSettings& project, const QString& suggestedPath, QWidget* parent)
    : QDialog(parent)
    , m_project(project)
{
    setWindowTitle(tr("Export Video"));
    auto* form = new QFormLayout;

    auto* pathRow = new QWidget(this);
    auto* ph = new QHBoxLayout(pathRow);
    ph->setContentsMargins(0, 0, 0, 0);
    m_path = new QLineEdit(suggestedPath, pathRow);
    auto* browseBtn = new QPushButton(tr("Browse..."), pathRow);
    ph->addWidget(m_path, 1);
    ph->addWidget(browseBtn);
    connect(browseBtn, &QPushButton::clicked, this, &ExportDialog::browse);
    form->addRow(tr("File"), pathRow);

    m_resolution = new QComboBox(this);
    m_resolution->addItem(tr("Project (%1 x %2)").arg(project.width).arg(project.height), QSize(project.width, project.height));
    const double aspect = double(project.width) / std::max(1, project.height);
    for (int h : {2160, 1440, 1080, 720, 480, 360}) {
        const int w = int(std::lround(h * aspect)) & ~1;
        if (w == project.width && h == project.height) continue;
        m_resolution->addItem(QStringLiteral("%1 x %2").arg(w).arg(h), QSize(w, h));
    }
    form->addRow(tr("Resolution"), m_resolution);

    m_fps = new QDoubleSpinBox(this);
    m_fps->setRange(1, 240);
    m_fps->setDecimals(3);
    m_fps->setValue(project.fps);
    form->addRow(tr("Frame rate"), m_fps);

    m_encoder = new QComboBox(this);
    // GPU encoders are listed first; only those that work on this machine appear.
    int preferred = -1;
    const QString last = QSettings().value(QStringLiteral("export/encoder")).toString();
    for (const auto& e : hw::availableEncoders()) {
        m_encoder->addItem(e.label, e.name);
        if (e.name == last) preferred = m_encoder->count() - 1;
        else if (preferred < 0 && last.isEmpty() && e.hardware && e.name.startsWith(QLatin1String("h264_")))
            preferred = m_encoder->count() - 1;
    }
    if (m_encoder->count() == 0) m_encoder->addItem(tr("Default"), QString());
    if (preferred >= 0) m_encoder->setCurrentIndex(preferred);
    connect(m_encoder, &QComboBox::currentIndexChanged, this, &ExportDialog::updateExtension);
    form->addRow(tr("Video codec"), m_encoder);

    m_crf = new QSpinBox(this);
    m_crf->setRange(0, 51);
    m_crf->setValue(20);
    m_crf->setToolTip(tr("Constant quality (CRF / NVENC CQ / AMF QP / QSV ICQ) - lower is better quality, larger file."));
    form->addRow(tr("Quality (CRF)"), m_crf);

    m_audio = new QCheckBox(tr("Include audio"), this);
    m_audio->setChecked(true);
    form->addRow(QString(), m_audio);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Export"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
    resize(520, sizeHint().height());
    updateExtension();
}

void ExportDialog::browse()
{
    const QString ext = extensionFor(m_encoder->currentData().toString());
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Video"), m_path->text(),
                                                      tr("Video (*.%1)").arg(ext));
    if (!path.isEmpty()) m_path->setText(path);
}

void ExportDialog::updateExtension()
{
    const QString ext = extensionFor(m_encoder->currentData().toString());
    QString p = m_path->text();
    if (p.isEmpty()) return;
    const QFileInfo fi(p);
    if (fi.suffix().compare(ext, Qt::CaseInsensitive) != 0)
        m_path->setText(fi.path() + QLatin1Char('/') + fi.completeBaseName() + QLatin1Char('.') + ext);
}

ExportSettings ExportDialog::settings() const
{
    ExportSettings s;
    s.path = m_path->text();
    const QSize sz = m_resolution->currentData().toSize();
    s.width = sz.width();
    s.height = sz.height();
    s.fps = m_fps->value();
    s.videoEncoder = m_encoder->currentData().toString();
    QSettings().setValue(QStringLiteral("export/encoder"), s.videoEncoder);
    s.crf = m_crf->value();
    s.includeAudio = m_audio->isChecked();
    return s;
}

} // namespace ve
