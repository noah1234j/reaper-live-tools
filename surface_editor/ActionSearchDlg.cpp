// ---------------------------------------------------------------------------
// ActionSearchDlg.cpp  —  Action picker dialog implementation
//
// Created programmatically (no .rc template) using CreateDialogParam /
// DialogBoxParam pattern consistent with other Live Tools dialogs.
// ---------------------------------------------------------------------------

#include "ActionSearchDlg.h"
#include "ZoneModel.h"
#include "resource.h"
#include "api.h"

#include <commctrl.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "comctl32.lib")

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

struct ActionEntry {
    int         cmdId;    // 0 for CSI built-ins
    std::string name;
    std::string token;    // what gets written to the .zon file
    bool        isCSI;
};

struct DlgState {
    std::vector<ActionEntry> allActions;
    std::vector<int>         filtered;   // indices into allActions
    int                      activeTab;  // 0=REAPER, 1=CSI
    std::string              result;
    bool                     accepted;
    HINSTANCE                hInst;

    // Control handles
    HWND hTab;
    HWND hSearch;
    HWND hList;
    HWND hCustom;
    HWND hOk;
    HWND hCancel;
};

// ---------------------------------------------------------------------------
// Populate the action list from REAPER API + CSI built-ins
// ---------------------------------------------------------------------------

static void BuildActionList(DlgState* ds)
{
    ds->allActions.clear();

    // --- REAPER actions (section 0 = main) ---
    KbdSectionInfo* sec = SectionFromUniqueID ? SectionFromUniqueID(0) : nullptr;
    if (sec && kbd_enumerateActions && kbd_getTextFromCmd)
    {
        int idx = 0;
        const char* namePtr = nullptr;
        int cmd = 0;
        while ((cmd = kbd_enumerateActions(sec, idx, &namePtr)) != 0)
        {
            ActionEntry ae;
            ae.cmdId  = cmd;
            ae.isCSI  = false;
            ae.name   = namePtr ? namePtr : "";
            // Build the token: "Reaper <cmdId>" for native actions
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Reaper %d", cmd);
            ae.token  = buf;
            ds->allActions.push_back(std::move(ae));
            ++idx;
        }
    }

    // --- CSI built-in actions ---
    int csiCount = 0;
    const SurfaceEditor::CsiAction* csiList = SurfaceEditor::GetCsiActions(&csiCount);
    for (int i = 0; i < csiCount; ++i)
    {
        ActionEntry ae;
        ae.cmdId  = 0;
        ae.isCSI  = true;
        ae.name   = csiList[i].name;
        ae.token  = csiList[i].token;
        ds->allActions.push_back(std::move(ae));
    }
}

// ---------------------------------------------------------------------------
// Repopulate the ListView based on current search text and active tab
// ---------------------------------------------------------------------------

static void ApplyFilter(DlgState* ds)
{
    ds->filtered.clear();

    char filter[256] = {};
    GetWindowTextA(ds->hSearch, filter, sizeof(filter));
    // Convert filter to lower-case for case-insensitive match
    for (char& c : filter) c = (char)tolower((unsigned char)c);

    bool showCSI    = (ds->activeTab == 1);
    bool showReaper = (ds->activeTab == 0);

    for (int i = 0; i < (int)ds->allActions.size(); ++i)
    {
        const ActionEntry& ae = ds->allActions[i];
        if (showCSI    && !ae.isCSI)   continue;
        if (showReaper &&  ae.isCSI)   continue;

        if (filter[0])
        {
            // Simple substring match on lowercased name and token
            char name[512] = {};
            std::strncpy(name, ae.name.c_str(), sizeof(name) - 1);
            for (char& c : name) c = (char)tolower((unsigned char)c);

            char tok[256] = {};
            std::strncpy(tok, ae.token.c_str(), sizeof(tok) - 1);
            for (char& c : tok) c = (char)tolower((unsigned char)c);

            if (!std::strstr(name, filter) && !std::strstr(tok, filter))
                continue;
        }

        ds->filtered.push_back(i);
    }

    // Rebuild ListView
    ListView_DeleteAllItems(ds->hList);
    int row = 0;
    for (int idx : ds->filtered)
    {
        const ActionEntry& ae = ds->allActions[idx];
        LVITEMA item = {};
        item.mask    = LVIF_TEXT | LVIF_PARAM;
        item.iItem   = row;
        item.lParam  = (LPARAM)idx;
        item.pszText = const_cast<char*>(ae.name.c_str());
        ListView_InsertItem(ds->hList, &item);

        LVITEMA sub = {};
        sub.mask     = LVIF_TEXT;
        sub.iItem    = row;
        sub.iSubItem = 1;
        sub.pszText  = const_cast<char*>(ae.token.c_str());
        ListView_SetItem(ds->hList, &sub);

        ++row;
    }
}

// ---------------------------------------------------------------------------
// Commit selected action to result string
// ---------------------------------------------------------------------------

static void CommitSelection(HWND hwnd, DlgState* ds)
{
    // Priority: if the ListView has a selected item, use it
    int sel = ListView_GetNextItem(ds->hList, -1, LVNI_SELECTED);
    if (sel >= 0)
    {
        char buf[256] = {};
        ListView_GetItemText(ds->hList, sel, 1, buf, sizeof(buf));
        ds->result = buf;
    }
    else
    {
        // Fall back to custom field
        char buf[512] = {};
        GetWindowTextA(ds->hCustom, buf, sizeof(buf));
        ds->result = buf;
    }
    ds->accepted = true;
    EndDialog(hwnd, IDOK);
}

// ---------------------------------------------------------------------------
// Dialog procedure (built as a static proc, state in DWLP_USER)
// ---------------------------------------------------------------------------

static INT_PTR CALLBACK ActionSearchProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DlgState* ds = reinterpret_cast<DlgState*>(GetWindowLongPtrA(hwnd, DWLP_USER));

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ds = reinterpret_cast<DlgState*>(lp);
        SetWindowLongPtrA(hwnd, DWLP_USER, (LONG_PTR)ds);

        // Cache control handles
        ds->hTab    = GetDlgItem(hwnd, IDC_AS_TAB);
        ds->hSearch = GetDlgItem(hwnd, IDC_AS_SEARCH);
        ds->hList   = GetDlgItem(hwnd, IDC_AS_LIST);
        ds->hCustom = GetDlgItem(hwnd, IDC_AS_CUSTOM);
        ds->hOk     = GetDlgItem(hwnd, IDC_AS_OK);
        ds->hCancel = GetDlgItem(hwnd, IDC_AS_CANCEL);

        // Set up tab control
        TCITEMA ti = {};
        ti.mask    = TCIF_TEXT;
        ti.pszText = const_cast<char*>("REAPER Actions");
        TabCtrl_InsertItem(ds->hTab, 0, &ti);
        ti.pszText = const_cast<char*>("CSI Actions");
        TabCtrl_InsertItem(ds->hTab, 1, &ti);

        // Set up ListView columns
        LVCOLUMNA col = {};
        col.mask   = LVCF_TEXT | LVCF_WIDTH;
        col.pszText= const_cast<char*>("Name");
        col.cx     = 320;
        ListView_InsertColumn(ds->hList, 0, &col);
        col.pszText= const_cast<char*>("Token");
        col.cx     = 200;
        ListView_InsertColumn(ds->hList, 1, &col);

        ListView_SetExtendedListViewStyle(ds->hList,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        // Pre-fill custom field if initial provided
        // (initial is stored in result before dialog opens)
        if (!ds->result.empty())
            SetWindowTextA(ds->hCustom, ds->result.c_str());
        ds->result.clear();

        BuildActionList(ds);
        ApplyFilter(ds);

        // Focus search field
        SetFocus(ds->hSearch);
        return FALSE;
    }

    case WM_COMMAND:
    {
        if (!ds) return FALSE;
        int id   = LOWORD(wp);
        int note = HIWORD(wp);

        if (id == IDC_AS_OK || id == IDOK)
        {
            CommitSelection(hwnd, ds);
            return TRUE;
        }
        if (id == IDC_AS_CANCEL || id == IDCANCEL)
        {
            ds->accepted = false;
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        if (id == IDC_AS_SEARCH && note == EN_CHANGE)
        {
            ApplyFilter(ds);
            return TRUE;
        }
        return FALSE;
    }

    case WM_NOTIFY:
    {
        if (!ds) return FALSE;
        NMHDR* nm = (NMHDR*)lp;

        // Tab switch
        if (nm->hwndFrom == ds->hTab && nm->code == TCN_SELCHANGE)
        {
            ds->activeTab = TabCtrl_GetCurSel(ds->hTab);
            ApplyFilter(ds);
            return TRUE;
        }

        // ListView double-click → commit
        if (nm->hwndFrom == ds->hList && nm->code == NM_DBLCLK)
        {
            CommitSelection(hwnd, ds);
            return TRUE;
        }

        // ListView selection changed → copy token to custom field
        if (nm->hwndFrom == ds->hList &&
            (nm->code == LVN_ITEMCHANGED || nm->code == NM_CLICK))
        {
            int sel = ListView_GetNextItem(ds->hList, -1, LVNI_SELECTED);
            if (sel >= 0)
            {
                char buf[256] = {};
                ListView_GetItemText(ds->hList, sel, 1, buf, sizeof(buf));
                SetWindowTextA(ds->hCustom, buf);
            }
            return TRUE;
        }
        return FALSE;
    }

    // Allow Enter to commit from within the list or search field
    case WM_KEYDOWN:
        if (wp == VK_RETURN && ds)
        {
            CommitSelection(hwnd, ds);
            return TRUE;
        }
        break;
    }

    return FALSE;
}

// ---------------------------------------------------------------------------
// Dialog template — built in memory (no .rc entry needed)
// ---------------------------------------------------------------------------
//
// Layout (580 × 420 dialog units):
//   [Tab control            — 0,0  → 580,20  ]
//   [Search edit            — 0,24 → 580,14  ]
//   [ListView               — 0,40 → 580,310 ]
//   [Label "Custom:" static — 0,354→ 60,12   ]
//   [Custom edit            — 62,354→518,12  ]
//   [OK button              — 420,374→80,14  ]
//   [Cancel button          — 506,374→74,14  ]

static const DLGTEMPLATE* BuildDialogTemplate()
{
    // Use a static buffer — built once.
    // We'll use the DLGTEMPLATEEX approach via raw memory.
    // For simplicity we just use CreateDialog with DS_SETFONT etc but we can
    // do it even simpler: just allocate a minimal template.
    // The dialog controls are created manually in WM_INITDIALOG via resource IDs.

    // Minimal DLGTEMPLATE (no controls in template – all created in WM_INITDIALOG)
    static struct {
        DLGTEMPLATE dt;
        WORD        menu, cls, title[16];
        WORD        pointsize;
        WCHAR       font[12];
    } tmpl = {};

    tmpl.dt.style           = DS_SETFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
    tmpl.dt.dwExtendedStyle = 0;
    tmpl.dt.cdit            = 0;
    tmpl.dt.x               = 0;
    tmpl.dt.y               = 0;
    tmpl.dt.cx              = 440;
    tmpl.dt.cy              = 300;
    // menu  = 0 (no menu)
    // class = 0 (default dialog class)
    // title
    static const wchar_t kTitle[] = L"Assign Action";
    for (int i = 0; i < 15 && kTitle[i]; ++i)
        tmpl.title[i] = (WORD)kTitle[i];

    tmpl.pointsize = 9;
    static const wchar_t kFont[] = L"Segoe UI";
    for (int i = 0; i < 11 && kFont[i]; ++i)
        tmpl.font[i] = (WCHAR)kFont[i];

    return &tmpl.dt;
}

// Create child controls in WM_INITDIALOG is done in the proc.
// But since we have zero controls in the DLGTEMPLATE we need to create them
// ourselves in WM_INITDIALOG.  We embed this in a second pass of WM_INITDIALOG.

// We'll actually use a separate creation path — see ActionSearchDlg_Show below.

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static HINSTANCE g_hInst = nullptr;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ActionSearchDlg_Init(HINSTANCE hInst)
{
    g_hInst = hInst;
}

// Create the dialog window and all its child controls programmatically
static HWND CreateActionSearchDialog(HWND parent, DlgState* ds)
{
    // Create a plain WS_OVERLAPPEDWINDOW as a modal-style dialog.
    // We use CreateDialogIndirectParam with a zero-control template so we have
    // full control over child layout.

    HWND hwnd = CreateDialogIndirectParamA(
        g_hInst,
        BuildDialogTemplate(),
        parent,
        ActionSearchProc,
        (LPARAM)ds);

    if (!hwnd) return nullptr;

    // Now create all child controls
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int pad = 6;

    // Tab control
    HWND hTab = CreateWindowExA(0, WC_TABCONTROLA, nullptr,
        WS_CHILD | WS_VISIBLE | TCS_TABS,
        pad, pad, W - 2*pad, 24,
        hwnd, (HMENU)(INT_PTR)IDC_AS_TAB, g_hInst, nullptr);

    // Search edit
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        pad, pad + 30, W - 2*pad, 20,
        hwnd, (HMENU)(INT_PTR)IDC_AS_SEARCH, g_hInst, nullptr);
    SetWindowTextA(GetDlgItem(hwnd, IDC_AS_SEARCH), "");

    // Hint text placeholder — set cue banner if supported
    // (We can't use EM_SETCUEBANNER easily on XP, skip for now)

    // ListView
    HWND hList = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, nullptr,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        pad, pad + 56, W - 2*pad, H - 56 - 60,
        hwnd, (HMENU)(INT_PTR)IDC_AS_LIST, g_hInst, nullptr);

    // Custom text field label
    int customY = H - 52;
    CreateWindowExA(0, "STATIC", "Custom action:",
        WS_CHILD | WS_VISIBLE,
        pad, customY + 3, 90, 18,
        hwnd, nullptr, g_hInst, nullptr);

    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        pad + 96, customY, W - 2*pad - 96, 20,
        hwnd, (HMENU)(INT_PTR)IDC_AS_CUSTOM, g_hInst, nullptr);

    // Buttons
    int btnY = H - 28;
    CreateWindowExA(0, "BUTTON", "Assign",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        W - 2*pad - 160, btnY, 74, 22,
        hwnd, (HMENU)(INT_PTR)IDC_AS_OK, g_hInst, nullptr);

    CreateWindowExA(0, "BUTTON", "Cancel",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        W - 2*pad - 80, btnY, 74, 22,
        hwnd, (HMENU)(INT_PTR)IDC_AS_CANCEL, g_hInst, nullptr);

    (void)hTab; (void)hList; // used via GetDlgItem in WM_INITDIALOG

    return hwnd;
}

std::string ActionSearchDlg_Show(HWND parent, const std::string& initial)
{
    if (!g_hInst) return initial;

    DlgState ds;
    ds.activeTab = 0;
    ds.accepted  = false;
    ds.result    = initial; // will be moved to custom field in WM_INITDIALOG
    ds.hInst     = g_hInst;

    // Run as a modal dialog via DialogBoxIndirectParam
    INT_PTR ret = DialogBoxIndirectParamA(
        g_hInst,
        BuildDialogTemplate(),
        parent,
        ActionSearchProc,
        (LPARAM)&ds);

    if (ret == IDOK && ds.accepted)
        return ds.result;
    return "";
}
