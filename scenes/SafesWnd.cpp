#include "SafesWnd.h"
#include "TransitionEngine.h"    // g_globalSafeMask, g_trackSafes, GetEffectiveSafeMask
#include "TransitionSnapshot.h"  // TS_* bit flags
#include "api.h"                 // GetNumTracks, GetTrack, GetSetMediaTrackInfo, etc.
#include "resource.h"

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
    TS_TRACKNAME | TS_TRACKCOLOR | TS_TRACKHEIGHT | TS_TRACKORDER | TS_LAYERS;

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
// Row data (row 0 = Global)
// ---------------------------------------------------------------------------
struct SafeRow {
    std::string label;
    GUID        guid;     // zero for Global row
    bool        isGlobal;
};

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------
static HINSTANCE   g_hInst       = nullptr;
static HWND        g_hDlg        = nullptr;
static HWND        g_hList       = nullptr;
static std::vector<SafeRow> g_rows;

// Drag-to-check state (used by SafesListSubclassProc)
static bool s_cbDragActive   = false;
static bool s_cbDragChecking = false;   // true = checking, false = unchecking
static int  s_cbDragLastRow  = -1;
static int  s_cbDragLastCol  = -1;
static bool s_suppressClick  = false;   // suppress NM_CLICK while dragging

// ---------------------------------------------------------------------------
// Row helpers
// ---------------------------------------------------------------------------
static int GetRowMask(int row)
{
    if (row < 0 || row >= (int)g_rows.size()) return 0;
    if (g_rows[row].isGlobal) return g_globalSafeMask;
    for (const auto& e : g_trackSafes)
        if (IsEqualGUID(e.guid, g_rows[row].guid)) return e.mask;
    return 0;
}

static void SetRowMask(int row, int mask)
{
    if (row < 0 || row >= (int)g_rows.size()) return;
    if (g_rows[row].isGlobal) {
        g_globalSafeMask = mask;
        return;
    }
    const GUID& guid = g_rows[row].guid;
    for (auto& e : g_trackSafes) {
        if (IsEqualGUID(e.guid, guid)) { e.mask = mask; return; }
    }
    g_trackSafes.push_back({ guid, mask });
}

static void ToggleBit(int row, int bit)
{
    int m = GetRowMask(row);
    SetRowMask(row, m ^ bit);
}

// ---------------------------------------------------------------------------
// Rebuild g_rows from the current REAPER project
// ---------------------------------------------------------------------------
static void RebuildRows()
{
    g_rows.clear();

    // One row per track (Global safes are now shown as individual checkboxes)
    const int n = GetNumTracks();
    for (int i = 0; i < n; ++i)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;

        SafeRow r;
        r.isGlobal = false;

        // Track name
        char name[256] = {};
        if (!GetTrackName(tr, name, sizeof(name)) || name[0] == '\0')
            snprintf(name, sizeof(name), "Track %d", i + 1);
        r.label = name;

        // GUID
        GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
        r.guid = pg ? *pg : GUID{};

        g_rows.push_back(r);
    }
}

// ---------------------------------------------------------------------------
// Populate the ListView from g_rows
// ---------------------------------------------------------------------------
static void PopulateList()
{
    if (!g_hList) return;
    ListView_DeleteAllItems(g_hList);

    for (int i = 0; i < (int)g_rows.size(); ++i)
    {
        LVITEMA item = {};
        item.mask    = LVIF_TEXT;
        item.iItem   = i;
        item.pszText = (LPSTR)g_rows[i].label.c_str();
        ListView_InsertItem(g_hList, &item);

        // Sub-items: we use the custom-draw to paint checkboxes, but we set
        // a placeholder space so the item has the right number of sub-items.
        for (int c = 1; c < k_ptColCount; ++c)
            ListView_SetItemText(g_hList, i, c, (LPSTR)" ");
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
static void ApplyCellToggle(int row, int sc, bool checking)
{
    if (row < 0 || row >= (int)g_rows.size()) return;
    int bit = k_colBit[sc];
    if (sc == COL_ALL)
    {
        // Toggle all per-track bits
        int m = GetRowMask(row);
        SetRowMask(row, checking ? (m | k_ptAllBits) : (m & ~k_ptAllBits));
    }
    else if (bit)
    {
        int m = GetRowMask(row);
        SetRowMask(row, checking ? (m | bit) : (m & ~bit));
    }
    RECT rcRow;
    ListView_GetItemRect(g_hList, row, &rcRow, LVIR_BOUNDS);
    InvalidateRect(g_hList, &rcRow, FALSE);
    MarkProjectDirty(nullptr);
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
                                               ULONG_PTR /*uId*/, DWORD_PTR /*dwRef*/)
{
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
                isChecked = ((GetRowMask(row) & k_ptAllBits) == k_ptAllBits);
            else
                isChecked = bit ? ((GetRowMask(row) & bit) != 0) : false;

            s_cbDragChecking = !isChecked;
            s_cbDragActive   = true;
            s_cbDragLastRow  = row;
            s_cbDragLastCol  = lvc;
            s_suppressClick  = false;

            ApplyCellToggle(row, sc, s_cbDragChecking);
            SetCapture(hList);
            // Don't let default handler process this click further
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE:
    {
        if (!s_cbDragActive || !(wParam & MK_LBUTTON)) break;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        LVHITTESTINFO ht = {}; ht.pt = pt;
        ListView_SubItemHitTest(hList, &ht);
        const int row = ht.iItem;
        const int lvc = ht.iSubItem;
        const int sc  = PtLvcToSafeCol(lvc);
        // Only process if we've moved to a new cell with a valid checkbox column
        if (row >= 0 && lvc > 0 && sc > 0 &&
            (row != s_cbDragLastRow || lvc != s_cbDragLastCol))
        {
            s_cbDragLastRow = row;
            s_cbDragLastCol = lvc;
            s_suppressClick = true;
            ApplyCellToggle(row, sc, s_cbDragChecking);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (s_cbDragActive)
        {
            s_cbDragActive  = false;
            s_suppressClick = false;
            ReleaseCapture();
            return 0;
        }
        break;

    case WM_CAPTURECHANGED:
        if (s_cbDragActive)
        {
            s_cbDragActive  = false;
            s_suppressClick = false;
        }
        break;
    }

    return DefSubclassProc(hList, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Dialog procedure
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK SafesDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        // Create the ListView dynamically (same pattern as TransitionWnd)
        INITCOMMONCONTROLSEX icx = { sizeof(icx), ICC_LISTVIEW_CLASSES };
        InitCommonControlsEx(&icx);

        // Get placeholder rect
        HWND hPlaceholder = GetDlgItem(hDlg, IDC_SAFESLIST);
        RECT rc = {};
        GetClientRect(hPlaceholder, &rc);
        MapWindowPoints(hPlaceholder, hDlg, (POINT*)&rc, 2);
        DestroyWindow(hPlaceholder);

        g_hList = CreateWindowExA(
            WS_EX_CLIENTEDGE,
            WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
            hDlg, (HMENU)(UINT_PTR)IDC_SAFESLIST, g_hInst, nullptr);

        // Full-row select + grid lines + double-buffer
        ListView_SetExtendedListViewStyle(g_hList,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        // Add per-track columns (subset: no Vis/Sel/Height/Order)
        LVCOLUMNA col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        for (int lvc = 0; lvc < k_ptColCount; ++lvc)
        {
            int sc = k_ptColToSafeCol[lvc];
            col.pszText = (LPSTR)k_colName[sc];
            col.cx      = k_colWidth[sc];
            col.fmt     = (sc == COL_TRACK) ? LVCFMT_LEFT : LVCFMT_CENTER;
            ListView_InsertColumn(g_hList, lvc, &col);
        }

        // Build rows and populate
        RebuildRows();
        PopulateList();

        // Subclass the ListView header for rotated column labels
        HWND hHdr = ListView_GetHeader(g_hList);
        if (hHdr)
            SetWindowSubclass(hHdr, SafesHeaderSubclassProc, 1, 0);

        // Subclass the ListView itself for drag-to-check
        SetWindowSubclass(g_hList, SafesListSubclassProc, 2, 0);

        // Initialize global safe param checkboxes from g_globalSafeMask
        CheckDlgButton(hDlg, IDC_GSAFE_VOL,    (g_globalSafeMask & TS_VOL)   ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_GSAFE_PAN,    (g_globalSafeMask & TS_PAN)   ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_GSAFE_MUTE,   (g_globalSafeMask & TS_MUTE)  ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_GSAFE_SOLO,   (g_globalSafeMask & TS_SOLO)  ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_GSAFE_PHASE,  (g_globalSafeMask & TS_PHASE) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_GSAFE_FX,     (g_globalSafeMask & (TS_FXPARAMS|TS_FXCHAIN)) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_GSAFE_VIS,    (g_globalSafeMask & TS_VIS)         ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_GSAFE_NAME,   (g_globalSafeMask & TS_TRACKNAME)   ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_GSAFE_COLOR,  (g_globalSafeMask & TS_TRACKCOLOR)  ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_GSAFE_HEIGHT, (g_globalSafeMask & TS_TRACKHEIGHT) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_GSAFE_ORDER,  (g_globalSafeMask & TS_TRACKORDER)  ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_GSAFE_LAYERS, (g_globalSafeMask & TS_LAYERS) ? BST_CHECKED : BST_UNCHECKED);

        // "All Tracks" checkbox – checked if every track row has all per-track bits set
        {
            bool allSet = !g_rows.empty();
            for (int i = 0; allSet && i < (int)g_rows.size(); ++i)
                if ((GetRowMask(i) & k_ptAllBits) != k_ptAllBits) allSet = false;
            CheckDlgButton(hDlg, IDC_GSAFE_ALL, allSet ? BST_CHECKED : BST_UNCHECKED);
        }

        // Per-track enable toggle
        CheckDlgButton(hDlg, IDC_TRACK_SAFES_EN,
            g_trackSafesEnabled ? BST_CHECKED : BST_UNCHECKED);

        return TRUE;
    }

    case WM_SIZE:
    {
        if (!g_hList) break;
        RECT rcDlg;
        GetClientRect(hDlg, &rcDlg);
        const int W = rcDlg.right;
        const int H = rcDlg.bottom;
        const int MARGIN = 5;
        const int BTN_H = 24, BTN_W = 70;
        const int GRP_H = 68;   // Global Safes groupbox (3 rows)
        const int CHK_H = 14;   // per-track enable checkbox row

        // Global Safes groupbox spans full width
        HWND hGrp = GetDlgItem(hDlg, IDC_GSAFES_GROUP);
        if (hGrp) SetWindowPos(hGrp, nullptr, MARGIN, MARGIN, W - MARGIN*2, GRP_H, SWP_NOZORDER);

        // 5+5+3 param checkboxes spread evenly inside groupbox (3 rows of 5)
        // Row 0: Vol  Pan   Mutes  Solo   Phase
        // Row 1: FX   Vis   Name   Color  Height
        // Row 2: Order Layers All Tracks
        static const int k_gsIds[] = {
            IDC_GSAFE_VOL, IDC_GSAFE_PAN, IDC_GSAFE_MUTE, IDC_GSAFE_SOLO, IDC_GSAFE_PHASE,
            IDC_GSAFE_FX,  IDC_GSAFE_VIS, IDC_GSAFE_NAME, IDC_GSAFE_COLOR, IDC_GSAFE_HEIGHT,
            IDC_GSAFE_ORDER, IDC_GSAFE_LAYERS, IDC_GSAFE_ALL
        };
        static const int k_gsRow[] = { 0,0,0,0,0, 1,1,1,1,1, 2,2,2 };
        static const int k_gsCol[] = { 0,1,2,3,4, 0,1,2,3,4, 0,1,2 };
        const int grpInner = W - MARGIN*2 - 14;
        const int slot5 = grpInner / 5;
        for (int i = 0; i < 13; ++i) {
            HWND h = GetDlgItem(hDlg, k_gsIds[i]);
            if (!h) continue;
            int col = k_gsCol[i];
            int row = k_gsRow[i];
            SetWindowPos(h, nullptr,
                MARGIN + 7 + col * slot5,
                MARGIN + 18 + row * 16,
                slot5 - 2, CHK_H, SWP_NOZORDER);
        }

        // Per-track enable checkbox
        const int trackEnY = MARGIN + GRP_H + MARGIN;
        HWND hTrackEn = GetDlgItem(hDlg, IDC_TRACK_SAFES_EN);
        if (hTrackEn) SetWindowPos(hTrackEn, nullptr, MARGIN, trackEnY, 160, CHK_H, SWP_NOZORDER);

        // ListView
        const int listTop    = trackEnY + CHK_H + MARGIN;
        const int listBottom = H - BTN_H - MARGIN * 2;
        SetWindowPos(g_hList, nullptr,
            MARGIN, listTop, W - MARGIN*2, listBottom - listTop, SWP_NOZORDER);

        // Bottom buttons
        const int by = listBottom + MARGIN;
        HWND hRefresh = GetDlgItem(hDlg, IDC_REFRESH_SAFES);
        HWND hClear   = GetDlgItem(hDlg, IDC_CLEAR_SAFES);
        if (hRefresh) SetWindowPos(hRefresh, nullptr, MARGIN,              by, BTN_W, BTN_H, SWP_NOZORDER);
        if (hClear)   SetWindowPos(hClear,   nullptr, MARGIN + BTN_W + MARGIN, by, BTN_W, BTN_H, SWP_NOZORDER);
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_REFRESH_SAFES:
            RebuildRows();
            PopulateList();
            break;

        case IDC_CLEAR_SAFES:
            g_globalSafeMask = 0;
            g_trackSafes.clear();
            CheckDlgButton(hDlg, IDC_GSAFE_VOL,    BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_GSAFE_PAN,    BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_GSAFE_MUTE,   BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_GSAFE_SOLO,   BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_GSAFE_PHASE,  BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_GSAFE_FX,     BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_GSAFE_VIS,    BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_GSAFE_NAME,   BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_GSAFE_COLOR,  BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_GSAFE_HEIGHT, BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_GSAFE_ORDER,  BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_GSAFE_LAYERS, BST_UNCHECKED);
            CheckDlgButton(hDlg, IDC_GSAFE_ALL,    BST_UNCHECKED);
            if (g_hList) InvalidateRect(g_hList, nullptr, FALSE);
            MarkProjectDirty(nullptr);
            break;

        // Global safe per-parameter toggles
        case IDC_GSAFE_VOL:    if (IsDlgButtonChecked(hDlg, IDC_GSAFE_VOL)    == BST_CHECKED) g_globalSafeMask |= TS_VOL;    else g_globalSafeMask &= ~TS_VOL;    MarkProjectDirty(nullptr); break;
        case IDC_GSAFE_PAN:    if (IsDlgButtonChecked(hDlg, IDC_GSAFE_PAN)    == BST_CHECKED) g_globalSafeMask |= TS_PAN;    else g_globalSafeMask &= ~TS_PAN;    MarkProjectDirty(nullptr); break;
        case IDC_GSAFE_MUTE:   if (IsDlgButtonChecked(hDlg, IDC_GSAFE_MUTE)   == BST_CHECKED) g_globalSafeMask |= TS_MUTE;   else g_globalSafeMask &= ~TS_MUTE;   MarkProjectDirty(nullptr); break;
        case IDC_GSAFE_SOLO:   if (IsDlgButtonChecked(hDlg, IDC_GSAFE_SOLO)   == BST_CHECKED) g_globalSafeMask |= TS_SOLO;   else g_globalSafeMask &= ~TS_SOLO;   MarkProjectDirty(nullptr); break;
        case IDC_GSAFE_PHASE:  if (IsDlgButtonChecked(hDlg, IDC_GSAFE_PHASE)  == BST_CHECKED) g_globalSafeMask |= TS_PHASE;  else g_globalSafeMask &= ~TS_PHASE;  MarkProjectDirty(nullptr); break;
        case IDC_GSAFE_FX:     if (IsDlgButtonChecked(hDlg, IDC_GSAFE_FX)     == BST_CHECKED) g_globalSafeMask |= (TS_FXPARAMS|TS_FXCHAIN); else g_globalSafeMask &= ~(TS_FXPARAMS|TS_FXCHAIN); MarkProjectDirty(nullptr); break;
        case IDC_GSAFE_VIS:    if (IsDlgButtonChecked(hDlg, IDC_GSAFE_VIS)    == BST_CHECKED) g_globalSafeMask |= TS_VIS;    else g_globalSafeMask &= ~TS_VIS;    MarkProjectDirty(nullptr); break;
        case IDC_GSAFE_NAME:   if (IsDlgButtonChecked(hDlg, IDC_GSAFE_NAME)   == BST_CHECKED) g_globalSafeMask |= TS_TRACKNAME;   else g_globalSafeMask &= ~TS_TRACKNAME;   MarkProjectDirty(nullptr); break;
        case IDC_GSAFE_COLOR:  if (IsDlgButtonChecked(hDlg, IDC_GSAFE_COLOR)  == BST_CHECKED) g_globalSafeMask |= TS_TRACKCOLOR;  else g_globalSafeMask &= ~TS_TRACKCOLOR;  MarkProjectDirty(nullptr); break;
        case IDC_GSAFE_HEIGHT: if (IsDlgButtonChecked(hDlg, IDC_GSAFE_HEIGHT) == BST_CHECKED) g_globalSafeMask |= TS_TRACKHEIGHT; else g_globalSafeMask &= ~TS_TRACKHEIGHT; MarkProjectDirty(nullptr); break;
        case IDC_GSAFE_ORDER:  if (IsDlgButtonChecked(hDlg, IDC_GSAFE_ORDER)  == BST_CHECKED) g_globalSafeMask |= TS_TRACKORDER;  else g_globalSafeMask &= ~TS_TRACKORDER;  MarkProjectDirty(nullptr); break;
        case IDC_GSAFE_LAYERS: if (IsDlgButtonChecked(hDlg, IDC_GSAFE_LAYERS) == BST_CHECKED) g_globalSafeMask |= TS_LAYERS;      else g_globalSafeMask &= ~TS_LAYERS;      MarkProjectDirty(nullptr); break;

        case IDC_GSAFE_ALL:
            if (IsDlgButtonChecked(hDlg, IDC_GSAFE_ALL) == BST_CHECKED)
            {
                for (int i = 0; i < (int)g_rows.size(); ++i)
                    SetRowMask(i, GetRowMask(i) | k_ptAllBits);
            }
            else
            {
                g_trackSafes.clear();
            }
            if (g_hList) InvalidateRect(g_hList, nullptr, FALSE);
            MarkProjectDirty(nullptr);
            break;

        case IDC_TRACK_SAFES_EN:
            g_trackSafesEnabled = (IsDlgButtonChecked(hDlg, IDC_TRACK_SAFES_EN) == BST_CHECKED);
            if (g_hList) InvalidateRect(g_hList, nullptr, FALSE);
            MarkProjectDirty(nullptr);
            break;
        }
        break;

    case WM_NOTIFY:
    {
        NMHDR* pnm = (NMHDR*)lParam;
        if (pnm->hwndFrom != g_hList) break;

        if (pnm->code == NM_CLICK)
        {
            // Suppress if drag-to-check already handled this
            if (s_suppressClick) { s_suppressClick = false; break; }

            // Hit-test to find row + ListView column
            NMITEMACTIVATE* pnia = (NMITEMACTIVATE*)lParam;
            LVHITTESTINFO ht = {};
            ht.pt = pnia->ptAction;
            ListView_SubItemHitTest(g_hList, &ht);
            const int row = ht.iItem;
            const int lvc = ht.iSubItem;   // ListView column index
            const int sc  = PtLvcToSafeCol(lvc);  // SafeCol
            if (row >= 0 && lvc > 0 && sc > 0)
            {
                if (sc == COL_ALL)
                {
                    const int m = GetRowMask(row);
                    bool wasAllSet = ((m & k_ptAllBits) == k_ptAllBits);
                    SetRowMask(row, wasAllSet ? (m & ~k_ptAllBits) : (m | k_ptAllBits));
                }
                else
                {
                    ToggleBit(row, k_colBit[sc]);
                }
                // Repaint just the clicked row
                RECT rcRow;
                ListView_GetItemRect(g_hList, row, &rcRow, LVIR_BOUNDS);
                InvalidateRect(g_hList, &rcRow, FALSE);
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
                const int row = (int)pcd->nmcd.dwItemSpec;
                const int lvc = (int)pcd->iSubItem;   // ListView column index
                const int sc  = PtLvcToSafeCol(lvc);  // SafeCol
                if (sc == COL_TRACK) break; // let default draw track name
                if (sc < 0) break;

                HDC   hdc  = pcd->nmcd.hdc;
                RECT  rcIt = pcd->nmcd.rc;

                // Fill background
                const bool isSelected = (ListView_GetItemState(g_hList, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                COLORREF bg = isSelected ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_WINDOW);
                SetBkColor(hdc, bg);
                ExtTextOutA(hdc, 0, 0, ETO_OPAQUE, &rcIt, "", 0, nullptr);

                // Draw checkbox centred in cell
                const int rowMask = GetRowMask(row);
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

                SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                return TRUE;
            }
            }
        }
        break;
    }

    case WM_CLOSE:
        ShowWindow(hDlg, SW_HIDE);
        return TRUE;
    }

    return FALSE;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void SafesWnd_Init(HINSTANCE hInstance)
{
    g_hInst = hInstance;
    g_hDlg  = CreateDialogParamA(hInstance, MAKEINTRESOURCEA(IDD_SAFES),
                                  nullptr, SafesDlgProc, 0);
    if (g_hDlg) SetWindowTextA(g_hDlg, "Live Tools - Safes");
}

void SafesWnd_Cleanup()
{
    if (g_hDlg) { DestroyWindow(g_hDlg); g_hDlg = nullptr; }
    g_hList = nullptr;
}

void SafesWnd_ShowHide()
{
    if (!g_hDlg) return;
    if (IsWindowVisible(g_hDlg))
        ShowWindow(g_hDlg, SW_HIDE);
    else {
        SafesWnd_Refresh();
        ShowWindow(g_hDlg, SW_SHOW);
        SetForegroundWindow(g_hDlg);
    }
}

bool SafesWnd_IsVisible()
{
    return g_hDlg && IsWindowVisible(g_hDlg);
}

void SafesWnd_Refresh()
{
    if (!g_hDlg) return;
    RebuildRows();
    if (g_hList) PopulateList();
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
void SafesWnd_ResetForProject()
{
    g_globalSafeMask    = 0;
    g_trackSafesEnabled = true;
    g_trackSafes.clear();
}

bool SafesWnd_ProcessLine(const char* line)
{
    if (!line) return false;
    while (*line == ' ' || *line == '\t') ++line;

    int val = 0;
    if (sscanf(line, "LTSAFEGLOBAL %d", &val) == 1)
        { g_globalSafeMask = val; return true; }
    if (sscanf(line, "LTSAFETRACKSEN %d", &val) == 1)
        { g_trackSafesEnabled = (val != 0); return true; }

    char sguid[80] = {};
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
    ctx->AddLine("LTSAFETRACKSEN %d", g_trackSafesEnabled ? 1 : 0);
    for (const auto& e : g_trackSafes)
    {
        if (e.mask == 0) continue;
        std::string sg = SafesGuidToString(e.guid);
        ctx->AddLine("LTSAFETRACK %s %d", sg.c_str(), e.mask);
    }
}
