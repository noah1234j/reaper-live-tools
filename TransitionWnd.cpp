#include "TransitionWnd.h"
#include "TransitionEngine.h"
#include "SafesWnd.h"
#include "api.h"
#include "resource.h"

#include <commctrl.h>
#include <windowsx.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <memory>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
std::vector<std::unique_ptr<TransitionSnapshot>> g_snapshots;

static HWND      g_wnd        = nullptr;
static HINSTANCE g_hInstance  = nullptr;
// Clipboard for copy/paste
static std::unique_ptr<TransitionSnapshot> g_clipboard;

// Guard: set true when programmatically updating editor fields to prevent
// EN_CHANGE / CBN_SELCHANGE from writing back to the snapshot.
static bool g_syncingEditor = false;

// Guard: when true, WM_DESTROY skips overwriting the dock-state pref (used by ToggleDocking)
static bool g_suppressDockStateSave = false;

// UI timer ID
static const UINT UI_TIMER_ID = 1;

// Context menu item IDs
enum { CTX_RENAME = 100, CTX_OVERWRITE, CTX_DELETE };

// Docker context menu IDs
enum { CTX_DOCK = 200, CTX_CLOSE };

// ---------------------------------------------------------------------------
// Layout / resize state
// ---------------------------------------------------------------------------
struct SidebarCtrl { HWND hwnd; int origLeft; int origTop; int w; int h; };
static std::vector<SidebarCtrl> g_sidebarCtrls;
static int  g_initCx = 0, g_initCy = 0;
static RECT g_listInitRect = {};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void ToggleDocking();
static void RefreshListView(HWND hwnd);
static void DoRecall(HWND hwnd, int index);
static void DoSave(HWND hwnd);
static void ShowContextMenu(HWND hwnd, int item, POINT pt);
static void LoadEditorFromSnapshot(HWND hwnd, const TransitionSnapshot* snap);
static TransitionSnapshot* GetSelectedSnapshot(HWND hwnd);
static int  GetSelectedListIndex(HWND hwnd);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void TransitionWnd_Init(HINSTANCE hInstance)
{
    g_hInstance = hInstance;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
}

void TransitionWnd_Cleanup()
{
    if (g_wnd && IsWindow(g_wnd))
    {
        bool isFloat = false;
        if (DockIsChildOfDock(g_wnd, &isFloat) >= 0)
            DockWindowRemove(g_wnd);
        DestroyWindow(g_wnd);
        g_wnd = nullptr;
    }
}

void TransitionWnd_ShowHide()
{
    if (!g_wnd || !IsWindow(g_wnd))
    {
        HWND hMain = GetMainHwnd();
        g_wnd = CreateDialogParam(g_hInstance,
                                  MAKEINTRESOURCE(IDD_TSNAPS),
                                  hMain,
                                  DialogProc,
                                  0);
        if (!g_wnd)
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "CreateDialogParam failed. Error=%lu", GetLastError());
            MessageBoxA(hMain, buf, "Live Tools", MB_OK | MB_ICONERROR);
            return;
        }
        // Always register with REAPER's docker so drag-to-edge works even when floating.
        // allowShow=true  → immediately reparent into the docker (docked mode)
        // allowShow=false → registered as dockable but stays as a regular float window
        const char* dockPref = GetExtState("reaper_transitions", "scenes_docked");
        bool wantDocked = (dockPref && atoi(dockPref) != 0);
        if (wantDocked)
        {
            DockWindowAddEx(g_wnd, "Scenes", "reaper_trans_scenes", true);
            DockWindowActivate(g_wnd);
        }
        else
        {
            ShowWindow(g_wnd, SW_SHOW);
        }
        return;
    }

    // Window already exists – activate if docked, otherwise toggle visibility
    bool isFloat = false;
    if (DockIsChildOfDock(g_wnd, &isFloat) >= 0)
    {
        DockWindowActivate(g_wnd);
    }
    else
    {
        if (IsWindowVisible(g_wnd))
            ShowWindow(g_wnd, SW_HIDE);
        else
            ShowWindow(g_wnd, SW_SHOW);
    }
}

// Destroy the window and recreate it with the opposite dock state (mirrors SWS ToggleDocking)
static void ToggleDocking()
{
    bool isFloat = false;
    bool wasDocked = (DockIsChildOfDock(g_wnd, &isFloat) >= 0);
    bool newDocked = !wasDocked;
    SetExtState("reaper_transitions", "scenes_docked", newDocked ? "1" : "0", true);
    g_suppressDockStateSave = true;  // prevent WM_DESTROY from overwriting the new pref
    DestroyWindow(g_wnd);
    g_suppressDockStateSave = false;
    // g_wnd is cleared by WM_DESTROY handler; recreate via ShowHide
    TransitionWnd_ShowHide();
}

int TransitionWnd_IsVisible()
{
    if (!g_wnd || !IsWindow(g_wnd)) return 0;
    bool isFloat = false;
    if (DockIsChildOfDock(g_wnd, &isFloat) >= 0) return 1;
    return IsWindowVisible(g_wnd) ? 1 : 0;
}

int TransitionWnd_GetSelectedIndex()
{
    if (!g_wnd || !IsWindow(g_wnd)) return -1;
    return GetSelectedListIndex(g_wnd);
}

void TransitionWnd_RefreshList()
{
    if (g_wnd && IsWindow(g_wnd))
        RefreshListView(g_wnd);
}

void TransitionWnd_RecallScene(int index)
{
    if (index < 0 || index >= (int)g_snapshots.size()) return;
    const TransitionSnapshot* snap = g_snapshots[index].get();
    bool instant = false;
    if (g_wnd && IsWindow(g_wnd))
        instant = (IsDlgButtonChecked(g_wnd, IDC_INSTANT) == BST_CHECKED);
    double duration = instant ? 0.0 : snap->m_duration;
    if (g_wnd && IsWindow(g_wnd) && IsDlgButtonChecked(g_wnd, IDC_MARKER_BTN) == BST_CHECKED)
    {
        double pos = GetPlayPosition();
        AddProjectMarker2(nullptr, false, pos, 0.0, snap->m_name.c_str(), -1, 0);
    }
    TransitionEngine::Get().Recall(snap, snap->m_mask, duration);
    TransitionEngine::Get().SetCurrentSlot(index);
    if (g_wnd && IsWindow(g_wnd))
    {
        HWND hList = GetDlgItem(g_wnd, IDC_LIST);
        ListView_SetItemState(hList, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hList, index, FALSE);
    }
    Undo_OnStateChangeEx("Recall Scene", -1, -1);
}

void TransitionWnd_OverwriteScene(int index)
{
    if (index < 0 || index >= (int)g_snapshots.size()) return;
    g_snapshots[index]->Capture(TS_CAPTURE_ALL);
    if (g_wnd && IsWindow(g_wnd))
        RefreshListView(g_wnd);
    Undo_OnStateChangeEx("Save Scene", -1, -1);
}

// ---------------------------------------------------------------------------
// GetSelectedListIndex
// ---------------------------------------------------------------------------
static int GetSelectedListIndex(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LIST);
    if (!hList) return -1;
    return ListView_GetNextItem(hList, -1, LVNI_SELECTED);
}

// ---------------------------------------------------------------------------
// GetSelectedSnapshot
// ---------------------------------------------------------------------------
static TransitionSnapshot* GetSelectedSnapshot(HWND hwnd)
{
    int idx = GetSelectedListIndex(hwnd);
    if (idx < 0 || idx >= (int)g_snapshots.size()) return nullptr;
    return g_snapshots[idx].get();
}

// ---------------------------------------------------------------------------
// LoadEditorFromSnapshot – fill right-panel controls from a snapshot
// ---------------------------------------------------------------------------
static void LoadEditorFromSnapshot(HWND hwnd, const TransitionSnapshot* snap)
{
    g_syncingEditor = true;

    if (!snap)
    {
        SetDlgItemText(hwnd, IDC_SNAPNAME,     "");
        SetDlgItemText(hwnd, IDC_SNAPNOTES,    "");
        SetDlgItemText(hwnd, IDC_DURATION,     "2.00");
        SetDlgItemText(hwnd, IDC_TAPER_CUSTOM, "2.00");
        SendDlgItemMessage(hwnd, IDC_TAPER, CB_SETCURSEL, TAPER_SCURVE, 0);
        EnableWindow(GetDlgItem(hwnd, IDC_TAPER_CUSTOM), FALSE);
    }
    else
    {
        SetDlgItemText(hwnd, IDC_SNAPNAME,  snap->m_name.c_str());
        SetDlgItemText(hwnd, IDC_SNAPNOTES, snap->m_notes.c_str());

        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f", snap->m_duration);
        SetDlgItemText(hwnd, IDC_DURATION, buf);

        int taperSel = snap->m_taper;
        if (taperSel < 0 || taperSel > TAPER_CUSTOM) taperSel = TAPER_SCURVE;
        SendDlgItemMessage(hwnd, IDC_TAPER, CB_SETCURSEL, taperSel, 0);

        snprintf(buf, sizeof(buf), "%.2f", snap->m_taperExp);
        SetDlgItemText(hwnd, IDC_TAPER_CUSTOM, buf);
        EnableWindow(GetDlgItem(hwnd, IDC_TAPER_CUSTOM), snap->m_taper == TAPER_CUSTOM);
    }

    bool instant = (IsDlgButtonChecked(hwnd, IDC_INSTANT) == BST_CHECKED);
    EnableWindow(GetDlgItem(hwnd, IDC_DURATION), !instant);
    EnableWindow(GetDlgItem(hwnd, IDC_TAPER),    !instant);
    if (instant) EnableWindow(GetDlgItem(hwnd, IDC_TAPER_CUSTOM), FALSE);

    g_syncingEditor = false;
}

// ---------------------------------------------------------------------------
// RefreshListView – rebuild all rows from g_snapshots
// ---------------------------------------------------------------------------
static void RefreshListView(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LIST);
    if (!hList) return;

    int selBefore = GetSelectedListIndex(hwnd);

    ListView_DeleteAllItems(hList);

    for (int i = 0; i < (int)g_snapshots.size(); i++)
    {
        const auto& ss = g_snapshots[i];

        LVITEM lvi = {};
        lvi.mask    = LVIF_TEXT;
        lvi.iItem   = i;
        char slotBuf[16];
        snprintf(slotBuf, sizeof(slotBuf), "%d", ss->m_slot + 1);
        lvi.pszText = slotBuf;
        ListView_InsertItem(hList, &lvi);

        ListView_SetItemText(hList, i, 1, const_cast<char*>(ss->m_name.c_str()));

        char timeBuf[32] = "";
        if (ss->m_time)
        {
            struct tm* lt = localtime((const time_t*)&ss->m_time);
            if (lt) strftime(timeBuf, sizeof(timeBuf), "%m/%d %H:%M", lt);
        }
        ListView_SetItemText(hList, i, 2, timeBuf);
    }

    if (selBefore >= 0 && selBefore < (int)g_snapshots.size())
    {
        ListView_SetItemState(hList, selBefore,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hList, selBefore, FALSE);
    }
}

// ---------------------------------------------------------------------------
// DoSave – capture and add new snapshot
// ---------------------------------------------------------------------------
static void DoSave(HWND hwnd)
{
    char name[256] = {};
    GetDlgItemText(hwnd, IDC_SNAPNAME, name, sizeof(name));
    // If the field is empty, OR contains an existing snapshot's name (meaning
    // the user didn't retype it), auto-generate a new incremented name.
    bool nameIsNew = (name[0] != '\0');
    if (nameIsNew) {
        for (auto& s : g_snapshots)
            if (s->m_name == name) { nameIsNew = false; break; }
    }
    if (!nameIsNew) {
        int maxN = (int)g_snapshots.size();
        for (auto& s : g_snapshots) {
            int n = 0;
            if (sscanf(s->m_name.c_str(), "Scene %d", &n) == 1 && n > maxN)
                maxN = n;
        }
        snprintf(name, sizeof(name), "Scene %d", maxN + 1);
    }

    char durBuf[64] = "2.0";
    GetDlgItemText(hwnd, IDC_DURATION, durBuf, sizeof(durBuf));
    double dur = atof(durBuf);
    if (dur <= 0.0) dur = 2.0;

    int taper = (int)SendDlgItemMessage(hwnd, IDC_TAPER, CB_GETCURSEL, 0, 0);
    if (taper < 0 || taper > TAPER_CUSTOM) taper = TAPER_SCURVE;

    char expBuf[64] = "2.0";
    GetDlgItemText(hwnd, IDC_TAPER_CUSTOM, expBuf, sizeof(expBuf));
    double taperExp = atof(expBuf);
    if (taperExp <= 0.0) taperExp = 2.0;

    int slot = (int)g_snapshots.size();
    auto ss  = std::make_unique<TransitionSnapshot>(slot, name);
    ss->m_duration = dur;
    ss->m_taper    = taper;
    ss->m_taperExp = taperExp;
    ss->Capture(TS_CAPTURE_ALL);
    ss->m_slot = slot;

    g_snapshots.push_back(std::move(ss));
    RefreshListView(hwnd);

    HWND hList = GetDlgItem(hwnd, IDC_LIST);
    int  newIdx = (int)g_snapshots.size() - 1;
    ListView_SetItemState(hList, newIdx,
        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(hList, newIdx, FALSE);
    LoadEditorFromSnapshot(hwnd, g_snapshots[newIdx].get());

    Undo_OnStateChangeEx("Save Scene", -1, -1);
}

// ---------------------------------------------------------------------------
// DoRecall – run engine on selected snapshot
// ---------------------------------------------------------------------------
static void DoRecall(HWND hwnd, int index)
{
    if (index < 0 || index >= (int)g_snapshots.size()) return;

    const TransitionSnapshot* snap = g_snapshots[index].get();
    bool   instant  = (IsDlgButtonChecked(hwnd, IDC_INSTANT) == BST_CHECKED);
    double duration = instant ? 0.0 : snap->m_duration;

    // Place a named marker at the play cursor position if option is enabled
    if (IsDlgButtonChecked(hwnd, IDC_MARKER_BTN) == BST_CHECKED)
    {
        double pos = GetPlayPosition();
        AddProjectMarker2(nullptr, false, pos, 0.0, snap->m_name.c_str(), -1, 0);
    }

    TransitionEngine::Get().Recall(snap, snap->m_mask, duration);
    TransitionEngine::Get().SetCurrentSlot(index);
    Undo_OnStateChangeEx("Recall Scene", -1, -1);
}

// ---------------------------------------------------------------------------
// ShowContextMenu
// ---------------------------------------------------------------------------
static void ShowContextMenu(HWND hwnd, int item, POINT pt)
{
    if (item < 0) return;

    HMENU hMenu = CreatePopupMenu();
    AppendMenu(hMenu, MF_STRING,    CTX_RENAME,    "Rename");
    AppendMenu(hMenu, MF_STRING,    CTX_OVERWRITE, "Overwrite (re-capture)");
    AppendMenu(hMenu, MF_SEPARATOR, 0,             nullptr);
    AppendMenu(hMenu, MF_STRING,    CTX_DELETE,    "Delete");

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                              pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);

    if (cmd == CTX_RENAME && item >= 0 && item < (int)g_snapshots.size())
    {
        ListView_EditLabel(GetDlgItem(hwnd, IDC_LIST), item);
    }
    else if (cmd == CTX_OVERWRITE && item >= 0 && item < (int)g_snapshots.size())
    {
        g_snapshots[item]->Capture(TS_CAPTURE_ALL);
        RefreshListView(hwnd);
        Undo_OnStateChangeEx("Overwrite Scene", -1, -1);
    }
    else if (cmd == CTX_DELETE && item >= 0 && item < (int)g_snapshots.size())
    {
        g_snapshots.erase(g_snapshots.begin() + item);
        for (int i = 0; i < (int)g_snapshots.size(); i++)
            g_snapshots[i]->m_slot = i;
        RefreshListView(hwnd);
        LoadEditorFromSnapshot(hwnd, nullptr);
        Undo_OnStateChangeEx("Delete Scene", -1, -1);
    }
}

// ---------------------------------------------------------------------------
// DialogProc
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS;
        InitCommonControlsEx(&icc);

        // ---- Create ListView dynamically ----------------------------------
        HWND hListPH = GetDlgItem(hwnd, IDC_LIST);
        RECT rList = {};
        GetWindowRect(hListPH, &rList);
        MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&rList, 2);
        DestroyWindow(hListPH);

        HWND hList = CreateWindowExA(0, "SysListView32", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
            LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | LVS_EDITLABELS,
            rList.left, rList.top,
            rList.right - rList.left, rList.bottom - rList.top,
            hwnd, (HMENU)(INT_PTR)IDC_LIST, g_hInstance, nullptr);

        if (hList)
        {
            ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

            LVCOLUMN col = {};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.cx = 28;  col.pszText = const_cast<char*>("#");
            ListView_InsertColumn(hList, 0, &col);
            col.cx = 110; col.pszText = const_cast<char*>("Name");
            ListView_InsertColumn(hList, 1, &col);
            col.cx = 75;  col.pszText = const_cast<char*>("Saved");
            ListView_InsertColumn(hList, 2, &col);
        }

        // ---- Create ProgressBar dynamically --------------------------------
        HWND hProgPH = GetDlgItem(hwnd, IDC_PROGRESS);
        RECT rProg = {};
        GetWindowRect(hProgPH, &rProg);
        MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&rProg, 2);
        DestroyWindow(hProgPH);

        HWND hProg = CreateWindowExA(0, "msctls_progressbar32", "",
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            rProg.left, rProg.top,
            rProg.right - rProg.left, rProg.bottom - rProg.top,
            hwnd, (HMENU)(INT_PTR)IDC_PROGRESS, g_hInstance, nullptr);
        if (hProg)
        {
            SendMessage(hProg, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            SendMessage(hProg, PBM_SETPOS,   0, 0);
        }

        // ---- Populate taper combobox -------------------------------------
        const char* taperItems[] = {
            "Linear", "S-Curve", "Logarithmic", "Exponential", "Custom..."
        };
        for (const char* n : taperItems)
            SendDlgItemMessage(hwnd, IDC_TAPER, CB_ADDSTRING, 0, (LPARAM)n);
        SendDlgItemMessage(hwnd, IDC_TAPER, CB_SETCURSEL, TAPER_SCURVE, 0);

        // ---- Initial editor state ----------------------------------------
        LoadEditorFromSnapshot(hwnd, nullptr);

        // ---- Populate from already-loaded snapshots ----------------------
        RefreshListView(hwnd);

        // ---- Engine completion callback ----------------------------------
        TransitionEngine::Get().onTransitionComplete = [hwnd]() {
            PostMessage(hwnd, WM_USER + 1, 0, 0);
        };

        SetTimer(hwnd, UI_TIMER_ID, 100, nullptr);

        // ---- Record initial layout for WM_SIZE --------------------------
        {
            RECT cr;
            GetClientRect(hwnd, &cr);
            g_initCx = cr.right;
            g_initCy = cr.bottom;

            HWND hListSz = GetDlgItem(hwnd, IDC_LIST);
            if (hListSz)
            {
                GetWindowRect(hListSz, &g_listInitRect);
                MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&g_listInitRect, 2);
            }

            int threshold = g_listInitRect.right - 5;
            g_sidebarCtrls.clear();
            HWND hChild = GetWindow(hwnd, GW_CHILD);
            while (hChild)
            {
                RECT r;
                GetWindowRect(hChild, &r);
                MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&r, 2);
                if (r.left > threshold)
                {
                    SidebarCtrl sc;
                    sc.hwnd     = hChild;
                    sc.origLeft = r.left;
                    sc.origTop  = r.top;
                    sc.w        = r.right  - r.left;
                    sc.h        = r.bottom - r.top;
                    g_sidebarCtrls.push_back(sc);
                }
                hChild = GetWindow(hChild, GW_HWNDNEXT);
            }
        }
        return TRUE;
    }

    case WM_GETMINMAXINFO:
    {
        if (g_initCx > 0)
        {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            RECT r = { 0, 0, g_initCx, g_initCy };
            AdjustWindowRectEx(&r,
                (DWORD)GetWindowLong(hwnd, GWL_STYLE),
                FALSE,
                (DWORD)GetWindowLong(hwnd, GWL_EXSTYLE));
            mmi->ptMinTrackSize.x = r.right  - r.left;
            mmi->ptMinTrackSize.y = r.bottom - r.top;
        }
        return 0;
    }

    case WM_SIZE:
    {
        int newCx = (int)(short)LOWORD(lParam);
        int newCy = (int)(short)HIWORD(lParam);
        if (g_initCx <= 0 || newCx <= 0 || newCy <= 0) break;

        int dx = newCx - g_initCx;
        int dy = newCy - g_initCy;

        // Resize the ListView to fill extra width and height
        HWND hListSz = GetDlgItem(hwnd, IDC_LIST);
        if (hListSz && g_listInitRect.right > g_listInitRect.left)
        {
            int newW = (g_listInitRect.right  - g_listInitRect.left) + dx;
            int newH = (g_listInitRect.bottom - g_listInitRect.top)  + dy;
            if (newW > 10 && newH > 10)
            {
                SetWindowPos(hListSz, nullptr,
                    g_listInitRect.left, g_listInitRect.top, newW, newH,
                    SWP_NOZORDER | SWP_NOACTIVATE);

                // Stretch "Name" column to fill available list width
                int col0W = ListView_GetColumnWidth(hListSz, 0);
                int col2W = ListView_GetColumnWidth(hListSz, 2);
                int col1W = newW - col0W - col2W
                            - GetSystemMetrics(SM_CXVSCROLL) - 4;
                if (col1W > 20)
                    ListView_SetColumnWidth(hListSz, 1, col1W);
            }
        }

        // Shift right-sidebar controls by dx (keep same y, w, h)
        if (!g_sidebarCtrls.empty())
        {
            HDWP hdwp = BeginDeferWindowPos((int)g_sidebarCtrls.size());
            for (const auto& sc : g_sidebarCtrls)
            {
                hdwp = DeferWindowPos(hdwp, sc.hwnd, nullptr,
                    sc.origLeft + dx, sc.origTop, sc.w, sc.h,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            EndDeferWindowPos(hdwp);
        }

        InvalidateRect(hwnd, nullptr, TRUE);
        break;
    }

    case WM_TIMER:
        if (wParam == UI_TIMER_ID)
        {
            TransitionEngine& eng = TransitionEngine::Get();

            HWND hProg = GetDlgItem(hwnd, IDC_PROGRESS);
            int  pct   = (int)(eng.GetProgress() * 100.0 + 0.5);
            if (pct > 100) pct = 100;
            SendMessage(hProg, PBM_SETPOS, (WPARAM)pct, 0);

            SetDlgItemText(hwnd, IDC_STATUS, eng.GetStatus());
        }
        return TRUE;

    case WM_USER + 1:
        {
            HWND hProg = GetDlgItem(hwnd, IDC_PROGRESS);
            SendMessage(hProg, PBM_SETPOS, 100, 0);
            SetDlgItemText(hwnd, IDC_STATUS, TransitionEngine::Get().GetStatus());
        }
        return TRUE;

    case WM_COMMAND:
    {
        int id  = LOWORD(wParam);
        int evt = HIWORD(wParam);

        // ---- Snapshot editor live-update handlers -----------------------
        if (id == IDC_SNAPNAME && evt == EN_CHANGE && !g_syncingEditor)
        {
            int idx = GetSelectedListIndex(hwnd);
            if (idx >= 0 && idx < (int)g_snapshots.size())
            {
                char buf[256] = {};
                GetDlgItemText(hwnd, IDC_SNAPNAME, buf, sizeof(buf));
                g_snapshots[idx]->m_name = buf;
                // Update name column inline (no full refresh)
                ListView_SetItemText(GetDlgItem(hwnd, IDC_LIST), idx, 1, buf);
            }
            return TRUE;
        }

        if (id == IDC_SNAPNOTES && evt == EN_CHANGE && !g_syncingEditor)
        {
            int idx = GetSelectedListIndex(hwnd);
            if (idx >= 0 && idx < (int)g_snapshots.size())
            {
                char buf[4096] = {};
                GetDlgItemText(hwnd, IDC_SNAPNOTES, buf, sizeof(buf));
                g_snapshots[idx]->m_notes = buf;
            }
            return TRUE;
        }

        if (id == IDC_DURATION && evt == EN_CHANGE && !g_syncingEditor)
        {
            int idx = GetSelectedListIndex(hwnd);
            if (idx >= 0 && idx < (int)g_snapshots.size())
            {
                char buf[64] = {};
                GetDlgItemText(hwnd, IDC_DURATION, buf, sizeof(buf));
                double d = atof(buf);
                if (d > 0.0) g_snapshots[idx]->m_duration = d;
            }
            return TRUE;
        }

        if (id == IDC_TAPER && evt == CBN_SELCHANGE && !g_syncingEditor)
        {
            int sel = (int)SendDlgItemMessage(hwnd, IDC_TAPER, CB_GETCURSEL, 0, 0);
            int idx = GetSelectedListIndex(hwnd);
            if (idx >= 0 && idx < (int)g_snapshots.size())
                g_snapshots[idx]->m_taper = sel;
            bool instant = (IsDlgButtonChecked(hwnd, IDC_INSTANT) == BST_CHECKED);
            EnableWindow(GetDlgItem(hwnd, IDC_TAPER_CUSTOM), (sel == TAPER_CUSTOM) && !instant);
            return TRUE;
        }

        if (id == IDC_TAPER_CUSTOM && evt == EN_CHANGE && !g_syncingEditor)
        {
            int idx = GetSelectedListIndex(hwnd);
            if (idx >= 0 && idx < (int)g_snapshots.size())
            {
                char buf[64] = {};
                GetDlgItemText(hwnd, IDC_TAPER_CUSTOM, buf, sizeof(buf));
                double ex = atof(buf);
                if (ex > 0.0) g_snapshots[idx]->m_taperExp = ex;
            }
            return TRUE;
        }

        // ---- Button / checkbox handlers ----------------------------------
        switch (id)
        {
        case IDC_SAVE:
            DoSave(hwnd);
            break;

        case IDC_RECALL:
            DoRecall(hwnd, GetSelectedListIndex(hwnd));
            break;

        case IDC_COPY_SNAP:
        {
            int sel = GetSelectedListIndex(hwnd);
            if (sel >= 0 && sel < (int)g_snapshots.size())
                g_clipboard = std::make_unique<TransitionSnapshot>(*g_snapshots[sel]);
            break;
        }

        case IDC_PASTE_SNAP:
        {
            if (!g_clipboard) break;
            int newSlot = (int)g_snapshots.size();
            auto copy   = std::make_unique<TransitionSnapshot>(*g_clipboard);
            copy->m_slot = newSlot;
            if (copy->m_name.find(" (copy)") == std::string::npos)
                copy->m_name += " (copy)";
            copy->m_time = (int)std::time(nullptr);
            g_snapshots.push_back(std::move(copy));
            RefreshListView(hwnd);
            HWND hList = GetDlgItem(hwnd, IDC_LIST);
            ListView_SetItemState(hList, newSlot,
                LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(hList, newSlot, FALSE);
            LoadEditorFromSnapshot(hwnd, g_snapshots[newSlot].get());
            Undo_OnStateChangeEx("Paste Scene", -1, -1);
            break;
        }

        case IDC_OVERWRITE_BTN:
        {
            int sel = GetSelectedListIndex(hwnd);
            if (sel < 0 || sel >= (int)g_snapshots.size()) break;
            g_snapshots[sel]->Capture(TS_CAPTURE_ALL);
            g_snapshots[sel]->m_time = (int)std::time(nullptr);
            RefreshListView(hwnd);
            Undo_OnStateChangeEx("Overwrite Scene", -1, -1);
            break;
        }

        case IDC_DELETE_BTN:
        {
            int sel = GetSelectedListIndex(hwnd);
            if (sel < 0 || sel >= (int)g_snapshots.size()) break;
            g_snapshots.erase(g_snapshots.begin() + sel);
            for (int i = 0; i < (int)g_snapshots.size(); i++)
                g_snapshots[i]->m_slot = i;
            RefreshListView(hwnd);
            LoadEditorFromSnapshot(hwnd, nullptr);
            Undo_OnStateChangeEx("Delete Scene", -1, -1);
            break;
        }

        case IDC_INSTANT:
        {
            bool instant = (IsDlgButtonChecked(hwnd, IDC_INSTANT) == BST_CHECKED);
            EnableWindow(GetDlgItem(hwnd, IDC_DURATION), !instant);
            EnableWindow(GetDlgItem(hwnd, IDC_TAPER),    !instant);
            int taperSel = (int)SendDlgItemMessage(hwnd, IDC_TAPER, CB_GETCURSEL, 0, 0);
            EnableWindow(GetDlgItem(hwnd, IDC_TAPER_CUSTOM),
                         !instant && (taperSel == TAPER_CUSTOM));
            break;
        }

        case IDC_PREVIOUS:
        {
            int cur = TransitionEngine::Get().GetCurrentSlot();
            if (cur > 0)
            {
                cur--;
                HWND hList = GetDlgItem(hwnd, IDC_LIST);
                ListView_SetItemState(hList, cur,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(hList, cur, FALSE);
                LoadEditorFromSnapshot(hwnd, g_snapshots[cur].get());
                DoRecall(hwnd, cur);
            }
            break;
        }

        case IDC_NEXT:
        {
            int cur = TransitionEngine::Get().GetCurrentSlot();
            if (cur + 1 < (int)g_snapshots.size())
            {
                cur++;
                HWND hList = GetDlgItem(hwnd, IDC_LIST);
                ListView_SetItemState(hList, cur,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(hList, cur, FALSE);
                LoadEditorFromSnapshot(hwnd, g_snapshots[cur].get());
                DoRecall(hwnd, cur);
            }
            break;
        }

        case IDC_SWAP_UP:
        {
            int sel = GetSelectedListIndex(hwnd);
            if (sel > 0)
            {
                std::swap(g_snapshots[sel], g_snapshots[sel - 1]);
                g_snapshots[sel - 1]->m_slot = sel - 1;
                g_snapshots[sel    ]->m_slot = sel;
                RefreshListView(hwnd);
                HWND hList = GetDlgItem(hwnd, IDC_LIST);
                ListView_SetItemState(hList, sel - 1,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                LoadEditorFromSnapshot(hwnd, g_snapshots[sel - 1].get());
            }
            break;
        }

        case IDC_SWAP_DOWN:
        {
            int sel = GetSelectedListIndex(hwnd);
            if (sel >= 0 && sel + 1 < (int)g_snapshots.size())
            {
                std::swap(g_snapshots[sel], g_snapshots[sel + 1]);
                g_snapshots[sel    ]->m_slot = sel;
                g_snapshots[sel + 1]->m_slot = sel + 1;
                RefreshListView(hwnd);
                HWND hList = GetDlgItem(hwnd, IDC_LIST);
                ListView_SetItemState(hList, sel + 1,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                LoadEditorFromSnapshot(hwnd, g_snapshots[sel + 1].get());
            }
            break;
        }

        case IDC_SAFES_BTN:
            SafesWnd_ShowHide();
            break;

        default:
            (void)evt;
            break;
        }
        return TRUE;
    }

    case WM_NOTIFY:
    {
        NMHDR* hdr = (NMHDR*)lParam;
        if (hdr->idFrom == IDC_LIST)
        {
            if (hdr->code == NM_DBLCLK)
            {
                NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
                if (nia->iItem >= 0) DoRecall(hwnd, nia->iItem);
            }
            else if (hdr->code == NM_RCLICK)
            {
                NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
                POINT pt;
                GetCursorPos(&pt);
                ShowContextMenu(hwnd, nia->iItem, pt);
            }
            else if (hdr->code == LVN_ITEMCHANGED)
            {
                NMLISTVIEW* nlv = (NMLISTVIEW*)lParam;
                if ((nlv->uNewState & LVIS_SELECTED) && nlv->iItem >= 0 &&
                    nlv->iItem < (int)g_snapshots.size())
                {
                    LoadEditorFromSnapshot(hwnd, g_snapshots[nlv->iItem].get());
                }
            }
            else if (hdr->code == LVN_ENDLABELEDIT)
            {
                NMLVDISPINFO* di = (NMLVDISPINFO*)lParam;
                if (di->item.pszText && di->item.iItem >= 0 &&
                    di->item.iItem < (int)g_snapshots.size())
                {
                    g_snapshots[di->item.iItem]->m_name = di->item.pszText;
                    g_syncingEditor = true;
                    SetDlgItemText(hwnd, IDC_SNAPNAME, di->item.pszText);
                    g_syncingEditor = false;
                    SetWindowLongPtr(hwnd, DWLP_MSGRESULT, TRUE);
                    RefreshListView(hwnd);
                }
                return TRUE;
            }
        }
        return FALSE;
    }

    case WM_CONTEXTMENU:
    {
        // Right-click on title bar or window – show "Dock Scenes in Docker" / "Close window"
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        if (x == -1 || y == -1)
        {
            RECT r;
            GetWindowRect(hwnd, &r);
            x = r.left; y = r.top;
        }
        HMENU hMenu = CreatePopupMenu();
        bool isFloat = false;
        bool docked  = (DockIsChildOfDock(hwnd, &isFloat) >= 0);
        AppendMenuA(hMenu, MF_STRING | (docked ? MF_CHECKED : 0), CTX_DOCK,  "Dock Scenes in Docker");
        AppendMenuA(hMenu, MF_STRING, CTX_CLOSE, "Close window");
        int id = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, hwnd, nullptr);
        DestroyMenu(hMenu);
        if (id == CTX_DOCK)
            ToggleDocking();
        else if (id == CTX_CLOSE)
            ShowWindow(hwnd, SW_HIDE);
        return TRUE;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return TRUE;

    case WM_DESTROY:
        KillTimer(hwnd, UI_TIMER_ID);
        if (!g_suppressDockStateSave)
        {
            bool isFloat = false;
            bool wasDocked = (DockIsChildOfDock(hwnd, &isFloat) >= 0);
            SetExtState("reaper_transitions", "scenes_docked", wasDocked ? "1" : "0", true);
            if (wasDocked)
                DockWindowRemove(hwnd);
        }
        else if (DockIsChildOfDock(hwnd, nullptr) >= 0)
        {
            DockWindowRemove(hwnd);
        }
        TransitionEngine::Get().onTransitionComplete = nullptr;
        g_wnd = nullptr;
        return TRUE;

    default:
        break;
    }

    return FALSE;
}
