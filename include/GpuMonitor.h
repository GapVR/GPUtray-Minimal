#pragma once
#define NOMINMAX
#include <windows.h>
#include <pdh.h>
#include <string>
#include <vector>

enum class Metric { GPU, GPU_MEM, GPU_TEMP, COUNT };

struct SystemStats {
    float gpuUsage;
    float gpuMemoryUsage;
    float gpuTemp;
};

class GpuMonitor {
public:
    GpuMonitor();
    ~GpuMonitor();

    bool Initialize();
    SystemStats Update();

private:
    // PDH for CPU/GPU Usage
    PDH_HQUERY m_hQuery = nullptr;
    PDH_HCOUNTER m_hGpuCounter = nullptr;
    std::vector<PDH_HCOUNTER> m_gpuCounters;

    // NVML for NVIDIA GPUs
    HMODULE m_hNvml = nullptr;
    bool m_nvmlInitialized = false;
    bool m_nvmlBroken = false;
    ULARGE_INTEGER m_nvmlDllTime = {};
    bool m_nvmlDllTimeValid = false;
    ULONGLONG m_nvmlLastInitAttempt = 0;
    void* m_nvmlDevice = nullptr;
    bool InitNvml();
    float GetGpuTempNvml();
    void TeardownNvml();

    // Driver-update resilience: nvml.dll / nvapi64.dll are replaced while a
    // graphics driver is being installed. Detect the swap and reload both.
    void RefreshDriverHandles();

    // WMI for Temperatures and Fallback
    bool InitWmi();
    void CleanupWmi();
    float GetGpuTempWmi();
    // GPU Memory via DXGI
    float GetGpuMemoryUsageDxgi();

    bool m_wmiInitialized = false;
};
