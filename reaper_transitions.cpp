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
#include "MeterBridgeWnd.h"
#include "LiveOptimizeWnd.h"
#include "LiveLockEngine.h"
#include "LiveLockWnd.h"
#include "ControlSurface.h"
#include "TalkbackWnd.h"
#include "LayersWnd.h"
#include "DcaEngine.h"
#include "DcaWnd.h"
#include "MuteGroup.h"
#include "MuteGroupsWnd.h"
#include "DcaGroup.h"
#include "DcaWnd.h"
#include "DcaEngine.h"
#include "LayersEngine.h"
#include "LayersWnd.h"

// reaper_plugin.h is included transitively via api.h → reaper_plugin_functions.h
// (the SDK SDK dir is on the include path via CMake)

#include <memory>
#include <vector>
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
static HINSTANCE g_hInst              = nullptr;
static int       g_cmdShowHide        = 0;
static int       g_cmdShowLayouts     = 0;
static int       g_cmdShowPafl        = 0;
static int       g_cmdShowMonitor     = 0;
static int       g_cmdLiveOpt         = 0;
static int       g_cmdShowMeterBridge = 0;
static int       g_cmdShowLiveLock    = 0;
static int       g_cmdShowLayers      = 0;
static int       g_cmdShowMuteGroups  = 0;
static int       g_cmdShowDca         = 0;
static int       g_cmdShowCSurf       = 0;
static int       g_cmdShowTalkback    = 0;
static int       g_cmdTbOn            = 0;
static int       g_cmdTbOff           = 0;
static gaccel_register_t g_liveLockAccel;
static gaccel_register_t g_talkbackAccel;
static gaccel_register_t g_tbOnAccel;
static gaccel_register_t g_tbOffAccel;
static gaccel_register_t g_muteGroupsAccel;

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
    g_dcaGroups.clear();
    TransitionWnd_ResetCueList();
    TransitionWnd_ResetSettings();
    LayersEngine::Get().ResetForProject();
    LiveLockEngine::Get().ResetSettingsToDefaults();
    DcaWnd_ResetSettings();
    PaflWnd_ResetProjectState();
    MuteGroupsEngine::Get().ResetForProject();
    if (!isUndo)
    {
        PaflWnd_OnProjectLoad();
        TalkbackWnd_OnProjectLoad();
        DcaWnd_OnProjectLoad();
    }
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
    if (strncmp(line, "<TSSPACER", 9) == 0)
    {
        int slot = 0;
        sscanf(line, "<TSSPACER %d", &slot);
        auto* ss = new TransitionSnapshot(slot, "");
        ss->m_isSpacer = true;
        char tmp[64]; ctx->GetLine(tmp, sizeof(tmp));  // consume closing ">"
        g_snapshots.push_back(std::unique_ptr<TransitionSnapshot>(ss));
        TransitionWnd_RefreshList();
        return true;
    }

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
    if (TalkbackWnd_ProcessLine(line)) return true;
    if (MuteGroupsEngine::Get().ProcessLine(line)) { MuteGroupsWnd_Refresh(); return true; }

    if (strncmp(line, "<DCAGROUP", 9) == 0)
    {
        DcaGroup* dca = DcaGroup::Deserialize(line, ctx);
        if (dca)
        {
            g_dcaGroups.push_back(std::unique_ptr<DcaGroup>(dca));
            DcaWnd_Refresh();
        }
        return true;
    }

    if (TransitionWnd_LoadCueListLine(line)) return true;
    if (TransitionWnd_ProcessSettingsLine(line)) return true;
    if (LayersEngine::Get().ProcessLine(line, ctx)) return true;
    if (LiveLockEngine::Get().ProcessLine(line)) return true;
    if (DcaWnd_ProcessSettingsLine(line)) return true;

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
    TalkbackWnd_SaveConfig(ctx);
    MuteGroupsEngine::Get().SaveConfig(ctx);
    for (const auto& dca : g_dcaGroups)
        dca->Serialize(ctx);
    TransitionWnd_SaveCueList(ctx);
    TransitionWnd_SaveSettings(ctx);
    LayersEngine::Get().SaveConfig(ctx);
    LiveLockEngine::Get().SaveConfig(ctx);
    DcaWnd_SaveSettings(ctx);
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
    if (cmd == g_cmdShowMeterBridge) { MeterBridgeWnd_ShowHide();  return true; }
    if (cmd == g_cmdLiveOpt)     { LiveOptimizeWnd_ShowHide(); return true; }
    if (cmd == g_cmdShowLiveLock) { LiveLockWnd_ShowHide();    return true; }
    if (cmd == g_cmdShowTalkback) { TalkbackWnd_ShowHide();    return true; }
    if (cmd == g_cmdTbOn)         { TalkbackWnd_TbOn();        return true; }
    if (cmd == g_cmdTbOff)        { TalkbackWnd_TbOff();       return true; }
    if (cmd == g_cmdShowMuteGroups) { MuteGroupsWnd_ShowHide(); return true; }
    if (cmd == g_cmdShowLayers)  { LayersWnd_ShowHide();         return true; }
    if (cmd == g_cmdShowDca)     { DcaWnd_ShowHide();            return true; }
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
    if (cmd == g_cmdShowMeterBridge) return MeterBridgeWnd_IsVisible()  ? 1 : 0;
    if (cmd == g_cmdLiveOpt)     return LiveOptimizeWnd_IsVisible()  ? 1 : 0;
    if (cmd == g_cmdShowLiveLock) return LiveLockWnd_IsVisible()     ? 1 : 0;
    if (cmd == g_cmdShowTalkback) return TalkbackWnd_IsVisible()      ? 1 : 0;
    if (cmd == g_cmdShowMuteGroups) return MuteGroupsWnd_IsVisible() ? 1 : 0;
    if (cmd == g_cmdShowLayers)     return LayersWnd_IsVisible()     ? 1 : 0;
    if (cmd == g_cmdShowDca)        return DcaWnd_IsVisible()        ? 1 : 0;
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
        MeterBridgeWnd_Cleanup();
        LiveOptimizeWnd_Cleanup();
        LiveLockWnd_Cleanup();
        LiveLockEngine_Cleanup();
        TalkbackWnd_Cleanup();
        MuteGroupsWnd_Cleanup();
        LayersWnd_Cleanup();
        LayersEngine_Cleanup();
        DcaWnd_Cleanup();
        plugin_register("-timer",          (void*)LiveLockEngine::TimerCallback);
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

    // ---- Register Meter Bridge command ------------------------------------
    g_cmdShowMeterBridge = plugin_register("command_id", (void*)"LT_METERBRIDGE");
    if (!g_cmdShowMeterBridge) return 0;

    static gaccel_register_t g_meterBridgeAccel;
    memset(&g_meterBridgeAccel, 0, sizeof(g_meterBridgeAccel));
    g_meterBridgeAccel.desc      = "Live Tools: Meter Bridge - Show/Hide";
    g_meterBridgeAccel.accel.cmd = (WORD)g_cmdShowMeterBridge;
    plugin_register("gaccel", &g_meterBridgeAccel);

    // ---- Register Live Lock command ---------------------------------------
    g_cmdShowLiveLock = plugin_register("command_id", (void*)"LT_LIVELOCK");
    if (!g_cmdShowLiveLock) return 0;

    memset(&g_liveLockAccel, 0, sizeof(g_liveLockAccel));
    g_liveLockAccel.desc      = "Live Tools: Live Lock - Show/Hide";
    g_liveLockAccel.accel.cmd = (WORD)g_cmdShowLiveLock;
    plugin_register("gaccel", &g_liveLockAccel);

    // Register the live lock enforcement timer
    plugin_register("timer", (void*)LiveLockEngine::TimerCallback);

    // ---- Register Talkback commands --------------------------------------
    g_cmdShowTalkback = plugin_register("command_id", (void*)"LT_TALKBACK");
    if (!g_cmdShowTalkback) return 0;

    memset(&g_talkbackAccel, 0, sizeof(g_talkbackAccel));
    g_talkbackAccel.desc      = "Live Tools: Talkback - Show/Hide";
    g_talkbackAccel.accel.cmd = (WORD)g_cmdShowTalkback;
    plugin_register("gaccel", &g_talkbackAccel);

    g_cmdTbOn = plugin_register("command_id", (void*)"LT_TB_ON");
    if (!g_cmdTbOn) return 0;

    memset(&g_tbOnAccel, 0, sizeof(g_tbOnAccel));
    g_tbOnAccel.desc      = "Live Tools: Talkback - On";
    g_tbOnAccel.accel.cmd = (WORD)g_cmdTbOn;
    plugin_register("gaccel", &g_tbOnAccel);

    g_cmdTbOff = plugin_register("command_id", (void*)"LT_TB_OFF");
    if (!g_cmdTbOff) return 0;

    memset(&g_tbOffAccel, 0, sizeof(g_tbOffAccel));
    g_tbOffAccel.desc      = "Live Tools: Talkback - Off";
    g_tbOffAccel.accel.cmd = (WORD)g_cmdTbOff;
    plugin_register("gaccel", &g_tbOffAccel);

    // ---- Register Mute Groups command ------------------------------------
    g_cmdShowMuteGroups = plugin_register("command_id", (void*)"LT_MUTEGROUPS");
    if (!g_cmdShowMuteGroups) return 0;

    memset(&g_muteGroupsAccel, 0, sizeof(g_muteGroupsAccel));
    g_muteGroupsAccel.desc      = "Live Tools: Mute Groups - Show/Hide";
    g_muteGroupsAccel.accel.cmd = (WORD)g_cmdShowMuteGroups;
    plugin_register("gaccel", &g_muteGroupsAccel);

    // ---- Register Layers command -----------------------------------------
    static gaccel_register_t g_layersAccel;
    g_cmdShowLayers = plugin_register("command_id", (void*)"LT_LAYERS");
    if (!g_cmdShowLayers) return 0;

    memset(&g_layersAccel, 0, sizeof(g_layersAccel));
    g_layersAccel.desc      = "Live Tools: Layers - Show/Hide";
    g_layersAccel.accel.cmd = (WORD)g_cmdShowLayers;
    plugin_register("gaccel", &g_layersAccel);

    // ---- Register DCA command --------------------------------------------
    g_cmdShowDca = plugin_register("command_id", (void*)"LT_DCA_GROUPS");
    if (g_cmdShowDca)
    {
        static gaccel_register_t g_dcaAccel;
        memset(&g_dcaAccel, 0, sizeof(g_dcaAccel));
        g_dcaAccel.desc      = "Live Tools: DCA Groups - Show/Hide";
        g_dcaAccel.accel.cmd = (WORD)g_cmdShowDca;
        plugin_register("gaccel", &g_dcaAccel);
    }

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
    CSurfConfigDlg_SetInstance(hInstance);
    TransitionWnd_Init(hInstance);
    SafesWnd_Init(hInstance);
    LayoutsWnd_Init(hInstance);
    PaflWnd_Init(hInstance);
    MonitorWnd_Init(hInstance);
    MeterBridgeWnd_Init(hInstance);
    LiveOptimizeWnd_Init(hInstance);
    LiveLockWnd_Init(hInstance);
    LiveLockEngine_Init();
    TalkbackWnd_Init(hInstance);
    MuteGroupsWnd_Init(hInstance);
    LayersEngine_Init();
    LayersWnd_Init(hInstance);
    DcaWnd_Init(hInstance);
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
        AppendMenuA(hSub,  MF_STRING, (UINT_PTR)g_cmdShowMonitor, "Live Monitor...");
        AppendMenuA(hSub,  MF_STRING, (UINT_PTR)g_cmdShowMeterBridge, "Meter Bridge...");
        AppendMenuA(hSub,  MF_STRING, (UINT_PTR)g_cmdShowLiveLock,    "Live Lock...");
        AppendMenuA(hSub,  MF_STRING, (UINT_PTR)g_cmdShowMuteGroups,  "Mute Groups...");
        AppendMenuA(hSub,  MF_STRING, (UINT_PTR)g_cmdShowLayers,      "Layers...");
        AppendMenuA(hSub,  MF_STRING, (UINT_PTR)g_cmdShowDca,         "DCA Groups...");
        AppendMenuA(hSub,  MF_SEPARATOR, 0, nullptr);
        AppendMenuA(hSub,  MF_STRING, (UINT_PTR)g_cmdShowCSurf,   "Control Surface Settings...");
        AppendMenuA(hSub,  MF_STRING, (UINT_PTR)g_cmdShowPafl,    "PAFL Monitor...");
        AppendMenuA(hSub,  MF_STRING, (UINT_PTR)g_cmdShowTalkback,    "Talkback...");
        AppendMenuA(hSub,  MF_STRING, (UINT_PTR)g_cmdLiveOpt,     "Live Optimizer...");
        AppendMenuA(hMenu, MF_POPUP,  (UINT_PTR)hSub,             "Live Tools");
        s_hLiveToolsMenu = hSub;
    }
    else if (flag == 1 && s_hLiveToolsMenu)
    {
        // Update check states
        // Menu positions: 0=Scenes, 1=Layouts, 2=Monitor,
        //                  3=MeterBridge, 4=LiveLock, 5=MuteGroups,
        //                  6=Layers, 7=DCA Groups, 8=separator,
        //                  9=CSurf, 10=PAFL, 11=Talkback, 12=Optimizer
        CheckMenuItem(s_hLiveToolsMenu,  0, MF_BYPOSITION | (TransitionWnd_IsVisible()   ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(s_hLiveToolsMenu,  1, MF_BYPOSITION | (LayoutsWnd_IsVisible()      ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(s_hLiveToolsMenu,  2, MF_BYPOSITION | (MonitorWnd_IsVisible()      ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(s_hLiveToolsMenu,  3, MF_BYPOSITION | (MeterBridgeWnd_IsVisible()  ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(s_hLiveToolsMenu,  4, MF_BYPOSITION | (LiveLockWnd_IsVisible()     ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(s_hLiveToolsMenu,  5, MF_BYPOSITION | (MuteGroupsWnd_IsVisible()   ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(s_hLiveToolsMenu,  6, MF_BYPOSITION | (LayersWnd_IsVisible()       ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(s_hLiveToolsMenu,  7, MF_BYPOSITION | (DcaWnd_IsVisible()          ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(s_hLiveToolsMenu, 10, MF_BYPOSITION | (PaflWnd_IsVisible()         ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(s_hLiveToolsMenu, 11, MF_BYPOSITION | (TalkbackWnd_IsVisible()     ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(s_hLiveToolsMenu, 12, MF_BYPOSITION | (LiveOptimizeWnd_IsVisible() ? MF_CHECKED : MF_UNCHECKED));
    }
}
