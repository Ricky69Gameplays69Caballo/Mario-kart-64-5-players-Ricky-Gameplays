#include "MultiplayerDisplays.h"
#include "LocalMultiplayer.h"
#include <algorithm>
#include <array>
#include <vector>
#include <libultraship.h>
#include <fast/Fast3dGui.h>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {
int applied = 1;
int lastRequest = -1;
const char* status = "One screen";
struct Panel { int left, top, right, bottom; };
std::array<Panel, 3> panels{};
#ifdef _WIN32
HWND gameWindow = nullptr;
LONG_PTR savedStyle = 0;
bool savedFullscreen = false;
WINDOWPLACEMENT savedPlacement{sizeof(WINDOWPLACEMENT)};
BOOL CALLBACK FindWindow(HWND window, LPARAM) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process == GetCurrentProcessId() && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) {
        gameWindow = window;
        return FALSE;
    }
    return TRUE;
}
BOOL CALLBACK FindMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    MONITORINFO info{sizeof(MONITORINFO)};
    if (GetMonitorInfo(monitor, &info)) reinterpret_cast<std::vector<RECT>*>(data)->push_back(info.rcMonitor);
    return TRUE;
}
void Restore() {
    if (applied <= 1 || !IsWindow(gameWindow)) return;
    SetWindowLongPtr(gameWindow, GWL_STYLE, savedStyle);
    SetWindowPlacement(gameWindow, &savedPlacement);
    SetWindowPos(gameWindow, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (savedFullscreen) Ship::Context::GetRawInstance()->GetWindow()->SetFullscreen(true);
    auto gui = std::dynamic_pointer_cast<Fast::Fast3dGui>(Ship::Context::GetRawInstance()->GetWindow()->GetGui());
    if (gui) gui->SetDisplaySpanActive(false);
}
#endif
int LogicalEdge(int value, int extent, int pixels) {
    return ((value * extent + pixels) / (2 * pixels)) * 2;
}
}

extern "C" void MultiplayerDisplays_Update(void) {
    const int wanted = LocalMP_IsActive() ? std::min(MultiplayerOptions_DisplayCount(), LocalMP_Count()) : 1;
    if (wanted == lastRequest) return;
    lastRequest = wanted;
#ifdef _WIN32
    Restore();
    applied = 1;
    if (wanted == 1) { status = "One screen"; return; }
    if (!IsWindow(gameWindow)) { gameWindow = nullptr; EnumWindows(FindWindow, 0); }
    if (!gameWindow) { status = "Span unavailable: game window was not found"; return; }
    std::vector<RECT> monitors;
    EnumDisplayMonitors(nullptr, nullptr, FindMonitor, reinterpret_cast<LPARAM>(&monitors));
    if (monitors.size() < static_cast<size_t>(wanted)) {
        status = "Connect the selected screens in Windows extended desktop mode";
        return;
    }
    std::sort(monitors.begin(), monitors.end(), [](const RECT& a, const RECT& b) {
        return a.left == b.left ? a.top < b.top : a.left < b.left;
    });
    // Prefer a consecutive set containing the game's current display.
    RECT windowRect{};
    GetWindowRect(gameWindow, &windowRect);
    int start = 0;
    const int center = (windowRect.left + windowRect.right) / 2;
    for (int i = 0; i < static_cast<int>(monitors.size()); ++i)
        if (center >= monitors[i].left && center < monitors[i].right)
            start = std::min(i, static_cast<int>(monitors.size()) - wanted);
    RECT bounds = monitors[start];
    for (int i = 1; i < wanted; ++i) {
        const auto& monitor = monitors[start + i];
        bounds.left = std::min(bounds.left, monitor.left);
        bounds.top = std::min(bounds.top, monitor.top);
        bounds.right = std::max(bounds.right, monitor.right);
        bounds.bottom = std::max(bounds.bottom, monitor.bottom);
    }
    for (int i = 0; i < wanted; ++i) {
        const auto& monitor = monitors[start + i];
        panels[i] = {LogicalEdge(monitor.left - bounds.left, 320, bounds.right - bounds.left),
                     LogicalEdge(monitor.top - bounds.top, 240, bounds.bottom - bounds.top),
                     LogicalEdge(monitor.right - bounds.left, 320, bounds.right - bounds.left),
                     LogicalEdge(monitor.bottom - bounds.top, 240, bounds.bottom - bounds.top)};
    }
    savedStyle = GetWindowLongPtr(gameWindow, GWL_STYLE);
    GetWindowPlacement(gameWindow, &savedPlacement);
    savedFullscreen = Ship::Context::GetRawInstance()->GetWindow()->IsFullscreen();
    if (savedFullscreen) Ship::Context::GetRawInstance()->GetWindow()->SetFullscreen(false);
    // The same render surface spans the selected displays, so controllers,
    // race simulation and frame timing remain shared across all players.
    SetWindowLongPtr(gameWindow, GWL_STYLE, (savedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
    if (!SetWindowPos(gameWindow, HWND_NOTOPMOST, bounds.left, bounds.top,
                      bounds.right - bounds.left, bounds.bottom - bounds.top,
                      SWP_FRAMECHANGED | SWP_NOACTIVATE)) {
        SetWindowLongPtr(gameWindow, GWL_STYLE, savedStyle);
        SetWindowPlacement(gameWindow, &savedPlacement);
        if (savedFullscreen) Ship::Context::GetRawInstance()->GetWindow()->SetFullscreen(true);
        status = "Could not span the game window";
        return;
    }
    applied = wanted;
    // Fill the span and cap its render surface without changing saved graphics preferences.
    auto gui = std::dynamic_pointer_cast<Fast::Fast3dGui>(Ship::Context::GetRawInstance()->GetWindow()->GetGui());
    if (gui) gui->SetDisplaySpanActive(true);
    status = wanted == 2 ? "Game window spans two screens" : "Game window spans three screens";
#else
    status = wanted == 1 ? "One screen" : "Extend the game window across the selected screens";
    applied = 1;
#endif
}

extern "C" void MultiplayerDisplays_Panel(int index, int count, int* left, int* top, int* right, int* bottom) {
    if (applied == count && count > 1 && index >= 0 && index < count) {
        const auto& panel = panels[index];
        *left = panel.left; *top = panel.top; *right = panel.right; *bottom = panel.bottom;
    } else {
        *left = LogicalEdge(index, 320, count); *right = LogicalEdge(index + 1, 320, count);
        *top = 0; *bottom = 240;
    }
}
extern "C" const char* MultiplayerDisplays_Status(void) { return status; }
extern "C" void MultiplayerDisplays_Refresh(void) { lastRequest = -1; }
extern "C" void MultiplayerDisplays_Stop(void) {
#ifdef _WIN32
    Restore();
#endif
    applied = 1;
    lastRequest = -1;
}
