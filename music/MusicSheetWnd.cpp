#include "MusicSheetWnd.h"
#include "MusicSheet.h"
#include "MusicSheetRender.h"
#include "TransitionWnd.h"
#include "TransitionEngine.h"
#include "ReaperTheme.h"
#include "api.h"
#include "resource.h"

#ifdef _WIN32
#  include <windowsx.h>
#  include <commdlg.h>
#  include <objbase.h>
#  include <gdiplus.h>
#else
#  include <sys/stat.h>
#  include <cerrno>
// SWELL has no GetKeyState; the async form reports the same modifiers.
#  define GetKeyState GetAsyncKeyState
#  ifndef IDC_HAND
#    define IDC_HAND IDC_ARROW
#  endif
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// MusicSheetWnd
//
// Everything in the window is painted here - toolbar, pages, marks - so it
// looks the same docked or floating, in Dark mode or not, and there is one
// hit-test to reason about rather than a dozen child controls. The only child
// is the text box that exists while a note is being typed.
//
// Layout, top to bottom:
//   row 1  scene title ..................  < page >   Page | 2-Up | Scroll
//   row 2  Pencil Text Eraser  [six colours]  Undo ..................  More
//   the pages, on a darker ground so the paper stands out
// ---------------------------------------------------------------------------

static HINSTANCE g_hInst = nullptr;
static HWND      g_wnd   = nullptr;

static const char* kSec      = "reaper_transitions";
static const char* kDockKey  = "sheet_docked";
static const char* kRectKey  = "sheet_wnd_rect";
static const char* kViewKey  = "sheet_view";
static const char* kColorKey = "sheet_color";
static const char* kBarKey   = "sheet_toolbar_hidden";

// The toolbar is a column down the left. Collapsed, it folds away to a
// floating chevron (lower left) and two floating page buttons (lower
// right), and the music gets the whole window. Everything else is on the
// right-click menu then.
static bool g_tbCollapsed = false;

static const UINT WM_SHEET_LOADED   = WM_APP + 0x51;  // lParam: SheetDoc*
static const UINT WM_SHEET_EDITDONE = WM_APP + 0x52;  // wParam: 1 keep, 0 cancel
static const UINT kTimerId          = 1;
static const UINT kTimerMs          = 250;

static bool g_suppressDockStateSave = false;

// ---------------------------------------------------------------------------
// Which scene is showing
//
// Held by pointer, and checked against g_snapshots before every use: the
// scene can be deleted under us, and a REAPER undo or a project reload
// rebuilds every scene object. After a rebuild the same scene is found again
// by its row and name, so an undo does not blank the window.
// ---------------------------------------------------------------------------
static TransitionSnapshot* g_owner     = nullptr;
static int                 g_ownerIdx  = -1;
static std::string         g_ownerName;

// ---------------------------------------------------------------------------
// Rendered documents
//
// A few are kept, so going back to the previous song is instant. Keyed by
// path and the file's modification stamp: a re-exported chart is a different
// document and renders again.
// ---------------------------------------------------------------------------
struct CachedDoc
{
    std::string        path;
    unsigned long long stamp = 0;
    SheetDoc*          doc   = nullptr;
    unsigned           used  = 0;     // last-use tick, for eviction
};
static const size_t           kCacheMax = 3;
static std::vector<CachedDoc> g_cache;
static unsigned               g_useClock = 0;

static SheetDoc*          g_doc = nullptr;      // the owner's, once loaded
static bool               g_loading = false;
static std::string        g_loadingPath;
static unsigned long long g_loadingStamp = 0;
static bool               g_missing = false;    // attached, file not there

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------
enum { VIEW_PAGE = 0, VIEW_TWO = 1, VIEW_SCROLL = 2 };
static int g_view    = VIEW_PAGE;
static int g_page    = 0;        // first page shown (page / two-up)
static int g_scrollY = 0;        // scroll mode, pixels

enum { TOOL_NONE = 0, TOOL_PEN, TOOL_TEXT, TOOL_ERASE };
static int g_tool  = TOOL_NONE;  // always starts as "just look": no stray marks on stage
static int g_color = 0;

struct PagePlace { int page; RECT r; };

// Pages scaled to the size they are drawn at. Rebuilt only when that size
// changes, so a stroke being drawn repaints from these rather than
// re-stretching a 1700-pixel page on every mouse move.
struct ScaledPage
{
    const SheetDoc* doc  = nullptr;
    int             page = -1, w = 0, h = 0;
    HBITMAP         bmp  = nullptr;
};
static std::vector<ScaledPage> g_scaled;

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------
enum
{
    TB_NONE = 0, TB_COLLAPSE, TB_EXPAND, TB_PREV, TB_PAGELBL, TB_NEXT,
    TB_VIEW_PAGE, TB_VIEW_TWO, TB_VIEW_SCROLL,
    TB_PEN, TB_TEXT, TB_ERASE, TB_UNDO, TB_MORE,
    TB_COLOR0,                         // ..TB_COLOR0 + kSheetPaletteCount - 1
};
struct TbItem { int id; RECT r; };
static std::vector<TbItem> g_tb;
static int  g_tbW        = 0;     // width of the column; 0 when collapsed
static int  g_minClientW = 200;
static int  g_minClientH = 200;
static int  g_hot        = TB_NONE;
static int  g_pressed    = TB_NONE;
static RECT g_actionRect = {};       // the button in an empty state, if any
static bool g_actionDown = false;

// ---------------------------------------------------------------------------
// Pointer gestures
// ---------------------------------------------------------------------------
static bool        g_drawing = false;     // pencil stroke in progress
static int         g_drawPage = -1;
static SheetStroke g_curStroke;
static POINT       g_lastPt = {};
static bool        g_erasing = false;
static bool        g_erasedThisDrag = false;
static bool        g_panning = false;

// Rotating a note (text tool): the note under the pointer gets a handle above
// it; dragging the handle turns the note about its centre.
static int         g_hotNotePage = -1;    // the note showing the handle
static int         g_hotNoteIdx  = -1;
static bool        g_rotating    = false;
static bool        g_rotatedThisDrag = false;   // the angle changed (undo pushed)
static bool        g_rotDragging = false;       // the knob moved: a drag, not a click
static POINT       g_rotDownPt   = {};     // where the knob was pressed

// Moving a note (text tool): pressed on a note, it follows the pointer once
// the pointer has moved a few pixels; released without moving, it opens for
// editing instead.
static bool        g_notePressed = false;
static bool        g_noteMoving  = false;
static int         g_movePage    = -1;
static int         g_moveIdx     = -1;
static POINT       g_moveDownPt  = {};
static SheetPoint  g_moveOrigPos;
static int         g_panY0 = 0, g_panScroll0 = 0;
static int         g_wheelAcc = 0;

// ---------------------------------------------------------------------------
// The note being typed
// ---------------------------------------------------------------------------
static HWND       g_edit          = nullptr;
static WNDPROC    g_editOldProc   = nullptr;
static int        g_editPage      = -1;
static int        g_editTextIdx   = -1;   // the note being re-edited, or -1
static SheetPoint g_editPos;
static float      g_editHeight    = 0.025f;
static int        g_editColor     = 0;
static HFONT      g_editFont      = nullptr;
static bool       g_editClosing   = false;
static HBRUSH     g_editBrush     = nullptr;

// ---------------------------------------------------------------------------
// Undo - the window's own, one step per stroke / note / erase drag.
//
// REAPER's undo would also work, but every step there re-serialises every
// scene in the project. So marks go on this stack, and one REAPER undo point
// is made once drawing pauses (see FlushUndoPoint), which keeps REAPER's
// history consistent with what is on the page.
// ---------------------------------------------------------------------------
struct UndoStep
{
    int                    page = -1;     // -1: `all` holds every page
    SheetPage              before;
    std::vector<SheetPage> all;
};
static std::vector<UndoStep> g_undo;
static const size_t          kUndoMax = 100;
static bool                  g_undoPointPending = false;
static DWORD                 g_lastEditTick = 0;

static accelerator_register_t g_accel;
static bool                   g_accelRegistered = false;

// Forward declarations
static INT_PTR CALLBACK SheetDlgProc(HWND, UINT, WPARAM, LPARAM);
static void CommitTextEdit(bool keep);
static void SyncDoc();
static void Redraw();

// ===========================================================================
// Small helpers
// ===========================================================================
#ifdef _WIN32
static std::wstring Widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string Narrow(const wchar_t* w)
{
    if (!w || !*w) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
#endif

// DrawText on UTF-8, which is what REAPER and the project file speak.
static int DrawU8(HDC hdc, const std::string& s, RECT* r, UINT fmt)
{
#ifdef _WIN32
    const std::wstring w = Widen(s);
    return DrawTextW(hdc, w.c_str(), (int)w.size(), r, fmt);
#else
    return DrawTextA(hdc, s.c_str(), -1, r, fmt);
#endif
}

static COLORREF PaletteRef(int idx)
{
    const unsigned c = MusicSheet_PaletteColor(idx);
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

static COLORREF Blend(COLORREF a, COLORREF b, int pctB)
{
    const int pa = 100 - pctB;
    return RGB((GetRValue(a) * pa + GetRValue(b) * pctB) / 100,
               (GetGValue(a) * pa + GetGValue(b) * pctB) / 100,
               (GetBValue(a) * pa + GetBValue(b) * pctB) / 100);
}

static void FillSolid(HDC hdc, const RECT& r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(hdc, &r, b);
    DeleteObject(b);
}

static void FrameSolid(HDC hdc, const RECT& r, COLORREF c, int width = 1)
{
    HPEN    pen = CreatePen(PS_SOLID, width, c);
    HGDIOBJ op  = SelectObject(hdc, pen);
    HGDIOBJ ob  = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    const int in = width / 2;   // keep a wide pen inside the rect
    Rectangle(hdc, r.left + in, r.top + in, r.right - in, r.bottom - in);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(pen);
}

static bool InRect(const RECT& r, POINT p)
{
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

// The file's modification time and size, folded together; 0 when it is not
// there. Cheap enough for the window timer, and what tells a re-exported
// chart from the one already rendered.
static unsigned long long FileStamp(const std::string& path)
{
    if (path.empty()) return 0;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!GetFileAttributesExW(Widen(path).c_str(), GetFileExInfoStandard, &fa)) return 0;
    if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    const unsigned long long t = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) |
                                 fa.ftLastWriteTime.dwLowDateTime;
    return (t ^ ((unsigned long long)fa.nFileSizeLow << 7)) | 1ULL;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return 0;
    return ((unsigned long long)st.st_mtime << 20) ^ (unsigned long long)st.st_size | 1ULL;
#endif
}

// ---------------------------------------------------------------------------
// Icons - from the Windows icon font when there is one (Segoe Fluent Icons
// on 11, Segoe MDL2 Assets on 10), short words otherwise.
// ---------------------------------------------------------------------------
enum
{
    ICON_PREV = 0xE76B, ICON_NEXT = 0xE76C, ICON_PEN = 0xE70F, ICON_TEXT = 0xE8D2,
    ICON_ERASE = 0xE75C, ICON_UNDO = 0xE7A7, ICON_MORE = 0xE712,
    ICON_CLIP = 0xE723, ICON_TRASH = 0xE74D,
};

static HFONT IconFont(int px)
{
#ifdef _WIN32
    static HFONT s_font = nullptr;
    static int   s_px   = 0;
    static bool  s_none = false;
    if (s_none) return nullptr;
    if (s_font && s_px == px) return s_font;
    if (s_font) { DeleteObject(s_font); s_font = nullptr; }

    const wchar_t* faces[] = { L"Segoe Fluent Icons", L"Segoe MDL2 Assets" };
    for (const wchar_t* face : faces)
    {
        HFONT f = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
        if (!f) continue;
        // The mapper substitutes silently; only keep the font if it is the
        // one asked for, or the glyphs come out as boxes.
        wchar_t got[64] = {};
        HDC dc = GetDC(nullptr);
        HGDIOBJ o = SelectObject(dc, f);
        GetTextFaceW(dc, 64, got);
        SelectObject(dc, o);
        ReleaseDC(nullptr, dc);
        if (_wcsicmp(got, face) == 0) { s_font = f; s_px = px; return s_font; }
        DeleteObject(f);
    }
    s_none = true;
    return nullptr;
#else
    (void)px;
    return nullptr;
#endif
}

// Draw `glyph` from the icon font centred in r, or `fallback` in the current
// font when there is no icon font.
static void DrawIcon(HDC hdc, const RECT& r, int glyph, const char* fallback, int iconPx)
{
    RECT rr = r;
#ifdef _WIN32
    if (HFONT f = IconFont(iconPx))
    {
        HGDIOBJ o = SelectObject(hdc, f);
        wchar_t g[2] = { (wchar_t)glyph, 0 };
        DrawTextW(hdc, g, 1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, o);
        return;
    }
#else
    (void)glyph; (void)iconPx;
#endif
    DrawTextA(hdc, fallback, -1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static HFONT UiFont()
{
    if (g_wnd)
        if (HFONT f = ReaperTheme_HeadingFont(g_wnd)) return f;
    HFONT f = g_wnd ? (HFONT)SendMessage(g_wnd, WM_GETFONT, 0, 0) : nullptr;
    return f ? f : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

// The face notes are written in. Cached per pixel size; a page rarely has
// more than a couple of sizes on it.
struct NoteFont { int px; HFONT f; };
static std::vector<NoteFont> g_noteFonts;

static HFONT NoteFontPx(int px)
{
    if (px < 6) px = 6;
    for (const auto& nf : g_noteFonts) if (nf.px == px) return nf.f;
    if (g_noteFonts.size() > 8 && !g_edit)   // the text box may be using one
    {
        for (auto& nf : g_noteFonts)
            if (nf.f != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(nf.f);
        g_noteFonts.clear();
    }
#ifdef _WIN32
    HFONT f = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
#else
    HFONT f = CreateFont(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Helvetica");
#endif
    if (!f) f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_noteFonts.push_back({ px, f });
    return f;
}

static void FreeNoteFonts()
{
    for (auto& nf : g_noteFonts)
        if (nf.f && nf.f != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(nf.f);
    g_noteFonts.clear();
}

// ===========================================================================
// Owner and document
// ===========================================================================
static TransitionSnapshot* Owner()
{
    if (!g_owner) return nullptr;
    for (size_t i = 0; i < g_snapshots.size(); ++i)
    {
        if (g_snapshots[i].get() == g_owner)
        {
            g_ownerIdx  = (int)i;
            g_ownerName = g_owner->m_name;
            return g_owner;
        }
    }
    // Rebuilt by an undo or reload: the same row with the same name is the
    // same scene. The marks may have changed under us, so the window's own
    // undo history no longer applies to them.
    g_undo.clear();
    if (g_ownerIdx >= 0 && g_ownerIdx < (int)g_snapshots.size())
    {
        TransitionSnapshot* s = g_snapshots[(size_t)g_ownerIdx].get();
        if (s && !s->m_isSub && !s->m_isSpacer && s->m_name == g_ownerName)
        {
            g_owner = s;
            return g_owner;
        }
    }
    g_owner    = nullptr;
    g_ownerIdx = -1;
    g_ownerName.clear();
    return nullptr;
}

static int PageCount()
{
    return (g_doc && g_doc->Ok()) ? g_doc->PageCount() : 0;
}

static void FreeScaled()
{
    for (auto& sp : g_scaled) if (sp.bmp) DeleteObject(sp.bmp);
    g_scaled.clear();
}

static void FreeCache()
{
    FreeScaled();
    for (auto& c : g_cache) MusicSheetRender_Free(c.doc);
    g_cache.clear();
    g_doc = nullptr;
    g_loading = false;
    g_loadingPath.clear();
}

static void ClampView()
{
    const int n = PageCount();
    if (g_page >= n) g_page = n - 1;
    if (g_page < 0)  g_page = 0;
    if (g_view == VIEW_TWO) g_page -= g_page % 2;
}

// Bring g_doc in line with the owner's attachment: from the cache when it is
// there, otherwise ask the renderer for it.
static void SyncDoc()
{
    TransitionSnapshot* o = Owner();
    const std::string want = (o && o->m_sheet.Attached()) ? MusicSheet_Resolve(o->m_sheet)
                                                          : std::string();
    if (want.empty())
    {
        g_doc = nullptr; g_missing = false;
        if (g_loading) { MusicSheetRender_Cancel(); g_loading = false; }
        return;
    }

    const unsigned long long stamp = FileStamp(want);
    if (!stamp)
    {
        // Gone (moved folder, other machine). Say so, keep the marks.
        g_doc = nullptr; g_missing = true;
        if (g_loading) { MusicSheetRender_Cancel(); g_loading = false; }
        return;
    }
    g_missing = false;

    for (auto& c : g_cache)
    {
        if (c.path == want && c.stamp == stamp)
        {
            c.used = ++g_useClock;
            if (g_doc != c.doc) { g_doc = c.doc; ClampView(); }
            return;
        }
    }

    if (g_loading && g_loadingPath == want && g_loadingStamp == stamp) return;  // on its way
    if (!g_wnd) return;   // nowhere to deliver it; asked again when the window opens

    g_doc          = nullptr;
    g_loading      = true;
    g_loadingPath  = want;
    g_loadingStamp = stamp;
    MusicSheetRender_LoadAsync(want.c_str(), g_wnd, WM_SHEET_LOADED);
}

static void OnDocLoaded(SheetDoc* doc)
{
    if (!doc) return;
    if (!g_loading || doc->path != g_loadingPath)
    {
        MusicSheetRender_Free(doc);   // for a sheet no longer wanted
        return;
    }
    g_loading = false;

    // Failures are cached too, so a broken file is not retried every tick;
    // a new stamp (the file fixed or replaced) retries it.
    CachedDoc c;
    c.path  = doc->path;
    c.stamp = g_loadingStamp;
    c.doc   = doc;
    c.used  = ++g_useClock;
    g_cache.push_back(c);
    while (g_cache.size() > kCacheMax)
    {
        size_t lru = 0;
        for (size_t i = 1; i < g_cache.size(); ++i)
            if (g_cache[i].used < g_cache[lru].used) lru = i;
        for (auto it = g_scaled.begin(); it != g_scaled.end();)
        {
            if (it->doc == g_cache[lru].doc) { if (it->bmp) DeleteObject(it->bmp); it = g_scaled.erase(it); }
            else ++it;
        }
        MusicSheetRender_Free(g_cache[lru].doc);
        g_cache.erase(g_cache.begin() + (long)lru);
    }
    g_doc = doc;
    ClampView();
    Redraw();
}

// ===========================================================================
// Undo
// ===========================================================================
static void NoteEdited()
{
    MarkProjectDirty(nullptr);
    g_undoPointPending = true;
    g_lastEditTick     = GetTickCount();
}

// One REAPER undo point covering the marks made since the last one.
static void FlushUndoPoint()
{
    if (!g_undoPointPending) return;
    g_undoPointPending = false;
    Undo_OnStateChangeEx("Music sheet: annotate", -1, -1);
}

static void PushUndoPage(int page)
{
    TransitionSnapshot* o = Owner();
    if (!o) return;
    UndoStep st;
    st.page = page;
    if (const SheetPage* p = o->m_sheet.PageOrNull(page)) st.before = *p;
    g_undo.push_back(std::move(st));
    if (g_undo.size() > kUndoMax) g_undo.erase(g_undo.begin());
}

static void PushUndoAll()
{
    TransitionSnapshot* o = Owner();
    if (!o) return;
    UndoStep st;
    st.page = -1;
    st.all  = o->m_sheet.pages;
    g_undo.push_back(std::move(st));
    if (g_undo.size() > kUndoMax) g_undo.erase(g_undo.begin());
}

static void DoUndo()
{
    CommitTextEdit(true);
    TransitionSnapshot* o = Owner();
    if (!o || g_undo.empty()) return;
    UndoStep st = std::move(g_undo.back());
    g_undo.pop_back();
    if (st.page < 0) o->m_sheet.pages = std::move(st.all);
    else             o->m_sheet.Page(st.page) = std::move(st.before);
    NoteEdited();
    Redraw();
}

// ===========================================================================
// Layout
// ===========================================================================
static RECT ContentRect()
{
    RECT cr = {};
    if (g_wnd) GetClientRect(g_wnd, &cr);
    cr.left = g_tbW;
    if (cr.right < cr.left) cr.right = cr.left;
    return cr;
}

static int Margin() { return g_tbCollapsed ? 2 : 8; }

// Where each visible page goes, in client coordinates. `totalH` (scroll mode)
// is the height of the whole stack, for clamping the scroll.
static void LayoutPages(std::vector<PagePlace>& out, int* totalH)
{
    out.clear();
    if (totalH) *totalH = 0;
    const int n = PageCount();
    if (n <= 0) return;

    const RECT c = ContentRect();
    const int  m = Margin();
    const int  W = (c.right - c.left) - 2 * m;
    const int  H = (c.bottom - c.top) - 2 * m;
    if (W < 10 || H < 10) return;

    auto aspect = [](int p) -> double {   // height / width
        const SheetDocPage& pg = g_doc->pages[(size_t)p];
        return (pg.w > 0) ? (double)pg.h / pg.w : 1.4142;
    };

    if (g_view == VIEW_SCROLL)
    {
        int y = c.top + m - g_scrollY;
        int stack = 0;
        for (int p = 0; p < n; ++p)
        {
            const int ph = (int)(W * aspect(p) + 0.5);
            RECT r = { c.left + m, y, c.left + m + W, y + ph };
            if (r.bottom >= c.top && r.top < c.bottom) out.push_back({ p, r });
            y     += ph + m;
            stack += ph + m;
        }
        if (totalH) *totalH = stack + m;
        return;
    }

    if (g_view == VIEW_TWO && g_page + 1 < n)
    {
        // Two pages side by side, at one shared height that lets both fit.
        const int    gap   = m;
        const double a0    = aspect(g_page), a1 = aspect(g_page + 1);
        double       h     = H;
        // widths at height h are h/a0 + h/a1; shrink until they fit.
        const double wAtH  = h / a0 + h / a1 + gap;
        if (wAtH > W) h = (W - gap) / (1.0 / a0 + 1.0 / a1);
        const int w0 = (int)(h / a0 + 0.5), w1 = (int)(h / a1 + 0.5), hh = (int)(h + 0.5);
        const int x0 = c.left + m + (W - (w0 + gap + w1)) / 2;
        const int y0 = c.top  + m + (H - hh) / 2;
        out.push_back({ g_page,     { x0, y0, x0 + w0, y0 + hh } });
        out.push_back({ g_page + 1, { x0 + w0 + gap, y0, x0 + w0 + gap + w1, y0 + hh } });
        return;
    }

    // One page, as large as fits.
    const double a = aspect(g_page);
    int w = W, h = (int)(W * a + 0.5);
    if (h > H) { h = H; w = (int)(H / a + 0.5); }
    const int x0 = c.left + m + (W - w) / 2;
    const int y0 = c.top  + m + (H - h) / 2;
    out.push_back({ g_page, { x0, y0, x0 + w, y0 + h } });
}

static void ClampScroll()
{
    if (g_view != VIEW_SCROLL) return;
    std::vector<PagePlace> tmp;
    int total = 0;
    const int keep = g_scrollY;
    g_scrollY = 0;
    LayoutPages(tmp, &total);
    const RECT c = ContentRect();
    int maxY = total - (c.bottom - c.top);
    if (maxY < 0) maxY = 0;
    g_scrollY = keep < 0 ? 0 : (keep > maxY ? maxY : keep);
}

// The page under a point, and its rect.
static bool PageAt(POINT p, PagePlace& out)
{
    std::vector<PagePlace> places;
    LayoutPages(places, nullptr);
    for (const auto& pl : places)
        if (InRect(pl.r, p)) { out = pl; return true; }
    return false;
}

static bool PlaceOf(int page, PagePlace& out)
{
    std::vector<PagePlace> places;
    LayoutPages(places, nullptr);
    for (const auto& pl : places)
        if (pl.page == page) { out = pl; return true; }
    return false;
}

// In scroll mode, the page most of the view is on - what the counter shows
// and what next/previous step from.
static int CurrentPage()
{
    if (g_view != VIEW_SCROLL) return g_page;
    std::vector<PagePlace> places;
    LayoutPages(places, nullptr);
    const RECT c = ContentRect();
    const int  probe = c.top + (c.bottom - c.top) / 3;
    for (const auto& pl : places)
        if (pl.r.bottom > probe) return pl.page;
    return places.empty() ? 0 : places.back().page;
}

static SheetPoint ToNorm(const RECT& r, POINT p)
{
    const float W = (float)(r.right - r.left), H = (float)(r.bottom - r.top);
    SheetPoint s;
    s.x = W > 0 ? (p.x - r.left) / W : 0.0f;
    s.y = H > 0 ? (p.y - r.top)  / H : 0.0f;
    s.x = s.x < 0 ? 0 : (s.x > 1 ? 1 : s.x);
    s.y = s.y < 0 ? 0 : (s.y > 1 ? 1 : s.y);
    return s;
}

static POINT ToPx(const RECT& r, SheetPoint s)
{
    POINT p;
    p.x = r.left + (int)std::lround(s.x * (r.right - r.left));
    p.y = r.top  + (int)std::lround(s.y * (r.bottom - r.top));
    return p;
}

// Where a note sits on the page before it is turned, measured in the font it
// is drawn in. Turned notes rotate this box about its centre.
static RECT NoteRect(HDC hdc, const RECT& page, const SheetText& t)
{
    const int px = (int)std::lround(t.height * (page.bottom - page.top));
    HGDIOBJ o = SelectObject(hdc, NoteFontPx(px));
    RECT r = { 0, 0, 0, 0 };
    DrawU8(hdc, t.text, &r, DT_CALCRECT | DT_NOPREFIX | DT_LEFT);
    SelectObject(hdc, o);
    const POINT tl = ToPx(page, t.pos);
    OffsetRect(&r, tl.x, tl.y);
    return r;
}

static POINT NoteCentre(const RECT& r)
{
    return { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
}

// p turned by `deg` (clockwise on screen) about c.
static POINT Rotate(POINT p, POINT c, float deg)
{
    const double a = deg * 3.14159265358979 / 180.0;
    const double ca = std::cos(a), sa = std::sin(a);
    const double dx = p.x - c.x, dy = p.y - c.y;
    return { c.x + (LONG)std::lround(dx * ca - dy * sa), c.y + (LONG)std::lround(dx * sa + dy * ca) };
}

// Is p on the note, allowing for its turn?
static bool NoteHit(HDC hdc, const RECT& page, const SheetText& t, POINT p, int slop)
{
    RECT r = NoteRect(hdc, page, t);
    InflateRect(&r, slop, slop);
    const POINT local = t.angle != 0.0f ? Rotate(p, NoteCentre(r), -t.angle) : p;
    return InRect(r, local);
}

// The rotate handle: a knob a little above the note's top edge, turning with it.
static const int kKnobR = 7;

static POINT KnobPos(HDC hdc, const RECT& page, const SheetText& t)
{
    const RECT  r = NoteRect(hdc, page, t);
    const POINT c = NoteCentre(r);
    return Rotate({ c.x, r.top - 18 }, c, t.angle);
}

// ===========================================================================
// Toolbar layout and painting
// ===========================================================================
static void AddTb(int id, int l, int t, int r, int b) { g_tb.push_back({ id, { l, t, r, b } }); }

static const TbItem* TbFind(int id)
{
    for (const auto& it : g_tb) if (it.id == id) return &it;
    return nullptr;
}

static int TbHit(POINT p)
{
    for (const auto& it : g_tb)
        if (it.id != TB_PAGELBL && InRect(it.r, p)) return it.id;
    return TB_NONE;
}

// Is p on the toolbar - a button, a floating button, or the column itself?
static bool OverBar(POINT p)
{
    return TbHit(p) != TB_NONE || p.x < g_tbW;
}

static int TextW(HDC hdc, const char* s)
{
    RECT r = { 0, 0, 0, 0 };
    DrawTextA(hdc, s, -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    return r.right - r.left;
}

// One column of square buttons, top to bottom:
//   collapse | Page 2-Up Scroll | Pencil Text Eraser | colours | Undo | < page >
// with More pinned to the bottom. Collapsed: a floating expand chevron in the
// lower left and floating < > in the lower right, over the music.
static void LayoutToolbar(HDC hdc, int cw, int ch)
{
    g_tb.clear();
    HGDIOBJ o = SelectObject(hdc, UiFont());
    TEXTMETRIC tm = {};
    GetTextMetrics(hdc, &tm);
    const int th  = (int)tm.tmHeight;
    const int sq  = th + 14;            // square buttons
    const int pad = 5;
    const int gap = 3;
    const int grp = 10;                 // between groups
    SelectObject(hdc, o);

    if (g_tbCollapsed)
    {
        g_tbW = 0;
        const int y = ch - pad - sq;
        AddTb(TB_EXPAND, pad, y, pad + sq, y + sq);
        AddTb(TB_NEXT, cw - pad - sq, y, cw - pad, y + sq);
        AddTb(TB_PREV, cw - pad - 2 * sq - gap, y, cw - pad - sq - gap, y + sq);
        g_minClientW = 3 * sq + 4 * pad + 2 * gap;
        g_minClientH = sq + 2 * pad + 60;
        return;
    }

    const int x = pad;
    int y = pad;
    auto add = [&](int id, int h) { AddTb(id, x, y, x + sq, y + h); y += h + gap; };

    add(TB_COLLAPSE, sq);                          y += grp - gap;
    add(TB_VIEW_PAGE, sq);
    add(TB_VIEW_TWO, sq);
    add(TB_VIEW_SCROLL, sq);                       y += grp - gap;
    add(TB_PEN, sq);
    add(TB_TEXT, sq);
    add(TB_ERASE, sq);                             y += grp - gap;
    for (int i = 0; i < kSheetPaletteCount; ++i) add(TB_COLOR0 + i, sq - 6);
    y += grp - gap;
    add(TB_UNDO, sq);                              y += grp - gap;
    add(TB_PREV, sq);
    add(TB_PAGELBL, th * 2 + 4);                   // "1-2" over "/ 3"
    add(TB_NEXT, sq);

    // More sits at the foot of the column, or right after Undo if the
    // window is shorter than the column.
    const int moreY = (ch - pad - sq > y + grp) ? ch - pad - sq : y + grp;
    AddTb(TB_MORE, x, moreY, x + sq, moreY + sq);

    g_tbW        = sq + 2 * pad + 1;               // +1: the rule on its right
    g_minClientW = g_tbW + 160;
    g_minClientH = moreY + sq + pad;
}
static bool TbEnabled(int id)
{
    const int n = PageCount();
    switch (id)
    {
    case TB_PREV:  return n > 0 && (g_view == VIEW_SCROLL ? CurrentPage() > 0 || g_scrollY > 0 : g_page > 0);
    case TB_NEXT:
        if (n <= 0) return false;
        if (g_view == VIEW_SCROLL) return CurrentPage() < n - 1;
        return g_page + (g_view == VIEW_TWO ? 2 : 1) < n;
    case TB_PEN: case TB_TEXT: case TB_ERASE: return n > 0;
    case TB_UNDO:  return !g_undo.empty() && Owner() != nullptr;
    case TB_MORE:  return true;
    default:       return true;
    }
}

static bool TbActive(int id)
{
    switch (id)
    {
    case TB_VIEW_PAGE:   return g_view == VIEW_PAGE;
    case TB_VIEW_TWO:    return g_view == VIEW_TWO;
    case TB_VIEW_SCROLL: return g_view == VIEW_SCROLL;
    case TB_PEN:         return g_tool == TOOL_PEN;
    case TB_TEXT:        return g_tool == TOOL_TEXT;
    case TB_ERASE:       return g_tool == TOOL_ERASE;
    default:             return id >= TB_COLOR0 && id < TB_COLOR0 + kSheetPaletteCount &&
                                id - TB_COLOR0 == g_color;
    }
}

static void PaintButtonFace(HDC hdc, const RECT& r, bool active, bool enabled, bool hot, bool down,
                            COLORREF* fgOut)
{
    // The stock buttons' face (see ReaperTheme_PaintButtonFace), so the
    // toolbar matches the rest of Live Tools in Dark mode and out of it.
    ReaperTheme_PaintButtonFace(hdc, r, active, down, hot, enabled, fgOut);
}

// The three view buttons get small drawn pictures rather than words, which
// would not fit the column: one sheet, two side by side, a stack.
static void DrawViewIcon(HDC hdc, const RECT& r, int id, COLORREF fg)
{
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    const int h  = (r.bottom - r.top) * 9 / 20;          // sheet height
    const int w  = h * 3 / 4;                             // sheet width
    HPEN    pen = CreatePen(PS_SOLID, 1, fg);
    HGDIOBJ op  = SelectObject(hdc, pen);
    HGDIOBJ ob  = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    if (id == TB_VIEW_PAGE)
    {
        Rectangle(hdc, cx - w / 2, cy - h / 2, cx + w / 2 + 1, cy + h / 2 + 1);
    }
    else if (id == TB_VIEW_TWO)
    {
        const int sw = w * 4 / 5, sh = h * 4 / 5;
        Rectangle(hdc, cx - sw - 1, cy - sh / 2, cx - 1, cy + sh / 2 + 1);
        Rectangle(hdc, cx + 1,      cy - sh / 2, cx + sw + 1, cy + sh / 2 + 1);
    }
    else   // scroll: one sheet over the top of the next
    {
        const int sh = h * 3 / 5;
        Rectangle(hdc, cx - w / 2, cy - h / 2 - 1, cx + w / 2 + 1, cy - h / 2 + sh);
        Rectangle(hdc, cx - w / 2, cy - h / 2 + sh + 2, cx + w / 2 + 1, cy + h / 2 + 3);
    }
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(pen);
}

static void PaintToolbar(HDC hdc, int ch)
{
    if (!g_tbCollapsed)
    {
        RECT bar = { 0, 0, g_tbW - 1, ch };
        FillSolid(hdc, bar, ReaperTheme_DialogBg());
        RECT line = { g_tbW - 1, 0, g_tbW, ch };
        FillSolid(hdc, line, ReaperTheme_Line());
    }

    HGDIOBJ of = SelectObject(hdc, UiFont());
    SetBkMode(hdc, TRANSPARENT);
    TEXTMETRIC tm = {};
    GetTextMetrics(hdc, &tm);
    const int iconPx = (int)tm.tmHeight + 2;
    const int n = PageCount();

    for (const auto& it : g_tb)
    {
        const bool en  = TbEnabled(it.id);
        const bool act = TbActive(it.id);
        const bool hot = (g_hot == it.id);
        const bool dn  = (g_pressed == it.id && hot);
        RECT r = it.r;
        COLORREF fg = ReaperTheme_Text();

        if (it.id == TB_PAGELBL)
        {
            // Two short lines: the page(s) showing, then the count.
            char top[24] = "-", bot[24] = "";
            if (n > 0)
            {
                const int cur = CurrentPage();
                if (g_view == VIEW_TWO && cur + 1 < n) snprintf(top, sizeof(top), "%d-%d", cur + 1, cur + 2);
                else                                   snprintf(top, sizeof(top), "%d", cur + 1);
                snprintf(bot, sizeof(bot), "/ %d", n);
            }
            SetTextColor(hdc, n > 0 ? ReaperTheme_Text() : ReaperTheme_List().muted);
            RECT r1 = r, r2 = r;
            r1.bottom = (r.top + r.bottom) / 2;
            r2.top    = r1.bottom;
            DrawTextA(hdc, top, -1, &r1, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
            DrawTextA(hdc, bot, -1, &r2, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            continue;
        }
        if (it.id >= TB_COLOR0 && it.id < TB_COLOR0 + kSheetPaletteCount)
        {
            // A swatch: the colour itself, framed; the chosen one gets a
            // heavy frame in the text colour with a light rule inside it.
            const COLORREF c = PaletteRef(it.id - TB_COLOR0);
            RECT sw = r;
            InflateRect(&sw, -3, -2);
            if (hot && !act) InflateRect(&sw, 1, 1);
            FillSolid(hdc, sw, c);
            FrameSolid(hdc, sw, ReaperTheme_Line());
            if (act)
            {
                RECT fr = r;
                FrameSolid(hdc, fr, ReaperTheme_Text(), 2);
                RECT in = sw;
                FrameSolid(hdc, in, RGB(255, 255, 255));
            }
            continue;
        }

        PaintButtonFace(hdc, r, act, en, hot, dn, &fg);
        SetTextColor(hdc, fg);
        switch (it.id)
        {
        case TB_COLLAPSE: DrawIcon(hdc, r, ICON_PREV,  "<<",   iconPx); break;
        case TB_EXPAND:   DrawIcon(hdc, r, ICON_NEXT,  ">>",   iconPx); break;
        case TB_PREV:     DrawIcon(hdc, r, ICON_PREV,  "<",    iconPx); break;
        case TB_NEXT:     DrawIcon(hdc, r, ICON_NEXT,  ">",    iconPx); break;
        case TB_PEN:      DrawIcon(hdc, r, ICON_PEN,   "P",    iconPx); break;
        case TB_TEXT:     DrawIcon(hdc, r, ICON_TEXT,  "T",    iconPx); break;
        case TB_ERASE:    DrawIcon(hdc, r, ICON_ERASE, "E",    iconPx); break;
        case TB_UNDO:     DrawIcon(hdc, r, ICON_UNDO,  "U",    iconPx); break;
        case TB_MORE:     DrawIcon(hdc, r, ICON_MORE,  "...",  iconPx); break;
        case TB_VIEW_PAGE: case TB_VIEW_TWO: case TB_VIEW_SCROLL:
            DrawViewIcon(hdc, r, it.id, fg);
            break;
        default: break;
        }
    }
    SelectObject(hdc, of);
}

// ===========================================================================
// Pages and marks
// ===========================================================================
static HBITMAP ScaledBitmap(HDC ref, int page, int w, int h, std::vector<ScaledPage>& keep)
{
    for (auto it = g_scaled.begin(); it != g_scaled.end(); ++it)
    {
        if (it->doc == g_doc && it->page == page && it->w == w && it->h == h)
        {
            keep.push_back(*it);
            g_scaled.erase(it);
            return keep.back().bmp;
        }
    }
    const SheetDocPage& src = g_doc->pages[(size_t)page];
    if (!src.bmp || w <= 0 || h <= 0) return nullptr;
    HBITMAP bmp = CreateCompatibleBitmap(ref, w, h);
    if (!bmp) return nullptr;
    HDC sdc = CreateCompatibleDC(ref);
    HDC ddc = CreateCompatibleDC(ref);
    HGDIOBJ os = SelectObject(sdc, src.bmp);
    HGDIOBJ od = SelectObject(ddc, bmp);
#ifdef _WIN32
    SetStretchBltMode(ddc, HALFTONE);
    SetBrushOrgEx(ddc, 0, 0, nullptr);
#endif
    StretchBlt(ddc, 0, 0, w, h, sdc, 0, 0, src.w, src.h, SRCCOPY);
    SelectObject(sdc, os);
    SelectObject(ddc, od);
    DeleteDC(sdc);
    DeleteDC(ddc);
    ScaledPage sp;
    sp.doc = g_doc; sp.page = page; sp.w = w; sp.h = h; sp.bmp = bmp;
    keep.push_back(sp);
    return bmp;
}

static float StrokePx(const RECT& page, const SheetStroke& st)
{
    float w = st.width * (float)(page.right - page.left);
    return w < 1.0f ? 1.0f : w;
}

// `gfx` is a GDI+ context on hdc when GDI+ is running (anti-aliased lines),
// null for plain GDI.
static void DrawStroke(HDC hdc, void* gfx, const RECT& page, const SheetStroke& st)
{
    if (st.pts.empty()) return;
    const COLORREF c  = PaletteRef(st.colorIdx);
    const float    wp = StrokePx(page, st);
#ifdef _WIN32
    if (gfx)
    {
        Gdiplus::Graphics& g = *(Gdiplus::Graphics*)gfx;
        const Gdiplus::Color gc(255, GetRValue(c), GetGValue(c), GetBValue(c));
        const float W = (float)(page.right - page.left), H = (float)(page.bottom - page.top);
        if (st.pts.size() == 1)
        {
            Gdiplus::SolidBrush b(gc);
            const float x = page.left + st.pts[0].x * W, y = page.top + st.pts[0].y * H;
            g.FillEllipse(&b, x - wp / 2, y - wp / 2, wp, wp);
            return;
        }
        std::vector<Gdiplus::PointF> pts;
        pts.reserve(st.pts.size());
        for (const auto& p : st.pts)
            pts.push_back(Gdiplus::PointF(page.left + p.x * W, page.top + p.y * H));
        Gdiplus::Pen pen(gc, wp);
        pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawLines(&pen, pts.data(), (INT)pts.size());
        return;
    }
#endif
    // Plain GDI: no anti-aliasing, but the same line.
    HPEN    pen = CreatePen(PS_SOLID, (int)(wp + 0.5f), c);
    HGDIOBJ op  = SelectObject(hdc, pen);
    std::vector<POINT> pts;
    for (const auto& p : st.pts) pts.push_back(ToPx(page, p));
    if (pts.size() == 1) { pts.push_back(pts[0]); pts.back().x += 1; }
    Polyline(hdc, pts.data(), (int)pts.size());
    SelectObject(hdc, op);
    DeleteObject(pen);
}

static void DrawNote(HDC hdc, const RECT& page, const SheetText& t)
{
    const int px = (int)std::lround(t.height * (page.bottom - page.top));
#ifdef _WIN32
    // A turned note: the DC's world transform turns everything drawn about
    // the note's centre. (The clip region is in device units and stays put.)
    bool turned = false;
    if (t.angle != 0.0f && SetGraphicsMode(hdc, GM_ADVANCED))
    {
        const RECT  box = NoteRect(hdc, page, t);
        const POINT c   = NoteCentre(box);
        const double a  = t.angle * 3.14159265358979 / 180.0;
        const float ca = (float)std::cos(a), sa = (float)std::sin(a);
        XFORM xf;
        xf.eM11 = ca;  xf.eM12 = sa;
        xf.eM21 = -sa; xf.eM22 = ca;
        xf.eDx  = (float)c.x - c.x * ca + c.y * sa;
        xf.eDy  = (float)c.y - c.x * sa - c.y * ca;
        turned = SetWorldTransform(hdc, &xf) != 0;
    }
#endif
    HGDIOBJ o = SelectObject(hdc, NoteFontPx(px));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, PaletteRef(t.colorIdx));
    const POINT tl = ToPx(page, t.pos);
    RECT r = { tl.x, tl.y, page.right + 4000, page.bottom + 4000 };
    DrawU8(hdc, t.text, &r, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_NOCLIP);
    SelectObject(hdc, o);
#ifdef _WIN32
    if (turned) ModifyWorldTransform(hdc, nullptr, MWT_IDENTITY);
    SetGraphicsMode(hdc, GM_COMPATIBLE);
#endif
}

// The handle on the note under the pointer (text tool): a stalk up from the
// top edge to a round knob, both turning with the note, and a dotted outline
// of the note so it is clear which one the handle belongs to.
static void DrawRotateHandle(HDC hdc, const RECT& page, const SheetText& t)
{
    const RECT  r = NoteRect(hdc, page, t);
    const POINT c = NoteCentre(r);
    const COLORREF ink = RGB(0x1F, 0x6F, 0xD0);

    POINT box[5] = { { r.left - 3, r.top - 3 }, { r.right + 3, r.top - 3 },
                     { r.right + 3, r.bottom + 3 }, { r.left - 3, r.bottom + 3 },
                     { r.left - 3, r.top - 3 } };
    for (POINT& q : box) q = Rotate(q, c, t.angle);
    const POINT top  = Rotate({ c.x, r.top - 3 }, c, t.angle);
    const POINT knob = KnobPos(hdc, page, t);

    HPEN    pen = CreatePen(PS_DOT, 1, ink);
    HGDIOBJ op  = SelectObject(hdc, pen);
    SetBkMode(hdc, TRANSPARENT);
    Polyline(hdc, box, 5);
    SelectObject(hdc, op);
    DeleteObject(pen);

    HPEN    solid = CreatePen(PS_SOLID, 1, ink);
    HBRUSH  fill  = CreateSolidBrush(RGB(255, 255, 255));
    op = SelectObject(hdc, solid);
    HGDIOBJ ob = SelectObject(hdc, fill);
    MoveToEx(hdc, top.x, top.y, nullptr);
    LineTo(hdc, knob.x, knob.y);
    Ellipse(hdc, knob.x - kKnobR, knob.y - kKnobR, knob.x + kKnobR + 1, knob.y + kKnobR + 1);
    // An open arc inside, so it reads as "turn" rather than a dot.
    Arc(hdc, knob.x - 4, knob.y - 4, knob.x + 5, knob.y + 5,
        knob.x + 4, knob.y, knob.x, knob.y - 4);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(solid);
    DeleteObject(fill);
}

static void DrawMarks(HDC hdc, const PagePlace& pl)
{
    TransitionSnapshot* o = Owner();
    const SheetPage* sp = o ? o->m_sheet.PageOrNull(pl.page) : nullptr;

    // The page, but only the part of it below the toolbar: in scroll mode a
    // page can run up underneath it.
    RECT vis;
    const RECT content = ContentRect();
    if (!IntersectRect(&vis, &pl.r, &content)) return;
    HRGN clip = CreateRectRgn(vis.left, vis.top, vis.right, vis.bottom);
    SelectClipRgn(hdc, clip);
    void* gfx = nullptr;
#ifdef _WIN32
    // One GDI+ context for the whole page rather than one per stroke.
    Gdiplus::Graphics* g = nullptr;
    if ((sp && !sp->strokes.empty()) || (g_drawing && g_drawPage == pl.page))
    {
        if (MusicSheetRender_Startup())
        {
            g = new Gdiplus::Graphics(hdc);
            g->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g->SetClip(Gdiplus::Rect(vis.left, vis.top, vis.right - vis.left, vis.bottom - vis.top));
            gfx = g;
        }
    }
#endif
    if (sp)
    {
        for (const auto& st : sp->strokes) DrawStroke(hdc, gfx, pl.r, st);
    }
    if (g_drawing && g_drawPage == pl.page) DrawStroke(hdc, gfx, pl.r, g_curStroke);
#ifdef _WIN32
    delete g;   // flushes; GDI text goes on top of the lines below
#endif
    if (sp)
    {
        for (int i = 0; i < (int)sp->texts.size(); ++i)
        {
            if (g_edit && pl.page == g_editPage && i == g_editTextIdx) continue;  // being edited
            DrawNote(hdc, pl.r, sp->texts[(size_t)i]);
        }
        if (g_tool == TOOL_TEXT && !g_edit && g_hotNotePage == pl.page &&
            g_hotNoteIdx >= 0 && g_hotNoteIdx < (int)sp->texts.size())
            DrawRotateHandle(hdc, pl.r, sp->texts[(size_t)g_hotNoteIdx]);
    }
    SelectClipRgn(hdc, nullptr);
    DeleteObject(clip);
}

// The middle of an empty window: what is (not) there, and the one thing to do.
static void PaintMessage(HDC hdc, const RECT& c, const std::string& head,
                         const std::string& sub, const char* action)
{
    g_actionRect = {};
    HGDIOBJ of = SelectObject(hdc, UiFont());
    SetBkMode(hdc, TRANSPARENT);
    TEXTMETRIC tm = {};
    GetTextMetrics(hdc, &tm);
    const int lh = (int)tm.tmHeight;
    const int cy = (c.top + c.bottom) / 2;

    RECT r1 = { c.left + 20, cy - lh * 2 - 6, c.right - 20, cy - lh - 6 };
    SetTextColor(hdc, ReaperTheme_Text());
    DrawU8(hdc, head, &r1, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    RECT r2 = { c.left + 20, cy - lh, c.right - 20, cy + lh * 2 };
    SetTextColor(hdc, ReaperTheme_List().muted);
    DrawU8(hdc, sub, &r2, DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    if (action)
    {
        const int w = TextW(hdc, action) + 30;
        const int h = lh + 12;
        g_actionRect = { (c.left + c.right - w) / 2, cy + lh * 2 + 4,
                         (c.left + c.right + w) / 2, cy + lh * 2 + 4 + h };
        COLORREF fg;
        PaintButtonFace(hdc, g_actionRect, false, true, false, g_actionDown, &fg);
        SetTextColor(hdc, fg);
        DrawTextA(hdc, action, -1, &g_actionRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(hdc, of);
}

static void Paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    const int cw = cr.right, ch = cr.bottom;
    if (cw <= 0 || ch <= 0) { EndPaint(hwnd, &ps); return; }

    HDC     mem = CreateCompatibleDC(hdc);
    HBITMAP bb  = CreateCompatibleBitmap(hdc, cw, ch);
    HGDIOBJ ob  = SelectObject(mem, bb);

    LayoutToolbar(mem, cw, ch);
    ClampScroll();
    if (!g_tbCollapsed) PaintToolbar(mem, ch);

    const RECT c = ContentRect();
    const COLORREF ground = Blend(ReaperTheme_DialogBg(), RGB(0, 0, 0), 25);
    FillSolid(mem, c, ground);

    TransitionSnapshot* o = Owner();
    g_actionRect = {};
    if (!o)
    {
        PaintMessage(mem, c, "No scene",
                     "Recall a scene, or select one in Scenes and click its music button.",
                     nullptr);
    }
    else if (!o->m_sheet.Attached())
    {
        PaintMessage(mem, c, o->m_name, "No music sheet attached to this scene.", "Attach Music...");
    }
    else if (g_missing)
    {
        PaintMessage(mem, c, "Can't find " + o->m_sheet.displayName,
                     MusicSheet_Resolve(o->m_sheet) +
                     "\nThe annotations are kept. Locate the file to see them again.",
                     "Locate File...");
    }
    else if (!g_doc)
    {
        PaintMessage(mem, c, "Loading " + o->m_sheet.displayName + "...", "", nullptr);
    }
    else if (!g_doc->Ok())
    {
        PaintMessage(mem, c, o->m_sheet.displayName,
                     g_doc->error.empty() ? "Could not open this file." : g_doc->error,
                     "Choose Another File...");
    }
    else
    {
        std::vector<PagePlace> places;
        LayoutPages(places, nullptr);
        std::vector<ScaledPage> keep;
        HRGN clip = CreateRectRgn(c.left, c.top, c.right, c.bottom);
        SelectClipRgn(mem, clip);
        for (const auto& pl : places)
        {
            const int w = pl.r.right - pl.r.left, h = pl.r.bottom - pl.r.top;
            // A hairline shadow so the paper reads against the ground.
            RECT sh = pl.r;
            OffsetRect(&sh, 2, 2);
            FillSolid(mem, sh, Blend(ground, RGB(0, 0, 0), 40));
            if (HBITMAP b = ScaledBitmap(hdc, pl.page, w, h, keep))
            {
                HDC     bdc = CreateCompatibleDC(hdc);
                HGDIOBJ obb = SelectObject(bdc, b);
                BitBlt(mem, pl.r.left, pl.r.top, w, h, bdc, 0, 0, SRCCOPY);
                SelectObject(bdc, obb);
                DeleteDC(bdc);
            }
            else
            {
                FillSolid(mem, pl.r, RGB(255, 255, 255));
            }
            DrawMarks(mem, pl);
            SelectClipRgn(mem, clip);   // DrawMarks cleared it
        }
        SelectClipRgn(mem, nullptr);
        DeleteObject(clip);
        // Pages scrolled out of view are kept while they are still the size
        // they would be drawn at (scroll mode: every page shares a width), so
        // scrolling back does not re-scale them. Anything else is stale.
        for (auto& sp : g_scaled)
        {
            bool reuse = false;
            if (sp.doc == g_doc && keep.size() < 12)
                for (const auto& k : keep) if (k.w == sp.w) { reuse = true; break; }
            if (reuse) keep.push_back(sp);
            else if (sp.bmp) DeleteObject(sp.bmp);
        }
        g_scaled = std::move(keep);
    }

    // Collapsed: the floating buttons, over the music.
    if (g_tbCollapsed) PaintToolbar(mem, ch);

    BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob);
    DeleteObject(bb);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static void Redraw()
{
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
}

static void UpdateCaption()
{
    if (!g_wnd) return;
    TransitionSnapshot* o = Owner();
    std::string cap = "Music";
    if (o) cap += " - " + o->m_name;
#ifdef _WIN32
    SetWindowTextW(g_wnd, Widen(cap).c_str());
#else
    SetWindowText(g_wnd, cap.c_str());
#endif
}

// ===========================================================================
// Navigation
// ===========================================================================
static void GoToPage(int page)
{
    CommitTextEdit(true);
    const int n = PageCount();
    if (n <= 0) return;
    if (page < 0) page = 0;
    if (page > n - 1) page = n - 1;
    if (g_view == VIEW_SCROLL)
    {
        // Scroll so the page's top sits at the top of the view: the heights
        // of the pages above it, as LayoutPages stacks them.
        const RECT c = ContentRect();
        const int W = (c.right - c.left) - 2 * Margin();
        int y = 0;
        for (int p = 0; p < page; ++p)
        {
            const SheetDocPage& pg = g_doc->pages[(size_t)p];
            y += (pg.w > 0 ? (int)(W * (double)pg.h / pg.w + 0.5) : W) + Margin();
        }
        g_scrollY = y;
        ClampScroll();
    }
    else
    {
        g_page = page;
        ClampView();
    }
    Redraw();
}

static void StepPage(int dir)
{
    const int n = PageCount();
    if (n <= 0) return;
    const int step = (g_view == VIEW_TWO) ? 2 : 1;
    const int cur  = CurrentPage();
    // Scrolled partway down the first page, "back" means its top.
    if (dir < 0 && cur == 0) { if (g_view == VIEW_SCROLL) GoToPage(0); return; }
    int next = cur + dir * step;
    if (next < 0) next = 0;
    if (next > n - 1) return;   // already on the last
    GoToPage(next);
}

static void SetView(int v)
{
    CommitTextEdit(true);
    const int cur = CurrentPage();
    g_view = v;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", v);
    SetExtState(kSec, kViewKey, buf, true);
    g_scrollY = 0;
    GoToPage(cur);
    Redraw();
}

static void SetTool(int t)
{
    CommitTextEdit(true);
    g_tool = (g_tool == t) ? TOOL_NONE : t;   // clicking the active tool puts it down
    g_hotNotePage = g_hotNoteIdx = -1;
    Redraw();
}

static void SetColor(int c)
{
    g_color = c;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", c);
    SetExtState(kSec, kColorKey, buf, true);
    // Picking a colour means wanting to mark something.
    if (g_tool == TOOL_NONE || g_tool == TOOL_ERASE) g_tool = TOOL_PEN;
    if (g_edit)
    {
        g_editColor = c;
        InvalidateRect(g_edit, nullptr, TRUE);
    }
    Redraw();
}

// ===========================================================================
// Strokes
// ===========================================================================

// Ramer-Douglas-Peucker: drop the points that do not change the line's shape
// by more than eps. A mouse trail is hundreds of points; a stroke stored in
// the project is tens.
static void Simplify(std::vector<SheetPoint>& pts, float epsX, float epsY)
{
    if (pts.size() < 3) return;
    std::vector<char> keep(pts.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<size_t, size_t>> stack;
    stack.push_back({ 0, pts.size() - 1 });
    while (!stack.empty())
    {
        const auto seg = stack.back();
        stack.pop_back();
        const SheetPoint a = pts[seg.first], b = pts[seg.second];
        // Distances in pixel-ish units: x and y scale differently on a page.
        const float ax = a.x / epsX, ay = a.y / epsY, bx = b.x / epsX, by = b.y / epsY;
        const float dx = bx - ax, dy = by - ay;
        const float len2 = dx * dx + dy * dy;
        float  worst = 0.0f;
        size_t wi    = 0;
        for (size_t i = seg.first + 1; i < seg.second; ++i)
        {
            const float px = pts[i].x / epsX, py = pts[i].y / epsY;
            float d2;
            if (len2 <= 1e-12f) d2 = (px - ax) * (px - ax) + (py - ay) * (py - ay);
            else
            {
                const float cross = dx * (ay - py) - dy * (ax - px);
                d2 = cross * cross / len2;
            }
            if (d2 > worst) { worst = d2; wi = i; }
        }
        if (worst > 1.0f)
        {
            keep[wi] = 1;
            stack.push_back({ seg.first, wi });
            stack.push_back({ wi, seg.second });
        }
    }
    std::vector<SheetPoint> out;
    for (size_t i = 0; i < pts.size(); ++i) if (keep[i]) out.push_back(pts[i]);
    pts.swap(out);
}

static void BeginStroke(const PagePlace& pl, POINT p)
{
    g_drawing  = true;
    g_drawPage = pl.page;
    g_curStroke = SheetStroke();
    g_curStroke.colorIdx = g_color;
    g_curStroke.pts.push_back(ToNorm(pl.r, p));
    g_lastPt = p;
    SetCapture(g_wnd);
    Redraw();
}

static void ExtendStroke(POINT p)
{
    PagePlace pl;
    if (!PlaceOf(g_drawPage, pl)) return;
    const int dx = p.x - g_lastPt.x, dy = p.y - g_lastPt.y;
    if (dx * dx + dy * dy < 4) return;   // under 2px: not worth a point
    g_curStroke.pts.push_back(ToNorm(pl.r, p));
    g_lastPt = p;
    Redraw();
}

static void EndStroke(bool keep)
{
    if (!g_drawing) return;
    g_drawing = false;
    if (GetCapture() == g_wnd) ReleaseCapture();
    TransitionSnapshot* o = Owner();
    PagePlace pl;
    if (keep && o && !g_curStroke.pts.empty() && PlaceOf(g_drawPage, pl))
    {
        // About three-quarters of a pixel at the size it was drawn.
        const float ex = 0.75f / (float)(pl.r.right - pl.r.left);
        const float ey = 0.75f / (float)(pl.r.bottom - pl.r.top);
        Simplify(g_curStroke.pts, ex, ey);
        PushUndoPage(g_drawPage);
        o->m_sheet.Page(g_drawPage).strokes.push_back(g_curStroke);
        NoteEdited();
    }
    g_curStroke = SheetStroke();
    g_drawPage  = -1;
    Redraw();
}

// ===========================================================================
// Eraser
// ===========================================================================
static float SegDist2(float px, float py, float ax, float ay, float bx, float by)
{
    const float dx = bx - ax, dy = by - ay;
    const float l2 = dx * dx + dy * dy;
    float t = (l2 > 0.0f) ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0.0f;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const float qx = ax + t * dx - px, qy = ay + t * dy - py;
    return qx * qx + qy * qy;
}

// Remove whatever is under p: strokes it touches, notes it is inside.
static void EraseAt(POINT p)
{
    TransitionSnapshot* o = Owner();
    PagePlace pl;
    if (!o || !PageAt(p, pl)) return;
    const SheetPage* existing = o->m_sheet.PageOrNull(pl.page);
    if (!existing || existing->IsEmpty()) return;   // nothing to erase; do not grow pages
    SheetPage* sp = &o->m_sheet.Page(pl.page);

    const float W = (float)(pl.r.right - pl.r.left), H = (float)(pl.r.bottom - pl.r.top);
    std::vector<int> strokes, texts;
    for (int i = 0; i < (int)sp->strokes.size(); ++i)
    {
        const SheetStroke& st = sp->strokes[(size_t)i];
        const float tol = StrokePx(pl.r, st) / 2.0f + 6.0f;
        bool hit = false;
        for (size_t k = 0; k < st.pts.size() && !hit; ++k)
        {
            const float ax = pl.r.left + st.pts[k].x * W, ay = pl.r.top + st.pts[k].y * H;
            const SheetPoint& nb = st.pts[k + 1 < st.pts.size() ? k + 1 : k];
            const float bx = pl.r.left + nb.x * W, by = pl.r.top + nb.y * H;
            if (SegDist2((float)p.x, (float)p.y, ax, ay, bx, by) <= tol * tol) hit = true;
        }
        if (hit) strokes.push_back(i);
    }
    HDC dc = GetDC(g_wnd);
    for (int i = 0; i < (int)sp->texts.size(); ++i)
    {
        if (NoteHit(dc, pl.r, sp->texts[(size_t)i], p, 3)) texts.push_back(i);
    }
    ReleaseDC(g_wnd, dc);
    if (strokes.empty() && texts.empty()) return;

    if (!g_erasedThisDrag) { PushUndoPage(pl.page); g_erasedThisDrag = true; }
    for (int k = (int)strokes.size() - 1; k >= 0; --k)
        sp->strokes.erase(sp->strokes.begin() + strokes[(size_t)k]);
    for (int k = (int)texts.size() - 1; k >= 0; --k)
        sp->texts.erase(sp->texts.begin() + texts[(size_t)k]);
    NoteEdited();
    Redraw();
}

// ===========================================================================
// Notes - the in-place text box
// ===========================================================================
static std::string EditTextU8()
{
    if (!g_edit) return std::string();
#ifdef _WIN32
    const int n = GetWindowTextLengthW(g_edit);
    std::wstring w((size_t)n + 1, L'\0');
    GetWindowTextW(g_edit, &w[0], n + 1);
    w.resize((size_t)n);
    std::string s = Narrow(w.c_str());
#else
    char buf[4096] = {};
    GetWindowText(g_edit, buf, sizeof(buf));
    std::string s = buf;
#endif
    std::string out;
    for (char ch : s) if (ch != '\r') out += ch;
    // Trailing blank lines are a slip of the Enter key, not content.
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

// Fit the box to what has been typed, with room for the next few letters.
static void SizeEditor()
{
    PagePlace pl;
    if (!g_edit || !PlaceOf(g_editPage, pl)) return;
    const int px = (int)std::lround(g_editHeight * (pl.r.bottom - pl.r.top));
    std::string s = EditTextU8();
    if (s.empty()) s = "M";
    HDC dc = GetDC(g_edit);
    HGDIOBJ o = SelectObject(dc, g_editFont);
    RECT r = { 0, 0, 0, 0 };
    DrawU8(dc, s + "  ", &r, DT_CALCRECT | DT_NOPREFIX | DT_LEFT);
    SelectObject(dc, o);
    ReleaseDC(g_edit, dc);
    const POINT tl = ToPx(pl.r, g_editPos);
    int w = (r.right - r.left) + px * 2;
    if (w < px * 6) w = px * 6;
    int h = (r.bottom - r.top) + 6;
    if (h < px + 6) h = px + 6;
    SetWindowPos(g_edit, nullptr, tl.x - 3, tl.y - 3, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    switch (msg)
    {
    case WM_KEYDOWN:
        if (wp == VK_RETURN && !shift) { PostMessage(g_wnd, WM_SHEET_EDITDONE, 1, 0); return 0; }
        if (wp == VK_ESCAPE)           { PostMessage(g_wnd, WM_SHEET_EDITDONE, 0, 0); return 0; }
        break;
    case WM_CHAR:
        // Swallow the Enter and Escape that closed the box (the beep, the
        // stray line break); Shift+Enter is a line break and goes through.
        if ((wp == '\r' && !shift) || wp == 27) return 0;
        break;
    case WM_KILLFOCUS:
        if (!g_editClosing) PostMessage(g_wnd, WM_SHEET_EDITDONE, 1, 0);
        break;
#ifdef _WIN32
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | CallWindowProc(g_editOldProc, h, msg, wp, lp);
#endif
    }
    return CallWindowProc(g_editOldProc, h, msg, wp, lp);
}

static void BeginTextEdit(const PagePlace& pl, SheetPoint pos, int existingIdx)
{
    CommitTextEdit(true);
    TransitionSnapshot* o = Owner();
    if (!o) return;

    g_editPage    = pl.page;
    g_editTextIdx = existingIdx;
    g_editPos     = pos;
    g_editHeight  = 0.025f;
    g_editColor   = g_color;
    std::string initial;
    if (existingIdx >= 0)
    {
        const SheetText& t = o->m_sheet.Page(pl.page).texts[(size_t)existingIdx];
        g_editPos    = t.pos;
        g_editHeight = t.height;
        g_editColor  = t.colorIdx;
        initial      = t.text;
    }
    const int px = (int)std::lround(g_editHeight * (pl.r.bottom - pl.r.top));
    g_editFont = NoteFontPx(px);

    std::string ctl;
    for (char ch : initial) { if (ch == '\n') ctl += '\r'; ctl += ch; }

#ifdef _WIN32
    g_edit = CreateWindowExW(0, L"EDIT", Widen(ctl).c_str(),
                             WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOHSCROLL |
                             ES_AUTOVSCROLL | ES_WANTRETURN,
                             0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)IDC_SHEET_TEXTEDIT, g_hInst, nullptr);
#else
    g_edit = CreateWindowEx(0, "Edit", ctl.c_str(),
                            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOHSCROLL |
                            ES_AUTOVSCROLL | ES_WANTRETURN,
                            0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)IDC_SHEET_TEXTEDIT, g_hInst, nullptr);
#endif
    if (!g_edit) return;
    SendMessage(g_edit, WM_SETFONT, (WPARAM)g_editFont, FALSE);
#ifdef _WIN32
    SendMessage(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
#endif
    g_editOldProc = (WNDPROC)SetWindowLongPtr(g_edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
    g_editClosing = false;
    SizeEditor();
    SetFocus(g_edit);
    SendMessage(g_edit, EM_SETSEL, 0, (LPARAM)-1);
    Redraw();
}

// Close the box. With `keep`, what was typed replaces the note (or becomes a
// new one); an emptied note is deleted.
static void CommitTextEdit(bool keep)
{
    if (!g_edit || g_editClosing) return;
    g_editClosing = true;

    const std::string text = EditTextU8();
    TransitionSnapshot* o = Owner();
    if (keep && o && g_editPage >= 0)
    {
        SheetPage& pg = o->m_sheet.Page(g_editPage);
        const bool had = g_editTextIdx >= 0 && g_editTextIdx < (int)pg.texts.size();
        const bool changed = had ? (pg.texts[(size_t)g_editTextIdx].text != text ||
                                    pg.texts[(size_t)g_editTextIdx].colorIdx != g_editColor)
                                 : !text.empty();
        if (changed)
        {
            PushUndoPage(g_editPage);
            SheetPage& p2 = o->m_sheet.Page(g_editPage);   // PushUndo may not move it, but be safe
            if (had && text.empty())
                p2.texts.erase(p2.texts.begin() + g_editTextIdx);
            else if (had)
            {
                p2.texts[(size_t)g_editTextIdx].text     = text;
                p2.texts[(size_t)g_editTextIdx].colorIdx = g_editColor;
            }
            else
            {
                SheetText t;
                t.pos      = g_editPos;
                t.height   = g_editHeight;
                t.colorIdx = g_editColor;
                t.text     = text;
                p2.texts.push_back(t);
            }
            NoteEdited();
        }
    }

    HWND e = g_edit;
    g_edit = nullptr;
    const bool hadFocus = (GetFocus() == e);
    SetWindowLongPtr(e, GWLP_WNDPROC, (LONG_PTR)g_editOldProc);
    DestroyWindow(e);
    g_editOldProc = nullptr;
    g_editPage    = -1;
    g_editTextIdx = -1;
    g_editClosing = false;
    if (hadFocus && g_wnd) SetFocus(g_wnd);
    Redraw();
}

// A note under p on this page, or -1.
static int NoteAt(const PagePlace& pl, POINT p)
{
    TransitionSnapshot* o = Owner();
    const SheetPage* sp = o ? o->m_sheet.PageOrNull(pl.page) : nullptr;
    if (!sp) return -1;
    HDC dc = GetDC(g_wnd);
    int hit = -1;
    for (int i = (int)sp->texts.size() - 1; i >= 0 && hit < 0; --i)
    {
        if (NoteHit(dc, pl.r, sp->texts[(size_t)i], p, 3)) hit = i;
    }
    ReleaseDC(g_wnd, dc);
    return hit;
}

// The note with the rotate handle, if it still exists.
static SheetText* HotNote(PagePlace* plOut)
{
    TransitionSnapshot* o = Owner();
    if (!o || g_hotNoteIdx < 0) return nullptr;
    const SheetPage* sp = o->m_sheet.PageOrNull(g_hotNotePage);
    if (!sp || g_hotNoteIdx >= (int)sp->texts.size()) return nullptr;
    PagePlace pl;
    if (!PlaceOf(g_hotNotePage, pl)) return nullptr;
    if (plOut) *plOut = pl;
    return &o->m_sheet.Page(g_hotNotePage).texts[(size_t)g_hotNoteIdx];
}

static void ClearHotNote()
{
    if (g_hotNoteIdx < 0) return;
    g_hotNotePage = g_hotNoteIdx = -1;
    Redraw();
}

// Is p on the hot note's rotate knob?
static bool KnobHit(POINT p)
{
    if (g_tool != TOOL_TEXT || g_edit) return false;
    PagePlace pl;
    SheetText* t = HotNote(&pl);
    if (!t) return false;
    HDC dc = GetDC(g_wnd);
    const POINT k = KnobPos(dc, pl.r, *t);
    ReleaseDC(g_wnd, dc);
    const int dx = p.x - k.x, dy = p.y - k.y, r = kKnobR + 3;
    return dx * dx + dy * dy <= r * r;
}

// Text tool, pointer moving: give the note under it the handle. The handle
// stays while the pointer is anywhere near that note (the knob sits above
// it, and the pointer has to cross the gap to reach it).
static void UpdateHotNote(POINT p)
{
    if (g_tool != TOOL_TEXT || g_edit || OverBar(p)) { ClearHotNote(); return; }
    {
        PagePlace pl;
        if (SheetText* t = HotNote(&pl))
        {
            HDC dc = GetDC(g_wnd);
            const bool close = NoteHit(dc, pl.r, *t, p, 28);
            ReleaseDC(g_wnd, dc);
            if (close) return;
        }
    }
    PagePlace pl;
    int page = -1, idx = -1;
    if (PageAt(p, pl) && (idx = NoteAt(pl, p)) >= 0) page = pl.page;
    if (page != g_hotNotePage || idx != g_hotNoteIdx)
    {
        g_hotNotePage = page;
        g_hotNoteIdx  = idx;
        Redraw();
    }
#ifdef _WIN32
    if (g_hotNoteIdx >= 0)
    {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, g_wnd, 0 };
        TrackMouseEvent(&tme);
    }
#endif
}

// Set the hot note's angle, with one undo step per gesture.
static void SetHotNoteAngle(float deg)
{
    SheetText* t = HotNote(nullptr);
    if (!t || t->angle == deg) return;
    if (!g_rotatedThisDrag)
    {
        PushUndoPage(g_hotNotePage);
        g_rotatedThisDrag = true;
        t = HotNote(nullptr);
        if (!t) return;
    }
    t->angle = deg;
    Redraw();
}

static float QuarterTurn(double deg)
{
    deg = std::floor(deg / 90.0 + 0.5) * 90.0;
    while (deg > 180.0)   deg -= 360.0;
    while (deg <= -180.0) deg += 360.0;
    return (float)deg;
}

// Dragging the knob: the note turns to face the pointer, in quarter turns.
static void RotateHotNoteTo(POINT p)
{
    PagePlace pl;
    SheetText* t = HotNote(&pl);
    if (!t) return;
    HDC dc = GetDC(g_wnd);
    const POINT c = NoteCentre(NoteRect(dc, pl.r, *t));
    ReleaseDC(g_wnd, dc);
    if (p.x == c.x && p.y == c.y) return;

    // Until the pointer has really moved this is a click, not a drag.
    const int mx = p.x - g_rotDownPt.x, my = p.y - g_rotDownPt.y;
    if (!g_rotDragging && mx * mx + my * my < 25) return;
    g_rotDragging = true;

    // 0 with the knob straight up: atan2 of "up" is -90.
    const double deg = std::atan2((double)(p.y - c.y), (double)(p.x - c.x)) * 180.0 / 3.14159265358979 + 90.0;
    const float  q   = QuarterTurn(deg);
    SetHotNoteAngle(q);
}

static void EndRotate(bool released)
{
    if (!g_rotating) return;
    g_rotating = false;
    if (GetCapture() == g_wnd) ReleaseCapture();
    if (released && !g_rotDragging)
    {
        // A click on the knob: a quarter turn clockwise, Shift for back.
        if (SheetText* t = HotNote(nullptr))
        {
            const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            SetHotNoteAngle(QuarterTurn(t->angle + (back ? -90.0 : 90.0)));
        }
    }
    if (g_rotatedThisDrag) NoteEdited();
    g_rotatedThisDrag = false;
    g_rotDragging     = false;
    Redraw();
}

// ---------------------------------------------------------------------------
// Moving notes
// ---------------------------------------------------------------------------
static void MoveNoteTo(POINT p)
{
    const int dx = p.x - g_moveDownPt.x, dy = p.y - g_moveDownPt.y;
    if (!g_noteMoving)
    {
        if (dx * dx + dy * dy < 25) return;   // still a click
        g_noteMoving = true;
        PushUndoPage(g_movePage);
    }
    TransitionSnapshot* o = Owner();
    PagePlace pl;
    if (!o || !PlaceOf(g_movePage, pl)) return;
    SheetPage& pg = o->m_sheet.Page(g_movePage);
    if (g_moveIdx < 0 || g_moveIdx >= (int)pg.texts.size()) return;
    const float W = (float)(pl.r.right - pl.r.left), H = (float)(pl.r.bottom - pl.r.top);
    if (W <= 0 || H <= 0) return;
    SheetPoint np = { g_moveOrigPos.x + dx / W, g_moveOrigPos.y + dy / H };
    // Keep the note's corner on the page; the rest may hang over the edge.
    np.x = np.x < 0 ? 0 : (np.x > 0.98f ? 0.98f : np.x);
    np.y = np.y < 0 ? 0 : (np.y > 0.98f ? 0.98f : np.y);
    pg.texts[(size_t)g_moveIdx].pos = np;
    Redraw();
}

// Released: a moved note is done; a note that was only clicked opens for
// editing.
static void EndNotePress(bool released)
{
    if (!g_notePressed) return;
    g_notePressed = false;
    if (GetCapture() == g_wnd) ReleaseCapture();
    if (g_noteMoving)
    {
        g_noteMoving = false;
        NoteEdited();
        Redraw();
        return;
    }
    PagePlace pl;
    if (released && PlaceOf(g_movePage, pl))
    {
        g_hotNotePage = g_hotNoteIdx = -1;
        BeginTextEdit(pl, SheetPoint(), g_moveIdx);
    }
}

// ===========================================================================
// Attach / detach (shared with the Scenes sidebar)
// ===========================================================================
static bool PickSheetFile(HWND parent, const TransitionSnapshot* owner, std::string& out)
{
#ifdef _WIN32
    wchar_t file[4096] = {};
    std::wstring initDir;
    if (owner && owner->m_sheet.Attached())
    {
        std::wstring cur = Widen(MusicSheet_Resolve(owner->m_sheet));
        const size_t cut = cur.find_last_of(L"\\/");
        if (cut != std::wstring::npos) initDir = cur.substr(0, cut);
    }
    if (initDir.empty())
    {
        // The project's own folder (GetProjectPath is the recording path).
        char proj[4096] = {};
        EnumProjects(-1, proj, (int)sizeof(proj));
        std::wstring w = Widen(proj);
        const size_t cut = w.find_last_of(L"\\/");
        if (cut != std::wstring::npos) initDir = w.substr(0, cut);
    }
    OPENFILENAMEW ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = parent;
    ofn.lpstrFilter  = L"Music sheets (PDF, images)\0*.pdf;*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0"
                       L"PDF\0*.pdf\0"
                       L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0"
                       L"All files\0*.*\0";
    ofn.lpstrFile    = file;
    ofn.nMaxFile     = 4096;
    ofn.lpstrInitialDir = initDir.empty() ? nullptr : initDir.c_str();
    ofn.lpstrTitle   = L"Attach Music Sheet";
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    out = Narrow(file);
    return !out.empty();
#else
    (void)parent; (void)owner;
    char file[4096] = {};
    if (!GetUserFileNameForRead(file, "Attach Music Sheet", "pdf")) return false;
    out = file;
    return !out.empty();
#endif
}

// After an attachment changes: the sidebar's button, and the window if it
// is showing that scene.
static void AfterSheetChanged(TransitionSnapshot* owner)
{
    TransitionWnd_RefreshSheetButton();
    if (g_wnd && owner == Owner())
    {
        SyncDoc();
        g_page = 0; g_scrollY = 0;
        ClampView();
        UpdateCaption();
        Redraw();
    }
}

// ---------------------------------------------------------------------------
// Keeping a copy in the project
//
// A chart attached from anywhere else (Downloads, a shared scores folder, a
// USB stick) is copied into a "Music Sheets" folder beside the .rpp and
// linked from there, so the project folder carries its charts with it and
// moving or deleting the original does not break anything. A chart already
// inside the project folder is linked where it is. An unsaved project has no
// folder yet, so the file is linked where it is.
// ---------------------------------------------------------------------------
static const char* kSheetFolder = "Music Sheets";

static const char* SepStr()
{
#ifdef _WIN32
    return "\\";
#else
    return "/";
#endif
}

static bool IsSheetFile(const std::string& path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    static const char* ok[] = { "pdf", "png", "jpg", "jpeg", "bmp", "gif", "tif", "tiff" };
    for (const char* e : ok) if (ext == e) return true;
    return false;
}

static bool PathExists(const std::string& path)
{
    return FileStamp(path) != 0;
}

// Byte-for-byte the same file? Used to recognise a chart already copied in,
// so attaching it twice does not leave "Verse (2).pdf" lying around.
static bool SameContent(const std::string& a, const std::string& b)
{
#ifdef _WIN32
    FILE* fa = _wfopen(Widen(a).c_str(), L"rb");
    FILE* fb = _wfopen(Widen(b).c_str(), L"rb");
#else
    FILE* fa = fopen(a.c_str(), "rb");
    FILE* fb = fopen(b.c_str(), "rb");
#endif
    bool same = fa && fb;
    if (same)
    {
        char ba[65536], bb[65536];
        for (;;)
        {
            const size_t na = fread(ba, 1, sizeof(ba), fa);
            const size_t nb = fread(bb, 1, sizeof(bb), fb);
            if (na != nb || memcmp(ba, bb, na) != 0) { same = false; break; }
            if (na == 0) break;
        }
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

static bool CopyFileU8(const std::string& from, const std::string& to)
{
#ifdef _WIN32
    return CopyFileW(Widen(from).c_str(), Widen(to).c_str(), TRUE) != 0;   // never overwrite
#else
    FILE* in = fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE* out = fopen(to.c_str(), "wb");
    if (!out) { fclose(in); return false; }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    return ok;
#endif
}

static bool MakeDir(const std::string& dir)
{
#ifdef _WIN32
    if (CreateDirectoryW(Widen(dir).c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(dir.c_str(), 0777) == 0 || errno == EEXIST;
#endif
}

static bool IsUnder(const std::string& dir, const std::string& path)
{
    if (dir.empty() || path.size() <= dir.size() + 1) return false;
    for (size_t i = 0; i < dir.size(); ++i)
    {
        char a = dir[i], b = path[i];
#ifdef _WIN32
        if (a == '/') a = '\\';
        if (b == '/') b = '\\';
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
#endif
        if (a != b) return false;
    }
    const char after = path[dir.size()];
    return after == '\\' || after == '/';
}

// The path to link: a copy in <project>/Music Sheets, or `src` itself when
// no copy is wanted or it could not be made.
static std::string CopyIntoProject(HWND parent, const std::string& src)
{
    const std::string proj = MusicSheet_ProjectDir();
    if (proj.empty() || IsUnder(proj, src)) return src;

    const std::string folder = proj + SepStr() + kSheetFolder;
    if (!MakeDir(folder)) return src;

    std::string name = src;
    const size_t cut = name.find_last_of("\\/");
    if (cut != std::string::npos) name = name.substr(cut + 1);
    const size_t dot = name.find_last_of('.');
    const std::string stem = (dot == std::string::npos) ? name : name.substr(0, dot);
    const std::string ext  = (dot == std::string::npos) ? std::string() : name.substr(dot);

    for (int n = 1; n < 100; ++n)
    {
        char suffix[16] = "";
        if (n > 1) snprintf(suffix, sizeof(suffix), " (%d)", n);
        const std::string dst = folder + SepStr() + stem + suffix + ext;
        if (PathExists(dst))
        {
            if (SameContent(src, dst)) return dst;   // already copied in earlier
            continue;                                // a different chart: next name
        }
        if (CopyFileU8(src, dst)) return dst;
        break;
    }

    // Could not copy (read-only folder, full disk): link the original rather
    // than fail the attach, and say so.
    const std::string msg = "Could not copy the music sheet into the project's \"" +
                            std::string(kSheetFolder) + "\" folder, so it is linked where it is:\n\n" + src;
#ifdef _WIN32
    MessageBoxW(parent, Widen(msg).c_str(), L"Attach Music Sheet", MB_OK | MB_ICONWARNING);
#else
    MessageBox(parent, msg.c_str(), "Attach Music Sheet", MB_OK);
#endif
    return src;
}

// Attach `path` to `owner`: a copy in the project, then the link. A new file
// keeps whatever marks there are: either there are none (a first attach), or
// this is the same chart found again, and marks stored per page land back
// where they were. `confirmReplace` asks first when the scene already has a
// different sheet (a drop can land on the wrong scene; the menu's Replace
// File is already the question).
static bool AttachPath(HWND parent, TransitionSnapshot* owner, const std::string& path,
                       bool confirmReplace)
{
    if (!owner || path.empty()) return false;
    if (!IsSheetFile(path))
    {
#ifdef _WIN32
        MessageBoxW(parent, L"A music sheet can be a PDF or an image (PNG, JPG, BMP, GIF, TIFF).",
                    L"Attach Music Sheet", MB_OK | MB_ICONINFORMATION);
#else
        MessageBox(parent, "A music sheet can be a PDF or an image (PNG, JPG, BMP, GIF, TIFF).",
                   "Attach Music Sheet", MB_OK);
#endif
        return false;
    }
    if (confirmReplace && owner->m_sheet.Attached() && MusicSheet_Resolve(owner->m_sheet) != path)
    {
        std::string name = path;
        const size_t cut = name.find_last_of("\\/");
        if (cut != std::string::npos) name = name.substr(cut + 1);
        const std::string msg = "Replace \"" + owner->m_sheet.displayName + "\" on \"" + owner->m_name +
                                "\" with \"" + name + "\"?\n\nIts annotations are kept.";
#ifdef _WIN32
        if (MessageBoxW(parent, Widen(msg).c_str(), L"Replace Music Sheet",
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
            return false;
#else
        if (MessageBox(parent, msg.c_str(), "Replace Music Sheet", MB_YESNO) != IDYES)
            return false;
#endif
    }

    const std::string linked = CopyIntoProject(parent, path);
    MusicSheet_SetPath(owner->m_sheet, linked.c_str());
    MarkProjectDirty(nullptr);
    Undo_OnStateChangeEx("Attach music sheet", -1, -1);
    AfterSheetChanged(owner);
    return true;
}

bool MusicSheetWnd_Attach(HWND parent, TransitionSnapshot* owner)
{
    if (!owner) return false;
    std::string path;
    if (!PickSheetFile(parent, owner, path)) return false;
    return AttachPath(parent, owner, path, false);
}

bool MusicSheetWnd_AttachDrop(HWND parent, TransitionSnapshot* owner, HDROP drop)
{
    if (!drop) return false;
    std::string path;
#ifdef _WIN32
    wchar_t buf[4096] = {};
    if (DragQueryFileW(drop, 0, buf, 4096)) path = Narrow(buf);
#else
    char buf[4096] = {};
    if (DragQueryFile(drop, 0, buf, sizeof(buf))) path = buf;
#endif
    DragFinish(drop);
    if (path.empty()) return false;
    if (!owner)
    {
#ifdef _WIN32
        MessageBoxW(parent, L"Drop the file onto a scene, or select one first.",
                    L"Attach Music Sheet", MB_OK | MB_ICONINFORMATION);
#else
        MessageBox(parent, "Drop the file onto a scene, or select one first.", "Attach Music Sheet", MB_OK);
#endif
        return false;
    }
    if (!AttachPath(parent, owner, path, true)) return false;
    MusicSheetWnd_ShowScene(owner);
    return true;
}

bool MusicSheetWnd_Detach(HWND parent, TransitionSnapshot* owner)
{
    if (!owner || !owner->m_sheet.Attached()) return false;

    int marks = 0;
    for (const auto& p : owner->m_sheet.pages)
        marks += (int)(p.strokes.size() + p.texts.size());
    std::string msg = "Remove the music sheet \"" + owner->m_sheet.displayName +
                      "\" from \"" + owner->m_name + "\"?";
    if (marks > 0)
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "\n\nIts %d annotation%s will be removed too.",
                 marks, marks == 1 ? "" : "s");
        msg += buf;
    }
    msg += "\n\nThe file itself is not deleted.";
#ifdef _WIN32
    if (MessageBoxW(parent, Widen(msg).c_str(), L"Remove Music Sheet",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return false;
#else
    if (MessageBox(parent, msg.c_str(), "Remove Music Sheet", MB_YESNO) != IDYES)
        return false;
#endif

    if (owner == Owner()) { CommitTextEdit(false); g_undo.clear(); }
    owner->m_sheet.Clear();
    MarkProjectDirty(nullptr);
    Undo_OnStateChangeEx("Remove music sheet", -1, -1);
    AfterSheetChanged(owner);
    return true;
}

// ===========================================================================
// The sidebar button
// ===========================================================================
static int TrashZoneW(const RECT& rc)
{
    const int h = rc.bottom - rc.top;
    int w = h * 13 / 10;
    if (w > (rc.right - rc.left) / 3) w = (rc.right - rc.left) / 3;
    return w;
}

void MusicSheetWnd_DrawSheetButton(const DRAWITEMSTRUCT* dis, const TransitionSnapshot* owner)
{
    HDC  hdc = dis->hDC;
    RECT rc  = dis->rcItem;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0 || !owner;
    const bool down     = (dis->itemState & ODS_SELECTED) != 0;
    const bool attached = owner && owner->m_sheet.Attached();
    const bool missing  = attached && !MusicSheet_Exists(owner->m_sheet);
    // The same face as the painted buttons around it (Settings, Layers...).
    COLORREF fg;
    ReaperTheme_PaintButtonFace(hdc, rc, false, down && !disabled,
                                !disabled && ReaperTheme_IsHot(dis->hwndItem), !disabled, &fg);
    const COLORREF ruleC = ReaperTheme_MatchTheme() ? ReaperTheme_Line()
                                                    : ReaperTheme_Sys(COLOR_BTNSHADOW);

    HFONT   f  = (HFONT)SendMessage(dis->hwndItem, WM_GETFONT, 0, 0);
    HGDIOBJ of = f ? SelectObject(hdc, f) : nullptr;
    TEXTMETRIC tm = {};
    GetTextMetrics(hdc, &tm);
    const int iconPx = (int)tm.tmHeight + 1;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);

    const int h = rc.bottom - rc.top;
    RECT clip = { rc.left + 2, rc.top, rc.left + 2 + h, rc.bottom };
    DrawIcon(hdc, clip, ICON_CLIP, "", iconPx);
    const bool hasClipIcon = IconFont(iconPx) != nullptr;

    RECT text = rc;
    text.left = hasClipIcon ? clip.right : rc.left + 6;
    text.right -= 4;
    if (attached)
    {
        const int tw = TrashZoneW(rc);
        RECT trash = { rc.right - tw, rc.top, rc.right, rc.bottom };
        // A rule between the two zones so the trash reads as its own button.
        RECT rule = { trash.left, rc.top + 3, trash.left + 1, rc.bottom - 3 };
        FillSolid(hdc, rule, ruleC);
        DrawIcon(hdc, trash, ICON_TRASH, "X", iconPx);
        text.right = trash.left - 3;

        std::string label = owner->m_sheet.displayName;
        if (missing)
        {
            label += " (missing)";
            SetTextColor(hdc, disabled ? fg : RGB(0xD4, 0x2A, 0x2A));
        }
        DrawU8(hdc, label, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    else
    {
        DrawTextA(hdc, "Attach Music...", -1, &text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    if ((dis->itemState & ODS_FOCUS) && !disabled)
    {
        RECT fr = rc;
        InflateRect(&fr, -2, -2);
        DrawFocusRect(hdc, &fr);
    }
    if (of) SelectObject(hdc, of);
}

bool MusicSheetWnd_SheetButtonTrashHit(HWND hBtn, POINT ptClient, const TransitionSnapshot* owner)
{
    if (!hBtn || !owner || !owner->m_sheet.Attached()) return false;
    RECT rc;
    GetClientRect(hBtn, &rc);
    return ptClient.x >= rc.right - TrashZoneW(rc) && ptClient.x < rc.right &&
           ptClient.y >= rc.top && ptClient.y < rc.bottom;
}

// ===========================================================================
// Menu
// ===========================================================================
enum { MENU_ATTACH = 1, MENU_CLEAR_PAGE, MENU_CLEAR_ALL, MENU_DETACH, MENU_DOCK, MENU_CLOSE,
       MENU_TOOLBAR, MENU_TOOL_NONE, MENU_TOOL_PEN, MENU_TOOL_TEXT, MENU_TOOL_ERASE,
       MENU_VIEW_PAGE, MENU_VIEW_TWO, MENU_VIEW_SCROLL, MENU_UNDO,
       MENU_COLOR0 = 100 };

static void SetToolbarHidden(bool hidden);
static void TbActivate(int id);

static void ToggleDock();

static void SetToolbarHidden(bool hidden)
{
    CommitTextEdit(true);
    g_tbCollapsed = hidden;
    SetExtState(kSec, kBarKey, hidden ? "1" : "0", true);
    g_hot = g_pressed = TB_NONE;
    Redraw();
}

static void ShowMenu(POINT screen)
{
    CommitTextEdit(true);
    TransitionSnapshot* o = Owner();
    const bool attached = o && o->m_sheet.Attached();
    const int  cur      = CurrentPage();
    const SheetPage* sp = o ? o->m_sheet.PageOrNull(cur) : nullptr;

    HMENU m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, MENU_TOOLBAR, g_tbCollapsed ? "Expand Toolbar" : "Collapse Toolbar (Full Page)");
    if (g_tbCollapsed)
    {
        // With the bar away, what it held lives here.
        const UINT noPages = PageCount() > 0 ? 0 : MF_GRAYED;
        AppendMenuA(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(m, MF_STRING | (g_tool == TOOL_NONE  ? MF_CHECKED : 0), MENU_TOOL_NONE, "Look Only");
        AppendMenuA(m, MF_STRING | noPages | (g_tool == TOOL_PEN   ? MF_CHECKED : 0), MENU_TOOL_PEN,   "Pencil");
        AppendMenuA(m, MF_STRING | noPages | (g_tool == TOOL_TEXT  ? MF_CHECKED : 0), MENU_TOOL_TEXT,  "Text");
        AppendMenuA(m, MF_STRING | noPages | (g_tool == TOOL_ERASE ? MF_CHECKED : 0), MENU_TOOL_ERASE, "Eraser");
        HMENU colors = CreatePopupMenu();
        static const char* kNames[kSheetPaletteCount] = { "Red", "Blue", "Green", "Amber", "Violet", "Black" };
        for (int i = 0; i < kSheetPaletteCount; ++i)
            AppendMenuA(colors, MF_STRING | (g_color == i ? MF_CHECKED : 0), MENU_COLOR0 + i, kNames[i]);
        AppendMenuA(m, MF_POPUP, (UINT_PTR)colors, "Colour");
        AppendMenuA(m, MF_STRING | (TbEnabled(TB_UNDO) ? 0 : MF_GRAYED), MENU_UNDO, "Undo\tCtrl+Z");
        AppendMenuA(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(m, MF_STRING | (g_view == VIEW_PAGE   ? MF_CHECKED : 0), MENU_VIEW_PAGE,   "Page");
        AppendMenuA(m, MF_STRING | (g_view == VIEW_TWO    ? MF_CHECKED : 0), MENU_VIEW_TWO,    "2-Up");
        AppendMenuA(m, MF_STRING | (g_view == VIEW_SCROLL ? MF_CHECKED : 0), MENU_VIEW_SCROLL, "Scroll");
    }
    AppendMenuA(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(m, MF_STRING | (o ? 0 : MF_GRAYED), MENU_ATTACH,
                !attached ? "Attach Music..." : (g_missing ? "Locate File..." : "Replace File..."));
    AppendMenuA(m, MF_STRING | (sp && !sp->IsEmpty() && PageCount() > 0 ? 0 : MF_GRAYED),
                MENU_CLEAR_PAGE, "Clear Annotations on This Page");
    AppendMenuA(m, MF_STRING | (o && o->m_sheet.HasAnnotations() ? 0 : MF_GRAYED),
                MENU_CLEAR_ALL, "Clear All Annotations");
    AppendMenuA(m, MF_STRING | (attached ? 0 : MF_GRAYED), MENU_DETACH, "Remove Music Sheet...");
    AppendMenuA(m, MF_SEPARATOR, 0, nullptr);
    bool isFloat = false;
    const bool docked = DockIsChildOfDock(g_wnd, &isFloat) >= 0;
    AppendMenuA(m, MF_STRING | (docked ? MF_CHECKED : 0), MENU_DOCK, "Dock Music in Docker");
    AppendMenuA(m, MF_STRING, MENU_CLOSE, "Close");
    const int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, 0, g_wnd, nullptr);
    DestroyMenu(m);

    switch (id)
    {
    case MENU_ATTACH: MusicSheetWnd_Attach(g_wnd, o); break;
    case MENU_CLEAR_PAGE:
        if (o) { PushUndoPage(cur); o->m_sheet.Page(cur) = SheetPage(); NoteEdited(); Redraw(); }
        break;
    case MENU_CLEAR_ALL:
        if (o) { PushUndoAll(); o->m_sheet.ClearAnnotations(); NoteEdited(); Redraw(); }
        break;
    case MENU_DETACH: MusicSheetWnd_Detach(g_wnd, o); break;
    case MENU_DOCK:   ToggleDock(); break;
    case MENU_CLOSE:  DestroyWindow(g_wnd); break;
    case MENU_TOOLBAR: SetToolbarHidden(!g_tbCollapsed); break;
    case MENU_TOOL_NONE:  g_tool = TOOL_NONE; g_hotNotePage = g_hotNoteIdx = -1; Redraw(); break;
    case MENU_TOOL_PEN:   if (g_tool != TOOL_PEN)   SetTool(TOOL_PEN);   break;
    case MENU_TOOL_TEXT:  if (g_tool != TOOL_TEXT)  SetTool(TOOL_TEXT);  break;
    case MENU_TOOL_ERASE: if (g_tool != TOOL_ERASE) SetTool(TOOL_ERASE); break;
    case MENU_VIEW_PAGE:   SetView(VIEW_PAGE);   break;
    case MENU_VIEW_TWO:    SetView(VIEW_TWO);    break;
    case MENU_VIEW_SCROLL: SetView(VIEW_SCROLL); break;
    case MENU_UNDO:        DoUndo(); break;
    default:
        if (id >= MENU_COLOR0 && id < MENU_COLOR0 + kSheetPaletteCount) SetColor(id - MENU_COLOR0);
        break;
    }
}

// ===========================================================================
// Toolbar actions
// ===========================================================================
static void TbActivate(int id)
{
    if (!TbEnabled(id)) return;
    switch (id)
    {
    case TB_COLLAPSE:    SetToolbarHidden(true);  break;
    case TB_EXPAND:      SetToolbarHidden(false); break;
    case TB_PREV:        StepPage(-1); break;
    case TB_NEXT:        StepPage(+1); break;
    case TB_VIEW_PAGE:   SetView(VIEW_PAGE); break;
    case TB_VIEW_TWO:    SetView(VIEW_TWO); break;
    case TB_VIEW_SCROLL: SetView(VIEW_SCROLL); break;
    case TB_PEN:         SetTool(TOOL_PEN); break;
    case TB_TEXT:        SetTool(TOOL_TEXT); break;
    case TB_ERASE:       SetTool(TOOL_ERASE); break;
    case TB_UNDO:        DoUndo(); break;
    case TB_MORE:
        if (const TbItem* it = TbFind(TB_MORE))
        {
            POINT p = { it->r.right, it->r.top };
            ClientToScreen(g_wnd, &p);
            ShowMenu(p);
        }
        break;
    default:
        if (id >= TB_COLOR0 && id < TB_COLOR0 + kSheetPaletteCount) SetColor(id - TB_COLOR0);
        break;
    }
}

// ===========================================================================
// Keyboard
//
// REAPER sees keys before the window does, and runs whatever action they are
// bound to. While the window has focus, this claims the page-turn keys and
// Ctrl+Z; everything else goes on to REAPER as usual, so Space still starts
// the transport. While a note is being typed, the text box gets everything.
// ===========================================================================
static int SheetTranslateAccel(MSG* msg, accelerator_register_t*)
{
    if (!msg || !g_wnd || !IsWindow(g_wnd)) return 0;
    if (g_edit && (msg->hwnd == g_edit || GetFocus() == g_edit))
    {
        if (msg->message == WM_KEYDOWN || msg->message == WM_KEYUP || msg->message == WM_CHAR ||
            msg->message == WM_SYSKEYDOWN)
            return -1;
        return 0;
    }
    if (msg->hwnd != g_wnd && !IsChild(g_wnd, msg->hwnd)) return 0;
    if (msg->message != WM_KEYDOWN) return 0;

    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (msg->wParam)
    {
    case VK_RIGHT: case VK_NEXT:  StepPage(+1); return 1;
    case VK_LEFT:  case VK_PRIOR: StepPage(-1); return 1;
    case VK_DOWN:
        if (g_view == VIEW_SCROLL) { g_scrollY += 60; ClampScroll(); Redraw(); }
        else StepPage(+1);
        return 1;
    case VK_UP:
        if (g_view == VIEW_SCROLL) { g_scrollY -= 60; ClampScroll(); Redraw(); }
        else StepPage(-1);
        return 1;
    case VK_HOME: GoToPage(0); return 1;
    case VK_END:  GoToPage(PageCount() - 1); return 1;
    case VK_ESCAPE:
        if (g_tool != TOOL_NONE) { g_tool = TOOL_NONE; Redraw(); return 1; }
        return 0;
    case 'Z':
        if (ctrl) { DoUndo(); return 1; }
        return 0;
    }
    return 0;
}

// ===========================================================================
// Window lifetime
// ===========================================================================
static void SaveFloatRect(HWND hwnd)
{
    bool isFloat = false;
    if (DockIsChildOfDock(hwnd, &isFloat) >= 0) return;
    RECT r;
    if (!GetWindowRect(hwnd, &r)) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "%d %d %d %d", (int)r.left, (int)r.top,
             (int)(r.right - r.left), (int)(r.bottom - r.top));
    SetExtState(kSec, kRectKey, buf, true);
}

static void RestoreFloatRect(HWND hwnd)
{
    const char* v = GetExtState(kSec, kRectKey);
    int x = 0, y = 0, w = 0, h = 0;
    if (!v || sscanf(v, "%d %d %d %d", &x, &y, &w, &h) != 4 || w < 200 || h < 150) return;
#ifdef _WIN32
    // Not onto a monitor that is no longer there.
    RECT r = { x, y, x + w, y + h };
    if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return;
#endif
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

// Create the window if needed, and make sure it can be seen. `activate`
// false for an automatic open on recall: the performer's keyboard focus
// stays where it was.
static HWND EnsureOpen(bool activate)
{
    if (!g_wnd || !IsWindow(g_wnd))
    {
        g_wnd = CreateDialogParam(g_hInst, MAKEINTRESOURCE(IDD_MUSICSHEET), GetMainHwnd(),
                                  SheetDlgProc, 0);
        if (!g_wnd) return nullptr;
        const char* dock = GetExtState(kSec, kDockKey);
        if (dock && atoi(dock) != 0)
        {
            DockWindowAddEx(g_wnd, "Music", "reaper_trans_music", true);
            DockWindowActivate(g_wnd);
        }
        else
        {
            RestoreFloatRect(g_wnd);
            ShowWindow(g_wnd, activate ? SW_SHOW : SW_SHOWNA);
        }
        return g_wnd;
    }
    bool isFloat = false;
    if (DockIsChildOfDock(g_wnd, &isFloat) >= 0)
    {
        if (!IsWindowVisible(g_wnd)) DockWindowActivate(g_wnd);
    }
    else if (!IsWindowVisible(g_wnd))
        ShowWindow(g_wnd, activate ? SW_SHOW : SW_SHOWNA);
    else if (activate)
        SetForegroundWindow(g_wnd);
    return g_wnd;
}

static void ToggleDock()
{
    if (!g_wnd) return;
    bool isFloat = false;
    const bool docked = DockIsChildOfDock(g_wnd, &isFloat) >= 0;
    SetExtState(kSec, kDockKey, docked ? "0" : "1", true);
    g_suppressDockStateSave = true;
    DestroyWindow(g_wnd);
    g_suppressDockStateSave = false;
    EnsureOpen(true);
}

// Point the window at a scene. Same scene: the page stays where it is - a
// subscene recall is a move within the song, not a new song.
static void SetOwner(TransitionSnapshot* owner)
{
    if (owner != Owner())
    {
        CommitTextEdit(true);
        EndStroke(true);
        FlushUndoPoint();
        g_undo.clear();
        g_owner     = owner;
        g_ownerIdx  = -1;
        g_ownerName = owner ? owner->m_name : std::string();
        Owner();                 // fills g_ownerIdx
        g_page    = 0;
        g_scrollY = 0;
    }
    SyncDoc();
    ClampView();
    UpdateCaption();
    Redraw();
}

static INT_PTR CALLBACK SheetDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // The note box: the chosen colour on white paper, whatever the theme.
    if (msg == WM_CTLCOLOREDIT && g_edit && (HWND)lParam == g_edit)
    {
        HDC dc = (HDC)wParam;
        SetTextColor(dc, PaletteRef(g_editColor));
        SetBkColor(dc, RGB(255, 255, 255));
        if (!g_editBrush) g_editBrush = CreateSolidBrush(RGB(255, 255, 255));
        return (INT_PTR)g_editBrush;
    }
    if (INT_PTR r = ReaperTheme_CtlColor(hwnd, msg, wParam, lParam)) return r;
    if (msg == WM_INITDIALOG) ReaperTheme_ApplyDialog(hwnd);

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        g_wnd = hwnd;
        MusicSheetRender_Startup();
        if (const char* v = GetExtState(kSec, kViewKey))
        {
            const int x = atoi(v);
            if (x >= VIEW_PAGE && x <= VIEW_SCROLL) g_view = x;
        }
        if (const char* b = GetExtState(kSec, kBarKey))
            g_tbCollapsed = atoi(b) != 0;
        if (const char* c = GetExtState(kSec, kColorKey))
        {
            const int x = atoi(c);
            if (x >= 0 && x < kSheetPaletteCount) g_color = x;
        }
        g_tool = TOOL_NONE;
        DragAcceptFiles(hwnd, TRUE);   // drop a chart on the window to attach it
        if (!g_accelRegistered)
        {
            memset(&g_accel, 0, sizeof(g_accel));
            g_accel.translateAccel = SheetTranslateAccel;
            g_accel.isLocal        = true;
            plugin_register("accelerator", &g_accel);
            g_accelRegistered = true;
        }
        SetTimer(hwnd, kTimerId, kTimerMs, nullptr);
        SyncDoc();
        UpdateCaption();
        return TRUE;
    }

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        RECT r = { 0, 0, g_minClientW, g_minClientH };
        AdjustWindowRectEx(&r, (DWORD)GetWindowLong(hwnd, GWL_STYLE), FALSE,
                           (DWORD)GetWindowLong(hwnd, GWL_EXSTYLE));
        mmi->ptMinTrackSize.x = r.right - r.left;
        mmi->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        Paint(hwnd);
        return 0;

    case WM_SIZE:
        CommitTextEdit(true);
        ClampScroll();
        Redraw();
        return 0;

#ifdef _WIN32
    case WM_EXITSIZEMOVE:
        SaveFloatRect(hwnd);
        return 0;
#endif

    case WM_SHEET_LOADED:
        OnDocLoaded((SheetDoc*)lParam);
        return TRUE;

    case WM_DROPFILES:
        // Onto the scene showing (a dropped chart for "no scene" is refused).
        CommitTextEdit(true);
        MusicSheetWnd_AttachDrop(hwnd, Owner(), (HDROP)wParam);
        return TRUE;

    case WM_SHEET_EDITDONE:
        CommitTextEdit(wParam != 0);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_SHEET_TEXTEDIT && HIWORD(wParam) == EN_CHANGE)
            SizeEditor();
        return TRUE;

    case WM_TIMER:
        if (wParam == kTimerId)
        {
            // Notice what changed elsewhere: the scene renamed or deleted, the
            // sheet attached or removed from the sidebar, the file re-exported
            // or gone missing, a theme switch.
            static std::string        s_sigName, s_sigPath;
            static TransitionSnapshot* s_sigOwner = nullptr;
            static int                 s_sigTheme = -1;
            static int                 s_tick = 0;
            TransitionSnapshot* o = Owner();
            const std::string name = o ? o->m_name : std::string();
            const std::string path = (o && o->m_sheet.Attached()) ? o->m_sheet.path : std::string();
            if (s_tick % 4 == 0) ReaperTheme_Refresh();   // a theme switch, about once a second
            bool changed = (o != s_sigOwner || name != s_sigName || path != s_sigPath ||
                            ReaperTheme_Generation() != s_sigTheme);
            if (changed)
            {
                s_sigOwner = o; s_sigName = name; s_sigPath = path;
                s_sigTheme = ReaperTheme_Generation();
                SyncDoc();
                UpdateCaption();
                Redraw();
            }
            else if (++s_tick >= 8)   // every two seconds: has the file changed on disk?
            {
                s_tick = 0;
                const bool wasMissing = g_missing;
                SheetDoc*  wasDoc     = g_doc;
                SyncDoc();
                if (wasMissing != g_missing || wasDoc != g_doc) Redraw();
            }

            // One REAPER undo point once marking pauses.
            if (g_undoPointPending && !g_drawing && !g_edit && !g_erasing &&
                GetTickCount() - g_lastEditTick > 1500)
                FlushUndoPoint();
        }
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    {
        POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (g_edit)
        {
            // A colour while typing recolours the note and keeps typing;
            // any other click finishes it.
            const int hit = TbHit(p);
            if (hit >= TB_COLOR0 && hit < TB_COLOR0 + kSheetPaletteCount)
            {
                SetColor(hit - TB_COLOR0);
                SetFocus(g_edit);
                return 0;
            }
            CommitTextEdit(true);
            return 0;
        }
        SetFocus(hwnd);

        if (OverBar(p))
        {
            g_pressed = TbHit(p);
            if (g_pressed != TB_NONE) { g_hot = g_pressed; SetCapture(hwnd); Redraw(); }
            return 0;
        }
        if (g_actionRect.right > g_actionRect.left && InRect(g_actionRect, p))
        {
            g_actionDown = true;
            SetCapture(hwnd);
            Redraw();
            return 0;
        }

        PagePlace pl;
        const bool onPage = PageAt(p, pl);
        switch (g_tool)
        {
        case TOOL_PEN:
            if (onPage && msg == WM_LBUTTONDBLCLK)
            {
                const int hit = NoteAt(pl, p);
                if (hit >= 0)
                {
                    // The first click of the double-click left a dot on the
                    // note; take it back, then edit.
                    if (!g_undo.empty() && g_undo.back().page == pl.page) DoUndo();
                    BeginTextEdit(pl, SheetPoint(), NoteAt(pl, p));
                    break;
                }
            }
            if (onPage) BeginStroke(pl, p);
            break;
        case TOOL_TEXT:
            if (KnobHit(p))
            {
                // Each click is a quarter turn (a double-click is two).
                g_rotating = true;
                g_rotatedThisDrag = false;
                g_rotDragging     = false;
                g_rotDownPt = p;
                SetCapture(hwnd);
                break;
            }
            if (onPage)
            {
                const int hit = NoteAt(pl, p);
                if (hit >= 0)
                {
                    // On a note: drag to move it, click to edit it (decided
                    // on release).
                    TransitionSnapshot* own = Owner();
                    g_notePressed = true;
                    g_noteMoving  = false;
                    g_movePage    = pl.page;
                    g_moveIdx     = hit;
                    g_moveDownPt  = p;
                    g_moveOrigPos = own ? own->m_sheet.Page(pl.page).texts[(size_t)hit].pos : SheetPoint();
                    g_hotNotePage = pl.page;
                    g_hotNoteIdx  = hit;
                    SetCapture(hwnd);
                    Redraw();
                    break;
                }
                g_hotNotePage = g_hotNoteIdx = -1;
                BeginTextEdit(pl, ToNorm(pl.r, p), -1);
            }
            break;
        case TOOL_ERASE:
            g_erasing = true;
            g_erasedThisDrag = false;
            SetCapture(hwnd);
            EraseAt(p);
            break;
        default:
            if (PageCount() <= 0) break;
            if (onPage)
            {
                const int hit = NoteAt(pl, p);
                if (hit >= 0)
                {
                    // Double-click a note to edit it, whatever tool is up.
                    if (msg == WM_LBUTTONDBLCLK) BeginTextEdit(pl, SheetPoint(), hit);
                    break;
                }
            }
            if (g_view == VIEW_SCROLL)
            {
                g_panning    = true;
                g_panY0      = p.y;
                g_panScroll0 = g_scrollY;
                SetCapture(hwnd);
            }
            else
            {
                // Page-turner: the left third goes back, the right third on.
                const RECT c = ContentRect();
                const int  third = (c.right - c.left) / 3;
                if (p.x < c.left + third)        StepPage(-1);
                else if (p.x >= c.right - third) StepPage(+1);
            }
            break;
        }
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (g_drawing)  { ExtendStroke(p); return 0; }
        if (g_erasing)  { EraseAt(p); return 0; }
        if (g_rotating) { RotateHotNoteTo(p); return 0; }
        if (g_notePressed) { MoveNoteTo(p); return 0; }
        UpdateHotNote(p);
        if (g_panning)
        {
            g_scrollY = g_panScroll0 - (p.y - g_panY0);
            ClampScroll();
            Redraw();
            return 0;
        }
        const int hot = TbHit(p);
        if (hot != g_hot && (g_pressed == TB_NONE || hot == g_pressed || hot == TB_NONE))
        {
            g_hot = hot;
            Redraw();
        }
#ifdef _WIN32
        if (hot != TB_NONE)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
#endif
        return 0;
    }

#ifdef _WIN32
    case WM_MOUSELEAVE:
        if (g_hot != TB_NONE && g_pressed == TB_NONE) { g_hot = TB_NONE; Redraw(); }
        if (!g_rotating) ClearHotNote();
        return 0;
#endif

    case WM_LBUTTONUP:
    {
        POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (g_drawing)  { EndStroke(true); return 0; }
        if (g_rotating) { EndRotate(true); return 0; }
        if (g_notePressed) { EndNotePress(true); return 0; }
        if (g_erasing)
        {
            g_erasing = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        }
        if (g_panning)
        {
            g_panning = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        }
        if (g_actionDown)
        {
            g_actionDown = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (InRect(g_actionRect, p)) MusicSheetWnd_Attach(hwnd, Owner());
            Redraw();
            return 0;
        }
        if (g_pressed != TB_NONE)
        {
            const int id = g_pressed;
            g_pressed = TB_NONE;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (TbHit(p) == id) TbActivate(id);
            Redraw();
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        if ((HWND)lParam != hwnd)
        {
            if (g_drawing) EndStroke(true);
            EndRotate(false);
            EndNotePress(false);
            g_erasing = false;
            g_panning = false;
            g_actionDown = false;
            if (g_pressed != TB_NONE) { g_pressed = TB_NONE; Redraw(); }
        }
        return 0;

    case WM_MOUSEWHEEL:
    {
        CommitTextEdit(true);
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (g_view == VIEW_SCROLL)
        {
            const RECT c = ContentRect();
            g_scrollY -= delta * ((c.bottom - c.top) / 6) / 120;
            ClampScroll();
            Redraw();
        }
        else
        {
            // A notch is a page; a touchpad's small deltas add up to one.
            g_wheelAcc += delta;
            while (g_wheelAcc >= 120)  { g_wheelAcc -= 120; StepPage(-1); }
            while (g_wheelAcc <= -120) { g_wheelAcc += 120; StepPage(+1); }
        }
        return 0;
    }

    case WM_SETCURSOR:
        if ((HWND)wParam == hwnd && LOWORD(lParam) == HTCLIENT)
        {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            PagePlace pl;
            const char* cur = IDC_ARROW;
            if (g_notePressed && g_noteMoving) cur = IDC_SIZEALL;
            else if (g_rotating || KnobHit(p)) cur = IDC_HAND;
            else if (!OverBar(p) && PageAt(p, pl))
            {
                if (g_tool == TOOL_PEN || g_tool == TOOL_ERASE) cur = IDC_CROSS;
                else if (g_tool == TOOL_TEXT) cur = NoteAt(pl, p) >= 0 ? IDC_SIZEALL : IDC_IBEAM;
                else if (g_view == VIEW_SCROLL) cur = IDC_SIZENS;
            }
            SetCursor(LoadCursor(nullptr, cur));
            SetWindowLongPtr(hwnd, DWLP_MSGRESULT, TRUE);   // the dialog's answer: handled
            return TRUE;
        }
        break;

    case WM_CONTEXTMENU:
    {
        POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (p.x == -1 && p.y == -1) { p.x = p.y = 0; ClientToScreen(hwnd, &p); }
        ShowMenu(p);
        return TRUE;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return TRUE;

    case WM_DESTROY:
    {
        KillTimer(hwnd, kTimerId);
        CommitTextEdit(true);
        EndStroke(true);
        FlushUndoPoint();
        SaveFloatRect(hwnd);
        bool isFloat = false;
        const bool docked = DockIsChildOfDock(hwnd, &isFloat) >= 0;
        if (!g_suppressDockStateSave)
            SetExtState(kSec, kDockKey, docked ? "1" : "0", true);
        if (docked) DockWindowRemove(hwnd);
        if (g_accelRegistered)
        {
            plugin_register("-accelerator", &g_accel);
            g_accelRegistered = false;
        }
        MusicSheetRender_Cancel();
#ifdef _WIN32
        // A document posted but not yet delivered would be lost with the
        // window; take it off the queue and free it.
        MSG m;
        while (PeekMessage(&m, hwnd, WM_SHEET_LOADED, WM_SHEET_LOADED, PM_REMOVE))
            MusicSheetRender_Free((SheetDoc*)m.lParam);
#endif
        // A closed viewer holds no page bitmaps.
        FreeCache();
        FreeNoteFonts();
        if (g_editBrush) { DeleteObject(g_editBrush); g_editBrush = nullptr; }
        g_hot = g_pressed = TB_NONE;
        g_tool = TOOL_NONE;
        g_wnd = nullptr;
        return TRUE;
    }
    }
    return FALSE;
}

// ===========================================================================
// Public API
// ===========================================================================
void MusicSheetWnd_Init(HINSTANCE hInstance)
{
    g_hInst = hInstance;   // the window is created on first use
}

void MusicSheetWnd_Cleanup()
{
    // REAPER is unloading us: no undo points from here on.
    g_undoPointPending = false;
    if (g_wnd && IsWindow(g_wnd))
    {
        g_suppressDockStateSave = false;
        DestroyWindow(g_wnd);
    }
    g_wnd = nullptr;
    FreeCache();
    MusicSheetRender_Cleanup();
}

int MusicSheetWnd_IsVisible()
{
    if (!g_wnd || !IsWindow(g_wnd)) return 0;
    bool isFloat = false;
    if (DockIsChildOfDock(g_wnd, &isFloat) >= 0) return 1;
    return IsWindowVisible(g_wnd) ? 1 : 0;
}

void MusicSheetWnd_ShowHide()
{
    if (MusicSheetWnd_IsVisible())
    {
        bool isFloat = false;
        if (DockIsChildOfDock(g_wnd, &isFloat) >= 0 && !IsWindowVisible(g_wnd))
        {
            DockWindowActivate(g_wnd);   // docked behind another tab: bring it up
            return;
        }
        DestroyWindow(g_wnd);
        return;
    }
    // Opening: on whatever was showing last, else the last-recalled scene.
    if (!Owner())
    {
        const int slot = TransitionEngine::Get().GetCurrentSlot();
        g_owner = TransitionWnd_SheetOwnerOf(slot);
        g_ownerIdx = -1;
        g_ownerName = g_owner ? g_owner->m_name : std::string();
    }
    if (EnsureOpen(true)) SetOwner(Owner());
}

void MusicSheetWnd_ShowScene(TransitionSnapshot* owner)
{
    if (!EnsureOpen(true)) return;
    SetOwner(owner);
}

void MusicSheetWnd_OnSceneRecalled(TransitionSnapshot* owner, bool openIfAttached)
{
    if (MusicSheetWnd_IsVisible())
    {
        SetOwner(owner);
        return;
    }
    // Closed: remember it, so opening later shows the song on stage now.
    if (owner != Owner())
    {
        g_undo.clear();
        g_owner     = owner;
        g_ownerIdx  = -1;
        g_ownerName = owner ? owner->m_name : std::string();
        g_page = 0; g_scrollY = 0;
    }
    if (openIfAttached && owner && owner->m_sheet.Attached())
        if (EnsureOpen(false)) SetOwner(owner);
}

void MusicSheetWnd_OnProjectReset()
{
    if (g_wnd)
    {
        CommitTextEdit(false);
        EndStroke(false);
    }
    // Whatever was pending belongs to the project being left.
    g_undoPointPending = false;
    g_undo.clear();
    g_owner = nullptr;
    g_ownerIdx = -1;
    g_ownerName.clear();
    g_page = 0; g_scrollY = 0;
    if (g_wnd) { SyncDoc(); UpdateCaption(); Redraw(); }
}

void MusicSheetWnd_ToggleToolbar()
{
    if (g_wnd) SetToolbarHidden(!g_tbCollapsed);
    else
    {
        g_tbCollapsed = !g_tbCollapsed;
        SetExtState(kSec, kBarKey, g_tbCollapsed ? "1" : "0", true);
    }
}

void MusicSheetWnd_NextPage() { if (g_wnd) StepPage(+1); }
void MusicSheetWnd_PrevPage() { if (g_wnd) StepPage(-1); }
