// ---------------------------------------------------------------------------
// PaflWnd.cpp  –  PAFL (Pre/After Fader Listen) monitor window
//
// Standalone Win32 dialog, no SWS dependency.
// Settings persisted via REAPER's GetExtState / SetExtState (per-machine).
// Bus / source GUIDs persisted via GetProjExtState / SetProjExtState (per-project).
// Solo intercept uses a timer-based solo-state poll (called from plugin timer).
// ---------------------------------------------------------------------------

#include "PaflWnd.h"
#include "api.h"
#include "resource.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Persistence constants
// ---------------------------------------------------------------------------
static const char* k_busKey     = "bus";  // identifier for bus track
static const char* k_srcKey     = "src";  // identifier for src track
static const char* k_appSection = "reaper_transitions_PAFL"; // machine ext state

// ---------------------------------------------------------------------------
// Per-project GUID storage (populated by PaflWnd_ProcessLine on load,
// written by PaflWnd_SaveConfig on save – no dependency on SetProjExtState)
// ---------------------------------------------------------------------------
static std::string s_busGuidStr;
static std::string s_srcGuidStr;

// ---------------------------------------------------------------------------
// Per-machine settings (saved to reaper-extstate.ini)
// ---------------------------------------------------------------------------
static bool s_intercept  = false;
static int  s_sendType   = 3;   // 3 = Pre-Fader (Post-FX) in REAPER
static int  s_hwOut      = 0;   // 0 = none
static bool s_autoSetup  = false; // Ensure setup on project start
static bool s_hideFader  = true;  // Hide PAFL bus fader (default: hidden)

// ---------------------------------------------------------------------------
// Dialog handle / instance
// ---------------------------------------------------------------------------
static HINSTANCE s_hInst   = nullptr;
static HWND      s_hwnd    = nullptr;

// Countdown ticks until deferred auto-setup runs (0 = inactive)
static int s_pendingAutoSetupTicks = 0;

// ---------------------------------------------------------------------------
// GUID helpers (Windows-only, same pattern as TransitionSnapshot.cpp)
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
// Find a track by its GUID stored in our in-memory project strings.
// ---------------------------------------------------------------------------
static MediaTrack* FindTrackByKey(const char* key)
{
    const std::string& guidStr = (key == k_busKey) ? s_busGuidStr : s_srcGuidStr;
    if (guidStr.empty()) goto fallback;

    {
        GUID target = StringToGuid(guidStr.c_str());
        GUID zero   = {};
        if (IsEqualGUID(target, zero)) goto fallback;

        // Check master track
        MediaTrack* master = GetMasterTrack(nullptr);
        if (master)
        {
            GUID* pg = (GUID*)GetSetMediaTrackInfo(master, "GUID", nullptr);
            if (pg && IsEqualGUID(*pg, target)) return master;
        }

        // Regular tracks
        const int n = GetNumTracks();
        for (int i = 0; i < n; i++)
        {
            MediaTrack* tr = GetTrack(nullptr, i);
            if (!tr) continue;
            GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
            if (pg && IsEqualGUID(*pg, target)) return tr;
        }
    }

fallback:
    // Name-based fallback for bus track only
    if (key == k_busKey)
    {
        const int n = GetNumTracks();
        for (int i = 0; i < n; i++)
        {
            MediaTrack* tr = GetTrack(nullptr, i);
            if (!tr) continue;
            char name[64] = {};
            GetSetMediaTrackInfo_String(tr, "P_NAME", name, false);
            if (strcmp(name, "PAFL") == 0)
            {
                // Re-capture GUID so future lookups are fast
                GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
                if (pg) s_busGuidStr = GuidToString(*pg);
                return tr;
            }
        }
    }
    return nullptr;
}

static void StoreTrackByKey(const char* key, MediaTrack* tr)
{
    std::string& store = (key == k_busKey) ? s_busGuidStr : s_srcGuidStr;
    if (!tr)
    {
        store.clear();
        MarkProjectDirty(nullptr);
        return;
    }
    GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
    store = pg ? GuidToString(*pg) : "";
    MarkProjectDirty(nullptr);
}

// ---------------------------------------------------------------------------
// Track accessors
// ---------------------------------------------------------------------------
static MediaTrack* GetBusTrack() { return FindTrackByKey(k_busKey); }
static MediaTrack* GetSrcTrack() { return FindTrackByKey(k_srcKey); }

// ---------------------------------------------------------------------------
// Send helpers
// ---------------------------------------------------------------------------
static int FindSendToTrack(MediaTrack* srcTr, MediaTrack* destTr)
{
    const int n = GetTrackNumSends(srcTr, 0);
    for (int i = 0; i < n; i++)
    {
        MediaTrack* dst = (MediaTrack*)GetSetTrackSendInfo(srcTr, 0, i, "P_DESTTRACK", nullptr);
        if (dst == destTr) return i;
    }
    return -1;
}

// Create or update a send from srcTr to destTr. Returns send index or -1.
static int EnsureSend(MediaTrack* srcTr, MediaTrack* destTr, int sendType, bool muted)
{
    int idx = FindSendToTrack(srcTr, destTr);
    if (idx < 0)
    {
        idx = CreateTrackSend(srcTr, destTr);
        if (idx < 0) return -1;
        GetSetTrackSendInfo(srcTr, 0, idx, "I_SENDMODE", &sendType);
    }
    bool m = muted;
    GetSetTrackSendInfo(srcTr, 0, idx, "B_MUTE", &m);
    return idx;
}

// ---------------------------------------------------------------------------
// Status label update
// ---------------------------------------------------------------------------
static void UpdateStatus()
{
    if (!s_hwnd) return;

    MediaTrack* busTr = GetBusTrack();
    if (!busTr)
    {
        SetDlgItemTextA(s_hwnd, IDC_PAFL_STATUS, "No PAFL bus – click 'Initialize PAFL bus'.");
        return;
    }

    MediaTrack* srcTr = GetSrcTrack();
    std::string active;
    const int n = GetNumTracks();

    // Check master track
    MediaTrack* master = GetMasterTrack(nullptr);
    if (master && master != busTr && master != srcTr)
    {
        int idx = FindSendToTrack(master, busTr);
        if (idx >= 0)
        {
            bool* pm = (bool*)GetSetTrackSendInfo(master, 0, idx, "B_MUTE", nullptr);
            if (pm && !*pm)
                active = "Master";
        }
    }

    for (int i = 0; i < n; i++)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr || tr == busTr || tr == srcTr) continue;
        int idx = FindSendToTrack(tr, busTr);
        if (idx < 0) continue;
        bool* pm = (bool*)GetSetTrackSendInfo(tr, 0, idx, "B_MUTE", nullptr);
        if (pm && !*pm)
        {
            char name[128] = {};
            GetSetMediaTrackInfo_String(tr, "P_NAME", name, false);
            if (!name[0])
            {
                int id = 0;
                int* pid = (int*)GetSetMediaTrackInfo(tr, "IP_TRACKNUMBER", nullptr);
                if (pid) id = *pid;
                snprintf(name, sizeof(name), "Track %d", id);
            }
            if (!active.empty()) active += ", ";
            active += name;
        }
    }

    char status[256] = {};
    if (!active.empty())
        snprintf(status, sizeof(status), "PAFL: %s", active.c_str());
    else
        lstrcpynA(status, "Program", sizeof(status));

    SetDlgItemTextA(s_hwnd, IDC_PAFL_STATUS, status);
}

// ---------------------------------------------------------------------------
// Settings load / save (reaper-extstate.ini, per machine)
// ---------------------------------------------------------------------------
static void LoadSettings()
{
    const char* v;
    v = GetExtState(k_appSection, "intercept");
    s_intercept = (v && atoi(v) != 0);
    v = GetExtState(k_appSection, "sendtype");
    s_sendType  = (v && v[0]) ? atoi(v) : 3;
    v = GetExtState(k_appSection, "hwout");
    s_hwOut     = (v && v[0]) ? atoi(v) : 0;
    v = GetExtState(k_appSection, "autosetup");
    s_autoSetup = (v && atoi(v) != 0);
    v = GetExtState(k_appSection, "hidefader");
    s_hideFader = (!v || !v[0]) ? true : (atoi(v) != 0); // default: hidden
}

static void SaveSettings()
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s_intercept ? 1 : 0);
    SetExtState(k_appSection, "intercept", buf, true);
    snprintf(buf, sizeof(buf), "%d", s_sendType);
    SetExtState(k_appSection, "sendtype", buf, true);
    snprintf(buf, sizeof(buf), "%d", s_hwOut);
    SetExtState(k_appSection, "hwout", buf, true);
    snprintf(buf, sizeof(buf), "%d", s_autoSetup ? 1 : 0);
    SetExtState(k_appSection, "autosetup", buf, true);
    snprintf(buf, sizeof(buf), "%d", s_hideFader ? 1 : 0);
    SetExtState(k_appSection, "hidefader", buf, true);
}

// ---------------------------------------------------------------------------
// Combo fill helpers
// ---------------------------------------------------------------------------
static void FillSrcTrackCombo(HWND hwnd)
{
    HWND hCombo = GetDlgItem(hwnd, IDC_PAFL_SRCTRACK);
    SendMessageA(hCombo, CB_RESETCONTENT, 0, 0);

    // <none>
    int ni = (int)SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)"<none>");
    SendMessageA(hCombo, CB_SETITEMDATA, ni, (LPARAM)-1); // -1 = none

    MediaTrack* srcTr = GetSrcTrack();
    int selIdx = 0;

    // Master track first
    MediaTrack* master = GetMasterTrack(nullptr);
    if (master)
    {
        int cbIdx = (int)SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)"Master");
        SendMessageA(hCombo, CB_SETITEMDATA, cbIdx, (LPARAM)-2); // -2 = master
        if (srcTr == master) selIdx = cbIdx;
    }

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
        SendMessageA(hCombo, CB_SETITEMDATA, cbIdx, (LPARAM)i); // 0-based index
        if (srcTr == tr) selIdx = cbIdx;
    }
    SendMessageA(hCombo, CB_SETCURSEL, selIdx, 0);
}

static void FillHwOutCombo(HWND hwnd)
{
    HWND hCombo = GetDlgItem(hwnd, IDC_PAFL_HWOUT);
    SendMessageA(hCombo, CB_RESETCONTENT, 0, 0);
    int ni = (int)SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)"<none>");
    SendMessageA(hCombo, CB_SETITEMDATA, ni, 0);

    int count = 0;
    while (const char* chName = GetOutputChannelName(count))
    {
        int cbIdx = (int)SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)chName);
        SendMessageA(hCombo, CB_SETITEMDATA, cbIdx, (LPARAM)(count + 1));
        count++;
    }
    int sel = (s_hwOut >= 0 && s_hwOut <= count) ? s_hwOut : 0;
    SendMessageA(hCombo, CB_SETCURSEL, sel, 0);
}

static void FillSendTypeCombo(HWND hwnd)
{
    HWND hCombo = GetDlgItem(hwnd, IDC_PAFL_SENDTYPE);
    SendMessageA(hCombo, CB_RESETCONTENT, 0, 0);
    SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)"Post-Fader (Post-Pan)");
    SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)"Pre-Fader (Post-FX)");
    SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)"Pre-FX");
    int sel = (s_sendType == 3) ? 1 : (s_sendType == 1) ? 2 : 0;
    SendMessageA(hCombo, CB_SETCURSEL, sel, 0);
}

static int ComboIdxToSendType(int idx)
{
    switch (idx) {
        case 1: return 3;
        case 2: return 1;
        default: return 0;
    }
}

// Returns the selected source MediaTrack* from the combo (may be nullptr or master).
static MediaTrack* GetSrcTrackFromCombo(HWND hwnd)
{
    int sel  = (int)SendDlgItemMessageA(hwnd, IDC_PAFL_SRCTRACK, CB_GETCURSEL, 0, 0);
    INT_PTR d = (INT_PTR)SendDlgItemMessageA(hwnd, IDC_PAFL_SRCTRACK, CB_GETITEMDATA, sel, 0);
    if (d == -1) return nullptr;          // <none>
    if (d == -2) return GetMasterTrack(nullptr); // Master
    return GetTrack(nullptr, (int)d);     // 0-based index
}

// ---------------------------------------------------------------------------
// Core PAFL logic
// ---------------------------------------------------------------------------
void PaflToggleTrack(MediaTrack* tr)
{
    if (!tr) return;
    MediaTrack* busTr = GetBusTrack();
    if (!busTr) return;

    // Ensure send exists; create muted if absent
    int idx = FindSendToTrack(tr, busTr);
    if (idx < 0)
    {
        idx = CreateTrackSend(tr, busTr);
        if (idx < 0) return;
        GetSetTrackSendInfo(tr, 0, idx, "I_SENDMODE", &s_sendType);
        bool yes = true;
        GetSetTrackSendInfo(tr, 0, idx, "B_MUTE", &yes);
    }

    bool* pCur = (bool*)GetSetTrackSendInfo(tr, 0, idx, "B_MUTE", nullptr);
    bool curMuted = pCur ? *pCur : true;
    bool newMuted = !curMuted;
    GetSetTrackSendInfo(tr, 0, idx, "B_MUTE", &newMuted);

    MediaTrack* srcTr = GetSrcTrack();

    if (!newMuted)
    {
        // Just soloed: mute the program source send
        if (srcTr)
        {
            int si = FindSendToTrack(srcTr, busTr);
            if (si >= 0) { bool yes = true; GetSetTrackSendInfo(srcTr, 0, si, "B_MUTE", &yes); }
        }
    }
    else
    {
        // Just un-soloed: restore program feed if nothing else is active
        bool anyActive = false;
        MediaTrack* master = GetMasterTrack(nullptr);
        const int n = GetNumTracks();

        auto checkActive = [&](MediaTrack* t) {
            if (!t || t == busTr || t == srcTr) return;
            int si = FindSendToTrack(t, busTr);
            if (si < 0) return;
            bool* pm = (bool*)GetSetTrackSendInfo(t, 0, si, "B_MUTE", nullptr);
            if (pm && !*pm) anyActive = true;
        };

        checkActive(master);
        for (int i = 0; i < n && !anyActive; i++)
            checkActive(GetTrack(nullptr, i));

        if (!anyActive && srcTr)
        {
            int si = FindSendToTrack(srcTr, busTr);
            if (si >= 0) { bool no = false; GetSetTrackSendInfo(srcTr, 0, si, "B_MUTE", &no); }
        }
    }

    UpdateStatus();
    UpdateTimeline();
}

static void DoClearAll()
{
    MediaTrack* busTr = GetBusTrack();
    MediaTrack* srcTr = GetSrcTrack();
    if (!busTr) return;

    MediaTrack* master = GetMasterTrack(nullptr);
    const int n = GetNumTracks();

    auto muteTrackSend = [&](MediaTrack* tr) {
        if (!tr || tr == busTr || tr == srcTr) return;
        int idx = FindSendToTrack(tr, busTr);
        if (idx < 0) return;
        bool yes = true;
        GetSetTrackSendInfo(tr, 0, idx, "B_MUTE", &yes);
    };

    muteTrackSend(master);
    for (int i = 0; i < n; i++) muteTrackSend(GetTrack(nullptr, i));

    // Restore program feed
    if (srcTr)
    {
        int si = FindSendToTrack(srcTr, busTr);
        if (si >= 0) { bool no = false; GetSetTrackSendInfo(srcTr, 0, si, "B_MUTE", &no); }
    }

    UpdateStatus();
    UpdateTimeline();
    Undo_OnStateChangeEx("PAFL: Clear all solos", UNDO_STATE_ALL, -1);
}

static void DoInitBus(HWND hwnd)
{
    MediaTrack* srcTr = hwnd ? GetSrcTrackFromCombo(hwnd) : GetSrcTrack();

    // Reuse existing bus or create new one
    MediaTrack* busTr = GetBusTrack();
    if (!busTr)
    {
        InsertTrackAtIndex(GetNumTracks(), false);
        TrackList_AdjustWindows(false);
        busTr = GetTrack(nullptr, GetNumTracks() - 1);
        if (!busTr) return;

        GetSetMediaTrackInfo_String(busTr, "P_NAME", (char*)"PAFL", true);

        int vis = s_hideFader ? 0 : 1;
        int one = 1, zero = 0;
        GetSetMediaTrackInfo(busTr, "B_SHOWINTCP",   &vis);
        GetSetMediaTrackInfo(busTr, "B_SHOWINMIXER", &vis);
        GetSetMediaTrackInfo(busTr, "B_SOLO_DEFEAT", &one);
        GetSetMediaTrackInfo(busTr, "B_MAINSEND",    &zero);

        StoreTrackByKey(k_busKey, busTr);
    }

    // Program source: unmuted post-fader send
    if (srcTr && srcTr != busTr)
    {
        EnsureSend(srcTr, busTr, 0, false);
        StoreTrackByKey(k_srcKey, srcTr);
    }

    // Muted sends from every other track (including master)
    PreventUIRefresh(1);

    MediaTrack* master = GetMasterTrack(nullptr);
    if (master && master != busTr && master != srcTr)
        EnsureSend(master, busTr, s_sendType, true);

    const int n = GetNumTracks();
    for (int i = 0; i < n; i++)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr || tr == busTr || tr == srcTr) continue;
        EnsureSend(tr, busTr, s_sendType, true);
    }
    PreventUIRefresh(-1);

    if (hwnd) FillSrcTrackCombo(hwnd);
    UpdateStatus();
    UpdateTimeline();
    Undo_OnStateChangeEx("PAFL: Initialize bus", UNDO_STATE_ALL, -1);
}

// ---------------------------------------------------------------------------
// Solo intercept (called from plugin timer ~30ms)
// ---------------------------------------------------------------------------
void PaflWnd_TimerTick()
{
    // Handle pending auto-setup from project load
    if (s_pendingAutoSetupTicks > 0)
    {
        if (--s_pendingAutoSetupTicks == 0)
        {
            DoInitBus(nullptr);
            UpdateStatus();
        }
        return;
    }

    if (!s_intercept) return;

    MediaTrack* busTr = GetBusTrack();
    if (!busTr) return;

    MediaTrack* master = GetMasterTrack(nullptr);
    const int n = GetNumTracks();

    // Since we always clear I_SOLO immediately when intercepting,
    // any non-zero I_SOLO value is always a fresh button press.
    auto checkAndIntercept = [&](MediaTrack* tr) {
        if (!tr || tr == busTr) return;
        int* ps = (int*)GetSetMediaTrackInfo(tr, "I_SOLO", nullptr);
        if (!ps || *ps == 0) return;
        // Clear solo first to prevent feedback loops
        int zero = 0;
        GetSetMediaTrackInfo(tr, "I_SOLO", &zero);
        // Toggle this track's PAFL send
        PaflToggleTrack(tr);
    };

    checkAndIntercept(master);
    for (int i = 0; i < n; i++) checkAndIntercept(GetTrack(nullptr, i));
}

// ---------------------------------------------------------------------------
// Dialog procedure
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK PaflDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM /*lParam*/)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        FillSrcTrackCombo(hwnd);
        FillHwOutCombo(hwnd);
        FillSendTypeCombo(hwnd);
        CheckDlgButton(hwnd, IDC_PAFL_INTERCEPT,
                       s_intercept ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_PAFL_AUTOSETUP,
                       s_autoSetup ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_PAFL_HIDEFADER,
                       s_hideFader ? BST_CHECKED : BST_UNCHECKED);
        UpdateStatus();
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_PAFL_SRCTRACK:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                MediaTrack* tr = GetSrcTrackFromCombo(hwnd);
                StoreTrackByKey(k_srcKey, tr);
            }
            break;

        case IDC_PAFL_HWOUT:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                int sel = (int)SendDlgItemMessageA(hwnd, IDC_PAFL_HWOUT, CB_GETCURSEL, 0, 0);
                s_hwOut = (int)SendDlgItemMessageA(hwnd, IDC_PAFL_HWOUT, CB_GETITEMDATA, sel, 0);
                SaveSettings();
            }
            break;

        case IDC_PAFL_SENDTYPE:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                int sel = (int)SendDlgItemMessageA(hwnd, IDC_PAFL_SENDTYPE, CB_GETCURSEL, 0, 0);
                s_sendType = ComboIdxToSendType(sel);
                SaveSettings();
            }
            break;

        case IDC_PAFL_INTERCEPT:
            s_intercept = (IsDlgButtonChecked(hwnd, IDC_PAFL_INTERCEPT) == BST_CHECKED);
            SaveSettings();
            break;

        case IDC_PAFL_AUTOSETUP:
            s_autoSetup = (IsDlgButtonChecked(hwnd, IDC_PAFL_AUTOSETUP) == BST_CHECKED);
            SaveSettings();
            break;

        case IDC_PAFL_HIDEFADER:
            s_hideFader = (IsDlgButtonChecked(hwnd, IDC_PAFL_HIDEFADER) == BST_CHECKED);
            SaveSettings();
            {
                // Apply immediately to existing bus track
                MediaTrack* busTr = GetBusTrack();
                if (busTr)
                {
                    int vis = s_hideFader ? 0 : 1;
                    GetSetMediaTrackInfo(busTr, "B_SHOWINTCP",   &vis);
                    GetSetMediaTrackInfo(busTr, "B_SHOWINMIXER", &vis);
                    TrackList_AdjustWindows(false);
                }
            }
            break;

        case IDC_PAFL_INIT:
            DoInitBus(hwnd);
            break;

        case IDC_PAFL_CLEAR:
            DoClearAll();
            break;

        case IDCANCEL:
            ShowWindow(hwnd, SW_HIDE);
            break;
        }
        return TRUE;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return TRUE;

    case WM_DESTROY:
        s_hwnd = nullptr;
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void PaflWnd_Init(HINSTANCE hInstance)
{
    s_hInst = hInstance;
    LoadSettings();
    plugin_register("timer", (void*)PaflWnd_TimerTick);
}

void PaflWnd_Cleanup()
{
    plugin_register("-timer", (void*)PaflWnd_TimerTick);
    if (s_hwnd && IsWindow(s_hwnd))
    {
        DestroyWindow(s_hwnd);
        s_hwnd = nullptr;
    }
}

void PaflWnd_ShowHide()
{
    if (!s_hwnd || !IsWindow(s_hwnd))
    {
        HWND hMain = GetMainHwnd();
        s_hwnd = CreateDialogParam(s_hInst,
                                   MAKEINTRESOURCE(IDD_PAFL),
                                   hMain,
                                   PaflDlgProc,
                                   0);
        if (s_hwnd)
            ShowWindow(s_hwnd, SW_SHOW);
        return;
    }
    ShowWindow(s_hwnd, IsWindowVisible(s_hwnd) ? SW_HIDE : SW_SHOW);
}

int PaflWnd_IsVisible()
{
    return (s_hwnd && IsWindow(s_hwnd) && IsWindowVisible(s_hwnd)) ? 1 : 0;
}

void PaflWnd_OnProjectLoad()
{
    // Defer auto-setup: wait ~15 timer ticks (~450ms) for the project to finish loading
    if (s_autoSetup)
        s_pendingAutoSetupTicks = 15;
}

// ---------------------------------------------------------------------------
// Project serialization – called from reaper_transitions.cpp callbacks
// ---------------------------------------------------------------------------

// Called from BeginLoadProjectState to reset per-project GUID state before load.
void PaflWnd_ResetProjectState()
{
    s_busGuidStr.clear();
    s_srcGuidStr.clear();
    s_pendingAutoSetupTicks = 0;
}

// Called from ProcessExtensionLine for lines we own.
// Returns true if the line was consumed.
bool PaflWnd_ProcessLine(const char* line)
{
    if (strncmp(line, "LTPAFLBUS ", 10) == 0)
    {
        s_busGuidStr = line + 10;
        return true;
    }
    if (strncmp(line, "LTPAFLSRC ", 10) == 0)
    {
        s_srcGuidStr = line + 10;
        return true;
    }
    return false;
}

// Called from SaveExtensionConfig to write GUIDs into the .RPP file.
void PaflWnd_SaveConfig(ProjectStateContext* ctx)
{
    if (!ctx) return;
    if (!s_busGuidStr.empty())
    {
        char line[160];
        snprintf(line, sizeof(line), "LTPAFLBUS %s", s_busGuidStr.c_str());
        ctx->AddLine(line);
    }
    if (!s_srcGuidStr.empty())
    {
        char line[160];
        snprintf(line, sizeof(line), "LTPAFLSRC %s", s_srcGuidStr.c_str());
        ctx->AddLine(line);
    }
}
