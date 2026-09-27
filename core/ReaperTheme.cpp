#include "ReaperTheme.h"
#include "api.h"

// ---------------------------------------------------------------------------
// Theme reads
// ---------------------------------------------------------------------------
static ReaperListColors s_list = {};
static int              s_generation = 0;
static bool             s_loaded = false;

COLORREF ReaperTheme_Sys(int sysIdx)
{
    if (GSC_mainwnd)
        return (COLORREF)(GSC_mainwnd(sysIdx) & 0xFFFFFF);
    return GetSysColor(sysIdx);
}

// A named theme colour, or `fallback` when the key is missing.
static COLORREF Key(const char* key, COLORREF fallback)
{
    const int c = GetThemeColor ? GetThemeColor(key, 0) : -1;
    return c < 0 ? fallback : (COLORREF)(c & 0xFFFFFF);
}

static COLORREF Blend(COLORREF a, COLORREF b, int pctB)
{
    return RGB((GetRValue(a) * (100 - pctB) + GetRValue(b) * pctB) / 100,
               (GetGValue(a) * (100 - pctB) + GetGValue(b) * pctB) / 100,
               (GetBValue(a) * (100 - pctB) + GetBValue(b) * pctB) / 100);
}

static ReaperListColors ReadList()
{
    ReaperListColors c;
    c.bg    = Key("genlist_bg",    ReaperTheme_Sys(COLOR_WINDOW));
    c.fg    = Key("genlist_fg",    ReaperTheme_Sys(COLOR_WINDOWTEXT));
    // A theme that leaves the list colours equal would make every row
    // unreadable; fall back to the window colours instead.
    if (c.bg == c.fg)
    {
        c.bg = ReaperTheme_Sys(COLOR_WINDOW);
        c.fg = ReaperTheme_Sys(COLOR_WINDOWTEXT);
    }
    c.selBg   = Key("genlist_selbg",   ReaperTheme_Sys(COLOR_HIGHLIGHT));
    c.selFg   = Key("genlist_selfg",   ReaperTheme_Sys(COLOR_HIGHLIGHTTEXT));
    c.selInBg = Key("genlist_seliabg", c.selBg);
    c.selInFg = Key("genlist_seliafg", c.selFg);
    if (c.selBg == c.selFg)
    {
        c.selBg = ReaperTheme_Sys(COLOR_HIGHLIGHT);
        c.selFg = ReaperTheme_Sys(COLOR_HIGHLIGHTTEXT);
    }
    if (c.selInBg == c.selInFg)
    {
        c.selInBg = c.selBg;
        c.selInFg = c.selFg;
    }
    // Halfway between text and background: clearly secondary, still legible.
    c.muted = Blend(c.bg, c.fg, 55);
    return c;
}

bool ReaperTheme_Refresh()
{
    static COLORREF s_face = 0;
    const ReaperListColors c = ReadList();
    const COLORREF face = ReaperTheme_Sys(COLOR_BTNFACE);
    const bool changed = !s_loaded || face != s_face ||
        c.bg != s_list.bg || c.fg != s_list.fg ||
        c.selBg != s_list.selBg || c.selFg != s_list.selFg ||
        c.selInBg != s_list.selInBg || c.selInFg != s_list.selInFg;
    if (changed)
    {
        s_face   = face;
        s_list   = c;
        s_loaded = true;
        s_generation++;
    }
    return changed;
}

int ReaperTheme_Generation() { return s_generation; }

const ReaperListColors& ReaperTheme_List()
{
    if (!s_loaded) ReaperTheme_Refresh();
    return s_list;
}

// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------
INT_PTR ReaperTheme_CtlColor(HWND /*hDlg*/, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSCROLLBAR:
        if (HWND main = GetMainHwnd())
            return (INT_PTR)SendMessage(main, msg, wParam, lParam);
        break;
    }
    return 0;
}

#ifdef _WIN32
// uxtheme.h clashes with the REAPER SDK (both declare GetThemeColor), so
// SetWindowTheme is resolved at runtime instead of including the header.
typedef HRESULT (WINAPI *SetWindowTheme_t)(HWND, LPCWSTR, LPCWSTR);

static BOOL CALLBACK StripStyleProc(HWND h, LPARAM fn)
{
    char cls[32] = {};
    GetClassNameA(h, cls, sizeof(cls));
    if (_stricmp(cls, "Button") != 0) return TRUE;

    switch (GetWindowLong(h, GWL_STYLE) & BS_TYPEMASK)
    {
    case BS_CHECKBOX: case BS_AUTOCHECKBOX:
    case BS_3STATE:   case BS_AUTO3STATE:
    case BS_RADIOBUTTON: case BS_AUTORADIOBUTTON:
    case BS_GROUPBOX:
        // Themed checkboxes draw their text in the visual style's colour and
        // ignore WM_CTLCOLORBTN, which is black on a dark REAPER theme.
        ((SetWindowTheme_t)fn)(h, L"", L"");
        break;
    }
    return TRUE;
}
#endif

void ReaperTheme_ApplyDialog(HWND hDlg)
{
#ifdef _WIN32
    static SetWindowTheme_t s_fn = []() -> SetWindowTheme_t {
        HMODULE h = LoadLibraryA("uxtheme.dll");
        return h ? (SetWindowTheme_t)GetProcAddress(h, "SetWindowTheme") : nullptr;
    }();
    if (s_fn && hDlg) EnumChildWindows(hDlg, StripStyleProc, (LPARAM)s_fn);
#else
    (void)hDlg;   // SWELL already draws controls from the REAPER theme
#endif
}

// ---------------------------------------------------------------------------
// List views
// ---------------------------------------------------------------------------
void ReaperTheme_ApplyListView(HWND hList)
{
    if (!hList) return;
    const ReaperListColors& c = ReaperTheme_List();
    ListView_SetBkColor(hList, c.bg);
    ListView_SetTextBkColor(hList, c.bg);
    ListView_SetTextColor(hList, c.fg);
    InvalidateRect(hList, nullptr, TRUE);
}

static bool RowSelected(HWND hList, int row)
{
    return row >= 0 &&
        (ListView_GetItemState(hList, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
}

void ReaperTheme_ListItemPrePaint(NMLVCUSTOMDRAW* cd, HWND hList, bool muted)
{
    const ReaperListColors& c = ReaperTheme_List();
    const int  row = (int)cd->nmcd.dwItemSpec;
    if (RowSelected(hList, row))
    {
        const bool focused = GetFocus() == hList;
        cd->clrTextBk = focused ? c.selBg : c.selInBg;
        cd->clrText   = focused ? c.selFg : c.selInFg;
    }
    else
    {
        cd->clrTextBk = c.bg;
        cd->clrText   = muted ? c.muted : c.fg;
    }
    cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
}

COLORREF ReaperTheme_ListCellBg(HWND hList, int row)
{
    const ReaperListColors& c = ReaperTheme_List();
    if (!RowSelected(hList, row)) return c.bg;
    return GetFocus() == hList ? c.selBg : c.selInBg;
}
