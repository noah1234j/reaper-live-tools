// ---------------------------------------------------------------------------
// DcaWnd.cpp  –  DCA Groups window  (vertical list view)
//
// Layout:
//   [Top toolbar: "+ Add DCA" button on the right]
//   [Header row: column labels]
//   [Scrollable list: one row per DCA group]
//
// Each row:  [Name] [G##] [V][P][W][M][S][R] [N trk] [Assign Sel][Spill][x]
//
// Right-click anywhere:
//   • (If on a row)  Rename..., Assign Selected Tracks, Spill, Delete
//   • Separator
//   • Set Start Group...
//   • Default flags: (checkable: Vol / Pan / Width / Mute / Solo / Rec Arm)
//
// Single-click on the Name cell starts an inline rename edit.
// F2 or right-click > Rename also starts inline rename.
// ---------------------------------------------------------------------------

#include "DcaWnd.h"
#include "api.h"
#include "DcaEngine.h"
#include "DcaGroup.h"
#include "resource.h"

#include <commctrl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <windowsx.h>

// ---------------------------------------------------------------------------
// Layout constants
// ---------------------------------------------------------------------------

static const char* k_ExtSection  = "reaper_transitions";
static const char* k_DefFlagsKey = "dca_default_flags";
static const char* k_StartGrpKey = "dca_start_group";

static const int kTopBarH = 26;   // height of the top toolbar
static const int kHdrH    = 17;   // height of the column header row
static const int kRowH    = 24;   // height of each DCA row

// Column left-edge positions and widths
static const int kCx_Name   = 0;   static const int kCw_Name   = 150;
static const int kCx_Grp    = 150; static const int kCw_Grp    = 38;
static const int kCx_FlagV  = 188; static const int kCw_Flag   = 20;
static const int kCx_FlagP  = 208;
static const int kCx_FlagW  = 228;
static const int kCx_FlagM  = 248;
static const int kCx_FlagS  = 268;
static const int kCx_FlagR  = 288;
static const int kCx_Tracks = 308; static const int kCw_Tracks = 42;
static const int kCx_Assign = 350; static const int kCw_Assign = 82;
static const int kCx_Spill  = 432; static const int kCw_Spill  = 52;
static const int kCx_Del    = 484; static const int kCw_Del    = 28;

// Context menu command IDs (local, not in resource.h)
enum {
    kMID_Rename        = 3300,
    kMID_Assign        = 3301,
    kMID_Spill         = 3302,
    kMID_Delete        = 3303,
    kMID_SetStartGroup = 3304,
    kMID_DefVol        = 3305,
    kMID_DefPan        = 3306,
    kMID_DefWidth      = 3307,
    kMID_DefMute       = 3308,
    kMID_DefSolo       = 3309,
    kMID_DefRecArm     = 3310,
};

// Row hit zones
enum RowHit {
    kRH_None   = -1,
    kRH_Name   =  0,
    kRH_Grp    =  1,
    kRH_FlagV  =  2,
    kRH_FlagP  =  3,
    kRH_FlagW  =  4,
    kRH_FlagM  =  5,
    kRH_FlagS  =  6,
    kRH_FlagR  =  7,
    kRH_Tracks =  8,
    kRH_Assign =  9,
    kRH_Spill  = 10,
    kRH_Del    = 11,
};

// ---------------------------------------------------------------------------
// Saved settings
// ---------------------------------------------------------------------------

static uint32_t g_defaultFlags = DCA_VOL | DCA_PAN | DCA_MUTE | DCA_SOLO;
static int      g_startGroup   = 33;   // first group in the safe 33-64 range

static void LoadSettings()
{
    const char* v;
    v = GetExtState(k_ExtSection, k_DefFlagsKey);
    if (v && *v)
        g_defaultFlags = (uint32_t)atoi(v);

    v = GetExtState(k_ExtSection, k_StartGrpKey);
    if (v && *v)
    {
        int sg = atoi(v);
        if (sg >= 1 && sg <= 128)
            g_startGroup = sg;
    }
}

static void SaveSettings()
{
    // Settings are project-specific; persisted via SaveExtensionConfig -> DcaWnd_SaveSettings.
    MarkProjectDirty(nullptr);
}

void DcaWnd_ResetSettings()
{
    g_defaultFlags = DCA_VOL | DCA_PAN | DCA_MUTE | DCA_SOLO;
    g_startGroup   = 33;
}

bool DcaWnd_ProcessSettingsLine(const char* line)
{
    if (!line || strncmp(line, "LTDCASET ", 9) != 0) return false;
    int flags    = (int)(DCA_VOL | DCA_PAN | DCA_MUTE | DCA_SOLO);
    int startgrp = 33;
    sscanf(line + 9, "flags=%d startgrp=%d", &flags, &startgrp);
    g_defaultFlags = (uint32_t)flags;
    if (startgrp >= 1 && startgrp <= 128) g_startGroup = startgrp;
    return true;
}

void DcaWnd_SaveSettings(ProjectStateContext* ctx)
{
    ctx->AddLine("LTDCASET flags=%d startgrp=%d", (int)g_defaultFlags, g_startGroup);
}

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static HINSTANCE g_hInst    = nullptr;
static HWND      g_dlg      = nullptr;   // main dialog (IDD_DCA)
static HWND      g_listWnd  = nullptr;   // custom list-area child HWND
static HWND      g_addBtn   = nullptr;   // "+ Add DCA" button in top bar
static bool      g_skipNextCtxMenu = false;
static HWND      g_nameEdit = nullptr;   // hidden inline name-edit control
static int       g_editRow  = -1;        // row being edited (-1 = none)

static int       s_scrollY  = 0;         // vertical scroll offset (pixels)
static int       s_hotRow   = -1;        // row index under cursor
static int       s_hotZone  = kRH_None;  // zone within hot row

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static LRESULT CALLBACK ListWndProc(HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK DcaDlgProc(HWND, UINT, WPARAM, LPARAM);
static void CreateListArea(HWND dlg);
static void RepositionListArea(HWND dlg);
static void UpdateScrollInfo();
static void DrawList(HDC hdc, int cw, int ch);
static void DrawHeader(HDC hdc, int cw);
static void DrawRow(HDC hdc, int rowIdx, int y, int cw, bool isHot, int hotZone);
static RowHit HitTestRow(int mouseX, int mouseY, int& outRow);
static void CommitNameEdit();
static void StartNameEdit(int row);
static void RefreshListView(HWND dlg);
static void ShowContextMenu(HWND hwnd, int row, int screenX, int screenY);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void DcaWnd_Init(HINSTANCE hInstance)
{
    g_hInst = hInstance;
    DcaWnd_ResetSettings();  // defaults; actual values loaded per-project via ProcessLine

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);
}

void DcaWnd_Cleanup()
{
    if (g_dlg && IsWindow(g_dlg))
    {
        DestroyWindow(g_dlg);
        g_dlg = nullptr;
    }
}

void DcaWnd_ShowHide()
{
    if (!g_dlg || !IsWindow(g_dlg))
    {
        HWND hMain = GetMainHwnd ? GetMainHwnd() : nullptr;
        g_dlg = CreateDialogParam(g_hInst, MAKEINTRESOURCE(IDD_DCA),
                                   hMain, DcaDlgProc, 0);
        if (g_dlg)
            ShowWindow(g_dlg, SW_SHOW);
        return;
    }
    IsWindowVisible(g_dlg)
        ? ShowWindow(g_dlg, SW_HIDE)
        : ShowWindow(g_dlg, SW_SHOW);
}

int DcaWnd_IsVisible()
{
    return (g_dlg && IsWindow(g_dlg) && IsWindowVisible(g_dlg)) ? 1 : 0;
}

void DcaWnd_Refresh()
{
    if (g_dlg && IsWindow(g_dlg))
        RefreshListView(g_dlg);
}

void DcaWnd_OnProjectLoad()
{
    s_scrollY = 0;
    g_editRow = -1;
    if (g_nameEdit && IsWindow(g_nameEdit))
        ShowWindow(g_nameEdit, SW_HIDE);
    DcaEngine_ReapplyAll();
    DcaWnd_Refresh();
}

// ---------------------------------------------------------------------------
// CreateListArea / RepositionListArea
// ---------------------------------------------------------------------------

static void CreateListArea(HWND dlg)
{
    RECT rc;
    GetClientRect(dlg, &rc);
    int y = kTopBarH;
    int w = rc.right  - rc.left;
    int h = rc.bottom - rc.top  - y;
    if (h < 1) h = 1;

    g_listWnd = CreateWindowExA(0, "LT_DcaListArea", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS,
        0, y, w, h,
        dlg, (HMENU)(UINT_PTR)IDC_DCA_SCROLL, g_hInst, nullptr);

    // Hidden inline rename edit box (child of the list area)
    g_nameEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_CLIPSIBLINGS | ES_AUTOHSCROLL,
        0, 0, kCw_Name - 2, kRowH - 2,
        g_listWnd, (HMENU)9999, g_hInst, nullptr);

    HFONT hf = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (hf)
        SendMessage(g_nameEdit, WM_SETFONT, (WPARAM)hf, FALSE);

    // "+ Add DCA" button in top toolbar
    g_addBtn = CreateWindowExA(0, "BUTTON", "+ Add DCA",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        rc.right - 92, 3, 84, 20,
        dlg, (HMENU)(UINT_PTR)IDC_DCA_ADDSTRIP, g_hInst, nullptr);
    if (hf)
        SendMessage(g_addBtn, WM_SETFONT, (WPARAM)hf, FALSE);

    UpdateScrollInfo();
}

static void RepositionListArea(HWND dlg)
{
    if (!g_listWnd || !IsWindow(g_listWnd)) return;

    RECT rc;
    GetClientRect(dlg, &rc);
    int y = kTopBarH;
    int w = rc.right  - rc.left;
    int h = rc.bottom - rc.top  - y;
    if (h < 1) h = 1;
    SetWindowPos(g_listWnd, nullptr, 0, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);

    if (g_addBtn && IsWindow(g_addBtn))
        SetWindowPos(g_addBtn, nullptr,
                     rc.right - 92, 3, 84, 20, SWP_NOZORDER | SWP_NOACTIVATE);

    UpdateScrollInfo();
}

// ---------------------------------------------------------------------------
// UpdateScrollInfo
// ---------------------------------------------------------------------------

static void UpdateScrollInfo()
{
    if (!g_listWnd || !IsWindow(g_listWnd)) return;

    RECT rc;
    GetClientRect(g_listWnd, &rc);
    int clientH = rc.bottom;

    int contentH  = kHdrH + (int)g_dcaGroups.size() * kRowH;
    int maxScroll = contentH - clientH;
    if (maxScroll < 0) maxScroll = 0;
    if (s_scrollY > maxScroll) s_scrollY = maxScroll;

    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin   = 0;
    si.nMax   = (contentH > 0) ? (contentH - 1) : 0;
    si.nPage  = (UINT)clientH;
    si.nPos   = s_scrollY;
    SetScrollInfo(g_listWnd, SB_VERT, &si, TRUE);
}

// ---------------------------------------------------------------------------
// CommitNameEdit / StartNameEdit
// ---------------------------------------------------------------------------

static void CommitNameEdit()
{
    if (g_editRow < 0 || g_editRow >= (int)g_dcaGroups.size()) return;
    if (!g_nameEdit || !IsWindow(g_nameEdit)) return;
    if (!IsWindowVisible(g_nameEdit)) return;

    char buf[256] = {};
    GetWindowTextA(g_nameEdit, buf, (int)sizeof(buf));
    if (buf[0])
    {
        g_dcaGroups[(size_t)g_editRow]->name = buf;
        MediaTrack* lead =
            DcaEngine_GetControlTrack(g_dcaGroups[(size_t)g_editRow].get());
        if (lead)
            GetSetMediaTrackInfo_String(lead, "P_NAME", buf, true);
    }

    ShowWindow(g_nameEdit, SW_HIDE);
    g_editRow = -1;
    DcaWnd_Refresh();
}

static void StartNameEdit(int row)
{
    if (row < 0 || row >= (int)g_dcaGroups.size()) return;
    if (!g_nameEdit || !IsWindow(g_nameEdit)) return;

    CommitNameEdit();   // flush any prior edit

    int y = kHdrH + row * kRowH - s_scrollY + 1;
    SetWindowPos(g_nameEdit, HWND_TOP,
                 kCx_Name + 1, y,
                 kCw_Name - 2, kRowH - 2,
                 SWP_SHOWWINDOW);

    SetWindowTextA(g_nameEdit, g_dcaGroups[(size_t)row]->name.c_str());
    SetFocus(g_nameEdit);
    SendMessage(g_nameEdit, EM_SETSEL, 0, -1);
    g_editRow = row;
}

// ---------------------------------------------------------------------------
// HitTestRow
// mouseX/Y in list-area client coordinates.
// Returns zone; sets outRow = row index or -1.
// ---------------------------------------------------------------------------

static RowHit HitTestRow(int mouseX, int mouseY, int& outRow)
{
    outRow = -1;

    int worldY = mouseY + s_scrollY;

    if (worldY < kHdrH)
        return kRH_None;

    int row = (worldY - kHdrH) / kRowH;
    if (row < 0 || row >= (int)g_dcaGroups.size())
        return kRH_None;

    outRow = row;

    // Check columns right-to-left so narrow ones take priority
    if (mouseX >= kCx_Del    && mouseX < kCx_Del    + kCw_Del)    return kRH_Del;
    if (mouseX >= kCx_Spill  && mouseX < kCx_Spill  + kCw_Spill)  return kRH_Spill;
    if (mouseX >= kCx_Assign && mouseX < kCx_Assign + kCw_Assign)  return kRH_Assign;
    if (mouseX >= kCx_Tracks && mouseX < kCx_Tracks + kCw_Tracks)  return kRH_Tracks;
    if (mouseX >= kCx_FlagR  && mouseX < kCx_FlagR  + kCw_Flag)    return kRH_FlagR;
    if (mouseX >= kCx_FlagS  && mouseX < kCx_FlagS  + kCw_Flag)    return kRH_FlagS;
    if (mouseX >= kCx_FlagM  && mouseX < kCx_FlagM  + kCw_Flag)    return kRH_FlagM;
    if (mouseX >= kCx_FlagW  && mouseX < kCx_FlagW  + kCw_Flag)    return kRH_FlagW;
    if (mouseX >= kCx_FlagP  && mouseX < kCx_FlagP  + kCw_Flag)    return kRH_FlagP;
    if (mouseX >= kCx_FlagV  && mouseX < kCx_FlagV  + kCw_Flag)    return kRH_FlagV;
    if (mouseX >= kCx_Grp    && mouseX < kCx_Grp    + kCw_Grp)     return kRH_Grp;
    if (mouseX >= kCx_Name   && mouseX < kCx_Name   + kCw_Name)    return kRH_Name;

    return kRH_None;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

// REAPER-palette neutral darks
static const COLORREF kC_BgMain    = RGB(26,  26,  26);   // main background
static const COLORREF kC_BgEven    = RGB(32,  32,  32);   // even row
static const COLORREF kC_BgOdd     = RGB(37,  37,  37);   // odd row
static const COLORREF kC_BgHot     = RGB(44,  54,  68);   // hover (muted blue-gray)
static const COLORREF kC_BgHdr     = RGB(46,  46,  46);   // column header strip
static const COLORREF kC_BgToolbar = RGB(52,  52,  52);   // top toolbar

static const COLORREF kC_TextMain  = RGB(220, 220, 220);  // primary text
static const COLORREF kC_TextLight = RGB(220, 220, 220);  // alias
static const COLORREF kC_TextDim   = RGB(128, 128, 128);  // secondary text
static const COLORREF kC_TextHdr   = RGB(158, 158, 158);  // column header labels

// Flat button palette
static const COLORREF kC_BtnBorder = RGB(18,  18,  18);   // button border
static const COLORREF kC_BtnNorm   = RGB(52,  52,  52);   // normal button fill
static const COLORREF kC_BtnHot    = RGB(66,  66,  66);   // hover button fill

// Flag toggle states
static const COLORREF kC_FlagOnBg  = RGB(72,  82,  96);   // lit fill (blue-gray)
static const COLORREF kC_FlagOnTc  = RGB(236, 236, 236);  // lit text
static const COLORREF kC_FlagOffBg = RGB(42,  42,  42);   // unlit fill
static const COLORREF kC_FlagOffTc = RGB(100, 100, 100);  // unlit text
static const COLORREF kC_FlagOn    = RGB(72,  82,  96);   // legacy alias
static const COLORREF kC_FlagOff   = RGB(42,  42,  42);   // legacy alias

// Action button states
static const COLORREF kC_AssignBg  = RGB(48,  50,  58);   // assign normal fill
static const COLORREF kC_AssignTc  = RGB(180, 200, 240);  // assign text
static const COLORREF kC_SpillOnBg = RGB(88,  66,  12);   // spill active fill
static const COLORREF kC_SpillOnTc = RGB(248, 196, 50);   // spill active text
static const COLORREF kC_SpillOffTc= RGB(108, 100, 56);   // spill inactive text
static const COLORREF kC_SpillOn   = RGB(88,  66,  12);   // legacy alias
static const COLORREF kC_SpillOff  = RGB(108, 100, 56);   // legacy alias
static const COLORREF kC_DelTc     = RGB(165, 75,  75);   // delete text
static const COLORREF kC_DelHotBg  = RGB(100, 28,  28);   // delete hover fill
static const COLORREF kC_DelHotTc  = RGB(255, 165, 165);  // delete hover text
static const COLORREF kC_DelBg     = RGB(52,  52,  52);   // legacy alias

// Structure
static const COLORREF kC_NameBg    = RGB(22,  22,  22);   // name field inset
static const COLORREF kC_GridLine  = RGB(18,  18,  18);   // row separator

static void FillRc(HDC hdc, int x, int y, int w, int h, COLORREF c)
{
    RECT rc = { x, y, x + w, y + h };
    HBRUSH hbr = CreateSolidBrush(c);
    FillRect(hdc, &rc, hbr);
    DeleteObject(hbr);
}

static void DrawCell(HDC hdc, int x, int y, int w, int h,
                     const char* text, COLORREF tc,
                     UINT dtFlags = DT_CENTER | DT_VCENTER)
{
    RECT rc = { x + 2, y, x + w - 2, y + h };
    SetTextColor(hdc, tc);
    DrawTextA(hdc, text, -1, &rc,
              DT_SINGLELINE | dtFlags | DT_NOPREFIX | DT_END_ELLIPSIS);
}

// Draw a flat bordered button (REAPER style: solid fill + 1px border via Rectangle)
static void DrawFlatBtn(HDC hdc, int x, int y, int w, int h,
                         COLORREF fillC, COLORREF borderC)
{
    HPEN   hp  = CreatePen(PS_SOLID, 1, borderC);
    HBRUSH hbr = CreateSolidBrush(fillC);
    HPEN   hpo = (HPEN)SelectObject(hdc, hp);
    HBRUSH hbo = (HBRUSH)SelectObject(hdc, hbr);
    Rectangle(hdc, x, y, x + w, y + h);
    SelectObject(hdc, hpo);
    SelectObject(hdc, hbo);
    DeleteObject(hp);
    DeleteObject(hbr);
}

static void DrawFlagButton(HDC hdc, int x, int y, const char* label,
                            bool active, bool hot)
{
    int bx = x + 1, by = y + 3, bw = kCw_Flag - 2, bh = kRowH - 6;
    COLORREF fillC = active ? kC_FlagOnBg
                            : (hot ? kC_BtnHot : kC_FlagOffBg);
    DrawFlatBtn(hdc, bx, by, bw, bh, fillC, kC_BtnBorder);
    RECT rc = { bx + 1, by, bx + bw - 1, by + bh };
    COLORREF tc = active ? kC_FlagOnTc
                         : (hot ? kC_TextMain : kC_FlagOffTc);
    SetTextColor(hdc, tc);
    DrawTextA(hdc, label, -1, &rc,
              DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
}

static void DrawHeader(HDC hdc, int cw)
{
    FillRc(hdc, 0, 0, cw, kHdrH, kC_BgHdr);
    SetBkMode(hdc, TRANSPARENT);

    DrawCell(hdc, kCx_Name,   0, kCw_Name,   kHdrH, "Name",        kC_TextHdr, DT_LEFT  | DT_VCENTER);
    DrawCell(hdc, kCx_Grp,    0, kCw_Grp,    kHdrH, "Grp",         kC_TextHdr);
    DrawCell(hdc, kCx_FlagV,  0, kCw_Flag,   kHdrH, "V",           kC_TextHdr);
    DrawCell(hdc, kCx_FlagP,  0, kCw_Flag,   kHdrH, "P",           kC_TextHdr);
    DrawCell(hdc, kCx_FlagW,  0, kCw_Flag,   kHdrH, "W",           kC_TextHdr);
    DrawCell(hdc, kCx_FlagM,  0, kCw_Flag,   kHdrH, "M",           kC_TextHdr);
    DrawCell(hdc, kCx_FlagS,  0, kCw_Flag,   kHdrH, "S",           kC_TextHdr);
    DrawCell(hdc, kCx_FlagR,  0, kCw_Flag,   kHdrH, "R",           kC_TextHdr);
    DrawCell(hdc, kCx_Tracks, 0, kCw_Tracks, kHdrH, "Trk",         kC_TextHdr);
    DrawCell(hdc, kCx_Assign, 0, kCw_Assign, kHdrH, "Assign Sel",  kC_TextHdr);
    DrawCell(hdc, kCx_Spill,  0, kCw_Spill,  kHdrH, "Spill",       kC_TextHdr);
    DrawCell(hdc, kCx_Del,    0, kCw_Del,    kHdrH, "Del",         kC_TextHdr);

    // Bottom separator line
    HPEN hp = CreatePen(PS_SOLID, 1, kC_GridLine);
    HPEN ho = (HPEN)SelectObject(hdc, hp);
    MoveToEx(hdc, 0, kHdrH - 1, nullptr);
    LineTo(hdc, cw, kHdrH - 1);
    SelectObject(hdc, ho);
    DeleteObject(hp);
}

static void DrawRow(HDC hdc, int rowIdx, int y, int cw, bool isHot, int hotZone)
{
    const DcaGroup& dca = *g_dcaGroups[(size_t)rowIdx];

    // Row background
    COLORREF bg = isHot ? kC_BgHot
                        : ((rowIdx & 1) ? kC_BgOdd : kC_BgEven);
    FillRc(hdc, 0, y, cw, kRowH, bg);

    SetBkMode(hdc, TRANSPARENT);

    // ── Name cell ────────────────────────────────────────────────────────────
    if (g_editRow != rowIdx)
    {
        FillRc(hdc, kCx_Name + 1, y + 1, kCw_Name - 2, kRowH - 2, kC_NameBg);
        RECT rcN = { kCx_Name + 5, y, kCx_Name + kCw_Name - 2, y + kRowH };
        SetTextColor(hdc, kC_TextLight);
        DrawTextA(hdc, dca.name.c_str(), -1, &rcN,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    // ── Group number ──────────────────────────────────────────────────────────
    char grpBuf[12];
    snprintf(grpBuf, sizeof(grpBuf), "G%d", dca.groupNum);
    DrawCell(hdc, kCx_Grp, y, kCw_Grp, kRowH, grpBuf, kC_TextDim);

    // ── Flag toggle buttons ───────────────────────────────────────────────────
    static const struct { int x; DcaFlag flag; const char* lbl; int zone; } kF[6] = {
        { kCx_FlagV, DCA_VOL,    "V", kRH_FlagV },
        { kCx_FlagP, DCA_PAN,    "P", kRH_FlagP },
        { kCx_FlagW, DCA_WIDTH,  "W", kRH_FlagW },
        { kCx_FlagM, DCA_MUTE,   "M", kRH_FlagM },
        { kCx_FlagS, DCA_SOLO,   "S", kRH_FlagS },
        { kCx_FlagR, DCA_RECARM, "R", kRH_FlagR },
    };
    for (int f = 0; f < 6; f++)
    {
        bool active = (dca.flags & kF[f].flag) != 0;
        bool hot    = isHot && (hotZone == kF[f].zone);
        DrawFlagButton(hdc, kF[f].x, y, kF[f].lbl, active, hot);
    }

    // ── Track count ───────────────────────────────────────────────────────────
    {
        int n = (int)DcaEngine_GetMemberTracks(&dca).size();
        char buf[12];
        snprintf(buf, sizeof(buf), "%d", n);
        DrawCell(hdc, kCx_Tracks, y, kCw_Tracks, kRowH, buf, kC_TextDim);
    }

    // ── Assign button ─────────────────────────────────────────────────────────
    {
        bool hot = isHot && (hotZone == kRH_Assign);
        int bx = kCx_Assign + 2, by = y + 3, bw = kCw_Assign - 4, bh = kRowH - 6;
        DrawFlatBtn(hdc, bx, by, bw, bh,
                    hot ? kC_BtnHot : kC_AssignBg, kC_BtnBorder);
        RECT rcA = { bx + 1, by, bx + bw - 1, by + bh };
        SetTextColor(hdc, hot ? kC_TextMain : kC_AssignTc);
        DrawTextA(hdc, "Assign Sel", -1, &rcA,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    }

    // ── Spill toggle ──────────────────────────────────────────────────────────
    {
        bool active = dca.spilled;
        bool hot    = isHot && (hotZone == kRH_Spill);
        int bx = kCx_Spill + 2, by = y + 3, bw = kCw_Spill - 4, bh = kRowH - 6;
        COLORREF fillC = active ? kC_SpillOnBg : (hot ? kC_BtnHot : kC_BtnNorm);
        DrawFlatBtn(hdc, bx, by, bw, bh, fillC, kC_BtnBorder);
        RECT rcS = { bx + 1, by, bx + bw - 1, by + bh };
        COLORREF tc = active ? kC_SpillOnTc
                             : (hot ? kC_TextMain : kC_SpillOffTc);
        SetTextColor(hdc, tc);
        DrawTextA(hdc, active ? "Spill On" : "Spill", -1, &rcS,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    }

    // ── Delete button ─────────────────────────────────────────────────────────
    {
        bool hot = isHot && (hotZone == kRH_Del);
        int bx = kCx_Del + 1, by = y + 3, bw = kCw_Del - 2, bh = kRowH - 6;
        DrawFlatBtn(hdc, bx, by, bw, bh,
                    hot ? kC_DelHotBg : kC_BtnNorm, kC_BtnBorder);
        RECT rcD = { bx + 1, by, bx + bw - 1, by + bh };
        SetTextColor(hdc, hot ? kC_DelHotTc : kC_DelTc);
        DrawTextA(hdc, "x", -1, &rcD,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    }

    // Bottom grid line
    HPEN hp = CreatePen(PS_SOLID, 1, kC_GridLine);
    HPEN ho = (HPEN)SelectObject(hdc, hp);
    MoveToEx(hdc, 0, y + kRowH - 1, nullptr);
    LineTo(hdc, cw, y + kRowH - 1);
    SelectObject(hdc, ho);
    DeleteObject(hp);
}

static void DrawList(HDC hdc, int cw, int ch)
{
    // Full background
    FillRc(hdc, 0, 0, cw, ch, kC_BgMain);

    HFONT hf = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT ho = (HFONT)SelectObject(hdc, hf);
    SetBkMode(hdc, TRANSPARENT);

    // Header (not scrolled — always at top)
    DrawHeader(hdc, cw);

    if (g_dcaGroups.empty())
    {
        SetTextColor(hdc, kC_TextDim);
        RECT rc = { 0, kHdrH, cw, ch };
        DrawTextA(hdc,
                  "Right-click for settings.  Click \"+ Add DCA\" to create a group.",
                  -1, &rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        SelectObject(hdc, ho);
        return;
    }

    for (int i = 0; i < (int)g_dcaGroups.size(); i++)
    {
        int rowY = kHdrH + i * kRowH - s_scrollY;
        if (rowY + kRowH < 0 || rowY >= ch) continue;
        bool isHot = (s_hotRow == i);
        DrawRow(hdc, i, rowY, cw, isHot, isHot ? s_hotZone : kRH_None);
    }

    SelectObject(hdc, ho);
}

// ---------------------------------------------------------------------------
// RefreshListView  –  populate the SysListView32 from g_dcaGroups
// ---------------------------------------------------------------------------

static const uint32_t kFlagBit[6] = {
    DCA_VOL, DCA_PAN, DCA_WIDTH, DCA_MUTE, DCA_SOLO, DCA_RECARM
};
static const char* kFlagLetters[6] = { "V", "P", "W", "M", "S", "R" };
static const int   kFlagCol[6]     = { kRH_FlagV, kRH_FlagP, kRH_FlagW,
                                       kRH_FlagM, kRH_FlagS, kRH_FlagR };

static void RefreshListView(HWND /*dlg*/)
{
    if (!g_listWnd || !IsWindow(g_listWnd)) return;

    int selRow = ListView_GetNextItem(g_listWnd, -1, LVNI_SELECTED);

    ListView_DeleteAllItems(g_listWnd);

    for (int i = 0; i < (int)g_dcaGroups.size(); i++)
    {
        const DcaGroup& dca = *g_dcaGroups[(size_t)i];

        LVITEMA lvi = {};
        lvi.mask    = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem   = i;
        lvi.lParam  = (LPARAM)i;
        lvi.pszText = const_cast<char*>(dca.name.c_str());
        ListView_InsertItem(g_listWnd, &lvi);

        char grpBuf[16];
        snprintf(grpBuf, sizeof(grpBuf), "%d", dca.groupNum);
        ListView_SetItemText(g_listWnd, i, 1, grpBuf);

        for (int f = 0; f < 6; f++)
        {
            ListView_SetItemText(g_listWnd, i, 2 + f,
                const_cast<char*>((dca.flags & kFlagBit[f]) ? kFlagLetters[f] : ""));
        }

        char trkBuf[16];
        snprintf(trkBuf, sizeof(trkBuf), "%d",
                 (int)DcaEngine_GetMemberTracks(&dca).size());
        ListView_SetItemText(g_listWnd, i, 8, trkBuf);

        ListView_SetItemText(g_listWnd, i, 9,  const_cast<char*>("Assign"));
        ListView_SetItemText(g_listWnd, i, 10, const_cast<char*>(dca.spilled ? "On" : "Spill"));
    }

    if (selRow >= 0 && selRow < (int)g_dcaGroups.size())
    {
        ListView_SetItemState(g_listWnd, selRow,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_listWnd, selRow, FALSE);
    }
}

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------

static void ShowContextMenu(HWND hwnd, int row, int screenX, int screenY)
{
    HMENU hMenu = CreatePopupMenu();

    DcaGroup* dca = (row >= 0 && row < (int)g_dcaGroups.size())
                    ? g_dcaGroups[(size_t)row].get() : nullptr;

    // Row-specific items
    if (dca)
    {
        char buf[160];
        snprintf(buf, sizeof(buf), "Rename \"%s\"...", dca->name.c_str());
        AppendMenuA(hMenu, MF_STRING, kMID_Rename, buf);
        AppendMenuA(hMenu, MF_STRING, kMID_Assign, "Assign Selected Tracks");
        AppendMenuA(hMenu,
                    dca->spilled ? (MF_STRING | MF_CHECKED) : MF_STRING,
                    kMID_Spill, "Spill");
        AppendMenuA(hMenu, MF_STRING, kMID_Delete, "Delete");
        AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
    }

    // Set Start Group
    char startBuf[80];
    snprintf(startBuf, sizeof(startBuf),
             "Set Start Group...  (currently %d)", g_startGroup);
    AppendMenuA(hMenu, MF_STRING, kMID_SetStartGroup, startBuf);
    AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);

    // Flag toggles: per-DCA when on a row, global defaults otherwise
    const uint32_t flagSrc = dca ? dca->flags : g_defaultFlags;
    AppendMenuA(hMenu, MF_STRING | MF_GRAYED | MF_DISABLED, 0,
                dca ? "Flags:" : "Default flags for new DCAs:");
    AppendMenuA(hMenu,
        MF_STRING | ((flagSrc & DCA_VOL)    ? MF_CHECKED : 0),
        kMID_DefVol,    "  Volume");
    AppendMenuA(hMenu,
        MF_STRING | ((flagSrc & DCA_PAN)    ? MF_CHECKED : 0),
        kMID_DefPan,    "  Pan");
    AppendMenuA(hMenu,
        MF_STRING | ((flagSrc & DCA_WIDTH)  ? MF_CHECKED : 0),
        kMID_DefWidth,  "  Width");
    AppendMenuA(hMenu,
        MF_STRING | ((flagSrc & DCA_MUTE)   ? MF_CHECKED : 0),
        kMID_DefMute,   "  Mute");
    AppendMenuA(hMenu,
        MF_STRING | ((flagSrc & DCA_SOLO)   ? MF_CHECKED : 0),
        kMID_DefSolo,   "  Solo");
    AppendMenuA(hMenu,
        MF_STRING | ((flagSrc & DCA_RECARM) ? MF_CHECKED : 0),
        kMID_DefRecArm, "  Rec Arm");

    int cmd = (int)TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                   screenX, screenY, 0, hwnd, nullptr);
    DestroyMenu(hMenu);

    if (cmd == 0) return;

    // ── Row actions ──────────────────────────────────────────────────────────

    if (cmd == kMID_Rename && dca)
    {
        if (g_listWnd) ListView_EditLabel(g_listWnd, row);
        return;
    }

    if (cmd == kMID_Assign && dca)
    {
        DcaEngine_AssignSelectedTracks(dca);
        DcaWnd_Refresh();
        return;
    }

    if (cmd == kMID_Spill && dca)
    {
        DcaEngine_Spill(dca, !dca->spilled);
        DcaWnd_Refresh();
        return;
    }

    if (cmd == kMID_Delete && dca)
    {
        char msg[160];
        snprintf(msg, sizeof(msg), "Delete DCA \"%s\"?", dca->name.c_str());
        if (MessageBoxA(hwnd, msg, "DCA Groups", MB_YESNO | MB_ICONQUESTION) == IDYES)
        {
            DcaEngine_Delete(row, true);
            s_scrollY = 0;
            DcaWnd_Refresh();
        }
        return;
    }

    // ── Settings actions ─────────────────────────────────────────────────────

    if (cmd == kMID_SetStartGroup)
    {
        char values[16];
        _itoa_s(g_startGroup, values, sizeof(values), 10);
        if (GetUserInputs("DCA Groups", 1,
                          "Start group (1-128):", values, (int)sizeof(values)))
        {
            int sg = atoi(values);
            if (sg >= 1 && sg <= 128)
            {
                g_startGroup = sg;
                SaveSettings();
            }
        }
        return;
    }

    // ── Flag toggles ─────────────────────────────────────────────────────────

    static const DcaFlag kFlagMap[6] = {
        DCA_VOL, DCA_PAN, DCA_WIDTH, DCA_MUTE, DCA_SOLO, DCA_RECARM
    };
    for (int f = 0; f < 6; f++)
    {
        if (cmd == kMID_DefVol + f)
        {
            if (dca)
            {
                DcaEngine_SetFlag(dca, kFlagMap[f], !(dca->flags & kFlagMap[f]));
                DcaWnd_Refresh();
            }
            else
            {
                g_defaultFlags ^= kFlagMap[f];
                SaveSettings();
            }
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// ListWndProc  –  custom child window that owns the scrollable DCA list
// ---------------------------------------------------------------------------

static LRESULT CALLBACK ListWndProc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    // ── Paint ─────────────────────────────────────────────────────────────────
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        // Double-buffer to eliminate flicker
        HDC     hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hBmp   = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP hOld   = (HBITMAP)SelectObject(hdcMem, hBmp);

        DrawList(hdcMem, rc.right, rc.bottom);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, hOld);
        DeleteObject(hBmp);
        DeleteDC(hdcMem);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;   // handled in WM_PAINT

    case WM_CTLCOLOREDIT:
    {
        // Style the inline name-edit box to match the REAPER theme
        HDC hdcEdit = (HDC)wParam;
        SetTextColor(hdcEdit, kC_TextMain);
        SetBkColor(hdcEdit, kC_NameBg);
        static HBRUSH s_hbrNameEdit = nullptr;
        if (!s_hbrNameEdit) s_hbrNameEdit = CreateSolidBrush(kC_NameBg);
        return (LRESULT)s_hbrNameEdit;
    }

    // ── Vertical scroll ───────────────────────────────────────────────────────
    case WM_VSCROLL:
    {
        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask  = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);

        int newPos = si.nPos;
        switch (LOWORD(wParam))
        {
        case SB_LINEUP:        newPos -= kRowH;         break;
        case SB_LINEDOWN:      newPos += kRowH;         break;
        case SB_PAGEUP:        newPos -= (int)si.nPage; break;
        case SB_PAGEDOWN:      newPos += (int)si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: newPos  = si.nTrackPos;  break;
        default: break;
        }

        int maxScroll = si.nMax - (int)si.nPage + 1;
        if (maxScroll < 0) maxScroll = 0;
        if (newPos < 0)         newPos = 0;
        if (newPos > maxScroll) newPos = maxScroll;

        if (newPos != s_scrollY)
        {
            s_scrollY = newPos;
            si.fMask  = SIF_POS;
            si.nPos   = s_scrollY;
            SetScrollInfo(hwnd, SB_VERT, &si, TRUE);

            // Keep inline edit in sync
            if (g_editRow >= 0 && g_nameEdit && IsWindowVisible(g_nameEdit))
            {
                int ny = kHdrH + g_editRow * kRowH - s_scrollY + 1;
                SetWindowPos(g_nameEdit, nullptr,
                             kCx_Name + 1, ny, 0, 0,
                             SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
            }

            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);

        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask  = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);

        int newPos = si.nPos - (delta / WHEEL_DELTA) * kRowH * 3;
        int maxScroll = si.nMax - (int)si.nPage + 1;
        if (maxScroll < 0) maxScroll = 0;
        if (newPos < 0)         newPos = 0;
        if (newPos > maxScroll) newPos = maxScroll;

        if (newPos != s_scrollY)
        {
            s_scrollY = newPos;
            si.fMask  = SIF_POS;
            si.nPos   = s_scrollY;
            SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_SIZE:
        UpdateScrollInfo();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    // ── Left-click ────────────────────────────────────────────────────────────
    case WM_LBUTTONDOWN:
    {
        CommitNameEdit();

        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        int row = -1;
        RowHit zone = HitTestRow(mx, my, row);
        if (row < 0 || zone == kRH_None) return 0;

        DcaGroup* dca = g_dcaGroups[(size_t)row].get();

        if (zone == kRH_Name)
        {
            StartNameEdit(row);
        }
        else if (zone == kRH_Assign)
        {
            DcaEngine_AssignSelectedTracks(dca);
            DcaWnd_Refresh();
        }
        else if (zone == kRH_Spill)
        {
            DcaEngine_Spill(dca, !dca->spilled);
            DcaWnd_Refresh();
        }
        else if (zone == kRH_Del)
        {
            char msg[160];
            snprintf(msg, sizeof(msg), "Delete DCA \"%s\"?", dca->name.c_str());
            if (MessageBoxA(GetParent(hwnd), msg, "DCA Groups",
                            MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
                DcaEngine_Delete(row, true);
                s_scrollY = 0;
                DcaWnd_Refresh();
            }
        }
        else if (zone >= kRH_FlagV && zone <= kRH_FlagR)
        {
            // Bit index: 0=Vol, 1=Pan, 2=Width, 3=Mute, 4=Solo, 5=RecArm
            int bitIdx = zone - kRH_FlagV;
            uint32_t flag = (1u << bitIdx);
            DcaEngine_SetFlag(dca, flag, !(dca->flags & flag));
            DcaWnd_Refresh();
        }
        return 0;
    }

    // ── Right-click ───────────────────────────────────────────────────────────
    case WM_RBUTTONUP:
    {
        CommitNameEdit();
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        int row = -1;
        HitTestRow(mx, my, row);

        POINT pt = { mx, my };
        ClientToScreen(hwnd, &pt);
        ShowContextMenu(hwnd, row, pt.x, pt.y);
        return 0;
    }

    // ── Hover highlighting ────────────────────────────────────────────────────
    case WM_MOUSEMOVE:
    {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        int row = -1;
        int zone = (int)HitTestRow(mx, my, row);

        if (row != s_hotRow || zone != s_hotZone)
        {
            s_hotRow  = row;
            s_hotZone = zone;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (s_hotRow != -1 || s_hotZone != kRH_None)
        {
            s_hotRow  = -1;
            s_hotZone = kRH_None;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    // ── Keyboard ─────────────────────────────────────────────────────────────
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            if (g_nameEdit && IsWindow(g_nameEdit) && IsWindowVisible(g_nameEdit))
            {
                ShowWindow(g_nameEdit, SW_HIDE);
                g_editRow = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        else if (wParam == VK_RETURN)
        {
            CommitNameEdit();
        }
        else if (wParam == VK_F2 && g_editRow < 0 && s_hotRow >= 0)
        {
            StartNameEdit(s_hotRow);
        }
        return 0;

    // ── Inline edit notifications ─────────────────────────────────────────────
    case WM_COMMAND:
        if (LOWORD(wParam) == 9999 && HIWORD(wParam) == EN_KILLFOCUS)
            CommitNameEdit();
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// DcaDlgProc  –  main IDD_DCA dialog (SysListView32 version)
// ---------------------------------------------------------------------------

static const int kTopBarH_lv = 28;   // toolbar strip height for the ListView layout

static const int kLvColWidths[11] = { 150,38,22,22,22,22,22,22,42,82,52 };
static const char* kLvColHdrs[11] = {
    "Name","G#","V","P","W","M","S","R","Trk","Assign","Spill"
};

static INT_PTR CALLBACK DcaDlgProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int cw = rc.right, ch = rc.bottom;

        HFONT hf = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        // ── "+ Add DCA" toolbar button ──
        g_addBtn = CreateWindowExA(0, "BUTTON", "+ Add DCA",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            cw - 88, 4, 84, 20,
            hwnd, (HMENU)(UINT_PTR)IDC_DCA_ADDSTRIP, g_hInst, nullptr);
        if (hf) SendMessage(g_addBtn, WM_SETFONT, (WPARAM)hf, FALSE);

        // ── SysListView32 filling the remaining area ──
        g_listWnd = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_EDITLABELS,
            0, kTopBarH_lv, cw, ch - kTopBarH_lv,
            hwnd, (HMENU)(UINT_PTR)IDC_DCA_LIST, g_hInst, nullptr);

        if (g_listWnd)
        {
            ListView_SetExtendedListViewStyle(g_listWnd,
                LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            if (hf) SendMessage(g_listWnd, WM_SETFONT, (WPARAM)hf, FALSE);

            LVCOLUMNA col = {};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            for (int c = 0; c < 11; c++)
            {
                col.cx      = kLvColWidths[c];
                col.pszText = const_cast<char*>(kLvColHdrs[c]);
                col.fmt     = (c == 0) ? LVCFMT_LEFT : LVCFMT_CENTER;
                ListView_InsertColumn(g_listWnd, c, &col);
            }

            RefreshListView(hwnd);
        }
        return TRUE;
    }

    case WM_SIZE:
    {
        int cw = (int)(short)LOWORD(lParam);
        int ch = (int)(short)HIWORD(lParam);
        if (cw > 0 && ch > 0)
        {
            if (g_listWnd && IsWindow(g_listWnd))
                SetWindowPos(g_listWnd, nullptr,
                    0, kTopBarH_lv, cw, ch - kTopBarH_lv,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_addBtn && IsWindow(g_addBtn))
                SetWindowPos(g_addBtn, nullptr,
                    cw - 88, 4, 84, 20,
                    SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_DCA_ADDSTRIP)
        {
            DcaGroup* dca = DcaEngine_Create(g_startGroup, g_defaultFlags);
            if (dca)
            {
                RefreshListView(hwnd);
                int newRow = (int)g_dcaGroups.size() - 1;
                if (newRow >= 0 && g_listWnd)
                {
                    ListView_EnsureVisible(g_listWnd, newRow, FALSE);
                    ListView_EditLabel(g_listWnd, newRow);
                }
            }
            else
            {
                MessageBoxA(hwnd,
                    "No free REAPER group slots available (groups 1-128 are all in use).",
                    "DCA Groups", MB_OK | MB_ICONWARNING);
            }
            return TRUE;
        }
        return FALSE;

    case WM_NOTIFY:
    {
        NMHDR* hdr = (NMHDR*)lParam;
        if (hdr->idFrom != IDC_DCA_LIST) return FALSE;

        // ── Single-click: dispatch column actions ──
        if (hdr->code == NM_CLICK)
        {
            NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
            int row = nia->iItem;
            int col = nia->iSubItem;
            if (row < 0 || row >= (int)g_dcaGroups.size()) break;

            DcaGroup* dca = g_dcaGroups[(size_t)row].get();

            if (col >= 2 && col <= 7)
            {
                int f = col - 2;
                DcaEngine_SetFlag(dca, kFlagBit[f], !(dca->flags & kFlagBit[f]));
                RefreshListView(hwnd);
            }
            else if (col == 9)  // Assign
            {
                DcaEngine_AssignSelectedTracks(dca);
                RefreshListView(hwnd);
            }
            else if (col == 10) // Spill
            {
                DcaEngine_Spill(dca, !dca->spilled);
                RefreshListView(hwnd);
            }
            break;
        }

        // ── Inline rename ──
        if (hdr->code == LVN_BEGINLABELEDIT)
            return FALSE;  // allow editing

        if (hdr->code == LVN_ENDLABELEDIT)
        {
            NMLVDISPINFOA* di = (NMLVDISPINFOA*)lParam;
            if (di->item.pszText && di->item.iItem >= 0 &&
                di->item.iItem < (int)g_dcaGroups.size())
            {
                DcaGroup* dca = g_dcaGroups[(size_t)di->item.iItem].get();
                dca->name = di->item.pszText;
                MediaTrack* lead = DcaEngine_GetControlTrack(dca);
                if (lead)
                    GetSetMediaTrackInfo_String(lead, "P_NAME",
                        const_cast<char*>(di->item.pszText), true);
                SetWindowLongPtr(hwnd, DWLP_MSGRESULT, TRUE);
                RefreshListView(hwnd);
            }
            return TRUE;
        }

        // ── Right-click context menu ──
        if (hdr->code == NM_RCLICK)
        {
            NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
            int row = (nia->iItem >= 0 && nia->iItem < (int)g_dcaGroups.size())
                          ? nia->iItem : -1;
            POINT ptScreen = nia->ptAction;
            ClientToScreen(hdr->hwndFrom, &ptScreen);

            g_skipNextCtxMenu = true;
            ShowContextMenu(hwnd, row, ptScreen.x, ptScreen.y);
            g_skipNextCtxMenu = false;

            SetWindowLongPtr(hwnd, DWLP_MSGRESULT, TRUE);
            return TRUE;
        }

        return FALSE;
    }

    case WM_CONTEXTMENU:
    {
        if (g_skipNextCtxMenu) { g_skipNextCtxMenu = false; return TRUE; }
        POINT pt; GetCursorPos(&pt);
        ShowContextMenu(hwnd, -1, pt.x, pt.y);
        return TRUE;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return TRUE;

    case WM_DESTROY:
        g_dlg     = nullptr;
        g_listWnd = nullptr;
        g_addBtn  = nullptr;
        return TRUE;
    }

    return FALSE;
}
