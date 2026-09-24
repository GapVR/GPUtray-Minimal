#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <cwchar>
#include "GpuMonitor.h"
#include "TrayIcon.h"
#include "resource.h"

// Globals
GpuMonitor* g_monitor = nullptr;
TrayIcon* g_trayIcon = nullptr;
UINT g_taskbarCreatedMessage = 0;

LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (g_taskbarCreatedMessage != 0 && uMsg == g_taskbarCreatedMessage) {
        if (g_trayIcon) {
            g_trayIcon->RestoreAfterExplorerRestart();
        }
        return 0;
    }

    switch (uMsg) {
    case WM_APP + 1: // Tray message
    {
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Exit");
            SetForegroundWindow(hWnd);
            int sel = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(menu);
            if (sel == 1) {
                KillTimer(hWnd, 1);
                PostQuitMessage(0);
            }
        }
        return 0;
    }
    case WM_TIMER:
    {
        if (wParam == 1) {
            SystemStats stats = g_monitor->Update();
            g_trayIcon->Update(stats);
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR, int nCmdShow) {
    // Single instance guard: allow only one tray instance (helpers above may still run).
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\GpuTraySingleInstance");
    if (hMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    // Hidden window to handle messages
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    if (g_taskbarCreatedMessage == 0) return 0;

    const wchar_t CLASS_NAME[] = L"GpuTrayHiddenWindow";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);

    HWND hWnd = CreateWindowExW(0, CLASS_NAME, L"GpuTray", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (hWnd == NULL) return 0;

    g_monitor = new GpuMonitor();
    if (!g_monitor->Initialize()) {
        MessageBox(NULL, L"Failed to initialize GPU Monitor", L"Error", MB_ICONERROR);
        return 0;
    }

    g_trayIcon = new TrayIcon(hWnd, g_monitor);
    if (!g_trayIcon->Initialize()) return 0;

    // 1s Refresh Timer
    SetTimer(hWnd, 1, 2000, NULL);

    // Message Loop
    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    delete g_trayIcon;
    delete g_monitor;
    CloseHandle(hMutex);

    return 0;
}
