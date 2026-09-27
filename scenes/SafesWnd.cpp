#include "SafesWnd.h"
#include "TransitionEngine.h"    // g_globalSafeMask, g_trackSafes, g_subsceneSafes
#include "TransitionSnapshot.h"  // TS_* bit flags, SafeSet
#include "api.h"                 // GetNumTracks, GetTrack, GetSetMediaTrackInfo, etc.
#include "resource.h"
#include "ReaperTheme.h"
#include "../layers/LayersEngine.h"   // layer names for the layer safe rows
#include "TransitionWnd.h"            // g_snapshots: how many layers each scene holds

extern bool g_trackSafesEnabled;

#ifdef _WIN32
#  include <commctrl.h>
#  include <windowsx.h>
#endif
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Per-track grid columns
//
// The track number on its color, the name, then one dot column per safe.
// A track inside a folder is marked the way a subscene is in the Scenes list:
// indented by its depth with a corner in front of the name. Nothing folds —
// every track always has a row.
//
// Visibility, name and track order are not safes any more: layers own what
// each track shows and where it sits, so there is nothing for a safe to hold
// back. Color and height went the same way. Their TS_* bits still load and
// save, so a project that had them set recalls the way it always did; there
// is just no control for them here.
// ---------------------------------------------------------------------------
enum SafeCol {
    COL_NUM = 0,
    COL_TRACK,
    COL_VOL,
    COL_PAN,
    COL_MUTE,
    COL_SOLO,
    COL_PHASE,
    COL_FX,
    COL_SENDS,
    COL_SENDLVL,
    COL_ALL,
    COL_COUNT
};

// Mapping: SafeCol enum → TS_* bit(s)
static const int k_colBit[COL_COUNT] = {
    0,                           // COL_NUM   – no bit
    0,                           // COL_TRACK – no bit
    TS_VOL,
    TS_PAN,
    TS_MUTE,
    TS_SOLO,
    TS_PHASE,
    TS_FXPARAMS | TS_FXCHAIN,    // FX column covers both
    TS_SENDS,                    // track sends + hardware outputs
    TS_SENDLEVEL,                // send levels only; routing still recalls
    0,                           // COL_ALL – handled specially
};

// Full names are the header items' text; the header paints the short forms.
static const char* k_colName[COL_COUNT] = {
    "#", "Track", "Vol", "Pan", "Mute", "Solo", "Phase", "FX", "Sends",
    "Send Level", "All"
};
static const int k_colWidth[COL_COUNT] = {
    44, 160, 28, 28, 28, 28, 28, 28, 34, 34, 30
};

// Header labels. Phase is the null sign, as on a console; everything else is
// plain ASCII.
#ifdef _WIN32
static const wchar_t* k_colAbbr[COL_COUNT] = {
    L"#", L"Track", L"V", L"P", L"M", L"S", L"Ø", L"FX", L"Snd", L"SLv", L"All"
};
#else
static const char* k_colAbbr[COL_COUNT] = {
    "#", "Track", "V", "P", "M", "S", "Ã", "FX", "Snd", "SLv", "All"
};
#endif

// The grid shows every column, so a list view column index is its SafeCol.
static const int k_ptColCount = COL_COUNT;

// Columns that hold a clickable dot.
static bool IsDotCol(int sc) { return sc >= COL_VOL && sc < COL_COUNT; }

// Bitmask for COL_ALL: every safe the grid has a column for.
static const int k_ptAllBits =
    TS_VOL | TS_PAN | TS_MUTE | TS_SOLO | TS_PHASE |
    TS_FXPARAMS | TS_FXCHAIN | TS_SENDS | TS_SENDLEVEL;

// Folder indent per nesting level, and the color box in the # column.
static const int kFolderIndent = 12;
static const int kColorBoxSize = 10;

// ---------------------------------------------------------------------------
// Row data
// ---------------------------------------------------------------------------
struct SafeRow {
    std::string label;
    GUID        guid      = {};
    int         trackNum  = 0;      // 1-based project position
    int         depth     = 0;      // folder nesting level
    bool        hasColor  = false;
    COLORREF    color     = 0;
};

// ---------------------------------------------------------------------------
// SafesTarget – the three pieces of a safes set, by pointer.
//
// The project set is three loose globals (the engine has always read them
// there and a dozen call sites depend on that), while the subscene and
// per-scene sets are SafeSet members. Pointing at the fields rather than at a
// SafeSet lets one grid implementation edit all three without moving the
// project globals into a struct.
// ---------------------------------------------------------------------------
struct SafesTarget {
    int*  mask    = nullptr;
    bool* trackEn = nullptr;
    std::vector<TrackSafeEntry>* tracks = nullptr;
    bool  valid() const { return mask && trackEn && tracks; }
};

static SafesTarget ProjectTarget()
{
    return { &g_globalSafeMask, &g_trackSafesEnabled, &g_trackSafes };
}
static SafesTarget SetTarget(SafeSet& s)
{
    return { &s.globalMask, &s.trackSafesEnabled, &s.trackSafes };
}

// ---------------------------------------------------------------------------
// SafesPane – everything one open safes grid owns.
//
// There can be two at once (the dockable window and a modal per-scene popup),
// so none of this can live at file scope the way it used to. The pane pointer
// rides in the dialog's DWLP_USER and in both subclasses' reference data.
// ---------------------------------------------------------------------------
struct SafesPane {
    HWND hDlg       = nullptr;
    HWND hList      = nullptr;
    HWND hLayerList = nullptr;   // main window, Layers tab only
    HWND hTabs      = nullptr;   // main window only

    std::vector<SafeRow> rows;
    SafesTarget          tgt;

    bool     isMain   = false;   // main window: tabs + layer table
    int      tab      = 0;       // k_tabProject / k_tabSubscenes / k_tabLayers
    SafeSet* sceneSet = nullptr; // popup only: the snapshot's own set
    bool     dirty    = false;   // popup only: did the user change anything
    HFONT    hBanner  = nullptr; // popup only: bold font for the banner

    // Drag-to-check state
    bool s_cbDragActive   = false;
    bool s_cbDragChecking = false;
    int  s_cbDragLastRow  = -1;
    int  s_cbDragLastCol  = -1;
    bool s_suppressClick  = false;

    // Layer rows as last populated, so the refresh timer can tell whether the
    // layer set has changed.
    std::vector<std::string> layerLabels;

    // Drag state for the Layers tab's Safe column
    bool lyrDragActive  = false;
    bool lyrDragOn      = false;    // what the drag is writing
    int  lyrDragLastRow = -1;
};

// The dockable window's pane. Created once at startup and only hidden on
// close, exactly like the dialog it belongs to.
static SafesPane  g_mainPane;
static HINSTANCE  g_hInst = nullptr;

static SafesPane* PaneOf(HWND hDlg)
{
    return (SafesPane*)GetWindowLongPtr(hDlg, DWLP_USER);
}

// Tab labels for the main window. Project and Subscenes both drive the track
// grid; Layers swaps the grid out for the layer table.
static const char* k_tabName[] = { "Project", "Subscenes", "Layers" };
static const int   k_tabCount  = 3;
enum { k_tabProject = 0, k_tabSubscenes = 1, k_tabLayers = 2 };

// Every control that belongs to the track grid's tabs, hidden on Layers.
static const int k_trackTabIds[] = {
    IDC_GSAFES_GROUP,
    IDC_GSAFE_VOL, IDC_GSAFE_PAN, IDC_GSAFE_MUTE, IDC_GSAFE_SOLO, IDC_GSAFE_PHASE,
    IDC_GSAFE_FX,  IDC_GSAFE_SENDS, IDC_GSAFE_SENDLVL,
    IDC_GSAFE_LAYERS, IDC_GSAFE_SLOTS,
    IDC_SAFESLIST
};

// ---------------------------------------------------------------------------
// Layer safes live in their own list (hLayerList) rather than as rows in the
// track grid: a layer is a single yes/no, not a set of track parameters, so it
// has nothing to say about any of that grid's columns. They are project-level
// only — a subscene inherits whatever the project set says.
// ---------------------------------------------------------------------------
static bool LayerRowSafed(int layerIdx)
{
    if (layerIdx < 0 || layerIdx >= kLayerSafeCount) return false;
    return ((unsigned)g_layerSafeMask & (1u << layerIdx)) != 0;
}

static void SetLayerRowSafed(int layerIdx, bool on)
{
    if (layerIdx < 0 || layerIdx >= kLayerSafeCount) return;
    if (on) g_layerSafeMask = (int)((unsigned)g_layerSafeMask |  (1u << layerIdx));
    else    g_layerSafeMask = (int)((unsigned)g_layerSafeMask & ~(1u << layerIdx));
}

// ---------------------------------------------------------------------------
// Row helpers – all read and write through the pane's current target
// ---------------------------------------------------------------------------
static int GetRowMask(SafesPane* p, int row)
{
    if (!p || !p->tgt.valid()) return 0;
    if (row < 0 || row >= (int)p->rows.size()) return 0;
    for (const auto& e : *p->tgt.tracks)
        if (IsEqualGUID(e.guid, p->rows[row].guid)) return e.mask;
    return 0;
}

static void SetRowMask(SafesPane* p, int row, int mask)
{
    if (!p || !p->tgt.valid()) return;
    if (row < 0 || row >= (int)p->rows.size()) return;
    const GUID& guid = p->rows[row].guid;
    for (auto& e : *p->tgt.tracks)
    {
        if (IsEqualGUID(e.guid, guid)) { e.mask = mask; return; }
    }
    p->tgt.tracks->push_back({ guid, mask });
}

static void ToggleBit(SafesPane* p, int row, int bit)
{
    SetRowMask(p, row, GetRowMask(p, row) ^ bit);
}

// A change the user made. The popup remembers it so the caller can mark the
// project dirty exactly once; the dockable window marks it straight away.
static void NoteChange(SafesPane* p)
{
    if (p) p->dirty = true;
    MarkProjectDirty(nullptr);
}

// ---------------------------------------------------------------------------
// SyncGlobalCheckboxes - push the pane's target (and the other globals this
// dialog owns) back out to the controls.
//
// The dockable dialog is created once at startup and only hidden on close, so
// WM_INITDIALOG runs before any project has been loaded. Everything that
// changes the mask from outside - a project load, an undo, a project-tab
// switch - therefore left these checkboxes showing whatever the previous
// project had. A project that saved the Layers safe came back with the mask
// set and the box unchecked, which greyed out the scene layer dropdown with
// nothing in the UI to explain it; checking and unchecking the box was the
// only way to get the mask to agree with what was on screen again.
// ---------------------------------------------------------------------------
static void SyncGlobalCheckboxes(SafesPane* p)
{
    if (!p || !p->hDlg || !p->tgt.valid()) return;
    HWND hDlg = p->hDlg;
    const int m = *p->tgt.mask;

    CheckDlgButton(hDlg, IDC_GSAFE_VOL,    (m & TS_VOL)   ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_GSAFE_PAN,    (m & TS_PAN)   ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_GSAFE_MUTE,   (m & TS_MUTE)  ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_GSAFE_SOLO,   (m & TS_SOLO)  ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_GSAFE_PHASE,  (m & TS_PHASE) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_GSAFE_FX,     (m & (TS_FXPARAMS|TS_FXCHAIN)) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_GSAFE_SENDS,  (m & TS_SENDS)       ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_GSAFE_SENDLVL,(m & TS_SENDLEVEL)   ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_GSAFE_LAYERS, (m & TS_LAYERS)      ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_GSAFE_SLOTS,  (m & TS_FXSLOTS)     ? BST_CHECKED : BST_UNCHECKED);

    // Slot positions only exist on REAPER v7.75+; disable rather than hide
    // so the control keeps its place in the row on older builds.
    if (!LT_SlotHintsSupported())
        EnableWindow(GetDlgItem(hDlg, IDC_GSAFE_SLOTS), FALSE);



    // Popup-only switches.
    if (p->sceneSet)
    {
        CheckDlgButton(hDlg, IDC_SCSAFE_REPLACE,
            p->sceneSet->replaceGlobal ? BST_CHECKED : BST_UNCHECKED);
    }
}

// Same, for the paths that change the globals from outside the dialog: a
// no-op while the window has not been created yet.
static void SyncDlgFromState()
{
    SafesPane* p = &g_mainPane;
    if (!p->hDlg) return;
    SyncGlobalCheckboxes(p);
    if (p->hLayerList) InvalidateRect(p->hLayerList, nullptr, FALSE);
    if (p->hList)      InvalidateRect(p->hList,      nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Layer safe list: one row per layer slot.
//
// Slots by index rather than one row per existing layer: a scene recall
// replaces the whole layer set, so the slot number is the only reference that
// still means anything on the other side of one. The list is as long as the
// largest layer set any scene holds (or the current one, if that is larger),
// so every slot a recall can bring in has a row.
// ---------------------------------------------------------------------------
static int LayerSafeRowCount()
{
    int n = LayersEngine::Get().GetLayerCount();
    for (const auto& snap : g_snapshots)
        if (snap && (int)snap->m_layers.size() > n) n = (int)snap->m_layers.size();
    return n < kLayerSafeCount ? n : kLayerSafeCount;
}

static std::vector<std::string> LayerRowLabels()
{
    std::vector<std::string> out;
    const int layerCount = LayersEngine::Get().GetLayerCount();
    const int rows       = LayerSafeRowCount();
    for (int i = 0; i < rows; ++i)
    {
        char lbl[160];
        if (i < layerCount)
            snprintf(lbl, sizeof(lbl), "%d.  %s",
                     i + 1, LayersEngine::Get().GetLayer(i).name);
        else
            snprintf(lbl, sizeof(lbl), "%d.  (no layer)", i + 1);
        out.push_back(lbl);
    }
    return out;
}

static void PopulateLayerList(SafesPane* p)
{
    if (!p || !p->hLayerList) return;
    ListView_DeleteAllItems(p->hLayerList);

    p->layerLabels = LayerRowLabels();
    for (int i = 0; i < (int)p->layerLabels.size(); ++i)
    {
        LVITEMA item = {};
        item.mask    = LVIF_TEXT;
        item.iItem   = i;
        item.pszText = (LPSTR)p->layerLabels[i].c_str();
        ListView_InsertItem(p->hLayerList, &item);
        ListView_SetItemText(p->hLayerList, i, 1, (LPSTR)" ");
    }
}

// ---------------------------------------------------------------------------
// Rebuild rows from the current REAPER project: every track, in project
// order, with its folder depth.
// ---------------------------------------------------------------------------
static void RebuildRows(SafesPane* p)
{
    if (!p) return;
    p->rows.clear();

    int depth = 0;
    const int n = GetNumTracks();
    for (int i = 0; i < n; ++i)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;

        int fd = 0;
        if (int* pfd = (int*)GetSetMediaTrackInfo(tr, "I_FOLDERDEPTH", nullptr)) fd = *pfd;

        SafeRow r;
        GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
        r.guid     = pg ? *pg : GUID{};
        r.trackNum = i + 1;
        r.depth    = depth;

        char name[256] = {};
        if (!GetTrackName(tr, name, sizeof(name)) || name[0] == '\0')
            snprintf(name, sizeof(name), "Track %d", i + 1);
        r.label = name;

        // Same convention as GetTrackColor: 0 is "no color", anything else
        // is a native color with 0x1000000 set.
        const int nc = GetTrackColor(tr);
        if (nc != 0)
        {
            int cr = 0, cg = 0, cb = 0;
            ColorFromNative(nc & 0xFFFFFF, &cr, &cg, &cb);
            r.hasColor = true;
            r.color    = RGB(cr, cg, cb);
        }
        p->rows.push_back(r);

        // I_FOLDERDEPTH is the change in depth after this track.
        depth += fd;
        if (depth < 0) depth = 0;
    }
}

// ---------------------------------------------------------------------------
// Populate the ListView from the pane's rows. The list holds only the number
// and name text; dots, colors and arrows are painted from p->rows.
// ---------------------------------------------------------------------------
static void PopulateList(SafesPane* p)
{
    if (!p || !p->hList) return;
    SendMessage(p->hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(p->hList);

    for (int i = 0; i < (int)p->rows.size(); ++i)
    {
        char num[16];
        snprintf(num, sizeof(num), "%d", p->rows[i].trackNum);

        LVITEMA item = {};
        item.mask    = LVIF_TEXT;
        item.iItem   = i;
        item.pszText = num;
        ListView_InsertItem(p->hList, &item);
        ListView_SetItemText(p->hList, i, COL_TRACK, (LPSTR)p->rows[i].label.c_str());

        for (int c = COL_TRACK + 1; c < k_ptColCount; ++c)
            ListView_SetItemText(p->hList, i, c, (LPSTR)" ");
    }
    SendMessage(p->hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(p->hList, nullptr, TRUE);
}

// Refill the grid from p->rows, keeping the scroll position.
static void RepopulateKeepScroll(SafesPane* p)
{
    const int top = ListView_GetTopIndex(p->hList);
    PopulateList(p);
    RECT rcItem;
    if (top > 0 && ListView_GetItemRect(p->hList, 0, &rcItem, LVIR_BOUNDS))
        ListView_Scroll(p->hList, 0, top * (rcItem.bottom - rcItem.top));
}


// Same tracks, names, colors and folder shape as the rows on screen?
static bool SameRows(const std::vector<SafeRow>& a, const std::vector<SafeRow>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const SafeRow& x = a[i];
        const SafeRow& y = b[i];
        if (!IsEqualGUID(x.guid, y.guid) || x.label != y.label ||
            x.trackNum != y.trackNum || x.depth != y.depth ||
            x.hasColor != y.hasColor || (x.hasColor && x.color != y.color))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Keep the dockable window current without a Refresh button: while it is
// visible, a timer rebuilds the rows off the project and repaints only when
// something the grid shows (tracks, names, colors, folder depth, layer names) has
// actually changed, so a click on a dot is never fighting a rebuild.
// ---------------------------------------------------------------------------
static const UINT_PTR kSafesRefreshTimer = 1;

static void RefreshIfChanged(SafesPane* p)
{
    if (!p || !p->hDlg || !IsWindowVisible(p->hDlg)) return;
    if (p->s_cbDragActive || p->lyrDragActive) return;

    SafesPane probe;
    RebuildRows(&probe);
    if (!SameRows(probe.rows, p->rows))
    {
        p->rows.swap(probe.rows);
        if (p->hList) RepopulateKeepScroll(p);
    }
    if (p->hLayerList && LayerRowLabels() != p->layerLabels)
        PopulateLayerList(p);
}

// Text colour for a custom-drawn cell, matching the background
// ReaperTheme_ListCellBg picked for it.
static COLORREF CellFg(HWND hList, int row)
{
    const ReaperListColors& lc = ReaperTheme_List();
    const COLORREF bg = ReaperTheme_ListCellBg(hList, row);
    if (bg == lc.selBg)   return lc.selFg;
    if (bg == lc.selInBg) return lc.selInFg;
    return lc.fg;
}

// A filled dot in the middle of a cell, as in the Layers window.
static void PaintDot(HDC hdc, const RECT& rc, COLORREF fg)
{
    const int cx = (rc.left + rc.right)  / 2;
    const int cy = (rc.top  + rc.bottom) / 2;
    const int r  = 2;
    HBRUSH hb  = CreateSolidBrush(fg);
    HPEN   hp  = CreatePen(PS_SOLID, 1, fg);
    HGDIOBJ ob = SelectObject(hdc, hb);
    HGDIOBJ op = SelectObject(hdc, hp);
    Ellipse(hdc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(hb);
    DeleteObject(hp);
}

// ---------------------------------------------------------------------------
// Helper: given a ListView subitem index (in the per-track list),
// return the SafeCol enum value. Returns -1 for invalid.
// ---------------------------------------------------------------------------
static int PtLvcToSafeCol(int lvc)
{
    if (lvc < 0 || lvc >= k_ptColCount) return -1;
    return lvc;
}

// ---------------------------------------------------------------------------
// Helper: apply a checkbox toggle at (row, SafeCol sc) in the per-track list.
// ---------------------------------------------------------------------------
static void ApplyCellToggle(SafesPane* p, int row, int sc, bool checking)
{
    if (!p || row < 0 || row >= (int)p->rows.size()) return;
    int bit = k_colBit[sc];
    if (sc == COL_ALL)
    {
        int m = GetRowMask(p, row);
        SetRowMask(p, row, checking ? (m | k_ptAllBits) : (m & ~k_ptAllBits));
    }
    else if (bit)
    {
        int m = GetRowMask(p, row);
        SetRowMask(p, row, checking ? (m | bit) : (m & ~bit));
    }
    RECT rcRow;
    ListView_GetItemRect(p->hList, row, &rcRow, LVIR_BOUNDS);
    InvalidateRect(p->hList, &rcRow, FALSE);
    NoteChange(p);
}

// ---------------------------------------------------------------------------
// SafesHeaderSubclassProc – paints the per-track list's column labels in theme
// colours: the short forms, level, centred over the dot columns.
// ---------------------------------------------------------------------------
static LRESULT CALLBACK SafesHeaderSubclassProc(HWND hHdr, UINT msg,
                                                  WPARAM wParam, LPARAM lParam,
                                                  ULONG_PTR /*uId*/, DWORD_PTR /*dwRef*/)
{
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hHdr, &ps);

        RECT rcClient;
        GetClientRect(hHdr, &rcClient);
        HBRUSH hbrFace = CreateSolidBrush(ReaperTheme_Sys(COLOR_BTNFACE));
        FillRect(hdc, &rcClient, hbrFace);
        DeleteObject(hbrFace);

        HGDIOBJ oldFont = SelectObject(hdc, (HFONT)SendMessage(hHdr, WM_GETFONT, 0, 0));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ReaperTheme_Sys(COLOR_BTNTEXT));

        HPEN hPen = CreatePen(PS_SOLID, 1, ReaperTheme_Sys(COLOR_BTNSHADOW));
        HGDIOBJ oldPen = SelectObject(hdc, hPen);

        const int itemCount = Header_GetItemCount(hHdr);
        for (int i = 0; i < itemCount && i < COL_COUNT; ++i)
        {
            RECT rcItem;
            Header_GetItemRect(hHdr, i, &rcItem);

            MoveToEx(hdc, rcItem.right - 1, rcItem.top, nullptr);
            LineTo(hdc, rcItem.right - 1, rcItem.bottom);

            RECT rc = rcItem;
            UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS;
            if (i == COL_TRACK) { rc.left += 4; fmt |= DT_LEFT; }
            else                fmt |= DT_CENTER;
#ifdef _WIN32
            DrawTextW(hdc, k_colAbbr[i], -1, &rc, fmt);
#else
            DrawTextA(hdc, k_colAbbr[i], -1, &rc, fmt);
#endif
        }

        SelectObject(hdc, oldPen);
        DeleteObject(hPen);
        SelectObject(hdc, oldFont);
        EndPaint(hHdr, &ps);
        return 0;
    }

    return DefSubclassProc(hHdr, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// SafesListSubclassProc – drag-to-check multiple checkboxes
// ---------------------------------------------------------------------------
static LRESULT CALLBACK SafesListSubclassProc(HWND hList, UINT msg,
                                               WPARAM wParam, LPARAM lParam,
                                               ULONG_PTR /*uId*/, DWORD_PTR dwRef)
{
    SafesPane* p = (SafesPane*)dwRef;
    if (!p) return DefSubclassProc(hList, msg, wParam, lParam);

    switch (msg)
    {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:   // a quick second press on a dot is still a press
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        LVHITTESTINFO ht = {}; ht.pt = pt;
        ListView_SubItemHitTest(hList, &ht);
        const int row = ht.iItem;
        const int lvc = ht.iSubItem;
        const int sc  = PtLvcToSafeCol(lvc);

        if (row >= 0 && IsDotCol(sc))
        {
            // Determine whether this click is checking or unchecking
            int bit = k_colBit[sc];
            bool isChecked;
            if (sc == COL_ALL)
                isChecked = ((GetRowMask(p, row) & k_ptAllBits) == k_ptAllBits);
            else
                isChecked = bit ? ((GetRowMask(p, row) & bit) != 0) : false;

            p->s_cbDragChecking = !isChecked;
            p->s_cbDragActive   = true;
            p->s_cbDragLastRow  = row;
            p->s_cbDragLastCol  = lvc;
            p->s_suppressClick  = false;

            ApplyCellToggle(p, row, sc, p->s_cbDragChecking);
            SetCapture(hList);
            // Don't let default handler process this click further
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE:
    {
        if (!p->s_cbDragActive || !(wParam & MK_LBUTTON)) break;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        LVHITTESTINFO ht = {}; ht.pt = pt;
        ListView_SubItemHitTest(hList, &ht);
        const int row = ht.iItem;
        const int lvc = ht.iSubItem;
        const int sc  = PtLvcToSafeCol(lvc);
        // Only process if we've moved to a new cell with a dot column
        if (row >= 0 && IsDotCol(sc) &&
            (row != p->s_cbDragLastRow || lvc != p->s_cbDragLastCol))
        {
            p->s_cbDragLastRow = row;
            p->s_cbDragLastCol = lvc;
            p->s_suppressClick = true;
            ApplyCellToggle(p, row, sc, p->s_cbDragChecking);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (p->s_cbDragActive)
        {
            p->s_cbDragActive  = false;
            p->s_suppressClick = false;
            ReleaseCapture();
            return 0;
        }
        break;

    case WM_CAPTURECHANGED:
        if (p->s_cbDragActive)
        {
            p->s_cbDragActive  = false;
            p->s_suppressClick = false;
        }
        break;
    }

    return DefSubclassProc(hList, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// LayerListSubclassProc – the Layers tab's Safe column, with the same press
// and drag as the track grid: the first cell decides whether the drag fills
// or clears, and every row it crosses gets that.
// ---------------------------------------------------------------------------
static void ApplyLayerDot(SafesPane* p, int row, bool on)
{
    if (row < 0 || row >= ListView_GetItemCount(p->hLayerList)) return;
    if (LayerRowSafed(row) == on) return;
    SetLayerRowSafed(row, on);
    RECT rcRow;
    if (ListView_GetItemRect(p->hLayerList, row, &rcRow, LVIR_BOUNDS))
        InvalidateRect(p->hLayerList, &rcRow, FALSE);
    MarkProjectDirty(nullptr);
}

static LRESULT CALLBACK LayerListSubclassProc(HWND hList, UINT msg,
                                              WPARAM wParam, LPARAM lParam,
                                              ULONG_PTR /*uId*/, DWORD_PTR dwRef)
{
    SafesPane* p = (SafesPane*)dwRef;
    if (!p) return DefSubclassProc(hList, msg, wParam, lParam);

    switch (msg)
    {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:   // a quick second press is still a press
    {
        LVHITTESTINFO ht = {};
        ht.pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ListView_SubItemHitTest(hList, &ht);
        if (ht.iItem >= 0 && ht.iSubItem == 1)
        {
            p->lyrDragOn      = !LayerRowSafed(ht.iItem);
            p->lyrDragActive  = true;
            p->lyrDragLastRow = ht.iItem;
            ApplyLayerDot(p, ht.iItem, p->lyrDragOn);
            SetFocus(hList);
            SetCapture(hList);
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE:
    {
        if (!p->lyrDragActive) break;
        if (!(wParam & MK_LBUTTON)) { ReleaseCapture(); break; }
        // Rows only: the drag follows the pointer up and down the list
        // wherever it is horizontally, so a slightly wobbly drag still lands.
        LVHITTESTINFO ht = {};
        ht.pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ListView_SubItemHitTest(hList, &ht);
        if (ht.iItem >= 0 && ht.iItem != p->lyrDragLastRow)
        {
            p->lyrDragLastRow = ht.iItem;
            ApplyLayerDot(p, ht.iItem, p->lyrDragOn);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (p->lyrDragActive) { ReleaseCapture(); return 0; }
        break;

    case WM_CAPTURECHANGED:
        p->lyrDragActive  = false;
        p->lyrDragLastRow = -1;
        break;
    }
    return DefSubclassProc(hList, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// CreateGrid – build the per-track ListView over its placeholder.
// Shared by both templates; they use the same IDC_SAFESLIST id.
// ---------------------------------------------------------------------------
static void CreateGrid(SafesPane* p)
{
    HWND hDlg = p->hDlg;

    INITCOMMONCONTROLSEX icx = { sizeof(icx), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icx);

    HWND hPlaceholder = GetDlgItem(hDlg, IDC_SAFESLIST);
    RECT rc = {};
    GetClientRect(hPlaceholder, &rc);
    MapWindowPoints(hPlaceholder, hDlg, (POINT*)&rc, 2);
    DestroyWindow(hPlaceholder);

    p->hList = CreateWindowExA(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWA, "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
        LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
        hDlg, (HMENU)(UINT_PTR)IDC_SAFESLIST, g_hInst, nullptr);

    ListView_SetExtendedListViewStyle(p->hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    ReaperTheme_ApplyListView(p->hList);

    LVCOLUMNA col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    for (int sc = 0; sc < k_ptColCount; ++sc)
    {
        col.pszText = (LPSTR)k_colName[sc];
        col.cx      = k_colWidth[sc];
        col.fmt     = (sc == COL_TRACK) ? LVCFMT_LEFT : LVCFMT_CENTER;
        ListView_InsertColumn(p->hList, sc, &col);
    }

    HWND hHdr = ListView_GetHeader(p->hList);
    if (hHdr)
        SetWindowSubclass(hHdr, SafesHeaderSubclassProc, 1, (DWORD_PTR)p);

    SetWindowSubclass(p->hList, SafesListSubclassProc, 2, (DWORD_PTR)p);
}

// ---------------------------------------------------------------------------
// LayoutGlobalRow – position the row of global checkboxes inside their
// groupbox. Shared by both templates so they stay visually identical.
// ---------------------------------------------------------------------------
static void LayoutGlobalRow(HWND hDlg, int x, int y, int w, int chkH)
{
    // Row 0: Vol    Pan    Mutes      Solo    Phase
    // Row 1: FX     Sends  Send Level Layers  Slots
    static const int k_gsIds[] = {
        IDC_GSAFE_VOL, IDC_GSAFE_PAN, IDC_GSAFE_MUTE, IDC_GSAFE_SOLO, IDC_GSAFE_PHASE,
        IDC_GSAFE_FX,  IDC_GSAFE_SENDS, IDC_GSAFE_SENDLVL, IDC_GSAFE_LAYERS, IDC_GSAFE_SLOTS,
    };
    static const int k_gsRow[] = { 0,0,0,0,0, 1,1,1,1,1 };
    static const int k_gsCol[] = { 0,1,2,3,4, 0,1,2,3,4 };
    const int count = (int)(sizeof(k_gsIds) / sizeof(k_gsIds[0]));
    const int slot5 = w / 5;
    for (int i = 0; i < count; ++i)
    {
        HWND h = GetDlgItem(hDlg, k_gsIds[i]);
        if (!h) continue;
        SetWindowPos(h, nullptr,
            x + k_gsCol[i] * slot5,
            y + k_gsRow[i] * 16,
            slot5 - 2, chkH, SWP_NOZORDER);
    }
}

// ---------------------------------------------------------------------------
// HandleGlobalToggle – the IDC_GSAFE_* checkboxes, shared by both dialogs.
// Returns true if the command was one of them.
// ---------------------------------------------------------------------------
static bool HandleGlobalToggle(SafesPane* p, int id)
{
    if (!p || !p->tgt.valid()) return false;
    int& m = *p->tgt.mask;
    HWND hDlg = p->hDlg;

    struct { int id; int bits; } k_map[] = {
        { IDC_GSAFE_VOL,    TS_VOL },
        { IDC_GSAFE_PAN,    TS_PAN },
        { IDC_GSAFE_MUTE,   TS_MUTE },
        { IDC_GSAFE_SOLO,   TS_SOLO },
        { IDC_GSAFE_PHASE,  TS_PHASE },
        { IDC_GSAFE_FX,     TS_FXPARAMS | TS_FXCHAIN },
        { IDC_GSAFE_SENDS,  TS_SENDS },
        { IDC_GSAFE_SENDLVL,TS_SENDLEVEL },
        { IDC_GSAFE_LAYERS, TS_LAYERS },
        { IDC_GSAFE_SLOTS,  TS_FXSLOTS },
    };
    for (const auto& e : k_map)
    {
        if (e.id != id) continue;
        if (IsDlgButtonChecked(hDlg, id) == BST_CHECKED) m |=  e.bits;
        else                                             m &= ~e.bits;
        NoteChange(p);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Grid notifications (NM_CLICK / NM_CUSTOMDRAW on the per-track ListView),
// shared by both dialogs.
// ---------------------------------------------------------------------------
static bool HandleGridNotify(SafesPane* p, NMHDR* pnm, LPARAM lParam)
{
    if (!p || !p->hList || pnm->hwndFrom != p->hList) return false;

    if (pnm->code == NM_CLICK)
    {
        // Suppress if drag-to-check already handled this
        if (p->s_suppressClick) { p->s_suppressClick = false; return true; }

        NMITEMACTIVATE* pnia = (NMITEMACTIVATE*)lParam;
        LVHITTESTINFO ht = {};
        ht.pt = pnia->ptAction;
        ListView_SubItemHitTest(p->hList, &ht);
        const int row = ht.iItem;
        const int lvc = ht.iSubItem;
        const int sc  = PtLvcToSafeCol(lvc);
        if (row >= 0 && IsDotCol(sc))
        {
            if (sc == COL_ALL)
            {
                const int m = GetRowMask(p, row);
                bool wasAllSet = ((m & k_ptAllBits) == k_ptAllBits);
                SetRowMask(p, row, wasAllSet ? (m & ~k_ptAllBits) : (m | k_ptAllBits));
            }
            else
            {
                ToggleBit(p, row, k_colBit[sc]);
            }
            RECT rcRow;
            ListView_GetItemRect(p->hList, row, &rcRow, LVIR_BOUNDS);
            InvalidateRect(p->hList, &rcRow, FALSE);
            NoteChange(p);
        }
        return true;
    }

    if (pnm->code != NM_CUSTOMDRAW) return false;

    NMLVCUSTOMDRAW* pcd = (NMLVCUSTOMDRAW*)lParam;
    switch (pcd->nmcd.dwDrawStage)
    {
    case CDDS_PREPAINT:
        SetWindowLongPtr(p->hDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
        return true;

    case CDDS_ITEMPREPAINT:
        ReaperTheme_ListItemPrePaint(pcd, p->hList, false);
        SetWindowLongPtr(p->hDlg, DWLP_MSGRESULT, CDRF_NOTIFYSUBITEMDRAW | CDRF_NEWFONT);
        return true;

    case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
    {
        const int row = (int)pcd->nmcd.dwItemSpec;
        const int lvc = (int)pcd->iSubItem;
        const int sc  = PtLvcToSafeCol(lvc);
        if (sc < 0 || row < 0 || row >= (int)p->rows.size())
        {
            ReaperTheme_ListItemPrePaint(pcd, p->hList, false);
            SetWindowLongPtr(p->hDlg, DWLP_MSGRESULT, CDRF_NEWFONT);
            return true;
        }

        HDC  hdc = pcd->nmcd.hdc;
        const SafeRow&  r  = p->rows[row];
        const COLORREF  bg = ReaperTheme_ListCellBg(p->hList, row);
        const COLORREF  fg = CellFg(p->hList, row);

        // Column 0's custom-draw rect spans the whole row on some comctl
        // versions, so the text cells are measured rather than trusted.
        RECT rcIt = pcd->nmcd.rc;
        if (sc == COL_NUM)
        {
            if (ListView_GetItemRect(p->hList, row, &rcIt, LVIR_BOUNDS))
                rcIt.right = rcIt.left + ListView_GetColumnWidth(p->hList, COL_NUM);
        }
        else if (sc == COL_TRACK)
        {
            ListView_GetSubItemRect(p->hList, row, COL_TRACK, LVIR_BOUNDS, &rcIt);
        }

        SetBkColor(hdc, bg);
        ExtTextOutA(hdc, 0, 0, ETO_OPAQUE, &rcIt, "", 0, nullptr);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, fg);

        if (sc == COL_NUM)
        {
            // A color box on every row, then the track number. An uncolored
            // track gets an empty outline so the column reads the same all the
            // way down.
            const int bs = kColorBoxSize;
            const int by = (rcIt.top + rcIt.bottom - bs) / 2;
            RECT rb = { rcIt.left + 4, by, rcIt.left + 4 + bs, by + bs };
            HPEN    hp = CreatePen(PS_SOLID, 1, ReaperTheme_List().muted);
            HBRUSH  hb = r.hasColor ? CreateSolidBrush(r.color) : nullptr;
            HGDIOBJ op = SelectObject(hdc, hp);
            HGDIOBJ ob = SelectObject(hdc, hb ? (HGDIOBJ)hb : GetStockObject(NULL_BRUSH));
            Rectangle(hdc, rb.left, rb.top, rb.right, rb.bottom);
            SelectObject(hdc, op);
            SelectObject(hdc, ob);
            DeleteObject(hp);
            if (hb) DeleteObject(hb);

            char num[16];
            snprintf(num, sizeof(num), "%d", r.trackNum);
            RECT rn = rcIt;
            rn.left = rb.right + 2;
            DrawTextA(hdc, num, -1, &rn, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        else if (sc == COL_TRACK)
        {
            RECT rt = rcIt;
            rt.left += 4;
            if (r.depth > 0)
            {
                // Indent, then U+2514 U+2500 (box-drawing corner): the same
                // mark the Scenes list puts in front of a subscene.
                rt.left += (r.depth - 1) * kFolderIndent;
                RECT rc2 = rt;
#ifdef _WIN32
                DrawTextW(hdc, L"\u2514\u2500 ", -1, &rc2,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
                DrawTextW(hdc, L"\u2514\u2500 ", -1, &rt,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
#else
                DrawTextA(hdc, "\xE2\x94\x94\xE2\x94\x80 ", -1, &rc2,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
                DrawTextA(hdc, "\xE2\x94\x94\xE2\x94\x80 ", -1, &rt,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
#endif
                rt.left += rc2.right - rc2.left;
            }
            DrawTextA(hdc, r.label.c_str(), -1, &rt,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        else
        {
            const int rowMask = GetRowMask(p, row);
            const bool on = (sc == COL_ALL)
                ? (rowMask & k_ptAllBits) == k_ptAllBits
                : (rowMask & k_colBit[sc]) != 0;
            if (on) PaintDot(hdc, rcIt, fg);
        }

        SetWindowLongPtr(p->hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
        return true;
    }
    }
    return false;
}

// ---------------------------------------------------------------------------
// SwitchTab – repoint the main window's grid at the other safes set, or swap
// the grid out for the layer table.
// ---------------------------------------------------------------------------
static void SwitchTab(SafesPane* p, int tab)
{
    if (!p || !p->isMain) return;
    if (tab < 0 || tab >= k_tabCount) tab = k_tabProject;
    p->tab = tab;
    // The Layers tab keeps the project set behind the (hidden) grid controls
    // so they always have a valid target.
    p->tgt = (tab == k_tabSubscenes) ? SetTarget(g_subsceneSafes) : ProjectTarget();

    SetDlgItemText(p->hDlg, IDC_GSAFES_GROUP,
        tab == k_tabSubscenes
            ? "Subscene Global Safes  (added to the project safes on every subscene recall)"
            : "Global Safes");

    // Layers are project-level (a subscene has no layer set of its own) and
    // share nothing with the track grid's columns, so they get their own tab.
    const bool showLayers = (tab == k_tabLayers);
    for (int id : k_trackTabIds)
        if (HWND h = GetDlgItem(p->hDlg, id)) ShowWindow(h, showLayers ? SW_HIDE : SW_SHOW);
    if (p->hLayerList) ShowWindow(p->hLayerList, showLayers ? SW_SHOW : SW_HIDE);
    HWND hLbl = GetDlgItem(p->hDlg, IDC_SAFESLAYERLBL);
    if (hLbl) ShowWindow(hLbl, showLayers ? SW_SHOW : SW_HIDE);

    PopulateList(p);
    SyncGlobalCheckboxes(p);

    RECT rc; GetClientRect(p->hDlg, &rc);
    SendMessage(p->hDlg, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
    InvalidateRect(p->hDlg, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Main Safes window dialog procedure
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK SafesDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    SafesPane* p = PaneOf(hDlg);

    // Dialog colours come from the REAPER theme (see ReaperTheme.h).
    if (INT_PTR r = ReaperTheme_CtlColor(hDlg, msg, wParam, lParam)) return r;
    if (msg == WM_INITDIALOG) ReaperTheme_ApplyDialog(hDlg);

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        p = &g_mainPane;
        SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)p);
        p->hDlg   = hDlg;
        p->isMain = true;
        p->tab    = k_tabProject;
        p->tgt    = ProjectTarget();

        CreateGrid(p);

        // ---- Tab strip ---------------------------------------------------
        p->hTabs = GetDlgItem(hDlg, IDC_SAFES_TABS);
        if (p->hTabs)
        {
            for (int i = 0; i < k_tabCount; ++i)
            {
                TCITEMA ti = {};
                ti.mask    = TCIF_TEXT;
                ti.pszText = (LPSTR)k_tabName[i];
                SendMessageA(p->hTabs, TCM_INSERTITEMA, i, (LPARAM)&ti);
            }
            TabCtrl_SetCurSel(p->hTabs, k_tabProject);
        }

        // ---- Layer recall safes: its own small table ---------------------
        HWND hLyrPh = GetDlgItem(hDlg, IDC_SAFESLAYERLIST);
        if (hLyrPh)
        {
            RECT rcL = {};
            GetClientRect(hLyrPh, &rcL);
            MapWindowPoints(hLyrPh, hDlg, (POINT*)&rcL, 2);
            DestroyWindow(hLyrPh);

            p->hLayerList = CreateWindowExA(
                WS_EX_CLIENTEDGE,
                WC_LISTVIEWA, "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                rcL.left, rcL.top, rcL.right - rcL.left, rcL.bottom - rcL.top,
                hDlg, (HMENU)(UINT_PTR)IDC_SAFESLAYERLIST, g_hInst, nullptr);

            ListView_SetExtendedListViewStyle(p->hLayerList,
                LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            ReaperTheme_ApplyListView(p->hLayerList);
            SetWindowSubclass(p->hLayerList, LayerListSubclassProc, 3, (DWORD_PTR)p);

            LVCOLUMNA lc = {};
            lc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            lc.pszText = (LPSTR)"Layer"; lc.cx = 240; lc.fmt = LVCFMT_LEFT;
            ListView_InsertColumn(p->hLayerList, 0, &lc);
            lc.pszText = (LPSTR)"Safe";  lc.cx = 48;  lc.fmt = LVCFMT_CENTER;
            ListView_InsertColumn(p->hLayerList, 1, &lc);
        }

        RebuildRows(p);
        PopulateList(p);
        PopulateLayerList(p);

        // Initialize every global control from the state it mirrors. This is
        // the same pass used whenever that state changes from outside.
        SyncGlobalCheckboxes(p);

        // Start on the Project tab, which hides the layer table.
        SwitchTab(p, k_tabProject);

        SetTimer(hDlg, kSafesRefreshTimer, 500, nullptr);

        return TRUE;
    }

    case WM_SIZE:
    {
        if (!p || !p->hList) break;
        RECT rcDlg;
        GetClientRect(hDlg, &rcDlg);
        const int W = rcDlg.right;
        const int H = rcDlg.bottom;
        const int MARGIN = 5;
        const int TAB_H = 22;   // tab strip
        const int GRP_H = 50;   // Global Safes groupbox (2 rows)
        const int CHK_H = 14;   // one checkbox row

        if (p->hTabs)
            SetWindowPos(p->hTabs, nullptr, MARGIN, MARGIN, W - MARGIN*2, TAB_H, SWP_NOZORDER);

        const int grpTop = MARGIN + TAB_H + 2;
        HWND hGrp = GetDlgItem(hDlg, IDC_GSAFES_GROUP);
        if (hGrp) SetWindowPos(hGrp, nullptr, MARGIN, grpTop, W - MARGIN*2, GRP_H, SWP_NOZORDER);

        LayoutGlobalRow(hDlg, MARGIN + 7, grpTop + 13, W - MARGIN*2 - 14, CHK_H);

        // Project / Subscenes: the track list takes everything below.
        const int listTop = grpTop + GRP_H + MARGIN;
        int listBottom    = H - MARGIN;
        if (listBottom < listTop + 40) listBottom = listTop + 40;
        SetWindowPos(p->hList, nullptr,
            MARGIN, listTop, W - MARGIN*2, listBottom - listTop, SWP_NOZORDER);

        // Layers: label under the tab strip, table fills the rest.
        const int LBL_H  = 12;
        const int lyrTop = grpTop + LBL_H + 2;
        int lyrBottom    = H - MARGIN;
        if (lyrBottom < lyrTop + 40) lyrBottom = lyrTop + 40;
        HWND hLyrLbl = GetDlgItem(hDlg, IDC_SAFESLAYERLBL);
        if (hLyrLbl) SetWindowPos(hLyrLbl, nullptr, MARGIN, grpTop, W - MARGIN*2, LBL_H, SWP_NOZORDER);
        if (p->hLayerList)
            SetWindowPos(p->hLayerList, nullptr,
                MARGIN, lyrTop, W - MARGIN*2, lyrBottom - lyrTop, SWP_NOZORDER);
        break;
    }

    case WM_COMMAND:
    {
        if (!p) break;
        const int id = LOWORD(wParam);

        HandleGlobalToggle(p, id);
        break;
    }

    case WM_NOTIFY:
    {
        if (!p) break;
        NMHDR* pnm = (NMHDR*)lParam;

        // ---- Tab strip ---------------------------------------------------
        if (p->hTabs && pnm->hwndFrom == p->hTabs && pnm->code == TCN_SELCHANGE)
        {
            SwitchTab(p, TabCtrl_GetCurSel(p->hTabs));
            break;
        }

        // ---- Layer safe table --------------------------------------------
        if (p->hLayerList && pnm->hwndFrom == p->hLayerList)
        {
            // Presses on the Safe column are handled (and dragged) in
            // LayerListSubclassProc, so only painting comes through here.
            if (pnm->code == NM_CUSTOMDRAW)
            {
                NMLVCUSTOMDRAW* pcd = (NMLVCUSTOMDRAW*)lParam;
                switch (pcd->nmcd.dwDrawStage)
                {
                case CDDS_PREPAINT:
                    SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                    return TRUE;
                case CDDS_ITEMPREPAINT:
                    ReaperTheme_ListItemPrePaint(pcd, p->hLayerList, false);
                    SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYSUBITEMDRAW | CDRF_NEWFONT);
                    return TRUE;
                case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
                {
                    if (pcd->iSubItem != 1)
                    {
                        // Column 0 draws normally, in theme colours
                        ReaperTheme_ListItemPrePaint(pcd, p->hLayerList, false);
                        SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NEWFONT);
                        return TRUE;
                    }

                    const int  row = (int)pcd->nmcd.dwItemSpec;
                    HDC        hdc = pcd->nmcd.hdc;
                    RECT       rcIt = pcd->nmcd.rc;

                    SetBkColor(hdc, ReaperTheme_ListCellBg(p->hLayerList, row));
                    ExtTextOutA(hdc, 0, 0, ETO_OPAQUE, &rcIt, "", 0, nullptr);

                    if (LayerRowSafed(row))
                        PaintDot(hdc, rcIt, CellFg(p->hLayerList, row));

                    SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                    return TRUE;
                }
                }
            }
            break;
        }

        if (HandleGridNotify(p, pnm, lParam)) return TRUE;
        break;
    }

    case WM_TIMER:
        if (wParam == kSafesRefreshTimer) RefreshIfChanged(p);
        break;

    case WM_CLOSE:
        ShowWindow(hDlg, SW_HIDE);
        return TRUE;
    }

    return FALSE;
}

// ---------------------------------------------------------------------------
// SceneSafesDlgProc – modal IDD_SCENE_SAFES popup: a scene's recall filters.
//
// A recall filter is a safe that belongs to one scene — what that scene's
// recall leaves alone — so it is the same grid, with no tabs and no layer
// table, plus the banner and the switch that decides how this set
// combines with the project safes.
// ---------------------------------------------------------------------------
struct SceneSafesInit {
    SafeSet*    set;
    const char* title;
};

static INT_PTR CALLBACK SceneSafesDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    SafesPane* p = PaneOf(hDlg);

    // Dialog colours come from the REAPER theme (see ReaperTheme.h).
    if (INT_PTR r = ReaperTheme_CtlColor(hDlg, msg, wParam, lParam)) return r;
    if (msg == WM_INITDIALOG) ReaperTheme_ApplyDialog(hDlg);

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SceneSafesInit* init = (SceneSafesInit*)lParam;
        p = new SafesPane();
        SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)p);
        p->hDlg     = hDlg;
        p->isMain   = false;
        p->sceneSet = init->set;
        p->tgt      = SetTarget(*init->set);
        // Per-track filters are always on, as in the Global Safes window.
        init->set->trackSafesEnabled = true;

        CreateGrid(p);
        RebuildRows(p);
        PopulateList(p);

        SetDlgItemText(hDlg, IDC_SCSAFE_TITLE, init->title);
        SetWindowTextA(hDlg, init->title);

        // Bold banner so there is no mistaking this for the project-wide grid.
        {
            HFONT hf = (HFONT)SendMessage(hDlg, WM_GETFONT, 0, 0);
            LOGFONT lf = {};
            if (hf && GetObject(hf, sizeof(lf), &lf))
            {
                lf.lfWeight = FW_BOLD;
                p->hBanner = CreateFontIndirect(&lf);
                if (p->hBanner)
                    SendDlgItemMessage(hDlg, IDC_SCSAFE_TITLE, WM_SETFONT,
                                       (WPARAM)p->hBanner, TRUE);
            }
        }

        SyncGlobalCheckboxes(p);
        SetTimer(hDlg, kSafesRefreshTimer, 500, nullptr);
        return TRUE;
    }

    case WM_TIMER:
        if (wParam == kSafesRefreshTimer) RefreshIfChanged(p);
        break;

    case WM_SIZE:
    {
        if (!p || !p->hList) break;
        RECT rcDlg;
        GetClientRect(hDlg, &rcDlg);
        const int W = rcDlg.right;
        const int H = rcDlg.bottom;
        const int MARGIN = 5;
        const int BTN_H = 24, BTN_W = 70;
        const int TITLE_H = 32;
        const int GRP_H = 50;   // two checkbox rows
        const int CHK_H = 14;

        HWND hTitle = GetDlgItem(hDlg, IDC_SCSAFE_TITLE);
        if (hTitle) SetWindowPos(hTitle, nullptr, MARGIN, MARGIN, W - MARGIN*2, TITLE_H, SWP_NOZORDER);

        const int swY = MARGIN + TITLE_H + 2;
        HWND hRp = GetDlgItem(hDlg, IDC_SCSAFE_REPLACE);
        if (hRp) SetWindowPos(hRp, nullptr, MARGIN + 3, swY, W - MARGIN*2 - 3, CHK_H, SWP_NOZORDER);

        const int grpTop = swY + CHK_H + 4;
        HWND hGrp = GetDlgItem(hDlg, IDC_GSAFES_GROUP);
        if (hGrp) SetWindowPos(hGrp, nullptr, MARGIN, grpTop, W - MARGIN*2, GRP_H, SWP_NOZORDER);

        LayoutGlobalRow(hDlg, MARGIN + 7, grpTop + 13, W - MARGIN*2 - 14, CHK_H);

        const int listTop = grpTop + GRP_H + MARGIN;
        const int by      = H - BTN_H - MARGIN;
        int listBottom    = by - MARGIN;
        if (listBottom < listTop + 40) listBottom = listTop + 40;

        SetWindowPos(p->hList, nullptr,
            MARGIN, listTop, W - MARGIN*2, listBottom - listTop, SWP_NOZORDER);

        HWND hClose = GetDlgItem(hDlg, IDC_SCSAFE_CLOSE);
        if (hClose) SetWindowPos(hClose, nullptr, W - MARGIN - BTN_W, by, BTN_W, BTN_H, SWP_NOZORDER);
        break;
    }

    case WM_COMMAND:
    {
        if (!p) break;
        const int id = LOWORD(wParam);

        if (HandleGlobalToggle(p, id)) break;

        switch (id)
        {
        case IDC_SCSAFE_REPLACE:
            p->sceneSet->replaceGlobal =
                (IsDlgButtonChecked(hDlg, IDC_SCSAFE_REPLACE) == BST_CHECKED);
            NoteChange(p);
            break;

        case IDC_SCSAFE_CLOSE:
        case IDOK:
        case IDCANCEL:
            // Every edit is applied in place as it is made, so there is no
            // Cancel to honour — closing by any route is the same answer.
            EndDialog(hDlg, p->dirty ? IDOK : IDCANCEL);
            break;
        }
        break;
    }

    case WM_NOTIFY:
        if (p && HandleGridNotify(p, (NMHDR*)lParam, lParam)) return TRUE;
        break;

    case WM_DESTROY:
        KillTimer(hDlg, kSafesRefreshTimer);
        if (p)
        {
            SetWindowLongPtr(hDlg, DWLP_USER, 0);
            if (p->hBanner) DeleteObject(p->hBanner);
            delete p;
        }
        break;

    case WM_CLOSE:
        EndDialog(hDlg, p && p->dirty ? IDOK : IDCANCEL);
        return TRUE;
    }

    return FALSE;
}

bool SafesWnd_EditSceneSafes(HWND parent, SafeSet& set, const char* title)
{
    SceneSafesInit init { &set, title ? title : "Recall Filters" };
    return DialogBoxParamA(g_hInst, MAKEINTRESOURCEA(IDD_SCENE_SAFES),
                           parent, SceneSafesDlgProc, (LPARAM)&init) == IDOK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void SafesWnd_Init(HINSTANCE hInstance)
{
    g_hInst = hInstance;
    // Before the template is instantiated, not after: the tab strip and the
    // grid are created by the dialog manager from the template, and a class
    // registered afterwards is too late for them.
    INITCOMMONCONTROLSEX icx = { sizeof(icx), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icx);
    g_mainPane.hDlg = CreateDialogParamA(hInstance, MAKEINTRESOURCEA(IDD_SAFES),
                                          nullptr, SafesDlgProc, 0);
    if (g_mainPane.hDlg) SetWindowTextA(g_mainPane.hDlg, "Global Safes");
}

void SafesWnd_Cleanup()
{
    if (g_mainPane.hDlg) { DestroyWindow(g_mainPane.hDlg); g_mainPane.hDlg = nullptr; }
    g_mainPane.hList      = nullptr;
    g_mainPane.hLayerList = nullptr;
    g_mainPane.hTabs      = nullptr;
}

void SafesWnd_ShowHide()
{
    HWND hDlg = g_mainPane.hDlg;
    if (!hDlg) return;
    if (IsWindowVisible(hDlg))
        ShowWindow(hDlg, SW_HIDE);
    else {
        SafesWnd_Refresh();
        ShowWindow(hDlg, SW_SHOW);
        SetForegroundWindow(hDlg);
    }
}

bool SafesWnd_IsVisible()
{
    return g_mainPane.hDlg && IsWindowVisible(g_mainPane.hDlg);
}

void SafesWnd_Refresh()
{
    SafesPane* p = &g_mainPane;
    if (!p->hDlg) return;
    RebuildRows(p);
    if (p->hList) PopulateList(p);
    PopulateLayerList(p);
    SyncGlobalCheckboxes(p);
}

void SafesWnd_AddSelectedTracksToSafes()
{
    const int nsel = CountSelectedTracks(nullptr);
    if (nsel <= 0) return;

    for (int i = 0; i < nsel; ++i)
    {
        MediaTrack* tr = GetSelectedTrack(nullptr, i);
        if (!tr) continue;
        GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
        if (!pg) continue;

        bool found = false;
        for (auto& e : g_trackSafes)
        {
            if (IsEqualGUID(e.guid, *pg)) { e.mask |= k_ptAllBits; found = true; break; }
        }
        if (!found)
            g_trackSafes.push_back({ *pg, k_ptAllBits });
    }

    MarkProjectDirty(nullptr);
    if (g_mainPane.hDlg && g_mainPane.hList)
    {
        RebuildRows(&g_mainPane);
        PopulateList(&g_mainPane);
    }
}

// ---------------------------------------------------------------------------
// GUID helpers (local – same pattern as TransitionSnapshot.cpp)
// ---------------------------------------------------------------------------
static std::string SafesGuidToString(const GUID& g)
{
    WCHAR wbuf[64];
    StringFromGUID2(g, wbuf, 64);
    char buf[64];
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, 64, nullptr, nullptr);
    return buf;
}

static GUID SafesStringToGuid(const char* s)
{
    GUID g = {};
    if (!s || !s[0]) return g;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (wlen <= 0) return g;
    std::vector<WCHAR> wbuf(wlen);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, wbuf.data(), wlen);
    CLSIDFromString(wbuf.data(), &g);
    return g;
}

// ---------------------------------------------------------------------------
// Project persistence
// ---------------------------------------------------------------------------

// A project that has never seen a subscene writes no LTSUBSAFE* lines, so the
// reset value is the default set rather than an empty one — otherwise opening
// an older project would silently drop the order/name/plugin protection that
// makes subscenes behave like variations in the first place.
static bool s_subSafesFromProject = false;

void SafesWnd_ResetForProject()
{
    g_globalSafeMask    = 0;
    g_layerSafeMask     = 0;
    g_trackSafesEnabled = true;
    g_trackSafes.clear();

    g_subsceneSafes                   = SafeSet{};
    g_subsceneSafes.enabled           = true;
    g_subsceneSafes.globalMask        = kSubsceneSafeDefaults;
    g_subsceneSafes.trackSafesEnabled = true;
    s_subSafesFromProject             = false;

    SyncDlgFromState();
}

bool SafesWnd_ProcessLine(const char* line)
{
    if (!line) return false;
    while (*line == ' ' || *line == '\t') ++line;

    // Each of these restores state the dialog is already showing, so the
    // controls have to follow the line in rather than keep the outgoing
    // project's values.
    int val = 0;
    if (sscanf(line, "LTSAFEGLOBAL %d", &val) == 1)
        { g_globalSafeMask = val; SyncDlgFromState(); return true; }
    if (sscanf(line, "LTSAFELAYERS %d", &val) == 1)
        { g_layerSafeMask = val; SyncDlgFromState(); return true; }
    // Per-track safes are always on for the project and subscene sets; the
    // switch for them is gone, so a saved "off" is read and dropped rather
    // than leaving a grid that silently does nothing.
    if (sscanf(line, "LTSAFETRACKSEN %d", &val) == 1)
        { g_trackSafesEnabled = true; SyncDlgFromState(); return true; }

    // ---- Subscene safes ---------------------------------------------------
    if (sscanf(line, "LTSUBSAFEGLOBAL %d", &val) == 1)
    {
        g_subsceneSafes.globalMask = val;
        g_subsceneSafes.enabled    = true;
        if (!s_subSafesFromProject)
        {
            // The project is authoritative from here on: drop the defaults'
            // per-track list so a saved empty list stays empty.
            g_subsceneSafes.trackSafes.clear();
            s_subSafesFromProject = true;
        }
        SyncDlgFromState();
        return true;
    }
    if (sscanf(line, "LTSUBSAFETRACKSEN %d", &val) == 1)
        { g_subsceneSafes.trackSafesEnabled = true; SyncDlgFromState(); return true; }

    char sguid[80] = {};
    if (sscanf(line, "LTSUBSAFETRACK %79s %d", sguid, &val) == 2)
    {
        g_subsceneSafes.trackSafes.push_back({ SafesStringToGuid(sguid), val });
        return true;
    }

    if (sscanf(line, "LTSAFETRACK %79s %d", sguid, &val) == 2)
    {
        TrackSafeEntry e;
        e.guid = SafesStringToGuid(sguid);
        e.mask = val;
        g_trackSafes.push_back(e);
        return true;
    }
    return false;
}

void SafesWnd_SaveConfig(ProjectStateContext* ctx)
{
    ctx->AddLine("LTSAFEGLOBAL %d", g_globalSafeMask);
    if (g_layerSafeMask)
        ctx->AddLine("LTSAFELAYERS %d", g_layerSafeMask);
    ctx->AddLine("LTSAFETRACKSEN %d", g_trackSafesEnabled ? 1 : 0);
    for (const auto& e : g_trackSafes)
    {
        if (e.mask == 0) continue;
        std::string sg = SafesGuidToString(e.guid);
        ctx->AddLine("LTSAFETRACK %s %d", sg.c_str(), e.mask);
    }

    // Subscene safes. Always written, because "no line" has to keep meaning
    // "an older project, use the defaults" — a user who cleared every box
    // needs that to survive a round trip.
    ctx->AddLine("LTSUBSAFEGLOBAL %d", g_subsceneSafes.globalMask);
    ctx->AddLine("LTSUBSAFETRACKSEN %d", g_subsceneSafes.trackSafesEnabled ? 1 : 0);
    for (const auto& e : g_subsceneSafes.trackSafes)
    {
        if (e.mask == 0) continue;
        std::string sg = SafesGuidToString(e.guid);
        ctx->AddLine("LTSUBSAFETRACK %s %d", sg.c_str(), e.mask);
    }
}
