// ---------------------------------------------------------------------------
// BtnMapWnd.cpp  –  Button Mapping editor window
//
// Displays all hardware buttons for the active control surface and lets
// the user override each button with a REAPER command ID.
// ---------------------------------------------------------------------------
#include "BtnMapWnd.h"
#include "ControlSurface.h"
#include "api.h"
#include "resource.h"

#include <commctrl.h>
#include <windowsx.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

// ---------------------------------------------------------------------------
// Button descriptor – one row in the ListView
// ---------------------------------------------------------------------------
struct BtnDesc
{
    const char* name;   // display name
    uint8_t     note;   // MIDI note byte
    int         group;  // filter group index
};

// Group 0 = Rec Arm
// Group 1 = Solo
// Group 2 = Mute
// Group 3 = Select
// Group 4 = Transport
// Group 5 = Global

static const BtnDesc k_fp16Buttons[] =
{
    // ---- Rec Arm (Ch 1-7) ------------------------------------------------
    { "RecArm Ch 1",    0x00, 0 },
    { "RecArm Ch 2",    0x01, 0 },
    { "RecArm Ch 3",    0x02, 0 },
    { "RecArm Ch 4",    0x03, 0 },
    { "RecArm Ch 5",    0x04, 0 },
    { "RecArm Ch 6",    0x05, 0 },
    { "RecArm Ch 7",    0x06, 0 },
    // ---- Solo (Ch 1-16) --------------------------------------------------
    { "Solo Ch 1",      0x08, 1 },
    { "Solo Ch 2",      0x09, 1 },
    { "Solo Ch 3",      0x0A, 1 },
    { "Solo Ch 4",      0x0B, 1 },
    { "Solo Ch 5",      0x0C, 1 },
    { "Solo Ch 6",      0x0D, 1 },
    { "Solo Ch 7",      0x0E, 1 },
    { "Solo Ch 8",      0x0F, 1 },
    { "Solo Ch 9",      0x50, 1 },
    { "Solo Ch 10",     0x51, 1 },
    { "Solo Ch 11",     0x52, 1 },
    { "Solo Ch 12",     0x53, 1 },
    { "Solo Ch 13",     0x54, 1 },
    { "Solo Ch 14",     0x55, 1 },
    { "Solo Ch 15",     0x56, 1 },   // note: same as LOOP (0x56) in transport
    { "Solo Ch 16",     0x57, 1 },
    // ---- Mute (Ch 1-16) -------------------------------------------------
    { "Mute Ch 1",      0x10, 2 },
    { "Mute Ch 2",      0x11, 2 },
    { "Mute Ch 3",      0x12, 2 },
    { "Mute Ch 4",      0x13, 2 },
    { "Mute Ch 5",      0x14, 2 },
    { "Mute Ch 6",      0x15, 2 },
    { "Mute Ch 7",      0x16, 2 },
    { "Mute Ch 8",      0x17, 2 },
    { "Mute Ch 9",      0x78, 2 },
    { "Mute Ch 10",     0x79, 2 },
    { "Mute Ch 11",     0x7A, 2 },
    { "Mute Ch 12",     0x7B, 2 },
    { "Mute Ch 13",     0x7C, 2 },
    { "Mute Ch 14",     0x7D, 2 },
    { "Mute Ch 15",     0x7E, 2 },
    { "Mute Ch 16",     0x7F, 2 },
    // ---- Select (Ch 1-16) -----------------------------------------------
    { "Select Ch 1",    0x18, 3 },
    { "Select Ch 2",    0x19, 3 },
    { "Select Ch 3",    0x1A, 3 },
    { "Select Ch 4",    0x1B, 3 },
    { "Select Ch 5",    0x1C, 3 },
    { "Select Ch 6",    0x1D, 3 },
    { "Select Ch 7",    0x1E, 3 },
    { "Select Ch 8",    0x1F, 3 },
    { "Select Ch 9",    0x07, 3 },   // note 0x07 is shared with RecArm-7 in MCU
    { "Select Ch 10",   0x21, 3 },
    { "Select Ch 11",   0x22, 3 },
    { "Select Ch 12",   0x23, 3 },
    { "Select Ch 13",   0x24, 3 },
    { "Select Ch 14",   0x25, 3 },
    { "Select Ch 15",   0x26, 3 },
    { "Select Ch 16",   0x27, 3 },
    // ---- Transport -------------------------------------------------------
    { "Play",           0x5E, 4 },
    { "Stop",           0x5D, 4 },
    { "Record",         0x5F, 4 },
    { "Rewind",         0x5B, 4 },
    { "Fast Fwd",       0x5C, 4 },
    { "Loop",           0x56, 4 },   // note: same as Solo Ch 15 above
    // ---- Global buttons --------------------------------------------------
    { "Zoom",           0x37, 5 },
    { "Scrub/Master",   0x3A, 5 },
    { "Click/Metro",    0x3B, 5 },
    { "Marker",         0x3D, 5 },
    { "Bank Left",      0x2E, 5 },
    { "Bank Right",     0x2F, 5 },
    { "Chan Left",      0x30, 5 },
    { "Chan Right",     0x31, 5 },
    { "Cursor Up",      0x60, 5 },
    { "Cursor Down",    0x61, 5 },
    { "Cursor Left",    0x62, 5 },
    { "Cursor Right",   0x63, 5 },
};
static const int k_fp16ButtonCount = (int)(sizeof(k_fp16Buttons) / sizeof(k_fp16Buttons[0]));

static const char* const k_groupNames[] =
{
    "All",
    "Rec Arm",
    "Solo",
    "Mute",
    "Select",
    "Transport",
    "Global",
};
static const int k_groupCount = (int)(sizeof(k_groupNames) / sizeof(k_groupNames[0]));

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static HINSTANCE g_hInstance = nullptr;

// Current in-memory copy of the button map shown in the window
static BtnMap    g_btnMap;

// Working copy of visible button indices (after group filter)
static std::vector<int> g_visible;  // indices into k_fp16Buttons[]

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK AssignDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static HWND  CreateListView(HWND parent, RECT r);
static void  PopulateFilter(HWND hwnd);
static void  RebuildList(HWND hwnd);
static void  RefreshItem(HWND hList, int listIdx, int descIdx);
static void  GetAssignmentText(uint8_t note, char* buf, int bufSz);
static int   GetSelectedListDesc(HWND hwnd);   // returns k_fp16Buttons index or -1
static void  OpenAssignDialog(HWND parent, int descIdx);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void BtnMapWnd_ShowModal(HWND hParent, HINSTANCE hInst, BtnMap& map)
{
    g_hInstance = hInst;
    g_btnMap    = map;
    DialogBoxParam(hInst, MAKEINTRESOURCE(IDD_BTN_MAP), hParent, DialogProc, 0);
    map = g_btnMap;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static HWND CreateListView(HWND parent, RECT r)
{
    HWND hList = CreateWindowEx(
        0, WC_LISTVIEWA, "",
        WS_CHILD | WS_VISIBLE | WS_BORDER |
        LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        r.left, r.top, r.right - r.left, r.bottom - r.top,
        parent, (HMENU)IDC_BM_LIST, g_hInstance, nullptr);

    ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    LVCOLUMNA col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;

    col.pszText = (LPSTR)"Button";      col.cx = 160; ListView_InsertColumn(hList, 0, &col);
    col.pszText = (LPSTR)"MIDI Note";   col.cx =  70; ListView_InsertColumn(hList, 1, &col);
    col.pszText = (LPSTR)"Assignment";  col.cx = 200; ListView_InsertColumn(hList, 2, &col);

    return hList;
}

static void PopulateFilter(HWND hwnd)
{
    HWND hCombo = GetDlgItem(hwnd, IDC_BM_FILTER);
    for (int i = 0; i < k_groupCount; ++i)
        ComboBox_AddString(hCombo, k_groupNames[i]);
    ComboBox_SetCurSel(hCombo, 0);
}

static void GetAssignmentText(uint8_t note, char* buf, int bufSz)
{
    auto it = g_btnMap.find(note);
    if (it == g_btnMap.end() || it->second.type == BtnActionType::Default)
    {
        lstrcpynA(buf, "(default)", bufSz);
        return;
    }
    if (it->second.type == BtnActionType::None)
    {
        lstrcpynA(buf, "(disabled)", bufSz);
        return;
    }
    // Command
    snprintf(buf, bufSz, "Cmd: %d", it->second.cmdId);
}

static void RefreshItem(HWND hList, int listIdx, int descIdx)
{
    const BtnDesc& d = k_fp16Buttons[descIdx];

    char noteBuf[16], assignBuf[64];
    snprintf(noteBuf, sizeof(noteBuf), "0x%02X", (unsigned)d.note);
    GetAssignmentText(d.note, assignBuf, sizeof(assignBuf));

    LVITEMA item = {};
    item.mask    = LVIF_TEXT;
    item.iItem   = listIdx;

    item.iSubItem = 0; item.pszText = (LPSTR)d.name;    ListView_SetItem(hList, &item);
    item.iSubItem = 1; item.pszText = noteBuf;           ListView_SetItem(hList, &item);
    item.iSubItem = 2; item.pszText = assignBuf;         ListView_SetItem(hList, &item);
}

static void RebuildList(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_BM_LIST);
    HWND hFilter = GetDlgItem(hwnd, IDC_BM_FILTER);
    int sel = ComboBox_GetCurSel(hFilter);  // 0 = All, 1-6 = group 0-5

    g_visible.clear();
    for (int i = 0; i < k_fp16ButtonCount; ++i)
    {
        if (sel <= 0 || k_fp16Buttons[i].group == sel - 1)
            g_visible.push_back(i);
    }

    ListView_DeleteAllItems(hList);
    for (int i = 0; i < (int)g_visible.size(); ++i)
    {
        LVITEMA item = {};
        item.mask    = LVIF_TEXT | LVIF_PARAM;
        item.iItem   = i;
        item.iSubItem = 0;
        item.pszText  = (LPSTR)k_fp16Buttons[g_visible[i]].name;
        item.lParam   = (LPARAM)g_visible[i];
        ListView_InsertItem(hList, &item);

        RefreshItem(hList, i, g_visible[i]);
    }
}

static int GetSelectedListDesc(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_BM_LIST);
    int idx = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (idx < 0) return -1;

    LVITEMA item = {};
    item.mask  = LVIF_PARAM;
    item.iItem = idx;
    if (!ListView_GetItem(hList, &item)) return -1;
    return (int)item.lParam;
}

// ---------------------------------------------------------------------------
// Assign sub-dialog
// ---------------------------------------------------------------------------
struct AssignDlgData
{
    const char* btnName;
    BtnAction   result;
    bool        ok;
};

static INT_PTR CALLBACK AssignDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        AssignDlgData* data = (AssignDlgData*)lParam;
        SetWindowLongPtr(hwnd, DWLP_USER, (LONG_PTR)data);

        // Show button name in label
        SetDlgItemTextA(hwnd, IDC_BA_LABEL, data->btnName);

        // Set radio buttons from current assignment
        int radioId;
        switch (data->result.type)
        {
        case BtnActionType::None:    radioId = IDC_BA_NONE; break;
        case BtnActionType::Command: radioId = IDC_BA_CMD;  break;
        default:                     radioId = IDC_BA_DEF;  break;
        }
        CheckRadioButton(hwnd, IDC_BA_DEF, IDC_BA_CMD, radioId);

        if (data->result.type == BtnActionType::Command && data->result.cmdId != 0)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", data->result.cmdId);
            SetDlgItemTextA(hwnd, IDC_BA_CMDID, buf);
        }

        // Enable/disable command ID edit
        EnableWindow(GetDlgItem(hwnd, IDC_BA_CMDID),
                     data->result.type == BtnActionType::Command);
        return TRUE;
    }

    case WM_COMMAND:
    {
        AssignDlgData* data = (AssignDlgData*)GetWindowLongPtr(hwnd, DWLP_USER);
        int ctrl = LOWORD(wParam);
        int note = HIWORD(wParam);

        if (ctrl == IDC_BA_DEF || ctrl == IDC_BA_NONE || ctrl == IDC_BA_CMD)
        {
            // Enable/disable command ID edit based on radio selection
            bool cmdEnabled = (IsDlgButtonChecked(hwnd, IDC_BA_CMD) == BST_CHECKED);
            EnableWindow(GetDlgItem(hwnd, IDC_BA_CMDID), cmdEnabled);
        }

        if (ctrl == IDOK)
        {
            if (IsDlgButtonChecked(hwnd, IDC_BA_DEF) == BST_CHECKED)
            {
                data->result.type  = BtnActionType::Default;
                data->result.cmdId = 0;
            }
            else if (IsDlgButtonChecked(hwnd, IDC_BA_NONE) == BST_CHECKED)
            {
                data->result.type  = BtnActionType::None;
                data->result.cmdId = 0;
            }
            else
            {
                // Command
                char buf[32] = {};
                GetDlgItemTextA(hwnd, IDC_BA_CMDID, buf, sizeof(buf));
                int cmdId = atoi(buf);
                if (cmdId <= 0)
                {
                    MessageBoxA(hwnd, "Please enter a valid REAPER Command ID (positive integer).",
                                "Button Map", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                data->result.type  = BtnActionType::Command;
                data->result.cmdId = cmdId;
            }
            data->ok = true;
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (ctrl == IDCANCEL)
        {
            data->ok = false;
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        // suppress unreferenced warning
        (void)note;
        break;
    }
    }
    return FALSE;
}

static void OpenAssignDialog(HWND parent, int descIdx)
{
    if (descIdx < 0 || descIdx >= k_fp16ButtonCount) return;
    const BtnDesc& d = k_fp16Buttons[descIdx];

    // Build label: "Button: <name>  [note 0x??]"
    char label[64];
    snprintf(label, sizeof(label), "Button: %s  [note 0x%02X]", d.name, (unsigned)d.note);

    // Seed with current assignment
    BtnAction cur;
    auto it = g_btnMap.find(d.note);
    if (it != g_btnMap.end()) cur = it->second;

    AssignDlgData data;
    data.btnName = label;
    data.result  = cur;
    data.ok      = false;

    DialogBoxParam(g_hInstance, MAKEINTRESOURCE(IDD_BTN_ASSIGN),
                   parent, AssignDialogProc, (LPARAM)&data);

    if (!data.ok) return;

    if (data.result.type == BtnActionType::Default)
        g_btnMap.erase(d.note);       // remove override; fall through to default
    else
        g_btnMap[d.note] = data.result;

    // Refresh the list row
    HWND hList = GetDlgItem(parent, IDC_BM_LIST);
    int listIdx = -1;
    for (int i = 0; i < (int)g_visible.size(); ++i)
    {
        if (g_visible[i] == descIdx) { listIdx = i; break; }
    }
    if (listIdx >= 0)
        RefreshItem(hList, listIdx, descIdx);
}

// ---------------------------------------------------------------------------
// Main dialog proc
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        // Replace ListView placeholder with a real ListView
        HWND hPlaceholder = GetDlgItem(hwnd, IDC_BM_LIST);
        RECT r;
        GetWindowRect(hPlaceholder, &r);
        MapWindowPoints(nullptr, hwnd, (POINT*)&r, 2);
        DestroyWindow(hPlaceholder);
        CreateListView(hwnd, r);

        // Surface label
        SetDlgItemTextA(hwnd, IDC_BM_SURFACE_LABEL,
                        "FaderPort 16 (Native)  –  button overrides apply only in FP16 mode");

        PopulateFilter(hwnd);
        RebuildList(hwnd);
        return TRUE;
    }

    case WM_COMMAND:
    {
        int ctrl = LOWORD(wParam);
        int notif = HIWORD(wParam);

        if (ctrl == IDC_BM_FILTER && notif == CBN_SELCHANGE)
        {
            RebuildList(hwnd);
            return TRUE;
        }

        if (ctrl == IDC_BM_ASSIGN)
        {
            int di = GetSelectedListDesc(hwnd);
            if (di >= 0) OpenAssignDialog(hwnd, di);
            return TRUE;
        }

        if (ctrl == IDC_BM_DEFAULT)
        {
            int di = GetSelectedListDesc(hwnd);
            if (di >= 0)
            {
                g_btnMap.erase(k_fp16Buttons[di].note);
                // Refresh that row
                HWND hList = GetDlgItem(hwnd, IDC_BM_LIST);
                int listIdx = -1;
                for (int i = 0; i < (int)g_visible.size(); ++i)
                    if (g_visible[i] == di) { listIdx = i; break; }
                if (listIdx >= 0) RefreshItem(hList, listIdx, di);
            }
            return TRUE;
        }

        if (ctrl == IDC_BM_DISABLE)
        {
            int di = GetSelectedListDesc(hwnd);
            if (di >= 0)
            {
                BtnAction a;
                a.type = BtnActionType::None;
                g_btnMap[k_fp16Buttons[di].note] = a;
                HWND hList = GetDlgItem(hwnd, IDC_BM_LIST);
                int listIdx = -1;
                for (int i = 0; i < (int)g_visible.size(); ++i)
                    if (g_visible[i] == di) { listIdx = i; break; }
                if (listIdx >= 0) RefreshItem(hList, listIdx, di);
            }
            return TRUE;
        }

        if (ctrl == IDC_BM_RESETALL)
        {
            if (MessageBoxA(hwnd,
                    "Reset all button assignments to their defaults?",
                    "Button Map", MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
                g_btnMap.clear();
                RebuildList(hwnd);
            }
            return TRUE;
        }

        if (ctrl == IDCANCEL)
        {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }

    case WM_NOTIFY:
    {
        NMHDR* hdr = (NMHDR*)lParam;
        if (hdr->idFrom == IDC_BM_LIST && hdr->code == NM_DBLCLK)
        {
            int di = GetSelectedListDesc(hwnd);
            if (di >= 0) OpenAssignDialog(hwnd, di);
        }
        break;
    }

    case WM_SIZE:
    {
        // Resize the ListView to fill the available area above the button row
        RECT cr;
        GetClientRect(hwnd, &cr);
        int bottom = cr.bottom;

        // Button row height ~20px from bottom; keep some margin
        HWND hAssign = GetDlgItem(hwnd, IDC_BM_ASSIGN);
        RECT br;
        GetWindowRect(hAssign, &br);
        MapWindowPoints(nullptr, hwnd, (POINT*)&br, 2);
        int btnTop = br.top - 4;

        // Filter + label row height
        HWND hFilter = GetDlgItem(hwnd, IDC_BM_FILTER);
        RECT fr;
        GetWindowRect(hFilter, &fr);
        MapWindowPoints(nullptr, hwnd, (POINT*)&fr, 2);
        int listTop = fr.bottom + 4;

        HWND hList = GetDlgItem(hwnd, IDC_BM_LIST);
        SetWindowPos(hList, nullptr, 5, listTop,
                     cr.right - 10, btnTop - listTop, SWP_NOZORDER);

        // Stretch the Reset All button to the right edge
        HWND hReset = GetDlgItem(hwnd, IDC_BM_RESETALL);
        RECT rr;
        GetWindowRect(hReset, &rr);
        MapWindowPoints(nullptr, hwnd, (POINT*)&rr, 2);
        SetWindowPos(hReset, nullptr, cr.right - (rr.right - rr.left) - 5, rr.top,
                     0, 0, SWP_NOZORDER | SWP_NOSIZE);

        // Move Close button to right edge of bottom row
        HWND hClose = GetDlgItem(hwnd, IDCANCEL);
        RECT clr;
        GetWindowRect(hClose, &clr);
        MapWindowPoints(nullptr, hwnd, (POINT*)&clr, 2);
        SetWindowPos(hClose, nullptr, cr.right - (clr.right - clr.left) - 5, clr.top,
                     0, 0, SWP_NOZORDER | SWP_NOSIZE);

        (void)bottom;
        InvalidateRect(hwnd, nullptr, TRUE);
        break;
    }

    case WM_CLOSE:
        EndDialog(hwnd, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}
