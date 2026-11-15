#include "app/GpuSettingsDialog.h"

#include "render/GpuInfo.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonArray>
#include <QLabel>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace ve {

GpuSettingsDialog::GpuSettingsDialog(const QString& decodeMode, const QString& vulkanDevice, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("GPU & Hardware Acceleration"));
    const QJsonObject info = collectGpuInfo();

    auto* settings = new QGroupBox(tr("Settings"), this);
    auto* form = new QFormLayout(settings);

    m_decode = new QComboBox(settings);
    m_decode->addItem(tr("Automatic (best available GPU decoder)"), QStringLiteral("auto"));
    m_decode->addItem(tr("Off - decode on the CPU"), QStringLiteral("none"));
    for (const auto& v : info["decode"].toObject()["available"].toArray()) {
        const auto o = v.toObject();
        m_decode->addItem(o["label"].toString(), o["name"].toString());
    }
    const int di = m_decode->findData(decodeMode);
    m_decode->setCurrentIndex(di >= 0 ? di : 0);
    form->addRow(tr("Video decoding"), m_decode);

    m_vulkan = new QComboBox(settings);
    m_vulkan->addItem(tr("Automatic (prefer discrete GPU)"), QString());
    const QJsonObject vk = info["vulkan"].toObject();
    for (const auto& v : vk["devices"].toArray()) {
        const auto o = v.toObject();
        m_vulkan->addItem(QStringLiteral("%1 (%2, Vulkan %3)").arg(o["name"].toString(), o["type"].toString(),
                                                                   o["apiVersion"].toString()),
                          o["name"].toString());
    }
    const int vi = m_vulkan->findData(vulkanDevice);
    m_vulkan->setCurrentIndex(vi >= 0 ? vi : 0);
    m_vulkan->setEnabled(vk["available"].toBool());
    form->addRow(tr("Vulkan preview GPU"), m_vulkan);

    auto* note = new QLabel(tr("GPU encoders (NVENC / AMF / Quick Sync) are chosen per export in the Export dialog."), settings);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: #9a9aa5;"));
    form->addRow(note);

    // Report
    QString html = QStringLiteral("<h4>OpenGL</h4><p>%1<br>%2</p>")
                       .arg(info["opengl"].toObject()["renderer"].toString().toHtmlEscaped(),
                            info["opengl"].toObject()["version"].toString().toHtmlEscaped());
    html += QStringLiteral("<h4>Vulkan</h4><p>");
    if (!vk["built"].toBool()) html += tr("Not built into this version.");
    else if (!vk["available"].toBool()) html += tr("No Vulkan device found.");
    for (const auto& v : vk["devices"].toArray()) {
        const auto o = v.toObject();
        html += QStringLiteral("%1 &mdash; %2, API %3<br>").arg(o["name"].toString().toHtmlEscaped(),
                                                                 o["type"].toString(), o["apiVersion"].toString());
    }
    html += QStringLiteral("</p><h4>Hardware decoders</h4><p>");
    const auto dec = info["decode"].toObject()["available"].toArray();
    if (dec.isEmpty()) html += tr("None available (CPU decoding).");
    for (const auto& v : dec) html += v.toObject()["label"].toString().toHtmlEscaped() + QStringLiteral("<br>");
    html += QStringLiteral("</p><h4>Encoders</h4><p>");
    for (const auto& v : info["encoders"].toArray())
        html += v.toObject()["label"].toString().toHtmlEscaped() + QStringLiteral("<br>");
    html += QStringLiteral("</p>");

    auto* report = new QTextBrowser(this);
    report->setHtml(html);
    report->setMinimumHeight(260);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(settings);
    layout->addWidget(new QLabel(tr("Detected hardware:"), this));
    layout->addWidget(report, 1);
    layout->addWidget(buttons);
    resize(560, 560);
}

QString GpuSettingsDialog::decodeMode() const { return m_decode->currentData().toString(); }
QString GpuSettingsDialog::vulkanDevice() const { return m_vulkan->currentData().toString(); }

} // namespace ve
