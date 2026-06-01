// ---------------------------------------------------------------------------
// MuteGroupsWnd.cpp  –  Mute Groups window
//
// Two-panel layout:
//   Left  – list of all mute groups (with "Muted" indicator column)
//   Right – tracks in the selected group
//
// Per-group keyboard actions are registered by MuteGroupsEngine; the window
// only calls ToggleGroup / AddGroup / RemoveGroup / rename helpers on the
// engine and then refreshes both lists.
// ---------------------------------------------------------------------------
#include "MuteGroupsWnd.h"
#include "MuteGroup.h"
#include "api.h"
#include "resource.h"

#include <commctrl.h>
#include <windowsx.h>
#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------
static HINSTANCE g_hInst  = nullptr;
static HWND      g_hwnd   = nullptr;
static HWND      g_hGroupList = nullptr;
static HWND      g_hTrackList = nullptr;

// When true, LVN_ITEMCHANGED from the groups list should not cascade
// (avoids redundant track-list refreshes during full redraws)
static bool g_refreshing = false;

// ---------------------------------------------------------------------------
// Drag-to-reorder state
// ---------------------------------------------------------------------------
static bool g_dragging      = false;
static int  g_dragSrcIdx    = -1;
static int  g_dragInsertIdx = -1;  // "insert before" position (0..count)

// ---------------------------------------------------------------------------
// Column definitions
// ---------------------------------------------------------------------------
enum GroupCol  { GC_NUM = 0, GC_NAME, GC_MUTED, GC_COUNT };
enum TrackCol  { TC_NAME = 0, TC_COUNT };

static const char* k_groupColName[]  = { "#",  "Name",  "Muted" };
static const int   k_groupColWidth[] = {  28,   100,      45    };

static const char* k_trackColName[]  = { "Track" };
static const int   k_trackColWidth[] = { 155 };

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK MuteGroupsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void RefreshGroupList(HWND hwnd);
static void RefreshTrackList(HWND hwnd);
static int  GetSelectedGroupIndex(HWND hwnd);
static int  GetSelectedTrackIndex(HWND hwnd);
static void SelectGroupRow(HWND hwnd, int idx);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void MuteGroupsWnd_Init(HINSTANCE hInst)
{
    g_hInst = hInst;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);
}

void MuteGroupsWnd_Cleanup()
{
    if (g_hwnd && IsWindow(g_hwnd))
    {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
}

void MuteGroupsWnd_ShowHide()
{
    if (!g_hwnd || !IsWindow(g_hwnd))
    {
        g_hwnd = CreateDialogParam(g_hInst,
                                   MAKEINTRESOURCE(IDD_MUTEGROUPS),
                                   GetMainHwnd(),
                                   MuteGroupsDlgProc,
                                   0);
        if (!g_hwnd)
        {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "CreateDialogParam failed for IDD_MUTEGROUPS. Error=%lu",
                     GetLastError());
            MessageBoxA(GetMainHwnd(), buf, "Live Tools", MB_OK | MB_ICONERROR);
            return;
        }
        ShowWindow(g_hwnd, SW_SHOW);
        return;
    }

    if (IsWindowVisible(g_hwnd))
        ShowWindow(g_hwnd, SW_HIDE);
    else
        ShowWindow(g_hwnd, SW_SHOW);
}

int MuteGroupsWnd_IsVisible()
{
    return (g_hwnd && IsWindow(g_hwnd) && IsWindowVisible(g_hwnd)) ? 1 : 0;
}

void MuteGroupsWnd_Refresh()
{
    if (g_hwnd && IsWindow(g_hwnd))
    {
        RefreshGroupList(g_hwnd);
        RefreshTrackList(g_hwnd);
    }
}

// ---------------------------------------------------------------------------
// RefreshGroupList
// ---------------------------------------------------------------------------
static void RefreshGroupList(HWND hwnd)
{
    if (!g_hGroupList) return;
    g_refreshing = true;

    // Remember selection
    int selIdx = GetSelectedGroupIndex(hwnd);

    ListView_DeleteAllItems(g_hGroupList);

    MuteGroupsEngine& eng = MuteGroupsEngine::Get();
    for (int i = 0; i < eng.GetGroupCount(); ++i)
    {
        const MuteGroup& g = eng.GetGroup(i);

        // Column 0: row number
        char numBuf[12];
        snprintf(numBuf, sizeof(numBuf), "%d", i + 1);
        LVITEMA lvi = {};
        lvi.mask    = LVIF_TEXT;
        lvi.iItem   = i;
        lvi.pszText = numBuf;
        ListView_InsertItem(g_hGroupList, &lvi);

        // Column 1: name
        ListView_SetItemText(g_hGroupList, i, GC_NAME,
                             const_cast<char*>(g.name.c_str()));

        // Column 2: muted indicator
        ListView_SetItemText(g_hGroupList, i, GC_MUTED,
                             const_cast<char*>(eng.IsGroupMuted(i) ? "Yes" : ""));
    }

    // Restore selection
    if (selIdx >= 0 && selIdx < eng.GetGroupCount())
        SelectGroupRow(hwnd, selIdx);
    else if (eng.GetGroupCount() > 0)
        SelectGroupRow(hwnd, 0);

    g_refreshing = false;

    // Always refresh track list to match new selection
    RefreshTrackList(hwnd);
}

// ---------------------------------------------------------------------------
// RefreshTrackList
// ---------------------------------------------------------------------------
static void RefreshTrackList(HWND hwnd)
{
    if (!g_hTrackList) return;
    ListView_DeleteAllItems(g_hTrackList);

    int groupIdx = GetSelectedGroupIndex(hwnd);
    if (groupIdx < 0) return;

    MuteGroupsEngine& eng = MuteGroupsEngine::Get();
    if (groupIdx >= eng.GetGroupCount()) return;

    const MuteGroup& g = eng.GetGroup(groupIdx);
    for (int i = 0; i < (int)g.trackGuids.size(); ++i)
    {
        // Try to find the live track name from the project
        char name[256] = {};
        MediaTrack* tr = nullptr;
        const int n = GetNumTracks();
        for (int j = 0; j < n; ++j)
        {
            MediaTrack* t = GetTrack(nullptr, j);
            if (!t) continue;
            GUID* pg = (GUID*)GetSetMediaTrackInfo(t, "GUID", nullptr);
            if (pg && IsEqualGUID(*pg, g.trackGuids[i]))
            {
                tr = t;
                break;
            }
        }

        if (tr)
        {
            if (!GetTrackName(tr, name, sizeof(name)) || name[0] == '\0')
                snprintf(name, sizeof(name), "Track (unnamed)");
        }
        else
        {
            // Track was deleted from the project but still referenced
            const std::string& gs = MuteGroupsEngine::GuidToStr(g.trackGuids[i]);
            snprintf(name, sizeof(name), "(missing) %s", gs.c_str());
        }

        LVITEMA lvi = {};
        lvi.mask    = LVIF_TEXT;
        lvi.iItem   = i;
        lvi.pszText = name;
        ListView_InsertItem(g_hTrackList, &lvi);
    }
}

// ---------------------------------------------------------------------------
// Selection helpers
// ---------------------------------------------------------------------------
static int GetSelectedGroupIndex(HWND /*hwnd*/)
{
    if (!g_hGroupList) return -1;
    return ListView_GetNextItem(g_hGroupList, -1, LVNI_SELECTED);
}

static int GetSelectedTrackIndex(HWND /*hwnd*/)
{
    if (!g_hTrackList) return -1;
    return ListView_GetNextItem(g_hTrackList, -1, LVNI_SELECTED);
}

static void SelectGroupRow(HWND /*hwnd*/, int idx)
{
    ListView_SetItemState(g_hGroupList, idx,
                          LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g_hGroupList, idx, FALSE);
}

// ---------------------------------------------------------------------------
// Status label helper
// ---------------------------------------------------------------------------
static void SetStatus(HWND hwnd, const char* msg)
{
    SetDlgItemTextA(hwnd, IDC_MG_STATUS, msg);
}

// ---------------------------------------------------------------------------
// Dialog procedure
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK MuteGroupsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    // -----------------------------------------------------------------------
    case WM_INITDIALOG:
    {
        // ---- Groups ListView (left panel) ---------------------------------
        {
            HWND hPh = GetDlgItem(hwnd, IDC_MG_GROUP_LIST);
            RECT rc = {};
            GetClientRect(hPh, &rc);
            MapWindowPoints(hPh, hwnd, (POINT*)&rc, 2);
            DestroyWindow(hPh);

            g_hGroupList = CreateWindowExA(
                WS_EX_CLIENTEDGE,
                WC_LISTVIEWA, "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_EDITLABELS,
                rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                hwnd, (HMENU)(UINT_PTR)IDC_MG_GROUP_LIST, g_hInst, nullptr);

            ListView_SetExtendedListViewStyle(g_hGroupList,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

            // Use the highlight colour for the drag-insert indicator
            ListView_SetInsertMarkColor(g_hGroupList, GetSysColor(COLOR_HIGHLIGHT));

            LVCOLUMNA col = {};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            for (int c = 0; c < GC_COUNT; ++c)
            {
                col.pszText = const_cast<char*>(k_groupColName[c]);
                col.cx      = k_groupColWidth[c];
                col.fmt     = (c == GC_NAME) ? LVCFMT_LEFT : LVCFMT_CENTER;
                ListView_InsertColumn(g_hGroupList, c, &col);
            }
        }

        // ---- Tracks ListView (right panel) --------------------------------
        {
            HWND hPh = GetDlgItem(hwnd, IDC_MG_TRACK_LIST);
            RECT rc = {};
            GetClientRect(hPh, &rc);
            MapWindowPoints(hPh, hwnd, (POINT*)&rc, 2);
            DestroyWindow(hPh);

            g_hTrackList = CreateWindowExA(
                WS_EX_CLIENTEDGE,
                WC_LISTVIEWA, "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                hwnd, (HMENU)(UINT_PTR)IDC_MG_TRACK_LIST, g_hInst, nullptr);

            ListView_SetExtendedListViewStyle(g_hTrackList,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

            LVCOLUMNA col = {};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            for (int c = 0; c < TC_COUNT; ++c)
            {
                col.pszText = const_cast<char*>(k_trackColName[c]);
                col.cx      = k_trackColWidth[c];
                col.fmt     = LVCFMT_LEFT;
                ListView_InsertColumn(g_hTrackList, c, &col);
            }
        }

        RefreshGroupList(hwnd);
        SetStatus(hwnd, "Ready");
        return TRUE;
    }

    // -----------------------------------------------------------------------
    case WM_COMMAND:
    {
        int ctrl = LOWORD(wParam);

        // ---- Add Group ----------------------------------------------------
        if (ctrl == IDC_MG_ADD_GROUP)
        {
            char name[256] = "New Group";
            if (GetUserInputs("Add Mute Group", 1, "Group Name:", name, sizeof(name)))
            {
                MuteGroupsEngine::Get().AddGroup(name);
                RefreshGroupList(hwnd);
                int newIdx = MuteGroupsEngine::Get().GetGroupCount() - 1;
                if (newIdx >= 0) SelectGroupRow(hwnd, newIdx);
                RefreshTrackList(hwnd);
                SetStatus(hwnd, "Group added");
                MarkProjectDirty(nullptr);
            }
            return TRUE;
        }

        // ---- Rename -------------------------------------------------------
        if (ctrl == IDC_MG_RENAME)
        {
            int idx = GetSelectedGroupIndex(hwnd);
            if (idx >= 0 && g_hGroupList)
                ListView_EditLabel(g_hGroupList, idx);
            return TRUE;
        }

        // ---- Delete -------------------------------------------------------
        if (ctrl == IDC_MG_DELETE)
        {
            int idx = GetSelectedGroupIndex(hwnd);
            if (idx < 0) return TRUE;

            const std::string& name = MuteGroupsEngine::Get().GetGroup(idx).name;
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Delete mute group \"%s\"?", name.c_str());
            if (MessageBoxA(hwnd, msg, "Live Tools", MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
                MuteGroupsEngine::Get().RemoveGroup(idx);
                RefreshGroupList(hwnd);
                SetStatus(hwnd, "Group deleted");
                MarkProjectDirty(nullptr);
            }
            return TRUE;
        }

        // ---- Toggle Mute --------------------------------------------------
        if (ctrl == IDC_MG_TOGGLE)
        {
            int idx = GetSelectedGroupIndex(hwnd);
            if (idx < 0) { SetStatus(hwnd, "No group selected"); return TRUE; }

            MuteGroupsEngine::Get().ToggleGroup(idx);
            RefreshGroupList(hwnd);  // updates "Muted" column
            SetStatus(hwnd,
                MuteGroupsEngine::Get().IsGroupMuted(idx) ? "Muted" : "Unmuted");
            return TRUE;
        }

        // ---- Add Selected Tracks ------------------------------------------
        if (ctrl == IDC_MG_ADD_TRACKS)
        {
            int groupIdx = GetSelectedGroupIndex(hwnd);
            if (groupIdx < 0) { SetStatus(hwnd, "No group selected"); return TRUE; }

            MuteGroupsEngine& eng = MuteGroupsEngine::Get();
            int added = 0;
            const int n = GetNumTracks();
            for (int i = 0; i < n; ++i)
            {
                MediaTrack* tr = GetTrack(nullptr, i);
                if (!tr) continue;
                int* ps = (int*)GetSetMediaTrackInfo(tr, "I_SELECTED", nullptr);
                if (!ps || !(*ps)) continue;

                GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
                if (pg)
                {
                    eng.AddTrackToGroup(groupIdx, *pg);
                    ++added;
                }
            }

            RefreshTrackList(hwnd);
            char buf[64];
            snprintf(buf, sizeof(buf), "Added %d track(s)", added);
            SetStatus(hwnd, buf);
            if (added) MarkProjectDirty(nullptr);
            return TRUE;
        }

        // ---- Remove Track -------------------------------------------------
        if (ctrl == IDC_MG_REM_TRACK)
        {
            int groupIdx = GetSelectedGroupIndex(hwnd);
            int trackIdx = GetSelectedTrackIndex(hwnd);
            if (groupIdx < 0 || trackIdx < 0) return TRUE;

            MuteGroupsEngine::Get().RemoveTrackFromGroup(groupIdx, trackIdx);
            RefreshTrackList(hwnd);
            SetStatus(hwnd, "Track removed");
            MarkProjectDirty(nullptr);
            return TRUE;
        }

        break;
    }

    // -----------------------------------------------------------------------
    case WM_NOTIFY:
    {
        NMHDR* hdr = (NMHDR*)lParam;

        // Groups list notifications
        if (hdr->idFrom == IDC_MG_GROUP_LIST)
        {
            // Selection changed → refresh track list
            if (hdr->code == LVN_ITEMCHANGED)
            {
                NMLISTVIEW* pnm = (NMLISTVIEW*)lParam;
                if (!g_refreshing && (pnm->uChanged & LVIF_STATE) &&
                    (pnm->uNewState & LVIS_SELECTED))
                {
                    RefreshTrackList(hwnd);
                }
                return TRUE;
            }

            // Double-click → toggle mute
            if (hdr->code == NM_DBLCLK)
            {
                int idx = GetSelectedGroupIndex(hwnd);
                if (idx >= 0)
                {
                    MuteGroupsEngine::Get().ToggleGroup(idx);
                    RefreshGroupList(hwnd);
                    SetStatus(hwnd,
                        MuteGroupsEngine::Get().IsGroupMuted(idx) ? "Muted" : "Unmuted");
                }
                return TRUE;
            }

            // Label edit completed → rename
            if (hdr->code == LVN_ENDLABELEDIT)
            {
                NMLVDISPINFOA* pdi = (NMLVDISPINFOA*)lParam;
                if (pdi->item.pszText && pdi->item.pszText[0])
                {
                    int idx = pdi->item.iItem;
                    MuteGroupsEngine::Get().SetGroupName(idx, pdi->item.pszText);
                    RefreshGroupList(hwnd);
                    SetStatus(hwnd, "Group renamed");
                    MarkProjectDirty(nullptr);
                }
                SetWindowLongPtr(hwnd, DWLP_MSGRESULT, TRUE);
                return TRUE;
            }

            // User started dragging a row → initiate reorder drag
            if (hdr->code == LVN_BEGINDRAG)
            {
                NMLISTVIEW* pnm = (NMLISTVIEW*)lParam;
                g_dragSrcIdx    = pnm->iItem;
                g_dragInsertIdx = g_dragSrcIdx;
                g_dragging      = true;
                SetCapture(hwnd);
                return TRUE;
            }

            // Right-click → context menu (Rename / Delete)
            if (hdr->code == NM_RCLICK)
            {
                NMITEMACTIVATE* pnma = (NMITEMACTIVATE*)lParam;
                int idx = pnma->iItem;
                if (idx < 0) idx = GetSelectedGroupIndex(hwnd);
                if (idx < 0) return TRUE;

                SelectGroupRow(hwnd, idx);

                POINT pt = pnma->ptAction;
                ClientToScreen(g_hGroupList, &pt);

                HMENU hMenu = CreatePopupMenu();
                AppendMenuA(hMenu, MF_STRING, 1, "Rename");
                AppendMenuA(hMenu, MF_STRING, 2, "Delete");

                int cmd = (int)TrackPopupMenu(hMenu,
                    TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                    pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(hMenu);

                if (cmd == 1)
                {
                    if (g_hGroupList)
                        ListView_EditLabel(g_hGroupList, idx);
                }
                else if (cmd == 2)
                {
                    const std::string& gname = MuteGroupsEngine::Get().GetGroup(idx).name;
                    char mbuf[256];
                    snprintf(mbuf, sizeof(mbuf),
                             "Delete mute group \"%s\"?", gname.c_str());
                    if (MessageBoxA(hwnd, mbuf, "Live Tools",
                                    MB_YESNO | MB_ICONQUESTION) == IDYES)
                    {
                        MuteGroupsEngine::Get().RemoveGroup(idx);
                        RefreshGroupList(hwnd);
                        SetStatus(hwnd, "Group deleted");
                        MarkProjectDirty(nullptr);
                    }
                }
                return TRUE;
            }
        }
        break;
    }

    // -----------------------------------------------------------------------
    case WM_MOUSEMOVE:
    {
        if (!g_dragging || !g_hGroupList) break;

        POINT lvPt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        MapWindowPoints(hwnd, g_hGroupList, &lvPt, 1);

        const int cnt = ListView_GetItemCount(g_hGroupList);
        int newInsert = cnt;  // default: append after last
        for (int i = 0; i < cnt; ++i)
        {
            RECT rc = {};
            ListView_GetItemRect(g_hGroupList, i, &rc, LVIR_BOUNDS);
            if (lvPt.y < (rc.top + rc.bottom) / 2)
            {
                newInsert = i;
                break;
            }
        }

        if (newInsert != g_dragInsertIdx)
        {
            g_dragInsertIdx = newInsert;
            LVINSERTMARK im = { sizeof(LVINSERTMARK), 0, 0, 0 };
            if (newInsert < cnt)
            {
                im.iItem   = newInsert;
                im.dwFlags = 0;          // line appears before this item
            }
            else
            {
                im.iItem   = cnt > 0 ? cnt - 1 : 0;
                im.dwFlags = LVIM_AFTER; // line appears after last item
            }
            ListView_SetInsertMark(g_hGroupList, &im);
        }
        SetCursor(LoadCursor(nullptr, IDC_SIZENS));
        return TRUE;
    }

    // -----------------------------------------------------------------------
    case WM_LBUTTONUP:
    {
        if (!g_dragging) break;

        ReleaseCapture();
        g_dragging = false;

        // Clear insert mark
        LVINSERTMARK im = { sizeof(LVINSERTMARK), 0, -1, 0 };
        ListView_SetInsertMark(g_hGroupList, &im);

        // Convert "insert before" slot to final engine index
        const int insertBefore = g_dragInsertIdx;
        const int src          = g_dragSrcIdx;
        g_dragSrcIdx = g_dragInsertIdx = -1;

        int finalPos;
        if (insertBefore <= src)
            finalPos = insertBefore;
        else
            finalPos = insertBefore - 1;  // item's removal shifts later slots left

        const int cnt = MuteGroupsEngine::Get().GetGroupCount();
        if (finalPos != src && finalPos >= 0 && finalPos < cnt)
        {
            MuteGroupsEngine::Get().MoveGroup(src, finalPos);
            RefreshGroupList(hwnd);
            SelectGroupRow(hwnd, finalPos);
            RefreshTrackList(hwnd);
            MarkProjectDirty(nullptr);
            SetStatus(hwnd, "Group moved");
        }
        return TRUE;
    }

    // -----------------------------------------------------------------------
    case WM_CAPTURECHANGED:
    {
        if (g_dragging)
        {
            g_dragging = false;
            LVINSERTMARK im = { sizeof(LVINSERTMARK), 0, -1, 0 };
            if (g_hGroupList) ListView_SetInsertMark(g_hGroupList, &im);
            g_dragSrcIdx = g_dragInsertIdx = -1;
        }
        break;
    }

    // -----------------------------------------------------------------------
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return TRUE;

    case WM_DESTROY:
        g_hwnd      = nullptr;
        g_hGroupList = nullptr;
        g_hTrackList = nullptr;
        return TRUE;
    }

    return FALSE;
}
