// ---------------------------------------------------------------------------
// SurfaceEditorWnd.cpp  —  top-level docked window for the Surface & Zone Editor
//
// Layout:
//   Toolbar    : [New] [Save] [Refresh] | Surface:[▼ combo] | Zone:[▼ combo] | [Save Zone]
//   Canvas     : full-width, always visible (surface-edit or zone-assign mode)
//   Bottom bar : surface mode — Name edit + Type label + Delete button  (80px)
//                zone mode    — Zone name, type, modifier filter checkboxes,
//                               Included/Sub zone text fields              (130px)
//   Status bar : 20px
// ---------------------------------------------------------------------------

#include "SurfaceEditorWnd.h"

extern HWND g_hwnd;
#include "SurfaceModel.h"
#include "ZoneModel.h"
#include "CanvasWnd.h"
#include "ActionSearchDlg.h"
#include "resource.h"
#include "api.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <sstream>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace SurfaceEditor;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const char* k_SEClassName  = "LT_SurfaceEditorWnd";
static const char* k_DockKey      = "reaper_lt_surface_editor";
static const int   k_ToolbarH     = 34;   // taller to fit combo boxes
static const int   k_PropBarH     = 80;   // surface-mode bottom bar height
static const int   k_ZoneBarH     = 130;  // zone-mode bottom bar height
static const int   k_StatusH      = 20;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct SEState {
    HINSTANCE hInst             = nullptr;
    HWND      hwnd              = nullptr;

    // Toolbar controls (direct children of hwnd)
    HWND hNewBtn                = nullptr;   // IDC_SE_NEW_BTN
    HWND hSaveBtn               = nullptr;   // IDC_SE_SAVE_BTN
    HWND hRefreshBtn            = nullptr;   // IDC_SE_REFRESH_BTN  (was hOpenBtn)
    HWND hSurfaceLabel          = nullptr;   // IDC_SE_SURFACE_LABEL  Static "Surface:"
    HWND hSurfaceCombo          = nullptr;   // IDC_SE_SURFACE_COMBO
    HWND hZoneLabel             = nullptr;   // IDC_SE_ZONE_LABEL     Static "Zone:"
    HWND hZoneCombo             = nullptr;   // IDC_SE_ZONE_COMBO
    HWND hZoneSaveBtn           = nullptr;   // IDC_SE_ZONE_SAVE_BTN

    // Canvas (always visible, full width)
    HWND hCanvas                = nullptr;

    // Bottom bar — Surface mode (direct children of hwnd)
    HWND hPropNameLbl           = nullptr;   // IDC_SE_PROP_NAME_LBL  Static "Name:"
    HWND hPropName              = nullptr;   // IDC_SE_PROP_NAME
    HWND hPropType              = nullptr;   // IDC_SE_PROP_GENTYPE   (label showing type)
    HWND hPropDelete            = nullptr;   // IDC_SE_PROP_DELETE

    // Bottom bar — Zone mode (direct children of hwnd)
    HWND hZoneNameLbl           = nullptr;   // Static "Zone:"
    HWND hZoneNameEdit          = nullptr;   // IDC_SE_ZONE_NAME
    HWND hZoneTypeLbl           = nullptr;   // Static "Type:"
    HWND hZoneTypeCombo         = nullptr;   // IDC_SE_ZONE_TYPE
    HWND hZoneModShift          = nullptr;   // IDC_SE_ZONE_MOD_SHIFT
    HWND hZoneModOption         = nullptr;   // IDC_SE_ZONE_MOD_OPTION
    HWND hZoneModControl        = nullptr;   // IDC_SE_ZONE_MOD_CONTROL
    HWND hZoneModAlt            = nullptr;   // IDC_SE_ZONE_MOD_ALT
    HWND hZoneModFlip           = nullptr;   // IDC_SE_ZONE_MOD_FLIP
    HWND hZoneModHold           = nullptr;   // IDC_SE_ZONE_MOD_HOLD
    HWND hZoneIncLbl            = nullptr;   // Static "Included:"
    HWND hZoneIncEdit           = nullptr;   // IDC_SE_ZONE_INC
    HWND hZoneSubLbl            = nullptr;   // Static "Sub zones:"
    HWND hZoneSubEdit           = nullptr;   // IDC_SE_ZONE_SUB

    // Status bar
    HWND hStatus                = nullptr;   // IDC_SE_STATUS

    // Loaded data
    std::unique_ptr<Surface>  surface;
    std::string               surfacePath;
    std::vector<std::string>  surfacePaths;  // parallel to hSurfaceCombo items (index 0 = none)

    ZoneFile                  currentZone;
    std::string               currentZonePath;
    std::vector<std::string>  zonePaths;     // index 0="" (surface layout), rest are .zon paths

    bool                      inZoneMode      = false;
    bool                      updatingZoneBar = false;  // suppress EN_CHANGE feedback loops

    int                       selWidgetCol    = -1;
    int                       selWidgetRow    = -1;
    bool                      dirty           = false;
    bool                      zoneDirty       = false;
};

static SEState* s_state = nullptr;

// ---------------------------------------------------------------------------
// Helpers: CSI paths
// ---------------------------------------------------------------------------

static std::string GetCsiSurfacesDir()
{
    const char* res = GetResourcePath ? GetResourcePath() : nullptr;
    if (!res) return "";
    std::string path = res;
    path += "\\LiveTools\\Surfaces";
    return path;
}

// ---------------------------------------------------------------------------
// String utilities
// ---------------------------------------------------------------------------

static std::string JoinStrings(const std::vector<std::string>& v, const char* sep)
{
    std::string result;
    for (int i = 0; i < (int)v.size(); ++i)
    {
        if (i > 0) result += sep;
        result += v[i];
    }
    return result;
}

static std::vector<std::string> SplitTrim(const std::string& s, char delim)
{
    std::vector<std::string> result;
    std::istringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim))
    {
        size_t start = item.find_first_not_of(" \t\r\n");
        size_t end   = item.find_last_not_of(" \t\r\n");
        if (start != std::string::npos)
            result.push_back(item.substr(start, end - start + 1));
    }
    return result;
}

// ---------------------------------------------------------------------------
// Bottom mode — show/hide prop bar vs zone bar controls
// ---------------------------------------------------------------------------

static void ShowPropBarControls(SEState* s, bool show)
{
    int sw = show ? SW_SHOW : SW_HIDE;
    ShowWindow(s->hPropNameLbl, sw);
    ShowWindow(s->hPropName,    sw);
    ShowWindow(s->hPropType,    sw);
    ShowWindow(s->hPropDelete,  sw);
}

static void ShowZoneBarControls(SEState* s, bool show)
{
    int sw = show ? SW_SHOW : SW_HIDE;
    ShowWindow(s->hZoneNameLbl,    sw);
    ShowWindow(s->hZoneNameEdit,   sw);
    ShowWindow(s->hZoneTypeLbl,    sw);
    ShowWindow(s->hZoneTypeCombo,  sw);
    ShowWindow(s->hZoneModShift,   sw);
    ShowWindow(s->hZoneModOption,  sw);
    ShowWindow(s->hZoneModControl, sw);
    ShowWindow(s->hZoneModAlt,     sw);
    ShowWindow(s->hZoneModFlip,    sw);
    ShowWindow(s->hZoneModHold,    sw);
    ShowWindow(s->hZoneIncLbl,     sw);
    ShowWindow(s->hZoneIncEdit,    sw);
    ShowWindow(s->hZoneSubLbl,     sw);
    ShowWindow(s->hZoneSubEdit,    sw);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

static void DoLayout(SEState* s, int W, int H)
{
    if (W <= 0 || H <= 0) return;

    int tbH  = k_ToolbarH;
    int sbH  = k_StatusH;
    int bbH  = s->inZoneMode ? k_ZoneBarH : k_PropBarH;
    int canvH = H - tbH - sbH - bbH;
    if (canvH < 40) canvH = 40;

    // --- Toolbar ---
    int tx = 4;
    MoveWindow(s->hNewBtn,     tx,  5, 56, 24, TRUE); tx += 60;
    MoveWindow(s->hSaveBtn,    tx,  5, 56, 24, TRUE); tx += 60;
    MoveWindow(s->hRefreshBtn, tx,  5, 56, 24, TRUE); tx += 66;

    // "Surface:" label + combo
    MoveWindow(s->hSurfaceLabel, tx, 9, 56, 16, TRUE); tx += 60;
    MoveWindow(s->hSurfaceCombo, tx, 5, 150, 200, TRUE); tx += 158;

    // "Zone:" label + combo
    MoveWindow(s->hZoneLabel, tx, 9, 44, 16, TRUE); tx += 48;
    MoveWindow(s->hZoneCombo, tx, 5, 160, 200, TRUE); tx += 168;

    // Save Zone button
    MoveWindow(s->hZoneSaveBtn, tx, 5, 78, 24, TRUE);

    // --- Status bar ---
    MoveWindow(s->hStatus, 0, H - sbH, W, sbH, TRUE);

    // --- Canvas ---
    if (s->hCanvas)
        MoveWindow(s->hCanvas, 0, tbH, W, canvH, TRUE);

    // --- Bottom bar: surface mode (prop) ---
    int barY = tbH + canvH;
    int p = 6;

    // Row 0 of prop bar
    MoveWindow(s->hPropNameLbl, p,          barY + 6,  40, 16, TRUE);
    MoveWindow(s->hPropName,    p + 44,     barY + 3, 180, 22, TRUE);
    MoveWindow(s->hPropType,    p,          barY + 32, 240, 16, TRUE);
    MoveWindow(s->hPropDelete,  p,          barY + 54,  90, 22, TRUE);

    // --- Bottom bar: zone mode ---
    // Row 0: "Zone:" + name edit | "Type:" + type combo
    MoveWindow(s->hZoneNameLbl,   p,          barY + 6,  40, 16, TRUE);
    MoveWindow(s->hZoneNameEdit,  p + 44,     barY + 3, 160, 22, TRUE);
    MoveWindow(s->hZoneTypeLbl,   p + 214,    barY + 6,  36, 16, TRUE);
    MoveWindow(s->hZoneTypeCombo, p + 254,    barY + 3, 140, 200, TRUE);

    // Row 1: modifier checkboxes
    int y1 = barY + 32;
    int cbW = 72;
    MoveWindow(s->hZoneModShift,   p + cbW*0, y1, cbW, 20, TRUE);
    MoveWindow(s->hZoneModOption,  p + cbW*1, y1, cbW, 20, TRUE);
    MoveWindow(s->hZoneModControl, p + cbW*2, y1, cbW, 20, TRUE);
    MoveWindow(s->hZoneModAlt,     p + cbW*3, y1, cbW, 20, TRUE);
    MoveWindow(s->hZoneModFlip,    p + cbW*4, y1, cbW, 20, TRUE);
    MoveWindow(s->hZoneModHold,    p + cbW*5, y1, cbW, 20, TRUE);

    // Row 2: included / sub zone text edits
    int y2 = barY + 58;
    MoveWindow(s->hZoneIncLbl,  p,          y2, 58, 16, TRUE);
    MoveWindow(s->hZoneIncEdit, p + 62,     y2, 200, 22, TRUE);
    MoveWindow(s->hZoneSubLbl,  p + 272,    y2, 66, 16, TRUE);
    MoveWindow(s->hZoneSubEdit, p + 342,    y2, 200, 22, TRUE);
}

// ---------------------------------------------------------------------------
// Surface combo population
// ---------------------------------------------------------------------------

static void PopulateSurfaceCombo(SEState* s)
{
    HWND cb = s->hSurfaceCombo;
    SendMessageA(cb, CB_RESETCONTENT, 0, 0);
    s->surfacePaths.clear();

    SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)"-- Select Surface --");
    s->surfacePaths.push_back("");

    std::string surfacesDir = GetCsiSurfacesDir();
    if (surfacesDir.empty()) return;

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA((surfacesDir + "\\*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        std::string surfFile = surfacesDir + "\\" + fd.cFileName + "\\Surface.txt";
        if (GetFileAttributesA(surfFile.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)fd.cFileName);
        s->surfacePaths.push_back(surfFile);
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);

    SendMessageA(cb, CB_SETCURSEL, 0, 0);
}

// ---------------------------------------------------------------------------
// Zone combo population
// ---------------------------------------------------------------------------

static void CollectZoneFiles(const std::string& dir,
                              std::vector<std::string>& paths,
                              std::vector<std::string>& labels,
                              const std::string& prefix = "")
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*.zon").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do {
            std::string lbl = prefix.empty() ? fd.cFileName : prefix + "/" + fd.cFileName;
            paths.push_back(dir + "\\" + fd.cFileName);
            labels.push_back(lbl);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    // Recurse into subdirectories
    HANDLE h2 = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h2 != INVALID_HANDLE_VALUE)
    {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            std::string sub = prefix.empty() ? fd.cFileName : prefix + "/" + fd.cFileName;
            CollectZoneFiles(dir + "\\" + fd.cFileName, paths, labels, sub);
        } while (FindNextFileA(h2, &fd));
        FindClose(h2);
    }
}

static void PopulateZoneCombo(SEState* s, const std::string& surfaceDir)
{
    HWND cb = s->hZoneCombo;
    SendMessageA(cb, CB_RESETCONTENT, 0, 0);
    s->zonePaths.clear();

    SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)"-- Surface Layout --");
    s->zonePaths.push_back("");  // index 0 = surface layout mode

    std::vector<std::string> paths, labels;
    CollectZoneFiles(surfaceDir + "\\Zones",                 paths, labels);
    CollectZoneFiles(surfaceDir + "\\FXZones",               paths, labels, "FX");
    CollectZoneFiles(surfaceDir + "\\AutoGeneratedFXZones",  paths, labels, "AutoFX");

    for (int i = 0; i < (int)paths.size(); ++i)
    {
        SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)labels[i].c_str());
        s->zonePaths.push_back(paths[i]);
    }
    SendMessageA(cb, CB_SETCURSEL, 0, 0);
}

// ---------------------------------------------------------------------------
// Zone bar population from model
// ---------------------------------------------------------------------------

static void SyncZoneBarFromModel(SEState* s)
{
    s->updatingZoneBar = true;

    SetWindowTextA(s->hZoneNameEdit, s->currentZone.zoneName.c_str());

    // Select matching type in combo (or "(custom)" at index 0)
    int cnt = (int)SendMessageA(s->hZoneTypeCombo, CB_GETCOUNT, 0, 0);
    int sel = 0;
    for (int i = 1; i < cnt; ++i)
    {
        char buf[128] = {};
        SendMessageA(s->hZoneTypeCombo, CB_GETLBTEXT, i, (LPARAM)buf);
        if (s->currentZone.zoneName == buf) { sel = i; break; }
    }
    SendMessageA(s->hZoneTypeCombo, CB_SETCURSEL, sel, 0);

    SetWindowTextA(s->hZoneIncEdit, JoinStrings(s->currentZone.includedZones, ", ").c_str());
    SetWindowTextA(s->hZoneSubEdit, JoinStrings(s->currentZone.subZones,      ", ").c_str());

    // Clear modifier checkboxes (these are for filter only, not stored in the model)
    SendMessageA(s->hZoneModShift,   BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageA(s->hZoneModOption,  BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageA(s->hZoneModControl, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageA(s->hZoneModAlt,     BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageA(s->hZoneModFlip,    BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageA(s->hZoneModHold,    BM_SETCHECK, BST_UNCHECKED, 0);

    s->updatingZoneBar = false;
}

// ---------------------------------------------------------------------------
// SetBottomMode — switches between surface-mode prop bar and zone-mode bar
// ---------------------------------------------------------------------------

static void SetBottomMode(SEState* s, bool zoneMode)
{
    s->inZoneMode = zoneMode;
    ShowPropBarControls(s, !zoneMode);
    ShowZoneBarControls(s, zoneMode);

    if (s->hCanvas)
    {
        CanvasWnd_SetMode(s->hCanvas, zoneMode ? CanvasMode::ZoneAssign : CanvasMode::SurfaceEdit);
        CanvasWnd_Refresh(s->hCanvas);
    }

    // Re-layout to change bottom bar height
    RECT rc;
    GetClientRect(s->hwnd, &rc);
    DoLayout(s, rc.right, rc.bottom);
    InvalidateRect(s->hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Load helpers
// ---------------------------------------------------------------------------

static void LoadSurface(SEState* s, const std::string& filePath)
{
    if (filePath.empty()) return;

    s->surface     = std::make_unique<Surface>(ParseSurfaceFile(filePath));
    s->surfacePath = filePath;
    s->selWidgetCol = s->selWidgetRow = -1;
    s->dirty = false;

    CanvasWnd_SetSurface(s->hCanvas, s->surface.get());
    CanvasWnd_SetMode(s->hCanvas, CanvasMode::SurfaceEdit);
    CanvasWnd_SetSelection(s->hCanvas, -1, -1);
    CanvasWnd_Refresh(s->hCanvas);

    // Derive surface directory and populate zone combo
    std::string surfDir = filePath;
    size_t sl = surfDir.rfind('\\');
    if (sl != std::string::npos) surfDir = surfDir.substr(0, sl);
    PopulateZoneCombo(s, surfDir);

    SetBottomMode(s, false);

    char status[256];
    std::snprintf(status, sizeof(status), "Surface: %s  (%d widgets)",
                  s->surface->name.c_str(), (int)s->surface->widgets.size());
    SetWindowTextA(s->hStatus, status);
}

static void LoadZone(SEState* s, const std::string& filePath)
{
    if (filePath.empty()) return;

    s->currentZone = ParseZoneFile(filePath);
    s->currentZone.filePath = filePath;  // ensure path is set for WriteZoneFile
    s->currentZonePath = filePath;
    s->zoneDirty = false;

    CanvasWnd_SetSurface(s->hCanvas, s->surface.get());
    CanvasWnd_SetZoneFile(s->hCanvas, &s->currentZone);
    CanvasWnd_SetMode(s->hCanvas, CanvasMode::ZoneAssign);
    CanvasWnd_Refresh(s->hCanvas);

    SyncZoneBarFromModel(s);
    SetBottomMode(s, true);

    SetWindowTextA(s->hStatus, s->currentZone.zoneName.c_str());
}

// ---------------------------------------------------------------------------
// Property panel refresh (surface mode)
// ---------------------------------------------------------------------------

static void RefreshPropertyPanel(SEState* s)
{
    if (!s->surface || s->selWidgetCol < 0)
    {
        SetWindowTextA(s->hPropName, "");
        SetWindowTextA(s->hPropType, "");
        return;
    }
    Widget* w = nullptr;
    for (auto& ww : s->surface->widgets)
        if (ww.gridCol == s->selWidgetCol && ww.gridRow == s->selWidgetRow)
        { w = &ww; break; }

    if (!w) { SetWindowTextA(s->hPropName, ""); return; }

    SetWindowTextA(s->hPropName, w->name.c_str());

    WidgetType wt = InferWidgetType(*w);
    char typeBuf[64];
    std::snprintf(typeBuf, sizeof(typeBuf), "Type: %s", WidgetTypeName(wt));
    SetWindowTextA(s->hPropType, typeBuf);
}

// ---------------------------------------------------------------------------
// Canvas callbacks
// ---------------------------------------------------------------------------

static void OnWidgetSelected(int col, int row, SEState* s)
{
    s->selWidgetCol = col;
    s->selWidgetRow = row;
    CanvasWnd_SetSelection(s->hCanvas, col, row);
    RefreshPropertyPanel(s);
}

static void OnWidgetMoved(int srcCol, int srcRow, int dstPixX, int dstPixY, SEState* s)
{
    if (!s->surface) return;
    for (auto& w : s->surface->widgets)
    {
        if (w.gridCol == srcCol && w.gridRow == srcRow)
        {
            w.pixX   = dstPixX;
            w.pixY   = dstPixY;
            s->dirty = true;
        }
    }
    CanvasWnd_Refresh(s->hCanvas);
}

static void OnBlankCellClicked(int col, int row, SEState* s)
{
    s->selWidgetCol = col;
    s->selWidgetRow = row;
}

static void OnZoneWidgetActivated(int col, int row, SEState* s)
{
    if (!s->surface || !s->inZoneMode) return;

    Widget* w = nullptr;
    for (auto& ww : s->surface->widgets)
        if (ww.gridCol == col && ww.gridRow == row)
        { w = &ww; break; }
    if (!w) return;

    // Find existing unmodified assignment for this widget
    ZoneAssignment* za = nullptr;
    for (auto& a : s->currentZone.assignments)
        if (a.widgetExpr == w->name && a.modifier.empty())
        { za = &a; break; }

    std::string chosen = ActionSearchDlg_Show(s->hwnd, za ? za->action : "");
    if (chosen.empty()) return;

    if (za)
        za->action = chosen;
    else
    {
        ZoneAssignment newZa;
        newZa.widgetExpr = w->name;
        newZa.action     = chosen;
        s->currentZone.assignments.push_back(newZa);
    }

    s->zoneDirty = true;
    CanvasWnd_SetZoneFile(s->hCanvas, &s->currentZone);
    CanvasWnd_Refresh(s->hCanvas);
}

// ---------------------------------------------------------------------------
// New surface wizard  (unchanged logic, only PopulateTree→PopulateSurfaceCombo)
// ---------------------------------------------------------------------------

struct NewSurfaceParams {
    char name[128];
    int  channels;
    int  rows;
};

static INT_PTR CALLBACK NewSurfaceDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        NewSurfaceParams* p = (NewSurfaceParams*)lp;
        SetWindowLongPtrA(hwnd, DWLP_USER, (LONG_PTR)p);
        SetDlgItemTextA(hwnd, IDC_NS_NAME, "MySurface");
        SetDlgItemInt(hwnd, IDC_NS_CHANNELS, 8, FALSE);
        SetDlgItemInt(hwnd, IDC_NS_ROWS,     4, FALSE);
        SendDlgItemMessageA(hwnd, IDC_NS_CHANNELS_SPIN, UDM_SETRANGE, 0, MAKELPARAM(64, 1));
        SendDlgItemMessageA(hwnd, IDC_NS_CHANNELS_SPIN, UDM_SETPOS,   0, 8);
        SendDlgItemMessageA(hwnd, IDC_NS_ROWS_SPIN,     UDM_SETRANGE, 0, MAKELPARAM(16, 1));
        SendDlgItemMessageA(hwnd, IDC_NS_ROWS_SPIN,     UDM_SETPOS,   0, 4);
        return TRUE;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wp);
        if (id == IDC_NS_OK || id == IDOK)
        {
            NewSurfaceParams* p = (NewSurfaceParams*)GetWindowLongPtrA(hwnd, DWLP_USER);
            GetDlgItemTextA(hwnd, IDC_NS_NAME, p->name, sizeof(p->name));
            BOOL ok1 = FALSE, ok2 = FALSE;
            p->channels = (int)GetDlgItemInt(hwnd, IDC_NS_CHANNELS, &ok1, FALSE);
            p->rows     = (int)GetDlgItemInt(hwnd, IDC_NS_ROWS,     &ok2, FALSE);
            if (p->channels < 1) p->channels = 8;
            if (p->rows     < 1) p->rows     = 4;
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (id == IDC_NS_CANCEL || id == IDCANCEL)
        {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }
    }
    return FALSE;
}

static const DLGTEMPLATE* BuildNewSurfaceTemplate()
{
    static struct {
        DLGTEMPLATE dt;
        WORD menu, cls;
        WCHAR title[24];
        WORD  pointsize;
        WCHAR font[12];
    } t = {};
    t.dt.style  = DS_SETFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    t.dt.cdit   = 0;
    t.dt.cx     = 220; t.dt.cy = 120;
    static const wchar_t kT[] = L"New Surface";
    for (int i = 0; i < 11; ++i) t.title[i] = kT[i];
    t.pointsize = 9;
    static const wchar_t kF[] = L"Segoe UI";
    for (int i = 0; i < 8; ++i) t.font[i] = kF[i];
    return &t.dt;
}

static void ShowNewSurfaceDialog(SEState* s)
{
    NewSurfaceParams params = {};
    HWND hDlg = CreateDialogIndirectParamA(s->hInst, BuildNewSurfaceTemplate(),
                                            s->hwnd, NewSurfaceDlgProc, (LPARAM)&params);
    if (!hDlg) return;

    int p = 8;
    CreateWindowExA(0, "STATIC", "Surface Name:",  WS_CHILD|WS_VISIBLE, p, p+4,    90, 16, hDlg, nullptr, s->hInst, nullptr);
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "MySurface", WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL, p+94, p, 110, 20, hDlg, (HMENU)(INT_PTR)IDC_NS_NAME, s->hInst, nullptr);
    CreateWindowExA(0, "STATIC", "Channels:",      WS_CHILD|WS_VISIBLE, p, p+30,   70, 16, hDlg, nullptr, s->hInst, nullptr);
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "8", WS_CHILD|WS_VISIBLE|ES_NUMBER,  p+80, p+28, 36, 20, hDlg, (HMENU)(INT_PTR)IDC_NS_CHANNELS, s->hInst, nullptr);
    CreateWindowExA(0, UPDOWN_CLASSA, nullptr, WS_CHILD|WS_VISIBLE|UDS_ALIGNRIGHT|UDS_SETBUDDYINT|UDS_ARROWKEYS, 0,0,0,0, hDlg, (HMENU)(INT_PTR)IDC_NS_CHANNELS_SPIN, s->hInst, nullptr);
    SendDlgItemMessageA(hDlg, IDC_NS_CHANNELS_SPIN, UDM_SETBUDDY, (WPARAM)GetDlgItem(hDlg, IDC_NS_CHANNELS), 0);
    CreateWindowExA(0, "STATIC", "Grid Rows:",     WS_CHILD|WS_VISIBLE, p, p+56,   70, 16, hDlg, nullptr, s->hInst, nullptr);
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "4", WS_CHILD|WS_VISIBLE|ES_NUMBER,  p+80, p+54, 36, 20, hDlg, (HMENU)(INT_PTR)IDC_NS_ROWS, s->hInst, nullptr);
    CreateWindowExA(0, UPDOWN_CLASSA, nullptr, WS_CHILD|WS_VISIBLE|UDS_ALIGNRIGHT|UDS_SETBUDDYINT|UDS_ARROWKEYS, 0,0,0,0, hDlg, (HMENU)(INT_PTR)IDC_NS_ROWS_SPIN, s->hInst, nullptr);
    SendDlgItemMessageA(hDlg, IDC_NS_ROWS_SPIN, UDM_SETBUDDY, (WPARAM)GetDlgItem(hDlg, IDC_NS_ROWS), 0);
    CreateWindowExA(0, "BUTTON", "Create", WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON, p+60, p+84, 60, 22, hDlg, (HMENU)(INT_PTR)IDC_NS_OK,     s->hInst, nullptr);
    CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,    p+128,p+84, 60, 22, hDlg, (HMENU)(INT_PTR)IDC_NS_CANCEL, s->hInst, nullptr);

    INT_PTR ret = DialogBoxIndirectParamA(s->hInst, BuildNewSurfaceTemplate(),
                                           s->hwnd, NewSurfaceDlgProc, (LPARAM)&params);
    DestroyWindow(hDlg);

    if (ret != IDOK) return;

    std::string csiDir  = GetCsiSurfacesDir();
    std::string surfDir = csiDir + "\\" + params.name;
    std::string zoneDir = surfDir + "\\Zones";
    CreateDirectoryA(surfDir.c_str(), nullptr);
    CreateDirectoryA(zoneDir.c_str(), nullptr);

    Surface newSurf;
    newSurf.name         = params.name;
    newSurf.filePath     = surfDir + "\\Surface.txt";
    newSurf.channelCount = params.channels;
    newSurf.gridCols     = params.channels;
    newSurf.gridRows     = params.rows;
    WriteSurfaceFile(newSurf);

    PopulateSurfaceCombo(s);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

static bool s_suppressDockStateSave = false;
static void ToggleDocking();

static LRESULT CALLBACK SEWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    SEState* s = s_state;

    switch (msg)
    {
    case WM_SIZE:
        if (s) DoLayout(s, LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_COMMAND:
    {
        if (!s) return 0;
        int id   = LOWORD(wp);
        int note = HIWORD(wp);

        // --- Toolbar buttons ---
        if (id == IDC_SE_NEW_BTN)
        {
            ShowNewSurfaceDialog(s);
            return 0;
        }
        if (id == IDC_SE_SAVE_BTN)
        {
            if (s->surface && !s->surfacePath.empty())
            {
                WriteSurfaceFile(*s->surface);
                s->dirty = false;
                SetWindowTextA(s->hStatus, "Surface saved.");
            }
            return 0;
        }
        if (id == IDC_SE_REFRESH_BTN)
        {
            PopulateSurfaceCombo(s);
            return 0;
        }

        // --- Surface combo ---
        if (id == IDC_SE_SURFACE_COMBO && note == CBN_SELCHANGE)
        {
            int sel = (int)SendMessageA(s->hSurfaceCombo, CB_GETCURSEL, 0, 0);
            if (sel > 0 && sel < (int)s->surfacePaths.size())
                LoadSurface(s, s->surfacePaths[sel]);
            return 0;
        }

        // --- Zone combo ---
        if (id == IDC_SE_ZONE_COMBO && note == CBN_SELCHANGE)
        {
            if (!s->surface) return 0;
            int sel = (int)SendMessageA(s->hZoneCombo, CB_GETCURSEL, 0, 0);
            if (sel <= 0 || sel >= (int)s->zonePaths.size())
            {
                // "Surface Layout" selected
                s->currentZonePath.clear();
                CanvasWnd_SetZoneFile(s->hCanvas, nullptr);
                SetBottomMode(s, false);
            }
            else
            {
                LoadZone(s, s->zonePaths[sel]);
            }
            return 0;
        }

        // --- Save Zone button ---
        if (id == IDC_SE_ZONE_SAVE_BTN)
        {
            if (!s->currentZonePath.empty())
            {
                WriteZoneFile(s->currentZone);
                s->zoneDirty = false;
                SetWindowTextA(s->hStatus, "Zone saved.");
            }
            return 0;
        }

        // --- Delete widget ---
        if (id == IDC_SE_PROP_DELETE)
        {
            if (s->surface && s->selWidgetCol >= 0)
            {
                auto& wv = s->surface->widgets;
                for (auto it = wv.begin(); it != wv.end(); ++it)
                {
                    if (it->gridCol == s->selWidgetCol && it->gridRow == s->selWidgetRow)
                    { wv.erase(it); break; }
                }
                s->selWidgetCol = s->selWidgetRow = -1;
                s->dirty = true;
                CanvasWnd_SetSelection(s->hCanvas, -1, -1);
                CanvasWnd_Refresh(s->hCanvas);
            }
            return 0;
        }

        // --- Zone bar: zone name edit ---
        if (id == IDC_SE_ZONE_NAME && note == EN_CHANGE && !s->updatingZoneBar)
        {
            char buf[256] = {};
            GetWindowTextA(s->hZoneNameEdit, buf, sizeof(buf));
            s->currentZone.zoneName = buf;
            s->zoneDirty = true;
            return 0;
        }

        // --- Zone bar: zone type combo ---
        if (id == IDC_SE_ZONE_TYPE && note == CBN_SELCHANGE && !s->updatingZoneBar)
        {
            int sel = (int)SendMessageA(s->hZoneTypeCombo, CB_GETCURSEL, 0, 0);
            if (sel > 0)  // 0 = "(custom)"
            {
                char buf[128] = {};
                SendMessageA(s->hZoneTypeCombo, CB_GETLBTEXT, sel, (LPARAM)buf);
                s->currentZone.zoneName = buf;
                s->updatingZoneBar = true;
                SetWindowTextA(s->hZoneNameEdit, buf);
                s->updatingZoneBar = false;
                s->zoneDirty = true;
            }
            return 0;
        }

        // --- Zone bar: included zones ---
        if (id == IDC_SE_ZONE_INC && note == EN_CHANGE && !s->updatingZoneBar)
        {
            char buf[512] = {};
            GetWindowTextA(s->hZoneIncEdit, buf, sizeof(buf));
            s->currentZone.includedZones = SplitTrim(buf, ',');
            s->zoneDirty = true;
            return 0;
        }

        // --- Zone bar: sub zones ---
        if (id == IDC_SE_ZONE_SUB && note == EN_CHANGE && !s->updatingZoneBar)
        {
            char buf[512] = {};
            GetWindowTextA(s->hZoneSubEdit, buf, sizeof(buf));
            s->currentZone.subZones = SplitTrim(buf, ',');
            s->zoneDirty = true;
            return 0;
        }

        // --- Zone bar: modifier checkboxes (filter refresh) ---
        if ((id == IDC_SE_ZONE_MOD_SHIFT   || id == IDC_SE_ZONE_MOD_OPTION  ||
             id == IDC_SE_ZONE_MOD_CONTROL || id == IDC_SE_ZONE_MOD_ALT     ||
             id == IDC_SE_ZONE_MOD_FLIP    || id == IDC_SE_ZONE_MOD_HOLD)   &&
            note == BN_CLICKED)
        {
            CanvasWnd_Refresh(s->hCanvas);
            return 0;
        }

        return 0;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
    {
        if (!s_suppressDockStateSave)
        {
            bool isFloat = false;
            bool wasDocked = (DockIsChildOfDock(hwnd, &isFloat) >= 0);
            SetExtState("reaper_transitions", "surface_editor_docked", wasDocked ? "1" : "0", true);
            if (wasDocked)
                DockWindowRemove(hwnd);
        }
        else if (DockIsChildOfDock(hwnd, nullptr) >= 0)
        {
            DockWindowRemove(hwnd);
        }
        if (s_state)
        {
            delete s_state;
            s_state = nullptr;
        }
        return 0;
    }

    case WM_CONTEXTMENU:
    {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        if (x == -1 || y == -1)
        {
            RECT r;
            GetWindowRect(hwnd, &r);
            x = r.left; y = r.top;
        }
        HMENU hMenu = CreatePopupMenu();
        bool isFloat = false;
        bool docked  = (DockIsChildOfDock(hwnd, &isFloat) >= 0);
        AppendMenuA(hMenu, MF_STRING | (docked ? MF_CHECKED : 0), 1, "Dock in REAPER docker");
        AppendMenuA(hMenu, MF_STRING, 2, "Close window");
        int id = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, hwnd, nullptr);
        DestroyMenu(hMenu);
        if (id == 1) ToggleDocking();
        else if (id == 2) ShowWindow(hwnd, SW_HIDE);
        return 0;
    }

    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

// ---------------------------------------------------------------------------
// Class registration
// ---------------------------------------------------------------------------

static bool s_classReg = false;

static bool EnsureClassReg(HINSTANCE hInst)
{
    if (s_classReg) return true;
    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = SEWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = k_SEClassName;
    s_classReg = (RegisterClassExA(&wc) != 0);
    return s_classReg;
}

// ---------------------------------------------------------------------------
// Toggle docking
// ---------------------------------------------------------------------------

static void ToggleDocking()
{
    if (!s_state || !s_state->hwnd) return;
    bool isFloat = false;
    bool wasDocked = (DockIsChildOfDock(s_state->hwnd, &isFloat) >= 0);
    bool newDocked = !wasDocked;
    SetExtState("reaper_transitions", "surface_editor_docked", newDocked ? "1" : "0", true);
    s_suppressDockStateSave = true;
    DestroyWindow(s_state->hwnd);
    s_suppressDockStateSave = false;
    SurfaceEditorWnd_ShowHide();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static HINSTANCE s_hInst = nullptr;

void SurfaceEditorWnd_Init(HINSTANCE hInstance)
{
    s_hInst = hInstance;
    ActionSearchDlg_Init(hInstance);
    CanvasWnd_RegisterClass(hInstance);
}

void SurfaceEditorWnd_ShowHide()
{
    if (!s_state || !s_state->hwnd || !IsWindow(s_state->hwnd))
    {
        if (!EnsureClassReg(s_hInst)) return;

        s_state = new SEState();
        s_state->hInst = s_hInst;

        int W = 1000, H = 650;
        HWND hwnd = CreateWindowExA(
            WS_EX_TOOLWINDOW,
            k_SEClassName,
            "Surface & Zone Editor",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, W, H,
            g_hwnd, nullptr, s_hInst, nullptr);

        if (!hwnd) { delete s_state; s_state = nullptr; return; }
        s_state->hwnd = hwnd;

        const char* dockPref = GetExtState("reaper_transitions", "surface_editor_docked");
        bool wantDocked = (dockPref && atoi(dockPref) != 0);
        if (wantDocked)
        {
            DockWindowAddEx(hwnd, "Surface & Zone Editor", k_DockKey, true);
            DockWindowActivate(hwnd);
        }
        else
        {
            ShowWindow(hwnd, SW_SHOW);
        }

        // --- Toolbar ---
        s_state->hNewBtn     = CreateWindowExA(0, "BUTTON", "New",     WS_CHILD|WS_VISIBLE, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_NEW_BTN,     s_hInst, nullptr);
        s_state->hSaveBtn    = CreateWindowExA(0, "BUTTON", "Save",    WS_CHILD|WS_VISIBLE, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_SAVE_BTN,    s_hInst, nullptr);
        s_state->hRefreshBtn = CreateWindowExA(0, "BUTTON", "Refresh", WS_CHILD|WS_VISIBLE, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_REFRESH_BTN, s_hInst, nullptr);

        s_state->hSurfaceLabel = CreateWindowExA(0, "STATIC", "Surface:", WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE,
            0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_SURFACE_LABEL, s_hInst, nullptr);
        s_state->hSurfaceCombo = CreateWindowExA(0, "COMBOBOX", nullptr,
            WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
            0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_SURFACE_COMBO, s_hInst, nullptr);

        s_state->hZoneLabel = CreateWindowExA(0, "STATIC", "Zone:", WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE,
            0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_ZONE_LABEL, s_hInst, nullptr);
        s_state->hZoneCombo = CreateWindowExA(0, "COMBOBOX", nullptr,
            WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
            0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_ZONE_COMBO, s_hInst, nullptr);

        s_state->hZoneSaveBtn = CreateWindowExA(0, "BUTTON", "Save Zone",
            WS_CHILD|WS_VISIBLE, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_ZONE_SAVE_BTN, s_hInst, nullptr);

        // --- Status bar ---
        s_state->hStatus = CreateWindowExA(0, "STATIC", "Ready",
            WS_CHILD|WS_VISIBLE|SS_SUNKEN, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_STATUS, s_hInst, nullptr);

        // --- Canvas ---
        s_state->hCanvas = CanvasWnd_Create(hwnd, s_hInst, 0, k_ToolbarH, W, H - k_ToolbarH - k_StatusH - k_PropBarH);
        {
            CanvasCallbacks cb;
            cb.onWidgetSelected   = [](int col, int row)                  { OnWidgetSelected(col, row, s_state); };
            cb.onWidgetMoved      = [](int sc, int sr, int dpx, int dpy)  { OnWidgetMoved(sc, sr, dpx, dpy, s_state); };
            cb.onBlankCellClicked = [](int col, int row)                  { OnBlankCellClicked(col, row, s_state); };
            cb.onWidgetActivated  = [](int col, int row)                  { OnZoneWidgetActivated(col, row, s_state); };
            CanvasWnd_SetCallbacks(s_state->hCanvas, cb);
        }

        // --- Bottom bar: Surface mode ---
        s_state->hPropNameLbl = CreateWindowExA(0, "STATIC", "Name:",
            WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_PROP_NAME_LBL, s_hInst, nullptr);
        s_state->hPropName = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_PROP_NAME, s_hInst, nullptr);
        s_state->hPropType = CreateWindowExA(0, "STATIC", "",
            WS_CHILD|WS_VISIBLE, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_PROP_GENTYPE, s_hInst, nullptr);
        s_state->hPropDelete = CreateWindowExA(0, "BUTTON", "Delete Widget",
            WS_CHILD|WS_VISIBLE, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_PROP_DELETE, s_hInst, nullptr);

        // --- Bottom bar: Zone mode ---
        s_state->hZoneNameLbl = CreateWindowExA(0, "STATIC", "Zone:",
            WS_CHILD|SS_CENTERIMAGE, 0,0,0,0, hwnd, nullptr, s_hInst, nullptr);
        s_state->hZoneNameEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD|ES_AUTOHSCROLL, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_ZONE_NAME, s_hInst, nullptr);
        s_state->hZoneTypeLbl = CreateWindowExA(0, "STATIC", "Type:",
            WS_CHILD|SS_CENTERIMAGE, 0,0,0,0, hwnd, nullptr, s_hInst, nullptr);
        s_state->hZoneTypeCombo = CreateWindowExA(0, "COMBOBOX", nullptr,
            WS_CHILD|CBS_DROPDOWNLIST|WS_VSCROLL, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_ZONE_TYPE, s_hInst, nullptr);
        // Populate zone type combo
        {
            int typeCount = 0;
            const char* const* types = GetZoneTypeNames(&typeCount);
            SendMessageA(s_state->hZoneTypeCombo, CB_ADDSTRING, 0, (LPARAM)"(custom)");
            for (int i = 0; i < typeCount; ++i)
                SendMessageA(s_state->hZoneTypeCombo, CB_ADDSTRING, 0, (LPARAM)types[i]);
            SendMessageA(s_state->hZoneTypeCombo, CB_SETCURSEL, 0, 0);
        }

        s_state->hZoneModShift   = CreateWindowExA(0, "BUTTON", "Shift",   WS_CHILD|BS_AUTOCHECKBOX, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_ZONE_MOD_SHIFT,   s_hInst, nullptr);
        s_state->hZoneModOption  = CreateWindowExA(0, "BUTTON", "Option",  WS_CHILD|BS_AUTOCHECKBOX, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_ZONE_MOD_OPTION,  s_hInst, nullptr);
        s_state->hZoneModControl = CreateWindowExA(0, "BUTTON", "Control", WS_CHILD|BS_AUTOCHECKBOX, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_ZONE_MOD_CONTROL, s_hInst, nullptr);
        s_state->hZoneModAlt     = CreateWindowExA(0, "BUTTON", "Alt",     WS_CHILD|BS_AUTOCHECKBOX, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_ZONE_MOD_ALT,     s_hInst, nullptr);
        s_state->hZoneModFlip    = CreateWindowExA(0, "BUTTON", "Flip",    WS_CHILD|BS_AUTOCHECKBOX, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_ZONE_MOD_FLIP,    s_hInst, nullptr);
        s_state->hZoneModHold    = CreateWindowExA(0, "BUTTON", "Hold",    WS_CHILD|BS_AUTOCHECKBOX, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_ZONE_MOD_HOLD,    s_hInst, nullptr);

        s_state->hZoneIncLbl  = CreateWindowExA(0, "STATIC", "Included:",   WS_CHILD|SS_CENTERIMAGE, 0,0,0,0, hwnd, nullptr, s_hInst, nullptr);
        s_state->hZoneIncEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD|ES_AUTOHSCROLL, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_ZONE_INC, s_hInst, nullptr);
        s_state->hZoneSubLbl  = CreateWindowExA(0, "STATIC", "Sub zones:",  WS_CHILD|SS_CENTERIMAGE, 0,0,0,0, hwnd, nullptr, s_hInst, nullptr);
        s_state->hZoneSubEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD|ES_AUTOHSCROLL, 0,0,0,0, hwnd, (HMENU)(INT_PTR)IDC_SE_ZONE_SUB, s_hInst, nullptr);

        // Initial state: surface mode
        ShowPropBarControls(s_state, true);
        ShowZoneBarControls(s_state, false);

        // Populate surface combo
        PopulateSurfaceCombo(s_state);

        RECT rc;
        GetClientRect(hwnd, &rc);
        DoLayout(s_state, rc.right, rc.bottom);
        return;
    }

    // Window already exists — activate if docked, otherwise toggle visibility
    HWND hwnd = s_state->hwnd;
    bool isFloat = false;
    if (DockIsChildOfDock(hwnd, &isFloat) >= 0)
    {
        DockWindowActivate(hwnd);
    }
    else
    {
        if (IsWindowVisible(hwnd))
            ShowWindow(hwnd, SW_HIDE);
        else
        {
            ShowWindow(hwnd, SW_SHOW);
            BringWindowToTop(hwnd);
        }
    }
}

int SurfaceEditorWnd_IsVisible()
{
    if (!s_state || !s_state->hwnd) return 0;
    bool isFloat = false;
    if (DockIsChildOfDock(s_state->hwnd, &isFloat) >= 0) return 1;
    return IsWindowVisible(s_state->hwnd) ? 1 : 0;
}

void SurfaceEditorWnd_Refresh()
{
    if (!s_state) return;
    PopulateSurfaceCombo(s_state);
    if (s_state->hCanvas) CanvasWnd_Refresh(s_state->hCanvas);
}

void SurfaceEditorWnd_Cleanup()
{
    if (!s_state || !s_state->hwnd) return;
    bool isFloat = false;
    if (DockIsChildOfDock(s_state->hwnd, &isFloat) >= 0)
        DockWindowRemove(s_state->hwnd);
    if (IsWindow(s_state->hwnd))
        DestroyWindow(s_state->hwnd);
    // s_state freed in WM_DESTROY
}
