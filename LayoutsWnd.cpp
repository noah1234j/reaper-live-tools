#include "LayoutsWnd.h"
#include "api.h"
#include "resource.h"

#include <commctrl.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
std::vector<std::unique_ptr<LayoutSnapshot>> g_layouts;

static HWND      g_lwnd      = nullptr;
static HINSTANCE g_lhInstance = nullptr;

// Context-menu item IDs
enum { LCTX_RENAME = 200, LCTX_OVERWRITE, LCTX_DELETE };

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

        case IDC_LAY_UP:
        {
            int sel = GetSelectedLayoutIndex(hwnd);
            if (sel > 0)
            {
                std::swap(g_layouts[sel], g_layouts[sel - 1]);
                g_layouts[sel - 1]->m_slot = sel - 1;
                g_layouts[sel    ]->m_slot = sel;
                RefreshLayoutList(hwnd);
                HWND hList = GetDlgItem(hwnd, IDC_LAY_LIST);
                ListView_SetItemState(hList, sel - 1,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
            break;
        }

        case IDC_LAY_DOWN:
        {
            int sel = GetSelectedLayoutIndex(hwnd);
            if (sel >= 0 && sel + 1 < (int)g_layouts.size())
            {
                std::swap(g_layouts[sel], g_layouts[sel + 1]);
                g_layouts[sel    ]->m_slot = sel;
                g_layouts[sel + 1]->m_slot = sel + 1;
                RefreshLayoutList(hwnd);
                HWND hList = GetDlgItem(hwnd, IDC_LAY_LIST);
                ListView_SetItemState(hList, sel + 1,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
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
        g_lwnd = nullptr;
        return TRUE;

    default:
        break;
    }
    return FALSE;
}
