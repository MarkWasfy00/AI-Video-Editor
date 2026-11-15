#pragma once

#include <QDialog>

class QComboBox;

namespace ve {

// Hardware decoding mode, Vulkan GPU choice, and a read-only report of the
// detected GPU capabilities.
class GpuSettingsDialog : public QDialog {
    Q_OBJECT
public:
    GpuSettingsDialog(const QString& decodeMode, const QString& vulkanDevice, QWidget* parent = nullptr);

    QString decodeMode() const;
    QString vulkanDevice() const;   // empty = automatic (prefer discrete GPU)

private:
    QComboBox* m_decode = nullptr;
    QComboBox* m_vulkan = nullptr;
};

} // namespace ve
