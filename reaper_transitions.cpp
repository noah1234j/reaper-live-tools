// ---------------------------------------------------------------------------
// reaper_transitions.cpp  –  REAPER extension DLL entry point
//
// THIS file must be the only one that defines REAPERAPI_IMPLEMENT.
// All other .cpp files just #include "api.h" without the define.
// ---------------------------------------------------------------------------
#define REAPERAPI_IMPLEMENT
#include "api.h"

#include "TransitionSnapshot.h"
#include "TransitionEngine.h"
#include "TransitionWnd.h"
#include "SafesWnd.h"
#include "LayoutsWnd.h"
#include "PaflWnd.h"
#include "MonitorWnd.h"
#include "LiveOptimizeWnd.h"
#include "ControlSurface.h"

// reaper_plugin.h is included transitively via api.h → reaper_plugin_functions.h
// (the SDK SDK dir is on the include path via CMake)

#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static bool ProcessExtensionLine(const char* line, ProjectStateContext* ctx,
                                 bool isUndo, struct project_config_extension_t*);
static void SaveExtensionConfig(ProjectStateContext* ctx, bool isUndo,
                                struct project_config_extension_t*);
static void BeginLoadProjectState(bool isUndo,
                                  struct project_config_extension_t*);

static bool RunCommand(int cmd, int);
static int  ToggleAction(int cmd);
static void MenuHook(const char* menustr, HMENU hMenu, int flag);

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static HINSTANCE g_hInst          = nullptr;
static int       g_cmdShowHide    = 0;
static int       g_cmdShowLayouts = 0;
static int       g_cmdShowPafl    = 0;
static int       g_cmdShowMonitor = 0;
static int       g_cmdLiveOpt     = 0;
static int       g_cmdShowCSurf   = 0;

// Per-scene recall/save actions (1-based slots 1-30, stored 0-based)
static const int kSceneActionCount = 30;
static int               g_cmdRecallScene[kSceneActionCount] = {};
static int               g_cmdSaveScene[kSceneActionCount]   = {};
static gaccel_register_t g_recallAccel[kSceneActionCount]    = {};
static gaccel_register_t g_saveAccel[kSceneActionCount]      = {};
static char              g_recallCmdStr[kSceneActionCount][32];
static char              g_saveCmdStr[kSceneActionCount][32];
static char              g_recallDesc[kSceneActionCount][64];
static char              g_saveDesc[kSceneActionCount][64];

static project_config_extension_t g_projectconfig =
{
    ProcessExtensionLine,
    SaveExtensionConfig,
    BeginLoadProjectState,
    nullptr  // userData
};

// ---------------------------------------------------------------------------
// Project state persistence
// ---------------------------------------------------------------------------

// Called once before REAPER starts feeding lines for a project load/undo.
static void BeginLoadProjectState(bool isUndo,
                                  struct project_config_extension_t*)
{
    g_snapshots.clear();
    g_layouts.clear();
    PaflWnd_ResetProjectState();
    if (!isUndo)
        PaflWnd_OnProjectLoad();
}

// Called for each unrecognised extension line in the .RPP file.
// NOTE: REAPER strips the leading '<' before calling us.
static bool ProcessExtensionLine(const char* line,
                                 ProjectStateContext* ctx,
                                 bool /*isUndo*/,
                                 struct project_config_extension_t*)
{
    if (!line) return false;

    // Skip leading whitespace
    while (*line == ' ' || *line == '\t') ++line;

    // REAPER passes extension lines WITH the leading '<' intact (same as SWS).
    if (strncmp(line, "<TSSNAPSHOT", 11) == 0)
    {
        TransitionSnapshot* ss = TransitionSnapshot::Deserialize(line, ctx);
        if (ss)
        {
            g_snapshots.push_back(std::unique_ptr<TransitionSnapshot>(ss));
            TransitionWnd_RefreshList();
        }
        return true;
    }

    if (strncmp(line, "<LTLAYOUT", 9) == 0)
    {
        LayoutSnapshot* ly = LayoutSnapshot::Deserialize(line, ctx);
        if (ly)
        {
            g_layouts.push_back(std::unique_ptr<LayoutSnapshot>(ly));
            LayoutsWnd_RefreshList();
        }
        return true;
    }

    if (PaflWnd_ProcessLine(line)) return true;

    return false;
}

// Called when REAPER saves the project (or writes an undo state).
static void SaveExtensionConfig(ProjectStateContext* ctx,
                                bool /*isUndo*/,
                                struct project_config_extension_t*)
{
    for (const auto& ss : g_snapshots)
        ss->Serialize(ctx);
    for (const auto& ly : g_layouts)
        ly->Serialize(ctx);
    PaflWnd_SaveConfig(ctx);
}

// ---------------------------------------------------------------------------
// Action callbacks
// ---------------------------------------------------------------------------
static bool RunCommand(int cmd, int /*flag*/)
{
    if (cmd == g_cmdShowHide)    { TransitionWnd_ShowHide(); return true; }
    if (cmd == g_cmdShowLayouts) { LayoutsWnd_ShowHide();    return true; }
    if (cmd == g_cmdShowPafl)    { PaflWnd_ShowHide();       return true; }
    if (cmd == g_cmdShowMonitor) { MonitorWnd_ShowHide();    return true; }
    if (cmd == g_cmdLiveOpt)     { LiveOptimizeWnd_ShowHide(); return true; }
    if (cmd == g_cmdShowCSurf)   {
        HWND parent = GetMainHwnd ? GetMainHwnd() : nullptr;
        CSurf_ShowStandaloneConfig(parent);
        return true;
    }
    // Per-scene recall / save
    for (int i = 0; i < kSceneActionCount; i++)
    {
        if (cmd == g_cmdRecallScene[i]) { TransitionWnd_RecallScene(i);    return true; }
        if (cmd == g_cmdSaveScene[i])   { TransitionWnd_OverwriteScene(i); return true; }
    }
    return false;
}

static int ToggleAction(int cmd)
{
    if (cmd == g_cmdShowHide)    return TransitionWnd_IsVisible()  ? 1 : 0;
    if (cmd == g_cmdShowLayouts) return LayoutsWnd_IsVisible()     ? 1 : 0;
    if (cmd == g_cmdShowPafl)    return PaflWnd_IsVisible()         ? 1 : 0;
    if (cmd == g_cmdShowMonitor) return MonitorWnd_IsVisible()      ? 1 : 0;
    if (cmd == g_cmdLiveOpt)     return LiveOptimizeWnd_IsVisible() ? 1 : 0;
    return -1;
}

// ---------------------------------------------------------------------------
// Plugin entry point
// ---------------------------------------------------------------------------
extern "C" REAPER_PLUGIN_DLL_EXPORT int ReaperPluginEntry(HINSTANCE hInstance,
                                                           reaper_plugin_info_t* rec)
{
    // Unload path
    if (!rec)
    {
        TransitionWnd_Cleanup();
        SafesWnd_Cleanup();
        LayoutsWnd_Cleanup();
        PaflWnd_Cleanup();
        MonitorWnd_Cleanup();
        LiveOptimizeWnd_Cleanup();
        CSurf_Unregister(nullptr); // uses plugin_register directly
        plugin_register("-timer",          (void*)&TransitionEngine::TimerCallback);
        plugin_register("-projectconfig",  &g_projectconfig);
        plugin_register("-hookcustommenu", (void*)MenuHook);
        // Unregister per-scene actions
        for (int i = 0; i < kSceneActionCount; i++)
        {
            if (g_cmdRecallScene[i]) plugin_register("-gaccel", &g_recallAccel[i]);
            if (g_cmdSaveScene[i])   plugin_register("-gaccel", &g_saveAccel[i]);
        }
        return 0;
    }

    // Version check
    if (rec->caller_version != REAPER_PLUGIN_VERSION) return 0;

    // Load REAPER API function pointers
    if (REAPERAPI_LoadAPI(rec->GetFunc) != 0) return 0; // missing required functions

    g_hInst = hInstance;

    // ---- Register action command ID ----------------------------------------
    g_cmdShowHide = plugin_register("command_id",
                                    (void*)"TS_SHOW_HIDE");
    if (!g_cmdShowHide) return 0;

    // Register accelerator / action name (Scenes)
    static gaccel_register_t g_accel;
    memset(&g_accel, 0, sizeof(g_accel));
    g_accel.desc      = "Live Tools: Scenes - Show/Hide";
    g_accel.accel.cmd = (WORD)g_cmdShowHide;
    plugin_register("gaccel", &g_accel);

    // ---- Register Layouts command ----------------------------------------
    g_cmdShowLayouts = plugin_register("command_id",
                                       (void*)"LT_SHOW_LAYOUTS");
    if (!g_cmdShowLayouts) return 0;

    static gaccel_register_t g_layoutAccel;
    memset(&g_layoutAccel, 0, sizeof(g_layoutAccel));
    g_layoutAccel.desc      = "Live Tools: Layouts - Show/Hide";
    g_layoutAccel.accel.cmd = (WORD)g_cmdShowLayouts;
    plugin_register("gaccel", &g_layoutAccel);

    // ---- Register callbacks ------------------------------------------------
    plugin_register("hookcommand",    (void*)RunCommand);
    plugin_register("toggleaction",   (void*)ToggleAction);
    plugin_register("projectconfig",  &g_projectconfig);
    plugin_register("hookcustommenu", (void*)MenuHook);

    // ---- Register PAFL command --------------------------------------------
    g_cmdShowPafl = plugin_register("command_id", (void*)"LT_PAFL");
    if (!g_cmdShowPafl) return 0;

    static gaccel_register_t g_paflAccel;
    memset(&g_paflAccel, 0, sizeof(g_paflAccel));
    g_paflAccel.desc      = "Live Tools: PAFL Monitor - Show/Hide";
    g_paflAccel.accel.cmd = (WORD)g_cmdShowPafl;
    plugin_register("gaccel", &g_paflAccel);

    // ---- Register Control Surface settings command -------------------------
    g_cmdShowCSurf = plugin_register("command_id", (void*)"LT_CSURF_SETTINGS");
    if (!g_cmdShowCSurf) return 0;

    static gaccel_register_t g_csurfAccel;
    memset(&g_csurfAccel, 0, sizeof(g_csurfAccel));
    g_csurfAccel.desc      = "Live Tools: Control Surface Settings";
    g_csurfAccel.accel.cmd = (WORD)g_cmdShowCSurf;
    plugin_register("gaccel", &g_csurfAccel);

    // ---- Register Live Monitor command ------------------------------------
    g_cmdShowMonitor = plugin_register("command_id", (void*)"LT_MONITOR");
    if (!g_cmdShowMonitor) return 0;

    static gaccel_register_t g_monitorAccel;
    memset(&g_monitorAccel, 0, sizeof(g_monitorAccel));
    g_monitorAccel.desc      = "Live Tools: Live Monitor - Show/Hide";
    g_monitorAccel.accel.cmd = (WORD)g_cmdShowMonitor;
    plugin_register("gaccel", &g_monitorAccel);

    // ---- Register Live Optimizer command ----------------------------------
    g_cmdLiveOpt = plugin_register("command_id", (void*)"LT_LIVE_OPTIMIZER");
    if (!g_cmdLiveOpt) return 0;

    static gaccel_register_t g_liveOptAccel;
    memset(&g_liveOptAccel, 0, sizeof(g_liveOptAccel));
    g_liveOptAccel.desc      = "Live Tools: Live Optimizer - Show/Hide";
    g_liveOptAccel.accel.cmd = (WORD)g_cmdLiveOpt;
    plugin_register("gaccel", &g_liveOptAccel);

    // ---- Register per-scene recall / save actions (slots 1-30) -----------
    for (int i = 0; i < kSceneActionCount; i++)
    {
        snprintf(g_recallCmdStr[i], sizeof(g_recallCmdStr[i]), "LT_RECALL_SCENE_%02d", i + 1);
        snprintf(g_saveCmdStr[i],   sizeof(g_saveCmdStr[i]),   "LT_SAVE_SCENE_%02d",   i + 1);
        snprintf(g_recallDesc[i],   sizeof(g_recallDesc[i]),   "Live Tools: Recall Scene %d", i + 1);
        snprintf(g_saveDesc[i],     sizeof(g_saveDesc[i]),     "Live Tools: Save Scene %d",   i + 1);

        g_cmdRecallScene[i] = plugin_register("command_id", (void*)g_recallCmdStr[i]);
        g_cmdSaveScene[i]   = plugin_register("command_id", (void*)g_saveCmdStr[i]);

        memset(&g_recallAccel[i], 0, sizeof(gaccel_register_t));
        g_recallAccel[i].desc      = g_recallDesc[i];
        g_recallAccel[i].accel.cmd = (WORD)g_cmdRecallScene[i];
        plugin_register("gaccel", &g_recallAccel[i]);

        memset(&g_saveAccel[i], 0, sizeof(gaccel_register_t));
        g_saveAccel[i].desc      = g_saveDesc[i];
        g_saveAccel[i].accel.cmd = (WORD)g_cmdSaveScene[i];
        plugin_register("gaccel", &g_saveAccel[i]);
    }

    // ---- Init UI -----------------------------------------------------------
    TransitionWnd_Init(hInstance);
    SafesWnd_Init(hInstance);
    LayoutsWnd_Init(hInstance);
    PaflWnd_Init(hInstance);
    MonitorWnd_Init(hInstance);
    LiveOptimizeWnd_Init(hInstance);
    CSurfConfigDlg_SetInstance(hInstance);
    CSurf_Register(rec);

    return 1; // success
}

// ---------------------------------------------------------------------------
// MenuHook – adds our Show/Hide item to the REAPER Extensions menu
//
// REAPER (via SWS or natively) calls hookcustommenu callbacks with:
//   flag == 0 : menu is being built   → add items
//   flag == 1 : menu is about to show → update check states
// ---------------------------------------------------------------------------
static HMENU s_hLiveToolsMenu = nullptr;

static void MenuHook(const char* menustr, HMENU hMenu, int flag)
{
    if (!g_cmdShowHide) return;
    if (strcmp(menustr, "Main extensions") != 0) return;

    if (flag == 0)
    {
        // Build: add "Live Tools" submenu containing Scenes + Layouts
        HMENU hSub = CreatePopupMenu();
        AppendMenuA(hSub,  MF_STRING, (UINT_PTR)g_cmdShowHide,    "Scenes...");
        AppendMenuA(hSub,  MF_STRING, (UINT_PTR)g_cmdShowLayouts, "Layouts...");
        AppendMenuA(hSub,  MF_STRING, (UINT_PTR)g_cmdShowPafl,    "PAFL Monitor...");
        AppendMenuA(hSub,  MF_STRING, (UINT_PTR)g_cmdShowMonitor, "Live Monitor...");
        AppendMenuA(hSub,  MF_STRING, (UINT_PTR)g_cmdLiveOpt,     "Live Optimizer...");
        AppendMenuA(hSub,  MF_SEPARATOR, 0, nullptr);
        AppendMenuA(hSub,  MF_STRING, (UINT_PTR)g_cmdShowCSurf,   "Control Surface Settings...");
        AppendMenuA(hMenu, MF_POPUP,  (UINT_PTR)hSub,             "Live Tools");
        s_hLiveToolsMenu = hSub;
    }
    else if (flag == 1 && s_hLiveToolsMenu)
    {
        // Update check states (indices 0-4 = Scenes, Layouts, PAFL, Monitor, Optimizer)
        CheckMenuItem(s_hLiveToolsMenu, 0, MF_BYPOSITION |
            (TransitionWnd_IsVisible()    ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(s_hLiveToolsMenu, 1, MF_BYPOSITION |
            (LayoutsWnd_IsVisible()       ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(s_hLiveToolsMenu, 2, MF_BYPOSITION |
            (PaflWnd_IsVisible()          ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(s_hLiveToolsMenu, 3, MF_BYPOSITION |
            (MonitorWnd_IsVisible()       ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(s_hLiveToolsMenu, 4, MF_BYPOSITION |
            (LiveOptimizeWnd_IsVisible()  ? MF_CHECKED : MF_UNCHECKED));
    }
}
