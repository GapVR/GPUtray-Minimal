#include "GpuMonitor.h"
#include <dxgi1_4.h>
#include <comdef.h>
#include <WbemIdl.h>
#include <pdhmsg.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <vector>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "dxgi.lib")

typedef enum nvmlReturn_enum {
    NVML_SUCCESS = 0,
    NVML_ERROR_INVALID_ARGUMENT = 2,
    NVML_ERROR_NOT_SUPPORTED = 3,
    NVML_ERROR_NO_PERMISSION = 4
} nvmlReturn_t;
typedef struct nvmlDevice_st* nvmlDevice_t;
typedef enum nvmlTemperatureSensors_enum { NVML_TEMPERATURE_GPU = 0 } nvmlTemperatureSensors_t;
typedef nvmlReturn_t (*pfnNvmlInit)(void);
typedef nvmlReturn_t (*pfnNvmlShutdown)(void);
typedef nvmlReturn_t (*pfnNvmlDeviceGetHandleByIndex)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t (*pfnNvmlDeviceGetTemperature)(nvmlDevice_t, nvmlTemperatureSensors_t, unsigned int*);

static FARPROC SafeGetProcAddress(HMODULE hModule, const char* procName) {
    if (!hModule) return nullptr;
    return GetProcAddress(hModule, procName);
}

static void SafeFreeLibrary(HMODULE hModule) {
    if (!hModule) return;
    FreeLibrary(hModule);
}

static bool SafeNvmlInit(pfnNvmlInit fn) {
    return fn() == NVML_SUCCESS;
}

static void SafeNvmlShutdown(pfnNvmlShutdown fn) {
    fn();
}

static bool SafeNvmlGetHandle(pfnNvmlDeviceGetHandleByIndex fn, unsigned int index, nvmlDevice_t* out) {
    return fn(index, out) == NVML_SUCCESS;
}

static bool SafeNvmlGetTemperature(pfnNvmlDeviceGetTemperature fn, nvmlDevice_t dev, unsigned int* out) {
    return fn(dev, NVML_TEMPERATURE_GPU, out) == NVML_SUCCESS;
}

// Last-write time of a driver DLL in System32, used to detect driver installs.
static bool GetDllLastWriteTime(const wchar_t* dllName, ULARGE_INTEGER* outTime) {
    wchar_t path[MAX_PATH];
    UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wcscat_s(path, L"\\");
    wcscat_s(path, dllName);
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return false;
    outTime->LowPart = data.ftLastWriteTime.dwLowDateTime;
    outTime->HighPart = data.ftLastWriteTime.dwHighDateTime;
    return true;
}



GpuMonitor::GpuMonitor() {}

GpuMonitor::~GpuMonitor() {
    if (m_hQuery) PdhCloseQuery(m_hQuery);
    TeardownNvml();
    CleanupWmi();
}

bool GpuMonitor::Initialize() {
    if (PdhOpenQuery(NULL, 0, &m_hQuery) != ERROR_SUCCESS) return false;

    // GPU Usage - Primary: English, Secondary: Localized
    if (PdhAddEnglishCounterW(m_hQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &m_hGpuCounter) != ERROR_SUCCESS) {
        if (PdhAddCounterW(m_hQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &m_hGpuCounter) != ERROR_SUCCESS) {
            PdhAddCounterW(m_hQuery, L"\\GPU 엔진(*)\\Utilization Percentage", 0, &m_hGpuCounter);
        }
    }

    // GPU Memory - Primary: English, Secondary: Localized
    PDH_HCOUNTER hGpuMem = nullptr;
    if (PdhAddEnglishCounterW(m_hQuery, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &hGpuMem) != ERROR_SUCCESS) {
        if (PdhAddCounterW(m_hQuery, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &hGpuMem) != ERROR_SUCCESS) {
             PdhAddCounterW(m_hQuery, L"\\GPU 어댑터 메모리(*)\\Dedicated Usage", 0, &hGpuMem);
        }
    }
    if (hGpuMem) m_gpuCounters.push_back(hGpuMem);

    PdhCollectQueryData(m_hQuery);
    InitWmi();
    InitNvml();

    return true;
}

SystemStats GpuMonitor::Update() {
    SystemStats stats = { 0 };

    if (m_hQuery) {
        if (PdhCollectQueryData(m_hQuery) == ERROR_SUCCESS) {
            if (m_hGpuCounter) {
                DWORD dwSize = 0, dwCount = 0;
                PdhGetFormattedCounterArray(m_hGpuCounter, PDH_FMT_DOUBLE, &dwSize, &dwCount, NULL);
                if (dwSize > 0) {
                    std::vector<BYTE> buffer(dwSize);
                    PPDH_FMT_COUNTERVALUE_ITEM pItems = (PPDH_FMT_COUNTERVALUE_ITEM)buffer.data();
                    if (PdhGetFormattedCounterArray(m_hGpuCounter, PDH_FMT_DOUBLE, &dwSize, &dwCount, pItems) == ERROR_SUCCESS) {
                        float maxGpu = 0.0f;
                        for (DWORD i = 0; i < dwCount; i++) {
                            if (pItems[i].FmtValue.doubleValue > maxGpu) maxGpu = (float)pItems[i].FmtValue.doubleValue;
                        }
                        stats.gpuUsage = maxGpu;
                    }
                }
            }
        }
    }

    // Memory Usage (Percentage)
    stats.gpuMemoryUsage = GetGpuMemoryUsageDxgi();

    stats.gpuUsage = std::clamp(stats.gpuUsage, 0.0f, 100.0f);
    
    RefreshDriverHandles();

    if (m_nvmlInitialized) {
        stats.gpuTemp = GetGpuTempNvml();
        if (m_nvmlBroken) stats.gpuTemp = GetGpuTempWmi();
    } else {
        stats.gpuTemp = GetGpuTempWmi();
    }

    return stats;
}

float GpuMonitor::GetGpuMemoryUsageDxgi() {
    float totalUsageBytes = 0.0f;
    float totalBudgetBytes = 0.0f;

    IDXGIFactory4* pFactory;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&pFactory))) {
        for (UINT i = 0; ; i++) {
            IDXGIAdapter1* pAdapter1;
            if (pFactory->EnumAdapters1(i, &pAdapter1) == DXGI_ERROR_NOT_FOUND) break;

            DXGI_ADAPTER_DESC1 desc1;
            pAdapter1->GetDesc1(&desc1);

            IDXGIAdapter3* pAdapter3;
            if (SUCCEEDED(pAdapter1->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&pAdapter3))) {
                DXGI_QUERY_VIDEO_MEMORY_INFO memInfo;
                if (SUCCEEDED(pAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo))) {
                    totalUsageBytes += (float)memInfo.CurrentUsage;
                }
                pAdapter3->Release();
            }
            totalBudgetBytes += (float)desc1.DedicatedVideoMemory;
            pAdapter1->Release();
        }
        pFactory->Release();
    }

    if (totalUsageBytes <= 0 && !m_gpuCounters.empty() && m_gpuCounters[0]) {
        DWORD dwSize = 0, dwCount = 0;
        PdhGetFormattedCounterArray(m_gpuCounters[0], PDH_FMT_DOUBLE, &dwSize, &dwCount, NULL);
        if (dwSize > 0) {
            std::vector<BYTE> buf(dwSize);
            PPDH_FMT_COUNTERVALUE_ITEM pItems = (PPDH_FMT_COUNTERVALUE_ITEM)buf.data();
            if (PdhGetFormattedCounterArray(m_gpuCounters[0], PDH_FMT_DOUBLE, &dwSize, &dwCount, pItems) == ERROR_SUCCESS) {
                float pdhSum = 0;
                for (DWORD i = 0; i < dwCount; i++) pdhSum += (float)pItems[i].FmtValue.doubleValue;
                totalUsageBytes = pdhSum;
            }
        }
    }

    if (totalBudgetBytes > 0) {
        return std::clamp((totalUsageBytes / totalBudgetBytes) * 100.0f, 0.0f, 100.0f);
    }
    return 0.0f;
}

bool GpuMonitor::InitNvml() {
    // First try secure system32 search
    m_hNvml = LoadLibraryExW(L"nvml.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m_hNvml) {
        // Fallback for custom driver paths or older setups
        m_hNvml = LoadLibraryW(L"nvml.dll");
    }
    
    if (!m_hNvml) return false;

    auto pInit = SafeGetProcAddress(m_hNvml, "nvmlInit");
    if (!pInit || !SafeNvmlInit((pfnNvmlInit)pInit)) {
        SafeFreeLibrary(m_hNvml);
        m_hNvml = nullptr;
        return false;
    }

    auto pGetHandle = SafeGetProcAddress(m_hNvml, "nvmlDeviceGetHandleByIndex");
    if (!pGetHandle || !SafeNvmlGetHandle((pfnNvmlDeviceGetHandleByIndex)pGetHandle, 0, (nvmlDevice_t*)&m_nvmlDevice)) {
        auto shutdown = (pfnNvmlShutdown)SafeGetProcAddress(m_hNvml, "nvmlShutdown");
        if (shutdown) SafeNvmlShutdown(shutdown);
        SafeFreeLibrary(m_hNvml);
        m_hNvml = nullptr;
        m_nvmlDevice = nullptr;
        return false;
    }

    m_nvmlInitialized = true;
    return true;
}



void GpuMonitor::TeardownNvml() {
    if (m_hNvml) {
        if (m_nvmlInitialized) {
            auto shutdown = (pfnNvmlShutdown)SafeGetProcAddress(m_hNvml, "nvmlShutdown");
            if (shutdown) SafeNvmlShutdown(shutdown);
        }
        SafeFreeLibrary(m_hNvml);
    }
    m_hNvml = nullptr;
    m_nvmlDevice = nullptr;
    m_nvmlInitialized = false;
}



void GpuMonitor::RefreshDriverHandles() {
    // A graphics driver install replaces nvml.dll / nvapi64.dll in System32.
    // Detect the swap via the files' last-write times, then reload and
    // re-initialize so the app keeps working with the new driver. NVML/NVAPI
    // also fail while the driver service is restarting, so keep retrying
    // (throttled to once every 3 s) until the driver is back.
    ULARGE_INTEGER nvmlTime = {};
    const bool nvmlExists = GetDllLastWriteTime(L"nvml.dll", &nvmlTime);
    const bool nvmlChanged = nvmlExists && m_nvmlDllTimeValid &&
                             nvmlTime.QuadPart != m_nvmlDllTime.QuadPart;
    if (nvmlExists) { m_nvmlDllTime = nvmlTime; m_nvmlDllTimeValid = true; }

    const ULONGLONG now = GetTickCount64();

    if (nvmlExists && (m_nvmlBroken || nvmlChanged || !m_nvmlInitialized) &&
        now - m_nvmlLastInitAttempt >= 3000) {
        m_nvmlLastInitAttempt = now;
        m_nvmlBroken = false;
        TeardownNvml();
        InitNvml();
    }
}



float GpuMonitor::GetGpuTempNvml() {
    if (!m_nvmlInitialized) return 0.0f;
    unsigned int temp = 0;
    auto getTemp = (pfnNvmlDeviceGetTemperature)SafeGetProcAddress(m_hNvml, "nvmlDeviceGetTemperature");
    if (getTemp && SafeNvmlGetTemperature(getTemp, (nvmlDevice_t)m_nvmlDevice, &temp)) {
        return (float)temp;
    }
    // Driver is being updated/unloaded: request a reload and let the caller
    // fall back to WMI for this tick.
    m_nvmlBroken = true;
    return 0.0f;
}

bool GpuMonitor::InitWmi() {
    HRESULT hr = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hr)) return false;
    hr = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE) return false;
    m_wmiInitialized = true;
    return true;
}

void GpuMonitor::CleanupWmi() {
    if (m_wmiInitialized) CoUninitialize();
}



float GpuMonitor::GetGpuTempWmi() {
    float temp = 0.0f;
    if (!m_wmiInitialized) return 0.0f;

    IWbemLocator* pLoc = NULL;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc);
    if (SUCCEEDED(hr)) {
        IWbemServices* pSvc = NULL;
        if (SUCCEEDED(pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, 0, NULL, 0, 0, &pSvc))) {
            IEnumWbemClassObject* pEnumerator = NULL;
            // Many integrated/standard GPUs report via CIMV2 WMI classes
            if (SUCCEEDED(pSvc->ExecQuery(_bstr_t("WQL"), _bstr_t("SELECT CurrentTemperature FROM Win32_VideoController"), 0, NULL, &pEnumerator))) {
                IWbemClassObject* pclsObj = NULL;
                ULONG uReturn = 0;
                if (SUCCEEDED(pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn)) && uReturn > 0) {
                    VARIANT vtProp;
                    if (SUCCEEDED(pclsObj->Get(L"CurrentTemperature", 0, &vtProp, 0, 0))) {
                        if (vtProp.vt != VT_NULL) {
                            if (vtProp.vt == VT_UI4) temp = (float)vtProp.uintVal;
                            else if (vtProp.vt == VT_I4) temp = (float)vtProp.lVal;
                            if (temp > 200) temp /= 10.0f; // Handle Celsius * 10
                        }
                        VariantClear(&vtProp);
                    }
                    pclsObj->Release();
                }
                pEnumerator->Release();
            }
            pSvc->Release();
        }
        pLoc->Release();
    }
    return temp;
}
