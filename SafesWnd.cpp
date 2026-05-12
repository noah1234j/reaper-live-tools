#include "SafesWnd.h"
#include "TransitionEngine.h"    // g_globalSafeMask, g_trackSafes, GetEffectiveSafeMask
#include "TransitionSnapshot.h"  // TS_* bit flags
#include "api.h"                 // GetNumTracks, GetTrack, GetSetMediaTrackInfo, etc.
#include "resource.h"

extern bool g_trackSafesEnabled;

#include <commctrl.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Column layout
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
    COL_COUNT
};

// Mapping: column → TS_* bit(s)
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
};

static const char* k_colName[COL_COUNT] = {
    "Track", "Vol", "Pan", "Mute", "Solo", "Phase", "FX", "Vis", "Sel",
    "Name", "Color", "Height", "Order"
};
static const int k_colWidth[COL_COUNT] = {
    140, 32, 32, 36, 36, 40, 32, 32, 32,
    38, 40, 44, 40
};

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
        for (int c = 1; c < COL_COUNT; ++c)
            ListView_SetItemText(g_hList, i, c, (LPSTR)" ");
    }
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

        // Add columns
        LVCOLUMNA col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        for (int c = 0; c < COL_COUNT; ++c)
        {
            col.pszText = (LPSTR)k_colName[c];
            col.cx      = k_colWidth[c];
            col.fmt     = (c == COL_TRACK) ? LVCFMT_LEFT : LVCFMT_CENTER;
            ListView_InsertColumn(g_hList, c, &col);
        }

        // Build rows and populate
        RebuildRows();
        PopulateList();

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
        CheckDlgButton(hDlg, IDC_GSAFE_LAYERS, (g_globalSafeMask & TS_LAYERS)       ? BST_CHECKED : BST_UNCHECKED);

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
        const int GRP_H = 40;   // Global Safes groupbox
        const int CHK_H = 14;   // per-track enable checkbox row

        // Global Safes groupbox spans full width
        HWND hGrp = GetDlgItem(hDlg, IDC_GSAFES_GROUP);
        if (hGrp) SetWindowPos(hGrp, nullptr, MARGIN, MARGIN, W - MARGIN*2, GRP_H, SWP_NOZORDER);

        // 7+4+1 param checkboxes spread evenly inside groupbox (2 rows)
        // Row 0: Vol Pan Mute Solo Phase FX Vis
        // Row 1: Name Color Height Order Layers
        static const int k_gsIds[] = {
            IDC_GSAFE_VOL, IDC_GSAFE_PAN, IDC_GSAFE_MUTE, IDC_GSAFE_SOLO,
            IDC_GSAFE_PHASE, IDC_GSAFE_FX, IDC_GSAFE_VIS,
            IDC_GSAFE_NAME, IDC_GSAFE_COLOR, IDC_GSAFE_HEIGHT, IDC_GSAFE_ORDER,
            IDC_GSAFE_LAYERS
        };
        static const int k_gsRow[] = { 0,0,0,0,0,0,0, 1,1,1,1,1 };
        static const int k_gsCol[] = { 0,1,2,3,4,5,6, 0,1,2,3,4 };
        const int grpInner = W - MARGIN*2 - 14;
        const int slot7 = grpInner / 7;
        for (int i = 0; i < 12; ++i) {
            HWND h = GetDlgItem(hDlg, k_gsIds[i]);
            if (!h) continue;
            int col = k_gsCol[i];
            int row = k_gsRow[i];
            SetWindowPos(h, nullptr,
                MARGIN + 7 + col * slot7,
                MARGIN + 18 + row * 16,
                slot7 - 2, CHK_H, SWP_NOZORDER);
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
            // Uncheck all global safe param buttons
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
            if (g_hList) InvalidateRect(g_hList, nullptr, FALSE);
            break;

        // Global safe per-parameter toggles
        case IDC_GSAFE_VOL:    if (IsDlgButtonChecked(hDlg, IDC_GSAFE_VOL)    == BST_CHECKED) g_globalSafeMask |= TS_VOL;    else g_globalSafeMask &= ~TS_VOL;    break;
        case IDC_GSAFE_PAN:    if (IsDlgButtonChecked(hDlg, IDC_GSAFE_PAN)    == BST_CHECKED) g_globalSafeMask |= TS_PAN;    else g_globalSafeMask &= ~TS_PAN;    break;
        case IDC_GSAFE_MUTE:   if (IsDlgButtonChecked(hDlg, IDC_GSAFE_MUTE)   == BST_CHECKED) g_globalSafeMask |= TS_MUTE;   else g_globalSafeMask &= ~TS_MUTE;   break;
        case IDC_GSAFE_SOLO:   if (IsDlgButtonChecked(hDlg, IDC_GSAFE_SOLO)   == BST_CHECKED) g_globalSafeMask |= TS_SOLO;   else g_globalSafeMask &= ~TS_SOLO;   break;
        case IDC_GSAFE_PHASE:  if (IsDlgButtonChecked(hDlg, IDC_GSAFE_PHASE)  == BST_CHECKED) g_globalSafeMask |= TS_PHASE;  else g_globalSafeMask &= ~TS_PHASE;  break;
        case IDC_GSAFE_FX:     if (IsDlgButtonChecked(hDlg, IDC_GSAFE_FX)     == BST_CHECKED) g_globalSafeMask |= (TS_FXPARAMS|TS_FXCHAIN); else g_globalSafeMask &= ~(TS_FXPARAMS|TS_FXCHAIN); break;
        case IDC_GSAFE_VIS:    if (IsDlgButtonChecked(hDlg, IDC_GSAFE_VIS)    == BST_CHECKED) g_globalSafeMask |= TS_VIS;    else g_globalSafeMask &= ~TS_VIS;    break;
        case IDC_GSAFE_NAME:   if (IsDlgButtonChecked(hDlg, IDC_GSAFE_NAME)   == BST_CHECKED) g_globalSafeMask |= TS_TRACKNAME;   else g_globalSafeMask &= ~TS_TRACKNAME;   break;
        case IDC_GSAFE_COLOR:  if (IsDlgButtonChecked(hDlg, IDC_GSAFE_COLOR)  == BST_CHECKED) g_globalSafeMask |= TS_TRACKCOLOR;  else g_globalSafeMask &= ~TS_TRACKCOLOR;  break;
        case IDC_GSAFE_HEIGHT: if (IsDlgButtonChecked(hDlg, IDC_GSAFE_HEIGHT) == BST_CHECKED) g_globalSafeMask |= TS_TRACKHEIGHT; else g_globalSafeMask &= ~TS_TRACKHEIGHT; break;
        case IDC_GSAFE_ORDER:  if (IsDlgButtonChecked(hDlg, IDC_GSAFE_ORDER)  == BST_CHECKED) g_globalSafeMask |= TS_TRACKORDER;  else g_globalSafeMask &= ~TS_TRACKORDER;  break;
        case IDC_GSAFE_LAYERS: if (IsDlgButtonChecked(hDlg, IDC_GSAFE_LAYERS) == BST_CHECKED) g_globalSafeMask |= TS_LAYERS;      else g_globalSafeMask &= ~TS_LAYERS;      break;

        case IDC_TRACK_SAFES_EN:
            g_trackSafesEnabled = (IsDlgButtonChecked(hDlg, IDC_TRACK_SAFES_EN) == BST_CHECKED);
            if (g_hList) InvalidateRect(g_hList, nullptr, FALSE);
            break;
        }
        break;

    case WM_NOTIFY:
    {
        NMHDR* pnm = (NMHDR*)lParam;
        if (pnm->hwndFrom != g_hList) break;

        if (pnm->code == NM_CLICK)
        {
            // Hit-test to find row + column
            NMITEMACTIVATE* pnia = (NMITEMACTIVATE*)lParam;
            LVHITTESTINFO ht = {};
            ht.pt = pnia->ptAction;
            ListView_SubItemHitTest(g_hList, &ht);
            const int row = ht.iItem;
            const int col = ht.iSubItem;
            if (row >= 0 && col > 0 && col < COL_COUNT)
            {
                ToggleBit(row, k_colBit[col]);
                // Repaint just the clicked row
                RECT rcRow;
                ListView_GetItemRect(g_hList, row, &rcRow, LVIR_BOUNDS);
                InvalidateRect(g_hList, &rcRow, FALSE);
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
                const int col = (int)pcd->iSubItem;
                if (col == COL_TRACK) break; // let default draw track name

                HDC   hdc  = pcd->nmcd.hdc;
                RECT  rcIt = pcd->nmcd.rc;

                // Fill background
                const bool isSelected = (ListView_GetItemState(g_hList, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                COLORREF bg = isSelected ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_WINDOW);
                SetBkColor(hdc, bg);
                ExtTextOutA(hdc, 0, 0, ETO_OPAQUE, &rcIt, "", 0, nullptr);

                // Draw checkbox centred in cell
                const int bit     = k_colBit[col];
                const int rowMask = GetRowMask(row);
                const bool checked = (rowMask & bit) != 0;

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
