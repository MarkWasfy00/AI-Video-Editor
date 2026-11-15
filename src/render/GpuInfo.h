#pragma once

#include <QJsonObject>

namespace ve {

// Snapshot of the GPU capabilities the editor can use:
//   opengl   { vendor, renderer, version }
//   vulkan   { built, available, devices: [{ name, type, apiVersion }] }
//   decode   { mode, available: [{ name, label }] }
//   encoders [{ name, label, hardware }]
// Must be called on the GUI thread (creates a temporary GL context once).
QJsonObject collectGpuInfo();

} // namespace ve
