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
static bool s_autoSetup  = false; // Active on project startup

// ---------------------------------------------------------------------------
// Dialog handle / instance
// ---------------------------------------------------------------------------
static HINSTANCE s_hInst   = nullptr;
static HWND      s_hwnd    = nullptr;

// Countdown ticks until deferred auto-setup runs (0 = inactive)
static int s_pendingAutoSetupTicks = 0;

// Guard: prevents re-entry when we write I_SOLO inside SetSurfaceSolo callbacks
static bool s_inCallback = false;

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
        SetDlgItemTextA(s_hwnd, IDC_PAFL_STATUS, "No PAFL bus – select a track or click New.");
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
    else if (!s_intercept)
        lstrcpynA(status, "PAFL inactive", sizeof(status));
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
    s_intercept = (!v || !v[0]) ? true : (atoi(v) != 0); // default: active
    v = GetExtState(k_appSection, "sendtype");
    s_sendType  = (v && v[0]) ? atoi(v) : 3;
    v = GetExtState(k_appSection, "autosetup");
    s_autoSetup = (v && atoi(v) != 0);
}

static void SaveSettings()
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s_intercept ? 1 : 0);
    SetExtState(k_appSection, "intercept", buf, true);
    snprintf(buf, sizeof(buf), "%d", s_sendType);
    SetExtState(k_appSection, "sendtype", buf, true);
    snprintf(buf, sizeof(buf), "%d", s_autoSetup ? 1 : 0);
    SetExtState(k_appSection, "autosetup", buf, true);
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
// Bus track combo: lists all regular tracks so the user can pick an existing
// track as the PAFL bus (or use the "New" button to create a fresh one).
// ---------------------------------------------------------------------------
static void FillBusTrackCombo(HWND hwnd)
{
    HWND hCombo = GetDlgItem(hwnd, IDC_PAFL_BUSTRACK);
    SendMessageA(hCombo, CB_RESETCONTENT, 0, 0);

    int ni = (int)SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)"<none>");
    SendMessageA(hCombo, CB_SETITEMDATA, ni, (LPARAM)-1);

    MediaTrack* busTr = GetBusTrack();
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
        if (busTr == tr) selIdx = cbIdx;
    }
    SendMessageA(hCombo, CB_SETCURSEL, selIdx, 0);
}

static MediaTrack* GetBusTrackFromCombo(HWND hwnd)
{
    int sel   = (int)SendDlgItemMessageA(hwnd, IDC_PAFL_BUSTRACK, CB_GETCURSEL, 0, 0);
    INT_PTR d = (INT_PTR)SendDlgItemMessageA(hwnd, IDC_PAFL_BUSTRACK, CB_GETITEMDATA, sel, 0);
    if (d == -1) return nullptr;
    return GetTrack(nullptr, (int)d);
}

// ---------------------------------------------------------------------------
// Core PAFL logic
// ---------------------------------------------------------------------------
void PaflToggleTrack(MediaTrack* tr)
{
    if (!tr) return;
    MediaTrack* busTr = GetBusTrack();
    if (!busTr) return;

    MediaTrack* srcTr = GetSrcTrack();

    // Determine current state: active = send exists AND is unmuted
    int idx = FindSendToTrack(tr, busTr);
    bool paflActive = false;
    if (idx >= 0)
    {
        bool* pm = (bool*)GetSetTrackSendInfo(tr, 0, idx, "B_MUTE", nullptr);
        paflActive = (pm && !*pm);
    }

    if (!paflActive)
    {
        // Activate: create send (or unmute an existing muted one from old state)
        if (idx < 0)
        {
            idx = CreateTrackSend(tr, busTr);
            if (idx < 0) return;
            GetSetTrackSendInfo(tr, 0, idx, "I_SENDMODE", &s_sendType);
        }
        bool no = false;
        GetSetTrackSendInfo(tr, 0, idx, "B_MUTE", &no);

        // Mute the program source feed
        if (srcTr)
        {
            int si = FindSendToTrack(srcTr, busTr);
            if (si >= 0) { bool yes = true; GetSetTrackSendInfo(srcTr, 0, si, "B_MUTE", &yes); }
        }
    }
    else
    {
        // Deactivate: remove the send entirely
        RemoveTrackSend(tr, 0, idx);

        // Restore program feed if no other tracks are still active
        bool anyActive = false;
        const int n = GetNumTracks();
        for (int i = 0; i < n && !anyActive; i++)
        {
            MediaTrack* t = GetTrack(nullptr, i);
            if (!t || t == busTr || t == srcTr || t == tr) continue;
            int si = FindSendToTrack(t, busTr);
            if (si < 0) continue;
            bool* pm = (bool*)GetSetTrackSendInfo(t, 0, si, "B_MUTE", nullptr);
            if (pm && !*pm) anyActive = true;
        }

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

    // Guard prevents our csurf SetSurfaceSolo handler from acting on the
    // I_SOLO clears we're about to make.
    s_inCallback = true;

    auto clearTrack = [&](MediaTrack* tr) {
        if (!tr || tr == busTr || tr == srcTr) return;
        int idx = FindSendToTrack(tr, busTr);
        if (idx < 0) return;
        // Remove the PAFL send
        RemoveTrackSend(tr, 0, idx);
        // Clear I_SOLO so the surface LED goes off (REAPER notifies surfaces)
        int* ps = (int*)GetSetMediaTrackInfo(tr, "I_SOLO", nullptr);
        if (ps && *ps != 0)
        {
            int zero = 0;
            GetSetMediaTrackInfo(tr, "I_SOLO", &zero);
        }
    };

    clearTrack(master);
    for (int i = 0; i < n; i++) clearTrack(GetTrack(nullptr, i));

    s_inCallback = false;

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

// ---------------------------------------------------------------------------
// Create a brand-new PAFL bus track (always adds a fresh track).
// ---------------------------------------------------------------------------
static void DoCreateNewBus(HWND hwnd)
{
    InsertTrackAtIndex(GetNumTracks(), false);
    TrackList_AdjustWindows(false);
    MediaTrack* busTr = GetTrack(nullptr, GetNumTracks() - 1);
    if (!busTr) return;

    GetSetMediaTrackInfo_String(busTr, "P_NAME", (char*)"PAFL", true);
    int one = 1, zero = 0, vis = 1;
    GetSetMediaTrackInfo(busTr, "B_SHOWINTCP",   &vis);
    GetSetMediaTrackInfo(busTr, "B_SHOWINMIXER", &vis);
    GetSetMediaTrackInfo(busTr, "B_SOLO_DEFEAT", &one);
    GetSetMediaTrackInfo(busTr, "B_MAINSEND",    &zero);
    StoreTrackByKey(k_busKey, busTr);

    MediaTrack* srcTr = hwnd ? GetSrcTrackFromCombo(hwnd) : GetSrcTrack();
    if (srcTr && srcTr != busTr)
    {
        EnsureSend(srcTr, busTr, 0, false);
        StoreTrackByKey(k_srcKey, srcTr);
    }

    if (hwnd)
    {
        FillBusTrackCombo(hwnd);
        FillSrcTrackCombo(hwnd);
    }
    UpdateStatus();
    UpdateTimeline();
    Undo_OnStateChangeEx("PAFL: Create new bus", UNDO_STATE_ALL, -1);
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

        int one = 1, zero = 0, vis = 1;
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

    // Sends from regular tracks are created on-demand when the user solos them.
    // DoInitBus only establishes the bus and program source – nothing else.

    if (hwnd)
    {
        FillBusTrackCombo(hwnd);
        FillSrcTrackCombo(hwnd);
    }
    UpdateStatus();
    UpdateTimeline();
    Undo_OnStateChangeEx("PAFL: Initialize bus", UNDO_STATE_ALL, -1);
}

// ---------------------------------------------------------------------------
// Timer tick (called ~30fps from REAPER main thread)
// Solo intercept is now handled by PaflMonitor (csurf_inst) which receives
// REAPER's SetSurfaceSolo events directly – I_SOLO is left intact so surface
// LEDs stay lit naturally.  The timer only drives deferred auto-setup.
// ---------------------------------------------------------------------------
void PaflWnd_TimerTick()
{
    if (s_pendingAutoSetupTicks > 0)
    {
        if (--s_pendingAutoSetupTicks == 0)
        {
            s_intercept = true;
            DoInitBus(nullptr);
            // Reflect active state in the button if the window is open
            if (s_hwnd && IsWindow(s_hwnd))
                CheckDlgButton(s_hwnd, IDC_PAFL_ACTIVE, BST_CHECKED);
            UpdateStatus();
        }
    }
}

// ---------------------------------------------------------------------------
// Dialog procedure
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK PaflDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM /*lParam*/)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        FillBusTrackCombo(hwnd);
        FillSrcTrackCombo(hwnd);
        FillSendTypeCombo(hwnd);
        CheckDlgButton(hwnd, IDC_PAFL_ACTIVE,
                       s_intercept ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_PAFL_AUTOSETUP,
                       s_autoSetup ? BST_CHECKED : BST_UNCHECKED);
        UpdateStatus();
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_PAFL_BUSTRACK:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                // Switching the bus: clear any active solos first, then point
                // at the new bus and reconnect the program source send.
                DoClearAll();
                MediaTrack* newBus = GetBusTrackFromCombo(hwnd);
                StoreTrackByKey(k_busKey, newBus);
                if (newBus)
                {
                    MediaTrack* srcTr = GetSrcTrackFromCombo(hwnd);
                    if (srcTr && srcTr != newBus)
                        EnsureSend(srcTr, newBus, 0, false);
                }
                UpdateStatus();
                UpdateTimeline();
            }
            break;

        case IDC_PAFL_NEWBUS:
            DoCreateNewBus(hwnd);
            break;

        case IDC_PAFL_SRCTRACK:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                MediaTrack* tr = GetSrcTrackFromCombo(hwnd);
                StoreTrackByKey(k_srcKey, tr);
                // Ensure program source send if bus is already configured
                MediaTrack* busTr = GetBusTrack();
                if (tr && busTr && tr != busTr)
                    EnsureSend(tr, busTr, 0, false);
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

        case IDC_PAFL_ACTIVE:
        {
            bool nowActive = (IsDlgButtonChecked(hwnd, IDC_PAFL_ACTIVE) == BST_CHECKED);
            s_intercept = nowActive;
            SaveSettings();
            if (nowActive)
            {
                // Activate: ensure the bus exists, then set up program source
                if (!GetBusTrack())
                {
                    DoCreateNewBus(hwnd);
                }
                else
                {
                    MediaTrack* busTr = GetBusTrack();
                    MediaTrack* srcTr = GetSrcTrackFromCombo(hwnd);
                    if (srcTr && busTr && srcTr != busTr)
                        EnsureSend(srcTr, busTr, 0, false);
                    UpdateStatus();
                }
            }
            else
            {
                DoClearAll();
            }
        }
        break;

        case IDC_PAFL_AUTOSETUP:
            s_autoSetup = (IsDlgButtonChecked(hwnd, IDC_PAFL_AUTOSETUP) == BST_CHECKED);
            SaveSettings();
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
// Control surface instance – receives REAPER's solo events natively.
//
// Strategy:
//   • Leave I_SOLO intact → surface LEDs stay lit without any heartbeat.
//   • Force I_SOLO=2 (solo-in-place) on activation so REAPER's solo engine
//     doesn't mute other tracks in the main mix.  Changing 1→2 keeps the
//     LED on because the value stays nonzero.
//   • s_inCallback guards against re-entry when we write I_SOLO inside the
//     callback (REAPER notifies surfaces synchronously from within the write).
// ---------------------------------------------------------------------------
class PaflMonitor : public IReaperControlSurface
{
public:
    const char* GetTypeString() override { return "PAFLTRANSITIONS"; }
    const char* GetDescString() override { return "Transition Snapshots PAFL"; }
    const char* GetConfigString() override { return ""; }

    void SetSurfaceSolo(MediaTrack* tr, bool solo) override
    {
        if (s_inCallback || !s_intercept) return;

        MediaTrack* busTr = GetBusTrack();
        if (!busTr || !tr || tr == busTr) return;

        // Don't intercept the program source track itself
        MediaTrack* srcTr = GetSrcTrack();
        if (tr == srcTr) return;

        s_inCallback = true;

        if (solo)
        {
            // Track was soloed: add a PAFL send if not already present.
            // If an old muted send exists (from a previous architecture), unmute it.
            int idx = FindSendToTrack(tr, busTr);
            if (idx < 0)
            {
                idx = CreateTrackSend(tr, busTr);
                if (idx >= 0)
                    GetSetTrackSendInfo(tr, 0, idx, "I_SENDMODE", &s_sendType);
            }
            if (idx >= 0)
            {
                bool no = false;
                GetSetTrackSendInfo(tr, 0, idx, "B_MUTE", &no);
            }

            // Mute the program source so only the soloed channel feeds the bus
            if (srcTr)
            {
                int si = FindSendToTrack(srcTr, busTr);
                if (si >= 0) { bool yes = true; GetSetTrackSendInfo(srcTr, 0, si, "B_MUTE", &yes); }
            }

            // Force solo-in-place (I_SOLO=2) so REAPER doesn't mute the main mix.
            // This call triggers another SetSurfaceSolo(tr, true) which hits the guard.
            int* ps = (int*)GetSetMediaTrackInfo(tr, "I_SOLO", nullptr);
            if (ps && *ps != 2)
            {
                int sip = 2;
                GetSetMediaTrackInfo(tr, "I_SOLO", &sip);
            }
        }
        else
        {
            // Track was unsoloed: remove the PAFL send
            int idx = FindSendToTrack(tr, busTr);
            if (idx >= 0)
                RemoveTrackSend(tr, 0, idx);

            // Restore program feed if no other tracks are still PAFL-active
            bool anyActive = false;
            const int n = GetNumTracks();
            for (int i = 0; i < n && !anyActive; i++)
            {
                MediaTrack* t = GetTrack(nullptr, i);
                if (!t || t == busTr || t == srcTr) continue;
                int si = FindSendToTrack(t, busTr);
                if (si < 0) continue;
                bool* pm = (bool*)GetSetTrackSendInfo(t, 0, si, "B_MUTE", nullptr);
                if (pm && !*pm) anyActive = true;
            }

            if (!anyActive && srcTr)
            {
                int si = FindSendToTrack(srcTr, busTr);
                if (si >= 0) { bool no = false; GetSetTrackSendInfo(srcTr, 0, si, "B_MUTE", &no); }
            }
        }

        UpdateStatus();
        UpdateTimeline();
        s_inCallback = false;
    }
};

static PaflMonitor s_paflMonitor;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void PaflWnd_Init(HINSTANCE hInstance)
{
    s_hInst = hInstance;
    LoadSettings();
    plugin_register("timer",      (void*)PaflWnd_TimerTick);
    plugin_register("csurf_inst", &s_paflMonitor);
}

void PaflWnd_Cleanup()
{
    plugin_register("-csurf_inst", &s_paflMonitor);
    plugin_register("-timer",      (void*)PaflWnd_TimerTick);
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
