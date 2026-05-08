// ---------------------------------------------------------------------------
// LiveLockWnd.cpp  -  Live Safe Locking compact dockable bar
//
// Fully custom-painted REAPER-esque horizontal bar, same docking pattern as
// MonitorWnd / TransitionWnd (persist on close, destroy-recreate to toggle
// dock state, right-click context menu for Dock/Undock + Settings).
//
// Layout zones (left → right):
//   Lock zone  (~32%)  – coloured LOCKED / UNLOCKED toggle
//   Status zone (~53%) – active category tags  |  revert counter
//   Gear zone  (~15%)  – ⚙ click to open IDD_LIVELOCK_SETTINGS
//
// IDD_LIVELOCK_SETTINGS is a normal modal dialog (standard controls are fine).
// ---------------------------------------------------------------------------
#include "LiveLockWnd.h"
#include "LiveLockEngine.h"
#include "api.h"
#include "resource.h"

#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static const char* k_ExtSection = "reaper_transitions";
static const char* k_DockKey    = "livelock_docked";
static const UINT  k_TimerID    = 55;
static const UINT  k_TimerMs    = 200;   // UI refresh rate

static HINSTANCE s_hInst                 = nullptr;
static HWND      s_hwnd                  = nullptr;
static bool      s_suppressDockStateSave = false;

// Client-coord hit zones – updated each WM_PAINT
static RECT s_lockZone = {};
static RECT s_gearZone = {};

// Context menu item IDs
enum { CTX_DOCK_TOGGLE = 200, CTX_SETTINGS, CTX_CLOSE };

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------
static const COLORREF k_BgDark      = RGB( 28,  28,  28);
static const COLORREF k_BgGear      = RGB( 40,  40,  50);
static const COLORREF k_Divider     = RGB( 55,  55,  60);
static const COLORREF k_LockOn      = RGB(  0, 140,  30);
static const COLORREF k_LockOff     = RGB(158,  28,  28);
static const COLORREF k_TextBright  = RGB(220, 220, 220);
static const COLORREF k_TextMid     = RGB(155, 155, 165);
static const COLORREF k_TextDim     = RGB( 90,  90, 100);
static const COLORREF k_GearSymbol  = RGB(140, 140, 165);

// ---------------------------------------------------------------------------
// Custom painting
// ---------------------------------------------------------------------------
static void OnPaint(HWND hwnd)
{
    bool locked = LiveLockEngine::Get().IsLocked();
    const LiveLockSettings& cfg = LiveLockEngine::Get().GetSettings();

    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rcClient;
    GetClientRect(hwnd, &rcClient);
    const int W = rcClient.right;
    const int H = rcClient.bottom;

    // Double-buffer
    HDC     hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbm    = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbm);

    // Background
    HBRUSH hbrBg = CreateSolidBrush(k_BgDark);
    FillRect(hdcMem, &rcClient, hbrBg);
    DeleteObject(hbrBg);

    // Fonts
    HFONT hFontBold = CreateFontA(-11, 0, 0, 0, FW_BOLD,   FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HFONT hFontNorm = CreateFontA(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    SetBkMode(hdcMem, TRANSPARENT);

    // ---- Zone widths -------------------------------------------------------
    const int kGearW = (std::max)(20, H + 4);      // square-ish gear zone
    const int kLockW = (W - kGearW) * 32 / 100;
    const int kStatX = kLockW + 2;
    const int kStatW = W - kGearW - kStatX - 4;

    // Store hit zones for mouse handling
    s_lockZone = { 0, 0, kLockW, H };
    s_gearZone = { W - kGearW, 0, W, H };

    // ---- Gear zone ---------------------------------------------------------
    HBRUSH hbrGear = CreateSolidBrush(k_BgGear);
    FillRect(hdcMem, &s_gearZone, hbrGear);
    DeleteObject(hbrGear);

    // Divider left of gear
    HBRUSH hbrDiv = CreateSolidBrush(k_Divider);
    RECT rGearDiv = { s_gearZone.left - 1, 0, s_gearZone.left, H };
    FillRect(hdcMem, &rGearDiv, hbrDiv);
    DeleteObject(hbrDiv);

    SelectObject(hdcMem, hFontBold);
    SetTextColor(hdcMem, k_GearSymbol);
    RECT rGearTxt = s_gearZone;
    DrawTextW(hdcMem, L"\u2699", -1, &rGearTxt,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // ---- Lock zone ---------------------------------------------------------
    COLORREF lockBg = locked ? k_LockOn : k_LockOff;
    HBRUSH hbrLock = CreateSolidBrush(lockBg);
    FillRect(hdcMem, &s_lockZone, hbrLock);
    DeleteObject(hbrLock);

    // 1px dark edge on right of lock zone
    HBRUSH hbrEdge = CreateSolidBrush(RGB(0, 0, 0));
    RECT rEdge = { kLockW, 0, kLockW + 1, H };
    FillRect(hdcMem, &rEdge, hbrEdge);
    DeleteObject(hbrEdge);

    SelectObject(hdcMem, hFontBold);
    SetTextColor(hdcMem, RGB(255, 255, 255));
    RECT rLockTxt = { 2, 0, kLockW - 2, H };
    DrawTextA(hdcMem, locked ? "LOCKED" : "UNLOCKED", -1, &rLockTxt,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // ---- Status zone -------------------------------------------------------
    // Build category string
    char cats[160] = {};
    if (locked)
    {
        auto cat = [&](const char* name) {
            if (cats[0]) strncat(cats, "  \xb7  ", sizeof(cats) - strlen(cats) - 1); // U+00B7 middle dot
            strncat(cats, name, sizeof(cats) - strlen(cats) - 1);
        };
        if (cfg.lockRouting)     cat("Routing");
        if (cfg.lockHardwareOut) cat("HW Out");
        if (cfg.lockMasterSend)  cat("Master Send");
        if (cfg.lockFxBypass)    cat("FX");
        if (cfg.lockRecArm)      cat("Rec Arm");
        if (cfg.lockSelectedOnly)
        {
            if (cats[0]) strncat(cats, "  \xb7  ", sizeof(cats) - strlen(cats) - 1);
            strncat(cats, "[selected only]", sizeof(cats) - strlen(cats) - 1);
        }
        if (!cats[0]) strncpy(cats, "(no categories)", sizeof(cats) - 1);
    }
    else
    {
        strncpy(cats, "Not locked \x96 click to engage", sizeof(cats) - 1);
    }

    // Revert counter (right-align inside status zone)
    char revBuf[32] = {};
    int  reverts    = LiveLockEngine::Get().GetRevertCount();
    if (reverts > 0)
        snprintf(revBuf, sizeof(revBuf), "%d revert%s", reverts, reverts == 1 ? "" : "s");

    SelectObject(hdcMem, hFontNorm);

    if (revBuf[0])
    {
        // Right-align revert count
        RECT rRev = { kStatX, 0, W - kGearW - 4, H };
        SetTextColor(hdcMem, k_TextDim);
        DrawTextA(hdcMem, revBuf, -1, &rRev,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        // Measure to avoid overlap
        SIZE sz = {};
        GetTextExtentPoint32A(hdcMem, revBuf, (int)strlen(revBuf), &sz);
        RECT rCat = { kStatX + 2, 0, W - kGearW - sz.cx - 12, H };
        SetTextColor(hdcMem, locked ? k_TextBright : k_TextMid);
        DrawTextA(hdcMem, cats, -1, &rCat,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    else
    {
        RECT rCat = { kStatX + 2, 0, W - kGearW - 4, H };
        SetTextColor(hdcMem, locked ? k_TextBright : k_TextMid);
        DrawTextA(hdcMem, cats, -1, &rCat,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    // Blit
    BitBlt(hdc, 0, 0, W, H, hdcMem, 0, 0, SRCCOPY);
    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    DeleteObject(hFontBold);
    DeleteObject(hFontNorm);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------------------
// Settings dialog procedure (standard controls – no custom painting needed)
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        INITCOMMONCONTROLSEX icx = { sizeof(icx), ICC_UPDOWN_CLASS };
        InitCommonControlsEx(&icx);

        const LiveLockSettings& s = LiveLockEngine::Get().GetSettings();

        CheckDlgButton(hDlg, IDC_LL_CHK_ROUTING,    s.lockRouting      ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_LL_CHK_SELONLY,    s.lockSelectedOnly ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_LL_CHK_HWOUT,      s.lockHardwareOut  ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_LL_CHK_MASTERSEND, s.lockMasterSend   ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_LL_CHK_FXBYPASS,   s.lockFxBypass     ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_LL_CHK_RECARM,     s.lockRecArm       ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_LL_CHK_CONFIRM,    s.requireConfirm   ? BST_CHECKED : BST_UNCHECKED);

        // Wire up the spin control to its buddy edit box
        HWND hSpin = GetDlgItem(hDlg, IDC_LL_INTERVAL_SPIN);
        HWND hEdit = GetDlgItem(hDlg, IDC_LL_INTERVAL_EDIT);
        SendMessage(hSpin, UDM_SETBUDDY,  (WPARAM)hEdit, 0);
        SendMessage(hSpin, UDM_SETRANGE32, (WPARAM)50, (LPARAM)2000);
        SendMessage(hSpin, UDM_SETPOS32,   0, (LPARAM)s.intervalMs);
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            LiveLockSettings s;
            s.lockRouting      = IsDlgButtonChecked(hDlg, IDC_LL_CHK_ROUTING)    == BST_CHECKED;
            s.lockSelectedOnly = IsDlgButtonChecked(hDlg, IDC_LL_CHK_SELONLY)    == BST_CHECKED;
            s.lockHardwareOut  = IsDlgButtonChecked(hDlg, IDC_LL_CHK_HWOUT)      == BST_CHECKED;
            s.lockMasterSend   = IsDlgButtonChecked(hDlg, IDC_LL_CHK_MASTERSEND) == BST_CHECKED;
            s.lockFxBypass     = IsDlgButtonChecked(hDlg, IDC_LL_CHK_FXBYPASS)   == BST_CHECKED;
            s.lockRecArm       = IsDlgButtonChecked(hDlg, IDC_LL_CHK_RECARM)     == BST_CHECKED;
            s.requireConfirm   = IsDlgButtonChecked(hDlg, IDC_LL_CHK_CONFIRM)    == BST_CHECKED;

            int interval = GetDlgItemInt(hDlg, IDC_LL_INTERVAL_EDIT, nullptr, FALSE);
            if (interval < 50)   interval = 50;
            if (interval > 2000) interval = 2000;
            s.intervalMs = interval;

            LiveLockEngine::Get().SetSettings(s);
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Open settings dialog
// ---------------------------------------------------------------------------
static void OpenSettings(HWND hwndParent)
{
    DialogBoxParam(s_hInst, MAKEINTRESOURCE(IDD_LIVELOCK_SETTINGS),
                   hwndParent, SettingsDlgProc, 0);
    if (s_hwnd && IsWindow(s_hwnd))
        InvalidateRect(s_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Dock / undock (destroy + recreate in opposite state, mirrors MonitorWnd)
// ---------------------------------------------------------------------------
static void ToggleDocking()
{
    if (!s_hwnd || !IsWindow(s_hwnd)) return;
    bool isFloat   = false;
    bool wasDocked = (DockIsChildOfDock(s_hwnd, &isFloat) >= 0);
    SetExtState(k_ExtSection, k_DockKey, !wasDocked ? "1" : "0", true);
    s_suppressDockStateSave = true;
    DestroyWindow(s_hwnd);           // WM_DESTROY clears s_hwnd
    s_suppressDockStateSave = false;
    LiveLockWnd_ShowHide();          // recreate in new dock state
}

// ---------------------------------------------------------------------------
// Main panel dialog procedure (fully custom-painted, no child controls)
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK LiveLockDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        SetTimer(hwnd, k_TimerID, k_TimerMs, nullptr);
        InvalidateRect(hwnd, nullptr, FALSE);
        return TRUE;

    case WM_TIMER:
        if (wParam == k_TimerID)
            InvalidateRect(hwnd, nullptr, FALSE);
        return TRUE;

    case WM_PAINT:
        OnPaint(hwnd);
        return TRUE;

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return FALSE;

    case WM_LBUTTONDOWN:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (PtInRect(&s_lockZone, pt))
        {
            LiveLockEngine::Get().ToggleLock();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (PtInRect(&s_gearZone, pt))
        {
            OpenSettings(hwnd);
        }
        return TRUE;
    }

    case WM_CONTEXTMENU:
    {
        bool isDocked = (DockIsChildOfDock(hwnd, nullptr) >= 0);
        HMENU hMenu = CreatePopupMenu();
        AppendMenuA(hMenu, MF_STRING,    CTX_DOCK_TOGGLE,
                    isDocked ? "Undock" : "Dock to Docker");
        AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(hMenu, MF_STRING,    CTX_SETTINGS, "Settings...");
        AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(hMenu, MF_STRING,    CTX_CLOSE,    "Close");
        int r = (int)(INT_PTR)TrackPopupMenu(
            hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), 0, hwnd, nullptr);
        DestroyMenu(hMenu);
        if      (r == CTX_DOCK_TOGGLE) ToggleDocking();
        else if (r == CTX_SETTINGS)    OpenSettings(hwnd);
        else if (r == CTX_CLOSE)       ShowWindow(hwnd, SW_HIDE);
        return TRUE;
    }

    case WM_CLOSE:
        // Hide (don't destroy) — same behaviour as TransitionWnd / Scenes
        ShowWindow(hwnd, SW_HIDE);
        return TRUE;

    case WM_DESTROY:
        KillTimer(hwnd, k_TimerID);
        if (!s_suppressDockStateSave)
        {
            bool isFloat   = false;
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
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void LiveLockWnd_Init(HINSTANCE hInst)
{
    s_hInst = hInst;
}

void LiveLockWnd_Cleanup()
{
    if (s_hwnd && IsWindow(s_hwnd))
    {
        bool isFloat = false;
        if (DockIsChildOfDock(s_hwnd, &isFloat) >= 0)
            DockWindowRemove(s_hwnd);
        DestroyWindow(s_hwnd);
        // s_hwnd cleared by WM_DESTROY
    }
}

void LiveLockWnd_ShowHide()
{
    if (!s_hwnd || !IsWindow(s_hwnd))
    {
        HWND hMain = GetMainHwnd();
        s_hwnd = CreateDialogParam(s_hInst,
                                   MAKEINTRESOURCE(IDD_LIVELOCK),
                                   hMain,
                                   LiveLockDlgProc,
                                   0);
        if (!s_hwnd)
        {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "CreateDialogParam failed for IDD_LIVELOCK. Error=%lu",
                     GetLastError());
            MessageBoxA(hMain, buf, "Live Tools", MB_OK | MB_ICONERROR);
            return;
        }

        const char* dockPref = GetExtState(k_ExtSection, k_DockKey);
        bool wantDocked = (dockPref && atoi(dockPref) != 0);
        if (wantDocked)
        {
            DockWindowAddEx(s_hwnd, "Live Lock", "reaper_trans_livelock", true);
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

int LiveLockWnd_IsVisible()
{
    if (!s_hwnd || !IsWindow(s_hwnd)) return 0;
    bool isFloat = false;
    if (DockIsChildOfDock(s_hwnd, &isFloat) >= 0) return 1;
    return IsWindowVisible(s_hwnd) ? 1 : 0;
}
