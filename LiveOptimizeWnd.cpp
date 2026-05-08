// ---------------------------------------------------------------------------
// LiveOptimizeWnd.cpp  –  Live Optimizer modeless dialog
//
// Scans REAPER settings + Windows system configuration for factors that
// affect live-sound performance.  Displays results in a categorized
// ListView with color-coded status, per-item tooltips, Apply Fix buttons,
// and a 0-100 Live Score bar at the top.
//
// Pattern: modeless dialog created once, shown/hidden on demand.
//          WM_CLOSE hides (not destroys).
// ---------------------------------------------------------------------------
#include "LiveOptimizeWnd.h"
#include "api.h"
#include "resource.h"

#include <commctrl.h>
#include <windowsx.h>
#include <powrprof.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <algorithm>
using std::min;
using std::max;
#include <vector>
#include <functional>
#include <algorithm>

#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Advapi32.lib")

// ---------------------------------------------------------------------------
// Helpers – forward declarations
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK LiveOptDlgProc(HWND, UINT, WPARAM, LPARAM);
static void RunChecks();
static void PopulateList(HWND hwnd);
static bool IsRunningAsAdmin();

// ---------------------------------------------------------------------------
// Check status constants
// ---------------------------------------------------------------------------
enum CheckStatus {
    STATUS_GOOD    = 0,   // green
    STATUS_WARN    = 1,   // yellow
    STATUS_BAD     = 2,   // red
    STATUS_INFO    = 3,   // grey / informational
};

// ---------------------------------------------------------------------------
// Single check entry
// ---------------------------------------------------------------------------
struct LiveOptCheck {
    const char*           category;    // "Audio Device", "REAPER Prefs", "System Health", "Project State"
    const char*           name;        // short display name
    const char*           tooltip;     // full explanation shown in info panel
    float                 weight;      // max points this check contributes
    int                   status;      // STATUS_GOOD / WARN / BAD / INFO
    float                 score;       // points awarded (0..weight)
    std::string           value;       // current value string for display
    std::string           recommend;   // short recommendation string
    bool                  canFix;      // whether Apply Fix is available
    std::function<void()> fixFunc;     // called when user clicks Apply Fix
};

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static HINSTANCE g_hInst    = nullptr;
static HWND      g_wnd      = nullptr;
static HWND      g_hList    = nullptr;
static float     g_totalScore = 0.f;
static int       g_selectedCheck = -1;

static std::vector<LiveOptCheck> g_checks;

// ListView timer ID
static const UINT LO_TIMER_ID = 42;

// Column indices
enum LO_Col {
    COL_STATUS   = 0,  // colored dot
    COL_CATEGORY = 1,
    COL_NAME     = 2,
    COL_VALUE    = 3,
    COL_RECOMMEND= 4,
    COL_COUNT    = 5
};

// ---------------------------------------------------------------------------
// Utility: read REAPER .ini string (fallback when get_config_var_string
// returns empty)
// ---------------------------------------------------------------------------
static int GetIniInt(const char* section, const char* key, int def)
{
    if (!get_ini_file) return def;
    const char* iniPath = get_ini_file();
    if (!iniPath || !*iniPath) return def;
    return (int)GetPrivateProfileIntA(section, key, def, iniPath);
}

static std::string GetIniStr(const char* section, const char* key,
                              const char* def = "")
{
    if (!get_ini_file) return def;
    const char* iniPath = get_ini_file();
    if (!iniPath || !*iniPath) return def;
    char buf[256] = {};
    GetPrivateProfileStringA(section, key, def, buf, (DWORD)sizeof(buf), iniPath);
    return buf;
}

// ---------------------------------------------------------------------------
// Admin check
// ---------------------------------------------------------------------------
static bool IsRunningAsAdmin()
{
    BOOL isAdmin = FALSE;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    PSID adminSid = nullptr;
    if (AllocateAndInitializeSid(&NtAuthority, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &adminSid))
    {
        CheckTokenMembership(nullptr, adminSid, &isAdmin);
        FreeSid(adminSid);
    }
    return isAdmin == TRUE;
}

// ---------------------------------------------------------------------------
// Power-plan helpers
// ---------------------------------------------------------------------------

// Well-known power plan GUIDs
static const GUID k_guidPlanHighPerf =
    { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };
static const GUID k_guidPlanUltimate =
    { 0xe9a42b02, 0xd5df, 0x448d, { 0xaa, 0x00, 0x03, 0xf1, 0x4d, 0xde, 0xa1, 0x16 } };
static const GUID k_guidPlanBalanced =
    { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };

// GUID_SLEEP_SUBGROUP / USB_SUBGROUP / PCIEXPRESS_SUBGROUP / PROCESSOR_SUBGROUP
static const GUID k_guidUSBSubgroup =
    { 0x2a737441, 0x1930, 0x4402, { 0x8d, 0x77, 0xb2, 0xbe, 0xbf, 0xa4, 0x1c, 0x09 } };
static const GUID k_guidUSBSelectiveSuspend =
    { 0x48e6b7a6, 0x50f5, 0x4782, { 0xa5, 0xd4, 0x53, 0xbb, 0x8f, 0x07, 0xe2, 0x26 } };
static const GUID k_guidPCIeSubgroup =
    { 0x501a4d13, 0x42af, 0x4429, { 0x9f, 0xd1, 0xa8, 0x21, 0x8c, 0x26, 0x8e, 0x20 } };
static const GUID k_guidPCIeASPM =
    { 0xee12f906, 0xd277, 0x404b, { 0xb6, 0xda, 0xe5, 0xfa, 0x1a, 0x57, 0x6d, 0xf5 } };
static const GUID k_guidProcessorSubgroup =
    { 0x54533251, 0x82be, 0x4824, { 0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00 } };
static const GUID k_guidProcessorThrottleMax =
    { 0xbc5038f7, 0x23e0, 0x4960, { 0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec } };
static const GUID k_guidProcessorThrottleMin =
    { 0x893dee8e, 0x2bef, 0x41e0, { 0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c } };

// Returns the active power scheme GUID (caller frees with LocalFree)
static GUID* GetActivePowerScheme()
{
    GUID* guid = nullptr;
    PowerGetActiveScheme(nullptr, &guid);
    return guid;
}

static DWORD ReadPowerACValue(GUID* planGuid, const GUID* subgroup, const GUID* setting)
{
    DWORD val   = 0;
    DWORD cbVal = sizeof(DWORD);
    PowerReadACValue(nullptr, planGuid, subgroup, setting, nullptr,
                     reinterpret_cast<LPBYTE>(&val), &cbVal);
    return val;
}

// ---------------------------------------------------------------------------
// Background process blacklist
// ---------------------------------------------------------------------------
static const char* k_badProcesses[] = {
    // Browsers
    "chrome.exe", "firefox.exe", "msedge.exe", "opera.exe", "brave.exe",
    // Video streaming / conferencing
    "obs64.exe", "obs32.exe", "obs.exe",
    "zoom.exe", "teams.exe", "slack.exe", "discord.exe",
    "skype.exe", "webex.exe",
    // Gaming overlays / launchers
    "steam.exe", "epicgameslauncher.exe", "gog galaxy.exe",
    "origin.exe", "battlenet.exe", "upc.exe",
    "nvcontainer.exe", "nvcplui.exe",
    // Antivirus / scanners (known to cause DPC spikes)
    "mbamservice.exe", "avgservice.exe", "avastservice.exe",
    "mcshield.exe", "msseces.exe",
    // Windows Update / delivery optimisation
    "tiworker.exe", "wuauclt.exe", "dosvc.exe",
    // Indexing
    "searchindexer.exe",
    // Cloud sync
    "onedrive.exe", "dropbox.exe", "googledrivefs.exe", "box.exe",
    // Media players that may grab ASIO
    "spotify.exe", "vlc.exe", "wmplayer.exe", "itunes.exe",
    nullptr
};

// Returns sorted list of found bad processes
static std::vector<std::string> FindBadProcesses()
{
    std::vector<std::string> found;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return found;

    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32First(hSnap, &pe))
    {
        do {
            // Lower-case the exe name for comparison
            char lower[MAX_PATH] = {};
            for (int i = 0; pe.szExeFile[i] && i < MAX_PATH - 1; ++i)
                lower[i] = (char)tolower((unsigned char)pe.szExeFile[i]);

            for (int k = 0; k_badProcesses[k]; ++k)
            {
                if (strcmp(lower, k_badProcesses[k]) == 0)
                {
                    // avoid duplicates
                    if (std::find(found.begin(), found.end(), std::string(lower)) == found.end())
                        found.push_back(lower);
                    break;
                }
            }
        } while (Process32Next(hSnap, &pe));
    }
    CloseHandle(hSnap);
    std::sort(found.begin(), found.end());
    return found;
}

// ---------------------------------------------------------------------------
// REAPER config-var helper (tries get_config_var_string, falls back to INI)
// ---------------------------------------------------------------------------
static std::string GetConfigVar(const char* varName,
                                const char* iniSection,
                                const char* iniKey,
                                const char* def = "")
{
    if (get_config_var_string)
    {
        char buf[64] = {};
        if (get_config_var_string(varName, buf, (int)sizeof(buf)) && buf[0])
            return buf;
    }
    return GetIniStr(iniSection, iniKey, def);
}

// ---------------------------------------------------------------------------
// RunChecks – populates g_checks and g_totalScore
// ---------------------------------------------------------------------------
static void RunChecks()
{
    g_checks.clear();

    // -----------------------------------------------------------------------
    // Helper lambda to add a check
    // -----------------------------------------------------------------------
    auto Add = [&](const char* cat, const char* name, const char* tip,
                   float weight, int status, float score,
                   std::string val, std::string rec,
                   bool canFix, std::function<void()> fix)
    {
        LiveOptCheck c;
        c.category  = cat;
        c.name      = name;
        c.tooltip   = tip;
        c.weight    = weight;
        c.status    = status;
        c.score     = score;
        c.value     = std::move(val);
        c.recommend = std::move(rec);
        c.canFix    = canFix;
        c.fixFunc   = std::move(fix);
        g_checks.push_back(std::move(c));
    };

    // ===================================================================
    // CATEGORY 1 – Audio Device  (30 pts)
    // ===================================================================

    // --- Check 1: Driver mode (12 pts) ---
    {
        char modeStr[64] = {};
        if (GetAudioDeviceInfo) GetAudioDeviceInfo("MODE", modeStr, (int)sizeof(modeStr));
        int   mode   = atoi(modeStr);
        // Also read from INI as fallback
        if (!modeStr[0]) mode = GetIniInt("audioconfig", "mode", 0);

        const char* modeName = "Unknown";
        int status = STATUS_BAD;
        float pts  = 0.f;
        if      (mode == 3) { modeName = "ASIO";        status = STATUS_GOOD; pts = 12.f; }
        else if (mode == 5) { modeName = "WASAPI";       status = STATUS_WARN; pts =  6.f; }
        else if (mode == 4) { modeName = "KS";           status = STATUS_WARN; pts =  6.f; }
        else if (mode == 2) { modeName = "DirectSound";  status = STATUS_BAD;  pts =  0.f; }
        else if (mode == 1) { modeName = "WaveOut";      status = STATUS_BAD;  pts =  0.f; }

        Add("Audio Device", "Driver Mode",
            "ASIO drivers bypass Windows audio stack, giving lowest latency and most stable "
            "performance for live sound. WASAPI Exclusive is second-best. WaveOut and "
            "DirectSound add significant latency and jitter.",
            12.f, status, pts, modeName,
            (mode == 3) ? "ASIO is optimal" : "Switch to ASIO for best performance",
            false, nullptr);
    }

    // --- Check 2: Buffer size (10 pts) ---
    {
        char bsStr[64] = {};
        if (GetAudioDeviceInfo) GetAudioDeviceInfo("BSIZE", bsStr, (int)sizeof(bsStr));
        int bs = atoi(bsStr);
        if (!bs) bs = GetIniInt("audioconfig", "asio_bsize", 0);

        int status = STATUS_INFO;
        float pts  = 0.f;
        std::string rec;
        if      (bs > 0 && bs <= 256) { status = STATUS_GOOD; pts = 10.f; rec = "Optimal for live"; }
        else if (bs > 256 && bs <= 512){ status = STATUS_WARN; pts =  5.f; rec = "Reduce to ≤256 if possible"; }
        else if (bs > 512)             { status = STATUS_BAD;  pts =  0.f; rec = "Reduce buffer size"; }
        else                           { status = STATUS_INFO; pts =  5.f; rec = "Could not read buffer size"; }

        char valStr[32] = {};
        if (bs > 0) snprintf(valStr, sizeof(valStr), "%d samples", bs);
        else        snprintf(valStr, sizeof(valStr), "Unknown");

        Add("Audio Device", "Buffer Size",
            "Buffer size (block size) directly determines output latency.  For live "
            "monitoring, 64–256 samples is ideal.  Large buffers (>512) add noticeable "
            "latency to stage monitoring feeds.",
            10.f, status, pts, valStr, rec, false, nullptr);
    }

    // --- Check 3: Sample rate (3 pts) ---
    {
        char srStr[64] = {};
        if (GetAudioDeviceInfo) GetAudioDeviceInfo("SRATE", srStr, (int)sizeof(srStr));
        int sr = atoi(srStr);
        if (!sr) sr = GetIniInt("audioconfig", "srate", 0);

        int status = (sr == 44100 || sr == 48000) ? STATUS_GOOD : STATUS_WARN;
        float pts  = (sr == 44100 || sr == 48000) ? 3.f : 0.f;
        char valStr[32] = {};
        snprintf(valStr, sizeof(valStr), sr ? "%d Hz" : "Unknown", sr);

        Add("Audio Device", "Sample Rate",
            "44100 Hz and 48000 Hz are industry standard live rates.  Higher rates "
            "(88200 / 96000+) increase DSP load with little audible benefit for FOH work.",
            3.f, status, pts, valStr,
            (status == STATUS_GOOD) ? "Standard rate" : "Use 44100 or 48000 Hz",
            false, nullptr);
    }

    // --- Check 4: Audio engine running (3 pts) ---
    {
        bool running = Audio_IsRunning ? (Audio_IsRunning() != 0) : false;
        Add("Audio Device", "Audio Engine",
            "REAPER's audio engine must be running for any live monitoring.  "
            "If stopped, use Audio → Start audio engine.",
            3.f,
            running ? STATUS_GOOD : STATUS_BAD,
            running ? 3.f : 0.f,
            running ? "Running" : "Stopped",
            running ? "Engine is active" : "Start the audio engine",
            !running,
            []() { if (Audio_Init) Audio_Init(); });
    }

    // --- Check 5: WASAPI exclusive mode (2 pts, only relevant for WASAPI) ---
    {
        int driverMode = GetIniInt("audioconfig", "mode", 0);
        if (driverMode == 5) // WASAPI
        {
            int wasapiMode = GetIniInt("audioconfig", "wasapi_mode", 0);
            bool exclusive = (wasapiMode == 1);
            Add("Audio Device", "WASAPI Exclusive",
                "WASAPI Exclusive mode gives REAPER sole control of the audio device, "
                "eliminating Windows audio mixer latency.  Only applies when using WASAPI driver.",
                2.f,
                exclusive ? STATUS_GOOD : STATUS_WARN,
                exclusive ? 2.f : 0.f,
                exclusive ? "Exclusive" : "Shared",
                exclusive ? "Already exclusive" : "Enable WASAPI Exclusive in audio prefs",
                false, nullptr);
        }
    }

    // ===================================================================
    // CATEGORY 2 – REAPER Preferences  (25 pts)
    // ===================================================================

    // --- Check 6: Anticipative FX (8 pts) ---
    {
        std::string val = GetConfigVar("anticipativefx", "audio", "anticipativefx", "0");
        bool enabled = (val == "1" || val == "true");
        Add("REAPER Prefs", "Anticipative FX",
            "Anticipative FX pre-renders plugin output ahead of the playhead, reducing "
            "the chance of audio dropouts.  Should always be ON for live use.  "
            "Found in: Preferences → Audio → Buffering.",
            8.f,
            enabled ? STATUS_GOOD : STATUS_WARN,
            enabled ? 8.f : 0.f,
            enabled ? "Enabled" : "Disabled",
            enabled ? "Optimal" : "Enable in Preferences → Audio → Buffering",
            false, nullptr);
    }

    // --- Check 7: Process priority (5 pts) ---
    {
        std::string val = GetConfigVar("priorhigh", "reaper", "priorhigh", "0");
        bool high = (val == "1" || val == "2");
        Add("REAPER Prefs", "Process Priority",
            "Setting REAPER to High or Above Normal process priority ensures Windows "
            "schedules audio threads first.  Found in: Preferences → General.",
            5.f,
            high ? STATUS_GOOD : STATUS_WARN,
            high ? 5.f : 0.f,
            high ? "High" : "Normal",
            high ? "Optimal" : "Set to High in Preferences → General",
            false, nullptr);
    }

    // --- Check 8: Automation override – Bypass All (5 pts) ---
    {
        int autoMode = GetGlobalAutomationOverride ? GetGlobalAutomationOverride() : -1;
        bool bypass  = (autoMode == 5);
        Add("REAPER Prefs", "Automation Override",
            "Setting automation to 'Bypass All' during a live show prevents any "
            "automation from fighting your live fader moves.  Click Apply Fix to set "
            "Bypass All now.",
            5.f,
            bypass ? STATUS_GOOD : STATUS_WARN,
            bypass ? 5.f : 0.f,
            bypass ? "Bypass All" : (autoMode == -1 ? "Unknown" : "Active"),
            bypass ? "Good – all automation bypassed" : "Set to Bypass All for live shows",
            !bypass && SetGlobalAutomationOverride != nullptr,
            []() { if (SetGlobalAutomationOverride) SetGlobalAutomationOverride(5); });
    }

    // --- Check 9: Undo levels (4 pts) ---
    {
        int levels = GetIniInt("reaper", "undolevels", 0);
        // REAPER sometimes stores it as "maxundolevels"
        if (!levels) levels = GetIniInt("reaper", "maxundolevels", 50);
        int status;
        float pts;
        std::string rec;
        if      (levels <= 50)  { status = STATUS_GOOD; pts = 4.f; rec = "Lean undo history"; }
        else if (levels <= 200) { status = STATUS_WARN; pts = 2.f; rec = "Consider reducing to ≤50"; }
        else                    { status = STATUS_BAD;  pts = 0.f; rec = "Reduce undo levels (Prefs → General)"; }

        char valStr[32] = {};
        snprintf(valStr, sizeof(valStr), "%d levels", levels);
        Add("REAPER Prefs", "Undo Levels",
            "Large undo histories consume RAM and can cause brief pauses when REAPER "
            "writes undo states.  Keep at 50 or fewer for live performance.",
            4.f, status, pts, valStr, rec, false, nullptr);
    }

    // --- Check 10: Multiprocessor audio (3 pts) ---
    {
        std::string val = GetConfigVar("nthreads", "audio", "nthreads", "0");
        int threads = atoi(val.c_str());
        if (!threads) threads = GetIniInt("audio", "nthreads", 0);
        SYSTEM_INFO si = {};
        GetSystemInfo(&si);
        int cpus = (int)si.dwNumberOfProcessors;
        bool multi = (threads > 1 || (threads == 0 && cpus > 1));

        char valStr[64] = {};
        snprintf(valStr, sizeof(valStr), threads > 0 ? "%d threads" : "Auto (%d cores)", threads > 0 ? threads : cpus);

        Add("REAPER Prefs", "Multiprocessor Audio",
            "REAPER can spread FX processing across CPU cores.  On multi-core systems "
            "this significantly reduces the chance of dropouts under heavy FX load.  "
            "Set in: Preferences → Audio → Buffering → Render ahead / CPU threads.",
            3.f,
            multi ? STATUS_GOOD : STATUS_WARN,
            multi ? 3.f : 0.f,
            valStr,
            multi ? "Multi-core in use" : "Enable multi-core audio in preferences",
            false, nullptr);
    }

    // ===================================================================
    // CATEGORY 3 – System Health  (35 pts)
    // ===================================================================

    GUID* activePlan = GetActivePowerScheme();

    // --- Check 11: Power plan (8 pts) ---
    {
        std::string planName = "Unknown";
        int status = STATUS_BAD;
        float pts  = 0.f;

        if (activePlan)
        {
            if (IsEqualGUID(*activePlan, k_guidPlanHighPerf))
            { planName = "High Performance"; status = STATUS_GOOD; pts = 8.f; }
            else if (IsEqualGUID(*activePlan, k_guidPlanUltimate))
            { planName = "Ultimate Performance"; status = STATUS_GOOD; pts = 8.f; }
            else if (IsEqualGUID(*activePlan, k_guidPlanBalanced))
            { planName = "Balanced"; status = STATUS_WARN; pts = 2.f; }
            else
            { planName = "Custom/Power Saver"; status = STATUS_BAD; pts = 0.f; }
        }

        Add("System Health", "Power Plan",
            "High Performance or Ultimate Performance power plans prevent the CPU from "
            "throttling during audio processing, eliminating DPC latency spikes.  "
            "Balanced and Power Saver plans can cause random dropouts.",
            8.f, status, pts, planName,
            (status == STATUS_GOOD) ? "Optimal" : "Switch to High Performance",
            (status != STATUS_GOOD),
            []() {
                // Prefer Ultimate if available, else High Performance
                if (PowerSetActiveScheme(nullptr, &k_guidPlanUltimate) != ERROR_SUCCESS)
                    PowerSetActiveScheme(nullptr, &k_guidPlanHighPerf);
            });
    }

    // --- Check 12: MMCSS SystemResponsiveness (5 pts) ---
    {
        DWORD val = 20;
        DWORD cbVal = sizeof(DWORD);
        HKEY hKey = nullptr;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
            0, KEY_READ, &hKey) == ERROR_SUCCESS)
        {
            RegQueryValueExA(hKey, "SystemResponsiveness", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&val), &cbVal);
            RegCloseKey(hKey);
        }

        bool good = (val <= 10);
        char valStr[32] = {};
        snprintf(valStr, sizeof(valStr), "%lu%%", (unsigned long)val);

        Add("System Health", "MMCSS Responsiveness",
            "MMCSS SystemResponsiveness controls how much CPU time the multimedia "
            "scheduler reserves for non-audio tasks.  The default is 20; lowering to "
            "10 gives audio threads more headroom.  Requires admin rights to change.",
            5.f,
            good ? STATUS_GOOD : STATUS_WARN,
            good ? 5.f : (val <= 20 ? 2.f : 0.f),
            valStr,
            good ? "Optimal (≤10%)" : "Set to 10 (requires admin rights)",
            !good && IsRunningAsAdmin(),
            []() {
                HKEY hKey2 = nullptr;
                if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
                    0, KEY_SET_VALUE, &hKey2) == ERROR_SUCCESS)
                {
                    DWORD v = 10;
                    RegSetValueExA(hKey2, "SystemResponsiveness", 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&v), sizeof(v));
                    RegCloseKey(hKey2);
                }
            });
    }

    // --- Check 13: USB Selective Suspend (4 pts) ---
    if (activePlan)
    {
        DWORD usbSS = ReadPowerACValue(activePlan, &k_guidUSBSubgroup,
                                       &k_guidUSBSelectiveSuspend);
        bool good = (usbSS == 0);
        char valStr[32] = {};
        snprintf(valStr, sizeof(valStr), usbSS == 0 ? "Disabled" : "Enabled (val=%lu)", (unsigned long)usbSS);

        Add("System Health", "USB Selective Suspend",
            "USB Selective Suspend allows Windows to power down USB ports to save energy.  "
            "This can cause audio interfaces to drop out momentarily.  Disable it in the "
            "active power plan for reliable USB audio device connections.",
            4.f,
            good ? STATUS_GOOD : STATUS_WARN,
            good ? 4.f : 0.f,
            valStr,
            good ? "Disabled (good)" : "Disable USB Selective Suspend",
            !good,
            []() {
                GUID* plan = GetActivePowerScheme();
                if (plan)
                {
                    PowerWriteACValueIndex(nullptr, plan,
                        &k_guidUSBSubgroup, &k_guidUSBSelectiveSuspend, 0);
                    PowerSetActiveScheme(nullptr, plan);
                    LocalFree(plan);
                }
            });
    }

    // --- Check 14: PCIe Link State (4 pts) ---
    if (activePlan)
    {
        DWORD pcie = ReadPowerACValue(activePlan, &k_guidPCIeSubgroup, &k_guidPCIeASPM);
        bool good  = (pcie == 0);
        char valStr[32] = {};
        snprintf(valStr, sizeof(valStr), pcie == 0 ? "Off" : "Active (val=%lu)", (unsigned long)pcie);

        Add("System Health", "PCIe Link State (ASPM)",
            "PCIe Active State Power Management (ASPM) can cause latency spikes on PCIe "
            "audio devices and Thunderbolt controllers.  Setting to Off disables power "
            "gating on the PCIe bus in the active power plan.",
            4.f,
            good ? STATUS_GOOD : STATUS_WARN,
            good ? 4.f : 0.f,
            valStr,
            good ? "Off (good)" : "Disable PCIe ASPM",
            !good,
            []() {
                GUID* plan = GetActivePowerScheme();
                if (plan)
                {
                    PowerWriteACValueIndex(nullptr, plan,
                        &k_guidPCIeSubgroup, &k_guidPCIeASPM, 0);
                    PowerSetActiveScheme(nullptr, plan);
                    LocalFree(plan);
                }
            });
    }

    // --- Check 15: Processor max state (4 pts) ---
    if (activePlan)
    {
        DWORD maxState = ReadPowerACValue(activePlan, &k_guidProcessorSubgroup,
                                          &k_guidProcessorThrottleMax);
        DWORD minState = ReadPowerACValue(activePlan, &k_guidProcessorSubgroup,
                                          &k_guidProcessorThrottleMin);
        bool good = (maxState >= 100 && minState >= 100);
        char valStr[48] = {};
        snprintf(valStr, sizeof(valStr), "Max %lu%% / Min %lu%%",
                 (unsigned long)maxState, (unsigned long)minState);

        Add("System Health", "CPU Throttle State",
            "Windows can throttle the processor to save power.  For audio, the processor "
            "should run at 100% max and min state to prevent latency spikes when cores "
            "ramp up from a low P-state.",
            4.f,
            good ? STATUS_GOOD : STATUS_WARN,
            good ? 4.f : (maxState >= 100 ? 2.f : 0.f),
            valStr,
            good ? "CPU at max (good)" : "Set CPU max/min state to 100%",
            !good,
            []() {
                GUID* plan = GetActivePowerScheme();
                if (plan)
                {
                    PowerWriteACValueIndex(nullptr, plan,
                        &k_guidProcessorSubgroup, &k_guidProcessorThrottleMax, 100);
                    PowerWriteACValueIndex(nullptr, plan,
                        &k_guidProcessorSubgroup, &k_guidProcessorThrottleMin, 100);
                    PowerSetActiveScheme(nullptr, plan);
                    LocalFree(plan);
                }
            });
    }

    if (activePlan) LocalFree(activePlan);

    // --- Check 16: Win32PrioritySeparation (3 pts) ---
    {
        DWORD val   = 0;
        DWORD cbVal = sizeof(DWORD);
        HKEY  hKey  = nullptr;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\PriorityControl",
            0, KEY_READ, &hKey) == ERROR_SUCCESS)
        {
            RegQueryValueExA(hKey, "Win32PrioritySeparation", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&val), &cbVal);
            RegCloseKey(hKey);
        }
        bool good = (val == 0x26 || val == 38); // 0x26 = short, fixed, foreground boost off
        char valStr[32] = {};
        snprintf(valStr, sizeof(valStr), "0x%02lX (%lu)", (unsigned long)val, (unsigned long)val);

        Add("System Health", "Win32PrioritySeparation",
            "Win32PrioritySeparation=0x26 (38) configures Windows for equal foreground / "
            "background scheduling, which benefits audio threads.  The default value (0x02) "
            "gives large boosts to foreground windows, sometimes starving audio threads.  "
            "Requires admin rights to change.",
            3.f,
            good ? STATUS_GOOD : STATUS_WARN,
            good ? 3.f : 0.f,
            valStr,
            good ? "Optimal (0x26)" : "Set to 0x26 (requires admin)",
            !good && IsRunningAsAdmin(),
            []() {
                HKEY hKey2 = nullptr;
                if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                    "SYSTEM\\CurrentControlSet\\Control\\PriorityControl",
                    0, KEY_SET_VALUE, &hKey2) == ERROR_SUCCESS)
                {
                    DWORD v = 0x26;
                    RegSetValueExA(hKey2, "Win32PrioritySeparation", 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&v), sizeof(v));
                    RegCloseKey(hKey2);
                }
            });
    }

    // --- Check 17: Windows Sound Scheme (2 pts) ---
    {
        char scheme[128] = {};
        DWORD cbVal = sizeof(scheme);
        HKEY hKey = nullptr;
        if (RegOpenKeyExA(HKEY_CURRENT_USER,
            "AppEvents\\Schemes\\.Current",
            0, KEY_READ, &hKey) == ERROR_SUCCESS)
        {
            RegQueryValueExA(hKey, "", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(scheme), &cbVal);
            RegCloseKey(hKey);
        }

        bool good = (scheme[0] == '\0' || strcmp(scheme, ".None") == 0);
        Add("System Health", "Windows Sound Scheme",
            "Windows system sounds play through the audio driver and can audibly "
            "interrupt a live performance.  Setting the scheme to 'No Sounds' prevents "
            "Windows from using the audio device for system alerts.",
            2.f,
            good ? STATUS_GOOD : STATUS_WARN,
            good ? 2.f : 0.f,
            good ? "No Sounds" : scheme,
            good ? "Silent (good)" : "Set Windows Sound Scheme to 'No Sounds'",
            !good,
            []() {
                // Write empty string to disable sounds
                HKEY hKey2 = nullptr;
                if (RegOpenKeyExA(HKEY_CURRENT_USER,
                    "AppEvents\\Schemes\\.Current",
                    0, KEY_SET_VALUE, &hKey2) == ERROR_SUCCESS)
                {
                    RegSetValueExA(hKey2, "", 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(".None"),
                                   (DWORD)(sizeof(".None")));
                    RegCloseKey(hKey2);
                }
                // Notify Windows of the change
                SendMessageTimeoutA(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                    (LPARAM)"Sounds", SMTO_ABORTIFHUNG, 1000, nullptr);
            });
    }

    // --- Check 18: Boot drive free space (2 pts) ---
    {
        ULARGE_INTEGER freeBytes = {}, totalBytes = {};
        GetDiskFreeSpaceExA("C:\\", &freeBytes, &totalBytes, nullptr);
        double pctFree = totalBytes.QuadPart > 0
            ? (double)freeBytes.QuadPart / (double)totalBytes.QuadPart * 100.0
            : 0.0;
        double freeGB = (double)freeBytes.QuadPart / (1024.0 * 1024.0 * 1024.0);

        bool good = (pctFree >= 20.0);
        char valStr[64] = {};
        snprintf(valStr, sizeof(valStr), "%.1f GB free (%.0f%%)", freeGB, pctFree);

        Add("System Health", "Boot Drive Free Space",
            "Low disk space on the system drive can prevent Windows paging and logging "
            "from operating smoothly, contributing to DPC latency.  Keep at least 20% "
            "free for best performance.",
            2.f,
            good ? STATUS_GOOD : STATUS_WARN,
            good ? 2.f : 0.f,
            valStr,
            good ? "Adequate free space" : "Free up disk space on C:\\ (aim for >20%)",
            false, nullptr);
    }

    // --- Check 19: Available RAM (1 pt) ---
    {
        MEMORYSTATUSEX ms = {};
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        double availGB = (double)ms.ullAvailPhys / (1024.0 * 1024.0 * 1024.0);

        bool good = (availGB >= 4.0);
        char valStr[64] = {};
        snprintf(valStr, sizeof(valStr), "%.1f GB available", availGB);

        Add("System Health", "Available RAM",
            "REAPER and plugins need RAM headroom.  With less than 4 GB available "
            "physical memory, Windows may begin using the page file, which can cause "
            "audio glitches due to memory access latency.",
            1.f,
            good ? STATUS_GOOD : STATUS_WARN,
            good ? 1.f : 0.5f,
            valStr,
            good ? "Sufficient RAM" : "Close other applications to free RAM",
            false, nullptr);
    }

    // --- Check 20: Background interfering processes (2 pts) ---
    {
        std::vector<std::string> bad = FindBadProcesses();
        bool good = bad.empty();
        std::string val = good ? "None found" : "";
        if (!good)
        {
            for (size_t i = 0; i < bad.size(); ++i)
            {
                if (i) val += ", ";
                val += bad[i];
            }
        }

        Add("System Health", "Background Processes",
            "Certain apps are known to cause DPC latency spikes or compete for audio "
            "resources: browsers (Chrome, Firefox), video apps (OBS, Zoom, Discord), "
            "cloud sync (OneDrive, Dropbox), and antivirus scanners.  Close them before "
            "a live performance for best stability.",
            2.f,
            good ? STATUS_GOOD : STATUS_WARN,
            good ? 2.f : 0.f,
            val,
            good ? "No known offenders running" : "Close listed apps before performing",
            false, nullptr);
    }

    // ===================================================================
    // CATEGORY 4 – Project State  (10 pts)
    // ===================================================================

    int numTracks = GetNumTracks ? GetNumTracks() : 0;

    // --- Check 21: Track count (4 pts) ---
    {
        int status;
        float pts;
        std::string rec;
        if      (numTracks < 32)  { status = STATUS_GOOD; pts = 4.f; rec = "Manageable track count"; }
        else if (numTracks < 64)  { status = STATUS_WARN; pts = 2.f; rec = "High track count – review unused tracks"; }
        else                      { status = STATUS_BAD;  pts = 0.f; rec = "Very high track count – may affect perf"; }

        char valStr[32] = {};
        snprintf(valStr, sizeof(valStr), "%d tracks", numTracks);
        Add("Project State", "Track Count",
            "More tracks means more DSP overhead.  Hidden or unused tracks still "
            "consume CPU if they have active FX.  Remove or freeze unused tracks.",
            4.f, status, pts, valStr, rec, false, nullptr);
    }

    // --- Check 22: Total FX count (3 pts) ---
    {
        int totalFX = 0;
        if (GetNumTracks && TrackFX_GetCount && GetTrack)
        {
            for (int t = 0; t < numTracks; ++t)
            {
                MediaTrack* tr = GetTrack(nullptr, t);
                if (tr) totalFX += TrackFX_GetCount(tr);
            }
        }

        int status;
        float pts;
        std::string rec;
        if      (totalFX <= 20) { status = STATUS_GOOD; pts = 3.f; rec = "Light FX load"; }
        else if (totalFX <= 50) { status = STATUS_WARN; pts = 1.f; rec = "Review enabled FX"; }
        else                    { status = STATUS_BAD;  pts = 0.f; rec = "Disable/freeze unused FX"; }

        char valStr[32] = {};
        snprintf(valStr, sizeof(valStr), "%d FX across all tracks", totalFX);
        Add("Project State", "Total FX Count",
            "Each active plugin adds to the real-time DSP budget.  Disable "
            "offline/rehearsal FX before the live show, or use FX Chains to "
            "quickly swap between rehearsal and live configurations.",
            3.f, status, pts, valStr, rec, false, nullptr);
    }

    // --- Check 23: Record-armed tracks (2 pts) ---
    {
        int armedCount = 0;
        if (GetNumTracks && GetTrack && GetSetMediaTrackInfo)
        {
            for (int t = 0; t < numTracks; ++t)
            {
                MediaTrack* tr = GetTrack(nullptr, t);
                if (tr)
                {
                    int armed = (int)(INT_PTR)GetSetMediaTrackInfo(tr, "I_RECARM", nullptr);
                    if (armed) ++armedCount;
                }
            }
        }

        int status;
        float pts;
        std::string rec;
        if      (armedCount == 0) { status = STATUS_GOOD; pts = 2.f; rec = "No armed tracks"; }
        else if (armedCount <= 2) { status = STATUS_WARN; pts = 1.f; rec = "Few armed tracks – expected for recording"; }
        else                      { status = STATUS_BAD;  pts = 0.f; rec = "Many armed tracks add record overhead"; }

        char valStr[32] = {};
        snprintf(valStr, sizeof(valStr), "%d armed", armedCount);
        Add("Project State", "Record-Armed Tracks",
            "Record-armed tracks keep monitoring active and buffer recording input "
            "continuously.  Unarm any tracks you are not actively recording to reduce "
            "system load.",
            2.f, status, pts, valStr, rec, false, nullptr);
    }

    // --- Check 24: Hardware output sends (1 pt) ---
    {
        int hwOutTracks = 0;
        if (GetNumTracks && GetTrack && GetTrackNumSends)
        {
            for (int t = 0; t < numTracks; ++t)
            {
                MediaTrack* tr = GetTrack(nullptr, t);
                if (tr && GetTrackNumSends(tr, 1) > 0)
                    ++hwOutTracks;
            }
        }
        bool hasHW = (hwOutTracks > 0);
        char valStr[48] = {};
        snprintf(valStr, sizeof(valStr), "%d tracks with HW sends", hwOutTracks);

        Add("Project State", "HW Output Sends",
            "Hardware output sends route directly from REAPER to physical audio "
            "outputs, bypassing the master.  Useful for FOH / monitor splits.  "
            "Informational only.",
            1.f,
            STATUS_INFO,
            1.f,
            valStr,
            "Informational",
            false, nullptr);
    }

    // ===================================================================
    // Compute total score
    // ===================================================================
    float total = 0.f, maxTotal = 0.f;
    for (const auto& c : g_checks)
    {
        total    += c.score;
        maxTotal += c.weight;
    }
    g_totalScore = (maxTotal > 0.f) ? (total / maxTotal * 100.f) : 0.f;
}

// ---------------------------------------------------------------------------
// PopulateList – rebuild the ListView from g_checks
// ---------------------------------------------------------------------------
static void PopulateList(HWND hwnd)
{
    if (!g_hList) return;

    // Remember selected index
    int selIdx = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);

    ListView_DeleteAllItems(g_hList);

    for (int i = 0; i < (int)g_checks.size(); ++i)
    {
        const LiveOptCheck& c = g_checks[i];

        LVITEMA lvi = {};
        lvi.mask    = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem   = i;
        lvi.iSubItem= 0;
        lvi.pszText = (LPSTR)"";
        lvi.lParam  = (LPARAM)i;
        ListView_InsertItem(g_hList, &lvi);

        ListView_SetItemText(g_hList, i, COL_CATEGORY, (LPSTR)c.category);
        ListView_SetItemText(g_hList, i, COL_NAME,     (LPSTR)c.name);
        ListView_SetItemText(g_hList, i, COL_VALUE,    (LPSTR)c.value.c_str());
        ListView_SetItemText(g_hList, i, COL_RECOMMEND,(LPSTR)c.recommend.c_str());
    }

    // Restore selection
    if (selIdx >= 0 && selIdx < (int)g_checks.size())
        ListView_SetItemState(g_hList, selIdx, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);

    // Update score text
    char scoreText[64] = {};
    snprintf(scoreText, sizeof(scoreText), "Live Score: %.0f / 100", g_totalScore);
    HWND hScoreTxt = GetDlgItem(hwnd, IDC_LO_SCORE_TEXT);
    if (hScoreTxt) SetWindowTextA(hScoreTxt, scoreText);

    // Trigger score bar repaint
    HWND hBar = GetDlgItem(hwnd, IDC_LO_SCORE_BAR);
    if (hBar) InvalidateRect(hBar, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// DrawScoreBar – render the colored score bar for IDC_LO_SCORE_BAR
// ---------------------------------------------------------------------------
static void DrawScoreBar(HWND hBar, HDC hdc)
{
    RECT rc;
    GetClientRect(hBar, &rc);
    int w  = rc.right  - rc.left;
    int h  = rc.bottom - rc.top;

    // Background
    HBRUSH hBkBrush = CreateSolidBrush(GetSysColor(COLOR_3DFACE));
    FillRect(hdc, &rc, hBkBrush);
    DeleteObject(hBkBrush);

    // Filled portion
    float score = max(0.f, min(100.f, g_totalScore));
    int fillW = (int)(w * score / 100.f);

    if (fillW > 0)
    {
        COLORREF barColor;
        if      (score >= 67.f) barColor = RGB( 50, 200,  50);  // green
        else if (score >= 34.f) barColor = RGB(230, 180,  30);  // yellow
        else                    barColor = RGB(220,  50,  50);  // red

        RECT fillRc = { rc.left, rc.top, rc.left + fillW, rc.bottom };
        HBRUSH hFill = CreateSolidBrush(barColor);
        FillRect(hdc, &fillRc, hFill);
        DeleteObject(hFill);
    }

    // Border
    DrawEdge(hdc, &rc, EDGE_SUNKEN, BF_RECT);

    // Label inside bar
    char label[32] = {};
    snprintf(label, sizeof(label), "%.0f / 100", score);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, (score < 50.f) ? RGB(255,255,255) : RGB(0,0,0));
    DrawTextA(hdc, label, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// ---------------------------------------------------------------------------
// LiveOptDlgProc
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK LiveOptDlgProc(HWND hwnd, UINT msg,
                                        WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    // -----------------------------------------------------------------------
    case WM_INITDIALOG:
    {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
        InitCommonControlsEx(&icc);

        // ---- Create SysListView32 over the placeholder ------------------
        HWND hPlaceholder = GetDlgItem(hwnd, IDC_LO_LIST);
        RECT rcList;
        GetWindowRect(hPlaceholder, &rcList);
        MapWindowPoints(nullptr, hwnd, (POINT*)&rcList, 2);
        ShowWindow(hPlaceholder, SW_HIDE);

        g_hList = CreateWindowExA(
            WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
            rcList.left, rcList.top,
            rcList.right - rcList.left, rcList.bottom - rcList.top,
            hwnd, (HMENU)(INT_PTR)IDC_LO_LIST, g_hInst, nullptr);

        ListView_SetExtendedListViewStyle(g_hList,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_INFOTIP);

        // Add columns
        struct ColDef { const char* name; int width; } cols[COL_COUNT] = {
            { "",              26 },  // status dot
            { "Category",      90 },
            { "Setting",      170 },
            { "Current Value",130 },
            { "Recommendation",165 },
        };
        for (int i = 0; i < COL_COUNT; ++i)
        {
            LVCOLUMNA lvc = {};
            lvc.mask    = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            lvc.iSubItem= i;
            lvc.pszText = (LPSTR)cols[i].name;
            lvc.cx      = cols[i].width;
            ListView_InsertColumn(g_hList, i, &lvc);
        }

        // Run checks and populate
        RunChecks();
        PopulateList(hwnd);

        // Start polling timer (every 3 seconds)
        SetTimer(hwnd, LO_TIMER_ID, 3000, nullptr);

        return TRUE;
    }

    // -----------------------------------------------------------------------
    case WM_TIMER:
        if (wParam == LO_TIMER_ID)
        {
            RunChecks();
            PopulateList(hwnd);
        }
        return 0;

    // -----------------------------------------------------------------------
    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lParam;
        if (di && di->CtlID == IDC_LO_SCORE_BAR)
        {
            DrawScoreBar(di->hwndItem, di->hDC);
            return TRUE;
        }
        break;
    }

    // -----------------------------------------------------------------------
    case WM_NOTIFY:
    {
        NMHDR* pnm = (NMHDR*)lParam;
        if (!pnm || pnm->hwndFrom != g_hList) break;

        if (pnm->code == NM_CUSTOMDRAW)
        {
            NMLVCUSTOMDRAW* pcd = (NMLVCUSTOMDRAW*)lParam;
            switch (pcd->nmcd.dwDrawStage)
            {
            case CDDS_PREPAINT:
                SetWindowLongPtr(hwnd, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                return TRUE;

            case CDDS_ITEMPREPAINT:
                SetWindowLongPtr(hwnd, DWLP_MSGRESULT, CDRF_NOTIFYSUBITEMDRAW);
                return TRUE;

            case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
            {
                if (pcd->iSubItem != COL_STATUS) break;

                const int idx = (int)pcd->nmcd.dwItemSpec;
                if (idx < 0 || idx >= (int)g_checks.size()) break;

                HDC  hdc  = pcd->nmcd.hdc;
                RECT rcIt = pcd->nmcd.rc;

                // Fill background
                const bool isSel = (ListView_GetItemState(g_hList, idx, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                COLORREF bg = isSel ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_WINDOW);
                SetBkColor(hdc, bg);
                ExtTextOutA(hdc, 0, 0, ETO_OPAQUE, &rcIt, "", 0, nullptr);

                // Draw colored circle
                COLORREF dotColor;
                switch (g_checks[idx].status)
                {
                case STATUS_GOOD: dotColor = RGB( 50, 200,  50); break;
                case STATUS_WARN: dotColor = RGB(230, 180,  30); break;
                case STATUS_BAD:  dotColor = RGB(220,  50,  50); break;
                default:          dotColor = RGB(160, 160, 160); break;
                }

                const int r = 6;
                int cx = rcIt.left + (rcIt.right  - rcIt.left)  / 2;
                int cy = rcIt.top  + (rcIt.bottom - rcIt.top)   / 2;
                RECT rcEll = { cx - r, cy - r, cx + r, cy + r };

                HBRUSH hDot   = CreateSolidBrush(dotColor);
                HPEN   hPen   = CreatePen(PS_SOLID, 1, RGB(0,0,0));
                HBRUSH hOldBr = (HBRUSH)SelectObject(hdc, hDot);
                HPEN   hOldPn = (HPEN)  SelectObject(hdc, hPen);
                Ellipse(hdc, rcEll.left, rcEll.top, rcEll.right, rcEll.bottom);
                SelectObject(hdc, hOldBr);
                SelectObject(hdc, hOldPn);
                DeleteObject(hDot);
                DeleteObject(hPen);

                SetWindowLongPtr(hwnd, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                return TRUE;
            }
            } // switch dwDrawStage
        }
        else if (pnm->code == LVN_ITEMCHANGED)
        {
            NMLISTVIEW* plv = (NMLISTVIEW*)lParam;
            if (plv->uNewState & LVIS_SELECTED)
            {
                g_selectedCheck = plv->iItem;
                HWND hInfo = GetDlgItem(hwnd, IDC_LO_INFO);
                if (hInfo && g_selectedCheck >= 0 &&
                    g_selectedCheck < (int)g_checks.size())
                {
                    SetWindowTextA(hInfo, g_checks[g_selectedCheck].tooltip);
                }
                // Update Apply Fix button state
                HWND hFix = GetDlgItem(hwnd, IDC_LO_APPLY_FIX);
                if (hFix)
                {
                    bool canFix = (g_selectedCheck >= 0 &&
                                   g_selectedCheck < (int)g_checks.size() &&
                                   g_checks[g_selectedCheck].canFix &&
                                   g_checks[g_selectedCheck].fixFunc);
                    EnableWindow(hFix, canFix ? TRUE : FALSE);
                }
            }
        }
        break;
    }

    // -----------------------------------------------------------------------
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == IDC_LO_REFRESH)
        {
            HWND hStatus = GetDlgItem(hwnd, IDC_LO_STATUS);
            if (hStatus) SetWindowTextA(hStatus, "Scanning...");
            RunChecks();
            PopulateList(hwnd);
            if (hStatus) SetWindowTextA(hStatus, "Scan complete.");
        }
        else if (id == IDC_LO_APPLY_FIX)
        {
            if (g_selectedCheck >= 0 && g_selectedCheck < (int)g_checks.size())
            {
                LiveOptCheck& c = g_checks[g_selectedCheck];
                if (c.canFix && c.fixFunc)
                {
                    c.fixFunc();
                    HWND hStatus = GetDlgItem(hwnd, IDC_LO_STATUS);
                    if (hStatus)
                    {
                        char msg[128] = {};
                        snprintf(msg, sizeof(msg), "Fix applied: %s", c.name);
                        SetWindowTextA(hStatus, msg);
                    }
                    // Re-scan to reflect new state
                    RunChecks();
                    PopulateList(hwnd);
                }
            }
        }
        break;
    }

    // -----------------------------------------------------------------------
    case WM_SIZE:
    {
        int dlgW = LOWORD(lParam);
        int dlgH = HIWORD(lParam);

        // Score bar: full width minus score text label
        const int kBarX = 5, kBarY = 3, kBarH = 18;
        const int kTxtW = 110;
        const int kBarW = dlgW - kBarX - kTxtW - 10;
        HWND hBar = GetDlgItem(hwnd, IDC_LO_SCORE_BAR);
        if (hBar) SetWindowPos(hBar, nullptr, kBarX, kBarY, kBarW, kBarH,
                               SWP_NOZORDER | SWP_NOACTIVATE);
        HWND hTxt = GetDlgItem(hwnd, IDC_LO_SCORE_TEXT);
        if (hTxt) SetWindowPos(hTxt, nullptr, kBarX + kBarW + 5, kBarY + 2,
                               kTxtW, 12, SWP_NOZORDER | SWP_NOACTIVATE);

        // List view: below score bar, above info panel
        const int kListY  = kBarY + kBarH + 4;
        const int kInfoH  = 48;
        const int kBtnH   = 14;
        const int kBtnY   = dlgH - kBtnH - 4;
        const int kStatusY= kBtnY + 2;
        const int kInfoY  = kBtnY - kInfoH - 4;
        const int kListH  = kInfoY - kListY - 4;

        if (g_hList)
            SetWindowPos(g_hList, nullptr, kBarX, kListY,
                         dlgW - 10, max(kListH, 40),
                         SWP_NOZORDER | SWP_NOACTIVATE);

        HWND hInfo = GetDlgItem(hwnd, IDC_LO_INFO);
        if (hInfo)
            SetWindowPos(hInfo, nullptr, kBarX, kInfoY,
                         dlgW - 10, kInfoH, SWP_NOZORDER | SWP_NOACTIVATE);

        HWND hRef = GetDlgItem(hwnd, IDC_LO_REFRESH);
        if (hRef) SetWindowPos(hRef, nullptr, kBarX, kBtnY,
                               55, kBtnH, SWP_NOZORDER | SWP_NOACTIVATE);

        HWND hFix = GetDlgItem(hwnd, IDC_LO_APPLY_FIX);
        if (hFix) SetWindowPos(hFix, nullptr, kBarX + 60, kBtnY,
                               55, kBtnH, SWP_NOZORDER | SWP_NOACTIVATE);

        HWND hStat = GetDlgItem(hwnd, IDC_LO_STATUS);
        if (hStat) SetWindowPos(hStat, nullptr, kBarX + 120, kStatusY,
                                dlgW - kBarX - 125, 10, SWP_NOZORDER | SWP_NOACTIVATE);

        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    // -----------------------------------------------------------------------
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mm = (MINMAXINFO*)lParam;
        mm->ptMinTrackSize.x = 420;
        mm->ptMinTrackSize.y = 300;
        return 0;
    }

    // -----------------------------------------------------------------------
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return TRUE;

    // -----------------------------------------------------------------------
    case WM_DESTROY:
        KillTimer(hwnd, LO_TIMER_ID);
        g_hList = nullptr;
        g_wnd   = nullptr;
        return 0;
    }

    return FALSE;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void LiveOptimizeWnd_Init(HINSTANCE hInstance)
{
    g_hInst = hInstance;
    HWND hMain = GetMainHwnd ? GetMainHwnd() : nullptr;
    g_wnd = CreateDialogParamA(hInstance,
                                MAKEINTRESOURCEA(IDD_LIVE_OPTIMIZE),
                                hMain, LiveOptDlgProc, 0);
    if (g_wnd)
    {
        SetWindowTextA(g_wnd, "Live Tools - Live Optimizer");
        // Start hidden; user shows via menu/action
        ShowWindow(g_wnd, SW_HIDE);
    }
}

void LiveOptimizeWnd_ShowHide()
{
    if (!g_wnd || !IsWindow(g_wnd))
    {
        LiveOptimizeWnd_Init(g_hInst);
        if (!g_wnd) return;
    }

    if (IsWindowVisible(g_wnd))
    {
        ShowWindow(g_wnd, SW_HIDE);
    }
    else
    {
        ShowWindow(g_wnd, SW_SHOW);
        SetForegroundWindow(g_wnd);
        // Trigger immediate scan on show
        RunChecks();
        PopulateList(g_wnd);
    }
}

int LiveOptimizeWnd_IsVisible()
{
    return (g_wnd && IsWindow(g_wnd) && IsWindowVisible(g_wnd)) ? 1 : 0;
}

void LiveOptimizeWnd_Cleanup()
{
    if (g_wnd && IsWindow(g_wnd))
    {
        KillTimer(g_wnd, LO_TIMER_ID);
        DestroyWindow(g_wnd);
    }
    g_wnd   = nullptr;
    g_hList = nullptr;
    g_checks.clear();
}
