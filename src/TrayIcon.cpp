#include "TrayIcon.h"
#include <shellapi.h>
#include <string>
#include <sstream>
#include <algorithm>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

TrayIcon::TrayIcon(HWND hWnd, GpuMonitor* /*monitor*/) : m_hWnd(hWnd) {
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, NULL);
}

TrayIcon::~TrayIcon() {
    if (m_iconAdded) {
        Shell_NotifyIcon(NIM_DELETE, &m_nid);
    }
    if (m_nid.hIcon) {
        DestroyIcon(m_nid.hIcon);
    }
    GdiplusShutdown(m_gdiplusToken);
}

bool TrayIcon::Initialize() {
    m_nid.cbSize = sizeof(NOTIFYICONDATA);
    m_nid.hWnd = m_hWnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_APP + 1;
    m_nid.hIcon = CreateDynamicIcon(SystemStats{});
    wcscpy_s(m_nid.szTip, L"GPU Tray Monitor");

    m_iconAdded = Shell_NotifyIcon(NIM_ADD, &m_nid) != FALSE;
    if (!m_iconAdded && m_nid.hIcon) {
        DestroyIcon(m_nid.hIcon);
        m_nid.hIcon = nullptr;
    }
    return m_iconAdded;
}

bool TrayIcon::RestoreAfterExplorerRestart() {
    if (!m_nid.hWnd || !m_nid.hIcon) {
        return false;
    }

    // Explorer removes notification icons when it exits. Re-add the current
    // icon and restore all fields needed for future tray callbacks.
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_iconAdded = Shell_NotifyIcon(NIM_ADD, &m_nid) != FALSE;
    return m_iconAdded;
}

void TrayIcon::Update(const SystemStats& stats) {
    ULONGLONG now = GetTickCount64();
    if (now - m_digitTick >= 2000) {
        m_digitTick = now;
        m_digitCycle = (m_digitCycle + 1) % 3;
    }

    HICON hOldIcon = m_nid.hIcon;
    m_nid.hIcon = CreateDynamicIcon(stats);

    wchar_t tip[64];
    swprintf_s(tip, L"GPU %d%% VRAM %d%% TEMP %dC", (int)stats.gpuUsage, (int)stats.gpuMemoryUsage, (int)stats.gpuTemp);
    wcscpy_s(m_nid.szTip, tip);

    m_nid.uFlags = NIF_ICON | NIF_TIP;
    Shell_NotifyIcon(NIM_MODIFY, &m_nid);

    if (hOldIcon) DestroyIcon(hOldIcon);
}

HICON TrayIcon::CreateDynamicIcon(const SystemStats& stats) {
    const int size = 16;

    float val = 0;
    Color c;
    switch(m_digitCycle) {
        case 0: val = stats.gpuUsage; c = Color(255, 200, 200, 100); break;
        case 1: val = stats.gpuMemoryUsage; c = Color(255, 100, 200, 255); break;
        default: val = stats.gpuTemp; c = Color(255, 255, 100, 50); break;
    }
    std::wstring text = std::to_wstring((int)val);

    Bitmap bitmap(size, size, PixelFormat32bppARGB);
    Graphics g(&bitmap);

    SolidBrush bg(Color(255, 0, 0, 0)); // Black background
    g.FillRectangle(&bg, 0, 0, size, size);

    Font font(L"Tahoma", 8, FontStyleRegular);
    SolidBrush brush(c);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(text.c_str(), -1, &font,
        RectF(0, 0, size, size), &fmt, &brush);

    HICON hIcon;
    bitmap.GetHICON(&hIcon);
    return hIcon;
}
