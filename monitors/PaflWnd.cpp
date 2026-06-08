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
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <set>

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
static bool s_debugLog   = false; // Toggled via IDC_PAFL_DEBUGLOG

// ---------------------------------------------------------------------------
// Debug logging helper – writes to REAPER console (Actions > Show REAPER console)
// ---------------------------------------------------------------------------
static void PaflLog(const char* fmt, ...)
{
    if (!s_debugLog) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    // Prepend tag so log lines are easy to grep
    char tagged[560];
    snprintf(tagged, sizeof(tagged), "[PAFL] %s\n", buf);
    ShowConsoleMsg(tagged);
}

// ---------------------------------------------------------------------------
// Dialog handle / instance
// ---------------------------------------------------------------------------
static HINSTANCE s_hInst   = nullptr;
static HWND      s_hwnd    = nullptr;

// Countdown ticks until deferred auto-setup runs (0 = inactive)
static int s_pendingAutoSetupTicks = 0;

// Guard: prevents re-entry when we write I_SOLO inside SetSurfaceSolo callbacks
static bool s_inCallback = false;

// Set of tracks currently PAFL-active (send created by this plugin).
// Used for anyActive check so permanent/infrastructure sends don't block restore.
static std::set<MediaTrack*> s_activePaflTracks;

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
                int id = (int)(intptr_t)GetSetMediaTrackInfo(tr, "IP_TRACKNUMBER", nullptr);
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
    v = GetExtState(k_appSection, "debuglog");
    s_debugLog  = (v && atoi(v) != 0);
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
    snprintf(buf, sizeof(buf), "%d", s_debugLog ? 1 : 0);
    SetExtState(k_appSection, "debuglog", buf, true);
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
        if (idx >= 0)
            RemoveTrackSend(tr, 0, idx);
        // Clear I_SOLO for any track that had a PAFL send, AND for folder parents
        // that may have had I_SOLO=2 left over from a previous buggy intercept
        // (no PAFL send required — the stale solo state is enough to cause harm).
        int* pfd = (int*)GetSetMediaTrackInfo(tr, "I_FOLDERDEPTH", nullptr);
        bool isFolderParent = (pfd && *pfd > 0);
        if (idx >= 0 || isFolderParent)
        {
            int* ps = (int*)GetSetMediaTrackInfo(tr, "I_SOLO", nullptr);
            if (ps && *ps != 0)
            {
                int zero = 0;
                GetSetMediaTrackInfo(tr, "I_SOLO", &zero);
            }
        }
    };

    clearTrack(master);
    for (int i = 0; i < n; i++) clearTrack(GetTrack(nullptr, i));

    // Always clear I_SOLO on the REAPER master bus regardless of whether it had
    // a PAFL send — REAPER sets it as a side-effect of any track solo and we
    // need to clean it up so it doesn't re-fire SetSurfaceSolo after clearing.
    if (master)
    {
        int* ms = (int*)GetSetMediaTrackInfo(master, "I_SOLO", nullptr);
        if (ms && *ms != 0)
        {
            int zero = 0;
            GetSetMediaTrackInfo(master, "I_SOLO", &zero);
        }
    }

    s_inCallback = false;
    s_activePaflTracks.clear(); // reset PAFL tracking set

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
        CheckDlgButton(hwnd, IDC_PAFL_DEBUGLOG,
                       s_debugLog  ? BST_CHECKED : BST_UNCHECKED);
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

        case IDC_PAFL_DEBUGLOG:
            s_debugLog = (IsDlgButtonChecked(hwnd, IDC_PAFL_DEBUGLOG) == BST_CHECKED);
            SaveSettings();
            if (s_debugLog)
                ShowConsoleMsg("[PAFL] Debug logging enabled. Open Actions > Show REAPER console to view.\n");
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
// Folder-aware solo helper.
//
// REAPER only fires SetSurfaceSolo for folder PARENTS when a child track
// inside a folder is soloed – it never fires for the child itself.  The
// child therefore keeps I_SOLO=1 (regular solo) which causes REAPER's solo
// engine to mute other tracks and break the main mix.
//
// This helper scans every child track inside the folder, forces I_SOLO=2
// (solo-in-place) on any child with a regular solo, and creates/removes
// PAFL sends accordingly.
// ---------------------------------------------------------------------------
static void HandleFolderSolo(MediaTrack* folderParent, bool solo,
                              MediaTrack* busTr, MediaTrack* srcTr)
{
    const int n = GetNumTracks();

    // Locate the folder parent's index
    int parentIdx = -1;
    for (int i = 0; i < n; i++)
    {
        if (GetTrack(nullptr, i) == folderParent) { parentIdx = i; break; }
    }
    if (parentIdx < 0) return;

    // Walk children. depth starts at 1 (we're inside the folder just opened
    // by folderParent). Each child's I_FOLDERDEPTH adjusts the running depth:
    //   positive → opens sub-folders, negative → closes levels.
    // We stop when depth drops to <= 0 (fully exited the folder).
    //
    // NOTE: REAPER sets each track's I_SOLO immediately before firing that
    // track's own SetSurfaceSolo callback – NOT before the folder parent's
    // callback.  So when this helper runs for the parent, children still read
    // I_SOLO=0 even if they are about to be soloed.  We therefore can only act
    // on children whose I_SOLO is already set (uncommon ordering), and must
    // not mute the program feed if no PAFL sends were actually created.
    bool anySendActive = false; // set true if we created/unmuted any child send

    int depth = 1;
    for (int i = parentIdx + 1; i < n && depth > 0; i++)
    {
        MediaTrack* child = GetTrack(nullptr, i);
        if (!child || child == busTr || child == srcTr)
        {
            int* pfd2 = child ? (int*)GetSetMediaTrackInfo(child, "I_FOLDERDEPTH", nullptr) : nullptr;
            depth += pfd2 ? *pfd2 : 0;
            continue;
        }

        int* pfd = (int*)GetSetMediaTrackInfo(child, "I_FOLDERDEPTH", nullptr);
        int  fdv  = pfd ? *pfd : 0;

        int* ps = (int*)GetSetMediaTrackInfo(child, "I_SOLO", nullptr);
        int  childSolo = ps ? *ps : -1;
        int  cnum = (int)(intptr_t)GetSetMediaTrackInfo(child, "IP_TRACKNUMBER", nullptr);
        PaflLog("  -> [scan] child track=%d I_SOLO=%d I_FOLDERDEPTH=%d", cnum, childSolo, fdv);

        if (solo)
        {
            if (fdv > 0)
            {
                // Sub-folder parent: upgrade I_SOLO if it's regular solo (keeps
                // REAPER's SIP state consistent), but do NOT create a PAFL send.
                // This track is a bus, not an audio source.  Its own nested children
                // will either fire their own callbacks or be handled when HandleFolderSolo
                // is dispatched for this sub-folder parent's own callback.
                if (ps && *ps != 0 && *ps != 2)
                {
                    int cnum = (int)(intptr_t)GetSetMediaTrackInfo(child, "IP_TRACKNUMBER", nullptr);
                    PaflLog("  -> sub-folder parent %d: I_SOLO %d->2 (SIP, no send)", cnum, *ps);
                    int sip = 2;
                    GetSetMediaTrackInfo(child, "I_SOLO", &sip);
                }
            }
            else
            {
                // Leaf / folder-closing track: upgrade I_SOLO if needed, create PAFL send.
                if (ps && *ps != 0 && *ps != 2)
                {
                    int cnum = (int)(intptr_t)GetSetMediaTrackInfo(child, "IP_TRACKNUMBER", nullptr);
                    PaflLog("  -> folder child %d: I_SOLO %d->2 (SIP)", cnum, *ps);
                    int sip = 2;
                    GetSetMediaTrackInfo(child, "I_SOLO", &sip);
                }

                // Create / unmute a PAFL send for any soloed leaf child
                if (ps && *ps != 0)
                {
                    int idx = FindSendToTrack(child, busTr);
                    if (idx < 0)
                    {
                        idx = CreateTrackSend(child, busTr);
                        if (idx >= 0)
                        {
                            GetSetTrackSendInfo(child, 0, idx, "I_SENDMODE", &s_sendType);
                            PaflLog("  -> folder child: created PAFL send idx=%d", idx);
                        }
                    }
                    if (idx >= 0)
                    {
                        bool no = false;
                        GetSetTrackSendInfo(child, 0, idx, "B_MUTE", &no);
                        anySendActive = true;
                    }
                }
            }
        }
        else
        {
            // Remove PAFL sends only from leaf/audio children (not sub-folder parents,
            // which we never assigned sends to).
            if (fdv <= 0)
            {
                int idx = FindSendToTrack(child, busTr);
                if (idx >= 0)
                {
                    RemoveTrackSend(child, 0, idx);
                    PaflLog("  -> folder child: removed PAFL send");
                }
            }
        }

        depth += fdv;
    }

    // Mute program feed only if we actually created PAFL sends for children.
    // If no children had I_SOLO set yet (common: REAPER sets I_SOLO per-track
    // just before each callback fires), the child will handle its own program
    // feed muting via the regular SetSurfaceSolo path.
    if (solo && anySendActive)
    {
        if (srcTr)
        {
            int si = FindSendToTrack(srcTr, busTr);
            PaflLog("  -> folder solo: muting program feed (srcSendIdx=%d)", si);
            if (si >= 0) { bool yes = true; GetSetTrackSendInfo(srcTr, 0, si, "B_MUTE", &yes); }
        }
    }
    else if (solo && !anySendActive)
    {
        PaflLog("  -> folder solo: no child sends created (children not yet I_SOLO-set), skipping program feed mute");
    }
    else
    {
        bool anyActive = false;
        for (int i = 0; i < n && !anyActive; i++)
        {
            MediaTrack* t = GetTrack(nullptr, i);
            if (!t || t == busTr || t == srcTr) continue;
            int si = FindSendToTrack(t, busTr);
            if (si < 0) continue;
            bool* pm = (bool*)GetSetTrackSendInfo(t, 0, si, "B_MUTE", nullptr);
            if (pm && !*pm) anyActive = true;
        }
        PaflLog("  -> folder unsolo: anyActive=%d", anyActive ? 1 : 0);
        if (!anyActive && srcTr)
        {
            int si = FindSendToTrack(srcTr, busTr);
            if (si >= 0) { bool no = false; GetSetTrackSendInfo(srcTr, 0, si, "B_MUTE", &no); }
            // Clear master I_SOLO residue
            MediaTrack* reaperMaster = GetMasterTrack(nullptr);
            if (reaperMaster)
            {
                int* ms = (int*)GetSetMediaTrackInfo(reaperMaster, "I_SOLO", nullptr);
                if (ms && *ms != 0) { int zero = 0; GetSetMediaTrackInfo(reaperMaster, "I_SOLO", &zero); }
            }
        }
    }
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

    // Run() is required by IReaperControlSurface but we have nothing to poll.
    // I_SOLO=6 (bit4 = propagated-from-child) on folder parents is REAPER-managed
    // informational state with no audio routing effect; REAPER refuses external writes
    // of 0 to such tracks while a child is SIP-soloed, so polling is both harmless
    // and pointless.
    void Run() override {}

    void SetSurfaceSolo(MediaTrack* tr, bool solo) override
    {
        if (s_inCallback || !s_intercept) return;

        // Identify the track for logging
        char trName[128] = {};
        if (tr) GetSetMediaTrackInfo_String(tr, "P_NAME", trName, false);
        if (!trName[0]) snprintf(trName, sizeof(trName), "(unnamed)");
        int  trNum  = tr ? (int)(intptr_t)GetSetMediaTrackInfo(tr, "IP_TRACKNUMBER", nullptr) : -1;
        int* pSoloVal = tr ? (int*)GetSetMediaTrackInfo(tr, "I_SOLO", nullptr) : nullptr;
        int  soloVal  = pSoloVal ? *pSoloVal : -1;
        int* pFD = tr ? (int*)GetSetMediaTrackInfo(tr, "I_FOLDERDEPTH", nullptr) : nullptr;
        int  fd  = pFD ? *pFD : 0;

        const char* soloMode = (soloVal == 0) ? "off"
                             : (soloVal == 1) ? "solo-mute-others"
                             : (soloVal == 2) ? "solo-in-place"
                             : (soloVal == 4) ? "safe-solo?"
                             : "unknown";
        PaflLog("SetSurfaceSolo  track=%d '%s'  solo=%d  I_SOLO=%d(%s)  I_FOLDERDEPTH=%d",
                trNum, trName, solo ? 1 : 0, soloVal, soloMode, fd);

        MediaTrack* busTr = GetBusTrack();
        if (!busTr || !tr || tr == busTr) return;

        // Don't intercept the program source track itself
        MediaTrack* srcTr = GetSrcTrack();
        if (tr == srcTr) { PaflLog("  -> skipped: is srcTr"); return; }

        // Folder parent: REAPER fires SetSurfaceSolo for folder parents when a child
        // is soloed, propagating I_SOLO=6 (bit4=from-child) up the hierarchy.
        // Diagnostic testing confirms I_SOLO=6 has zero audio routing effect and REAPER
        // refuses external writes that would clear it while a child is SIP-soloed.
        // Just skip — the leaf child fires its own callback and handles PAFL logic.
        if (fd > 0)
        {
            PaflLog("  -> folder parent: skipping (I_SOLO=%d is REAPER-managed)", soloVal);
            return;
        }

        // Skip REAPER's master bus. REAPER fires SetSurfaceSolo on the master as a
        // side-effect whenever any track is soloed (master I_SOLO reflects the global
        // solo state). If the user's program source is a regular track (not the REAPER
        // master bus), the master passes all earlier checks and gets a spurious PAFL
        // send + I_SOLO=2 that re-triggers this callback in a loop after unsolo.
        if (tr == GetMasterTrack(nullptr)) { PaflLog("  -> skipped: REAPER master bus"); return; }

        // Log parent track I_SOLO state at this moment so we can see what REAPER
        // has propagated upward before we do anything.
        if (s_debugLog)
        {
            PaflLog("  -> parent-walk: trNum=%d, starting at idx=%d", trNum, trNum - 2);
            for (int pi = trNum - 2; pi >= 0 && pi >= trNum - 5; pi--)
            {
                MediaTrack* pt = GetTrack(nullptr, pi);
                if (!pt) { PaflLog("  -> parent-walk idx=%d: null track", pi); break; }
                int* ppfd = (int*)GetSetMediaTrackInfo(pt, "I_FOLDERDEPTH", nullptr);
                int  fdv  = ppfd ? *ppfd : -99;
                int* pps  = (int*)GetSetMediaTrackInfo(pt, "I_SOLO", nullptr);
                int  ppsv = pps ? *pps : -1;
                const char* pm = (ppsv==0)?"off":(ppsv==1)?"solo-mute":(ppsv==2)?"SIP":(ppsv==4)?"safe?":"?";
                PaflLog("  -> parent-walk idx=%d track=%d  fd=%d  I_SOLO=%d(%s)", pi, pi+1, fdv, ppsv, pm);
                if (!ppfd || *ppfd <= 0) break;
            }
        }

        s_inCallback = true;

        if (solo)
        {
            // Track was soloed: add a PAFL send if not already present.
            // If an old muted send exists (from a previous architecture), unmute it.
            int idx = FindSendToTrack(tr, busTr);
            PaflLog("  -> SOLO ON  existingSendIdx=%d", idx);
            if (idx < 0)
            {
                idx = CreateTrackSend(tr, busTr);
                if (idx >= 0)
                    GetSetTrackSendInfo(tr, 0, idx, "I_SENDMODE", &s_sendType);
                PaflLog("  -> created send idx=%d sendType=%d", idx, s_sendType);
                s_activePaflTracks.insert(tr); // track that WE activated PAFL on
            }
            if (idx >= 0)
            {
                bool no = false;
                GetSetTrackSendInfo(tr, 0, idx, "B_MUTE", &no);
                PaflLog("  -> unmuted track->bus send");
            }

            // Mute the program source so only the soloed channel feeds the bus
            if (srcTr)
            {
                int si = FindSendToTrack(srcTr, busTr);
                PaflLog("  -> srcTr->bus sendIdx=%d", si);
                if (si >= 0) { bool yes = true; GetSetTrackSendInfo(srcTr, 0, si, "B_MUTE", &yes); PaflLog("  -> muted program feed"); }
                else         { PaflLog("  -> WARNING: no srcTr->bus send found, program feed NOT muted"); }
            }
            else { PaflLog("  -> no srcTr configured"); }

            // Force solo-in-place (I_SOLO=2) so REAPER doesn't mute the main mix.
            // This call triggers another SetSurfaceSolo(tr, true) which hits the guard.
            int* ps = (int*)GetSetMediaTrackInfo(tr, "I_SOLO", nullptr);
            if (ps && *ps != 2)
            {
                PaflLog("  -> setting I_SOLO 1->2 (SIP)");
                int sip = 2;
                GetSetMediaTrackInfo(tr, "I_SOLO", &sip);
            }
            else { PaflLog("  -> I_SOLO already 2, no change"); }
        }
        else
        {
            // Track was unsoloed: remove the PAFL send
            int idx = FindSendToTrack(tr, busTr);
            PaflLog("  -> SOLO OFF  sendIdx=%d", idx);
            if (idx >= 0)
            {
                RemoveTrackSend(tr, 0, idx);
                PaflLog("  -> removed track->bus send");
            }
            s_activePaflTracks.erase(tr); // remove from our tracking set

            // Restore program feed only if no other tracks WE activated PAFL on are
            // still active.  Using s_activePaflTracks avoids false positives from
            // permanent/infrastructure sends (talkback, surface sends etc.) that have
            // unmuted sends to the bus but were not initiated by this plugin.
            bool anyActive = !s_activePaflTracks.empty();
            if (s_debugLog)
            {
                for (MediaTrack* t : s_activePaflTracks)
                {
                    char tname[128] = {};
                    GetSetMediaTrackInfo_String(t, "P_NAME", tname, false);
                    int tnum = (int)(intptr_t)GetSetMediaTrackInfo(t, "IP_TRACKNUMBER", nullptr);
                    PaflLog("  -> anyActive: track=%d '%s' still in PAFL set", tnum, tname[0] ? tname : "(unnamed)");
                }
            }
            if (false) {} // dead code placeholder

            if (!anyActive && srcTr)
            {
                int si = FindSendToTrack(srcTr, busTr);
                PaflLog("  -> anyActive=false, restoring program feed (srcSendIdx=%d)", si);
                if (si >= 0) { bool no = false; GetSetTrackSendInfo(srcTr, 0, si, "B_MUTE", &no); }
            }
            // Clear any residual I_SOLO that REAPER set on its master bus as a
            // side-effect of the original solo. If we leave it nonzero it will keep
            // re-firing SetSurfaceSolo(master, solo=1) and re-muting the program.
            if (!anyActive)
            {
                MediaTrack* reaperMaster = GetMasterTrack(nullptr);
                if (reaperMaster)
                {
                    int* ms = (int*)GetSetMediaTrackInfo(reaperMaster, "I_SOLO", nullptr);
                    if (ms && *ms != 0)
                    {
                        PaflLog("  -> clearing residual master I_SOLO (was %d)", *ms);
                        int zero = 0;
                        GetSetMediaTrackInfo(reaperMaster, "I_SOLO", &zero);
                    }
                }
            }
            else if (anyActive) { PaflLog("  -> anyActive=true, program feed stays muted"); }
            else                { PaflLog("  -> anyActive=false but no srcTr, nothing to restore"); }
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
