#include "LiveTheme.h"
#include "../api.h"

#include <vector>

#ifdef _WIN32
#  include <commctrl.h>

// uxtheme.h clashes with the REAPER SDK (both declare GetThemeColor), so
// resolve SetWindowTheme dynamically instead of including the header.
typedef HRESULT (WINAPI *SetWindowTheme_t)(HWND, LPCWSTR, LPCWSTR);
static void ThemeSetWindowTheme(HWND hwnd, LPCWSTR appName)
{
    static SetWindowTheme_t s_fn = []() -> SetWindowTheme_t {
        HMODULE h = LoadLibraryA("uxtheme.dll");
        return h ? (SetWindowTheme_t)GetProcAddress(h, "SetWindowTheme") : nullptr;
    }();
    if (s_fn) s_fn(hwnd, appName, nullptr);
}
#endif

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool            s_light = false;
static int             s_generation = 0;
static LiveThemeColors s_col = {};
static HBRUSH          s_hbrDlg  = nullptr;
static HBRUSH          s_hbrEdit = nullptr;
static HBRUSH          s_hbrList = nullptr;

static std::vector<LiveThemeCallback> s_callbacks;

static int Luma(COLORREF c)
{
    return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
}

static COLORREF Blend(COLORREF a, COLORREF b, int pctB)
{
    return RGB(
        (GetRValue(a) * (100 - pctB) + GetRValue(b) * pctB) / 100,
        (GetGValue(a) * (100 - pctB) + GetGValue(b) * pctB) / 100,
        (GetBValue(a) * (100 - pctB) + GetBValue(b) * pctB) / 100);
}

// Themed GetSysColor: routed through the REAPER theme when available.
static COLORREF ThemeSysColor(int idx)
{
    if (GSC_mainwnd)
        return (COLORREF)(GSC_mainwnd(idx) & 0xFFFFFF);
    return GetSysColor(idx);
}

static void RecreateBrushes()
{
    if (s_hbrDlg)  DeleteObject(s_hbrDlg);
    if (s_hbrEdit) DeleteObject(s_hbrEdit);
    if (s_hbrList) DeleteObject(s_hbrList);
    s_hbrDlg  = CreateSolidBrush(s_col.dlgBg);
    s_hbrEdit = CreateSolidBrush(s_col.editBg);
    s_hbrList = CreateSolidBrush(s_col.listBg);
}

// Re-read the theme colors; returns true if anything changed.
static bool ReadTheme()
{
    LiveThemeColors c;
    c.dlgBg    = ThemeSysColor(COLOR_BTNFACE);
    c.dlgText  = ThemeSysColor(COLOR_BTNTEXT);
    c.editBg   = ThemeSysColor(COLOR_WINDOW);
    c.editText = ThemeSysColor(COLOR_WINDOWTEXT);
    c.listBg   = ThemeSysColor(COLOR_WINDOW);
    c.listText = ThemeSysColor(COLOR_WINDOWTEXT);
    c.listGrid = Blend(c.listBg, c.listText, 18);
    c.hlBg     = ThemeSysColor(COLOR_HIGHLIGHT);
    c.hlText   = ThemeSysColor(COLOR_HIGHLIGHTTEXT);

    bool light = Luma(c.editBg) >= 128;

    bool changed = light != s_light ||
                   c.dlgBg != s_col.dlgBg || c.dlgText != s_col.dlgText ||
                   c.editBg != s_col.editBg || c.editText != s_col.editText ||
                   c.hlBg != s_col.hlBg;
    if (changed)
    {
        s_col   = c;
        s_light = light;
        RecreateBrushes();
        s_generation++;
    }
    return changed;
}

bool LiveTheme_IsLight()  { return s_light; }
int  LiveTheme_Generation() { return s_generation; }
const LiveThemeColors& LiveTheme_Colors() { return s_col; }

void LiveTheme_RegisterCallback(LiveThemeCallback cb)
{
    if (cb) s_callbacks.push_back(cb);
}

// ---------------------------------------------------------------------------
// Central poll timer (registered as a REAPER "timer"; fires ~30 Hz, so the
// actual re-read is throttled with a tick divider)
// ---------------------------------------------------------------------------
static void LiveThemeTimerCallback()
{
    static int tick = 0;
    if (++tick < 30) return;   // ~1 s between polls
    tick = 0;

    if (ReadTheme())
        for (LiveThemeCallback cb : s_callbacks)
            cb();
}

void LiveTheme_Init()
{
    ReadTheme();
    if (plugin_register)
        plugin_register("timer", (void*)LiveThemeTimerCallback);
}

// ---------------------------------------------------------------------------
// Dialog helpers
// ---------------------------------------------------------------------------
INT_PTR LiveTheme_CtlColor(UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    HDC hdc = (HDC)wParam;
    switch (msg)
    {
    case WM_CTLCOLORDLG:
        return (INT_PTR)s_hbrDlg;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor(hdc, s_col.dlgText);
        SetBkColor(hdc, s_col.dlgBg);
        return (INT_PTR)s_hbrDlg;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(hdc, s_col.editText);
        SetBkColor(hdc, s_col.editBg);
        return (INT_PTR)s_hbrEdit;
    }
    return 0;
}

void LiveTheme_ApplyListView(HWND hList)
{
    if (!hList || !IsWindow(hList)) return;
#ifdef _WIN32
    ListView_SetBkColor(hList, s_col.listBg);
    ListView_SetTextBkColor(hList, s_col.listBg);
    ListView_SetTextColor(hList, s_col.listText);

    HWND hHdr = ListView_GetHeader(hList);
    if (hHdr)
        ThemeSetWindowTheme(hHdr, s_light ? L"ItemsView" : L"DarkMode_ItemsView");
    ThemeSetWindowTheme(hList, s_light ? L"Explorer" : L"DarkMode_Explorer");

    InvalidateRect(hList, nullptr, TRUE);
#else
    ListView_SetBkColor(hList, s_col.listBg);
    ListView_SetTextBkColor(hList, s_col.listBg);
    ListView_SetTextColor(hList, s_col.listText);
    InvalidateRect(hList, nullptr, TRUE);
#endif
}

#ifdef _WIN32
// Dark title bar (Win10 20H1+; silently ignored on older builds)
static void ApplyDarkTitleBar(HWND hwnd)
{
    typedef HRESULT (WINAPI *DwmSetAttr_t)(HWND, DWORD, LPCVOID, DWORD);
    static DwmSetAttr_t s_dwmSetAttr = []() -> DwmSetAttr_t {
        HMODULE h = LoadLibraryA("dwmapi.dll");
        return h ? (DwmSetAttr_t)GetProcAddress(h, "DwmSetWindowAttribute") : nullptr;
    }();
    if (!s_dwmSetAttr) return;

    BOOL dark = !s_light;
    const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
    if (FAILED(s_dwmSetAttr(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark))))
        s_dwmSetAttr(hwnd, 19 /* pre-20H1 attribute id */, &dark, sizeof(dark));
}

static BOOL CALLBACK ThemeChildProc(HWND hwnd, LPARAM lParam)
{
    (void)lParam;
    char cls[64] = {};
    GetClassNameA(hwnd, cls, sizeof(cls));

    if (!lstrcmpiA(cls, "SysListView32"))
        LiveTheme_ApplyListView(hwnd);
    else if (!lstrcmpiA(cls, "ComboBox") || !lstrcmpiA(cls, "ComboBoxEx32"))
        ThemeSetWindowTheme(hwnd, s_light ? L"CFD" : L"DarkMode_CFD");
    else if (!lstrcmpiA(cls, "Button") || !lstrcmpiA(cls, "ScrollBar") ||
             !lstrcmpiA(cls, "Edit"))
        ThemeSetWindowTheme(hwnd, s_light ? L"Explorer" : L"DarkMode_Explorer");

    InvalidateRect(hwnd, nullptr, TRUE);
    return TRUE;
}
#endif

void LiveTheme_ApplyDialog(HWND hDlg)
{
    if (!hDlg || !IsWindow(hDlg)) return;
#ifdef _WIN32
    HWND hTop = GetAncestor(hDlg, GA_ROOT);
    ApplyDarkTitleBar(hTop ? hTop : hDlg);
    EnumChildWindows(hDlg, ThemeChildProc, 0);
#endif
    InvalidateRect(hDlg, nullptr, TRUE);
}
