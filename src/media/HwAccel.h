#pragma once

#include <QString>
#include <QStringList>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include <vector>

// GPU hardware acceleration for decoding and encoding via FFmpeg
// (NVDEC/CUDA, D3D11VA, DXVA2, D3D12VA, Vulkan Video, VAAPI, VideoToolbox,
// NVENC, AMF, Quick Sync).
namespace ve::hw {

// ---- decoding --------------------------------------------------------------

// "auto" (default), "none" (CPU only) or an FFmpeg device type name such as
// "d3d11va" / "cuda". Thread-safe; affects decoders opened afterwards.
QString decodeMode();
void setDecodeMode(const QString& mode);

// Device types usable for decoding on this machine (probed once, cached).
QStringList availableDecodeDevices();

// Device types to try for a new decoder, in order, according to the mode.
std::vector<AVHWDeviceType> decodeCandidates();

// Shared device context for a type; returns a new reference (caller owns it,
// av_buffer_unref when done) or nullptr if the device cannot be created.
AVBufferRef* acquireDevice(AVHWDeviceType type);

QString deviceLabel(const QString& typeName);

// ---- encoding --------------------------------------------------------------

struct EncoderInfo {
    QString name;        // FFmpeg encoder name
    QString label;       // human readable
    bool hardware = false;
};

// Encoders from our list that this FFmpeg build has and that actually open
// on this machine (hardware encoders are test-opened once, then cached).
std::vector<EncoderInfo> availableEncoders();
bool isHardwareEncoder(const QString& name);
QString encoderLabel(const QString& name);

} // namespace ve::hw
