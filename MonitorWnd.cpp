// ---------------------------------------------------------------------------
// MonitorWnd.cpp  –  Live Monitor dockable window
//
// Displays real-time audio health metrics in a compact, fully custom-painted
// panel.  Updated on a 250 ms timer.  Docking behaviour mirrors TransitionWnd.
// ---------------------------------------------------------------------------

#include "MonitorWnd.h"
#include "api.h"
#include "resource.h"

#include <windowsx.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const UINT  k_TimerID    = 42;
static const UINT  k_TimerMs    = 250;
static const char* k_ExtSection = "reaper_transitions";
static const char* k_DockKey    = "monitor_docked";
static const char* k_ThrKey     = "monitor_thresholds";

// ---------------------------------------------------------------------------
// Thresholds  (yellow / orange / red breakpoints per metric)
// ---------------------------------------------------------------------------
struct Thresholds
{
    double cpuYel, cpuOra, cpuRed;   // 0.0–1.0 fractions
    double ioYel,  ioOra,  ioRed;    // ms
    double pdcYel, pdcOra, pdcRed;   // ms
    double rtYel,  rtOra,  rtRed;    // ms
    double rcYel,  rcOra,  rcRed;    // RT CPU, 0.0–1.0 fractions
};

static const Thresholds k_DefThr = {
    0.50, 0.70, 0.85,
    10.0, 20.0, 40.0,
    50.0, 100.0, 200.0,
    20.0, 50.0,  100.0,
    0.50, 0.75,  0.90,
};
static Thresholds s_thr = k_DefThr;

static void SaveThresholds()
{
    char buf[256];
    snprintf(buf, sizeof(buf),
        "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f",
        s_thr.cpuYel, s_thr.cpuOra, s_thr.cpuRed,
        s_thr.ioYel,  s_thr.ioOra,  s_thr.ioRed,
        s_thr.pdcYel, s_thr.pdcOra, s_thr.pdcRed,
        s_thr.rtYel,  s_thr.rtOra,  s_thr.rtRed,
        s_thr.rcYel,  s_thr.rcOra,  s_thr.rcRed);
    SetExtState(k_ExtSection, k_ThrKey, buf, true);
}

static void LoadThresholds()
{
    const char* s = GetExtState(k_ExtSection, k_ThrKey);
    if (!s || !*s) return;
    double v[15] = {};
    int n = sscanf(s,
        "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf",
        &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],
        &v[6],&v[7],&v[8],&v[9],&v[10],&v[11],
        &v[12],&v[13],&v[14]);
    if (n >= 12)
    {
        s_thr.cpuYel = v[0]; s_thr.cpuOra = v[1];  s_thr.cpuRed = v[2];
        s_thr.ioYel  = v[3]; s_thr.ioOra  = v[4];  s_thr.ioRed  = v[5];
        s_thr.pdcYel = v[6]; s_thr.pdcOra = v[7];  s_thr.pdcRed = v[8];
        s_thr.rtYel  = v[9]; s_thr.rtOra  = v[10]; s_thr.rtRed  = v[11];
    }
    if (n >= 15)
    {
        s_thr.rcYel = v[12]; s_thr.rcOra = v[13]; s_thr.rcRed = v[14];
    }
}

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static HINSTANCE s_hInst    = nullptr;
static HWND      s_hwnd     = nullptr;
static RECT      s_gearRect              = {};   // client-coord rect of the settings row
static bool      s_suppressDockStateSave  = false;

// ---------------------------------------------------------------------------
// RT CPU measurement via audio hook  (OnAudioBuffer runs on audio thread)
// ---------------------------------------------------------------------------
static audio_hook_register_t s_audioHook     = {};
static LARGE_INTEGER         s_qpcFreq       = {};
static LARGE_INTEGER         s_blockStart    = {};
static volatile double       s_rtCpuFraction = 0.0;

static void OnAudioBuffer(bool isPost, int len, double srate,
                          audio_hook_register_t* /*reg*/)
{
    if (!isPost)
    {
        QueryPerformanceCounter(&s_blockStart);
    }
    else
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (s_qpcFreq.QuadPart > 0 && srate > 0.0 && len > 0)
        {
            double elapsed = (double)(now.QuadPart - s_blockStart.QuadPart)
                             / (double)s_qpcFreq.QuadPart;
            double period  = (double)len / srate;
            double frac    = elapsed / period;
            if (frac < 0.0) frac = 0.0;
            if (frac > 2.0) frac = 2.0;  // cap at 200%
            // Exponential moving average (250 ms timer reads this safely)
            s_rtCpuFraction = s_rtCpuFraction * 0.85 + frac * 0.15;
        }
    }
}

// ---------------------------------------------------------------------------
// Metrics snapshot (written on timer, read on paint – both main thread)
// ---------------------------------------------------------------------------
struct MonitorMetrics
{
    double cpuFraction;    // 0.0 – 1.0  (system CPU, via GetSystemTimes)
    double rtCpuFraction;  // 0.0 – 1.0+ (audio-thread load via hook timing)
    double ioLatencyMs;    // hardware I/O round-trip in ms
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

    // --- RT CPU from audio hook ---
    s_m.rtCpuFraction = s_rtCpuFraction;

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
// Color helpers
// ---------------------------------------------------------------------------
static COLORREF ColorGrade(double v, double y, double o, double r)
{
    if (v < y) return RGB(32, 178, 32);
    if (v < o) return RGB(220, 192, 0);
    if (v < r) return RGB(255, 112, 0);
    return          RGB(210, 0, 0);
}

static void DrawRow(HDC hdc, RECT rc, int strip, COLORREF color,
                    const char* label, const char* value)
{
    RECT rStrip = { rc.left, rc.top, rc.left + strip, rc.bottom };
    HBRUSH hbr = CreateSolidBrush(color);
    FillRect(hdc, &rStrip, hbr);
    DeleteObject(hbr);

    RECT rLabel = { rc.left + strip + 3, rc.top, rc.right, rc.bottom };
    SetTextColor(hdc, RGB(190, 190, 190));
    DrawTextA(hdc, label, -1, &rLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT rVal = { rc.left + strip + 3, rc.top, rc.right - 3, rc.bottom };
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextA(hdc, value, -1, &rVal, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

// ---------------------------------------------------------------------------
// WM_PAINT  – compact custom GDI, double-buffered
// ---------------------------------------------------------------------------
static void OnPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rcClient;
    GetClientRect(hwnd, &rcClient);
    const int W = rcClient.right;
    const int H = rcClient.bottom;

    HDC     hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbm    = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbm);

    HBRUSH hbrBg = CreateSolidBrush(RGB(30, 30, 30));
    FillRect(hdcMem, &rcClient, hbrBg);
    DeleteObject(hbrBg);

    HFONT hFont = CreateFontA(
        -11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HFONT hOldFont = (HFONT)SelectObject(hdcMem, hFont);
    SetBkMode(hdcMem, TRANSPARENT);

    const int kStrip = 4;
    const int kSep   = 1;
    const int kN     = 6;
    const int rowH   = (H - kSep * (kN - 1)) / kN;
    auto rowY0 = [&](int i) { return i * (rowH + kSep); };

    HBRUSH hbrSep = CreateSolidBrush(RGB(48, 48, 48));

    char v0[24], v1[24], v2[24], v3[24], v4[24];
    snprintf(v0, sizeof(v0), "%.0f%%",  s_m.cpuFraction   * 100.0);
    snprintf(v4, sizeof(v4), "%.0f%%",  s_m.rtCpuFraction * 100.0);
    snprintf(v1, sizeof(v1), "%.1f ms", s_m.ioLatencyMs);
    snprintf(v2, sizeof(v2), "%.1f ms", s_m.maxPdcMs);
    snprintf(v3, sizeof(v3), "%.1f ms", s_m.roundTripMs);

    struct RowData { const char* label; COLORREF col; const char* val; };
    RowData rows[5] = {
        { "CPU",        ColorGrade(s_m.cpuFraction,    s_thr.cpuYel, s_thr.cpuOra, s_thr.cpuRed), v0 },
        { "RT CPU",     ColorGrade(s_m.rtCpuFraction,  s_thr.rcYel,  s_thr.rcOra,  s_thr.rcRed),  v4 },
        { "I/O",        ColorGrade(s_m.ioLatencyMs,    s_thr.ioYel,  s_thr.ioOra,  s_thr.ioRed),  v1 },
        { "PDC",        ColorGrade(s_m.maxPdcMs,       s_thr.pdcYel, s_thr.pdcOra, s_thr.pdcRed), v2 },
        { "Round-Trip", ColorGrade(s_m.roundTripMs,    s_thr.rtYel,  s_thr.rtOra,  s_thr.rtRed),  v3 },
    };

    for (int i = 0; i < 5; i++)
    {
        int y0 = rowY0(i);
        if (i > 0)
        {
            RECT rs = { 0, y0 - kSep, W, y0 };
            FillRect(hdcMem, &rs, hbrSep);
        }
        RECT rc = { 0, y0, W, y0 + rowH };
        DrawRow(hdcMem, rc, kStrip, rows[i].col, rows[i].label, rows[i].val);
    }

    // Bottom row: gear / settings indicator (left-click opens settings)
    {
        int y0 = rowY0(5);
        RECT rs = { 0, y0 - kSep, W, y0 };
        FillRect(hdcMem, &rs, hbrSep);

        RECT rRow = { 0, y0, W, y0 + rowH };
        HBRUSH hbrRow = CreateSolidBrush(RGB(40, 40, 48));
        FillRect(hdcMem, &rRow, hbrRow);
        DeleteObject(hbrRow);

        // Gear symbol (U+2699)
        RECT rGear = { 2, y0, 2 + rowH, y0 + rowH };
        SetTextColor(hdcMem, RGB(140, 140, 165));
        DrawTextW(hdcMem, L"\u2699", -1, &rGear,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        // Device info
        char infoBuf[48];
        if (s_m.audioDevOpen)
            snprintf(infoBuf, sizeof(infoBuf), "%d Hz  %d spl",
                     (int)s_m.sampleRate, s_m.bufferSize);
        else
            snprintf(infoBuf, sizeof(infoBuf), "No device");

        RECT rInfo = { 2 + rowH + 3, y0, W - 3, y0 + rowH };
        SetTextColor(hdcMem, RGB(115, 115, 130));
        DrawTextA(hdcMem, infoBuf, -1, &rInfo,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        s_gearRect = rRow;  // update click-hit zone each paint
    }

    DeleteObject(hbrSep);
    SelectObject(hdcMem, hOldFont);
    DeleteObject(hFont);

    BitBlt(hdc, 0, 0, W, H, hdcMem, 0, 0, SRCCOPY);
    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbm);
    DeleteDC(hdcMem);

    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------
// Settings dialog
// ---------------------------------------------------------------------------
static void PopulateSettingsDlg(HWND hDlg)
{
    char buf[32];
    // CPU thresholds stored as fraction (0–1), displayed as percent (0–100)
    snprintf(buf, sizeof(buf), "%.0f", s_thr.cpuYel * 100.0); SetDlgItemTextA(hDlg, IDC_MON_CPU_YEL, buf);
    snprintf(buf, sizeof(buf), "%.0f", s_thr.cpuOra * 100.0); SetDlgItemTextA(hDlg, IDC_MON_CPU_ORA, buf);
    snprintf(buf, sizeof(buf), "%.0f", s_thr.cpuRed * 100.0); SetDlgItemTextA(hDlg, IDC_MON_CPU_RED, buf);
    snprintf(buf, sizeof(buf), "%.1f", s_thr.ioYel);           SetDlgItemTextA(hDlg, IDC_MON_IO_YEL,  buf);
    snprintf(buf, sizeof(buf), "%.1f", s_thr.ioOra);           SetDlgItemTextA(hDlg, IDC_MON_IO_ORA,  buf);
    snprintf(buf, sizeof(buf), "%.1f", s_thr.ioRed);           SetDlgItemTextA(hDlg, IDC_MON_IO_RED,  buf);
    snprintf(buf, sizeof(buf), "%.1f", s_thr.pdcYel);          SetDlgItemTextA(hDlg, IDC_MON_PDC_YEL, buf);
    snprintf(buf, sizeof(buf), "%.1f", s_thr.pdcOra);          SetDlgItemTextA(hDlg, IDC_MON_PDC_ORA, buf);
    snprintf(buf, sizeof(buf), "%.1f", s_thr.pdcRed);          SetDlgItemTextA(hDlg, IDC_MON_PDC_RED, buf);
    snprintf(buf, sizeof(buf), "%.1f", s_thr.rtYel);           SetDlgItemTextA(hDlg, IDC_MON_RT_YEL,  buf);
    snprintf(buf, sizeof(buf), "%.1f", s_thr.rtOra);           SetDlgItemTextA(hDlg, IDC_MON_RT_ORA,  buf);
    snprintf(buf, sizeof(buf), "%.1f", s_thr.rtRed);           SetDlgItemTextA(hDlg, IDC_MON_RT_RED,  buf);
    snprintf(buf, sizeof(buf), "%.0f", s_thr.rcYel * 100.0);   SetDlgItemTextA(hDlg, IDC_MON_RC_YEL,  buf);
    snprintf(buf, sizeof(buf), "%.0f", s_thr.rcOra * 100.0);   SetDlgItemTextA(hDlg, IDC_MON_RC_ORA,  buf);
    snprintf(buf, sizeof(buf), "%.0f", s_thr.rcRed * 100.0);   SetDlgItemTextA(hDlg, IDC_MON_RC_RED,  buf);
}

static void CommitSettingsDlg(HWND hDlg)
{
    char buf[64] = {};
    auto getf = [&](int id) -> double {
        GetDlgItemTextA(hDlg, id, buf, (int)sizeof(buf));
        return atof(buf);
    };
    s_thr.cpuYel = getf(IDC_MON_CPU_YEL) / 100.0;
    s_thr.cpuOra = getf(IDC_MON_CPU_ORA) / 100.0;
    s_thr.cpuRed = getf(IDC_MON_CPU_RED) / 100.0;
    s_thr.ioYel  = getf(IDC_MON_IO_YEL);
    s_thr.ioOra  = getf(IDC_MON_IO_ORA);
    s_thr.ioRed  = getf(IDC_MON_IO_RED);
    s_thr.pdcYel = getf(IDC_MON_PDC_YEL);
    s_thr.pdcOra = getf(IDC_MON_PDC_ORA);
    s_thr.pdcRed = getf(IDC_MON_PDC_RED);
    s_thr.rtYel  = getf(IDC_MON_RT_YEL);
    s_thr.rtOra  = getf(IDC_MON_RT_ORA);
    s_thr.rtRed  = getf(IDC_MON_RT_RED);
    s_thr.rcYel  = getf(IDC_MON_RC_YEL) / 100.0;
    s_thr.rcOra  = getf(IDC_MON_RC_ORA) / 100.0;
    s_thr.rcRed  = getf(IDC_MON_RC_RED) / 100.0;
    SaveThresholds();
}

static INT_PTR CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        PopulateSettingsDlg(hwnd);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
            CommitSettingsDlg(hwnd);
            EndDialog(hwnd, IDOK);
            if (s_hwnd && IsWindow(s_hwnd))
                InvalidateRect(s_hwnd, nullptr, FALSE);
            return TRUE;
        case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        case IDC_MON_RESET:
            s_thr = k_DefThr;
            PopulateSettingsDlg(hwnd);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void OpenSettings(HWND hParent)
{
    DialogBoxParam(s_hInst, MAKEINTRESOURCE(IDD_MONITOR_SETTINGS),
                   hParent, SettingsDlgProc, 0);
}

// Destroy-and-recreate dock toggle, mirroring TransitionWnd ToggleDocking()
static void ToggleDocking()
{
    if (!s_hwnd || !IsWindow(s_hwnd)) return;
    bool isFloat = false;
    bool wasDocked = (DockIsChildOfDock(s_hwnd, &isFloat) >= 0);
    SetExtState(k_ExtSection, k_DockKey, !wasDocked ? "1" : "0", true);
    s_suppressDockStateSave = true;
    DestroyWindow(s_hwnd);          // WM_DESTROY clears s_hwnd
    s_suppressDockStateSave = false;
    MonitorWnd_ShowHide();           // recreate in new dock state
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
        LoadThresholds();
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
        return 1;

    case WM_PAINT:
        OnPaint(hwnd);
        return TRUE;

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return FALSE;

    case WM_LBUTTONDOWN:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (PtInRect(&s_gearRect, pt))
            OpenSettings(hwnd);
        return FALSE;
    }

    case WM_CONTEXTMENU:
    {
        bool isDocked = (DockIsChildOfDock(hwnd, nullptr) >= 0);
        HMENU hMenu = CreatePopupMenu();
        AppendMenuA(hMenu, MF_STRING, 1, isDocked ? "Undock" : "Dock to Docker");
        AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(hMenu, MF_STRING, 2, "Threshold Settings...");
        int r = (int)(INT_PTR)TrackPopupMenu(
            hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), 0, hwnd, nullptr);
        DestroyMenu(hMenu);
        if (r == 1)
            ToggleDocking();
        else if (r == 2)
            OpenSettings(hwnd);
        return TRUE;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return TRUE;

    case WM_DESTROY:
        KillTimer(hwnd, k_TimerID);
        if (!s_suppressDockStateSave)
        {
            bool isFloat = false;
            bool wasDocked = (DockIsChildOfDock(hwnd, &isFloat) >= 0);
            SetExtState(k_ExtSection, k_DockKey, wasDocked ? "1" : "0", true);
            if (wasDocked)
                DockWindowRemove(hwnd);
        }
        else if (DockIsChildOfDock(hwnd, nullptr) >= 0)
        {
            DockWindowRemove(hwnd);
        }
        s_hwnd = nullptr;
        return FALSE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void MonitorWnd_Init(HINSTANCE hInst)
{
    s_hInst = hInst;
    QueryPerformanceFrequency(&s_qpcFreq);
    s_audioHook.OnAudioBuffer = OnAudioBuffer;
    Audio_RegHardwareHook(true, &s_audioHook);
}

void MonitorWnd_Cleanup()
{
    Audio_RegHardwareHook(false, &s_audioHook);
    if (s_hwnd && IsWindow(s_hwnd))
    {
        DestroyWindow(s_hwnd);
        // s_hwnd cleared by WM_DESTROY
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
        if (wantDocked)
        {
            DockWindowAddEx(s_hwnd, "Live Monitor", "reaper_trans_monitor", true);
            DockWindowActivate(s_hwnd);
        }
        else
        {
            ShowWindow(s_hwnd, SW_SHOW);
        }
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
