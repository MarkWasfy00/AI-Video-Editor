#include "media/HwAccel.h"

#include "media/FFmpegUtil.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <optional>

namespace ve::hw {

namespace {

QMutex g_mutex;
QString g_mode = QStringLiteral("auto");
QHash<int, AVBufferRef*> g_devices;      // device type -> shared context (nullptr = failed)
std::optional<QStringList> g_decodeTypes;
std::optional<std::vector<EncoderInfo>> g_encoders;

// Preferred order for "auto"; only types that decode into frames we can
// download to system memory are listed.
const std::vector<AVHWDeviceType>& preferredDecodeOrder()
{
    static const std::vector<AVHWDeviceType> order = {
#if defined(_WIN32)
        AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_DXVA2, AV_HWDEVICE_TYPE_D3D12VA,
        AV_HWDEVICE_TYPE_VULKAN,
#elif defined(__APPLE__)
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#else
        AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_VULKAN,
#endif
    };
    return order;
}

// Caller holds g_mutex.
AVBufferRef* deviceLocked(AVHWDeviceType type)
{
    auto it = g_devices.find(int(type));
    if (it != g_devices.end()) return it.value();
    AVBufferRef* ref = nullptr;
    if (av_hwdevice_ctx_create(&ref, type, nullptr, nullptr, 0) < 0) ref = nullptr;
    g_devices.insert(int(type), ref);
    return ref;
}

struct EncoderSpec {
    const char* name;
    const char* label;
    bool hardware;
};

const EncoderSpec kEncoders[] = {
    {"h264_nvenc", "H.264 - NVIDIA NVENC (GPU)", true},
    {"hevc_nvenc", "H.265 / HEVC - NVIDIA NVENC (GPU)", true},
    {"av1_nvenc", "AV1 - NVIDIA NVENC (GPU)", true},
    {"h264_amf", "H.264 - AMD AMF (GPU)", true},
    {"hevc_amf", "H.265 / HEVC - AMD AMF (GPU)", true},
    {"av1_amf", "AV1 - AMD AMF (GPU)", true},
    {"h264_qsv", "H.264 - Intel Quick Sync (GPU)", true},
    {"hevc_qsv", "H.265 / HEVC - Intel Quick Sync (GPU)", true},
    {"av1_qsv", "AV1 - Intel Quick Sync (GPU)", true},
    {"h264_videotoolbox", "H.264 - VideoToolbox (GPU)", true},
    {"hevc_videotoolbox", "H.265 / HEVC - VideoToolbox (GPU)", true},
    {"libx264", "H.264 - x264 (CPU)", false},
    {"libx265", "H.265 / HEVC - x265 (CPU)", false},
    {"libvpx-vp9", "VP9 / WebM (CPU)", false},
    {"mpeg4", "MPEG-4 Part 2 (CPU)", false},
    {"prores_ks", "Apple ProRes 422 / MOV (CPU)", false},
};

// Hardware encoders exist in the FFmpeg build even without the hardware,
// so try to open one with a small frame size.
bool canOpenEncoder(const AVCodec* codec)
{
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return false;
    ctx->width = 640;
    ctx->height = 360;
    ctx->time_base = AVRational{1, 30};
    ctx->framerate = AVRational{30, 1};
    const QString name = QString::fromLatin1(codec->name);
    ctx->pix_fmt = (name.endsWith(QLatin1String("_qsv")) || name.endsWith(QLatin1String("_amf")) ||
                    name.endsWith(QLatin1String("_videotoolbox")))
                       ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    const int prevLevel = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET); // failures here are expected on other vendors' GPUs
    const bool ok = avcodec_open2(ctx, codec, nullptr) >= 0;
    av_log_set_level(prevLevel);
    avcodec_free_context(&ctx);
    return ok;
}

} // namespace

QString decodeMode()
{
    QMutexLocker lock(&g_mutex);
    return g_mode;
}

void setDecodeMode(const QString& mode)
{
    QMutexLocker lock(&g_mutex);
    g_mode = mode.isEmpty() ? QStringLiteral("auto") : mode;
}

QStringList availableDecodeDevices()
{
    QMutexLocker lock(&g_mutex);
    if (!g_decodeTypes) {
        QStringList out;
        const int prevLevel = av_log_get_level();
        av_log_set_level(AV_LOG_QUIET);
        for (AVHWDeviceType t : preferredDecodeOrder())
            if (deviceLocked(t)) out << QString::fromLatin1(av_hwdevice_get_type_name(t));
        av_log_set_level(prevLevel);
        g_decodeTypes = out;
    }
    return *g_decodeTypes;
}

std::vector<AVHWDeviceType> decodeCandidates()
{
    const QString mode = decodeMode();
    if (mode == QLatin1String("none")) return {};
    if (mode == QLatin1String("auto")) return preferredDecodeOrder();
    const AVHWDeviceType t = av_hwdevice_find_type_by_name(mode.toLatin1().constData());
    if (t == AV_HWDEVICE_TYPE_NONE) return preferredDecodeOrder();
    return {t};
}

AVBufferRef* acquireDevice(AVHWDeviceType type)
{
    QMutexLocker lock(&g_mutex);
    AVBufferRef* dev = deviceLocked(type);
    return dev ? av_buffer_ref(dev) : nullptr;
}

QString deviceLabel(const QString& t)
{
    if (t == QLatin1String("d3d11va")) return QStringLiteral("Direct3D 11 (D3D11VA)");
    if (t == QLatin1String("d3d12va")) return QStringLiteral("Direct3D 12 (D3D12VA)");
    if (t == QLatin1String("dxva2")) return QStringLiteral("DirectX VA 2 (DXVA2)");
    if (t == QLatin1String("cuda")) return QStringLiteral("NVIDIA NVDEC (CUDA)");
    if (t == QLatin1String("vulkan")) return QStringLiteral("Vulkan Video");
    if (t == QLatin1String("vaapi")) return QStringLiteral("VA-API");
    if (t == QLatin1String("videotoolbox")) return QStringLiteral("VideoToolbox");
    return t;
}

std::vector<EncoderInfo> availableEncoders()
{
    QMutexLocker lock(&g_mutex);
    if (!g_encoders) {
        std::vector<EncoderInfo> out;
        for (const auto& e : kEncoders) {
            const AVCodec* c = avcodec_find_encoder_by_name(e.name);
            if (!c) continue;
            if (e.hardware && !canOpenEncoder(c)) continue;
            out.push_back({QString::fromLatin1(e.name), QString::fromLatin1(e.label), e.hardware});
        }
        g_encoders = std::move(out);
    }
    return *g_encoders;
}

bool isHardwareEncoder(const QString& name)
{
    for (const auto& e : kEncoders)
        if (name == QLatin1String(e.name)) return e.hardware;
    return false;
}

QString encoderLabel(const QString& name)
{
    for (const auto& e : kEncoders)
        if (name == QLatin1String(e.name)) return QString::fromLatin1(e.label);
    return name;
}

} // namespace ve::hw
