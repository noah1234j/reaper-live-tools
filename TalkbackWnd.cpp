// ---------------------------------------------------------------------------
// TalkbackWnd.cpp  -  Talkback input routing window
//
// Manages a single "Talkback" microphone input track that can be routed
// to multiple mix buses / headphone feeds on demand.  Supports:
//   - Track volume (D_VOL) for mic gain trim (-24..+24 dB)
//   - Hardware input selection (mono or stereo pair)
//   - Routing to multiple tracks via a separate resizable popup window
//   - Auto-Dim master volume while talkback is active
//   - Toggle mode OR momentary mode (LT_TB_ON / LT_TB_OFF)
//   - Per-project GUID persistence (LTTBTRACK / LTTBDEST lines)
//   - Per-machine settings via GetExtState / SetExtState
//   - Double-click sliders to reset to 0 dB; editable gain label
// ---------------------------------------------------------------------------
#include "TalkbackWnd.h"
#include "api.h"
#include "resource.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const char* k_appSection = "reaper_transitions_TB";

// Gain slider: -24 dB ... +24 dB, 0.1 dB steps -> 480 ticks, 0 dB = tick 240
static constexpr int    k_gainSliderRange = 480;
static constexpr int    k_gainSliderZero  = 240;   // position for 0 dB
static constexpr double k_gainDbPerTick   = 0.1;
static constexpr double k_gainDbMin       = -24.0;
static constexpr double k_gainDbMax       =  24.0;

// Dim slider: -40 dB ... 0 dB, 0.1 dB steps -> 400 ticks
static constexpr int    k_dimSliderRange  = 400;
static constexpr double k_dimDbMin        = -40.0;
static constexpr double k_dimDbMax        =   0.0;
static constexpr double k_dimDbPerTick    =   0.1;

// ---------------------------------------------------------------------------
// Per-project state
// ---------------------------------------------------------------------------
static std::string              s_tbGuidStr;
static std::vector<std::string> s_destGuids;

// ---------------------------------------------------------------------------
// Per-machine settings
// ---------------------------------------------------------------------------
static bool   s_hideFader  = true;
static bool   s_autoSetup  = false;
static bool   s_autoDim    = true;
static double s_dimDb      = -18.0;
static double s_gainDb     =   0.0;
static int    s_inputCh    =   0;
static bool   s_stereo     = false;
static bool   s_modeToggle = true;

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
static bool   s_tbActive              = false;
static double s_savedMasterVol        = 1.0;
static int    s_pendingAutoSetupTicks = 0;

// Windows
static HINSTANCE  s_hInst       = nullptr;
static HWND       s_hwnd        = nullptr;   // main Talkback dialog
static HWND       s_routingHwnd = nullptr;   // separate routing popup

// Guard against re-entrant ListView checkbox updates
static bool s_populatingList = false;

// Parallel array: GUIDs for each row currently displayed in the routing ListView
static std::vector<std::string> s_destListGuids;

// Subclass IDs
static constexpr UINT_PTR k_gainSliderSubId = 101;
static constexpr UINT_PTR k_dimSliderSubId  = 102;
static constexpr UINT_PTR k_gainEditSubId   = 103;
static constexpr UINT_PTR k_dimEditSubId    = 104;

// ---------------------------------------------------------------------------
// GUID helpers
// ---------------------------------------------------------------------------
static std::string GuidToString(const GUID& g)
{
    WCHAR wbuf[64] = {};
    StringFromGUID2(g, wbuf, 64);
    char buf[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, 64, nullptr, nullptr);
    return buf;
}

static GUID StringToGuid(const char* s)
{
    GUID g = {};
    if (!s || !s[0]) return g;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (wlen <= 0) return g;
    std::vector<WCHAR> wbuf(wlen);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, wbuf.data(), wlen);
    CLSIDFromString(wbuf.data(), &g);
    return g;
}

// ---------------------------------------------------------------------------
// Track helpers
// ---------------------------------------------------------------------------
static MediaTrack* FindTrackByGuid(const std::string& guidStr)
{
    if (guidStr.empty()) return nullptr;
    GUID target = StringToGuid(guidStr.c_str());
    GUID zero   = {};
    if (IsEqualGUID(target, zero)) return nullptr;
    MediaTrack* master = GetMasterTrack(nullptr);
    if (master)
    {
        GUID* pg = (GUID*)GetSetMediaTrackInfo(master, "GUID", nullptr);
        if (pg && IsEqualGUID(*pg, target)) return master;
    }
    const int n = GetNumTracks();
    for (int i = 0; i < n; i++)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;
        GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
        if (pg && IsEqualGUID(*pg, target)) return tr;
    }
    return nullptr;
}

static MediaTrack* GetTbTrack()
{
    MediaTrack* tr = FindTrackByGuid(s_tbGuidStr);
    if (tr) return tr;
    // Name fallback
    const int n = GetNumTracks();
    for (int i = 0; i < n; i++)
    {
        tr = GetTrack(nullptr, i);
        if (!tr) continue;
        char name[64] = {};
        GetSetMediaTrackInfo_String(tr, "P_NAME", name, false);
        if (strcmp(name, "Talkback") == 0)
        {
            GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
            if (pg) s_tbGuidStr = GuidToString(*pg);
            return tr;
        }
    }
    return nullptr;
}

static void StoreTbTrack(MediaTrack* tr)
{
    if (!tr) { s_tbGuidStr.clear(); return; }
    GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
    s_tbGuidStr = pg ? GuidToString(*pg) : "";
    MarkProjectDirty(nullptr);
}

// ---------------------------------------------------------------------------
// Send helpers
// ---------------------------------------------------------------------------
static int FindSendToTrack(MediaTrack* srcTr, MediaTrack* destTr)
{
    const int n = GetTrackNumSends(srcTr, 0);
    for (int i = 0; i < n; i++)
    {
        MediaTrack* dst =
            (MediaTrack*)GetSetTrackSendInfo(srcTr, 0, i, "P_DESTTRACK", nullptr);
        if (dst == destTr) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Gain helpers -- use track volume (D_VOL) directly
// ---------------------------------------------------------------------------
static void ApplyGainToTrack()
{
    MediaTrack* tr = GetTbTrack();
    if (!tr) return;
    double linVol = pow(10.0, s_gainDb / 20.0);
    GetSetMediaTrackInfo(tr, "D_VOL", &linVol);
}

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------
static void LoadSettings()
{
    const char* v;
    v = GetExtState(k_appSection, "hidefader");
    s_hideFader  = (!v || !v[0]) ? true  : (atoi(v) != 0);
    v = GetExtState(k_appSection, "autosetup");
    s_autoSetup  = (v && atoi(v) != 0);
    v = GetExtState(k_appSection, "autodim");
    s_autoDim    = (!v || !v[0]) ? true  : (atoi(v) != 0);
    v = GetExtState(k_appSection, "dimdb");
    s_dimDb      = (v && v[0]) ? atof(v) : -18.0;
    v = GetExtState(k_appSection, "gaindb");
    s_gainDb     = (v && v[0]) ? atof(v) :   0.0;
    v = GetExtState(k_appSection, "inputch");
    s_inputCh    = (v && v[0]) ? atoi(v) :   0;
    v = GetExtState(k_appSection, "stereo");
    s_stereo     = (v && atoi(v) != 0);
    v = GetExtState(k_appSection, "modetoggle");
    s_modeToggle = (!v || !v[0]) ? true  : (atoi(v) != 0);
}

static void SaveSettings()
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d",   s_hideFader  ? 1 : 0); SetExtState(k_appSection, "hidefader",  buf, true);
    snprintf(buf, sizeof(buf), "%d",   s_autoSetup  ? 1 : 0); SetExtState(k_appSection, "autosetup",  buf, true);
    snprintf(buf, sizeof(buf), "%d",   s_autoDim    ? 1 : 0); SetExtState(k_appSection, "autodim",    buf, true);
    snprintf(buf, sizeof(buf), "%.2f", s_dimDb);               SetExtState(k_appSection, "dimdb",      buf, true);
    snprintf(buf, sizeof(buf), "%.2f", s_gainDb);              SetExtState(k_appSection, "gaindb",     buf, true);
    snprintf(buf, sizeof(buf), "%d",   s_inputCh);             SetExtState(k_appSection, "inputch",    buf, true);
    snprintf(buf, sizeof(buf), "%d",   s_stereo     ? 1 : 0); SetExtState(k_appSection, "stereo",     buf, true);
    snprintf(buf, sizeof(buf), "%d",   s_modeToggle ? 1 : 0); SetExtState(k_appSection, "modetoggle", buf, true);
}

// ---------------------------------------------------------------------------
// Track property application
// ---------------------------------------------------------------------------
static void ApplyTrackSettings(MediaTrack* tr)
{
    if (!tr) return;
    int inputVal = s_stereo ? s_inputCh : (s_inputCh | 1024);
    GetSetMediaTrackInfo(tr, "I_RECINPUT", &inputVal);
    int recmon = 1;
    GetSetMediaTrackInfo(tr, "I_RECMON", &recmon);
    bool mainSend = false;
    GetSetMediaTrackInfo(tr, "B_MAINSEND", &mainSend);
    bool soloDefeat = true;
    GetSetMediaTrackInfo(tr, "B_SOLO_DEFEAT", &soloDefeat);
    bool show = !s_hideFader;
    GetSetMediaTrackInfo(tr, "B_SHOWINTCP",   &show);
    GetSetMediaTrackInfo(tr, "B_SHOWINMIXER", &show);
    ApplyGainToTrack();
}

// ---------------------------------------------------------------------------
// Talkback On / Off
// ---------------------------------------------------------------------------
static void UpdateTalkBtn()
{
    if (!s_hwnd) return;
    HWND hBtn = GetDlgItem(s_hwnd, IDC_TB_TALK);
    if (s_tbActive)
    {
        SetWindowTextA(hBtn, "TALKBACK ON");
        SendMessageA(hBtn, BM_SETCHECK, BST_CHECKED, 0);
    }
    else
    {
        SetWindowTextA(hBtn, "TALKBACK OFF");
        SendMessageA(hBtn, BM_SETCHECK, BST_UNCHECKED, 0);
    }
}

void TalkbackWnd_TbOn()
{
    if (s_tbActive) return;
    s_tbActive = true;

    MediaTrack* tbTr = GetTbTrack();

    // Auto-dim master volume
    if (s_autoDim)
    {
        MediaTrack* master = GetMasterTrack(nullptr);
        if (master)
        {
            double* pv = (double*)GetSetMediaTrackInfo(master, "D_VOL", nullptr);
            if (pv) s_savedMasterVol = *pv;
            double dimVol = s_savedMasterVol * pow(10.0, s_dimDb / 20.0);
            GetSetMediaTrackInfo(master, "D_VOL", &dimVol);
        }
    }

    // Create sends from talkback to each destination (batched)
    if (tbTr && !s_destGuids.empty())
    {
        PreventUIRefresh(1);
        for (const std::string& guid : s_destGuids)
        {
            MediaTrack* dest = FindTrackByGuid(guid);
            if (!dest) continue;
            if (FindSendToTrack(tbTr, dest) < 0)
                CreateTrackSend(tbTr, dest);
        }
        PreventUIRefresh(-1);
        TrackList_AdjustWindows(false);
    }

    UpdateTalkBtn();
}

void TalkbackWnd_TbOff()
{
    if (!s_tbActive) return;
    s_tbActive = false;

    MediaTrack* tbTr = GetTbTrack();

    // Remove sends (batched)
    if (tbTr && !s_destGuids.empty())
    {
        PreventUIRefresh(1);
        for (const std::string& guid : s_destGuids)
        {
            MediaTrack* dest = FindTrackByGuid(guid);
            if (!dest) continue;
            int idx = FindSendToTrack(tbTr, dest);
            if (idx >= 0 && RemoveTrackSend)
                RemoveTrackSend(tbTr, 0, idx);
        }
        PreventUIRefresh(-1);
        TrackList_AdjustWindows(false);
    }

    // Restore master volume
    if (s_autoDim)
    {
        MediaTrack* master = GetMasterTrack(nullptr);
        if (master)
            GetSetMediaTrackInfo(master, "D_VOL", &s_savedMasterVol);
    }

    UpdateTalkBtn();
}

// ---------------------------------------------------------------------------
// Label helpers
// ---------------------------------------------------------------------------
static void UpdateGainLabel(HWND hwnd)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", s_gainDb);
    SetDlgItemTextA(hwnd, IDC_TB_GAIN_LABEL, buf);
}

static void UpdateDimLabel(HWND hwnd)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", s_dimDb);
    SetDlgItemTextA(hwnd, IDC_TB_DIM_LABEL, buf);
}

// ---------------------------------------------------------------------------
// Commit the typed dim value from IDC_TB_DIM_LABEL edit control
// ---------------------------------------------------------------------------
static void CommitDimEdit(HWND hwnd)
{
    char buf[32] = {};
    GetDlgItemTextA(hwnd, IDC_TB_DIM_LABEL, buf, sizeof(buf));
    double v = atof(buf);
    if (v < k_dimDbMin) v = k_dimDbMin;
    if (v > k_dimDbMax) v = k_dimDbMax;
    s_dimDb = v;
    int pos = (int)round((s_dimDb - k_dimDbMin) / k_dimDbPerTick);
    if (pos < 0) pos = 0;
    if (pos > k_dimSliderRange) pos = k_dimSliderRange;
    SendDlgItemMessageA(hwnd, IDC_TB_DIM_SLIDER, TBM_SETPOS, TRUE, pos);
    UpdateDimLabel(hwnd);
    SaveSettings();
}

// ---------------------------------------------------------------------------
// Commit the typed gain value from IDC_TB_GAIN_LABEL edit control
// ---------------------------------------------------------------------------
static void CommitGainEdit(HWND hwnd)
{
    char buf[32] = {};
    GetDlgItemTextA(hwnd, IDC_TB_GAIN_LABEL, buf, sizeof(buf));
    double v = atof(buf);
    if (v < k_gainDbMin) v = k_gainDbMin;
    if (v > k_gainDbMax) v = k_gainDbMax;
    s_gainDb = v;
    int pos = k_gainSliderZero + (int)round(s_gainDb / k_gainDbPerTick);
    if (pos < 0) pos = 0;
    if (pos > k_gainSliderRange) pos = k_gainSliderRange;
    SendDlgItemMessageA(hwnd, IDC_TB_GAIN_SLIDER, TBM_SETPOS, TRUE, pos);
    UpdateGainLabel(hwnd);
    ApplyGainToTrack();
    SaveSettings();
}

// ---------------------------------------------------------------------------
// Dialog fill helpers
// ---------------------------------------------------------------------------
static void FillTrackCombo(HWND hwnd)
{
    HWND hCombo = GetDlgItem(hwnd, IDC_TB_TRACK);
    SendMessageA(hCombo, CB_RESETCONTENT, 0, 0);
    int ni = (int)SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)"<none>");
    SendMessageA(hCombo, CB_SETITEMDATA, ni, (LPARAM)-1);

    MediaTrack* tbTr = GetTbTrack();
    int selIdx = 0;
    const int n = GetNumTracks();
    for (int i = 0; i < n; i++)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;
        char name[128] = {};
        GetSetMediaTrackInfo_String(tr, "P_NAME", name, false);
        char label[160] = {};
        if (name[0])
            snprintf(label, sizeof(label), "%d: %s", i + 1, name);
        else
            snprintf(label, sizeof(label), "Track %d", i + 1);
        int cbIdx = (int)SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)label);
        SendMessageA(hCombo, CB_SETITEMDATA, cbIdx, (LPARAM)i);
        if (tbTr == tr) selIdx = cbIdx;
    }
    SendMessageA(hCombo, CB_SETCURSEL, selIdx, 0);
}

static void FillInputCombo(HWND hwnd)
{
    HWND hCombo = GetDlgItem(hwnd, IDC_TB_INPUT);
    SendMessageA(hCombo, CB_RESETCONTENT, 0, 0);
    int selIdx = 0, count = 0;
    if (GetInputChannelName)
    {
        while (const char* chName = GetInputChannelName(count))
        {
            int cbIdx = (int)SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)chName);
            SendMessageA(hCombo, CB_SETITEMDATA, cbIdx, (LPARAM)count);
            if (count == s_inputCh) selIdx = cbIdx;
            count++;
        }
    }
    if (count == 0)
    {
        SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)"(no inputs)");
        SendMessageA(hCombo, CB_SETITEMDATA, 0, 0);
    }
    SendMessageA(hCombo, CB_SETCURSEL, selIdx, 0);
}

// ---------------------------------------------------------------------------
// Routing window -- destination checklist
// ---------------------------------------------------------------------------

// Fill / refresh the routing ListView from current project tracks.
static void FillDestList()
{
    if (!s_routingHwnd) return;
    HWND hList = GetDlgItem(s_routingHwnd, IDC_TBR_LIST);
    if (!hList) return;

    s_populatingList = true;
    SendMessageA(hList, LVM_DELETEALLITEMS, 0, 0);
    s_destListGuids.clear();

    MediaTrack* tbTr = GetTbTrack();
    const int n = GetNumTracks();
    for (int i = 0; i < n; i++)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr || tr == tbTr) continue;

        char name[128] = {};
        GetSetMediaTrackInfo_String(tr, "P_NAME", name, false);
        char label[160] = {};
        if (name[0])
            snprintf(label, sizeof(label), "%d: %s", i + 1, name);
        else
            snprintf(label, sizeof(label), "Track %d", i + 1);

        GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
        std::string guidStr = pg ? GuidToString(*pg) : "";

        LVITEMA lvi = {};
        lvi.mask    = LVIF_TEXT;
        lvi.iItem   = (int)s_destListGuids.size();
        lvi.pszText = label;
        SendMessageA(hList, LVM_INSERTITEMA, 0, (LPARAM)&lvi);
        s_destListGuids.push_back(guidStr);

        bool checked = false;
        for (const auto& g : s_destGuids)
            if (!g.empty() && g == guidStr) { checked = true; break; }
        ListView_SetCheckState(hList, (int)s_destListGuids.size() - 1,
                               checked ? TRUE : FALSE);
    }
    s_populatingList = false;
}

// Routing popup dialog procedure.
static INT_PTR CALLBACK RoutingDlgProc(HWND hwnd, UINT msg,
                                        WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        HWND hList = GetDlgItem(hwnd, IDC_TBR_LIST);
        ListView_SetExtendedListViewStyle(hList,
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNA col = {};
        col.mask = LVCF_WIDTH;
        col.cx   = 400;
        SendMessageA(hList, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
        FillDestList();
        // Resize list to fill client
        RECT rc; GetClientRect(hwnd, &rc);
        const int m = 4;
        MoveWindow(hList, m, m,
                   rc.right  - rc.left - 2*m,
                   rc.bottom - rc.top  - 2*m, TRUE);
        return TRUE;
    }

    case WM_SIZE:
    {
        HWND hList = GetDlgItem(hwnd, IDC_TBR_LIST);
        if (hList)
        {
            const int m = 4;
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            MoveWindow(hList, m, m, w - 2*m, h - 2*m, TRUE);
            // Keep column width flush with list width
            int scrollW = GetSystemMetrics(SM_CXVSCROLL);
            int colW = w - 2*m - scrollW - 2;
            if (colW < 60) colW = 60;
            LVCOLUMNA col = {};
            col.mask = LVCF_WIDTH;
            col.cx   = colW;
            SendMessageA(hList, LVM_SETCOLUMN, 0, (LPARAM)&col);
        }
        return 0;
    }

    case WM_NOTIFY:
    {
        NMHDR* pnm = (NMHDR*)lParam;
        if (pnm->idFrom == IDC_TBR_LIST && pnm->code == LVN_ITEMCHANGED)
        {
            if (s_populatingList) break;
            NMLISTVIEW* plv = (NMLISTVIEW*)lParam;
            if (plv->uChanged & LVIF_STATE)
            {
                bool wasChecked = ((plv->uOldState & LVIS_STATEIMAGEMASK) >> 12) == 2;
                bool isChecked  = ((plv->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
                if (wasChecked != isChecked &&
                    plv->iItem >= 0 &&
                    plv->iItem < (int)s_destListGuids.size())
                {
                    const std::string& guid = s_destListGuids[plv->iItem];
                    if (isChecked)
                    {
                        bool found = false;
                        for (const auto& g : s_destGuids)
                            if (g == guid) { found = true; break; }
                        if (!found) s_destGuids.push_back(guid);
                    }
                    else
                    {
                        for (auto it = s_destGuids.begin();
                             it != s_destGuids.end(); ++it)
                            if (*it == guid) { s_destGuids.erase(it); break; }
                    }
                    MarkProjectDirty(nullptr);
                }
            }
        }
        break;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return TRUE;

    case WM_DESTROY:
        s_routingHwnd = nullptr;
        return 0;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Slider and edit subclass procedures (double-click to reset; Enter to commit)
// ---------------------------------------------------------------------------

static LRESULT CALLBACK GainSliderSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*subId*/, DWORD_PTR /*refData*/)
{
    if (msg == WM_LBUTTONDBLCLK)
    {
        HWND hDlg = GetParent(hwnd);
        s_gainDb = 0.0;
        SendMessageA(hwnd, TBM_SETPOS, TRUE, k_gainSliderZero);
        UpdateGainLabel(hDlg);
        ApplyGainToTrack();
        SaveSettings();
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK DimSliderSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*subId*/, DWORD_PTR /*refData*/)
{
    if (msg == WM_LBUTTONDBLCLK)
    {
        HWND hDlg = GetParent(hwnd);
        // Double-click resets dim to 0 dB (no dimming)
        s_dimDb = 0.0;
        int pos = (int)round((s_dimDb - k_dimDbMin) / k_dimDbPerTick);
        SendMessageA(hwnd, TBM_SETPOS, TRUE, pos);
        UpdateDimLabel(hDlg);
        SaveSettings();
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK GainEditSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*subId*/, DWORD_PTR /*refData*/)
{
    if (msg == WM_KEYDOWN && wParam == VK_RETURN)
    {
        CommitGainEdit(GetParent(hwnd));
        return 0;
    }
    if (msg == WM_CHAR && wParam == 13)
        return 0;
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK DimEditSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*subId*/, DWORD_PTR /*refData*/)
{
    if (msg == WM_KEYDOWN && wParam == VK_RETURN)
    {
        CommitDimEdit(GetParent(hwnd));
        return 0;
    }
    if (msg == WM_CHAR && wParam == 13)
        return 0;
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// New talkback track creation
// ---------------------------------------------------------------------------
static void DoCreateNewTrack(HWND hwnd)
{
    const int n = GetNumTracks();
    InsertTrackAtIndex(n, false);
    MediaTrack* tr = GetTrack(nullptr, n);
    if (!tr) return;
    GetSetMediaTrackInfo_String(tr, "P_NAME", (char*)"Talkback", true);
    StoreTbTrack(tr);
    ApplyTrackSettings(tr);
    FillTrackCombo(hwnd);
    FillDestList();
}

// ---------------------------------------------------------------------------
// Main Talkback dialog procedure
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK TalkbackDlgProc(HWND hwnd, UINT msg,
                                          WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        FillTrackCombo(hwnd);
        FillInputCombo(hwnd);
        CheckDlgButton(hwnd, IDC_TB_STEREO, s_stereo ? BST_CHECKED : BST_UNCHECKED);

        // Gain slider
        SendDlgItemMessageA(hwnd, IDC_TB_GAIN_SLIDER, TBM_SETRANGE, TRUE,
                            MAKELONG(0, k_gainSliderRange));
        {
            int pos = k_gainSliderZero + (int)round(s_gainDb / k_gainDbPerTick);
            if (pos < 0) pos = 0; if (pos > k_gainSliderRange) pos = k_gainSliderRange;
            SendDlgItemMessageA(hwnd, IDC_TB_GAIN_SLIDER, TBM_SETPOS, TRUE, pos);
        }
        UpdateGainLabel(hwnd);

        // Dim slider
        SendDlgItemMessageA(hwnd, IDC_TB_DIM_SLIDER, TBM_SETRANGE, TRUE,
                            MAKELONG(0, k_dimSliderRange));
        {
            int pos = (int)round((s_dimDb - k_dimDbMin) / k_dimDbPerTick);
            if (pos < 0) pos = 0; if (pos > k_dimSliderRange) pos = k_dimSliderRange;
            SendDlgItemMessageA(hwnd, IDC_TB_DIM_SLIDER, TBM_SETPOS, TRUE, pos);
        }
        UpdateDimLabel(hwnd);

        CheckRadioButton(hwnd, IDC_TB_MODE_TOGGLE, IDC_TB_MODE_MOMENTARY,
                         s_modeToggle ? IDC_TB_MODE_TOGGLE : IDC_TB_MODE_MOMENTARY);
        CheckDlgButton(hwnd, IDC_TB_AUTODIM,    s_autoDim    ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_TB_HIDEFADER,  s_hideFader  ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_TB_AUTOSETUP,  s_autoSetup  ? BST_CHECKED : BST_UNCHECKED);

        // Install subclasses: sliders get double-click-to-reset; gain edit gets Enter-to-commit
        SetWindowSubclass(GetDlgItem(hwnd, IDC_TB_GAIN_SLIDER),
                          GainSliderSubclassProc, k_gainSliderSubId, 0);
        SetWindowSubclass(GetDlgItem(hwnd, IDC_TB_DIM_SLIDER),
                          DimSliderSubclassProc,  k_dimSliderSubId,  0);
        SetWindowSubclass(GetDlgItem(hwnd, IDC_TB_GAIN_LABEL),
                          GainEditSubclassProc,   k_gainEditSubId,   0);
        SetWindowSubclass(GetDlgItem(hwnd, IDC_TB_DIM_LABEL),
                          DimEditSubclassProc,    k_dimEditSubId,    0);

        UpdateTalkBtn();
        return TRUE;
    }

    case WM_COMMAND:
    {
        int ctrl = LOWORD(wParam);
        switch (ctrl)
        {
        case IDC_TB_TALK:
            if (HIWORD(wParam) == BN_CLICKED)
            {
                if (s_tbActive) TalkbackWnd_TbOff();
                else            TalkbackWnd_TbOn();
            }
            break;

        case IDC_TB_ROUTE_BTN:
        {
            if (!s_routingHwnd)
            {
                s_routingHwnd = CreateDialogA(s_hInst,
                                              MAKEINTRESOURCEA(IDD_TALKBACK_ROUTING),
                                              hwnd,
                                              RoutingDlgProc);
            }
            if (s_routingHwnd)
            {
                if (!IsWindowVisible(s_routingHwnd))
                {
                    FillDestList();
                    ShowWindow(s_routingHwnd, SW_SHOW);
                }
                SetForegroundWindow(s_routingHwnd);
            }
            break;
        }

        case IDC_TB_NEWTRACK:
            DoCreateNewTrack(hwnd);
            break;

        case IDC_TB_TRACK:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                int sel = (int)SendDlgItemMessageA(hwnd, IDC_TB_TRACK,
                                                    CB_GETCURSEL, 0, 0);
                INT_PTR d = (INT_PTR)SendDlgItemMessageA(hwnd, IDC_TB_TRACK,
                                                          CB_GETITEMDATA, sel, 0);
                MediaTrack* tr = (d >= 0) ? GetTrack(nullptr, (int)d) : nullptr;
                StoreTbTrack(tr);
                FillDestList();
            }
            break;

        case IDC_TB_INPUT:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                int sel = (int)SendDlgItemMessageA(hwnd, IDC_TB_INPUT,
                                                    CB_GETCURSEL, 0, 0);
                s_inputCh = (int)(INT_PTR)SendDlgItemMessageA(hwnd, IDC_TB_INPUT,
                                                               CB_GETITEMDATA, sel, 0);
                MediaTrack* tr = GetTbTrack();
                if (tr) ApplyTrackSettings(tr);
                SaveSettings();
            }
            break;

        case IDC_TB_STEREO:
            s_stereo = (IsDlgButtonChecked(hwnd, IDC_TB_STEREO) == BST_CHECKED);
            { MediaTrack* tr = GetTbTrack(); if (tr) ApplyTrackSettings(tr); }
            SaveSettings();
            break;

        case IDC_TB_AUTODIM:
            s_autoDim = (IsDlgButtonChecked(hwnd, IDC_TB_AUTODIM) == BST_CHECKED);
            SaveSettings();
            break;

        case IDC_TB_HIDEFADER:
            s_hideFader = (IsDlgButtonChecked(hwnd, IDC_TB_HIDEFADER) == BST_CHECKED);
            {
                MediaTrack* tr = GetTbTrack();
                if (tr)
                {
                    bool show = !s_hideFader;
                    GetSetMediaTrackInfo(tr, "B_SHOWINTCP",   &show);
                    GetSetMediaTrackInfo(tr, "B_SHOWINMIXER", &show);
                    TrackList_AdjustWindows(false);
                }
            }
            SaveSettings();
            break;

        case IDC_TB_AUTOSETUP:
            s_autoSetup = (IsDlgButtonChecked(hwnd, IDC_TB_AUTOSETUP) == BST_CHECKED);
            SaveSettings();
            break;

        case IDC_TB_MODE_TOGGLE:
        case IDC_TB_MODE_MOMENTARY:
            s_modeToggle = (ctrl == IDC_TB_MODE_TOGGLE);
            SaveSettings();
            break;

        case IDC_TB_GAIN_LABEL:
            if (HIWORD(wParam) == EN_KILLFOCUS)
                CommitGainEdit(hwnd);
            break;

        case IDC_TB_DIM_LABEL:
            if (HIWORD(wParam) == EN_KILLFOCUS)
                CommitDimEdit(hwnd);
            break;
        }
        return 0;
    }

    case WM_HSCROLL:
    {
        HWND hCtrl = (HWND)lParam;
        HWND hGain = GetDlgItem(hwnd, IDC_TB_GAIN_SLIDER);
        HWND hDim  = GetDlgItem(hwnd, IDC_TB_DIM_SLIDER);
        if (hCtrl == hGain)
        {
            int pos = (int)SendMessageA(hGain, TBM_GETPOS, 0, 0);
            s_gainDb = (pos - k_gainSliderZero) * k_gainDbPerTick;
            UpdateGainLabel(hwnd);
            ApplyGainToTrack();
            SaveSettings();
        }
        else if (hCtrl == hDim)
        {
            int pos = (int)SendMessageA(hDim, TBM_GETPOS, 0, 0);
            s_dimDb = k_dimDbMin + pos * k_dimDbPerTick;
            UpdateDimLabel(hwnd);
            SaveSettings();
        }
        return 0;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return TRUE;

    case WM_DESTROY:
        RemoveWindowSubclass(GetDlgItem(hwnd, IDC_TB_GAIN_SLIDER),
                             GainSliderSubclassProc, k_gainSliderSubId);
        RemoveWindowSubclass(GetDlgItem(hwnd, IDC_TB_DIM_SLIDER),
                             DimSliderSubclassProc,  k_dimSliderSubId);
        RemoveWindowSubclass(GetDlgItem(hwnd, IDC_TB_GAIN_LABEL),
                             GainEditSubclassProc,   k_gainEditSubId);
        RemoveWindowSubclass(GetDlgItem(hwnd, IDC_TB_DIM_LABEL),
                             DimEditSubclassProc,    k_dimEditSubId);
        s_hwnd = nullptr;
        return 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Timer tick
// ---------------------------------------------------------------------------
static void TimerTick()
{
    if (s_pendingAutoSetupTicks > 0)
    {
        --s_pendingAutoSetupTicks;
        if (s_pendingAutoSetupTicks == 0)
            TalkbackWnd_TbOn();
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void TalkbackWnd_Init(HINSTANCE hInstance)
{
    s_hInst = hInstance;
    LoadSettings();
    plugin_register("timer", (void*)TimerTick);
}

void TalkbackWnd_Cleanup()
{
    plugin_register("-timer", (void*)TimerTick);
    if (s_routingHwnd)
    {
        DestroyWindow(s_routingHwnd);
        s_routingHwnd = nullptr;
    }
    if (s_hwnd)
    {
        DestroyWindow(s_hwnd);
        s_hwnd = nullptr;
    }
}

void TalkbackWnd_ShowHide()
{
    if (!s_hwnd)
    {
        s_hwnd = CreateDialogA(s_hInst,
                               MAKEINTRESOURCEA(IDD_TALKBACK),
                               GetMainHwnd ? GetMainHwnd() : nullptr,
                               TalkbackDlgProc);
    }
    if (s_hwnd)
    {
        if (IsWindowVisible(s_hwnd))
            ShowWindow(s_hwnd, SW_HIDE);
        else
            ShowWindow(s_hwnd, SW_SHOW);
    }
}

int TalkbackWnd_IsVisible()
{
    return (s_hwnd && IsWindowVisible(s_hwnd)) ? 1 : 0;
}

void TalkbackWnd_OnProjectLoad()
{
    s_tbGuidStr.clear();
    s_destGuids.clear();
    s_tbActive       = false;
    s_savedMasterVol = 1.0;
    if (s_autoSetup)
        s_pendingAutoSetupTicks = 15;
}

bool TalkbackWnd_ProcessLine(const char* line)
{
    while (*line == ' ' || *line == '\t') ++line;
    if (strncmp(line, "LTTBTRACK ", 10) == 0)
    {
        s_tbGuidStr = line + 10;
        while (!s_tbGuidStr.empty() &&
               (s_tbGuidStr.back() == ' '  ||
                s_tbGuidStr.back() == '\r' ||
                s_tbGuidStr.back() == '\n'))
            s_tbGuidStr.pop_back();
        return true;
    }
    if (strncmp(line, "LTTBDEST ", 9) == 0)
    {
        std::string g = line + 9;
        while (!g.empty() &&
               (g.back() == ' ' || g.back() == '\r' || g.back() == '\n'))
            g.pop_back();
        if (!g.empty()) s_destGuids.push_back(g);
        return true;
    }
    return false;
}

void TalkbackWnd_SaveConfig(ProjectStateContext* ctx)
{
    if (!s_tbGuidStr.empty())
    {
        char line[256];
        snprintf(line, sizeof(line), "LTTBTRACK %s", s_tbGuidStr.c_str());
        ctx->AddLine("%s", line);
    }
    for (const auto& g : s_destGuids)
    {
        if (g.empty()) continue;
        char line[256];
        snprintf(line, sizeof(line), "LTTBDEST %s", g.c_str());
        ctx->AddLine("%s", line);
    }
}