#pragma once

#include "core/Time.h"

#include <QString>

#include <atomic>
#include <memory>
#include <vector>

namespace ve {

// Audio peaks for drawing waveforms on the timeline: one value (0..1, the
// loudest sample of either channel) per 1/kWaveformBucketsPerSecond of source.
// Decodes the whole file; call it off the GUI thread. Returns null when the
// file has no decodable audio or `cancel` became true.
std::shared_ptr<const std::vector<float>> computeWaveform(const QString& path, TimeUs duration,
                                                          const std::atomic<bool>* cancel = nullptr);

} // namespace ve
