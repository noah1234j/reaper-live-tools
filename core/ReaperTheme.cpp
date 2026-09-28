#include "ReaperTheme.h"
#include "api.h"
#include <stdlib.h>
#include <algorithm>
#include <vector>

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

// ---------------------------------------------------------------------------
// Dark mode (formerly "Match theme UI") - a machine-wide preference, not a project setting: it is how
// this REAPER install looks, whatever project is open.
// ---------------------------------------------------------------------------
static const char* k_ExtSection  = "reaper_transitions";
static const char* k_MatchKey    = "match_theme_ui";
static int         s_matchTheme  = -1;   // -1 until read from ExtState

bool ReaperTheme_MatchTheme()
{
    if (s_matchTheme < 0)
    {
        const char* v = GetExtState ? GetExtState(k_ExtSection, k_MatchKey) : nullptr;
        s_matchTheme = (v && v[0] == '1') ? 1 : 0;
    }
    return s_matchTheme == 1;
}

void ReaperTheme_SetMatchTheme(bool on)
{
    s_matchTheme = on ? 1 : 0;
    if (SetExtState) SetExtState(k_ExtSection, k_MatchKey, on ? "1" : "0", true);
    ReaperTheme_Refresh();
    ReaperTheme_ReapplyAll();
}

static int Lum(COLORREF c)
{
    return GetRValue(c) * 3 + GetGValue(c) * 6 + GetBValue(c);
}

// The mixer strip's colours: the darker of the two track backgrounds the
// theme alternates between, and the text the mixer writes unselected track
// names in.
//
// Current themes (Default 6 and 7 among them) set that name colour in their
// layout script - light grey (200,200,200) unselected, dark (38,38,38) when a
// selected track inverts it - which the API cannot read, and their colour
// keys (col_tcp_text and the like) are the dark name-field text. So a key is
// only used when it reads on the strip; otherwise the layout's unselected
// grey, or its dark counterpart on a light strip.
static void ReadMixerColors(COLORREF& bg, COLORREF& fg)
{
    const COLORREF tr1 = Key("col_tr1_bg", bg);
    const COLORREF tr2 = Key("col_tr2_bg", tr1);
    bg = Lum(tr2) < Lum(tr1) ? tr2 : tr1;

    const bool     dark    = Lum(bg) < 1275;    // half of 255 * (3+6+1)
    const int      minDiff = 1000;              // ~40% of the full range
    const COLORREF cand[]  = { Key("col_mcp_text", bg), Key("col_tcp_text", bg) };
    fg = dark ? RGB(200, 200, 200) : RGB(38, 38, 38);
    for (COLORREF c : cand)
        if (abs(Lum(c) - Lum(bg)) >= minDiff) { fg = c; break; }
}

// Dark mode off: the plain Windows look, as REAPER's own Preferences window
// has it - system colours, native controls, nothing painted by us.
static ReaperListColors NativeList()
{
    ReaperListColors c;
    c.bg      = GetSysColor(COLOR_WINDOW);
    c.fg      = GetSysColor(COLOR_WINDOWTEXT);
    c.selBg   = GetSysColor(COLOR_HIGHLIGHT);
    c.selFg   = GetSysColor(COLOR_HIGHLIGHTTEXT);
    c.selInBg = GetSysColor(COLOR_BTNFACE);
    c.selInFg = GetSysColor(COLOR_BTNTEXT);
    c.muted   = GetSysColor(COLOR_GRAYTEXT);
    return c;
}

static ReaperListColors ReadList()
{
    if (!ReaperTheme_MatchTheme()) return NativeList();

    ReaperListColors c;
    c.bg    = Key("genlist_bg",    ReaperTheme_Sys(COLOR_WINDOW));
    c.fg    = Key("genlist_fg",    ReaperTheme_Sys(COLOR_WINDOWTEXT));
    if (ReaperTheme_MatchTheme()) ReadMixerColors(c.bg, c.fg);
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
    // Dark mode: the theme's inactive-selection colours are made for its
    // light general lists (Default 7's is near white), and a list drops to
    // them whenever it loses focus for a moment - a click on a button, the
    // rename box opening - which flashed the row white. Keep the one
    // selection colour instead.
    if (ReaperTheme_MatchTheme())
    {
        c.selInBg = c.selBg;
        c.selInFg = c.selFg;
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

static COLORREF s_dlgBg = 0;

bool ReaperTheme_Refresh()
{
    static COLORREF s_face = 0;
    const ReaperListColors c = ReadList();
    const COLORREF face = ReaperTheme_MatchTheme() ? ReaperTheme_Sys(COLOR_BTNFACE)
                                                   : GetSysColor(COLOR_BTNFACE);
    // Dark mode: the whole window takes the mixer's background.
    const COLORREF dlgBg = ReaperTheme_MatchTheme() ? Key("col_mixerbg", c.bg) : face;
    const bool changed = !s_loaded || face != s_face || dlgBg != s_dlgBg ||
        c.bg != s_list.bg || c.fg != s_list.fg ||
        c.selBg != s_list.selBg || c.selFg != s_list.selFg ||
        c.selInBg != s_list.selInBg || c.selInFg != s_list.selInFg;
    if (changed)
    {
        s_face   = face;
        s_dlgBg  = dlgBg;
        s_list   = c;
        s_loaded = true;
        s_generation++;
    }
    return changed;
}

int ReaperTheme_Generation() { return s_generation; }

COLORREF ReaperTheme_DialogBg()
{
    if (!s_loaded) ReaperTheme_Refresh();
    return s_dlgBg;
}

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
    // Dark mode off: Windows' own colours, like REAPER's Preferences.
    if (!ReaperTheme_MatchTheme()) return 0;

    // Dark mode: the dialog, its labels and checkboxes are painted here in
    // the mixer background and the list text. Scrollbars still go to REAPER
    // below.
    if (msg == WM_CTLCOLORDLG || msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN)
    {
        static HBRUSH   s_brush = nullptr;
        static COLORREF s_brushCol = 0;
        const COLORREF bg = ReaperTheme_DialogBg();
        if (!s_brush || s_brushCol != bg)
        {
            if (s_brush) DeleteObject(s_brush);
            s_brush    = CreateSolidBrush(bg);
            s_brushCol = bg;
        }
        if (msg != WM_CTLCOLORDLG)
        {
            const COLORREF text = ReaperTheme_Text();
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, IsWindowEnabled((HWND)lParam) ? text : Blend(bg, text, 45));
            SetBkColor(hdc, bg);
        }
        return (INT_PTR)s_brush;
    }
    // Edit boxes (scene notes and the like) and the lists that open under
    // dropdowns read as part of the lists: the list background and text.
    if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX)
    {
        static HBRUSH   s_editBrush = nullptr;
        static COLORREF s_editCol = 0;
        const ReaperListColors& lc = ReaperTheme_List();
        if (!s_editBrush || s_editCol != lc.bg)
        {
            if (s_editBrush) DeleteObject(s_editBrush);
            s_editBrush = CreateSolidBrush(lc.bg);
            s_editCol   = lc.bg;
        }
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, lc.fg);
        SetBkColor(hdc, lc.bg);
        return (INT_PTR)s_editBrush;
    }

    if (msg == WM_CTLCOLORSCROLLBAR)
        if (HWND main = GetMainHwnd())
            return (INT_PTR)SendMessage(main, msg, wParam, lParam);
    return 0;
}

COLORREF ReaperTheme_HeaderBg()
{
    return Blend(ReaperTheme_DialogBg(), ReaperTheme_Text(), 12);
}

COLORREF ReaperTheme_Text()
{
    return ReaperTheme_MatchTheme() ? ReaperTheme_List().fg
                                    : GetSysColor(COLOR_BTNTEXT);
}

COLORREF ReaperTheme_Line()
{
    return ReaperTheme_MatchTheme() ? RGB(0, 0, 0) : GetSysColor(COLOR_BTNSHADOW);
}

#ifdef _WIN32
// uxtheme.h clashes with the REAPER SDK (both declare GetThemeColor), so
// SetWindowTheme is resolved at runtime instead of including the header.
typedef HRESULT (WINAPI *SetWindowTheme_t)(HWND, LPCWSTR, LPCWSTR);

// ---- Painted push buttons ------------------------------------------------
// Push buttons (and push-like checkboxes such as Scenes / Cue List) ignore
// WM_CTLCOLORBTN, so Windows draws them light whatever the theme. They are
// painted here instead: a face a step off the dialog background, a frame
// in the list dividers' colour, the theme's text, and the list selection
// colour for a push-like button that is checked. The control keeps its own
// behaviour (clicks, check state, keyboard); only the drawing is replaced.
//
// Plain checkboxes, radio buttons and group boxes go through the same
// subclass and are painted by PaintCheck / PaintGroup.
static bool IsDarkBg(COLORREF c) { return Lum(c) < 1275; }

// The fill inside checkboxes, radio buttons, edit and dropdown fields.
static COLORREF FieldBg()
{
    return ReaperTheme_MatchTheme() ? ReaperTheme_List().bg : ReaperTheme_Sys(COLOR_WINDOW);
}

static void PaintGroup(HWND h, HDC hdc)
{
    RECT rc;
    GetClientRect(h, &rc);
    const COLORREF dlg  = ReaperTheme_DialogBg();
    const COLORREF text = ReaperTheme_Text();

    wchar_t txt[256] = {};
    GetWindowTextW(h, txt, 256);
    HGDIOBJ of = SelectObject(hdc, (HFONT)SendMessage(h, WM_GETFONT, 0, 0));
    SIZE sz = {};
    GetTextExtentPoint32W(hdc, txt, (int)wcslen(txt), &sz);
    if (!sz.cy)
    {
        TEXTMETRICW tm = {};
        GetTextMetricsW(hdc, &tm);
        sz.cy = tm.tmHeight;
    }

    // The frame sits at the text's mid-height; the interior is left alone,
    // since the controls inside are siblings that paint themselves.
    HPEN    hp  = CreatePen(PS_SOLID, 1, ReaperTheme_Line());
    HGDIOBJ opn = SelectObject(hdc, hp);
    HGDIOBJ obr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top + sz.cy / 2, rc.right, rc.bottom);
    SelectObject(hdc, obr);
    SelectObject(hdc, opn);
    DeleteObject(hp);

    if (txt[0])
    {
        RECT rt = { rc.left + 6, rc.top, rc.left + 6 + sz.cx + 4, rc.top + sz.cy };
        if (rt.right > rc.right - 6) rt.right = rc.right - 6;
        HBRUSH hb = CreateSolidBrush(dlg);
        FillRect(hdc, &rt, hb);
        DeleteObject(hb);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, IsWindowEnabled(h) ? text : Blend(dlg, text, 45));
        const LRESULT ui = SendMessage(h, WM_QUERYUISTATE, 0, 0);
        DrawTextW(hdc, txt, -1, &rt, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  ((ui & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0));
    }
    SelectObject(hdc, of);
}

static void PaintCheck(HWND h, HDC hdcOut)
{
    RECT rc;
    GetClientRect(h, &rc);
    const int w = rc.right - rc.left, ht = rc.bottom - rc.top;
    if (w <= 0 || ht <= 0) return;

    const COLORREF dlg     = ReaperTheme_DialogBg();
    const COLORREF text    = ReaperTheme_Text();
    const LONG     style   = GetWindowLong(h, GWL_STYLE);
    const UINT     type    = style & BS_TYPEMASK;
    const bool     radio   = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    const LRESULT  state   = SendMessage(h, BM_GETSTATE, 0, 0);
    const bool     enabled = IsWindowEnabled(h) != 0;
    const bool     hot     = GetProp(h, "LT_Hot") != nullptr;
    const COLORREF fg      = enabled ? text : Blend(dlg, text, 45);

    HDC     hdc = CreateCompatibleDC(hdcOut);
    HBITMAP bmp = CreateCompatibleBitmap(hdcOut, w, ht);
    HGDIOBJ ob  = SelectObject(hdc, bmp);

    HBRUSH hbDlg = CreateSolidBrush(dlg);
    FillRect(hdc, &rc, hbDlg);
    DeleteObject(hbDlg);

    HGDIOBJ of = SelectObject(hdc, (HFONT)SendMessage(h, WM_GETFONT, 0, 0));
    TEXTMETRICW tm = {};
    GetTextMetricsW(hdc, &tm);

    // The box: the field colour, a ReaperTheme_Line frame, a light tick.
    int s = tm.tmAscent;
    if (s > ht) s = ht;
    if (s < 8)  s = 8;
    RECT rb = { rc.left, (rc.top + rc.bottom - s) / 2, 0, 0 };
    rb.right  = rb.left + s;
    rb.bottom = rb.top + s;

    COLORREF fill = FieldBg();
    if (state & BST_PUSHED) fill = Blend(fill, text, 20);
    else if (hot)           fill = Blend(fill, text, 10);
    if (!enabled)           fill = Blend(dlg, fill, 50);

    HBRUSH  hbFill = CreateSolidBrush(fill);
    HPEN    hpFrm  = CreatePen(PS_SOLID, 1, ReaperTheme_Line());
    HGDIOBJ obr = SelectObject(hdc, hbFill);
    HGDIOBJ opn = SelectObject(hdc, hpFrm);
    if (radio) Ellipse  (hdc, rb.left, rb.top, rb.right, rb.bottom);
    else       Rectangle(hdc, rb.left, rb.top, rb.right, rb.bottom);
    SelectObject(hdc, obr);
    SelectObject(hdc, opn);
    DeleteObject(hbFill);
    DeleteObject(hpFrm);

    const LRESULT check = state & (BST_CHECKED | BST_INDETERMINATE);
    if (check)
    {
        HBRUSH  hbMark = CreateSolidBrush(fg);
        HGDIOBJ obm    = SelectObject(hdc, hbMark);
        HGDIOBJ opm    = SelectObject(hdc, GetStockObject(NULL_PEN));
        const int in = s / 4 + 1;
        if (radio)
        {
            Ellipse(hdc, rb.left + in, rb.top + in, rb.right - in + 1, rb.bottom - in + 1);
        }
        else if (check & BST_INDETERMINATE)
        {
            RECT ri = { rb.left + in, rb.top + in, rb.right - in, rb.bottom - in };
            FillRect(hdc, &ri, hbMark);
        }
        else
        {
            // A tick: short stroke down to the low point, long one up.
            const int pw = s >= 14 ? 2 : 1;
            HPEN hpTick = CreatePen(PS_SOLID, pw + (s >= 11 ? 1 : 0), fg);
            SelectObject(hdc, hpTick);
            const int l = rb.left + 2, t = rb.top + 2, r = rb.right - 3, b = rb.bottom - 3;
            POINT pts[3] = {
                { l,                   t + (b - t) / 2 },
                { l + (r - l) * 2 / 5, b },
                { r,                   t },
            };
            Polyline(hdc, pts, 3);
            SelectObject(hdc, GetStockObject(NULL_PEN));
            DeleteObject(hpTick);
        }
        SelectObject(hdc, obm);
        SelectObject(hdc, opm);
        DeleteObject(hbMark);
    }

    // The label.
    const LRESULT ui = SendMessage(h, WM_QUERYUISTATE, 0, 0);
    wchar_t txt[256] = {};
    GetWindowTextW(h, txt, 256);
    RECT rt = { rb.right + 4, rc.top, rc.right, rc.bottom };
    const UINT fmt = ((style & BS_MULTILINE) ? DT_WORDBREAK : (DT_SINGLELINE | DT_VCENTER)) |
                     DT_LEFT | DT_END_ELLIPSIS | ((ui & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);
    DrawTextW(hdc, txt, -1, &rt, fmt);

    if (GetFocus() == h && !(ui & UISF_HIDEFOCUS) && txt[0])
    {
        RECT rf = rt;
        DrawTextW(hdc, txt, -1, &rf, fmt | DT_CALCRECT);
        if (!(style & BS_MULTILINE)) OffsetRect(&rf, 0, (rt.top + rt.bottom - rf.bottom - rf.top) / 2);
        InflateRect(&rf, 1, 1);
        SetTextColor(hdc, fg);
        SetBkColor(hdc, dlg);
        DrawFocusRect(hdc, &rf);
    }
    SelectObject(hdc, of);

    BitBlt(hdcOut, 0, 0, w, ht, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, ob);
    DeleteObject(bmp);
    DeleteDC(hdc);
}

static void PaintButton(HWND h, HDC hdcOut)
{
    {
        const LONG style = GetWindowLong(h, GWL_STYLE);
        const UINT type  = style & BS_TYPEMASK;
        if (type == BS_GROUPBOX) { PaintGroup(h, hdcOut); return; }
        if (type != BS_PUSHBUTTON && type != BS_DEFPUSHBUTTON && !(style & BS_PUSHLIKE))
        {
            PaintCheck(h, hdcOut);
            return;
        }
    }

    RECT rc;
    GetClientRect(h, &rc);
    const int w = rc.right - rc.left, ht = rc.bottom - rc.top;
    if (w <= 0 || ht <= 0) return;

    const ReaperListColors& lc = ReaperTheme_List();
    const COLORREF dlg      = ReaperTheme_DialogBg();
    const COLORREF text     = ReaperTheme_Text();
    const UINT     type     = GetWindowLong(h, GWL_STYLE) & BS_TYPEMASK;
    const bool     pushLike = type != BS_PUSHBUTTON && type != BS_DEFPUSHBUTTON;
    const LRESULT  state    = SendMessage(h, BM_GETSTATE, 0, 0);
    const bool     hot      = GetProp(h, "LT_Hot") != nullptr;
    const bool     pressed  = (state & BST_PUSHED) != 0;
    const bool     checked  = pushLike && (state & BST_CHECKED) != 0;

    COLORREF face, fg;
    if (checked)
    {
        face = pressed ? Blend(lc.selBg, dlg, 25)
             : hot     ? Blend(lc.selBg, lc.selFg, 10) : lc.selBg;
        fg   = lc.selFg;
    }
    else
    {
        face = Blend(dlg, text, pressed ? 30 : hot ? 22 : 14);
        fg   = text;
    }
    if (!IsWindowEnabled(h))
    {
        face = Blend(dlg, text, 8);
        fg   = Blend(face, text, 45);
    }

    HDC     hdc = CreateCompatibleDC(hdcOut);
    HBITMAP bmp = CreateCompatibleBitmap(hdcOut, w, ht);
    HGDIOBJ ob  = SelectObject(hdc, bmp);

    // Corners show the dialog behind the rounded frame.
    HBRUSH hbDlg = CreateSolidBrush(dlg);
    FillRect(hdc, &rc, hbDlg);
    DeleteObject(hbDlg);

    HBRUSH  hbFace = CreateSolidBrush(face);
    HPEN    hpFrm  = CreatePen(PS_SOLID, 1, ReaperTheme_Line());
    HGDIOBJ obr = SelectObject(hdc, hbFace);
    HGDIOBJ opn = SelectObject(hdc, hpFrm);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 4, 4);
    SelectObject(hdc, obr);
    SelectObject(hdc, opn);
    DeleteObject(hbFace);
    DeleteObject(hpFrm);

    const LRESULT ui = SendMessage(h, WM_QUERYUISTATE, 0, 0);
    wchar_t txt[256] = {};
    GetWindowTextW(h, txt, 256);
    HGDIOBJ of = SelectObject(hdc, (HFONT)SendMessage(h, WM_GETFONT, 0, 0));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);
    RECT rt = rc;
    InflateRect(&rt, -3, 0);
    if (pressed) OffsetRect(&rt, 0, 1);
    DrawTextW(hdc, txt, -1, &rt, DT_CENTER | DT_VCENTER | DT_SINGLELINE |
              DT_END_ELLIPSIS | ((ui & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0));
    SelectObject(hdc, of);

    if (GetFocus() == h && !(ui & UISF_HIDEFOCUS))
    {
        RECT rf = rc;
        InflateRect(&rf, -3, -3);
        SetTextColor(hdc, fg);
        SetBkColor(hdc, face);
        DrawFocusRect(hdc, &rf);
    }

    BitBlt(hdcOut, 0, 0, w, ht, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, ob);
    DeleteObject(bmp);
    DeleteDC(hdc);
}

static LRESULT CALLBACK ButtonSubclassProc(HWND h, UINT msg, WPARAM wParam,
                                           LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        PaintButton(h, hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        PaintButton(h, (HDC)wParam);
        return 0;
    case WM_MOUSEMOVE:
        if (!GetProp(h, "LT_Hot"))
        {
            SetProp(h, "LT_Hot", (HANDLE)1);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemoveProp(h, "LT_Hot");
        InvalidateRect(h, nullptr, FALSE);
        break;
    // The stock button draws itself straight away on these, outside
    // WM_PAINT; let it, then paint over it at once.
    case BM_SETSTATE: case BM_SETCHECK: case BM_SETSTYLE:
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE: case WM_SETTEXT:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_KEYDOWN: case WM_KEYUP: case WM_CAPTURECHANGED:
    case WM_UPDATEUISTATE:
    {
        const LRESULT r = DefSubclassProc(h, msg, wParam, lParam);
        if (IsWindow(h)) RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        return r;
    }
    case WM_NCDESTROY:
        RemoveProp(h, "LT_Hot");
        RemoveWindowSubclass(h, ButtonSubclassProc, 0);
        break;
    }
    return DefSubclassProc(h, msg, wParam, lParam);
}

// ---- Painted tab strip --------------------------------------------------
// The stock tab control draws light tabs with dark text on any theme. Here:
// the dialog background, unselected tabs a step off it, the selected tab in
// the list selection colour, labels in the theme's text colour.
static void PaintTabs(HWND h, HDC hdcOut)
{
    RECT rc;
    GetClientRect(h, &rc);
    const int w = rc.right - rc.left, ht = rc.bottom - rc.top;
    if (w <= 0 || ht <= 0) return;

    const ReaperListColors& lc = ReaperTheme_List();
    const COLORREF dlg     = ReaperTheme_DialogBg();
    const COLORREF text    = ReaperTheme_Text();
    const int      sel     = TabCtrl_GetCurSel(h);
    const int      hot     = (int)(INT_PTR)GetProp(h, "LT_HotTab") - 1;
    const int      n       = TabCtrl_GetItemCount(h);
    const bool     enabled = IsWindowEnabled(h) != 0;

    HDC     hdc = CreateCompatibleDC(hdcOut);
    HBITMAP bmp = CreateCompatibleBitmap(hdcOut, w, ht);
    HGDIOBJ ob  = SelectObject(hdc, bmp);

    HBRUSH hbDlg = CreateSolidBrush(dlg);
    FillRect(hdc, &rc, hbDlg);
    DeleteObject(hbDlg);

    HGDIOBJ of    = SelectObject(hdc, (HFONT)SendMessage(h, WM_GETFONT, 0, 0));
    HPEN    hpFrm = CreatePen(PS_SOLID, 1, ReaperTheme_Line());
    HGDIOBJ opn   = SelectObject(hdc, hpFrm);
    SetBkMode(hdc, TRANSPARENT);

    int bottom = 0;
    for (int i = 0; i < n; ++i)
    {
        RECT r;
        if (!TabCtrl_GetItemRect(h, i, &r)) continue;
        if (r.bottom > bottom) bottom = r.bottom;
        const bool isSel = i == sel;
        const COLORREF face = isSel ? lc.selBg : Blend(dlg, text, i == hot ? 22 : 12);
        COLORREF       fg   = isSel ? lc.selFg : text;
        if (!enabled) fg = Blend(face, fg, 45);

        HBRUSH  hb  = CreateSolidBrush(face);
        HGDIOBJ obr = SelectObject(hdc, hb);
        Rectangle(hdc, r.left, r.top, r.right, r.bottom + 1);
        SelectObject(hdc, obr);
        DeleteObject(hb);

        wchar_t txt[128] = {};
        TCITEMW ti = {};
        ti.mask       = TCIF_TEXT;
        ti.pszText    = txt;
        ti.cchTextMax = 128;
        SendMessageW(h, TCM_GETITEMW, i, (LPARAM)&ti);
        SetTextColor(hdc, fg);
        DrawTextW(hdc, txt, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                  DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    // Baseline under the strip.
    if (bottom > 0 && bottom < rc.bottom)
    {
        MoveToEx(hdc, rc.left, bottom, nullptr);
        LineTo  (hdc, rc.right, bottom);
    }
    SelectObject(hdc, opn);
    DeleteObject(hpFrm);
    SelectObject(hdc, of);

    BitBlt(hdcOut, 0, 0, w, ht, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, ob);
    DeleteObject(bmp);
    DeleteDC(hdc);
}

static LRESULT CALLBACK TabSubclassProc(HWND h, UINT msg, WPARAM wParam,
                                        LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        PaintTabs(h, hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        PaintTabs(h, (HDC)wParam);
        return 0;
    case WM_MOUSEMOVE:
    {
        TCHITTESTINFO hti = {};
        hti.pt.x = (short)LOWORD(lParam);
        hti.pt.y = (short)HIWORD(lParam);
        const int tab = TabCtrl_HitTest(h, &hti);
        if ((int)(INT_PTR)GetProp(h, "LT_HotTab") - 1 != tab)
        {
            SetProp(h, "LT_HotTab", (HANDLE)(INT_PTR)(tab + 1));
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE:
        RemoveProp(h, "LT_HotTab");
        InvalidateRect(h, nullptr, FALSE);
        break;
    case TCM_SETCURSEL: case WM_LBUTTONDOWN: case WM_KEYDOWN:
    case WM_ENABLE: case WM_SETFOCUS: case WM_KILLFOCUS:
    {
        const LRESULT r = DefSubclassProc(h, msg, wParam, lParam);
        if (IsWindow(h)) RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        return r;
    }
    case WM_NCDESTROY:
        RemoveProp(h, "LT_HotTab");
        RemoveWindowSubclass(h, TabSubclassProc, 0);
        break;
    }
    return DefSubclassProc(h, msg, wParam, lParam);
}

// ---- Edit boxes -----------------------------------------------------------
// The field colours come from WM_CTLCOLOREDIT; this replaces the sunken
// Windows border with a ReaperTheme_Line outline (the inner pixel of a client
// edge takes the field colour). In Dark mode, the scrollbar gets
// Windows' dark style on a dark field.
static void PaintEditFrame(HWND h)
{
    const LONG ex = GetWindowLong(h, GWL_EXSTYLE);
    const LONG st = GetWindowLong(h, GWL_STYLE);
    const int  bw = (ex & WS_EX_CLIENTEDGE) ? GetSystemMetrics(SM_CXEDGE)
                  : (st & WS_BORDER)        ? 1 : 0;
    if (bw <= 0) return;

    RECT rw;
    GetWindowRect(h, &rw);
    OffsetRect(&rw, -rw.left, -rw.top);
    HDC hdc = GetWindowDC(h);
    HBRUSH hbLine = CreateSolidBrush(ReaperTheme_Line());
    FrameRect(hdc, &rw, hbLine);
    DeleteObject(hbLine);
    if (bw > 1)
    {
        HBRUSH hb = CreateSolidBrush(IsWindowEnabled(h) ? FieldBg()
                                                        : ReaperTheme_DialogBg());
        for (int i = 1; i < bw; ++i)
        {
            InflateRect(&rw, -1, -1);
            FrameRect(hdc, &rw, hb);
        }
        DeleteObject(hb);
    }
    ReleaseDC(h, hdc);
}

static LRESULT CALLBACK EditSubclassProc(HWND h, UINT msg, WPARAM wParam,
                                         LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    switch (msg)
    {
    case WM_NCPAINT:
    {
        // Windows paints the non-client area (the scrollbar) only inside the
        // frame, so it never draws a border of its own to be painted over -
        // drawing it first and the black outline after flashed light.
        RECT rw;
        GetWindowRect(h, &rw);
        HRGN rgn = CreateRectRgn(rw.left + 1, rw.top + 1, rw.right - 1, rw.bottom - 1);
        if (wParam > 1) CombineRgn(rgn, rgn, (HRGN)wParam, RGN_AND);
        const LRESULT r = DefSubclassProc(h, msg, (WPARAM)rgn, lParam);
        DeleteObject(rgn);
        PaintEditFrame(h);
        return r;
    }
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE:
    {
        const LRESULT r = DefSubclassProc(h, msg, wParam, lParam);
        if (IsWindow(h)) PaintEditFrame(h);
        return r;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, EditSubclassProc, 0);
        break;
    }
    return DefSubclassProc(h, msg, wParam, lParam);
}

// ---- Dropdowns ------------------------------------------------------------
// Drop-down lists (CBS_DROPDOWNLIST) are painted like the buttons, with the
// selected item's text and an arrow; the list that opens takes the list
// colours through WM_CTLCOLORLISTBOX. Editable combos are left to Windows.
static void PaintCombo(HWND h, HDC hdcOut)
{
    RECT rc;
    GetClientRect(h, &rc);
    const int w = rc.right - rc.left, ht = rc.bottom - rc.top;
    if (w <= 0 || ht <= 0) return;

    const COLORREF dlg     = ReaperTheme_DialogBg();
    const COLORREF text    = ReaperTheme_Text();
    const bool     enabled = IsWindowEnabled(h) != 0;
    const bool     hot     = GetProp(h, "LT_Hot") != nullptr;
    const bool     open    = SendMessage(h, CB_GETDROPPEDSTATE, 0, 0) != 0;
    COLORREF face = Blend(dlg, text, open ? 30 : hot ? 22 : 14);
    COLORREF fg   = text;
    if (!enabled)
    {
        face = Blend(dlg, text, 8);
        fg   = Blend(face, text, 45);
    }

    HDC     hdc = CreateCompatibleDC(hdcOut);
    HBITMAP bmp = CreateCompatibleBitmap(hdcOut, w, ht);
    HGDIOBJ ob  = SelectObject(hdc, bmp);

    HBRUSH hbDlg = CreateSolidBrush(dlg);
    FillRect(hdc, &rc, hbDlg);
    DeleteObject(hbDlg);

    HBRUSH  hbFace = CreateSolidBrush(face);
    HPEN    hpFrm  = CreatePen(PS_SOLID, 1, ReaperTheme_Line());
    HGDIOBJ obr = SelectObject(hdc, hbFace);
    HGDIOBJ opn = SelectObject(hdc, hpFrm);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 4, 4);
    SelectObject(hdc, obr);
    SelectObject(hdc, opn);
    DeleteObject(hbFace);
    DeleteObject(hpFrm);

    // Arrow: a small filled triangle in the right-hand end.
    const int aw = ht * 3 / 8 < 7 ? 7 : ht * 3 / 8;
    const int ax = rc.right - 6 - aw, ay = (rc.top + rc.bottom) / 2 - aw / 4;
    {
        POINT tri[3] = { { ax, ay }, { ax + aw, ay }, { ax + aw / 2, ay + aw / 2 } };
        HBRUSH  hbA = CreateSolidBrush(fg);
        HPEN    hpA = CreatePen(PS_SOLID, 1, fg);
        HGDIOBJ oa  = SelectObject(hdc, hbA);
        HGDIOBJ op2 = SelectObject(hdc, hpA);
        Polygon(hdc, tri, 3);
        SelectObject(hdc, oa);
        SelectObject(hdc, op2);
        DeleteObject(hbA);
        DeleteObject(hpA);
    }

    // The current item.
    const int sel = (int)SendMessage(h, CB_GETCURSEL, 0, 0);
    if (sel >= 0)
    {
        const int len = (int)SendMessageW(h, CB_GETLBTEXTLEN, sel, 0);
        if (len > 0 && len < 1024)
        {
            wchar_t txt[1024] = {};
            SendMessageW(h, CB_GETLBTEXT, sel, (LPARAM)txt);
            HGDIOBJ of = SelectObject(hdc, (HFONT)SendMessage(h, WM_GETFONT, 0, 0));
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, fg);
            RECT rt = { rc.left + 6, rc.top, ax - 4, rc.bottom };
            DrawTextW(hdc, txt, -1, &rt, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                      DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(hdc, of);
        }
    }

    const LRESULT ui = SendMessage(h, WM_QUERYUISTATE, 0, 0);
    if (GetFocus() == h && !open && !(ui & UISF_HIDEFOCUS))
    {
        RECT rf = { rc.left + 3, rc.top + 3, ax - 3, rc.bottom - 3 };
        SetTextColor(hdc, fg);
        SetBkColor(hdc, face);
        DrawFocusRect(hdc, &rf);
    }

    BitBlt(hdcOut, 0, 0, w, ht, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, ob);
    DeleteObject(bmp);
    DeleteDC(hdc);
}

static LRESULT CALLBACK ComboSubclassProc(HWND h, UINT msg, WPARAM wParam,
                                          LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        PaintCombo(h, hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        PaintCombo(h, (HDC)wParam);
        return 0;
    case WM_MOUSEMOVE:
        if (!GetProp(h, "LT_Hot"))
        {
            SetProp(h, "LT_Hot", (HANDLE)1);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemoveProp(h, "LT_Hot");
        InvalidateRect(h, nullptr, FALSE);
        break;
    // The stock combo draws its field straight away on these.
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_KEYDOWN: case WM_CHAR:
    case WM_MOUSEWHEEL: case WM_COMMAND: case WM_CAPTURECHANGED:
    case CB_SETCURSEL: case CB_SHOWDROPDOWN: case CB_SELECTSTRING:
    case CB_RESETCONTENT: case WM_UPDATEUISTATE:
    {
        const LRESULT r = DefSubclassProc(h, msg, wParam, lParam);
        if (IsWindow(h)) RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        return r;
    }
    case WM_NCDESTROY:
        RemoveProp(h, "LT_Hot");
        RemoveWindowSubclass(h, ComboSubclassProc, 0);
        break;
    }
    return DefSubclassProc(h, msg, wParam, lParam);
}

static BOOL CALLBACK StripStyleProc(HWND h, LPARAM fn)
{
    char cls[32] = {};
    GetClassNameA(h, cls, sizeof(cls));
    if (_stricmp(cls, WC_TABCONTROLA) == 0)
    {
        ((SetWindowTheme_t)fn)(h, L"", L"");
        SetWindowSubclass(h, TabSubclassProc, 0, 0);
        return TRUE;
    }
    if (_stricmp(cls, "Edit") == 0)
    {
        if (ReaperTheme_MatchTheme() && IsDarkBg(FieldBg()))
            ((SetWindowTheme_t)fn)(h, L"DarkMode_Explorer", nullptr);
        // A sunken client edge is drawn by the visual style, with hover and
        // focus states of its own; a plain 1px border is only ever drawn by
        // EditSubclassProc.
        const LONG ex = GetWindowLong(h, GWL_EXSTYLE);
        if (ex & WS_EX_CLIENTEDGE)
        {
            SetWindowLong(h, GWL_EXSTYLE, ex & ~WS_EX_CLIENTEDGE);
            SetWindowLong(h, GWL_STYLE, GetWindowLong(h, GWL_STYLE) | WS_BORDER);
            SetProp(h, "LT_Edge", (HANDLE)1);   // RestoreStyleProc puts it back
        }
        SetWindowSubclass(h, EditSubclassProc, 0, 0);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        return TRUE;
    }
    if (_stricmp(cls, "ComboBox") == 0)
    {
        if ((GetWindowLong(h, GWL_STYLE) & 3) == CBS_DROPDOWNLIST)
        {
            ((SetWindowTheme_t)fn)(h, L"", L"");
            SetWindowSubclass(h, ComboSubclassProc, 0, 0);
        }
        return TRUE;
    }
    if (_stricmp(cls, "Button") != 0) return TRUE;

    const LONG style = GetWindowLong(h, GWL_STYLE);
    switch (style & BS_TYPEMASK)
    {
    case BS_PUSHBUTTON: case BS_DEFPUSHBUTTON:
        if (style & (BS_BITMAP | BS_ICON)) break;
        ((SetWindowTheme_t)fn)(h, L"", L"");
        SetWindowSubclass(h, ButtonSubclassProc, 0, 0);
        break;
    case BS_CHECKBOX: case BS_AUTOCHECKBOX:
    case BS_3STATE:   case BS_AUTO3STATE:
    case BS_RADIOBUTTON: case BS_AUTORADIOBUTTON:
    case BS_GROUPBOX:
        // Painted like the rest (PaintButton hands these to PaintCheck and
        // PaintGroup); the stock style comes off so it stops drawing its own
        // states between paints.
        if (style & (BS_BITMAP | BS_ICON)) break;
        ((SetWindowTheme_t)fn)(h, L"", L"");
        SetWindowSubclass(h, ButtonSubclassProc, 0, 0);
        break;
    }
    return TRUE;
}

// Undoes StripStyleProc, for Dark mode off: native controls with their
// visual style back. Harmless on a control that was never styled.
static BOOL CALLBACK RestoreStyleProc(HWND h, LPARAM fn)
{
    char cls[32] = {};
    GetClassNameA(h, cls, sizeof(cls));
    if (_stricmp(cls, WC_TABCONTROLA) == 0)
    {
        RemoveWindowSubclass(h, TabSubclassProc, 0);
        RemoveProp(h, "LT_HotTab");
    }
    else if (_stricmp(cls, "Edit") == 0)
    {
        RemoveWindowSubclass(h, EditSubclassProc, 0);
        if (GetProp(h, "LT_Edge"))
        {
            RemoveProp(h, "LT_Edge");
            SetWindowLong(h, GWL_STYLE, GetWindowLong(h, GWL_STYLE) & ~WS_BORDER);
            SetWindowLong(h, GWL_EXSTYLE, GetWindowLong(h, GWL_EXSTYLE) | WS_EX_CLIENTEDGE);
        }
    }
    else if (_stricmp(cls, "ComboBox") == 0)
    {
        RemoveWindowSubclass(h, ComboSubclassProc, 0);
        RemoveProp(h, "LT_Hot");
    }
    else if (_stricmp(cls, "Button") == 0)
    {
        RemoveWindowSubclass(h, ButtonSubclassProc, 0);
        RemoveProp(h, "LT_Hot");
    }
    else return TRUE;

    ((SetWindowTheme_t)fn)(h, nullptr, nullptr);
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return TRUE;
}

// Every dialog styled so far, so switching Dark mode restyles the open ones.
static std::vector<HWND> s_dialogs;

static BOOL CALLBACK ReapplyListProc(HWND h, LPARAM)
{
    char cls[32] = {};
    GetClassNameA(h, cls, sizeof(cls));
    if (_stricmp(cls, WC_LISTVIEWA) == 0) ReaperTheme_ApplyListView(h);
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
    if (!hDlg) return;
    s_dialogs.erase(std::remove_if(s_dialogs.begin(), s_dialogs.end(),
                        [](HWND h) { return !IsWindow(h); }), s_dialogs.end());
    if (std::find(s_dialogs.begin(), s_dialogs.end(), hDlg) == s_dialogs.end())
        s_dialogs.push_back(hDlg);
    if (s_fn)
        EnumChildWindows(hDlg, ReaperTheme_MatchTheme() ? StripStyleProc : RestoreStyleProc,
                         (LPARAM)s_fn);
#else
    (void)hDlg;   // SWELL already draws controls from the REAPER theme
#endif
}

void ReaperTheme_ReapplyAll()
{
#ifdef _WIN32
    const std::vector<HWND> dlgs = s_dialogs;
    for (HWND h : dlgs)
    {
        if (!IsWindow(h)) continue;
        ReaperTheme_ApplyDialog(h);
        EnumChildWindows(h, ReapplyListProc, 0);
        RedrawWindow(h, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    }
#endif
}

// ---------------------------------------------------------------------------
// List views
// ---------------------------------------------------------------------------
#ifdef _WIN32
// Windows draws LVS_EX_GRIDLINES in the system face colour, which is a light
// grey whatever the theme. The style is taken off and the lines are drawn in
// ReaperTheme_Line after each paint instead, in the same places Windows puts
// them.
static void PaintGrid(HWND hList)
{
    RECT rcClient;
    GetClientRect(hList, &rcClient);
    int top = rcClient.top;
    if (HWND hHdr = ListView_GetHeader(hList))
    {
        if (IsWindowVisible(hHdr))
        {
            RECT rh;
            GetWindowRect(hHdr, &rh);
            top += rh.bottom - rh.top;
        }
    }

    HDC hdc = GetDC(hList);
    HPEN hp = CreatePen(PS_SOLID, 1, ReaperTheme_Line());
    HGDIOBJ op = SelectObject(hdc, hp);

    // Row lines: along the bottom of each visible row, then on at the same
    // pitch through the empty space below the last one.
    const int count = ListView_GetItemCount(hList);
    const int first = ListView_GetTopIndex(hList);
    int y = top, pitch = 0;
    for (int i = first; i < count; ++i)
    {
        RECT r;
        if (!ListView_GetItemRect(hList, i, &r, LVIR_BOUNDS)) break;
        pitch = r.bottom - r.top;
        y = r.bottom;
        if (r.top >= rcClient.bottom) break;
        MoveToEx(hdc, rcClient.left, r.bottom - 1, nullptr);
        LineTo  (hdc, rcClient.right, r.bottom - 1);
    }
    if (pitch <= 0)
    {
        // No rows to measure: use the font height, as the list itself does.
        TEXTMETRIC tm = {};
        HGDIOBJ of = SelectObject(hdc, (HFONT)SendMessage(hList, WM_GETFONT, 0, 0));
        GetTextMetrics(hdc, &tm);
        SelectObject(hdc, of);
        pitch = tm.tmHeight + 4;
    }
    for (y += pitch; y - 1 < rcClient.bottom; y += pitch)
    {
        MoveToEx(hdc, rcClient.left, y - 1, nullptr);
        LineTo  (hdc, rcClient.right, y - 1);
    }

    // Column lines: at the right edge of each column, in display order, from
    // the header down.
    if (HWND hHdr = ListView_GetHeader(hList))
    {
        const int cols = Header_GetItemCount(hHdr);
        const int scrollX = GetScrollPos(hList, SB_HORZ);
        int x = rcClient.left - scrollX;
        for (int i = 0; i < cols; ++i)
        {
            const int idx = Header_OrderToIndex(hHdr, i);
            x += ListView_GetColumnWidth(hList, idx);
            if (x < rcClient.left) continue;
            if (x > rcClient.right) break;
            MoveToEx(hdc, x - 1, top, nullptr);
            LineTo  (hdc, x - 1, rcClient.bottom);
        }
    }

    SelectObject(hdc, op);
    DeleteObject(hp);
    ReleaseDC(hList, hdc);
}

static LRESULT CALLBACK GridSubclassProc(HWND hList, UINT msg, WPARAM wParam,
                                         LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    switch (msg)
    {
    case LVM_SETEXTENDEDLISTVIEWSTYLE:
        // Keep the native gridlines off; remember that they were asked for.
        if ((wParam == 0 || (wParam & LVS_EX_GRIDLINES)) && (lParam & LVS_EX_GRIDLINES))
            SetProp(hList, "LT_Grid", (HANDLE)1);
        else if (wParam == 0 || (wParam & LVS_EX_GRIDLINES))
            RemoveProp(hList, "LT_Grid");
        lParam &= ~LVS_EX_GRIDLINES;
        break;
    case WM_PAINT:
    {
        const LRESULT r = DefSubclassProc(hList, msg, wParam, lParam);
        if (GetProp(hList, "LT_Grid")) PaintGrid(hList);
        return r;
    }
    case WM_HSCROLL:
    case WM_VSCROLL:
    case WM_MOUSEWHEEL:
    {
        // Scrolling blits the old lines along with the rows; repaint whole.
        const LRESULT r = DefSubclassProc(hList, msg, wParam, lParam);
        InvalidateRect(hList, nullptr, FALSE);
        return r;
    }
    case WM_NCDESTROY:
        RemoveProp(hList, "LT_Grid");
        RemoveWindowSubclass(hList, GridSubclassProc, 0);
        break;
    }
    return DefSubclassProc(hList, msg, wParam, lParam);
}
#endif

#ifdef _WIN32
// The stock header is light with dark text on any theme. Painted here: a
// face a step off the window background, the theme's text, dividers in ReaperTheme_Line
// between columns and along the bottom, like the grid below it. A window
// that paints its own header subclasses it after this, and so comes first.
static void PaintHeader(HWND hHdr, HDC hdcOut)
{
    RECT rc;
    GetClientRect(hHdr, &rc);
    const int w = rc.right - rc.left, ht = rc.bottom - rc.top;
    if (w <= 0 || ht <= 0) return;

    const COLORREF text = ReaperTheme_Text();
    const COLORREF face = ReaperTheme_HeaderBg();

    HDC     hdc = CreateCompatibleDC(hdcOut);
    HBITMAP bmp = CreateCompatibleBitmap(hdcOut, w, ht);
    HGDIOBJ ob  = SelectObject(hdc, bmp);

    HBRUSH hb = CreateSolidBrush(face);
    FillRect(hdc, &rc, hb);
    DeleteObject(hb);

    HGDIOBJ of  = SelectObject(hdc, (HFONT)SendMessage(hHdr, WM_GETFONT, 0, 0));
    HPEN    hp  = CreatePen(PS_SOLID, 1, ReaperTheme_Line());
    HGDIOBJ opn = SelectObject(hdc, hp);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, text);

    const int n = Header_GetItemCount(hHdr);
    for (int i = 0; i < n; ++i)
    {
        RECT r;
        if (!Header_GetItemRect(hHdr, i, &r) || r.right <= r.left) continue;

        wchar_t txt[128] = {};
        HDITEMW hi = {};
        hi.mask       = HDI_TEXT | HDI_FORMAT;
        hi.pszText    = txt;
        hi.cchTextMax = 128;
        SendMessageW(hHdr, HDM_GETITEMW, i, (LPARAM)&hi);

        UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
        switch (hi.fmt & HDF_JUSTIFYMASK)
        {
        case HDF_CENTER: fmt |= DT_CENTER; break;
        case HDF_RIGHT:  fmt |= DT_RIGHT;  break;
        default:         fmt |= DT_LEFT;   break;
        }
        RECT rt = r;
        InflateRect(&rt, -6, 0);
        DrawTextW(hdc, txt, -1, &rt, fmt);

        MoveToEx(hdc, r.right - 1, r.top, nullptr);
        LineTo  (hdc, r.right - 1, r.bottom);
    }
    MoveToEx(hdc, rc.left, rc.bottom - 1, nullptr);
    LineTo  (hdc, rc.right, rc.bottom - 1);

    SelectObject(hdc, opn);
    DeleteObject(hp);
    SelectObject(hdc, of);

    BitBlt(hdcOut, 0, 0, w, ht, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, ob);
    DeleteObject(bmp);
    DeleteDC(hdc);
}

static LRESULT CALLBACK HeaderSubclassProc(HWND hHdr, UINT msg, WPARAM wParam,
                                           LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hHdr, &ps);
        PaintHeader(hHdr, hdc);
        EndPaint(hHdr, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        PaintHeader(hHdr, (HDC)wParam);
        return 0;
    // The stock header draws its pressed and hot states straight away.
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_MOUSEMOVE:
    case WM_MOUSELEAVE:  case WM_CAPTURECHANGED:
    {
        const LRESULT r = DefSubclassProc(hHdr, msg, wParam, lParam);
        if (IsWindow(hHdr)) RedrawWindow(hHdr, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        return r;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(hHdr, HeaderSubclassProc, 0);
        break;
    }
    return DefSubclassProc(hHdr, msg, wParam, lParam);
}
#endif

void ReaperTheme_ApplyListView(HWND hList)
{
    if (!hList) return;
#ifdef _WIN32
    if (ReaperTheme_MatchTheme())
    {
        if (HWND hHdr = ListView_GetHeader(hList))
            SetWindowSubclass(hHdr, HeaderSubclassProc, 0, 0);
        SetWindowSubclass(hList, GridSubclassProc, 0, 0);
        // Re-sent through the subclass, which notes the gridlines and strips them.
        const DWORD ex = ListView_GetExtendedListViewStyle(hList);
        if (ex & LVS_EX_GRIDLINES) ListView_SetExtendedListViewStyle(hList, ex);
    }
    else
    {
        // Dark mode off: the stock header and gridlines.
        if (HWND hHdr = ListView_GetHeader(hList))
        {
            RemoveWindowSubclass(hHdr, HeaderSubclassProc, 0);
            InvalidateRect(hHdr, nullptr, TRUE);
        }
        const bool grid = GetProp(hList, "LT_Grid") != nullptr;
        RemoveWindowSubclass(hList, GridSubclassProc, 0);
        RemoveProp(hList, "LT_Grid");
        if (grid)
            ListView_SetExtendedListViewStyleEx(hList, LVS_EX_GRIDLINES, LVS_EX_GRIDLINES);
    }
#endif
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

COLORREF ReaperTheme_ListCellFg(HWND hList, int row)
{
    const ReaperListColors& c = ReaperTheme_List();
    if (!RowSelected(hList, row)) return c.fg;
    return GetFocus() == hList ? c.selFg : c.selInFg;
}
