#include "SafesWnd.h"
#include "TransitionEngine.h"    // g_globalSafeMask, g_trackSafes, g_subsceneSafes
#include "TransitionSnapshot.h"  // TS_* bit flags, SafeSet
#include "api.h"                 // GetNumTracks, GetTrack, GetSetMediaTrackInfo, etc.
#include "resource.h"
#include "../layers/LayersEngine.h"   // layer names for the layer safe rows

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
// Full column definitions (used internally for bit mapping)
// ---------------------------------------------------------------------------
enum SafeCol {
    COL_TRACK = 0,
    COL_VOL,
    COL_PAN,
    COL_MUTE,
    COL_SOLO,
    COL_PHASE,
    COL_FX,
    COL_VIS,
    COL_SEL,
    COL_NAME,
    COL_COLOR,
    COL_HEIGHT,
    COL_ORDER,
    COL_ALL,
    COL_COUNT
};

// Mapping: SafeCol enum → TS_* bit(s)
static const int k_colBit[COL_COUNT] = {
    0,                           // COL_TRACK – no bit
    TS_VOL,
    TS_PAN,
    TS_MUTE,
    TS_SOLO,
    TS_PHASE,
    TS_FXPARAMS | TS_FXCHAIN,    // FX column covers both
    TS_VIS,
    TS_SELECTION,
    TS_TRACKNAME,
    TS_TRACKCOLOR,
    TS_TRACKHEIGHT,
    TS_TRACKORDER,
    0,                           // COL_ALL – handled specially
};

static const char* k_colName[COL_COUNT] = {
    "Track", "Vol", "Pan", "Mute", "Solo", "Phase", "FX", "Vis", "Sel",
    "Name", "Color", "Height", "Order", "All"
};
static const int k_colWidth[COL_COUNT] = {
    140, 32, 32, 36, 36, 40, 32, 32, 32,
    38, 40, 44, 40, 36
};

// Bitmask covering all safe-able parameters (used by COL_ALL toggle)
static const int k_allBits =
    TS_VOL | TS_PAN | TS_MUTE | TS_SOLO | TS_PHASE |
    TS_FXPARAMS | TS_FXCHAIN | TS_VIS | TS_SELECTION |
    TS_TRACKNAME | TS_TRACKCOLOR | TS_TRACKHEIGHT | TS_TRACKORDER | TS_LAYERS |
    TS_FXSLOTS;

// ---------------------------------------------------------------------------
// Per-track ListView columns: subset that omits Vis / Sel / Height / Order.
// List view column index → SafeCol mapping and back.
// ---------------------------------------------------------------------------
// The per-track list has these columns (in order):
//   0: Track, 1: Vol, 2: Pan, 3: Mute, 4: Solo, 5: Phase, 6: FX, 7: Name, 8: Color, 9: All
static const int k_ptColToSafeCol[] = {
    COL_TRACK, COL_VOL, COL_PAN, COL_MUTE, COL_SOLO, COL_PHASE, COL_FX,
    COL_NAME, COL_COLOR, COL_ALL
};
static const int k_ptColCount = (int)(sizeof(k_ptColToSafeCol) / sizeof(k_ptColToSafeCol[0]));

// Bitmask for COL_ALL in per-track mode (excludes Vis/Sel/Height/Order)
static const int k_ptAllBits =
    TS_VOL | TS_PAN | TS_MUTE | TS_SOLO | TS_PHASE |
    TS_FXPARAMS | TS_FXCHAIN | TS_TRACKNAME | TS_TRACKCOLOR;

// ---------------------------------------------------------------------------
// Row data
// ---------------------------------------------------------------------------
struct SafeRow {
    std::string label;
    GUID        guid;
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
    HWND hLayerList = nullptr;   // main window, Project tab only
    HWND hTabs      = nullptr;   // main window only

    std::vector<SafeRow> rows;
    SafesTarget          tgt;

    bool     isMain   = false;   // main window: tabs + layer table
    int      tab      = 0;       // 0 = Project, 1 = Subscenes
    SafeSet* sceneSet = nullptr; // popup only: the snapshot's own set
    bool     dirty    = false;   // popup only: did the user change anything
    HFONT    hBanner  = nullptr; // popup only: bold font for the banner

    // Drag-to-check state
    bool s_cbDragActive   = false;
    bool s_cbDragChecking = false;
    int  s_cbDragLastRow  = -1;
    int  s_cbDragLastCol  = -1;
    bool s_suppressClick  = false;
};

// The dockable window's pane. Created once at startup and only hidden on
// close, exactly like the dialog it belongs to.
static SafesPane  g_mainPane;
static HINSTANCE  g_hInst = nullptr;

static SafesPane* PaneOf(HWND hDlg)
{
    return (SafesPane*)GetWindowLongPtr(hDlg, DWLP_USER);
}

// Tab labels for the main window.
static const char* k_tabName[] = { "Project", "Subscenes" };
static const int   k_tabCount  = 2;

// ---------------------------------------------------------------------------
// Layer safes live in their own list (hLayerList) rather than as rows in the
// track grid: a layer is a single yes/no, not a set of track parameters, so it
// has nothing to say about any of that grid's columns. They are project-level
// only — a subscene inherits whatever the project set says.
// ---------------------------------------------------------------------------
static bool LayerRowSafed(int layerIdx)
{
    if (layerIdx < 0 || layerIdx >= kLayerSafeCount) return false;
    return (g_layerSafeMask & (1 << layerIdx)) != 0;
}

static void SetLayerRowSafed(int layerIdx, bool on)
{
    if (layerIdx < 0 || layerIdx >= kLayerSafeCount) return;
    if (on) g_layerSafeMask |=  (1 << layerIdx);
    else    g_layerSafeMask &= ~(1 << layerIdx);
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
    CheckDlgButton(hDlg, IDC_GSAFE_VIS,    (m & TS_VIS)         ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_GSAFE_NAME,   (m & TS_TRACKNAME)   ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_GSAFE_COLOR,  (m & TS_TRACKCOLOR)  ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_GSAFE_HEIGHT, (m & TS_TRACKHEIGHT) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_GSAFE_ORDER,  (m & TS_TRACKORDER)  ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_GSAFE_LAYERS, (m & TS_LAYERS)      ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_GSAFE_SLOTS,  (m & TS_FXSLOTS)     ? BST_CHECKED : BST_UNCHECKED);

    // Slot positions only exist on REAPER v7.75+; disable rather than hide
    // so the control keeps its place in the row on older builds.
    if (!LT_SlotHintsSupported())
        EnableWindow(GetDlgItem(hDlg, IDC_GSAFE_SLOTS), FALSE);

    // "All Tracks" checkbox - checked if every track row has all per-track bits
    // set. Only the dockable window has one.
    if (GetDlgItem(hDlg, IDC_GSAFE_ALL))
    {
        bool allSet = !p->rows.empty();
        for (int i = 0; allSet && i < (int)p->rows.size(); ++i)
            if ((GetRowMask(p, i) & k_ptAllBits) != k_ptAllBits) allSet = false;
        CheckDlgButton(hDlg, IDC_GSAFE_ALL, allSet ? BST_CHECKED : BST_UNCHECKED);
    }

    CheckDlgButton(hDlg, IDC_TRACK_SAFES_EN,
        *p->tgt.trackEn ? BST_CHECKED : BST_UNCHECKED);

    // Popup-only switches.
    if (p->sceneSet)
    {
        CheckDlgButton(hDlg, IDC_SCSAFE_ENABLE,
            p->sceneSet->enabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_SCSAFE_REPLACE,
            p->sceneSet->replaceGlobal ? BST_CHECKED : BST_UNCHECKED);
        EnableWindow(GetDlgItem(hDlg, IDC_SCSAFE_REPLACE), p->sceneSet->enabled);
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
// Layer safe list: one fixed row per slot.
//
// Slots by index rather than one row per existing layer: a scene recall
// replaces the whole layer set, so the slot number is the only reference that
// still means anything on the other side of one. Slots past the end of the
// current layer list are shown too, so a safe set now still applies to a layer
// created later.
// ---------------------------------------------------------------------------
static void PopulateLayerList(SafesPane* p)
{
    if (!p || !p->hLayerList) return;
    ListView_DeleteAllItems(p->hLayerList);

    const int layerCount = LayersEngine::Get().GetLayerCount();
    for (int i = 0; i < kLayerSafeCount; ++i)
    {
        char lbl[160];
        if (i < layerCount)
            snprintf(lbl, sizeof(lbl), "%d.  %s",
                     i + 1, LayersEngine::Get().GetLayer(i).name);
        else
            snprintf(lbl, sizeof(lbl), "%d.  (no layer)", i + 1);

        LVITEMA item = {};
        item.mask    = LVIF_TEXT;
        item.iItem   = i;
        item.pszText = lbl;
        ListView_InsertItem(p->hLayerList, &item);
        ListView_SetItemText(p->hLayerList, i, 1, (LPSTR)" ");
    }
}

// ---------------------------------------------------------------------------
// Rebuild rows from the current REAPER project
// ---------------------------------------------------------------------------
static void RebuildRows(SafesPane* p)
{
    if (!p) return;
    p->rows.clear();

    const int n = GetNumTracks();
    for (int i = 0; i < n; ++i)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;

        SafeRow r;

        char name[256] = {};
        if (!GetTrackName(tr, name, sizeof(name)) || name[0] == '\0')
            snprintf(name, sizeof(name), "Track %d", i + 1);
        r.label = name;

        GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
        r.guid = pg ? *pg : GUID{};

        p->rows.push_back(r);
    }
}

// ---------------------------------------------------------------------------
// Populate the ListView from the pane's rows
// ---------------------------------------------------------------------------
static void PopulateList(SafesPane* p)
{
    if (!p || !p->hList) return;
    ListView_DeleteAllItems(p->hList);

    for (int i = 0; i < (int)p->rows.size(); ++i)
    {
        LVITEMA item = {};
        item.mask    = LVIF_TEXT;
        item.iItem   = i;
        item.pszText = (LPSTR)p->rows[i].label.c_str();
        ListView_InsertItem(p->hList, &item);

        // Sub-items: we use the custom-draw to paint checkboxes, but we set
        // a placeholder space so the item has the right number of sub-items.
        for (int c = 1; c < k_ptColCount; ++c)
            ListView_SetItemText(p->hList, i, c, (LPSTR)" ");
    }
}

// ---------------------------------------------------------------------------
// Helper: given a ListView subitem index (in the per-track list),
// return the SafeCol enum value. Returns -1 for invalid.
// ---------------------------------------------------------------------------
static int PtLvcToSafeCol(int lvc)
{
    if (lvc < 0 || lvc >= k_ptColCount) return -1;
    return k_ptColToSafeCol[lvc];
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
// SafesHeaderSubclassProc – paints rotated column labels for the per-track list
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
        // Fill background
        FillRect(hdc, &rcClient, (HBRUSH)(COLOR_BTNFACE + 1));

        int itemCount = Header_GetItemCount(hHdr);

        // Create rotated font (escapement = 90°, counter-clockwise)
        LOGFONTA lf = {};
        GetObject(GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
        lf.lfEscapement  = 900;
        lf.lfOrientation = 900;
        HFONT hRotFont = CreateFontIndirectA(&lf);

        // Normal font for Track column (col 0)
        HFONT hNormFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));

        for (int i = 0; i < itemCount; ++i)
        {
            RECT rcItem;
            Header_GetItemRect(hHdr, i, &rcItem);

            // Draw separator line
            HPEN hPen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNSHADOW));
            HPEN hOld = (HPEN)SelectObject(hdc, hPen);
            MoveToEx(hdc, rcItem.right - 1, rcItem.top, nullptr);
            LineTo(hdc, rcItem.right - 1, rcItem.bottom);
            SelectObject(hdc, hOld);
            DeleteObject(hPen);

            // Get column label
            char text[64] = {};
            HDITEM hdi = {};
            hdi.mask      = HDI_TEXT;
            hdi.pszText   = text;
            hdi.cchTextMax = (int)sizeof(text) - 1;
            Header_GetItem(hHdr, i, &hdi);

            if (i == 0)
            {
                // Track column: horizontal text, vertically centred
                SelectObject(hdc, hNormFont);
                RECT rc = rcItem;
                rc.left += 4;
                DrawTextA(hdc, text, -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
            }
            else
            {
                // Other columns: rotated 90° text
                SelectObject(hdc, hRotFont);
                int cx = (rcItem.left + rcItem.right) / 2;
                // Draw from bottom of header upward (text ascends)
                TextOutA(hdc, cx + 5, rcItem.bottom - 3, text, (int)strlen(text));
            }
        }

        DeleteObject(hRotFont);
        EndPaint(hHdr, &ps);
        return 0;
    }

    // Set minimum header height to fit rotated labels (about 54px)
    if (msg == HDM_LAYOUT)
    {
        LRESULT r = DefSubclassProc(hHdr, msg, wParam, lParam);
        HDLAYOUT* phl = (HDLAYOUT*)lParam;
        if (phl && phl->prc && phl->pwpos)
        {
            const int kHeaderH = 54;
            phl->pwpos->cy = kHeaderH;
            phl->prc->top  = kHeaderH;
        }
        return r;
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
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        LVHITTESTINFO ht = {}; ht.pt = pt;
        ListView_SubItemHitTest(hList, &ht);
        const int row = ht.iItem;
        const int lvc = ht.iSubItem;
        const int sc  = PtLvcToSafeCol(lvc);
        if (row >= 0 && lvc > 0 && sc > 0)
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
        // Only process if we've moved to a new cell with a valid checkbox column
        if (row >= 0 && lvc > 0 && sc > 0 &&
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

    LVCOLUMNA col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    for (int lvc = 0; lvc < k_ptColCount; ++lvc)
    {
        int sc = k_ptColToSafeCol[lvc];
        col.pszText = (LPSTR)k_colName[sc];
        col.cx      = k_colWidth[sc];
        col.fmt     = (sc == COL_TRACK) ? LVCFMT_LEFT : LVCFMT_CENTER;
        ListView_InsertColumn(p->hList, lvc, &col);
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
static void LayoutGlobalRow(HWND hDlg, int x, int y, int w, int chkH, bool withAll)
{
    // Row 0: Vol  Pan   Mutes  Solo   Phase
    // Row 1: FX   Vis   Name   Color  Height
    // Row 2: Order Layers Slots (All Tracks)
    static const int k_gsIds[] = {
        IDC_GSAFE_VOL, IDC_GSAFE_PAN, IDC_GSAFE_MUTE, IDC_GSAFE_SOLO, IDC_GSAFE_PHASE,
        IDC_GSAFE_FX,  IDC_GSAFE_VIS, IDC_GSAFE_NAME, IDC_GSAFE_COLOR, IDC_GSAFE_HEIGHT,
        IDC_GSAFE_ORDER, IDC_GSAFE_LAYERS, IDC_GSAFE_SLOTS, IDC_GSAFE_ALL
    };
    static const int k_gsRow[] = { 0,0,0,0,0, 1,1,1,1,1, 2,2,2, 2 };
    static const int k_gsCol[] = { 0,1,2,3,4, 0,1,2,3,4, 0,1,2, 3 };
    const int count = withAll ? 14 : 13;
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
        { IDC_GSAFE_VIS,    TS_VIS },
        { IDC_GSAFE_NAME,   TS_TRACKNAME },
        { IDC_GSAFE_COLOR,  TS_TRACKCOLOR },
        { IDC_GSAFE_HEIGHT, TS_TRACKHEIGHT },
        { IDC_GSAFE_ORDER,  TS_TRACKORDER },
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
        if (row >= 0 && lvc > 0 && sc > 0)
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
        SetWindowLongPtr(p->hDlg, DWLP_MSGRESULT, CDRF_NOTIFYSUBITEMDRAW);
        return true;

    case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
    {
        const int row = (int)pcd->nmcd.dwItemSpec;
        const int lvc = (int)pcd->iSubItem;
        const int sc  = PtLvcToSafeCol(lvc);
        if (sc == COL_TRACK) return false; // let default draw track name
        if (sc < 0) return false;

        HDC   hdc  = pcd->nmcd.hdc;
        RECT  rcIt = pcd->nmcd.rc;

        const bool isSelected = (ListView_GetItemState(p->hList, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        COLORREF bg = isSelected ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_WINDOW);
        SetBkColor(hdc, bg);
        ExtTextOutA(hdc, 0, 0, ETO_OPAQUE, &rcIt, "", 0, nullptr);

        const int rowMask = GetRowMask(p, row);
        const bool checked = (sc == COL_ALL)
            ? (rowMask & k_ptAllBits) == k_ptAllBits
            : (rowMask & k_colBit[sc]) != 0;

        const int cbSize = 13;
        RECT rcCb;
        rcCb.left   = rcIt.left  + (rcIt.right  - rcIt.left  - cbSize) / 2;
        rcCb.top    = rcIt.top   + (rcIt.bottom - rcIt.top   - cbSize) / 2;
        rcCb.right  = rcCb.left  + cbSize;
        rcCb.bottom = rcCb.top   + cbSize;

        UINT dfcs = DFCS_BUTTONCHECK | DFCS_FLAT;
        if (checked) dfcs |= DFCS_CHECKED;
        DrawFrameControl(hdc, &rcCb, DFC_BUTTON, dfcs);

        SetWindowLongPtr(p->hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
        return true;
    }
    }
    return false;
}

// ---------------------------------------------------------------------------
// SwitchTab – repoint the main window's grid at the other safes set.
// ---------------------------------------------------------------------------
static void SwitchTab(SafesPane* p, int tab)
{
    if (!p || !p->isMain) return;
    if (tab < 0 || tab >= k_tabCount) tab = 0;
    p->tab = tab;
    p->tgt = (tab == 0) ? ProjectTarget() : SetTarget(g_subsceneSafes);

    SetDlgItemText(p->hDlg, IDC_GSAFES_GROUP,
        tab == 0 ? "Global Safes"
                 : "Subscene Global Safes  (added to the project safes on every subscene recall)");

    // Layers are project-level: a subscene has no layer set of its own, so the
    // table would be editing the same ten bits under a heading that implied
    // otherwise. Hide it and give the grid the space instead.
    const bool showLayers = (tab == 0);
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

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        p = &g_mainPane;
        SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)p);
        p->hDlg   = hDlg;
        p->isMain = true;
        p->tab    = 0;
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
            TabCtrl_SetCurSel(p->hTabs, 0);
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
        const int BTN_H = 24, BTN_W = 70;
        const int TAB_H = 22;   // tab strip
        const int GRP_H = 68;   // Global Safes groupbox (3 rows)
        const int CHK_H = 14;   // per-track enable checkbox row

        if (p->hTabs)
            SetWindowPos(p->hTabs, nullptr, MARGIN, MARGIN, W - MARGIN*2, TAB_H, SWP_NOZORDER);

        const int grpTop = MARGIN + TAB_H + 2;
        HWND hGrp = GetDlgItem(hDlg, IDC_GSAFES_GROUP);
        if (hGrp) SetWindowPos(hGrp, nullptr, MARGIN, grpTop, W - MARGIN*2, GRP_H, SWP_NOZORDER);

        LayoutGlobalRow(hDlg, MARGIN + 7, grpTop + 13, W - MARGIN*2 - 14, CHK_H, true);

        // Per-track enable checkbox
        const int trackEnY = grpTop + GRP_H + MARGIN;
        HWND hTrackEn = GetDlgItem(hDlg, IDC_TRACK_SAFES_EN);
        if (hTrackEn) SetWindowPos(hTrackEn, nullptr, MARGIN, trackEnY, 160, CHK_H, SWP_NOZORDER);

        // The layer table keeps a fixed height at the bottom — it holds a
        // known ten rows — and the track list absorbs everything left over.
        // On the Subscenes tab the table is hidden and the grid takes its room.
        const bool showLayers = (p->tab == 0);
        const int LBL_H  = showLayers ? 12  : 0;
        const int LYR_H  = showLayers ? 118 : 0;

        const int listTop = trackEnY + CHK_H + MARGIN;
        const int by      = H - BTN_H - MARGIN;
        const int lyrTop  = by - MARGIN - LYR_H;
        const int lblTop  = lyrTop - LBL_H;
        int listBottom    = lblTop - MARGIN;
        if (listBottom < listTop + 40) listBottom = listTop + 40;

        SetWindowPos(p->hList, nullptr,
            MARGIN, listTop, W - MARGIN*2, listBottom - listTop, SWP_NOZORDER);

        if (showLayers)
        {
            HWND hLyrLbl = GetDlgItem(hDlg, IDC_SAFESLAYERLBL);
            if (hLyrLbl) SetWindowPos(hLyrLbl, nullptr, MARGIN, lblTop, W - MARGIN*2, 12, SWP_NOZORDER);
            if (p->hLayerList) SetWindowPos(p->hLayerList, nullptr, MARGIN, lyrTop, W - MARGIN*2, 118, SWP_NOZORDER);
        }

        HWND hRefresh = GetDlgItem(hDlg, IDC_REFRESH_SAFES);
        HWND hClear   = GetDlgItem(hDlg, IDC_CLEAR_SAFES);
        if (hRefresh) SetWindowPos(hRefresh, nullptr, MARGIN,              by, BTN_W, BTN_H, SWP_NOZORDER);
        if (hClear)   SetWindowPos(hClear,   nullptr, MARGIN + BTN_W + MARGIN, by, BTN_W, BTN_H, SWP_NOZORDER);
        break;
    }

    case WM_COMMAND:
    {
        if (!p) break;
        const int id = LOWORD(wParam);

        if (HandleGlobalToggle(p, id)) break;

        switch (id)
        {
        case IDC_REFRESH_SAFES:
            RebuildRows(p);
            PopulateList(p);
            PopulateLayerList(p);
            SyncGlobalCheckboxes(p);
            break;

        case IDC_CLEAR_SAFES:
            *p->tgt.mask = 0;
            p->tgt.tracks->clear();
            // Layer safes belong to the project tab only.
            if (p->tab == 0) g_layerSafeMask = 0;
            if (p->hLayerList) InvalidateRect(p->hLayerList, nullptr, FALSE);
            SyncGlobalCheckboxes(p);
            if (p->hList) InvalidateRect(p->hList, nullptr, FALSE);
            NoteChange(p);
            break;

        case IDC_GSAFE_ALL:
            if (IsDlgButtonChecked(hDlg, IDC_GSAFE_ALL) == BST_CHECKED)
            {
                for (int i = 0; i < (int)p->rows.size(); ++i)
                    SetRowMask(p, i, GetRowMask(p, i) | k_ptAllBits);
            }
            else
            {
                p->tgt.tracks->clear();
            }
            if (p->hList) InvalidateRect(p->hList, nullptr, FALSE);
            NoteChange(p);
            break;

        case IDC_TRACK_SAFES_EN:
            *p->tgt.trackEn = (IsDlgButtonChecked(hDlg, IDC_TRACK_SAFES_EN) == BST_CHECKED);
            if (p->hList) InvalidateRect(p->hList, nullptr, FALSE);
            NoteChange(p);
            break;
        }
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
            if (pnm->code == NM_CLICK)
            {
                NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
                LVHITTESTINFO ht = {};
                ht.pt = nia->ptAction;
                ListView_SubItemHitTest(p->hLayerList, &ht);
                if (ht.iItem >= 0 && ht.iSubItem == 1)
                {
                    SetLayerRowSafed(ht.iItem, !LayerRowSafed(ht.iItem));
                    RECT rcRow;
                    ListView_GetItemRect(p->hLayerList, ht.iItem, &rcRow, LVIR_BOUNDS);
                    InvalidateRect(p->hLayerList, &rcRow, FALSE);
                    MarkProjectDirty(nullptr);
                }
            }
            else if (pnm->code == NM_CUSTOMDRAW)
            {
                NMLVCUSTOMDRAW* pcd = (NMLVCUSTOMDRAW*)lParam;
                switch (pcd->nmcd.dwDrawStage)
                {
                case CDDS_PREPAINT:
                    SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                    return TRUE;
                case CDDS_ITEMPREPAINT:
                    SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYSUBITEMDRAW);
                    return TRUE;
                case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
                {
                    if (pcd->iSubItem != 1) break;   // column 0 draws normally

                    const int  row = (int)pcd->nmcd.dwItemSpec;
                    HDC        hdc = pcd->nmcd.hdc;
                    RECT       rcIt = pcd->nmcd.rc;
                    const bool sel = (ListView_GetItemState(p->hLayerList, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;

                    SetBkColor(hdc, sel ? GetSysColor(COLOR_HIGHLIGHT)
                                        : GetSysColor(COLOR_WINDOW));
                    ExtTextOutA(hdc, 0, 0, ETO_OPAQUE, &rcIt, "", 0, nullptr);

                    const int cbSize = 13;
                    RECT rcCb;
                    rcCb.left   = rcIt.left + (rcIt.right  - rcIt.left - cbSize) / 2;
                    rcCb.top    = rcIt.top  + (rcIt.bottom - rcIt.top  - cbSize) / 2;
                    rcCb.right  = rcCb.left + cbSize;
                    rcCb.bottom = rcCb.top  + cbSize;

                    UINT dfcs = DFCS_BUTTONCHECK | DFCS_FLAT;
                    if (LayerRowSafed(row)) dfcs |= DFCS_CHECKED;
                    DrawFrameControl(hdc, &rcCb, DFC_BUTTON, dfcs);

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

    case WM_CLOSE:
        ShowWindow(hDlg, SW_HIDE);
        return TRUE;
    }

    return FALSE;
}

// ---------------------------------------------------------------------------
// SceneSafesDlgProc – modal IDD_SCENE_SAFES popup.
//
// Same grid, no tabs and no layer table, plus the banner and the two switches
// that decide how this set combines with the project safes.
// ---------------------------------------------------------------------------
struct SceneSafesInit {
    SafeSet*    set;
    const char* title;
};

static INT_PTR CALLBACK SceneSafesDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    SafesPane* p = PaneOf(hDlg);

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
        const int BTN_H = 24, BTN_W = 70;
        const int TITLE_H = 32;
        const int GRP_H = 68;
        const int CHK_H = 14;

        HWND hTitle = GetDlgItem(hDlg, IDC_SCSAFE_TITLE);
        if (hTitle) SetWindowPos(hTitle, nullptr, MARGIN, MARGIN, W - MARGIN*2, TITLE_H, SWP_NOZORDER);

        const int swY = MARGIN + TITLE_H + 2;
        HWND hEn = GetDlgItem(hDlg, IDC_SCSAFE_ENABLE);
        HWND hRp = GetDlgItem(hDlg, IDC_SCSAFE_REPLACE);
        if (hEn) SetWindowPos(hEn, nullptr, MARGIN + 3, swY, W / 2 - 20, CHK_H, SWP_NOZORDER);
        if (hRp) SetWindowPos(hRp, nullptr, W / 2,      swY, W / 2 - MARGIN, CHK_H, SWP_NOZORDER);

        const int grpTop = swY + CHK_H + 4;
        HWND hGrp = GetDlgItem(hDlg, IDC_GSAFES_GROUP);
        if (hGrp) SetWindowPos(hGrp, nullptr, MARGIN, grpTop, W - MARGIN*2, GRP_H, SWP_NOZORDER);

        LayoutGlobalRow(hDlg, MARGIN + 7, grpTop + 13, W - MARGIN*2 - 14, CHK_H, false);

        const int trackEnY = grpTop + GRP_H + MARGIN;
        HWND hTrackEn = GetDlgItem(hDlg, IDC_TRACK_SAFES_EN);
        if (hTrackEn) SetWindowPos(hTrackEn, nullptr, MARGIN, trackEnY, 160, CHK_H, SWP_NOZORDER);

        const int listTop = trackEnY + CHK_H + MARGIN;
        const int by      = H - BTN_H - MARGIN;
        int listBottom    = by - MARGIN;
        if (listBottom < listTop + 40) listBottom = listTop + 40;

        SetWindowPos(p->hList, nullptr,
            MARGIN, listTop, W - MARGIN*2, listBottom - listTop, SWP_NOZORDER);

        HWND hRefresh = GetDlgItem(hDlg, IDC_REFRESH_SAFES);
        HWND hClear   = GetDlgItem(hDlg, IDC_CLEAR_SAFES);
        HWND hClose   = GetDlgItem(hDlg, IDC_SCSAFE_CLOSE);
        if (hRefresh) SetWindowPos(hRefresh, nullptr, MARGIN,                   by, BTN_W, BTN_H, SWP_NOZORDER);
        if (hClear)   SetWindowPos(hClear,   nullptr, MARGIN + BTN_W + MARGIN,  by, BTN_W, BTN_H, SWP_NOZORDER);
        if (hClose)   SetWindowPos(hClose,   nullptr, W - MARGIN - BTN_W,       by, BTN_W, BTN_H, SWP_NOZORDER);
        break;
    }

    case WM_COMMAND:
    {
        if (!p) break;
        const int id = LOWORD(wParam);

        if (HandleGlobalToggle(p, id)) break;

        switch (id)
        {
        case IDC_SCSAFE_ENABLE:
            p->sceneSet->enabled =
                (IsDlgButtonChecked(hDlg, IDC_SCSAFE_ENABLE) == BST_CHECKED);
            EnableWindow(GetDlgItem(hDlg, IDC_SCSAFE_REPLACE), p->sceneSet->enabled);
            NoteChange(p);
            break;

        case IDC_SCSAFE_REPLACE:
            p->sceneSet->replaceGlobal =
                (IsDlgButtonChecked(hDlg, IDC_SCSAFE_REPLACE) == BST_CHECKED);
            NoteChange(p);
            break;

        case IDC_REFRESH_SAFES:
            RebuildRows(p);
            PopulateList(p);
            SyncGlobalCheckboxes(p);
            break;

        case IDC_CLEAR_SAFES:
            *p->tgt.mask = 0;
            p->tgt.tracks->clear();
            SyncGlobalCheckboxes(p);
            if (p->hList) InvalidateRect(p->hList, nullptr, FALSE);
            NoteChange(p);
            break;

        case IDC_TRACK_SAFES_EN:
            *p->tgt.trackEn = (IsDlgButtonChecked(hDlg, IDC_TRACK_SAFES_EN) == BST_CHECKED);
            if (p->hList) InvalidateRect(p->hList, nullptr, FALSE);
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
    SceneSafesInit init { &set, title ? title : "Scene Safes" };
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
    if (g_mainPane.hDlg) SetWindowTextA(g_mainPane.hDlg, "Live Tools - Safes");
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

void SafesWnd_ShowSubsceneTab()
{
    HWND hDlg = g_mainPane.hDlg;
    if (!hDlg) return;
    SafesWnd_Refresh();
    if (g_mainPane.hTabs)
    {
        TabCtrl_SetCurSel(g_mainPane.hTabs, 1);
        SwitchTab(&g_mainPane, 1);
    }
    ShowWindow(hDlg, SW_SHOW);
    SetForegroundWindow(hDlg);
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
    if (sscanf(line, "LTSAFETRACKSEN %d", &val) == 1)
        { g_trackSafesEnabled = (val != 0); SyncDlgFromState(); return true; }

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
        { g_subsceneSafes.trackSafesEnabled = (val != 0); SyncDlgFromState(); return true; }

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
