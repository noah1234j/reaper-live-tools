// ---------------------------------------------------------------------------
// PaflWnd.cpp  –  PAFL (Pre/After Fader Listen) monitor window
//
// Architecture: poll-based, REAPER solo bus backed.
//
// When active, this plugin:
//   1. Enables REAPER's native solo bus (soloip bit 16) via get_config_var so
//      REAPER handles all hardware output routing natively.
//   2. Polls I_SOLO on all tracks in Run() (~30fps) and maintains a matching
//      software send from each soloed track to the user-selected PAFL bus track.
//
// No SetSurfaceSolo intercept, no I_SOLO manipulation, no B_SOLO_DEFEAT.
// REAPER's audio engine does the heavy lifting; we just tap it with sends.
// ---------------------------------------------------------------------------

#include "PaflWnd.h"
#include "api.h"
#include "resource.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <set>

// ---------------------------------------------------------------------------
// Persistence constants
// ---------------------------------------------------------------------------
static const char* k_busKey     = "bus";
static const char* k_appSection = "reaper_transitions_PAFL";

// ---------------------------------------------------------------------------
// Per-project GUID storage
// ---------------------------------------------------------------------------
static std::string s_busGuidStr;
static std::string s_srcGuidStr;  // program source track

// ---------------------------------------------------------------------------
// Per-machine settings
// ---------------------------------------------------------------------------
static bool s_intercept  = false;
static int  s_sendType   = 3;    // 3 = Pre-Fader (Post-FX)
static bool s_autoSetup  = false;

// ---------------------------------------------------------------------------
// Dialog handle / instance
// ---------------------------------------------------------------------------
static HINSTANCE s_hInst   = nullptr;
static HWND      s_hwnd    = nullptr;

// Countdown ticks until deferred auto-setup runs (0 = inactive)
static int s_pendingAutoSetupTicks = 0;

// soloip config var – live pointer into REAPER's preference memory.
// Writing to *s_pSoloIp takes effect immediately in the audio engine.
static int* s_pSoloIp     = nullptr;
static int  s_soloIpSaved = 0;    // value before we enabled the solo bus

// nometers config var – bitmask controlling "show metering on unsoloed tracks".
// bit 0 (1)     = show meters on unsoloed tracks in MCP
// bit 12 (4096) = show meters on unsoloed tracks in TCP
// To ENABLE showing (check the box): set both bits → value 4097
static int* s_pNoMeters     = nullptr;
static int  s_noMetersSaved = 0;

// Bits to set in soloip when PAFL is active (all 5 solo-settings checkboxes):
//   bit 0  (  1) = Solos default to in-place solo
//   bit 1  (  2) = Unsolo parent/hardware sends when SIP track sends to another soloed track
//   bit 4  ( 16) = Solo via dedicated solo bus
//   bit 5  ( 32) = Apply master fader/mute to solo bus
//   bit 6  ( 64) = Ignore solo on child tracks when parent is soloed
static const int k_soloipBits = 1 | 2 | 16 | 32 | 64; // = 115

// Tracks we currently have a PAFL send on (managed by Run())
static std::set<MediaTrack*> s_paflSends;

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
// Find a track by its GUID stored in our in-memory project strings.
// ---------------------------------------------------------------------------
static MediaTrack* FindTrackByGuid()
{
    if (s_busGuidStr.empty()) goto fallback;
    {
        GUID target = StringToGuid(s_busGuidStr.c_str());
        GUID zero   = {};
        if (IsEqualGUID(target, zero)) goto fallback;
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
                GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
                if (pg) s_busGuidStr = GuidToString(*pg);
                return tr;
            }
        }
    }
    return nullptr;
}

static void StoreTrackByKey(MediaTrack* tr)
{
    if (!tr) { s_busGuidStr.clear(); MarkProjectDirty(nullptr); return; }
    GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
    s_busGuidStr = pg ? GuidToString(*pg) : "";
    MarkProjectDirty(nullptr);
}

static MediaTrack* GetBusTrack() { return FindTrackByGuid(); }

static void StoreTrackSrc(MediaTrack* tr)
{
    if (!tr) { s_srcGuidStr.clear(); MarkProjectDirty(nullptr); return; }
    GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
    s_srcGuidStr = pg ? GuidToString(*pg) : "";
    MarkProjectDirty(nullptr);
}

static MediaTrack* GetSrcTrack()
{
    if (s_srcGuidStr.empty()) return nullptr;
    GUID target = StringToGuid(s_srcGuidStr.c_str());
    GUID zero = {};
    if (IsEqualGUID(target, zero)) return nullptr;
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

// ---------------------------------------------------------------------------
// Status label
// ---------------------------------------------------------------------------
static void UpdateStatus()
{
    if (!s_hwnd) return;
    MediaTrack* busTr = GetBusTrack();
    if (!busTr)
    {
        SetDlgItemTextA(s_hwnd, IDC_PAFL_STATUS, "No PAFL bus - select a track or click New.");
        return;
    }
    if (!s_intercept)
    {
        SetDlgItemTextA(s_hwnd, IDC_PAFL_STATUS, "PAFL inactive.");
        return;
    }
    if (s_paflSends.empty())
    {
        SetDlgItemTextA(s_hwnd, IDC_PAFL_STATUS, "Active - no tracks soloed.");
        return;
    }
    std::string active;
    for (MediaTrack* tr : s_paflSends)
    {
        char name[128] = {};
        GetSetMediaTrackInfo_String(tr, "P_NAME", name, false);
        if (!name[0])
        {
            int* pid = (int*)GetSetMediaTrackInfo(tr, "IP_TRACKNUMBER", nullptr);
            snprintf(name, sizeof(name), "Track %d", pid ? *pid : 0);
        }
        if (!active.empty()) active += ", ";
        active += name;
    }
    char status[256] = {};
    snprintf(status, sizeof(status), "PAFL: %s", active.c_str());
    SetDlgItemTextA(s_hwnd, IDC_PAFL_STATUS, status);
}

// ---------------------------------------------------------------------------
// soloip helpers
// ---------------------------------------------------------------------------
static void EnableSoloBus()
{
    if (s_pSoloIp)
    {
        s_soloIpSaved = *s_pSoloIp;
        *s_pSoloIp = s_soloIpSaved | k_soloipBits;
    }
    if (s_pNoMeters)
    {
        s_noMetersSaved = *s_pNoMeters;
        *s_pNoMeters = s_noMetersSaved | (1 | 4096); // check "Show metering on unsoloed tracks" (TCP + MCP)
    }
}

static void RestoreSoloBus()
{
    if (s_pSoloIp)   *s_pSoloIp   = s_soloIpSaved;
    if (s_pNoMeters) *s_pNoMeters = s_noMetersSaved;
}

// ---------------------------------------------------------------------------
// Remove all our PAFL sends and clear the tracking set.
// Restores the program send (unmutes it) if one is configured.
// ---------------------------------------------------------------------------
static void DoClearSends()
{
    MediaTrack* busTr = GetBusTrack();
    for (MediaTrack* tr : s_paflSends)
    {
        if (!busTr) break;
        int idx = FindSendToTrack(tr, busTr);
        if (idx >= 0) RemoveTrackSend(tr, 0, idx);
    }
    s_paflSends.clear();
    // Restore program feed
    MediaTrack* srcTr = GetSrcTrack();
    if (srcTr && busTr)
    {
        int si = FindSendToTrack(srcTr, busTr);
        if (si >= 0) { bool no = false; GetSetTrackSendInfo(srcTr, 0, si, "B_MUTE", &no); }
    }
}

// ---------------------------------------------------------------------------
// Settings load / save (reaper-extstate.ini, per machine)
// ---------------------------------------------------------------------------
static void LoadSettings()
{
    const char* v;
    v = GetExtState(k_appSection, "sendtype");
    s_sendType  = (v && v[0]) ? atoi(v) : 3;
    v = GetExtState(k_appSection, "autosetup");
    s_autoSetup = (v && atoi(v) != 0);
}

static void SaveSettings()
{
    char buf[16];
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
    int ni = (int)SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)"<none>");
    SendMessageA(hCombo, CB_SETITEMDATA, ni, (LPARAM)-1);
    MediaTrack* busTr = GetBusTrack();
    MediaTrack* srcTr = GetSrcTrack();
    int selIdx = 0;
    const int n = GetNumTracks();
    for (int i = 0; i < n; i++)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr || tr == busTr) continue;
        char name[128] = {}, label[160] = {};
        GetSetMediaTrackInfo_String(tr, "P_NAME", name, false);
        if (name[0]) snprintf(label, sizeof(label), "%d: %s", i + 1, name);
        else         snprintf(label, sizeof(label), "Track %d", i + 1);
        int cbIdx = (int)SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)label);
        SendMessageA(hCombo, CB_SETITEMDATA, cbIdx, (LPARAM)i);
        if (srcTr == tr) selIdx = cbIdx;
    }
    SendMessageA(hCombo, CB_SETCURSEL, selIdx, 0);
}

static MediaTrack* GetSrcTrackFromCombo(HWND hwnd)
{
    int sel   = (int)SendDlgItemMessageA(hwnd, IDC_PAFL_SRCTRACK, CB_GETCURSEL, 0, 0);
    INT_PTR d = (INT_PTR)SendDlgItemMessageA(hwnd, IDC_PAFL_SRCTRACK, CB_GETITEMDATA, sel, 0);
    if (d == -1) return nullptr;
    return GetTrack(nullptr, (int)d);
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
    switch (idx) { case 1: return 3; case 2: return 1; default: return 0; }
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
        if (name[0]) snprintf(label, sizeof(label), "%d: %s", i + 1, name);
        else         snprintf(label, sizeof(label), "Track %d", i + 1);
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
    StoreTrackByKey(busTr);

    // Wire up the program source send if one is already selected
    MediaTrack* srcTr = hwnd ? GetSrcTrackFromCombo(hwnd) : GetSrcTrack();
    if (srcTr && srcTr != busTr)
    {
        if (FindSendToTrack(srcTr, busTr) < 0)
        {
            int idx = CreateTrackSend(srcTr, busTr);
            if (idx >= 0) { int mode = 0; GetSetTrackSendInfo(srcTr, 0, idx, "I_SENDMODE", &mode); }
        }
        StoreTrackSrc(srcTr);
    }

    if (hwnd) { FillBusTrackCombo(hwnd); FillSrcTrackCombo(hwnd); }
    UpdateStatus();
    UpdateTimeline();
    Undo_OnStateChangeEx("PAFL: Create bus track", UNDO_STATE_ALL, -1);
}

// ---------------------------------------------------------------------------
// Timer tick (called ~30fps from REAPER main thread)
// Drives deferred auto-setup.
// ---------------------------------------------------------------------------
void PaflWnd_TimerTick()
{
    if (s_pendingAutoSetupTicks > 0)
    {
        if (--s_pendingAutoSetupTicks == 0)
        {
            s_intercept = true;
            EnableSoloBus();
            SaveSettings();
            if (s_hwnd && IsWindow(s_hwnd))
                CheckDlgButton(s_hwnd, IDC_PAFL_ACTIVE, BST_CHECKED);
            if (!GetBusTrack()) DoCreateNewBus(nullptr);
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
                DoClearSends();
                MediaTrack* newBus = GetBusTrackFromCombo(hwnd);
                StoreTrackByKey(newBus);
                FillSrcTrackCombo(hwnd); // refresh (excludes new bus track)
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
                MediaTrack* newSrc = GetSrcTrackFromCombo(hwnd);
                MediaTrack* busTr  = GetBusTrack();
                // Remove old program send
                MediaTrack* oldSrc = GetSrcTrack();
                if (oldSrc && busTr)
                {
                    int si = FindSendToTrack(oldSrc, busTr);
                    if (si >= 0) RemoveTrackSend(oldSrc, 0, si);
                }
                StoreTrackSrc(newSrc);
                // Create new program send (unmuted, post-fader)
                if (newSrc && busTr && newSrc != busTr)
                {
                    int idx = CreateTrackSend(newSrc, busTr);
                    if (idx >= 0) { int mode = 0; GetSetTrackSendInfo(newSrc, 0, idx, "I_SENDMODE", &mode); }
                }
                UpdateStatus();
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
                EnableSoloBus();
                if (!GetBusTrack()) DoCreateNewBus(hwnd);
                else UpdateStatus();
            }
            else
            {
                DoClearSends();
                RestoreSoloBus();
                UpdateStatus();
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
// Control surface – Run() polls I_SOLO and maintains PAFL sends.
// REAPER's native solo bus (enabled in soloip) handles all hardware routing.
// We just add software sends to the PAFL bus to mirror what's soloed.
// ---------------------------------------------------------------------------
class PaflMonitor : public IReaperControlSurface
{
public:
    const char* GetTypeString() override { return "PAFLTRANSITIONS"; }
    const char* GetDescString() override { return "Transition Snapshots PAFL"; }
    const char* GetConfigString() override { return ""; }

    void Run() override
    {
        if (!s_intercept) return;
        MediaTrack* busTr = GetBusTrack();
        if (!busTr) return;

        const int n = GetNumTracks();
        std::set<MediaTrack*> nowSoloed;
        MediaTrack* srcTr = GetSrcTrack();

        for (int i = 0; i < n; i++)
        {
            MediaTrack* tr = GetTrack(nullptr, i);
            if (!tr || tr == busTr || tr == srcTr) continue; // never PAFL the bus or program source
            int* ps = (int*)GetSetMediaTrackInfo(tr, "I_SOLO", nullptr);
            if (ps && *ps != 0)
                nowSoloed.insert(tr);
        }

        // Add sends for newly soloed tracks
        bool changed = false;
        for (MediaTrack* tr : nowSoloed)
        {
            if (s_paflSends.count(tr)) continue;
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
                s_paflSends.insert(tr);
                changed = true;
            }
        }

        // Remove sends for tracks no longer soloed
        for (auto it = s_paflSends.begin(); it != s_paflSends.end(); )
        {
            if (!nowSoloed.count(*it))
            {
                int idx = FindSendToTrack(*it, busTr);
                if (idx >= 0) RemoveTrackSend(*it, 0, idx);
                it = s_paflSends.erase(it);
                changed = true;
            }
            else { ++it; }
        }

        // Program feed: unmuted when nothing is soloed, muted when anything is
        if (srcTr && srcTr != busTr)
        {
            int si = FindSendToTrack(srcTr, busTr);
            if (si >= 0)
            {
                bool wantMute = !s_paflSends.empty();
                bool* pm = (bool*)GetSetTrackSendInfo(srcTr, 0, si, "B_MUTE", nullptr);
                if (pm && *pm != wantMute)
                {
                    GetSetTrackSendInfo(srcTr, 0, si, "B_MUTE", &wantMute);
                    changed = true;
                }
            }
        }

        if (changed) UpdateStatus();
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

    // Grab live config var pointers once at startup
    int sz = 0;
    s_pSoloIp = (int*)get_config_var("soloip", &sz);
    if (sz != sizeof(int)) s_pSoloIp = nullptr;
    sz = 0;
    s_pNoMeters = (int*)get_config_var("nometers", &sz);
    if (sz != 4 && sz != 8) s_pNoMeters = nullptr; // accept 4-byte int or 8-byte value (little-endian safe)

    // s_intercept is never persisted; user must explicitly activate each session
    // (or enable "Active on project startup" to auto-activate with projects).

    plugin_register("timer",      (void*)PaflWnd_TimerTick);
    plugin_register("csurf_inst", &s_paflMonitor);
}

void PaflWnd_Cleanup()
{
    plugin_register("-csurf_inst", &s_paflMonitor);
    plugin_register("-timer",      (void*)PaflWnd_TimerTick);
    DoClearSends();
    RestoreSoloBus();
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
        s_hwnd = CreateDialogParam(s_hInst,
                                   MAKEINTRESOURCE(IDD_PAFL),
                                   GetMainHwnd(),
                                   PaflDlgProc, 0);
        if (s_hwnd) ShowWindow(s_hwnd, SW_SHOW);
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
    if (s_autoSetup)
        s_pendingAutoSetupTicks = 15;
}

void PaflWnd_ResetProjectState()
{
    // Deactivate PAFL when switching projects; autoSetup will re-enable if configured.
    if (s_intercept)
    {
        RestoreSoloBus();
        s_intercept = false;
        if (s_hwnd && IsWindow(s_hwnd))
            CheckDlgButton(s_hwnd, IDC_PAFL_ACTIVE, BST_UNCHECKED);
    }
    s_busGuidStr.clear();
    s_srcGuidStr.clear();
    s_pendingAutoSetupTicks = 0;
    s_paflSends.clear(); // pointers are stale after project change
}

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

void PaflWnd_SaveConfig(ProjectStateContext* ctx)
{
    if (!ctx) return;
    char line[160];
    if (!s_busGuidStr.empty())
    {
        snprintf(line, sizeof(line), "LTPAFLBUS %s", s_busGuidStr.c_str());
        ctx->AddLine(line);
    }
    if (!s_srcGuidStr.empty())
    {
        snprintf(line, sizeof(line), "LTPAFLSRC %s", s_srcGuidStr.c_str());
        ctx->AddLine(line);
    }
}
