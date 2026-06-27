// ==WindhawkMod==
// @id              borderless-fullscreen
// @name            Borderless Fullscreen
// @description     Removes the window border of selected apps/games and resizes them to the primary monitor's resolution (or a fixed size)
// @version         1.0.0
// @author          TomberWolf
// @github          https://github.com/TomberWolf
// @include         *
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Borderless Fullscreen

Add an executable name (e.g. `game.exe`) to the target list and the app
will automatically run without a title bar / border, stretched to fill
the primary monitor's current resolution.

Optionally, set a fixed window size instead (the window will then be
centered on the primary monitor) - useful for older games with a low
native resolution.

## How it works

This mod doesn't intercept window-creation APIs directly. Doing so can
interfere with a game's own startup routine - for example, a graphics
swap chain created right after the window appears often expects the
window to keep the size it was originally created with, and forcing a
resize at that exact moment can cause the game to crash, freeze, or
simply ignore the change.

Instead, the mod periodically checks the app's own windows in the
background and applies the borderless treatment once a matching window
is found and fully up and running. Because of this, there's a short
delay (up to ~1 second) after the app's window first appears before the
border actually disappears - this is expected, not a bug. The check
keeps repeating every few seconds afterward, so if the game later resets
its own window style (e.g. via an in-game windowed-mode toggle), the
borderless effect is automatically re-applied.

## Notes
- If a target doesn't react, double-check the *exact* process name in
  Task Manager's Details tab while it's running. Some launchers start a
  child process with a different executable name than expected.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- exeNames:
  - ""
  $name: Target Applications
  $description: >-
    Enter the executable file name (e.g. game.exe, no path) of each
    app/game that should run borderless. Click "+" to add more entries.
- windowSize:
  - customWidth: 0
    $name: Width
    $description: 0 = automatically use the primary monitor's current width
  - customHeight: 0
    $name: Height
    $description: 0 = automatically use the primary monitor's current height
  $name: Window Size
  $description: >-
    Leave both at 0 to automatically stretch to the primary monitor's
    current resolution. Set fixed values only if you want a smaller,
    centered window instead.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <atomic>
#include <string>
#include <vector>

namespace {

std::vector<std::wstring> g_targetExeNames;
int g_customWidth = 0;
int g_customHeight = 0;

std::atomic<bool> g_stopWorker{false};
HANDLE g_workerThread = nullptr;

bool GetCurrentExeName(std::wstring& outName) {
    WCHAR path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) {
        return false;
    }
    WCHAR* fileName = wcsrchr(path, L'\\');
    outName = fileName ? (fileName + 1) : path;
    return true;
}

bool IsCurrentProcessATarget() {
    std::wstring exeName;
    if (!GetCurrentExeName(exeName)) return false;

    for (const auto& target : g_targetExeNames) {
        if (_wcsicmp(target.c_str(), exeName.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

// Same criteria as NoMoreBorder's window enumeration: visible top-level
// windows with a non-empty title. A minimum size is added to filter out
// small tooltips/dialogs, since unlike NoMoreBorder, this mod applies
// automatically without a human picking the right window from a list.

bool IsEligibleMainWindow(HWND hWnd) {
    if (!IsWindowVisible(hWnd)) return false;
    if (GetWindowTextLengthW(hWnd) == 0) return false;

    RECT rc;
    if (!GetWindowRect(hWnd, &rc)) return false;
    if ((rc.right - rc.left) < 200 || (rc.bottom - rc.top) < 150) return false;

    return true;
}

// Mirrors NoMoreBorder's enhanced borderless function: strip the
// decoration-related style/ex-style bits, apply the frame change first,
// then move/resize in a separate step.

void ApplyBorderless(HWND hWnd) {
    LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);

    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZE | WS_MAXIMIZE |
               WS_SYSMENU);
    exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE |
                 WS_EX_STATICEDGE);

    SetWindowLongPtrW(hWnd, GWL_STYLE, style);
    SetWindowLongPtrW(hWnd, GWL_EXSTYLE, exStyle);

    SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    int width = g_customWidth;
    int height = g_customHeight;
    int x, y;

    if (width <= 0 || height <= 0) {
        width = screenW;
        height = screenH;
        x = 0;
        y = 0;
    } else {
        x = (screenW - width) / 2;
        y = (screenH - height) / 2;
    }

    Wh_Log(L"Applying borderless to HWND %p (%dx%d at %d,%d)", hWnd, width,
           height, x, y);

    SetWindowPos(hWnd, nullptr, x, y, width, height,
                 SWP_NOZORDER | SWP_FRAMECHANGED);
}

BOOL CALLBACK EnumWindowsProc(HWND hWnd, LPARAM lParam) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;

    if (IsEligibleMainWindow(hWnd)) {
        auto* matches = reinterpret_cast<std::vector<HWND>*>(lParam);
        matches->push_back(hWnd);
    }

    return TRUE;
}

DWORD WINAPI WorkerThreadProc(LPVOID) {
    // Give the app a moment to create its window(s) before the first pass.
    Sleep(1000);

    while (!g_stopWorker.load()) {
        std::vector<HWND> matches;
        EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&matches));

        for (HWND hWnd : matches) {
            ApplyBorderless(hWnd);
        }

        // Re-check every ~5 seconds (same interval as NoMoreBorder) in case
        // the app resets its own window style later on.
        for (int i = 0; i < 50 && !g_stopWorker.load(); i++) {
            Sleep(100);
        }
    }

    return 0;
}

void LoadSettings() {
    g_targetExeNames.clear();
    for (int i = 0;; i++) {
        PCWSTR value = Wh_GetStringSetting(L"exeNames[%d]", i);
        bool empty = !value || !value[0];
        if (!empty) {
            g_targetExeNames.push_back(value);
        }
        Wh_FreeStringSetting(value);
        if (empty) break;
    }

    g_customWidth = Wh_GetIntSetting(L"windowSize.customWidth");
    g_customHeight = Wh_GetIntSetting(L"windowSize.customHeight");
}

}  // 

BOOL Wh_ModInit() {
    LoadSettings();

    if (!IsCurrentProcessATarget()) {
        return FALSE;
    }

    g_stopWorker = false;
    g_workerThread = CreateThread(nullptr, 0, WorkerThreadProc, nullptr, 0,
                                   nullptr);

    return TRUE;
}

void Wh_ModUninit() {
    g_stopWorker = true;
    if (g_workerThread) {
        WaitForSingleObject(g_workerThread, 2000);
        CloseHandle(g_workerThread);
        g_workerThread = nullptr;
    }
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}
