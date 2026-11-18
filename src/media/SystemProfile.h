#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace ve {

// An NVIDIA GPU as reported by nvidia-smi (the only GPUs PyTorch's CUDA builds use).
struct NvidiaGpu {
    int index = 0;                 // nvidia-smi index (= CUDA index with CUDA_DEVICE_ORDER=PCI_BUS_ID)
    QString name;
    double vramGB = 0.0;
    double computeCapability = 0.0;  // 7.5 = RTX 20, 8.6 = RTX 30, 8.9 = RTX 40, 12.0 = RTX 50
};

// What this computer can give the local AI models: memory, CPU, CUDA GPUs.
// Used to recommend the model that fits (see AiModels).
struct SystemProfile {
    double ramGB = 0.0;
    int cpuThreads = 1;
    QVector<NvidiaGpu> nvidia;
    bool appleSilicon = false;     // PyTorch can use the GPU through Metal (MPS)
    double diskFreeGB = 0.0;       // where the AI models are stored
    bool simulated = false;        // from VE_SIMULATE_HARDWARE

    bool hasNvidia() const { return !nvidia.isEmpty(); }
    const NvidiaGpu* bestGpu() const;          // most VRAM
    double vramGB() const { return bestGpu() ? bestGpu()->vramGB : 0.0; }
    double computeCapability() const { return bestGpu() ? bestGpu()->computeCapability : 0.0; }
    QString gpuText() const;                   // "NVIDIA GeForce RTX 4090 (24 GB)" / "No NVIDIA GPU"
    QString summary() const;                   // one line for dialogs
    QString fingerprint() const;               // changes when the hardware changes
    QJsonObject toJson() const;

    // Detected once per process (runs nvidia-smi). VE_SIMULATE_HARDWARE overrides
    // it for testing, e.g. "ram=8,gpu=none" or "ram=32,vram=24,cc=8.9,name=RTX 4090".
    static const SystemProfile& current();
    static SystemProfile detect();
};

} // namespace ve
