#pragma once

#include "render/PreviewView.h"

#include <QVulkanWindow>

#include <memory>

namespace ve {

// Shared QVulkanInstance (created on first use). Returns nullptr if Vulkan
// is unavailable on this machine.
QVulkanInstance* sharedVulkanInstance(QString* error = nullptr);

// Physical devices visible to Vulkan: name, type ("discrete", "integrated", ...)
// and API version, for diagnostics and the GPU settings dialog.
struct VulkanDeviceInfo {
    QString name;
    QString type;
    QString apiVersion;
};
QList<VulkanDeviceInfo> vulkanDevices();

class VulkanPreviewWindow : public QVulkanWindow {
    Q_OBJECT
public:
    QVulkanWindowRenderer* createRenderer() override;

    void setFrame(CompositeFramePtr frame);
    CompositeFramePtr frame() const { return m_frame; }
    QString description() const { return m_description; }
    void setDescription(const QString& d) { m_description = d; }

private:
    CompositeFramePtr m_frame;
    QString m_description = QStringLiteral("Vulkan");
};

// PreviewView wrapper: hosts the Vulkan window inside a QWidget container.
class VulkanPreview : public PreviewView {
public:
    // deviceName: physical device to use; empty = prefer a discrete GPU.
    VulkanPreview(QVulkanInstance* instance, const QString& deviceName = {}, QWidget* parent = nullptr);
    ~VulkanPreview() override;

    QWidget* widget() override { return m_container; }
    void setFrame(CompositeFramePtr frame) override;
    RenderBackend backend() const override { return RenderBackend::Vulkan; }
    QString backendDescription() const override;
    bool hasDevice() const { return !m_window->availablePhysicalDevices().isEmpty(); }
    QString deviceName() const { return m_deviceName; }
    QObject* inputObject() override { return m_window; }
    void setCursorShape(Qt::CursorShape shape) override { m_window->setCursor(shape); }

private:
    VulkanPreviewWindow* m_window = nullptr;
    QWidget* m_container = nullptr;
    QString m_deviceName;
};

} // namespace ve
