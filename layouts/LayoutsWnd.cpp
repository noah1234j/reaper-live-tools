#include "LayoutsWnd.h"
#include "api.h"
#include "resource.h"

#include <commctrl.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <windowsx.h>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
std::vector<std::unique_ptr<LayoutSnapshot>> g_layouts;

static HWND      g_lwnd      = nullptr;
static HINSTANCE g_lhInstance = nullptr;

// Context-menu item IDs
enum { LCTX_RENAME = 200, LCTX_OVERWRITE, LCTX_DELETE };

// Drag-drop reorder state (layouts list)
static int        g_layDragSrc     = -1;
static int        g_layDragTarget  = -1;
static HIMAGELIST g_layHDragImages = nullptr;

// List subclass state (layouts list)
static WNDPROC s_layOrigListProc = nullptr;
static bool    s_layLbTracking   = false;
static POINT   s_layLbDownPt     = {};
static int     s_layLbDownItem   = -1;
static DWORD   s_layLbDownTime   = 0;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK LayoutsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void RefreshLayoutList(HWND hwnd);
static void DoCapture(HWND hwnd);
static void DoRecall(HWND hwnd, int index);
static void ShowLayoutContextMenu(HWND hwnd, int item, POINT pt);
static int  GetSelectedLayoutIndex(HWND hwnd);
static int  ComputeLayoutMask(HWND hwnd);
static void DoEndDragLayout(HWND hwnd);
static LRESULT CALLBACK LayoutListSubclassProc(HWND hList, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void LayoutsWnd_Init(HINSTANCE hInstance)
{
    g_lhInstance = hInstance;
}

void LayoutsWnd_Cleanup()
{
    if (g_lwnd && IsWindow(g_lwnd))
    {
        DestroyWindow(g_lwnd);
        g_lwnd = nullptr;
    }
}

void LayoutsWnd_ShowHide()
{
    if (!g_lwnd || !IsWindow(g_lwnd))
    {
        HWND hMain = GetMainHwnd();
        g_lwnd = CreateDialogParam(g_lhInstance,
                                   MAKEINTRESOURCE(IDD_LAYOUTS),
                                   hMain,
                                   LayoutsDlgProc,
                                   0);
        if (!g_lwnd)
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "CreateDialogParam failed for IDD_LAYOUTS. Error=%lu",
                     GetLastError());
            MessageBoxA(hMain, buf, "Layouts", MB_OK | MB_ICONERROR);
            return;
        }
    }

    if (IsWindowVisible(g_lwnd))
        ShowWindow(g_lwnd, SW_HIDE);
    else
        ShowWindow(g_lwnd, SW_SHOW);
}

int LayoutsWnd_IsVisible()
{
    return (g_lwnd && IsWindow(g_lwnd) && IsWindowVisible(g_lwnd)) ? 1 : 0;
}

void LayoutsWnd_RefreshList()
{
    if (g_lwnd && IsWindow(g_lwnd))
        RefreshLayoutList(g_lwnd);
}

// ---------------------------------------------------------------------------
// ComputeLayoutMask
// ---------------------------------------------------------------------------
static int ComputeLayoutMask(HWND hwnd)
{
    int mask = 0;
    if (IsDlgButtonChecked(hwnd, IDC_LAY_ORDER)  == BST_CHECKED) mask |= LY_ORDER;
    if (IsDlgButtonChecked(hwnd, IDC_LAY_HEIGHT) == BST_CHECKED) mask |= LY_HEIGHT;
    if (IsDlgButtonChecked(hwnd, IDC_LAY_VIS)    == BST_CHECKED) mask |= LY_VIS;
    if (IsDlgButtonChecked(hwnd, IDC_LAY_NAMES)  == BST_CHECKED) mask |= LY_NAME;
    return mask;
}

// ---------------------------------------------------------------------------
// RefreshLayoutList
// ---------------------------------------------------------------------------
static void RefreshLayoutList(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LAY_LIST);
    if (!hList) return;

    ListView_DeleteAllItems(hList);

    for (int i = 0; i < (int)g_layouts.size(); i++)
    {
        const auto& ly = g_layouts[i];

        LVITEM lvi = {};
        lvi.mask     = LVIF_TEXT;
        lvi.iItem    = i;
        lvi.iSubItem = 0;
        char slotBuf[16];
        snprintf(slotBuf, sizeof(slotBuf), "%d", i + 1);
        lvi.pszText = slotBuf;
        ListView_InsertItem(hList, &lvi);

        ListView_SetItemText(hList, i, 1, const_cast<char*>(ly->m_name.c_str()));

        char timeBuf[32] = "";
        if (ly->m_time)
        {
            struct tm* lt = localtime((const time_t*)&ly->m_time);
            if (lt) strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", lt);
        }
        ListView_SetItemText(hList, i, 2, timeBuf);
    }
}

// ---------------------------------------------------------------------------
// GetSelectedLayoutIndex
// ---------------------------------------------------------------------------
static int GetSelectedLayoutIndex(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LAY_LIST);
    if (!hList) return -1;
    return ListView_GetNextItem(hList, -1, LVNI_SELECTED);
}

// ---------------------------------------------------------------------------
// DoCapture
// ---------------------------------------------------------------------------
static void DoCapture(HWND hwnd)
{
    int mask = ComputeLayoutMask(hwnd);
    if (!mask) return;

    int slot = (int)g_layouts.size();
    char name[256];
    snprintf(name, sizeof(name), "Layout %d", slot + 1);

    auto ly = std::make_unique<LayoutSnapshot>(slot, name);
    ly->Capture(mask);

    g_layouts.push_back(std::move(ly));
    RefreshLayoutList(hwnd);

    // Select the new item
    HWND hList = GetDlgItem(hwnd, IDC_LAY_LIST);
    int newIdx = (int)g_layouts.size() - 1;
    ListView_SetItemState(hList, newIdx,
        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(hList, newIdx, FALSE);

    SetDlgItemText(hwnd, IDC_LAY_STATUS, "Captured");
    Undo_OnStateChangeEx("Capture Layout", -1, -1);
}

// ---------------------------------------------------------------------------
// DoRecall
// ---------------------------------------------------------------------------
static void DoRecall(HWND hwnd, int index)
{
    if (index < 0 || index >= (int)g_layouts.size()) return;

    int mask = ComputeLayoutMask(hwnd);
    if (!mask) mask = g_layouts[index]->m_mask; // fallback to stored mask

    g_layouts[index]->Recall(mask);

    SetDlgItemText(hwnd, IDC_LAY_STATUS, "Recalled");
}

// ---------------------------------------------------------------------------
// ShowLayoutContextMenu
// ---------------------------------------------------------------------------
static void ShowLayoutContextMenu(HWND hwnd, int item, POINT pt)
{
    if (item < 0) return;

    HMENU hMenu = CreatePopupMenu();
    AppendMenu(hMenu, MF_STRING, LCTX_RENAME,    "Rename");
    AppendMenu(hMenu, MF_STRING, LCTX_OVERWRITE, "Overwrite (re-capture)");
    AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hMenu, MF_STRING, LCTX_DELETE,    "Delete");

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                              pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);

    if (cmd == LCTX_RENAME)
    {
        HWND hList = GetDlgItem(hwnd, IDC_LAY_LIST);
        ListView_EditLabel(hList, item);
    }
    else if (cmd == LCTX_OVERWRITE)
    {
        if (item < (int)g_layouts.size())
        {
            int mask = g_layouts[item]->m_mask;
            if (!mask) mask = ComputeLayoutMask(hwnd);
            g_layouts[item]->Capture(mask);
            RefreshLayoutList(hwnd);
            SetDlgItemText(hwnd, IDC_LAY_STATUS, "Overwritten");
            Undo_OnStateChangeEx("Overwrite Layout", -1, -1);
        }
    }
    else if (cmd == LCTX_DELETE)
    {
        if (item < (int)g_layouts.size())
        {
            g_layouts.erase(g_layouts.begin() + item);
            for (int i = 0; i < (int)g_layouts.size(); i++)
                g_layouts[i]->m_slot = i;
            RefreshLayoutList(hwnd);
            SetDlgItemText(hwnd, IDC_LAY_STATUS, "Deleted");
            Undo_OnStateChangeEx("Delete Layout", -1, -1);
        }
    }
}

// ---------------------------------------------------------------------------
// DoEndDragLayout – commit drag-to-reorder on the layouts list
// ---------------------------------------------------------------------------
static void DoEndDragLayout(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LAY_LIST);

    if (g_layHDragImages)
    {
        ImageList_DragLeave(hwnd);
        ImageList_EndDrag();
        ImageList_Destroy(g_layHDragImages);
        g_layHDragImages = nullptr;
    }
    ListView_SetItemState(hList, -1, 0, LVIS_DROPHILITED);

    int src = g_layDragSrc;
    int tgt = g_layDragTarget;
    g_layDragSrc    = -1;
    g_layDragTarget = -1;

    if (src < 0 || tgt < 0 || src == tgt) return;
    if (src >= (int)g_layouts.size() || tgt >= (int)g_layouts.size()) return;

    auto moved = std::move(g_layouts[src]);
    g_layouts.erase(g_layouts.begin() + src);
    int insertAt = (src < tgt) ? tgt - 1 : tgt;
    if (insertAt >= (int)g_layouts.size())
        g_layouts.push_back(std::move(moved));
    else
        g_layouts.insert(g_layouts.begin() + insertAt, std::move(moved));

    for (int i = 0; i < (int)g_layouts.size(); i++)
        g_layouts[i]->m_slot = i;

    RefreshLayoutList(hwnd);
    ListView_SetItemState(hList, insertAt,
        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(hList, insertAt, FALSE);
    Undo_OnStateChangeEx("Reorder Layout", -1, -1);
}

// ---------------------------------------------------------------------------
// LayoutListSubclassProc – intercept mouse on the layouts list for drag-drop
// ---------------------------------------------------------------------------
static LRESULT CALLBACK LayoutListSubclassProc(HWND hList, UINT msg,
                                                WPARAM wParam, LPARAM lParam)
{
    HWND dlg = GetParent(hList);

    switch (msg)
    {
    case WM_LBUTTONDOWN:
    {
        LRESULT r = CallWindowProc(s_layOrigListProc, hList, msg, wParam, lParam);
        LVHITTESTINFO hti = {};
        hti.pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int item = ListView_HitTest(hList, &hti);
        if (item >= 0)
        {
            s_layLbTracking = true;
            s_layLbDownPt   = hti.pt;
            s_layLbDownItem = item;
            s_layLbDownTime = GetTickCount();
        }
        return r;
    }

    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        if (s_layLbTracking && !(wParam & MK_LBUTTON))
            s_layLbTracking = false;

        if (s_layLbTracking && g_layDragSrc < 0)
        {
            bool movedEnough = (abs(pt.x - s_layLbDownPt.x) > GetSystemMetrics(SM_CXDRAG) ||
                                abs(pt.y - s_layLbDownPt.y) > GetSystemMetrics(SM_CYDRAG));
            bool heldLongEnough = (GetTickCount() - s_layLbDownTime >= 200);
            if (movedEnough && heldLongEnough)
            {
                g_layDragSrc    = s_layLbDownItem;
                g_layDragTarget = -1;
                s_layLbTracking = false;
                SetCapture(hList);

                POINT ptOffset = { 8, 8 };
                g_layHDragImages = ListView_CreateDragImage(hList, g_layDragSrc, &ptOffset);
                if (g_layHDragImages)
                {
                    POINT dlgPt = pt;
                    ClientToScreen(hList, &dlgPt);
                    ScreenToClient(dlg, &dlgPt);
                    ImageList_BeginDrag(g_layHDragImages, 0, 8, 8);
                    ImageList_DragEnter(dlg, dlgPt.x, dlgPt.y);
                }
            }
        }

        if (g_layDragSrc >= 0)
        {
            POINT dlgPt = pt;
            ClientToScreen(hList, &dlgPt);
            ScreenToClient(dlg, &dlgPt);
            if (g_layHDragImages)
            {
                ImageList_DragMove(dlgPt.x, dlgPt.y);
                ImageList_DragShowNolock(FALSE);
            }
            LVHITTESTINFO hti = {};
            hti.pt = pt;
            int newTgt = ListView_HitTest(hList, &hti);
            if (newTgt != g_layDragTarget)
            {
                g_layDragTarget = newTgt;
                ListView_SetItemState(hList, -1, 0, LVIS_DROPHILITED);
                if (g_layDragTarget >= 0)
                    ListView_SetItemState(hList, g_layDragTarget,
                                         LVIS_DROPHILITED, LVIS_DROPHILITED);
            }
            if (g_layHDragImages)
                ImageList_DragShowNolock(TRUE);
            return 0;
        }
        break;
    }

    case WM_LBUTTONUP:
        s_layLbTracking = false;
        if (g_layDragSrc >= 0)
        {
            ReleaseCapture();
            DoEndDragLayout(dlg);
            return 0;
        }
        break;

    case WM_CAPTURECHANGED:
        s_layLbTracking = false;
        if (g_layDragSrc >= 0)
            DoEndDragLayout(dlg);
        break;
    }

    return CallWindowProc(s_layOrigListProc, hList, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// DialogProc
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK LayoutsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
        InitCommonControlsEx(&icc);

        // Create SysListView32 in place of the placeholder LTEXT
        HWND hPlaceholder = GetDlgItem(hwnd, IDC_LAY_LIST);
        RECT rList = {};
        GetWindowRect(hPlaceholder, &rList);
        MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&rList, 2);
        DestroyWindow(hPlaceholder);

        HWND hList = CreateWindowExA(0, "SysListView32", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
            LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | LVS_EDITLABELS,
            rList.left, rList.top,
            rList.right - rList.left, rList.bottom - rList.top,
            hwnd, (HMENU)(INT_PTR)IDC_LAY_LIST, g_lhInstance, nullptr);

        if (hList)
        {
            ListView_SetExtendedListViewStyle(hList,
                LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

            LVCOLUMN col = {};
            col.mask    = LVCF_TEXT | LVCF_WIDTH;
            col.cx      = 30;  col.pszText = const_cast<char*>("#");
            ListView_InsertColumn(hList, 0, &col);
            col.cx      = 130; col.pszText = const_cast<char*>("Name");
            ListView_InsertColumn(hList, 1, &col);
            col.cx      = 110; col.pszText = const_cast<char*>("Date");
            ListView_InsertColumn(hList, 2, &col);

            // Subclass the list for drag-drop reordering
            s_layOrigListProc = (WNDPROC)(LONG_PTR)SetWindowLongPtr(
                hList, GWLP_WNDPROC, (LONG_PTR)LayoutListSubclassProc);
        }

        // Default: all settings checked
        CheckDlgButton(hwnd, IDC_LAY_ORDER,  BST_CHECKED);
        CheckDlgButton(hwnd, IDC_LAY_HEIGHT, BST_CHECKED);
        CheckDlgButton(hwnd, IDC_LAY_VIS,    BST_CHECKED);
        CheckDlgButton(hwnd, IDC_LAY_NAMES,  BST_CHECKED);

        SetDlgItemText(hwnd, IDC_LAY_STATUS, "Ready");

        RefreshLayoutList(hwnd);
        return TRUE;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        switch (id)
        {
        case IDC_LAY_CAPTURE:
            DoCapture(hwnd);
            break;

        case IDC_LAY_RECALL:
            DoRecall(hwnd, GetSelectedLayoutIndex(hwnd));
            break;

        case IDC_LAY_PREV:
        {
            int sel = GetSelectedLayoutIndex(hwnd);
            if (sel > 0)
            {
                sel--;
                HWND hList = GetDlgItem(hwnd, IDC_LAY_LIST);
                ListView_SetItemState(hList, sel,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(hList, sel, FALSE);
                DoRecall(hwnd, sel);
            }
            break;
        }

        case IDC_LAY_NEXT:
        {
            int sel = GetSelectedLayoutIndex(hwnd);
            if (sel >= 0 && sel + 1 < (int)g_layouts.size())
            {
                sel++;
                HWND hList = GetDlgItem(hwnd, IDC_LAY_LIST);
                ListView_SetItemState(hList, sel,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(hList, sel, FALSE);
                DoRecall(hwnd, sel);
            }
            break;
        }

        default:
            break;
        }
        return TRUE;
    }

    case WM_NOTIFY:
    {
        NMHDR* hdr = (NMHDR*)lParam;
        if (hdr->idFrom == IDC_LAY_LIST)
        {
            if (hdr->code == NM_DBLCLK)
            {
                NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
                if (nia->iItem >= 0)
                    DoRecall(hwnd, nia->iItem);
            }
            else if (hdr->code == NM_RCLICK)
            {
                NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
                POINT pt;
                GetCursorPos(&pt);
                ShowLayoutContextMenu(hwnd, nia->iItem, pt);
            }
            else if (hdr->code == LVN_ENDLABELEDIT)
            {
                NMLVDISPINFO* di = (NMLVDISPINFO*)lParam;
                if (di->item.pszText && di->item.iItem >= 0 &&
                    di->item.iItem < (int)g_layouts.size())
                {
                    g_layouts[di->item.iItem]->m_name = di->item.pszText;
                    SetWindowLongPtr(hwnd, DWLP_MSGRESULT, TRUE);
                    RefreshLayoutList(hwnd);
                }
                return TRUE;
            }
        }
        return FALSE;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return TRUE;

    case WM_DESTROY:
        if (s_layOrigListProc)
        {
            HWND hListD = GetDlgItem(hwnd, IDC_LAY_LIST);
            if (hListD) SetWindowLongPtr(hListD, GWLP_WNDPROC, (LONG_PTR)s_layOrigListProc);
            s_layOrigListProc = nullptr;
        }
        g_lwnd = nullptr;
        return TRUE;

    default:
        break;
    }
    return FALSE;
}
