// ---------------------------------------------------------------------------
// LayersWnd.cpp  –  Live Tools: Layers  –  dockable window
//
// Left panel  : ListView of all 10 layers (drag-to-reorder).
//               Columns: #  Name  Max Ch  Tracks
// Right panel : Properties for the selected layer.
//               – Name edit + Set button
//               – Max channels spin (0 = unlimited)
//               – Track ListView (drag-to-reorder within layer)
//               – Add Selected | Remove | Move Up | Move Down | Capture | Clear
// Bottom bar  : Activate | Prev | Next | Show All | Settings... | status
//
// Settings modal (IDD_LAYERS_SETTINGS):
//   Apply MCP visibility | Also hide TCP | Reorder tracks | Restore on deactivate
// ---------------------------------------------------------------------------
#include "LayersWnd.h"
#include "LayersEngine.h"
#include "api.h"
#include "resource.h"

#include <windowsx.h>
#include <commctrl.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static HINSTANCE s_hInst  = nullptr;
static HWND      s_hwnd   = nullptr;
static int       s_selLayer = 0;   // index of layer selected in list (0-based)
static bool      s_suppressNameChange = false;  // guards EN_CHANGE during programmatic SetDlgItemText

// Drag state – layer list
static bool s_draggingLayer  = false;
static int  s_dragLayerSrc   = -1;
static int  s_dragLayerDst   = -1;
static HIMAGELIST s_hLayerDragImg = nullptr;

// Drag state – track list within a layer
static bool s_draggingTrack  = false;
static int  s_dragTrackSrc   = -1;
static int  s_dragTrackDst   = -1;
static HIMAGELIST s_hTrackDragImg = nullptr;

// Subclass state – layer list
static WNDPROC s_origLayerListProc = nullptr;
static bool    s_lyrLbTracking     = false;
static POINT   s_lyrLbDownPt       = {};
static int     s_lyrLbDownItem     = -1;
static DWORD   s_lyrLbDownTime     = 0;

// Subclass state – track list
static WNDPROC s_origTrackListProc = nullptr;
static bool    s_trkLbTracking     = false;
static POINT   s_trkLbDownPt       = {};
static int     s_trkLbDownItem     = -1;
static DWORD   s_trkLbDownTime     = 0;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK LayersDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static void RefreshLayerList(HWND hwnd);
static void RefreshTrackList(HWND hwnd);
static void UpdateStatus(HWND hwnd);
static void LoadLayerToUI(HWND hwnd, int idx);
static void ApplyNameFromUI(HWND hwnd);
static void ApplyMaxChFromUI(HWND hwnd);
static int  GetTrackListSel(HWND hwnd);
static void EndLayerDrag(HWND hwnd, bool apply);
static void EndTrackDrag(HWND hwnd, bool apply);
static LRESULT CALLBACK LayerListSubclassProc(HWND hList, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK TrackListSubclassProc(HWND hList, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void LayersWnd_Init(HINSTANCE hInst)
{
    s_hInst = hInst;
}

void LayersWnd_Cleanup()
{
    if (s_hwnd && IsWindow(s_hwnd))
    {
        DestroyWindow(s_hwnd);
        s_hwnd = nullptr;
    }
}

void LayersWnd_ShowHide()
{
    if (!s_hwnd || !IsWindow(s_hwnd))
    {
        HWND hMain = GetMainHwnd();
        s_hwnd = CreateDialogParam(s_hInst,
                                   MAKEINTRESOURCE(IDD_LAYERS),
                                   hMain,
                                   LayersDlgProc,
                                   0);
        if (!s_hwnd)
        {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "CreateDialogParam failed for IDD_LAYERS. Error=%lu", GetLastError());
            MessageBoxA(hMain, buf, "Layers", MB_OK | MB_ICONERROR);
            return;
        }
    }

    if (IsWindowVisible(s_hwnd))
        ShowWindow(s_hwnd, SW_HIDE);
    else
    {
        LayersEngine::Get().RefreshAllTrackNames();
        ShowWindow(s_hwnd, SW_SHOW);
    }
}

int LayersWnd_IsVisible()
{
    return (s_hwnd && IsWindow(s_hwnd) && IsWindowVisible(s_hwnd)) ? 1 : 0;
}

void LayersWnd_Refresh()
{
    if (s_hwnd && IsWindow(s_hwnd) && IsWindowVisible(s_hwnd))
    {
        LayersEngine::Get().RefreshAllTrackNames();
        RefreshLayerList(s_hwnd);
        RefreshTrackList(s_hwnd);
        UpdateStatus(s_hwnd);
    }
}

// ---------------------------------------------------------------------------
// RefreshLayerList
// ---------------------------------------------------------------------------
static void RefreshLayerList(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LYR_LAYER_LIST);
    if (!hList) return;

    int active = LayersEngine::Get().GetActiveLayer();
    int count  = LayersEngine::Get().GetLayerCount();

    int prevSel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (prevSel < 0) prevSel = s_selLayer;

    ListView_DeleteAllItems(hList);

    for (int i = 0; i < count; i++)
    {
        const LayerDef& ld = LayersEngine::Get().GetLayer(i);

        LVITEMA lvi = {};
        lvi.mask  = LVIF_TEXT;
        lvi.iItem = i;
        char numBuf[12];
        if (i == active)
            snprintf(numBuf, sizeof(numBuf), "%d*", i + 1);
        else
            snprintf(numBuf, sizeof(numBuf), "%d", i + 1);
        lvi.pszText = numBuf;
        ListView_InsertItem(hList, &lvi);

        ListView_SetItemText(hList, i, 1, const_cast<char*>(ld.name));

        char chBuf[16];
        if (ld.maxChannels > 0)
            snprintf(chBuf, sizeof(chBuf), "%d", ld.maxChannels);
        else
            strcpy(chBuf, "All");
        ListView_SetItemText(hList, i, 2, chBuf);

        // Track count = non-spacer entries
        int realTracks = 0;
        for (const auto& lt : ld.tracks)
            if (!lt.isSpacer) realTracks++;
        char trBuf[16];
        snprintf(trBuf, sizeof(trBuf), "%d", realTracks);
        ListView_SetItemText(hList, i, 3, trBuf);
    }

    int sel = (prevSel >= 0 && prevSel < count) ? prevSel : 0;
    if (count > 0)
    {
        ListView_SetItemState(hList, sel,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hList, sel, FALSE);
    }
}

// ---------------------------------------------------------------------------
// RefreshTrackList
// ---------------------------------------------------------------------------
static void RefreshTrackList(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LYR_TRACK_LIST);
    if (!hList) return;

    int n = LayersEngine::Get().GetLayerCount();
    if (s_selLayer < 0 || s_selLayer >= n)
    {
        ListView_DeleteAllItems(hList);
        return;
    }

    LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);

    int prevSel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);

    ListView_DeleteAllItems(hList);

    int realIdx = 0;  // count of real (non-spacer) tracks for numbering
    for (int i = 0; i < (int)ld.tracks.size(); i++)
    {
        const LayerTrack& lt = ld.tracks[i];
        LVITEMA lvi = {};
        lvi.mask     = LVIF_TEXT;
        lvi.iItem    = i;
        lvi.iSubItem = 0;
        char numBuf[8];
        if (lt.isSpacer)
            strcpy(numBuf, "--");
        else
            snprintf(numBuf, sizeof(numBuf), "%d", realIdx + 1);
        lvi.pszText = numBuf;
        ListView_InsertItem(hList, &lvi);

        if (lt.isSpacer)
        {
            ListView_SetItemText(hList, i, 1, const_cast<char*>("--- Spacer ---"));
        }
        else
        {
            int limit = ld.maxChannels > 0 ? ld.maxChannels : (int)ld.tracks.size();
            char dispName[160];
            if (i < limit)
                snprintf(dispName, sizeof(dispName), "%s", lt.name);
            else
                snprintf(dispName, sizeof(dispName), "[%s]", lt.name);
            ListView_SetItemText(hList, i, 1, dispName);
            realIdx++;
        }
    }

    if (prevSel >= 0 && prevSel < (int)ld.tracks.size())
    {
        ListView_SetItemState(hList, prevSel,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
}

// ---------------------------------------------------------------------------
// LoadLayerToUI – populate name + maxch controls from selected layer
// ---------------------------------------------------------------------------
static void LoadLayerToUI(HWND hwnd, int idx)
{
    int n = LayersEngine::Get().GetLayerCount();
    if (idx < 0 || idx >= n) return;
    const LayerDef& ld = LayersEngine::Get().GetLayer(idx);

    EnableWindow(GetDlgItem(hwnd, IDC_LYR_NAME_EDIT),   TRUE);
    EnableWindow(GetDlgItem(hwnd, IDC_LYR_MAXCH_EDIT),  TRUE);
    EnableWindow(GetDlgItem(hwnd, IDC_LYR_MAXCH_SPIN),  TRUE);
    EnableWindow(GetDlgItem(hwnd, IDC_LYR_ADD_TRACK),   TRUE);
    EnableWindow(GetDlgItem(hwnd, IDC_LYR_REM_TRACK),   TRUE);
    EnableWindow(GetDlgItem(hwnd, IDC_LYR_CAPTURE),     TRUE);
    EnableWindow(GetDlgItem(hwnd, IDC_LYR_CLEAR_LAYER), TRUE);
    EnableWindow(GetDlgItem(hwnd, IDC_LYR_ACTIVATE),    TRUE);

    s_suppressNameChange = true;
    SetDlgItemText(hwnd, IDC_LYR_NAME_EDIT, ld.name);
    s_suppressNameChange = false;
    SetDlgItemInt(hwnd, IDC_LYR_MAXCH_EDIT, ld.maxChannels, FALSE);

    char title[80];
    snprintf(title, sizeof(title), "Layer %d Properties", idx + 1);
    SetDlgItemText(hwnd, IDC_LYR_PROP_GROUP, title);
}

// ---------------------------------------------------------------------------
// UpdateStatus
// ---------------------------------------------------------------------------
static void UpdateStatus(HWND hwnd)
{
    int active = LayersEngine::Get().GetActiveLayer();
    int n      = LayersEngine::Get().GetLayerCount();
    char buf[128];
    if (active >= 0 && active < n)
        snprintf(buf, sizeof(buf), "Active: %s (Layer %d)",
            LayersEngine::Get().GetLayer(active).name, active + 1);
    else
        strcpy(buf, "No layer active (all tracks shown)");
    SetDlgItemText(hwnd, IDC_LYR_STATUS, buf);
}

// ---------------------------------------------------------------------------
// Apply name / max-ch from UI to engine
// ---------------------------------------------------------------------------
static void ApplyNameFromUI(HWND hwnd)
{
    int n = LayersEngine::Get().GetLayerCount();
    if (s_selLayer < 0 || s_selLayer >= n) return;
    char buf[64] = {};
    GetDlgItemText(hwnd, IDC_LYR_NAME_EDIT, buf, sizeof(buf));
    if (!buf[0]) return;
    strncpy(LayersEngine::Get().GetLayer(s_selLayer).name, buf, 63);
    LayersEngine::Get().UpdateLayerActionDesc(s_selLayer);
    LayersEngine::Get().SaveExtState();
    RefreshLayerList(hwnd);
}

static void ApplyMaxChFromUI(HWND hwnd)
{
    int n = LayersEngine::Get().GetLayerCount();
    if (s_selLayer < 0 || s_selLayer >= n) return;
    BOOL ok = FALSE;
    int val = (int)GetDlgItemInt(hwnd, IDC_LYR_MAXCH_EDIT, &ok, FALSE);
    if (!ok || val < 0) val = 0;
    LayersEngine::Get().GetLayer(s_selLayer).maxChannels = val;
    LayersEngine::Get().SaveExtState();
    RefreshLayerList(hwnd);
    RefreshTrackList(hwnd);
}

// ---------------------------------------------------------------------------
// Get selected index in track list
// ---------------------------------------------------------------------------
static int GetTrackListSel(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LYR_TRACK_LIST);
    if (!hList) return -1;
    return ListView_GetNextItem(hList, -1, LVNI_SELECTED);
}

// ---------------------------------------------------------------------------
// Drag helpers
// ---------------------------------------------------------------------------
static void EndLayerDrag(HWND hwnd, bool apply)
{
    if (s_hLayerDragImg)
    {
        ImageList_DragLeave(hwnd);
        ImageList_EndDrag();
        ImageList_Destroy(s_hLayerDragImg);
        s_hLayerDragImg = nullptr;
    }
    ReleaseCapture();

    HWND hList = GetDlgItem(hwnd, IDC_LYR_LAYER_LIST);
    if (hList)
    {
        int cnt = LayersEngine::Get().GetLayerCount();
        for (int i = 0; i < cnt; i++)
            ListView_SetItemState(hList, i, 0, LVIS_DROPHILITED);
    }

    if (apply && s_dragLayerDst >= 0 && s_dragLayerDst != s_dragLayerSrc)
    {
        LayersEngine::Get().MoveLayer(s_dragLayerSrc, s_dragLayerDst);
        s_selLayer = s_dragLayerDst;
        RefreshLayerList(hwnd);
        RefreshTrackList(hwnd);
        LoadLayerToUI(hwnd, s_selLayer);
    }

    s_draggingLayer = false;
    s_dragLayerSrc  = s_dragLayerDst = -1;
}

static void EndTrackDrag(HWND hwnd, bool apply)
{
    if (s_hTrackDragImg)
    {
        ImageList_DragLeave(hwnd);
        ImageList_EndDrag();
        ImageList_Destroy(s_hTrackDragImg);
        s_hTrackDragImg = nullptr;
    }
    ReleaseCapture();

    HWND hListT = GetDlgItem(hwnd, IDC_LYR_TRACK_LIST);
    if (hListT)
    {
        int cnt = LayersEngine::Get().GetLayerCount();
        int trks = (s_selLayer >= 0 && s_selLayer < cnt)
            ? (int)LayersEngine::Get().GetLayer(s_selLayer).tracks.size() : 0;
        for (int i = 0; i < trks; i++)
            ListView_SetItemState(hListT, i, 0, LVIS_DROPHILITED);
    }

    int n = LayersEngine::Get().GetLayerCount();
    if (apply && s_dragTrackDst >= 0 && s_dragTrackDst != s_dragTrackSrc &&
        s_selLayer >= 0 && s_selLayer < n)
    {
        LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);
        int sz = (int)ld.tracks.size();
        int from = s_dragTrackSrc;
        int to   = s_dragTrackDst;
        if (from >= 0 && from < sz && to >= 0 && to < sz)
        {
            LayerTrack temp = ld.tracks[from];
            if (from < to)
                for (int i = from; i < to; i++) ld.tracks[i] = ld.tracks[i + 1];
            else
                for (int i = from; i > to; i--) ld.tracks[i] = ld.tracks[i - 1];
            ld.tracks[to] = temp;
            LayersEngine::Get().SaveExtState();
        }
        RefreshTrackList(hwnd);
        RefreshLayerList(hwnd);
    }

    s_draggingTrack = false;
    s_dragTrackSrc  = s_dragTrackDst = -1;
}

// ---------------------------------------------------------------------------
// Settings dialog proc
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        const LayersSettings& cfg = LayersEngine::Get().GetSettings();
        CheckDlgButton(hwnd, IDC_LYR_SET_MCPVIS,  cfg.applyMcpVisibility  ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_LYR_SET_HIDETCP,  cfg.hideTcpToo          ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_LYR_SET_REORDER,  cfg.reorderTracks       ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_LYR_SET_RESTORE,  cfg.restoreOnDeactivate ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
        {
            LayersSettings cfg;
            cfg.applyMcpVisibility  = (IsDlgButtonChecked(hwnd, IDC_LYR_SET_MCPVIS)  == BST_CHECKED);
            cfg.hideTcpToo          = (IsDlgButtonChecked(hwnd, IDC_LYR_SET_HIDETCP)  == BST_CHECKED);
            cfg.reorderTracks       = (IsDlgButtonChecked(hwnd, IDC_LYR_SET_REORDER)  == BST_CHECKED);
            cfg.restoreOnDeactivate = (IsDlgButtonChecked(hwnd, IDC_LYR_SET_RESTORE)  == BST_CHECKED);
            LayersEngine::Get().SetSettings(cfg);
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Layer list subclass – smooth drag-to-reorder via mouse-move threshold
// ---------------------------------------------------------------------------
static LRESULT CALLBACK LayerListSubclassProc(HWND hList, UINT msg, WPARAM wParam, LPARAM lParam)
{
    HWND dlg = GetParent(hList);
    switch (msg)
    {
    case WM_LBUTTONDOWN:
    {
        LRESULT r = CallWindowProc(s_origLayerListProc, hList, msg, wParam, lParam);
        LVHITTESTINFO hti = {};
        hti.pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (ListView_HitTest(hList, &hti) >= 0)
        {
            s_lyrLbTracking = true;
            s_lyrLbDownPt   = hti.pt;
            s_lyrLbDownItem = hti.iItem;
            s_lyrLbDownTime = GetTickCount();
        }
        return r;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (s_lyrLbTracking && !(wParam & MK_LBUTTON))
            s_lyrLbTracking = false;
        if (s_lyrLbTracking && !s_draggingLayer)
        {
            bool moved = (abs(pt.x - s_lyrLbDownPt.x) > GetSystemMetrics(SM_CXDRAG) ||
                          abs(pt.y - s_lyrLbDownPt.y) > GetSystemMetrics(SM_CYDRAG));
            bool held  = (GetTickCount() - s_lyrLbDownTime >= 200);
            if (moved && held)
            {
                s_dragLayerSrc  = s_lyrLbDownItem;
                s_dragLayerDst  = -1;
                s_lyrLbTracking = false;
                SetCapture(hList);
                POINT off = { 8, 8 };
                s_hLayerDragImg = ListView_CreateDragImage(hList, s_dragLayerSrc, &off);
                if (s_hLayerDragImg)
                {
                    POINT dlgPt = pt;
                    ClientToScreen(hList, &dlgPt);
                    ScreenToClient(dlg, &dlgPt);
                    ImageList_BeginDrag(s_hLayerDragImg, 0, 8, 8);
                    ImageList_DragEnter(dlg, dlgPt.x, dlgPt.y);
                }
                s_draggingLayer = true;
            }
        }
        if (s_draggingLayer)
        {
            POINT dlgPt = pt;
            ClientToScreen(hList, &dlgPt);
            ScreenToClient(dlg, &dlgPt);
            if (s_hLayerDragImg)
            {
                ImageList_DragMove(dlgPt.x, dlgPt.y);
                ImageList_DragShowNolock(FALSE);
            }
            LVHITTESTINFO hti = {};
            hti.pt = pt;
            int dst = ListView_HitTest(hList, &hti);
            if (dst != s_dragLayerDst)
            {
                if (s_dragLayerDst >= 0)
                    ListView_SetItemState(hList, s_dragLayerDst, 0, LVIS_DROPHILITED);
                s_dragLayerDst = dst;
                if (dst >= 0)
                    ListView_SetItemState(hList, dst, LVIS_DROPHILITED, LVIS_DROPHILITED);
            }
            if (s_hLayerDragImg)
                ImageList_DragShowNolock(TRUE);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP:
        s_lyrLbTracking = false;
        if (s_draggingLayer)
        {
            s_draggingLayer = false;
            EndLayerDrag(dlg, true);
        }
        break;
    case WM_CAPTURECHANGED:
        s_lyrLbTracking = false;
        if (s_draggingLayer)
        {
            s_draggingLayer = false;
            EndLayerDrag(dlg, false);
        }
        break;
    }
    return CallWindowProc(s_origLayerListProc, hList, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Track list subclass – smooth drag-to-reorder via mouse-move threshold
// ---------------------------------------------------------------------------
static LRESULT CALLBACK TrackListSubclassProc(HWND hList, UINT msg, WPARAM wParam, LPARAM lParam)
{
    HWND dlg = GetParent(hList);
    switch (msg)
    {
    case WM_LBUTTONDOWN:
    {
        LRESULT r = CallWindowProc(s_origTrackListProc, hList, msg, wParam, lParam);
        LVHITTESTINFO hti = {};
        hti.pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (ListView_HitTest(hList, &hti) >= 0)
        {
            s_trkLbTracking = true;
            s_trkLbDownPt   = hti.pt;
            s_trkLbDownItem = hti.iItem;
            s_trkLbDownTime = GetTickCount();
        }
        return r;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (s_trkLbTracking && !(wParam & MK_LBUTTON))
            s_trkLbTracking = false;
        if (s_trkLbTracking && !s_draggingTrack)
        {
            bool moved = (abs(pt.x - s_trkLbDownPt.x) > GetSystemMetrics(SM_CXDRAG) ||
                          abs(pt.y - s_trkLbDownPt.y) > GetSystemMetrics(SM_CYDRAG));
            bool held  = (GetTickCount() - s_trkLbDownTime >= 200);
            if (moved && held)
            {
                s_dragTrackSrc  = s_trkLbDownItem;
                s_dragTrackDst  = -1;
                s_trkLbTracking = false;
                SetCapture(hList);
                POINT off = { 8, 8 };
                s_hTrackDragImg = ListView_CreateDragImage(hList, s_dragTrackSrc, &off);
                if (s_hTrackDragImg)
                {
                    POINT dlgPt = pt;
                    ClientToScreen(hList, &dlgPt);
                    ScreenToClient(dlg, &dlgPt);
                    ImageList_BeginDrag(s_hTrackDragImg, 0, 8, 8);
                    ImageList_DragEnter(dlg, dlgPt.x, dlgPt.y);
                }
                s_draggingTrack = true;
            }
        }
        if (s_draggingTrack)
        {
            POINT dlgPt = pt;
            ClientToScreen(hList, &dlgPt);
            ScreenToClient(dlg, &dlgPt);
            if (s_hTrackDragImg)
            {
                ImageList_DragMove(dlgPt.x, dlgPt.y);
                ImageList_DragShowNolock(FALSE);
            }
            LVHITTESTINFO hti = {};
            hti.pt = pt;
            int dst = ListView_HitTest(hList, &hti);
            if (dst != s_dragTrackDst)
            {
                if (s_dragTrackDst >= 0)
                    ListView_SetItemState(hList, s_dragTrackDst, 0, LVIS_DROPHILITED);
                s_dragTrackDst = dst;
                if (dst >= 0)
                    ListView_SetItemState(hList, dst, LVIS_DROPHILITED, LVIS_DROPHILITED);
            }
            if (s_hTrackDragImg)
                ImageList_DragShowNolock(TRUE);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP:
        s_trkLbTracking = false;
        if (s_draggingTrack)
        {
            s_draggingTrack = false;
            EndTrackDrag(dlg, true);
        }
        break;
    case WM_CAPTURECHANGED:
        s_trkLbTracking = false;
        if (s_draggingTrack)
        {
            s_draggingTrack = false;
            EndTrackDrag(dlg, false);
        }
        break;
    }
    return CallWindowProc(s_origTrackListProc, hList, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Main dialog proc
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK LayersDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    // -----------------------------------------------------------------------
    case WM_INITDIALOG:
    {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS };
        InitCommonControlsEx(&icc);

        // ---- Create layer ListView (left panel) ----------------------------
        {
            HWND hPh = GetDlgItem(hwnd, IDC_LYR_LAYER_LIST);
            RECT r = {};
            GetWindowRect(hPh, &r);
            MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&r, 2);
            DestroyWindow(hPh);

            HWND hList = CreateWindowExA(0, "SysListView32", "",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
                LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
                r.left, r.top, r.right - r.left, r.bottom - r.top,
                hwnd, (HMENU)(INT_PTR)IDC_LYR_LAYER_LIST, s_hInst, nullptr);

            if (hList)
            {
                ListView_SetExtendedListViewStyle(hList,
                    LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

                LVCOLUMNA col = {};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.cx = 28;  col.pszText = const_cast<char*>("#");
                ListView_InsertColumn(hList, 0, &col);
                col.cx = 110; col.pszText = const_cast<char*>("Name");
                ListView_InsertColumn(hList, 1, &col);
                col.cx = 40;  col.pszText = const_cast<char*>("Max Ch");
                ListView_InsertColumn(hList, 2, &col);
                col.cx = 36;  col.pszText = const_cast<char*>("Trks");
                ListView_InsertColumn(hList, 3, &col);
                s_origLayerListProc = (WNDPROC)(LONG_PTR)SetWindowLongPtr(
                    hList, GWLP_WNDPROC, (LONG_PTR)LayerListSubclassProc);
            }
        }

        // ---- Create track ListView (right panel) --------------------------
        {
            HWND hPh = GetDlgItem(hwnd, IDC_LYR_TRACK_LIST);
            RECT r = {};
            GetWindowRect(hPh, &r);
            MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&r, 2);
            DestroyWindow(hPh);

            HWND hList = CreateWindowExA(0, "SysListView32", "",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
                LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
                r.left, r.top, r.right - r.left, r.bottom - r.top,
                hwnd, (HMENU)(INT_PTR)IDC_LYR_TRACK_LIST, s_hInst, nullptr);

            if (hList)
            {
                ListView_SetExtendedListViewStyle(hList,
                    LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

                LVCOLUMNA col = {};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.cx = 28;  col.pszText = const_cast<char*>("#");
                ListView_InsertColumn(hList, 0, &col);
                col.cx = 200; col.pszText = const_cast<char*>("Track Name");
                ListView_InsertColumn(hList, 1, &col);
                s_origTrackListProc = (WNDPROC)(LONG_PTR)SetWindowLongPtr(
                    hList, GWLP_WNDPROC, (LONG_PTR)TrackListSubclassProc);
            }
        }

        // ---- Max channels spin -------------------------------------------
        {
            HWND hSpin = GetDlgItem(hwnd, IDC_LYR_MAXCH_SPIN);
            HWND hEdit = GetDlgItem(hwnd, IDC_LYR_MAXCH_EDIT);
            if (hSpin && hEdit)
            {
                SendMessage(hSpin, UDM_SETRANGE, 0, MAKELONG(256, 0));
                SendMessage(hSpin, UDM_SETBUDDY, (WPARAM)hEdit, 0);
            }
        }

        // ---- Populate -------------------------------------------------------
        LayersEngine::Get().RefreshAllTrackNames();
        s_selLayer = 0;
        RefreshLayerList(hwnd);
        RefreshTrackList(hwnd);
        LoadLayerToUI(hwnd, s_selLayer);
        UpdateStatus(hwnd);

        return TRUE;
    }

    // -----------------------------------------------------------------------
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        int note = HIWORD(wParam);
        (void)note;

        switch (id)
        {
        // --- Bottom bar ---------------------------------------------------
        case IDC_LYR_ACTIVATE:
            if (LayersEngine::Get().GetLayerCount() > 0 &&
                s_selLayer >= 0 && s_selLayer < LayersEngine::Get().GetLayerCount())
            {
                ApplyNameFromUI(hwnd);
                ApplyMaxChFromUI(hwnd);
                LayersEngine::Get().ActivateLayer(s_selLayer);
                RefreshLayerList(hwnd);
                UpdateStatus(hwnd);
            }
            break;

        case IDC_LYR_PREV:
            LayersEngine::Get().PrevLayer();
        {
            int active = LayersEngine::Get().GetActiveLayer();
            if (active >= 0)
            {
                s_selLayer = active;
                HWND hList = GetDlgItem(hwnd, IDC_LYR_LAYER_LIST);
                ListView_SetItemState(hList, s_selLayer,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(hList, s_selLayer, FALSE);
                RefreshTrackList(hwnd);
                LoadLayerToUI(hwnd, s_selLayer);
            }
        }
            RefreshLayerList(hwnd);
            UpdateStatus(hwnd);
            break;

        case IDC_LYR_NEXT:
            LayersEngine::Get().NextLayer();
        {
            int active = LayersEngine::Get().GetActiveLayer();
            if (active >= 0)
            {
                s_selLayer = active;
                HWND hList = GetDlgItem(hwnd, IDC_LYR_LAYER_LIST);
                ListView_SetItemState(hList, s_selLayer,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(hList, s_selLayer, FALSE);
                RefreshTrackList(hwnd);
                LoadLayerToUI(hwnd, s_selLayer);
            }
        }
            RefreshLayerList(hwnd);
            UpdateStatus(hwnd);
            break;

        case IDC_LYR_DEACTIVATE:
            LayersEngine::Get().Deactivate();
            RefreshLayerList(hwnd);
            UpdateStatus(hwnd);
            break;

        case IDC_LYR_SETTINGS_BTN:
            DialogBox(s_hInst,
                      MAKEINTRESOURCE(IDD_LAYERS_SETTINGS),
                      hwnd,
                      SettingsDlgProc);
            UpdateStatus(hwnd);
            break;

        // --- Layer list management ----------------------------------------
        case IDC_LYR_ADD_LAYER:
        {
            int idx = LayersEngine::Get().AddLayer(nullptr);
            s_selLayer = idx;
            RefreshLayerList(hwnd);
            RefreshTrackList(hwnd);
            LoadLayerToUI(hwnd, s_selLayer);
            UpdateStatus(hwnd);
            // Focus name field for immediate rename
            SetFocus(GetDlgItem(hwnd, IDC_LYR_NAME_EDIT));
            break;
        }

        case IDC_LYR_ADD_SPACER:
        {
            int n = LayersEngine::Get().GetLayerCount();
            if (s_selLayer < 0 || s_selLayer >= n) break;
            LayersEngine::Get().AddSpacerTrack(s_selLayer);
            RefreshTrackList(hwnd);
            RefreshLayerList(hwnd);
            // Select the new spacer row in the track list
            {
                LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);
                int newIdx = (int)ld.tracks.size() - 1;
                HWND hTrkList = GetDlgItem(hwnd, IDC_LYR_TRACK_LIST);
                ListView_SetItemState(hTrkList, newIdx,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(hTrkList, newIdx, FALSE);
            }
            break;
        }

        case IDC_LYR_DELETE_LAYER:
        {
            int n = LayersEngine::Get().GetLayerCount();
            if (s_selLayer < 0 || s_selLayer >= n) break;
            const LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);
            char msg[128];
            snprintf(msg, sizeof(msg), "Delete layer \"%.60s\"?", ld.name);
            if (MessageBoxA(hwnd, msg, "Layers", MB_YESNO | MB_ICONQUESTION) != IDYES) break;
            LayersEngine::Get().RemoveLayer(s_selLayer);
            s_selLayer = (s_selLayer > 0) ? s_selLayer - 1 : 0;
            RefreshLayerList(hwnd);
            RefreshTrackList(hwnd);
            LoadLayerToUI(hwnd, s_selLayer);
            UpdateStatus(hwnd);
            break;
        }

        // --- Layer properties (name saves live via EN_CHANGE) ---------------
        case IDC_LYR_NAME_EDIT:
            if (note == EN_CHANGE && !s_suppressNameChange)
                ApplyNameFromUI(hwnd);
            break;

        // --- Track management --------------------------------------------
        case IDC_LYR_ADD_TRACK:
        {
            int n = LayersEngine::Get().GetLayerCount();
            if (s_selLayer < 0 || s_selLayer >= n) break;
            LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);
            int added = 0;
            int numSel = CountSelectedTracks(0);
            for (int i = 0; i < numSel; i++)
            {
                MediaTrack* tr = GetSelectedTrack(0, i);
                if (!tr) continue;
                GUID* tg = GetTrackGUID(tr);
                if (!tg) continue;

                // Avoid duplicates (skip spacer entries in the scan)
                bool dup = false;
                for (const auto& lt : ld.tracks)
                    if (!lt.isSpacer && memcmp(&lt.guid, tg, sizeof(GUID)) == 0) { dup = true; break; }
                if (dup) continue;

                LayerTrack lt;
                lt.guid = *tg;
                char buf[128] = {};
                GetTrackName(tr, buf, (int)sizeof(buf));
                strncpy(lt.name, buf, sizeof(lt.name) - 1);
                ld.tracks.push_back(lt);
                added++;
            }
            if (added)
            {
                LayersEngine::Get().SaveExtState();
                RefreshTrackList(hwnd);
                RefreshLayerList(hwnd);

                char status[64];
                snprintf(status, sizeof(status), "Added %d track(s)", added);
                SetDlgItemText(hwnd, IDC_LYR_STATUS, status);
            }
            break;
        }

        case IDC_LYR_REM_TRACK:
        {
            int sel = GetTrackListSel(hwnd);
            int n   = LayersEngine::Get().GetLayerCount();
            if (sel < 0 || s_selLayer < 0 || s_selLayer >= n) break;
            LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);
            if (sel >= (int)ld.tracks.size()) break;
            ld.tracks.erase(ld.tracks.begin() + sel);
            LayersEngine::Get().SaveExtState();
            RefreshTrackList(hwnd);
            RefreshLayerList(hwnd);
            break;
        }

        case IDC_LYR_CAPTURE:
        {
            int n = LayersEngine::Get().GetLayerCount();
            if (s_selLayer < 0 || s_selLayer >= n) break;
            LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);
            ld.tracks.clear();

            int numTracks = CountTracks(0);
            for (int t = 0; t < numTracks; t++)
            {
                MediaTrack* tr = GetTrack(0, t);
                if (!tr) continue;
                bool vis = true;
                bool* pv = (bool*)GetSetMediaTrackInfo(tr, "B_SHOWINMIXER", nullptr);
                if (pv) vis = *pv;
                if (!vis) continue;

                GUID* tg = GetTrackGUID(tr);
                if (!tg) continue;

                LayerTrack lt;
                lt.guid = *tg;
                char buf[128] = {};
                GetTrackName(tr, buf, (int)sizeof(buf));
                strncpy(lt.name, buf, sizeof(lt.name) - 1);
                ld.tracks.push_back(lt);
            }
            LayersEngine::Get().SaveExtState();
            RefreshTrackList(hwnd);
            RefreshLayerList(hwnd);
            {
                char status[64];
                snprintf(status, sizeof(status), "Captured %d tracks", (int)ld.tracks.size());
                SetDlgItemText(hwnd, IDC_LYR_STATUS, status);
            }
            break;
        }

        case IDC_LYR_CLEAR_LAYER:
        {
            int n = LayersEngine::Get().GetLayerCount();
            if (s_selLayer < 0 || s_selLayer >= n) break;
            if (MessageBoxA(hwnd, "Clear all tracks from this layer?",
                "Layers", MB_YESNO | MB_ICONQUESTION) != IDYES) break;
            LayersEngine::Get().GetLayer(s_selLayer).tracks.clear();
            LayersEngine::Get().SaveExtState();
            RefreshTrackList(hwnd);
            RefreshLayerList(hwnd);
            break;
        }

        case IDCANCEL:
            ShowWindow(hwnd, SW_HIDE);
            return TRUE;
        }
        return TRUE;
    }

    // -----------------------------------------------------------------------
    case WM_CONTEXTMENU:
    {
        // Context menu IDs
        enum {
            CTX_LYR_ACTIVATE   = 3001,
            CTX_LYR_MOVE_UP    = 3002,
            CTX_LYR_MOVE_DOWN  = 3003,
            CTX_LYR_DELETE     = 3004,
            CTX_TRK_MOVE_UP    = 3010,
            CTX_TRK_MOVE_DOWN  = 3011,
            CTX_TRK_SPACER_BEF = 3012,
            CTX_TRK_SPACER_AFT = 3013,
            CTX_TRK_REMOVE     = 3014,
        };

        HWND hCtrl = (HWND)wParam;
        int  sx    = GET_X_LPARAM(lParam);
        int  sy    = GET_Y_LPARAM(lParam);

        HWND hLayerList = GetDlgItem(hwnd, IDC_LYR_LAYER_LIST);
        HWND hTrackList = GetDlgItem(hwnd, IDC_LYR_TRACK_LIST);

        if (hCtrl == hLayerList)
        {
            // Hit-test to find which item was right-clicked
            POINT ptC = { sx, sy };
            if (sx == -1 && sy == -1)
                GetCursorPos(&ptC);
            POINT ptL = ptC;
            ScreenToClient(hLayerList, &ptL);
            LVHITTESTINFO ht = {};
            ht.pt = ptL;
            int item = ListView_HitTest(hLayerList, &ht);

            if (item >= 0)
            {
                // Select the item
                ListView_SetItemState(hLayerList, item,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                s_selLayer = item;
                RefreshTrackList(hwnd);
                LoadLayerToUI(hwnd, s_selLayer);
            }

            int n = LayersEngine::Get().GetLayerCount();
            HMENU hMenu = CreatePopupMenu();
            AppendMenuA(hMenu, MF_STRING | (item < 0 ? MF_GRAYED : 0),
                CTX_LYR_ACTIVATE, "Activate\tDbl-click");
            AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(hMenu, MF_STRING | (item <= 0 ? MF_GRAYED : 0),
                CTX_LYR_MOVE_UP, "Move Up");
            AppendMenuA(hMenu, MF_STRING | (item < 0 || item >= n - 1 ? MF_GRAYED : 0),
                CTX_LYR_MOVE_DOWN, "Move Down");
            AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(hMenu, MF_STRING | (item < 0 ? MF_GRAYED : 0),
                CTX_LYR_DELETE, "Delete");

            int cmd = (int)TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                ptC.x, ptC.y, hwnd, nullptr);
            DestroyMenu(hMenu);

            switch (cmd)
            {
            case CTX_LYR_ACTIVATE:
                if (item >= 0 && item < n)
                {
                    ApplyNameFromUI(hwnd);
                    ApplyMaxChFromUI(hwnd);
                    LayersEngine::Get().ActivateLayer(item);
                    RefreshLayerList(hwnd);
                    UpdateStatus(hwnd);
                }
                break;
            case CTX_LYR_MOVE_UP:
                if (item > 0)
                {
                    LayersEngine::Get().MoveLayer(item, item - 1);
                    s_selLayer = item - 1;
                    RefreshLayerList(hwnd);
                    LoadLayerToUI(hwnd, s_selLayer);
                }
                break;
            case CTX_LYR_MOVE_DOWN:
                if (item >= 0 && item < n - 1)
                {
                    LayersEngine::Get().MoveLayer(item, item + 1);
                    s_selLayer = item + 1;
                    RefreshLayerList(hwnd);
                    LoadLayerToUI(hwnd, s_selLayer);
                }
                break;
            case CTX_LYR_DELETE:
            {
                if (item < 0 || item >= n) break;
                const LayerDef& ld = LayersEngine::Get().GetLayer(item);
                char msg[128];
                snprintf(msg, sizeof(msg), "Delete layer \"%.60s\"?", ld.name);
                if (MessageBoxA(hwnd, msg, "Layers", MB_YESNO | MB_ICONQUESTION) != IDYES) break;
                LayersEngine::Get().RemoveLayer(item);
                s_selLayer = (item > 0) ? item - 1 : 0;
                RefreshLayerList(hwnd);
                RefreshTrackList(hwnd);
                LoadLayerToUI(hwnd, s_selLayer);
                UpdateStatus(hwnd);
                break;
            }
            }
        }
        else if (hCtrl == hTrackList)
        {
            POINT ptC = { sx, sy };
            if (sx == -1 && sy == -1)
                GetCursorPos(&ptC);
            POINT ptL = ptC;
            ScreenToClient(hTrackList, &ptL);
            LVHITTESTINFO ht = {};
            ht.pt = ptL;
            int item = ListView_HitTest(hTrackList, &ht);

            int n = LayersEngine::Get().GetLayerCount();
            int numTracks = (s_selLayer >= 0 && s_selLayer < n)
                ? (int)LayersEngine::Get().GetLayer(s_selLayer).tracks.size() : 0;

            if (item >= 0)
            {
                ListView_SetItemState(hTrackList, item,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }

            HMENU hMenu = CreatePopupMenu();
            AppendMenuA(hMenu, MF_STRING | (item <= 0 ? MF_GRAYED : 0),
                CTX_TRK_MOVE_UP, "Move Up");
            AppendMenuA(hMenu, MF_STRING | (item < 0 || item >= numTracks - 1 ? MF_GRAYED : 0),
                CTX_TRK_MOVE_DOWN, "Move Down");
            AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(hMenu, MF_STRING | (item < 0 ? MF_GRAYED : 0),
                CTX_TRK_SPACER_BEF, "Add Spacer Before");
            AppendMenuA(hMenu, MF_STRING | (item < 0 ? MF_GRAYED : 0),
                CTX_TRK_SPACER_AFT, "Add Spacer After");
            AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(hMenu, MF_STRING | (item < 0 ? MF_GRAYED : 0),
                CTX_TRK_REMOVE, "Remove");

            int cmd = (int)TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                ptC.x, ptC.y, hwnd, nullptr);
            DestroyMenu(hMenu);

            if (s_selLayer < 0 || s_selLayer >= n) break;
            LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);

            switch (cmd)
            {
            case CTX_TRK_MOVE_UP:
                if (item > 0 && item < (int)ld.tracks.size())
                {
                    std::swap(ld.tracks[item], ld.tracks[item - 1]);
                    LayersEngine::Get().SaveExtState();
                    RefreshTrackList(hwnd);
                    ListView_SetItemState(hTrackList, item - 1,
                        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                }
                break;
            case CTX_TRK_MOVE_DOWN:
                if (item >= 0 && item + 1 < (int)ld.tracks.size())
                {
                    std::swap(ld.tracks[item], ld.tracks[item + 1]);
                    LayersEngine::Get().SaveExtState();
                    RefreshTrackList(hwnd);
                    ListView_SetItemState(hTrackList, item + 1,
                        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                }
                break;
            case CTX_TRK_SPACER_BEF:
                if (item >= 0 && item <= (int)ld.tracks.size())
                {
                    LayerTrack sp;
                    sp.isSpacer = true;
                    ld.tracks.insert(ld.tracks.begin() + item, sp);
                    LayersEngine::Get().SaveExtState();
                    RefreshTrackList(hwnd);
                    RefreshLayerList(hwnd);
                    ListView_SetItemState(hTrackList, item,
                        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                }
                break;
            case CTX_TRK_SPACER_AFT:
                if (item >= 0)
                {
                    int insertAt = item + 1;
                    LayerTrack sp;
                    sp.isSpacer = true;
                    ld.tracks.insert(ld.tracks.begin() + insertAt, sp);
                    LayersEngine::Get().SaveExtState();
                    RefreshTrackList(hwnd);
                    RefreshLayerList(hwnd);
                    ListView_SetItemState(hTrackList, insertAt,
                        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                }
                break;
            case CTX_TRK_REMOVE:
                if (item >= 0 && item < (int)ld.tracks.size())
                {
                    ld.tracks.erase(ld.tracks.begin() + item);
                    LayersEngine::Get().SaveExtState();
                    RefreshTrackList(hwnd);
                    RefreshLayerList(hwnd);
                }
                break;
            }
        }
        break;
    }

    // -----------------------------------------------------------------------
    case WM_NOTIFY:
    {
        NMHDR* hdr = (NMHDR*)lParam;

        // ---- Layer list notifications ------------------------------------
        if (hdr->idFrom == IDC_LYR_LAYER_LIST)
        {
            if (hdr->code == LVN_ITEMCHANGED)
            {
                NMLISTVIEW* nlv = (NMLISTVIEW*)lParam;
                if ((nlv->uNewState & LVIS_SELECTED) && nlv->iItem >= 0)
                {
                    s_selLayer = nlv->iItem;
                    RefreshTrackList(hwnd);
                    LoadLayerToUI(hwnd, s_selLayer);
                }
            }
            else if (hdr->code == NM_DBLCLK)
            {
                NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
                if (nia->iItem >= 0 && nia->iItem < LayersEngine::Get().GetLayerCount())
                {
                    s_selLayer = nia->iItem;
                    ApplyNameFromUI(hwnd);
                    ApplyMaxChFromUI(hwnd);
                    LayersEngine::Get().ActivateLayer(s_selLayer);
                    RefreshLayerList(hwnd);
                    UpdateStatus(hwnd);
                }
            }
        }

        // ---- Track list notifications ------------------------------------
        else if (hdr->idFrom == IDC_LYR_TRACK_LIST)
        {
        }

        return TRUE;
    }

    // -----------------------------------------------------------------------
    case WM_SIZE:
    {
        // Stretch layer list and track list to fill available height
        RECT rc;
        GetClientRect(hwnd, &rc);

        // We rely on the dialog's resizeable frame; no dynamic re-layout needed
        // for now – keeping it simple.
        (void)rc;
        break;
    }

    // -----------------------------------------------------------------------
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return TRUE;

    case WM_DESTROY:
        s_hwnd = nullptr;
        return TRUE;
    }

    return FALSE;
}
