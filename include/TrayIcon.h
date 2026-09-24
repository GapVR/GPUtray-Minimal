#pragma once

#include <windows.h>
#include <gdiplus.h>
#include "GpuMonitor.h"

class TrayIcon {
public:
    TrayIcon(HWND hWnd, GpuMonitor* monitor);
    ~TrayIcon();

    bool Initialize();
    bool RestoreAfterExplorerRestart();
    void Update(const SystemStats& stats);

private:
    HICON CreateDynamicIcon(const SystemStats& stats);

    HWND m_hWnd;
    NOTIFYICONDATA m_nid{};
    bool m_iconAdded = false;

    ULONG_PTR m_gdiplusToken;
    ULONGLONG m_digitTick = 0;
    int m_digitCycle = 0;
};
