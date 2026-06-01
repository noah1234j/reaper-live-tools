// ---------------------------------------------------------------------------
// ZoneEditorWnd.cpp  —  zone assignment editor panel
// ---------------------------------------------------------------------------

#include "ZoneEditorWnd.h"
#include "ZoneModel.h"
#include "SurfaceModel.h"
#include "CanvasWnd.h"
#include "ActionSearchDlg.h"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "comctl32.lib")

using namespace SurfaceEditor;

// ---------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------

static const int k_HeaderH   = 86;  // zone name / type / flags area
static const int k_ListH     = 80;  // included/sub zone lists
static const int k_TabBarH   = 24;
static const int k_ToolbarH  = 28;
static const int k_Pad       = 4;

// Modifier flag bits
enum ZeModFlag {
    ZEMOD_SHIFT   = 1 << 0,
    ZEMOD_OPTION  = 1 << 1,
    ZEMOD_CONTROL = 1 << 2,
    ZEMOD_ALT     = 1 << 3,
    ZEMOD_FLIP    = 1 << 4,
    ZEMOD_HOLD    = 1 << 5,
};

// ---------------------------------------------------------------------------
// Per-panel state
// ---------------------------------------------------------------------------

struct ZoneEditorState {
    ZoneFile         zone;
    Surface*         surf      = nullptr;
    bool             dirty     = false;
    int              activeTab = 0; // 0=Visual, 1=Table

    // Controls
    HWND hPanel      = nullptr;
    HWND hZoneName   = nullptr;
    HWND hZoneType   = nullptr;
    HWND hIncList    = nullptr;
    HWND hIncAdd     = nullptr;
    HWND hIncDel     = nullptr;
    HWND hSubList    = nullptr;
    HWND hSubAdd     = nullptr;
    HWND hSubDel     = nullptr;
    HWND hModShift   = nullptr;
    HWND hModOption  = nullptr;
    HWND hModControl = nullptr;
    HWND hModAlt     = nullptr;
    HWND hModFlip    = nullptr;
    HWND hModHold    = nullptr;
    HWND hTab        = nullptr;
    HWND hCanvas     = nullptr;
    HWND hTable      = nullptr;
    HWND hSave       = nullptr;
    HWND hAddRow     = nullptr;
    HWND hDelRow     = nullptr;
    HINSTANCE hInst  = nullptr;
};

static ZoneEditorState* GetState(HWND hwnd)
{
    return reinterpret_cast<ZoneEditorState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool ModifierVisible(const ZoneEditorState* s, const std::string& mod)
{
    // If no modifier checkboxes are checked, show everything
    bool anyChecked = (SendMessageA(s->hModShift,   BM_GETCHECK, 0, 0) == BST_CHECKED ||
                       SendMessageA(s->hModOption,  BM_GETCHECK, 0, 0) == BST_CHECKED ||
                       SendMessageA(s->hModControl, BM_GETCHECK, 0, 0) == BST_CHECKED ||
                       SendMessageA(s->hModAlt,     BM_GETCHECK, 0, 0) == BST_CHECKED ||
                       SendMessageA(s->hModFlip,    BM_GETCHECK, 0, 0) == BST_CHECKED ||
                       SendMessageA(s->hModHold,    BM_GETCHECK, 0, 0) == BST_CHECKED);

    if (!anyChecked) return true;

    if (SendMessageA(s->hModShift,   BM_GETCHECK, 0, 0) == BST_CHECKED && mod.find("Shift")   != std::string::npos) return true;
    if (SendMessageA(s->hModOption,  BM_GETCHECK, 0, 0) == BST_CHECKED && mod.find("Option")  != std::string::npos) return true;
    if (SendMessageA(s->hModControl, BM_GETCHECK, 0, 0) == BST_CHECKED && mod.find("Control") != std::string::npos) return true;
    if (SendMessageA(s->hModAlt,     BM_GETCHECK, 0, 0) == BST_CHECKED && mod.find("Alt")     != std::string::npos) return true;
    if (SendMessageA(s->hModFlip,    BM_GETCHECK, 0, 0) == BST_CHECKED && mod.find("Flip")    != std::string::npos) return true;
    if (SendMessageA(s->hModHold,    BM_GETCHECK, 0, 0) == BST_CHECKED && mod.find("Hold")    != std::string::npos) return true;

    return false;
}

// Rebuild the table (ListView) from the in-memory model
static void RebuildTable(ZoneEditorState* s)
{
    HWND lv = s->hTable;
    ListView_DeleteAllItems(lv);

    int row = 0;
    for (int i = 0; i < (int)s->zone.assignments.size(); ++i)
    {
        const ZoneAssignment& za = s->zone.assignments[i];
        if (!ModifierVisible(s, za.modifier)) continue;

        LVITEMA item = {};
        item.mask    = LVIF_TEXT | LVIF_PARAM;
        item.iItem   = row;
        item.lParam  = (LPARAM)i;  // original index for editing
        item.pszText = const_cast<char*>(za.widgetExpr.c_str());
        ListView_InsertItem(lv, &item);

        auto setCol = [&](int col, const char* text) {
            LVITEMA sub = {};
            sub.mask     = LVIF_TEXT;
            sub.iItem    = row;
            sub.iSubItem = col;
            sub.pszText  = const_cast<char*>(text);
            ListView_SetItem(lv, &sub);
        };

        setCol(1, za.modifier.c_str());
        setCol(2, za.action.c_str());
        setCol(3, za.params.c_str());
        ++row;
    }
}

// Reload IncludedZones / SubZones listboxes
static void RebuildLists(ZoneEditorState* s)
{
    SendMessageA(s->hIncList, LB_RESETCONTENT, 0, 0);
    for (const auto& iz : s->zone.includedZones)
        SendMessageA(s->hIncList, LB_ADDSTRING, 0, (LPARAM)iz.c_str());

    SendMessageA(s->hSubList, LB_RESETCONTENT, 0, 0);
    for (const auto& sz : s->zone.subZones)
        SendMessageA(s->hSubList, LB_ADDSTRING, 0, (LPARAM)sz.c_str());
}

// Full refresh from model
static void FullRefresh(ZoneEditorState* s)
{
    // Zone name
    SetWindowTextA(s->hZoneName, s->zone.zoneName.c_str());

    // Zone type combobox
    int typeCount = 0;
    const char* const* types = GetZoneTypeNames(&typeCount);
    int selIdx = CB_ERR;
    for (int i = 0; i < typeCount; ++i)
    {
        if (s->zone.zoneName == types[i])
        {
            selIdx = i;
            break;
        }
    }
    if (selIdx != CB_ERR)
        SendMessageA(s->hZoneType, CB_SETCURSEL, selIdx, 0);

    RebuildLists(s);
    RebuildTable(s);

    CanvasWnd_SetSurface(s->hCanvas, s->surf);
    CanvasWnd_SetZoneFile(s->hCanvas, &s->zone);
    CanvasWnd_SetMode(s->hCanvas, CanvasMode::ZoneAssign);
    CanvasWnd_Refresh(s->hCanvas);
}

// ---------------------------------------------------------------------------
// Canvas callbacks
// ---------------------------------------------------------------------------

static void OnCanvasWidgetActivated(int col, int row, void* userData)
{
    ZoneEditorState* s = (ZoneEditorState*)userData;
    if (!s->surf) return;

    // Find widget at (col, row)
    Widget* w = nullptr;
    for (auto& ww : s->surf->widgets)
        if (ww.gridCol == col && ww.gridRow == row)
        { w = &ww; break; }

    if (!w) return;

    // Find existing assignment
    ZoneAssignment* za = nullptr;
    for (auto& a : s->zone.assignments)
        if (a.widgetExpr == w->name) { za = &a; break; }

    std::string currentToken = za ? za->action : "";

    std::string chosen = ActionSearchDlg_Show(s->hPanel, currentToken);
    if (chosen.empty()) return;

    if (za)
    {
        za->action = chosen;
    }
    else
    {
        ZoneAssignment newZa;
        newZa.widgetExpr = w->name;
        newZa.action     = chosen;
        s->zone.assignments.push_back(newZa);
    }

    s->dirty = true;
    RebuildTable(s);
    CanvasWnd_SetZoneFile(s->hCanvas, &s->zone);
    CanvasWnd_Refresh(s->hCanvas);
}

// ---------------------------------------------------------------------------
// Layout helper
// ---------------------------------------------------------------------------

static void DoLayout(ZoneEditorState* s, int W, int H)
{
    if (W <= 0 || H <= 0) return;
    int p = k_Pad;

    // Row 0: Zone name label + edit + type combobox
    int y = p;
    MoveWindow(s->hZoneName, p + 70, y, W - 180 - 3*p, 22, TRUE);
    MoveWindow(s->hZoneType, W - 180 - p, y, 180, 22, TRUE);

    // Row 1: Modifier checkboxes
    y += 28;
    int cbW = 70;
    MoveWindow(s->hModShift,   p,           y, cbW, 20, TRUE);
    MoveWindow(s->hModOption,  p + cbW,     y, cbW, 20, TRUE);
    MoveWindow(s->hModControl, p + 2*cbW,   y, cbW, 20, TRUE);
    MoveWindow(s->hModAlt,     p + 3*cbW,   y, cbW, 20, TRUE);
    MoveWindow(s->hModFlip,    p + 4*cbW,   y, cbW, 20, TRUE);
    MoveWindow(s->hModHold,    p + 5*cbW,   y, cbW, 20, TRUE);

    // Row 2: IncludedZones list + SubZones list (side by side)
    y += 26;
    int halfW = (W - 3*p) / 2;
    MoveWindow(s->hIncList, p,           y, halfW - 40, k_ListH, TRUE);
    MoveWindow(s->hIncAdd,  p + halfW - 36, y,       34, 18, TRUE);
    MoveWindow(s->hIncDel,  p + halfW - 36, y + 20,  34, 18, TRUE);
    MoveWindow(s->hSubList, p + halfW + p, y, halfW - 40, k_ListH, TRUE);
    MoveWindow(s->hSubAdd,  p + halfW + p + halfW - 36, y,      34, 18, TRUE);
    MoveWindow(s->hSubDel,  p + halfW + p + halfW - 36, y + 20, 34, 18, TRUE);

    // Tab bar
    y += k_ListH + p;
    int tabAreaH = H - y - k_ToolbarH - p;
    if (tabAreaH < 0) tabAreaH = 0;
    MoveWindow(s->hTab, p, y, W - 2*p, k_TabBarH, TRUE);

    // Canvas and Table fill the tab area below the tab bar
    int contentY = y + k_TabBarH + 2;
    int contentH = tabAreaH - k_TabBarH - 2;
    if (contentH < 0) contentH = 0;
    MoveWindow(s->hCanvas, p, contentY, W - 2*p, contentH, TRUE);
    MoveWindow(s->hTable,  p, contentY, W - 2*p, contentH, TRUE);

    // Toolbar: Save, Add Row, Del Row
    int toolY = H - k_ToolbarH;
    MoveWindow(s->hSave,   p,       toolY, 60, 22, TRUE);
    MoveWindow(s->hAddRow, p + 66,  toolY, 60, 22, TRUE);
    MoveWindow(s->hDelRow, p + 132, toolY, 60, 22, TRUE);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

static LRESULT CALLBACK ZoneEditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ZoneEditorState* s = GetState(hwnd);

    switch (msg)
    {
    case WM_SIZE:
    {
        if (s) DoLayout(s, LOWORD(lp), HIWORD(lp));
        return 0;
    }

    case WM_COMMAND:
    {
        if (!s) return 0;
        int id = LOWORD(wp);

        // Zone name edit — update model
        if (id == IDC_ZE_ZONE_NAME && HIWORD(wp) == EN_CHANGE)
        {
            char buf[256] = {};
            GetWindowTextA(s->hZoneName, buf, sizeof(buf));
            s->zone.zoneName = buf;
            s->dirty = true;
            return 0;
        }

        // Zone type combobox
        if (id == IDC_ZE_ZONE_TYPE && HIWORD(wp) == CBN_SELCHANGE)
        {
            int sel = (int)SendMessageA(s->hZoneType, CB_GETCURSEL, 0, 0);
            if (sel != CB_ERR)
            {
                int cnt = 0;
                const char* const* types = GetZoneTypeNames(&cnt);
                if (sel < cnt)
                {
                    s->zone.zoneName = types[sel];
                    SetWindowTextA(s->hZoneName, types[sel]);
                    s->dirty = true;
                }
            }
            return 0;
        }

        // IncludedZones add
        if (id == IDC_ZE_INC_ADD)
        {
            char buf[256] = "ZoneName";
            if (s->hIncList)
            {
                // Simple input: get selection if any, else prompt
                s->zone.includedZones.push_back("NewZone");
                RebuildLists(s);
                s->dirty = true;
            }
            return 0;
        }
        // IncludedZones del
        if (id == IDC_ZE_INC_DEL)
        {
            int sel = (int)SendMessageA(s->hIncList, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR && sel < (int)s->zone.includedZones.size())
            {
                s->zone.includedZones.erase(s->zone.includedZones.begin() + sel);
                RebuildLists(s);
                s->dirty = true;
            }
            return 0;
        }
        // SubZones add
        if (id == IDC_ZE_SUB_ADD)
        {
            s->zone.subZones.push_back("NewSubZone");
            RebuildLists(s);
            s->dirty = true;
            return 0;
        }
        // SubZones del
        if (id == IDC_ZE_SUB_DEL)
        {
            int sel = (int)SendMessageA(s->hSubList, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR && sel < (int)s->zone.subZones.size())
            {
                s->zone.subZones.erase(s->zone.subZones.begin() + sel);
                RebuildLists(s);
                s->dirty = true;
            }
            return 0;
        }

        // Save
        if (id == IDC_ZE_SAVE)
        {
            if (!s->zone.filePath.empty())
            {
                WriteZoneFile(s->zone);
                s->dirty = false;
            }
            return 0;
        }

        // Add empty row
        if (id == IDC_ZE_ROW_ADD)
        {
            ZoneAssignment za;
            za.widgetExpr = "Widget";
            za.action     = "NoAction";
            s->zone.assignments.push_back(za);
            RebuildTable(s);
            CanvasWnd_SetZoneFile(s->hCanvas, &s->zone);
            CanvasWnd_Refresh(s->hCanvas);
            s->dirty = true;
            return 0;
        }

        // Delete selected row
        if (id == IDC_ZE_ROW_DEL)
        {
            int sel = ListView_GetNextItem(s->hTable, -1, LVNI_SELECTED);
            if (sel >= 0)
            {
                LVITEMA item = {};
                item.mask   = LVIF_PARAM;
                item.iItem  = sel;
                ListView_GetItem(s->hTable, &item);
                int origIdx = (int)item.lParam;
                if (origIdx >= 0 && origIdx < (int)s->zone.assignments.size())
                {
                    s->zone.assignments.erase(s->zone.assignments.begin() + origIdx);
                    RebuildTable(s);
                    CanvasWnd_SetZoneFile(s->hCanvas, &s->zone);
                    CanvasWnd_Refresh(s->hCanvas);
                    s->dirty = true;
                }
            }
            return 0;
        }

        // Modifier checkboxes — just refresh the table filter
        if (id == IDC_ZE_MOD_SHIFT  || id == IDC_ZE_MOD_OPTION  ||
            id == IDC_ZE_MOD_CONTROL|| id == IDC_ZE_MOD_ALT     ||
            id == IDC_ZE_MOD_FLIP   || id == IDC_ZE_MOD_HOLD)
        {
            RebuildTable(s);
            return 0;
        }

        return 0;
    }

    case WM_NOTIFY:
    {
        if (!s) return 0;
        NMHDR* nm = (NMHDR*)lp;

        // Tab switch
        if (nm->hwndFrom == s->hTab && nm->code == TCN_SELCHANGE)
        {
            s->activeTab = TabCtrl_GetCurSel(s->hTab);
            ShowWindow(s->hCanvas, s->activeTab == 0 ? SW_SHOW : SW_HIDE);
            ShowWindow(s->hTable,  s->activeTab == 1 ? SW_SHOW : SW_HIDE);
            return 0;
        }

        // Table double-click → open action picker
        if (nm->hwndFrom == s->hTable && nm->code == NM_DBLCLK)
        {
            int sel = ListView_GetNextItem(s->hTable, -1, LVNI_SELECTED);
            if (sel >= 0)
            {
                LVITEMA item = {};
                item.mask   = LVIF_PARAM;
                item.iItem  = sel;
                ListView_GetItem(s->hTable, &item);
                int origIdx = (int)item.lParam;
                if (origIdx >= 0 && origIdx < (int)s->zone.assignments.size())
                {
                    ZoneAssignment& za = s->zone.assignments[origIdx];
                    std::string chosen = ActionSearchDlg_Show(hwnd, za.action);
                    if (!chosen.empty())
                    {
                        za.action = chosen;
                        s->dirty  = true;
                        RebuildTable(s);
                        CanvasWnd_SetZoneFile(s->hCanvas, &s->zone);
                        CanvasWnd_Refresh(s->hCanvas);
                    }
                }
            }
            return 0;
        }
        return 0;
    }

    case WM_DESTROY:
    {
        if (s)
        {
            CanvasWnd_Destroy(s->hCanvas);
            s->hCanvas = nullptr;
            delete s;
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }

    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

// ---------------------------------------------------------------------------
// Class registration
// ---------------------------------------------------------------------------

static const char* k_ZEClassName = "LT_ZoneEditorWnd";
static bool s_classRegistered = false;

static bool EnsureClassRegistered(HINSTANCE hInst)
{
    if (s_classRegistered) return true;
    WNDCLASSEXA wc = {};
    wc.cbSize       = sizeof(wc);
    wc.lpfnWndProc  = ZoneEditorProc;
    wc.hInstance    = hInst;
    wc.hCursor      = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground= (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName= k_ZEClassName;
    s_classRegistered = (RegisterClassExA(&wc) != 0);
    return s_classRegistered;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

HWND ZoneEditorWnd_Create(HWND hParent, HINSTANCE hInst)
{
    if (!EnsureClassRegistered(hInst)) return nullptr;
    if (!CanvasWnd_RegisterClass(hInst)) {/* already registered is OK */}

    RECT rc; GetClientRect(hParent, &rc);
    HWND hwnd = CreateWindowExA(0, k_ZEClassName, nullptr,
        WS_CHILD | WS_CLIPCHILDREN,
        0, 0, rc.right, rc.bottom,
        hParent, nullptr, hInst, nullptr);
    if (!hwnd) return nullptr;

    ZoneEditorState* s = new ZoneEditorState();
    s->hPanel = hwnd;
    s->hInst  = hInst;
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)s);

    int W = rc.right, H = rc.bottom;
    int p = k_Pad;

    // ---- Create all child controls ----
    // Zone name label + edit
    CreateWindowExA(0, "STATIC", "Zone Name:",
        WS_CHILD | WS_VISIBLE, p, p + 4, 68, 18, hwnd, nullptr, hInst, nullptr);
    s->hZoneName = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        p + 70, p, 200, 22,
        hwnd, (HMENU)(INT_PTR)IDC_ZE_ZONE_NAME, hInst, nullptr);

    // Zone type combobox
    s->hZoneType = CreateWindowExA(0, "COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        W - 184, p, 180, 180,
        hwnd, (HMENU)(INT_PTR)IDC_ZE_ZONE_TYPE, hInst, nullptr);
    int zTypeCnt = 0;
    const char* const* zTypes = GetZoneTypeNames(&zTypeCnt);
    for (int i = 0; i < zTypeCnt; ++i)
        SendMessageA(s->hZoneType, CB_ADDSTRING, 0, (LPARAM)zTypes[i]);

    // Modifier checkboxes
    int y2 = p + 28;
    s->hModShift   = CreateWindowExA(0, "BUTTON", "Shift",   WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX, p,       y2, 68, 20, hwnd, (HMENU)(INT_PTR)IDC_ZE_MOD_SHIFT,   hInst, nullptr);
    s->hModOption  = CreateWindowExA(0, "BUTTON", "Option",  WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX, p+70,    y2, 68, 20, hwnd, (HMENU)(INT_PTR)IDC_ZE_MOD_OPTION,  hInst, nullptr);
    s->hModControl = CreateWindowExA(0, "BUTTON", "Control", WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX, p+140,   y2, 68, 20, hwnd, (HMENU)(INT_PTR)IDC_ZE_MOD_CONTROL, hInst, nullptr);
    s->hModAlt     = CreateWindowExA(0, "BUTTON", "Alt",     WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX, p+210,   y2, 68, 20, hwnd, (HMENU)(INT_PTR)IDC_ZE_MOD_ALT,     hInst, nullptr);
    s->hModFlip    = CreateWindowExA(0, "BUTTON", "Flip",    WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX, p+280,   y2, 68, 20, hwnd, (HMENU)(INT_PTR)IDC_ZE_MOD_FLIP,    hInst, nullptr);
    s->hModHold    = CreateWindowExA(0, "BUTTON", "Hold",    WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX, p+350,   y2, 68, 20, hwnd, (HMENU)(INT_PTR)IDC_ZE_MOD_HOLD,    hInst, nullptr);

    // IncludedZones
    int y3 = y2 + 26;
    int halfW = (W - 3*p) / 2;
    CreateWindowExA(0, "STATIC", "Included Zones:",
        WS_CHILD|WS_VISIBLE, p, y3 - 14, 100, 14, hwnd, nullptr, hInst, nullptr);
    s->hIncList = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", nullptr,
        WS_CHILD|WS_VISIBLE|LBS_NOINTEGRALHEIGHT|WS_VSCROLL,
        p, y3, halfW - 44, k_ListH,
        hwnd, (HMENU)(INT_PTR)IDC_ZE_INC_LIST, hInst, nullptr);
    s->hIncAdd = CreateWindowExA(0, "BUTTON", "+",
        WS_CHILD|WS_VISIBLE, p + halfW - 40, y3,     34, 18,
        hwnd, (HMENU)(INT_PTR)IDC_ZE_INC_ADD, hInst, nullptr);
    s->hIncDel = CreateWindowExA(0, "BUTTON", "-",
        WS_CHILD|WS_VISIBLE, p + halfW - 40, y3 + 20, 34, 18,
        hwnd, (HMENU)(INT_PTR)IDC_ZE_INC_DEL, hInst, nullptr);

    // SubZones
    int x2 = p + halfW + p;
    CreateWindowExA(0, "STATIC", "Sub Zones:",
        WS_CHILD|WS_VISIBLE, x2, y3 - 14, 80, 14, hwnd, nullptr, hInst, nullptr);
    s->hSubList = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", nullptr,
        WS_CHILD|WS_VISIBLE|LBS_NOINTEGRALHEIGHT|WS_VSCROLL,
        x2, y3, halfW - 44, k_ListH,
        hwnd, (HMENU)(INT_PTR)IDC_ZE_SUB_LIST, hInst, nullptr);
    s->hSubAdd = CreateWindowExA(0, "BUTTON", "+",
        WS_CHILD|WS_VISIBLE, x2 + halfW - 40, y3,      34, 18,
        hwnd, (HMENU)(INT_PTR)IDC_ZE_SUB_ADD, hInst, nullptr);
    s->hSubDel = CreateWindowExA(0, "BUTTON", "-",
        WS_CHILD|WS_VISIBLE, x2 + halfW - 40, y3 + 20, 34, 18,
        hwnd, (HMENU)(INT_PTR)IDC_ZE_SUB_DEL, hInst, nullptr);

    // Tab bar
    int y4 = y3 + k_ListH + p;
    s->hTab = CreateWindowExA(0, WC_TABCONTROLA, nullptr,
        WS_CHILD|WS_VISIBLE|TCS_TABS,
        p, y4, W - 2*p, k_TabBarH,
        hwnd, (HMENU)(INT_PTR)IDC_ZE_TAB, hInst, nullptr);
    TCITEMA ti = {};
    ti.mask    = TCIF_TEXT;
    ti.pszText = const_cast<char*>("Visual");
    TabCtrl_InsertItem(s->hTab, 0, &ti);
    ti.pszText = const_cast<char*>("Table");
    TabCtrl_InsertItem(s->hTab, 1, &ti);

    // Canvas
    int contentY = y4 + k_TabBarH + 2;
    int contentH = H - contentY - k_ToolbarH - p;
    if (contentH < 10) contentH = 10;
    s->hCanvas = CanvasWnd_Create(hwnd, hInst, p, contentY, W - 2*p, contentH);
    {
        CanvasCallbacks cb;
        cb.onWidgetActivated = [s](int col, int row) {
            OnCanvasWidgetActivated(col, row, s);
        };
        CanvasWnd_SetCallbacks(s->hCanvas, cb);
    }

    // Table (hidden initially)
    s->hTable = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, nullptr,
        WS_CHILD|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,
        p, contentY, W - 2*p, contentH,
        hwnd, (HMENU)(INT_PTR)IDC_ZE_TABLE, hInst, nullptr);
    ListView_SetExtendedListViewStyle(s->hTable,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    {
        auto addCol = [&](int i, const char* name, int w) {
            LVCOLUMNA col = {};
            col.mask   = LVCF_TEXT | LVCF_WIDTH;
            col.pszText= const_cast<char*>(name);
            col.cx     = w;
            ListView_InsertColumn(s->hTable, i, &col);
        };
        addCol(0, "Widget",     120);
        addCol(1, "Modifier",    80);
        addCol(2, "Action",     220);
        addCol(3, "Parameters", 150);
    }

    // Toolbar
    int toolY = H - k_ToolbarH;
    s->hSave   = CreateWindowExA(0, "BUTTON", "Save",     WS_CHILD|WS_VISIBLE, p,       toolY, 60, 22, hwnd, (HMENU)(INT_PTR)IDC_ZE_SAVE,    hInst, nullptr);
    s->hAddRow = CreateWindowExA(0, "BUTTON", "+ Row",    WS_CHILD|WS_VISIBLE, p + 66,  toolY, 60, 22, hwnd, (HMENU)(INT_PTR)IDC_ZE_ROW_ADD, hInst, nullptr);
    s->hDelRow = CreateWindowExA(0, "BUTTON", "- Row",    WS_CHILD|WS_VISIBLE, p + 132, toolY, 60, 22, hwnd, (HMENU)(INT_PTR)IDC_ZE_ROW_DEL, hInst, nullptr);

    return hwnd;
}

void ZoneEditorWnd_LoadZone(HWND hPanel, const std::string& filePath,
                             SurfaceEditor::Surface* surf)
{
    ZoneEditorState* s = GetState(hPanel);
    if (!s) return;

    if (filePath.empty())
    {
        s->zone = ZoneFile{};
        s->surf = nullptr;
    }
    else
    {
        s->zone = ParseZoneFile(filePath);
        s->surf = surf;
    }
    s->dirty = false;
    FullRefresh(s);
}

void ZoneEditorWnd_Resize(HWND hPanel, const RECT& r)
{
    if (!hPanel) return;
    MoveWindow(hPanel, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);

    ZoneEditorState* s = GetState(hPanel);
    if (s) DoLayout(s, r.right - r.left, r.bottom - r.top);
}

void ZoneEditorWnd_Refresh(HWND hPanel)
{
    ZoneEditorState* s = GetState(hPanel);
    if (s) FullRefresh(s);
}
