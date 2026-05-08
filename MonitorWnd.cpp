// ---------------------------------------------------------------------------
// MonitorWnd.cpp  –  Live Monitor dockable window
//
// Displays real-time audio health metrics in a compact, fully custom-painted
// panel.  Updated on a 250 ms timer.  Docking behaviour mirrors TransitionWnd.
// ---------------------------------------------------------------------------

#include "MonitorWnd.h"
#include "api.h"
#include "resource.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const UINT  k_TimerID    = 42;
static const UINT  k_TimerMs    = 250;  // poll interval (4 fps is plenty)
static const char* k_ExtSection = "reaper_transitions";
static const char* k_DockKey    = "monitor_docked";

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static HINSTANCE s_hInst = nullptr;
static HWND      s_hwnd  = nullptr;

// ---------------------------------------------------------------------------
// Metrics snapshot (written on timer, read on paint – both main thread)
// ---------------------------------------------------------------------------
struct MonitorMetrics
{
    double cpuFraction;   // 0.0 – 1.0  (system CPU, via GetSystemTimes)
    double ioLatencyMs;   // hardware I/O round-trip in ms
    double maxPdcMs;      // max FX chain PDC across all tracks in ms
    double roundTripMs;   // ioLatencyMs + maxPdcMs
    double sampleRate;    // e.g. 44100.0
    int    bufferSize;    // audio device block size in samples
    bool   audioDevOpen;  // false when no device is active
};

static MonitorMetrics s_m = {};

// ---------------------------------------------------------------------------
// CPU helper  (Windows only – system-wide idle/total fraction)
// ---------------------------------------------------------------------------
static double SampleCpu()
{
    static ULARGE_INTEGER sPrevIdle   = {};
    static ULARGE_INTEGER sPrevKernel = {};
    static ULARGE_INTEGER sPrevUser   = {};

    FILETIME ftIdle, ftKernel, ftUser;
    if (!GetSystemTimes(&ftIdle, &ftKernel, &ftUser))
        return 0.0;

    ULARGE_INTEGER idle, kernel, user;
    idle.LowPart    = ftIdle.dwLowDateTime;
    idle.HighPart   = ftIdle.dwHighDateTime;
    kernel.LowPart  = ftKernel.dwLowDateTime;
    kernel.HighPart = ftKernel.dwHighDateTime;
    user.LowPart    = ftUser.dwLowDateTime;
    user.HighPart   = ftUser.dwHighDateTime;

    ULONGLONG dIdle   = idle.QuadPart   - sPrevIdle.QuadPart;
    ULONGLONG dKernel = kernel.QuadPart - sPrevKernel.QuadPart;
    ULONGLONG dUser   = user.QuadPart   - sPrevUser.QuadPart;

    sPrevIdle   = idle;
    sPrevKernel = kernel;
    sPrevUser   = user;

    ULONGLONG total = dKernel + dUser;
    if (total == 0) return 0.0;

    double frac = 1.0 - (double)dIdle / (double)total;
    return frac < 0.0 ? 0.0 : (frac > 1.0 ? 1.0 : frac);
}

// ---------------------------------------------------------------------------
// Max PDC helper  – returns the maximum chain_pdc_reporting value in samples
// across all tracks (including master).
// ---------------------------------------------------------------------------
static int GetMaxChainPdcSamples()
{
    int maxPdc = 0;
    char buf[64] = {};

    auto checkTrack = [&](MediaTrack* tr)
    {
        if (!tr) return;
        int nfx = TrackFX_GetCount(tr);
        if (nfx <= 0) return;
        // chain_pdc_reporting is a property of the whole chain; querying FX 0 is sufficient.
        if (TrackFX_GetNamedConfigParm(tr, 0, "chain_pdc_reporting", buf, (int)sizeof(buf)))
        {
            int pdc = atoi(buf);
            if (pdc > maxPdc) maxPdc = pdc;
        }
    };

    checkTrack(GetMasterTrack(nullptr));
    const int n = GetNumTracks();
    for (int i = 0; i < n; ++i)
        checkTrack(GetTrack(nullptr, i));

    return maxPdc;
}

// ---------------------------------------------------------------------------
// Metrics poll  (called from WM_TIMER on the main thread)
// ---------------------------------------------------------------------------
static void PollMetrics()
{
    // --- Sample rate + buffer size ---
    char srateBuf[32] = {}, bsizeBuf[32] = {};
    bool devOpen = GetAudioDeviceInfo("SRATE", srateBuf, (int)sizeof(srateBuf))
                && GetAudioDeviceInfo("BSIZE", bsizeBuf, (int)sizeof(bsizeBuf));

    s_m.audioDevOpen = devOpen;
    s_m.sampleRate   = devOpen ? atof(srateBuf) : 48000.0;
    if (s_m.sampleRate < 1.0) s_m.sampleRate = 48000.0;
    s_m.bufferSize   = devOpen ? atoi(bsizeBuf) : 0;

    // --- System CPU ---
    s_m.cpuFraction = SampleCpu();

    // --- Hardware I/O latency ---
    int inSpl = 0, outSpl = 0;
    GetInputOutputLatency(&inSpl, &outSpl);
    s_m.ioLatencyMs = (inSpl + outSpl) * 1000.0 / s_m.sampleRate;

    // --- Max FX chain PDC ---
    int pdcSpl = GetMaxChainPdcSamples();
    s_m.maxPdcMs = pdcSpl * 1000.0 / s_m.sampleRate;

    // --- Round-trip ---
    s_m.roundTripMs = s_m.ioLatencyMs + s_m.maxPdcMs;
}

// ---------------------------------------------------------------------------
// Color mapping helpers
// ---------------------------------------------------------------------------
static COLORREF CpuColor(double frac)
{
    if (frac < 0.50) return RGB(32, 178,  32);   // green
    if (frac < 0.70) return RGB(220, 192,   0);  // yellow
    if (frac < 0.85) return RGB(255, 112,   0);  // orange
    return                   RGB(210,   0,   0); // red
}

static COLORREF IoColor(double ms)
{
    if (ms < 10.0) return RGB(32, 178,  32);
    if (ms < 20.0) return RGB(220, 192,   0);
    if (ms < 40.0) return RGB(255, 112,   0);
    return                RGB(210,   0,   0);
}

static COLORREF PdcColor(double ms)
{
    if (ms < 50.0)  return RGB(32, 178,  32);
    if (ms < 100.0) return RGB(220, 192,   0);
    if (ms < 200.0) return RGB(255, 112,   0);
    return                 RGB(210,   0,   0);
}

static COLORREF RtColor(double ms)
{
    if (ms < 20.0)  return RGB(32, 178,  32);
    if (ms < 50.0)  return RGB(220, 192,   0);
    if (ms < 100.0) return RGB(255, 112,   0);
    return                 RGB(210,   0,   0);
}

// ---------------------------------------------------------------------------
// Paint helper  – draw one metric row
//
//   rc      : full-width rect for this row
//   strip   : width of the colored left strip in pixels
//   color   : severity color
//   label   : left-aligned label text
//   value   : right-aligned value text
// ---------------------------------------------------------------------------
static void DrawRow(HDC hdc, RECT rc, int strip, COLORREF color,
                    const char* label, const char* value)
{
    // Colored severity strip on the left
    RECT rStrip = { rc.left, rc.top, rc.left + strip, rc.bottom };
    HBRUSH hbr = CreateSolidBrush(color);
    FillRect(hdc, &rStrip, hbr);
    DeleteObject(hbr);

    // Label (left, padded past the strip)
    RECT rLabel = { rc.left + strip + 4, rc.top, rc.right, rc.bottom };
    SetTextColor(hdc, RGB(200, 200, 200));
    DrawTextA(hdc, label, -1, &rLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Value (right-aligned)
    RECT rVal = { rc.left + strip + 4, rc.top, rc.right - 4, rc.bottom };
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextA(hdc, value, -1, &rVal, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

// ---------------------------------------------------------------------------
// WM_PAINT  – entirely custom GDI; no default drawing
// ---------------------------------------------------------------------------
static void OnPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rcClient;
    GetClientRect(hwnd, &rcClient);
    const int W = rcClient.right;
    const int H = rcClient.bottom;

    // --- Double-buffer to eliminate flicker ---
    HDC     hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbm    = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbm);

    // Background
    HBRUSH hbrBg = CreateSolidBrush(RGB(38, 38, 38));
    FillRect(hdcMem, &rcClient, hbrBg);
    DeleteObject(hbrBg);

    // Font: small, non-bold
    HFONT hFont = CreateFontA(
        -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HFONT hOldFont = (HFONT)SelectObject(hdcMem, hFont);
    SetBkMode(hdcMem, TRANSPARENT);

    // Layout: 5 rows of equal height, plus 2px top/bottom padding
    const int kPadTop  = 4;
    const int kPadBot  = 4;
    const int kRows    = 5;
    const int kSepH    = 1;    // separator line height
    const int kStrip   = 7;    // left colored strip width
    const int usable   = H - kPadTop - kPadBot - kSepH * (kRows - 1);
    const int rowH     = (usable > kRows * 4) ? usable / kRows : 16;

    // Separator color (slightly lighter than background)
    HBRUSH hbrSep = CreateSolidBrush(RGB(60, 60, 60));

    char valueBuf[64];

    // Helper lambda: compute y-top of row i (0-based)
    auto rowTop = [&](int i) -> int {
        return kPadTop + i * (rowH + kSepH);
    };

    auto drawSep = [&](int i) {
        RECT rSep = { 0, rowTop(i) - kSepH, W, rowTop(i) };
        FillRect(hdcMem, &rSep, hbrSep);
    };

    // ---- Row 0: System CPU ----
    {
        RECT rc = { 0, rowTop(0), W, rowTop(0) + rowH };
        double cpu = s_m.cpuFraction * 100.0;
        snprintf(valueBuf, sizeof(valueBuf), "%.0f%%", cpu);
        DrawRow(hdcMem, rc, kStrip, CpuColor(s_m.cpuFraction), "System CPU", valueBuf);
    }

    // ---- Row 1: I/O Latency ----
    drawSep(1);
    {
        RECT rc = { 0, rowTop(1), W, rowTop(1) + rowH };
        snprintf(valueBuf, sizeof(valueBuf), "%.1f ms", s_m.ioLatencyMs);
        DrawRow(hdcMem, rc, kStrip, IoColor(s_m.ioLatencyMs), "I/O Latency", valueBuf);
    }

    // ---- Row 2: Max FX PDC ----
    drawSep(2);
    {
        RECT rc = { 0, rowTop(2), W, rowTop(2) + rowH };
        snprintf(valueBuf, sizeof(valueBuf), "%.1f ms", s_m.maxPdcMs);
        DrawRow(hdcMem, rc, kStrip, PdcColor(s_m.maxPdcMs), "Max FX PDC", valueBuf);
    }

    // ---- Row 3: Round-Trip ----
    drawSep(3);
    {
        RECT rc = { 0, rowTop(3), W, rowTop(3) + rowH };
        snprintf(valueBuf, sizeof(valueBuf), "%.1f ms", s_m.roundTripMs);
        DrawRow(hdcMem, rc, kStrip, RtColor(s_m.roundTripMs), "Round-Trip", valueBuf);
    }

    // ---- Row 4: Device info (no color indicator) ----
    drawSep(4);
    {
        RECT rc = { 0, rowTop(4), W, rowTop(4) + rowH };
        if (s_m.audioDevOpen)
            snprintf(valueBuf, sizeof(valueBuf), "%d Hz / %d spl",
                     (int)s_m.sampleRate, s_m.bufferSize);
        else
            snprintf(valueBuf, sizeof(valueBuf), "No audio device");

        // Thin neutral strip
        RECT rStrip = { 0, rc.top, kStrip, rc.bottom };
        HBRUSH hbrN = CreateSolidBrush(RGB(80, 80, 80));
        FillRect(hdcMem, &rStrip, hbrN);
        DeleteObject(hbrN);

        RECT rTxt = { kStrip + 4, rc.top, W - 4, rc.bottom };
        SetTextColor(hdcMem, RGB(150, 150, 150));
        DrawTextA(hdcMem, valueBuf, -1, &rTxt,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(hdcMem, RGB(150, 150, 150));
        DrawTextA(hdcMem, "Device", -1, &rTxt,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    DeleteObject(hbrSep);
    SelectObject(hdcMem, hOldFont);
    DeleteObject(hFont);

    // Blit to screen
    BitBlt(hdc, 0, 0, W, H, hdcMem, 0, 0, SRCCOPY);
    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbm);
    DeleteDC(hdcMem);

    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------
// Dialog procedure
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK MonitorDlgProc(HWND hwnd, UINT msg,
                                        WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        // Prime metrics immediately so first paint has real data
        PollMetrics();
        SetTimer(hwnd, k_TimerID, k_TimerMs, nullptr);
        return TRUE;

    case WM_TIMER:
        if (wParam == k_TimerID)
        {
            PollMetrics();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return TRUE;

    case WM_ERASEBKGND:
        return 1;   // suppress default erase; we fill everything in WM_PAINT

    case WM_PAINT:
        OnPaint(hwnd);
        return TRUE;

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return FALSE;

    case WM_DESTROY:
        KillTimer(hwnd, k_TimerID);
        return FALSE;

    case WM_CLOSE:
        // Save dock state before closing
        {
            bool isFloat = false;
            bool isDocked = (DockIsChildOfDock(hwnd, &isFloat) >= 0);
            SetExtState(k_ExtSection, k_DockKey, isDocked ? "1" : "0", true);
        }
        if (DockIsChildOfDock(hwnd, nullptr) >= 0)
            DockWindowRemove(hwnd);
        DestroyWindow(hwnd);
        s_hwnd = nullptr;
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void MonitorWnd_Init(HINSTANCE hInst)
{
    s_hInst = hInst;
}

void MonitorWnd_Cleanup()
{
    if (s_hwnd && IsWindow(s_hwnd))
    {
        bool isFloat = false;
        if (DockIsChildOfDock(s_hwnd, &isFloat) >= 0)
            DockWindowRemove(s_hwnd);
        DestroyWindow(s_hwnd);
        s_hwnd = nullptr;
    }
}

void MonitorWnd_ShowHide()
{
    if (!s_hwnd || !IsWindow(s_hwnd))
    {
        HWND hMain = GetMainHwnd();
        s_hwnd = CreateDialogParam(s_hInst,
                                   MAKEINTRESOURCE(IDD_MONITOR),
                                   hMain,
                                   MonitorDlgProc,
                                   0);
        if (!s_hwnd)
        {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "CreateDialogParam failed for IDD_MONITOR. Error=%lu",
                     GetLastError());
            MessageBoxA(hMain, buf, "Live Tools", MB_OK | MB_ICONERROR);
            return;
        }

        const char* dockPref = GetExtState(k_ExtSection, k_DockKey);
        bool wantDocked = (dockPref && atoi(dockPref) != 0);
        DockWindowAddEx(s_hwnd, "Live Monitor", "reaper_trans_monitor", wantDocked);
        if (!wantDocked)
            ShowWindow(s_hwnd, SW_SHOW);
        else
            DockWindowActivate(s_hwnd);
        return;
    }

    // Window already exists – activate if docked, else toggle visibility
    bool isFloat = false;
    if (DockIsChildOfDock(s_hwnd, &isFloat) >= 0)
    {
        DockWindowActivate(s_hwnd);
    }
    else
    {
        if (IsWindowVisible(s_hwnd))
            ShowWindow(s_hwnd, SW_HIDE);
        else
            ShowWindow(s_hwnd, SW_SHOW);
    }
}

int MonitorWnd_IsVisible()
{
    if (!s_hwnd || !IsWindow(s_hwnd)) return 0;
    bool isFloat = false;
    if (DockIsChildOfDock(s_hwnd, &isFloat) >= 0) return 1;
    return IsWindowVisible(s_hwnd) ? 1 : 0;
}
