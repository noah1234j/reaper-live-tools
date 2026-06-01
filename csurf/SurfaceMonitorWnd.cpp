// SurfaceMonitorWnd.cpp
// Modeless floating window that captures all Live Tools control surface log output.
// The monitoring checkboxes in "Live Tools Settings" control what gets logged;
// this window shows all of it in one scrollable console view.

#include "../api.h"
#include "../resource.h"
#include "SurfaceMonitorWnd.h"
#include "control_surface_integrator.h"

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <cstring>

// ---------------------------------------------------------------------------
// Forward declaration – defined in control_surface_integrator.cpp
// ---------------------------------------------------------------------------
extern CSurfIntegrator* g_csi_;

// ---------------------------------------------------------------------------
// The function pointer that LogToConsole calls when the window is open.
// Defined here; declared extern in handy_functions.h.
// ---------------------------------------------------------------------------
void (*g_surfaceMonitorAppend)(const char*) = nullptr;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static HINSTANCE s_hInst = nullptr;
static HWND      s_hwnd  = nullptr;

// Maximum number of lines to keep in the edit control before trimming.
static const int kMaxLines = 2000;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
static void TrimToMaxLines(HWND hEdit)
{
    int lineCount = (int)SendMessageA(hEdit, EM_GETLINECOUNT, 0, 0);
    if (lineCount <= kMaxLines) return;

    // Find the character index of the start of line (lineCount - kMaxLines)
    int linesToRemove = lineCount - kMaxLines;
    int charIdx = (int)SendMessageA(hEdit, EM_LINEINDEX, linesToRemove, 0);
    if (charIdx < 0) return;

    // Select and delete the old lines
    SendMessageA(hEdit, EM_SETSEL, 0, (LPARAM)charIdx);
    SendMessageA(hEdit, EM_REPLACESEL, FALSE, (LPARAM)"");
}

// ---------------------------------------------------------------------------
// Public append function – called via g_surfaceMonitorAppend from LogToConsole
// ---------------------------------------------------------------------------
void SurfaceMonitorWnd_Append(const char* msg)
{
    if (!s_hwnd || !IsWindow(s_hwnd)) return;

    HWND hEdit = GetDlgItem(s_hwnd, IDC_SURF_MON_EDIT);
    if (!hEdit) return;

    // Convert "\n" to "\r\n" for the edit control
    std::string converted;
    for (const char* p = msg; *p; ++p)
    {
        if (*p == '\n' && (p == msg || *(p - 1) != '\r'))
            converted += '\r';
        converted += *p;
    }

    // Append: move caret to end, then insert
    int len = (int)SendMessageA(hEdit, WM_GETTEXTLENGTH, 0, 0);
    SendMessageA(hEdit, EM_SETSEL, len, len);
    SendMessageA(hEdit, EM_REPLACESEL, FALSE, (LPARAM)converted.c_str());

    // Scroll to bottom
    SendMessageA(hEdit, EM_SCROLLCARET, 0, 0);

    // Trim if too long
    TrimToMaxLines(hEdit);
}

// ---------------------------------------------------------------------------
// Dialog procedure
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK DlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM /*lParam*/)
{
    switch (uMsg)
    {
    case WM_INITDIALOG:
    {
        // Set monospaced font on the edit control for readability
        HWND hEdit = GetDlgItem(hwndDlg, IDC_SURF_MON_EDIT);
        HFONT hFont = CreateFontA(
            -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");
        if (hFont)
            SendMessageA(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Limit edit to a large but bounded buffer (~8 MB)
        SendMessageA(hEdit, EM_LIMITTEXT, 8 * 1024 * 1024, 0);
        return TRUE;
    }

    case WM_SIZE:
    {
        // Resize the edit control to fill most of the client area
        RECT rc;
        GetClientRect(hwndDlg, &rc);
        int btnH  = 20;
        int pad   = 5;
        int editH = rc.bottom - btnH - 3 * pad;
        SetWindowPos(GetDlgItem(hwndDlg, IDC_SURF_MON_EDIT),
                     nullptr, pad, pad,
                     rc.right - 2 * pad, editH,
                     SWP_NOZORDER);
        // Reposition buttons
        int btnY = rc.bottom - btnH - pad;
        SetWindowPos(GetDlgItem(hwndDlg, IDC_SURF_MON_DIAG),
                     nullptr, pad, btnY, 90, btnH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hwndDlg, IDC_SURF_MON_CLEAR),
                     nullptr, pad + 95, btnY, 55, btnH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hwndDlg, IDCANCEL),
                     nullptr, rc.right - pad - 60, btnY, 60, btnH, SWP_NOZORDER);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_SURF_MON_DIAG:
            if (g_csi_) g_csi_->RunDiagnostics();
            return TRUE;

        case IDC_SURF_MON_CLEAR:
            SetDlgItemTextA(hwndDlg, IDC_SURF_MON_EDIT, "");
            return TRUE;

        case IDCANCEL:
            SurfaceMonitorWnd_ShowHide();
            return TRUE;
        }
        break;

    case WM_CLOSE:
        SurfaceMonitorWnd_ShowHide();
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void SurfaceMonitorWnd_Init(HINSTANCE hInst)
{
    s_hInst = hInst;
    // Dialog is created lazily on first ShowHide call, like all other tool windows.
}

void SurfaceMonitorWnd_Cleanup()
{
    g_surfaceMonitorAppend = nullptr;
    if (s_hwnd) { DestroyWindow(s_hwnd); s_hwnd = nullptr; }
}

void SurfaceMonitorWnd_ShowHide()
{
    if (!s_hwnd)
    {
        // Create on first use
        s_hwnd = CreateDialogParam(s_hInst,
                                   MAKEINTRESOURCE(IDD_SURFACE_MONITOR),
                                   GetMainHwnd(),
                                   DlgProc,
                                   0);
        if (!s_hwnd) return;
        ShowWindow(s_hwnd, SW_HIDE); // will be shown below
    }
    if (IsWindowVisible(s_hwnd))
    {
        g_surfaceMonitorAppend = nullptr;
        ShowWindow(s_hwnd, SW_HIDE);
    }
    else
    {
        ShowWindow(s_hwnd, SW_SHOW);
        SetForegroundWindow(s_hwnd);
        g_surfaceMonitorAppend = SurfaceMonitorWnd_Append;
    }
}

int SurfaceMonitorWnd_IsVisible()
{
    return (s_hwnd && IsWindowVisible(s_hwnd)) ? 1 : 0;
}
