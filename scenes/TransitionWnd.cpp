#include "TransitionWnd.h"
#include "TransitionEngine.h"
#include "ChunkRecallList.h"
#include "SafesWnd.h"
#include "LayersEngine.h"
#include "../layers/LayersWnd.h"
#include "api.h"
#include "resource.h"

#ifdef _WIN32
#  include <commctrl.h>
#  include <commdlg.h>
#  include <windowsx.h>
#endif
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <memory>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
std::vector<std::unique_ptr<TransitionSnapshot>> g_snapshots;

static HWND      g_wnd        = nullptr;
static HINSTANCE g_hInstance  = nullptr;
// Clipboard for copy/paste
static std::unique_ptr<TransitionSnapshot> g_clipboard;

// Index of the most recently created, recalled, or saved/overwritten scene.
// Backs the "Update last touched scene" action so a performer can re-capture
// whatever scene they just interacted with without reselecting it in the list.
static int g_lastTouchedIdx = -1;
static void MarkTouched(int idx)
{
    if (idx >= 0 && idx < (int)g_snapshots.size())
        g_lastTouchedIdx = idx;
}

// Guard: set true when programmatically updating editor fields to prevent
// EN_CHANGE / CBN_SELCHANGE from writing back to the snapshot.
static bool g_syncingEditor = false;

// Guard: when true, WM_DESTROY skips overwriting the dock-state pref (used by ToggleDocking)
static bool g_suppressDockStateSave = false;

// Per-project window state (loaded from LTSCENESWND line, applied in TransitionWnd_OnProjectLoad)
static bool s_hasSavedWndState = false;
static bool s_savedWndVisible  = false;
static bool s_savedWndDocked   = false;
static int  s_savedWndX = 0, s_savedWndY = 0, s_savedWndW = 0, s_savedWndH = 0;

// UI timer ID
static const UINT UI_TIMER_ID = 1;

// Context menu item IDs
enum { CTX_RENAME = 100, CTX_OVERWRITE, CTX_DELETE,
       CTX_NEW, CTX_RECALL_CTX, CTX_COPY_CTX, CTX_PASTE_CTX,
       CTX_EXPORT, CTX_IMPORT, CTX_ADDSPACER, CTX_SCENE_SETTINGS,
       CTX_CUE_REMOVE, CTX_DELETE_ALL };

// Docker context menu IDs
enum { CTX_DOCK = 200, CTX_CLOSE };

// Cue-list mode flag
static bool g_cueMode = false;

// Ordered cue list (each entry is an index into g_snapshots)
static std::vector<int> g_cueList;

// Whether to place a project marker at the play-cursor on each recall
static bool g_placeMarker = false;

// Stop transport before recall; restart recording after recall
static bool g_stopRecBeforeRecall = false;
static bool g_startRecAfterRecall = false;

// Scene list interaction settings
static bool g_singleClickRecall   = false;  // single click recalls a scene
static bool g_altClickDelete      = false;  // Alt+click deletes a scene
static bool g_ctrlClickOverwrite  = false;  // Ctrl+click overwrites a scene
// FX window setting (non-static so TransitionEngine.cpp can extern it)
bool g_preloadOffline             = false;  // keep FX windows open during recall
bool g_skipUnchangedParams        = false;  // skip writing params that haven't changed
bool g_durationDebug              = false;  // print step-timing report to REAPER console on recall
bool g_shadowParams               = false;  // maintain VST3 param shadow map for instant recall
bool g_chunkAllInstant            = false;  // capture+restore all plugins by chunk on instant path
bool g_recallLog                  = false;  // write a per-recall trace to live_tools_recall.log

// Global default transition settings for newly created scenes
static double g_defaultDuration = 0.0;
static int    g_defaultTaper    = TAPER_SCURVE;
static double g_defaultTaperExp = 2.0;

// Drag-drop state
static int        g_dragSrc     = -1;
static int        g_dragTarget  = -1;
#ifdef _WIN32
static HIMAGELIST g_hDragImages = nullptr;
#endif

// List subclass for reliable drag-drop mouse tracking
static WNDPROC s_origListProc  = nullptr;
static bool    s_lbTracking    = false;
static POINT   s_lbDownPt      = {};
static int     s_lbDownItem    = -1;
static DWORD   s_lbDownTime    = 0;   // tick count when LButton went down

// Cue dialog drag state (file-scope so CueLvSubclassProc can access it)
struct CueDragState {
    bool  active;    // drag confirmed and in progress
    bool  tracking;  // LButton held, awaiting threshold
    HWND  srcList;   // which list view the drag started from
    int   srcItem;   // index of the item being dragged
    POINT downPt;    // starting cursor position in srcList client coords
    DWORD downTime;  // GetTickCount() at mouse-down
};
static CueDragState  s_cueDrag       = {};
static WNDPROC       s_origCueLvProc = nullptr;
static HWND          s_cueLeft       = nullptr;   // left list view in IDD_CUE_SETUP
static HWND          s_cueRight      = nullptr;   // right list view in IDD_CUE_SETUP
static std::vector<int>* s_cueEditList = nullptr; // points to g_cueList during edit

// Set to true after NM_RCLICK shows our scene menu; tells WM_CONTEXTMENU to eat
// any queued WM_CONTEXTMENU that REAPER's hook may have already posted.
static bool    g_skipNextContextMenu = false;

// ---------------------------------------------------------------------------
// Layout / resize state
// ---------------------------------------------------------------------------
struct SidebarCtrl { HWND hwnd; int origLeft; int origTop; int w; int h; };
static std::vector<SidebarCtrl> g_sidebarCtrls;
static int  g_initCx = 0, g_initCy = 0;

// ---- Notes box resizer ----------------------------------------------------
// The notes box is last in the sidebar stack, and the sidebar keeps its height
// when the window grows — so everything below the notes box is free space, and
// IDC_NOTES_GRIP lets the user drag the box down into it. g_notesExtra is how
// many pixels past its default height the user has dragged it; it is saved
// with the rest of the window state.
static int     g_notesExtra     = 0;
static RECT    g_notesInitRect  = {};   // client coords, recorded at WM_INITDIALOG
static RECT    g_gripInitRect   = {};
static WNDPROC g_gripOldProc    = nullptr;
static bool    g_gripDragging   = false;
static int     g_gripDragY0     = 0;
static int     g_gripDragExtra0 = 0;
static void    LayoutNotes(HWND hwnd);

// Version footer. Unlike the rest of the sidebar it tracks the bottom of the
// client area rather than keeping its y, so it stays the last thing in the
// column at any window size.
static RECT    g_versionInitRect = {};
static void    LayoutVersion(HWND hwnd);

// ---- Column splitter ------------------------------------------------------
// g_splitOffset is how far the divider has been dragged from where the .rc
// puts it, in pixels; positive widens the scene list at the sidebar's expense.
// Saved with the window state. Sidebar controls are re-laid out proportionally
// within whatever width is left, so they track the divider.
static int     g_splitOffset      = 0;
static RECT    g_splitInitRect    = {};
static int     g_sbInitLeft       = 0;   // sidebar bounding box at design size
static int     g_sbInitRight      = 0;
// Controls above the scene list (the Scenes / Cue List toggles). They belong to
// the left column, so they follow the divider rather than the window edge —
// otherwise dragging the divider left leaves them overhanging the sidebar.
static std::vector<SidebarCtrl> g_leftCtrls;
static WNDPROC g_splitOldProc     = nullptr;
static bool    g_splitDragging    = false;
static int     g_splitDragX0      = 0;
static int     g_splitDragOffset0 = 0;
static void    LayoutMain(HWND hwnd);
static RECT g_listInitRect = {};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void ToggleDocking();
static void RefreshListView(HWND hwnd);
static void DoRecall(HWND hwnd, int index);
static void DoSave(HWND hwnd);
static void ShowContextMenu(HWND hwnd, int item, POINT pt);
static void LoadEditorFromSnapshot(HWND hwnd, TransitionSnapshot* snap);
static void ExportScene(HWND hwnd, int item);
static void ImportScene(HWND hwnd);
static void DoEndDrag(HWND hwnd);
static TransitionSnapshot* GetSelectedSnapshot(HWND hwnd);
static int  GetSelectedListIndex(HWND hwnd);
static LRESULT CALLBACK ListSubclassProc(HWND hList, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK CueLvSubclassProc(HWND hList, UINT msg, WPARAM wParam, LPARAM lParam);
static void RefillCueRightList(HWND hRight, const std::vector<int>& list);
static void RestoreLayerState(TransitionSnapshot* snap);
static void EnsureLayerUids(TransitionSnapshot* snap);
static void ResolveSceneLayer(TransitionSnapshot* snap);
static INT_PTR CALLBACK GlobalSettingsDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK CueSetupDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void TransitionWnd_Init(HINSTANCE hInstance)
{
    g_hInstance = hInstance;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
}

void TransitionWnd_Cleanup()
{
    if (g_wnd && IsWindow(g_wnd))
    {
        bool isFloat = false;
        if (DockIsChildOfDock(g_wnd, &isFloat) >= 0)
            DockWindowRemove(g_wnd);
        DestroyWindow(g_wnd);
        g_wnd = nullptr;
    }
}

// Create the window if it doesn't exist yet (per the saved dock pref), or bring
// it to front if it exists but isn't currently the visible one. Never hides/closes
// an already-visible window — that's ShowHide()'s toggle behaviour, not this helper's.
// Returns the window handle, or nullptr if creation failed.
static HWND EnsureWndOpen()
{
    if (!g_wnd || !IsWindow(g_wnd))
    {
        HWND hMain = GetMainHwnd();
        g_wnd = CreateDialogParam(g_hInstance,
                                  MAKEINTRESOURCE(IDD_TSNAPS),
                                  hMain,
                                  DialogProc,
                                  0);
        if (!g_wnd)
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "CreateDialogParam failed. Error=%lu", GetLastError());
            MessageBoxA(hMain, buf, "Live Tools", MB_OK | MB_ICONERROR);
            return nullptr;
        }
        // Always register with REAPER's docker so drag-to-edge works even when floating.
        // allowShow=true  → immediately reparent into the docker (docked mode)
        // allowShow=false → registered as dockable but stays as a regular float window
        const char* dockPref = GetExtState("reaper_transitions", "scenes_docked");
        bool wantDocked = (dockPref && atoi(dockPref) != 0);
        if (wantDocked)
        {
            DockWindowAddEx(g_wnd, "Scenes", "reaper_trans_scenes", true);
            DockWindowActivate(g_wnd);
        }
        else
        {
            ShowWindow(g_wnd, SW_SHOW);
        }
        return g_wnd;
    }

    bool isFloat = false;
    if (DockIsChildOfDock(g_wnd, &isFloat) >= 0)
    {
        if (!IsWindowVisible(g_wnd))
            DockWindowActivate(g_wnd);
    }
    else
    {
        if (!IsWindowVisible(g_wnd))
            ShowWindow(g_wnd, SW_SHOW);
    }
    return g_wnd;
}

void TransitionWnd_ShowHide()
{
    if (!g_wnd || !IsWindow(g_wnd))
    {
        EnsureWndOpen();
        return;
    }

    // Window already exists – toggle visibility whether docked or floating.
    bool isFloat = false;
    if (DockIsChildOfDock(g_wnd, &isFloat) >= 0)
    {
        // Docked: only the active tab's window is actually visible. If we're the
        // active tab, "toggle off" closes the window (same as the docked "x" /
        // right-click Close); otherwise bring our tab to front.
        if (IsWindowVisible(g_wnd))
            DestroyWindow(g_wnd);
        else
            DockWindowActivate(g_wnd);
    }
    else
    {
        if (IsWindowVisible(g_wnd))
            ShowWindow(g_wnd, SW_HIDE);
        else
            ShowWindow(g_wnd, SW_SHOW);
    }
}

// Destroy the window and recreate it with the opposite dock state (mirrors SWS ToggleDocking)
static void ToggleDocking()
{
    bool isFloat = false;
    bool wasDocked = (DockIsChildOfDock(g_wnd, &isFloat) >= 0);
    bool newDocked = !wasDocked;
    SetExtState("reaper_transitions", "scenes_docked", newDocked ? "1" : "0", true);
    g_suppressDockStateSave = true;  // prevent WM_DESTROY from overwriting the new pref
    DestroyWindow(g_wnd);
    g_suppressDockStateSave = false;
    // g_wnd is cleared by WM_DESTROY handler; recreate via ShowHide
    TransitionWnd_ShowHide();
}

int TransitionWnd_IsVisible()
{
    if (!g_wnd || !IsWindow(g_wnd)) return 0;
    bool isFloat = false;
    if (DockIsChildOfDock(g_wnd, &isFloat) >= 0) return 1;
    return IsWindowVisible(g_wnd) ? 1 : 0;
}

int TransitionWnd_GetSelectedIndex()
{
    if (!g_wnd || !IsWindow(g_wnd)) return -1;
    return GetSelectedListIndex(g_wnd);
}

void TransitionWnd_RefreshList()
{
    if (g_wnd && IsWindow(g_wnd))
        RefreshListView(g_wnd);
}

void TransitionWnd_RecallScene(int index)
{
    if (index < 0 || index >= (int)g_snapshots.size()) return;
    TransitionSnapshot* snap = g_snapshots[index].get();
    ResolveSceneLayer(snap);   // migrate pre-uid scenes; drop dangling references
    // m_duration == 0 means instant
    double duration = snap->m_duration;
    if (g_placeMarker)
    {
        double pos = GetPlayPosition();
        AddProjectMarker2(nullptr, false, pos, 0.0, snap->m_name.c_str(), -1, 0);
    }
    // Stop recording before recall (marker placed first to capture correct play position)
    if (g_stopRecBeforeRecall && (GetPlayState() & 4))
        Main_OnCommand(1016, 0);  // Stop transport

    // Strip TS_VIS when a layer is being recalled (layers manage visibility)
    int effectiveMask = snap->m_mask;
    if (!snap->m_layers.empty() && snap->m_layerUid > 0)
        effectiveMask &= ~TS_VIS;
    TransitionEngine::Get().Recall(snap, effectiveMask, duration);
    TransitionEngine::Get().SetCurrentSlot(index);
    MarkTouched(index);
    // Restore full layer state (always, unless TS_LAYERS safe bit is set)
    RestoreLayerState(snap);
    if (g_wnd && IsWindow(g_wnd))
    {
        HWND hList = GetDlgItem(g_wnd, IDC_LIST);
        ListView_SetItemState(hList, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hList, index, FALSE);
        // Repaint the sidebar so the "Layer:" readout matches what was just
        // recalled; nothing else refreshed it after a recall.
        LoadEditorFromSnapshot(g_wnd, snap);
    }
    LayersWnd_Refresh();
    Undo_OnStateChangeEx("Recall Scene", -1, -1);
    // Start recording after recall if enabled
    if (g_startRecAfterRecall)
        Main_OnCommand(1013, 0);  // Record
}

void TransitionWnd_OverwriteScene(int index)
{
    if (index < 0 || index >= (int)g_snapshots.size()) return;
    g_snapshots[index]->Capture(TS_CAPTURE_ALL);
    MarkTouched(index);
    if (g_wnd && IsWindow(g_wnd))
        RefreshListView(g_wnd);
    Undo_OnStateChangeEx("Save Scene", -1, -1);
}

// ---------------------------------------------------------------------------
// Headless action helpers – thin wrappers so keyboard/MIDI-bound REAPER actions
// can drive the scene list without the user having the window focused or even
// open. All open/create the window on demand since scene creation and inline
// rename need somewhere to put the ListView row.
// ---------------------------------------------------------------------------
void TransitionWnd_CreateNewScene()
{
    HWND hwnd = EnsureWndOpen();
    if (!hwnd) return;
    DoSave(hwnd);  // also drops into inline rename with the name field focused/selected
}

void TransitionWnd_RecallSelectedScene()
{
    int idx = TransitionWnd_GetSelectedIndex();
    if (idx < 0 || idx >= (int)g_snapshots.size() || g_snapshots[idx]->m_isSpacer) return;
    TransitionWnd_RecallScene(idx);
}

void TransitionWnd_UpdateSelectedScene()
{
    int idx = TransitionWnd_GetSelectedIndex();
    if (idx < 0 || idx >= (int)g_snapshots.size() || g_snapshots[idx]->m_isSpacer) return;
    TransitionWnd_OverwriteScene(idx);
}

void TransitionWnd_UpdateLastTouchedScene()
{
    if (g_lastTouchedIdx < 0 || g_lastTouchedIdx >= (int)g_snapshots.size()) return;
    if (g_snapshots[g_lastTouchedIdx]->m_isSpacer) return;
    TransitionWnd_OverwriteScene(g_lastTouchedIdx);
}

void TransitionWnd_RecallNextScene()
{
    if (g_snapshots.empty()) return;
    int next = TransitionEngine::Get().GetCurrentSlot() + 1;
    if (next < 0) next = 0;
    while (next < (int)g_snapshots.size() && g_snapshots[next]->m_isSpacer)
        ++next;
    if (next < (int)g_snapshots.size())
        TransitionWnd_RecallScene(next);
}

// ---------------------------------------------------------------------------
// Per-project window state restore  (called from BeginLoadProjectState)
// ---------------------------------------------------------------------------
void TransitionWnd_OnProjectLoad()
{
    if (!s_hasSavedWndState) return;  // blank / pre-v0.0.14 project – leave as-is

    if (s_savedWndVisible)
    {
        bool wndOpen = (g_wnd && IsWindow(g_wnd));
        if (!wndOpen)
        {
            // Update the global dock preference so ShowHide creates the window correctly.
            SetExtState("reaper_transitions", "scenes_docked",
                        s_savedWndDocked ? "1" : "0", true);
            TransitionWnd_ShowHide();
        }
        else
        {
            // Window already exists – correct dock state if it doesn't match.
            bool isFloat = false;
            bool curDocked = (DockIsChildOfDock(g_wnd, &isFloat) >= 0);
            if (curDocked != s_savedWndDocked)
                ToggleDocking();
        }
        // Restore floating position/size (no-op when docked – docker manages geometry).
        if (!s_savedWndDocked && g_wnd && IsWindow(g_wnd) && s_savedWndW > 0 && s_savedWndH > 0)
            SetWindowPos(g_wnd, nullptr, s_savedWndX, s_savedWndY,
                         s_savedWndW, s_savedWndH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    else
    {
        // Project saved with window closed – hide it if it's currently floating and visible.
        if (g_wnd && IsWindow(g_wnd))
        {
            bool isFloat = false;
            bool curDocked = (DockIsChildOfDock(g_wnd, &isFloat) >= 0);
            if (!curDocked && IsWindowVisible(g_wnd))
                ShowWindow(g_wnd, SW_HIDE);
        }
    }
}

// ---------------------------------------------------------------------------
// Cue list persistence (called from reaper_transitions.cpp project-state hooks)
// ---------------------------------------------------------------------------
void TransitionWnd_ResetCueList()
{
    g_cueList.clear();
}

void TransitionWnd_SaveCueList(ProjectStateContext* ctx)
{
    if (g_cueList.empty()) return;
    std::string line = "TSCUELIST";
    for (int idx : g_cueList)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), " %d", idx);
        line += buf;
    }
    ctx->AddLine("%s", line.c_str());
}

bool TransitionWnd_LoadCueListLine(const char* line)
{
    if (!line || strncmp(line, "TSCUELIST", 9) != 0) return false;
    g_cueList.clear();
    const char* p = line + 9;
    int idx, consumed;
    while (*p && sscanf(p, " %d%n", &idx, &consumed) == 1)
    {
        g_cueList.push_back(idx);
        p += consumed;
    }
    if (g_wnd && IsWindow(g_wnd))
        RefreshListView(g_wnd);
    return true;
}

// ---------------------------------------------------------------------------
// Default transition settings – project-specific persistence
// ---------------------------------------------------------------------------
static bool s_chunkPluginsLoadedFromProject = false;

void TransitionWnd_ResetSettings()
{
    g_defaultDuration    = 0.0;
    g_defaultTaper       = TAPER_SCURVE;
    g_defaultTaperExp    = 2.0;
    g_placeMarker        = false;
    g_stopRecBeforeRecall = false;
    g_startRecAfterRecall = false;
    g_singleClickRecall  = false;
    g_altClickDelete     = false;
    g_ctrlClickOverwrite = false;
    g_preloadOffline     = false;
    g_skipUnchangedParams = false;
    g_shadowParams        = false;
    g_chunkAllInstant     = false;
    g_recallLog           = false;
    g_chunkRecallKeywords = GetChunkRecallDefaults();  // restore defaults on project reset
    g_chunkRecallNotify   = false;
    s_chunkPluginsLoadedFromProject = false;  // allow project list to replace defaults
    s_hasSavedWndState = false;               // no per-project window state for this load
}

bool TransitionWnd_ProcessSettingsLine(const char* line)
{
    if (!line) return false;
    if (strncmp(line, "LTDEFSETTINGS ", 14) == 0)
    {
        double dur = 2.0, taperExp = 2.0;
        int taper = TAPER_SCURVE, marker = 0;
        int singleClick = 0, altDelete = 0, ctrlOverwrite = 0;
        sscanf(line + 14, "%lf %d %lf %d %d %d %d",
               &dur, &taper, &taperExp, &marker,
               &singleClick, &altDelete, &ctrlOverwrite);
        g_defaultDuration    = (dur >= 0.0) ? dur : 2.0;
        g_defaultTaper       = (taper >= 0 && taper <= TAPER_CUSTOM) ? taper : TAPER_SCURVE;
        g_defaultTaperExp    = (taperExp > 0.0) ? taperExp : 2.0;
        g_placeMarker        = (marker != 0);
        g_singleClickRecall  = (singleClick != 0);
        g_altClickDelete     = (altDelete != 0);
        g_ctrlClickOverwrite = (ctrlOverwrite != 0);
        return true;
    }
    if (strncmp(line, "LTPRELOADOFFLINE ", 17) == 0)
    {
        int val = 0;
        sscanf(line + 17, "%d", &val);
        g_preloadOffline = (val != 0);
        return true;
    }
    if (strncmp(line, "LTSKIPUNCHANGED ", 16) == 0)
    {
        int val = 0;
        sscanf(line + 16, "%d", &val);
        g_skipUnchangedParams = (val != 0);
        return true;
    }
    if (strncmp(line, "LTSHADOWPARAMS ", 15) == 0)
    {
        int val = 0;
        sscanf(line + 15, "%d", &val);
        g_shadowParams = (val != 0);
        return true;
    }
    if (strncmp(line, "LTCHUNKALLINSTANT ", 18) == 0)
    {
        int val = 0;
        sscanf(line + 18, "%d", &val);
        g_chunkAllInstant = (val != 0);
        return true;
    }
    if (strncmp(line, "LTRECALLLOG ", 12) == 0)
    {
        int val = 0;
        sscanf(line + 12, "%d", &val);
        g_recallLog = (val != 0);
        return true;
    }
    if (strncmp(line, "LTCHUNKPLUGIN ", 14) == 0)
    {
        const char* kw = line + 14;
        if (kw[0])
        {
            if (!s_chunkPluginsLoadedFromProject)
            {
                // First LTCHUNKPLUGIN from the project: replace defaults with the saved list.
                g_chunkRecallKeywords.clear();
                s_chunkPluginsLoadedFromProject = true;
            }
            g_chunkRecallKeywords.push_back(kw);
        }
        return true;
    }
    if (strncmp(line, "LTCHUNKNOTIFY ", 14) == 0)
    {
        int val = 0;
        sscanf(line + 14, "%d", &val);
        g_chunkRecallNotify = (val != 0);
        return true;
    }
    if (strncmp(line, "LTDURATIONDEBUG ", 16) == 0)
    {
        int val = 0;
        sscanf(line + 16, "%d", &val);
        g_durationDebug = (val != 0);
        return true;
    }
    if (strncmp(line, "LTRECORDER ", 11) == 0)
    {
        int stopBefore = 0, startAfter = 0;
        sscanf(line + 11, "%d %d", &stopBefore, &startAfter);
        g_stopRecBeforeRecall = (stopBefore != 0);
        g_startRecAfterRecall = (startAfter != 0);
        return true;
    }
    if (strncmp(line, "LTSCENESWND ", 12) == 0)
    {
        int docked = 0, visible = 0, x = 0, y = 0, w = 500, h = 400;
        int notesExtra = 0, splitOffset = 0;
        sscanf(line + 12, "%d %d %d %d %d %d %d %d",
               &docked, &visible, &x, &y, &w, &h, &notesExtra, &splitOffset);
        g_notesExtra  = (notesExtra > 0) ? notesExtra : 0;  // clamped by LayoutNotes
        g_splitOffset = splitOffset;                        // clamped by LayoutMain
        s_savedWndDocked  = (docked  != 0);
        s_savedWndVisible = (visible != 0);
        s_savedWndX = x; s_savedWndY = y;
        s_savedWndW = (w > 0) ? w : 500;
        s_savedWndH = (h > 0) ? h : 400;
        s_hasSavedWndState = true;
        // Project lines are all available now — restore window state immediately.
        TransitionWnd_OnProjectLoad();
        return true;
    }
    return false;
}

void TransitionWnd_SaveSettings(ProjectStateContext* ctx)
{
    ctx->AddLine("LTDEFSETTINGS %.4f %d %.4f %d %d %d %d",
                 g_defaultDuration, g_defaultTaper,
                 g_defaultTaperExp, g_placeMarker ? 1 : 0,
                 g_singleClickRecall ? 1 : 0,
                 g_altClickDelete ? 1 : 0,
                 g_ctrlClickOverwrite ? 1 : 0);
    ctx->AddLine("LTPRELOADOFFLINE %d", g_preloadOffline ? 1 : 0);
    ctx->AddLine("LTSKIPUNCHANGED %d", g_skipUnchangedParams ? 1 : 0);
    ctx->AddLine("LTSHADOWPARAMS %d", g_shadowParams ? 1 : 0);
    ctx->AddLine("LTCHUNKALLINSTANT %d", g_chunkAllInstant ? 1 : 0);
    ctx->AddLine("LTRECALLLOG %d", g_recallLog ? 1 : 0);
    for (const auto& kw : g_chunkRecallKeywords)
        ctx->AddLine("LTCHUNKPLUGIN %s", kw.c_str());
    ctx->AddLine("LTCHUNKNOTIFY %d", g_chunkRecallNotify ? 1 : 0);
    ctx->AddLine("LTDURATIONDEBUG %d", g_durationDebug ? 1 : 0);
    ctx->AddLine("LTRECORDER %d %d", g_stopRecBeforeRecall ? 1 : 0, g_startRecAfterRecall ? 1 : 0);

    // Per-project window state – snapshot dock/float/rect so reloading the project
    // restores exactly what the user had open.
    {
        bool wndVisible = (TransitionWnd_IsVisible() != 0);
        bool wndDocked  = false;
        int  wx = 0, wy = 0, ww = 500, wh = 400;
        if (g_wnd && IsWindow(g_wnd))
        {
            bool isFloat = false;
            wndDocked = (DockIsChildOfDock(g_wnd, &isFloat) >= 0);
            if (!wndDocked)
            {
                RECT r = {};
                GetWindowRect(g_wnd, &r);
                wx = r.left; wy = r.top;
                ww = r.right  - r.left;
                wh = r.bottom - r.top;
            }
        }
        else if (s_hasSavedWndState)
        {
            // Window is currently closed – re-emit last loaded values so the rect
            // is preserved even though visible=0.
            wndVisible = false;
            wndDocked  = s_savedWndDocked;
            wx = s_savedWndX; wy = s_savedWndY;
            ww = s_savedWndW; wh = s_savedWndH;
        }
        // Trailing fields are optional on read, so older builds still parse
        // this line and simply ignore them.
        ctx->AddLine("LTSCENESWND %d %d %d %d %d %d %d %d",
                     wndDocked ? 1 : 0, wndVisible ? 1 : 0,
                     wx, wy, ww, wh, g_notesExtra, g_splitOffset);
    }
}

// ---------------------------------------------------------------------------
// NotesAccel – keyboard hook so Return reaches the notes box.
//
// ES_WANTRETURN tells the edit control to keep Return, but REAPER sits ahead
// of the dialog in the keyboard queue and, for a docked window, will run
// whatever action Return is bound to before the control ever sees it. This
// hook claims Return (and keypad Enter) only while focus is actually inside
// the notes box, and passes everything else through untouched.
// ---------------------------------------------------------------------------
static accelerator_register_t g_notesAccel;
static bool                   g_notesAccelRegistered = false;

static int NotesTranslateAccel(MSG* msg, accelerator_register_t* /*ctx*/)
{
    if (!msg || !g_wnd || !IsWindow(g_wnd)) return 0;
    if (msg->message != WM_KEYDOWN && msg->message != WM_KEYUP) return 0;
    if (msg->wParam != VK_RETURN) return 0;

    HWND hNotes = GetDlgItem(g_wnd, IDC_SNAPNOTES);
    if (!hNotes || GetFocus() != hNotes) return 0;

    return -1;   // pass it to the control; ES_WANTRETURN turns it into a break
}

// ---------------------------------------------------------------------------
// Notes line-ending helpers. Notes are stored with bare '\n', but a Win32
// multiline edit needs "\r\n" to render a line break and hands "\r\n" back.
// ---------------------------------------------------------------------------
static std::string NotesToControl(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\r') continue;
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

static std::string NotesFromControl(const char* s)
{
    std::string out;
    if (!s) return out;
    for (; *s; s++)
        if (*s != '\r') out += *s;
    return out;
}

// ---------------------------------------------------------------------------
// NotesMaxExtra – how far the notes box may still be dragged down.
// The sidebar controls keep their height on resize, so the gap between the
// grip and the bottom of the client area is free space the box can claim. The
// same margin the grip sits at by default is kept below it.
// ---------------------------------------------------------------------------
static int NotesMaxExtra(HWND hwnd)
{
    if (g_initCy <= 0 || g_gripInitRect.bottom <= g_gripInitRect.top) return 0;
    if (g_versionInitRect.bottom <= g_versionInitRect.top) return 0;

    RECT cr;
    GetClientRect(hwnd, &cr);

    // Stop just above wherever LayoutVersion has pinned the footer, rather
    // than reserving the whole gap that happens to exist at the design size —
    // that gap is exactly the room the box is meant to be able to claim.
    int verH   = g_versionInitRect.bottom - g_versionInitRect.top;
    int verGap = g_initCy - g_versionInitRect.bottom;   // gap below the footer
    int verTop = cr.bottom - verGap - verH;
    int pad    = verH / 3;                              // breathing room, DPI-scaled

    int baseH = g_notesInitRect.bottom - g_notesInitRect.top;
    int gripH = g_gripInitRect.bottom  - g_gripInitRect.top;
    int avail = (verTop - pad) - (g_notesInitRect.top + baseH + gripH);
    return avail > 0 ? avail : 0;
}

// ---------------------------------------------------------------------------
// LayoutNotes – apply g_notesExtra to the notes box and reseat the grip.
// Runs after the WM_SIZE sidebar pass, which has already put both controls
// back at their default height, so x/width are read from the live controls.
// ---------------------------------------------------------------------------
static void LayoutNotes(HWND hwnd)
{
    HWND hNotes = GetDlgItem(hwnd, IDC_SNAPNOTES);
    HWND hGrip  = GetDlgItem(hwnd, IDC_NOTES_GRIP);
    if (!hNotes || !hGrip) return;
    if (g_notesInitRect.bottom <= g_notesInitRect.top) return;

    int maxExtra = NotesMaxExtra(hwnd);
    if (g_notesExtra > maxExtra) g_notesExtra = maxExtra;
    if (g_notesExtra < 0)        g_notesExtra = 0;

    RECT nr, gr;
    GetWindowRect(hNotes, &nr); MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&nr, 2);
    GetWindowRect(hGrip,  &gr); MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&gr, 2);

    int baseH = g_notesInitRect.bottom - g_notesInitRect.top;
    int gripH = g_gripInitRect.bottom  - g_gripInitRect.top;
    int h     = baseH + g_notesExtra;

    SetWindowPos(hNotes, nullptr, nr.left, g_notesInitRect.top,
                 nr.right - nr.left, h, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(hGrip, nullptr, gr.left, g_notesInitRect.top + h,
                 gr.right - gr.left, gripH, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// LayoutVersion – keep the version footer pinned to the bottom of the sidebar.
// The WM_SIZE sidebar pass restores its original y, so this runs after it.
// ---------------------------------------------------------------------------
static void LayoutVersion(HWND hwnd)
{
    HWND hVer = GetDlgItem(hwnd, IDC_VERSION);
    if (!hVer || g_initCy <= 0) return;
    if (g_versionInitRect.bottom <= g_versionInitRect.top) return;

    RECT cr;
    GetClientRect(hwnd, &cr);

    RECT vr;
    GetWindowRect(hVer, &vr);
    MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&vr, 2);

    int h      = g_versionInitRect.bottom - g_versionInitRect.top;
    int margin = g_initCy - g_versionInitRect.bottom;   // bottom gap at default size
    int top    = cr.bottom - margin - h;
    if (top < g_versionInitRect.top) top = g_versionInitRect.top;

    SetWindowPos(hVer, nullptr, vr.left, top, vr.right - vr.left, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// LayoutMain – position the list, the divider and the sidebar for the current
// client size and divider offset. Replaces the old WM_SIZE pass, which shifted
// the sidebar by dx and left its width alone; the sidebar now has to follow
// the divider, so each control is placed proportionally within it.
// ---------------------------------------------------------------------------
static void LayoutMain(HWND hwnd)
{
    if (g_initCx <= 0 || g_sbInitRight <= g_sbInitLeft) return;

    RECT cr;
    GetClientRect(hwnd, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) return;

    int dx = cr.right  - g_initCx;
    int dy = cr.bottom - g_initCy;

    int listLeft  = g_listInitRect.left;
    int gap       = g_sbInitLeft - g_listInitRect.right;    // divider gutter
    int rightEdge = cr.right - (g_initCx - g_sbInitRight);  // sidebar right margin
    int sbInitW   = g_sbInitRight - g_sbInitLeft;

    // Keep both columns usable no matter where the divider is dragged.
    const int minList = (g_listInitRect.right - g_listInitRect.left) / 3;
    const int minSb   = sbInitW / 2;

    int divider = g_listInitRect.right + dx + g_splitOffset;
    if (divider < listLeft + minList)      divider = listLeft + minList;
    if (divider > rightEdge - gap - minSb) divider = rightEdge - gap - minSb;
    g_splitOffset = divider - g_listInitRect.right - dx;    // clamp back

    // ---- Scene list -------------------------------------------------------
    HWND hList = GetDlgItem(hwnd, IDC_LIST);
    if (hList && g_listInitRect.right > g_listInitRect.left)
    {
        int newW = divider - listLeft;
        int newH = (g_listInitRect.bottom - g_listInitRect.top) + dy;
        if (newW > 10 && newH > 10)
        {
            SetWindowPos(hList, nullptr, listLeft, g_listInitRect.top, newW, newH,
                         SWP_NOZORDER | SWP_NOACTIVATE);

            // Stretch "Name" to fill whatever the other two columns leave.
            int col0W = ListView_GetColumnWidth(hList, 0);
            int col2W = ListView_GetColumnWidth(hList, 2);
            int col1W = newW - col0W - col2W - GetSystemMetrics(SM_CXVSCROLL) - 4;
            if (col1W > 20) ListView_SetColumnWidth(hList, 1, col1W);
        }
    }

    // ---- Left-column controls above the list ------------------------------
    int listInitW = g_listInitRect.right - g_listInitRect.left;
    int listW     = divider - listLeft;
    if (listInitW > 0 && listW > 0 && !g_leftCtrls.empty())
    {
        HDWP hdwp = BeginDeferWindowPos((int)g_leftCtrls.size());
        for (const auto& lc : g_leftCtrls)
        {
            int l = listLeft + MulDiv(lc.origLeft - listLeft, listW, listInitW);
            int w = MulDiv(lc.w, listW, listInitW);
            if (w < 1) w = 1;
            hdwp = DeferWindowPos(hdwp, lc.hwnd, nullptr, l, lc.origTop, w, lc.h,
                                  SWP_NOZORDER | SWP_NOACTIVATE);
        }
        EndDeferWindowPos(hdwp);
    }

    // ---- Divider ----------------------------------------------------------
    HWND hSplit = GetDlgItem(hwnd, IDC_SPLITTER);
    if (hSplit && g_splitInitRect.right > g_splitInitRect.left)
    {
        SetWindowPos(hSplit, nullptr,
                     divider, g_splitInitRect.top,
                     g_splitInitRect.right - g_splitInitRect.left,
                     (g_splitInitRect.bottom - g_splitInitRect.top) + dy,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // ---- Sidebar ----------------------------------------------------------
    int sbLeft = divider + gap;
    int sbW    = rightEdge - sbLeft;
    if (sbW > 0 && sbInitW > 0 && !g_sidebarCtrls.empty())
    {
        HDWP hdwp = BeginDeferWindowPos((int)g_sidebarCtrls.size());
        for (const auto& sc : g_sidebarCtrls)
        {
            // Map each control's slot in the design-size sidebar onto the
            // sidebar's current width, so multi-control rows keep their
            // proportions and their gutters.
            int l = sbLeft + MulDiv(sc.origLeft - g_sbInitLeft, sbW, sbInitW);
            int w = MulDiv(sc.w, sbW, sbInitW);
            if (w < 1) w = 1;
            hdwp = DeferWindowPos(hdwp, sc.hwnd, nullptr, l, sc.origTop, w, sc.h,
                                  SWP_NOZORDER | SWP_NOACTIVATE);
        }
        EndDeferWindowPos(hdwp);
    }

    // Both of these depend on the widths just assigned above.
    LayoutVersion(hwnd);
    LayoutNotes(hwnd);

    InvalidateRect(hwnd, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// SplitterProc – subclass for IDC_SPLITTER: drag the column divider.
// ---------------------------------------------------------------------------
static LRESULT CALLBACK SplitterProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
        return TRUE;

    case WM_LBUTTONDOWN:
    {
        POINT pt;
        GetCursorPos(&pt);
        g_splitDragging    = true;
        g_splitDragX0      = pt.x;
        g_splitDragOffset0 = g_splitOffset;
        SetCapture(h);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g_splitDragging)
        {
            POINT pt;
            GetCursorPos(&pt);
            g_splitOffset = g_splitDragOffset0 + (pt.x - g_splitDragX0);
            LayoutMain(GetParent(h));
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_splitDragging)
        {
            g_splitDragging = false;
            ReleaseCapture();
            MarkProjectDirty(nullptr);   // divider position is window state
        }
        return 0;

    case WM_CAPTURECHANGED:
        g_splitDragging = false;
        return 0;
    }
    return CallWindowProc(g_splitOldProc, h, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// NotesGripProc – subclass for IDC_NOTES_GRIP: drag the notes box taller.
// ---------------------------------------------------------------------------
static LRESULT CALLBACK NotesGripProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_SIZENS));
        return TRUE;

    case WM_LBUTTONDOWN:
    {
        POINT pt;
        GetCursorPos(&pt);
        g_gripDragging   = true;
        g_gripDragY0     = pt.y;
        g_gripDragExtra0 = g_notesExtra;
        SetCapture(h);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g_gripDragging)
        {
            POINT pt;
            GetCursorPos(&pt);
            g_notesExtra = g_gripDragExtra0 + (pt.y - g_gripDragY0);
            LayoutNotes(GetParent(h));
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_gripDragging)
        {
            g_gripDragging = false;
            ReleaseCapture();
            MarkProjectDirty(nullptr);   // the new height is saved with the window state
        }
        return 0;

    case WM_CAPTURECHANGED:
        g_gripDragging = false;
        return 0;
    }
    return CallWindowProc(g_gripOldProc, h, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// GetSelectedListIndex
// ---------------------------------------------------------------------------
static int GetSelectedListIndex(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LIST);
    if (!hList) return -1;
    return ListView_GetNextItem(hList, -1, LVNI_SELECTED);
}

// ---------------------------------------------------------------------------
// GetSelectedSnapshot
// ---------------------------------------------------------------------------
static TransitionSnapshot* GetSelectedSnapshot(HWND hwnd)
{
    int idx = GetSelectedListIndex(hwnd);
    if (idx < 0 || idx >= (int)g_snapshots.size()) return nullptr;
    return g_snapshots[idx].get();
}

// ---------------------------------------------------------------------------
// CaptureLayersFromEngine – refresh a scene's stored layer set from the live
// LayersEngine, leaving every other captured value alone. Layer definitions
// are not performance state, so re-reading them is safe outside a full
// Overwrite; this is what keeps a scene able to recall a layer created after
// the scene was saved.
// ---------------------------------------------------------------------------
static void CaptureLayersFromEngine(TransitionSnapshot* snap)
{
    if (!snap) return;
    LayersEngine& le = LayersEngine::Get();
    snap->m_layers.clear();
    for (int li = 0; li < le.GetLayerCount(); li++)
    {
        const LayerDef& ld = le.GetLayer(li);
        CapturedLayer cl;
        cl.name        = ld.name;
        cl.maxChannels = ld.maxChannels;
        cl.uid         = ld.uid;
        for (const LayerTrack& lt : ld.tracks)
        {
            CapturedLayerTrack clt;
            clt.guid     = lt.guid;
            clt.isSpacer = lt.isSpacer;
            cl.tracks.push_back(clt);
        }
        snap->m_layers.push_back(cl);
    }
}

// ---------------------------------------------------------------------------
// EnsureLayerUids – migrate a scene saved before layer uids existed. Every
// captured layer gets a uid minted from the engine's counter, and the old
// index-based m_layerIdx is resolved to the uid it pointed at, once. After
// this the scene is referenced by uid only and survives renames/reordering.
// ---------------------------------------------------------------------------
static void EnsureLayerUids(TransitionSnapshot* snap)
{
    if (!snap || snap->m_layers.empty()) return;

    LayersEngine& le = LayersEngine::Get();
    bool changed = false;

    // Uids already handed out within this scene, so two captured layers can
    // never adopt the same one.
    std::vector<int> taken;
    for (const auto& cl : snap->m_layers)
        if (cl.uid > 0) taken.push_back(cl.uid);

    for (int i = 0; i < (int)snap->m_layers.size(); i++)
    {
        CapturedLayer& cl = snap->m_layers[i];
        if (cl.uid > 0) continue;

        // Adopt the uid of the live layer this capture refers to rather than
        // minting a fresh one. A minted uid matches nothing in the live list,
        // which is what made every layer of a pre-uid scene show up as "not
        // in Layers". Match on name first — that is how the user identifies a
        // layer — then fall back to the position it was captured at.
        int live = -1;
        for (int li = 0; li < le.GetLayerCount() && live < 0; li++)
            if (strcmp(le.GetLayer(li).name, cl.name.c_str()) == 0) live = li;
        if (live < 0 && i < le.GetLayerCount()) live = i;

        int uid = (live >= 0) ? le.GetLayerUid(live) : 0;
        if (uid > 0 && std::find(taken.begin(), taken.end(), uid) != taken.end())
            uid = 0;                       // already claimed by an earlier layer
        if (uid <= 0) uid = le.AllocUid();  // genuinely has no counterpart

        cl.uid = uid;
        taken.push_back(uid);
        changed = true;
    }

    if (snap->m_layerUid <= 0 &&
        snap->m_layerIdx >= 0 && snap->m_layerIdx < (int)snap->m_layers.size())
    {
        snap->m_layerUid = snap->m_layers[snap->m_layerIdx].uid;
        changed = true;
    }
    if (changed)
    {
        // AllocUid advanced the engine's uid counter. Persist it now: if the
        // counter reverted, a later new layer would be handed a uid this scene
        // already claims, and recall would resolve to the wrong layer.
        LayersEngine::Get().SaveExtState();
        MarkProjectDirty(nullptr);
    }
}

// ---------------------------------------------------------------------------
// FindCapturedLayerByUid – index into snap->m_layers, or -1
// ---------------------------------------------------------------------------
static int FindCapturedLayerByUid(const TransitionSnapshot* snap, int uid)
{
    if (!snap || uid <= 0) return -1;
    for (int i = 0; i < (int)snap->m_layers.size(); i++)
        if (snap->m_layers[i].uid == uid) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// ResolveSceneLayer – point the scene at a layer that actually exists.
// A scene whose layer was deleted falls back to the first layer rather than
// keeping a dangling reference. "(no layer recall)" (uid 0) is a deliberate
// choice and is left alone.
// ---------------------------------------------------------------------------
static void ResolveSceneLayer(TransitionSnapshot* snap)
{
    if (!snap || snap->m_isSpacer) return;
    EnsureLayerUids(snap);
    if (snap->m_layerUid <= 0) return;

    LayersEngine& le = LayersEngine::Get();
    if (le.FindLayerByUid(snap->m_layerUid) >= 0) return;   // still there

    // No layers loaded at all is not evidence the scene's layer was deleted —
    // it is what the world looks like before the project's layers arrive.
    // Clearing the reference here would wipe every scene's assignment.
    if (le.GetLayerCount() <= 0) return;

    snap->m_layerUid = le.GetLayerUid(0);

    // The replacement may be newer than the scene's captured layer set, in
    // which case recall would have nothing to apply for it.
    if (snap->m_layerUid > 0 && FindCapturedLayerByUid(snap, snap->m_layerUid) < 0)
        CaptureLayersFromEngine(snap);

    snap->m_layerIdx = FindCapturedLayerByUid(snap, snap->m_layerUid);
    MarkProjectDirty(nullptr);
}

// ---------------------------------------------------------------------------
// LoadEditorFromSnapshot – fill right-panel controls from a snapshot
// ---------------------------------------------------------------------------
static void LoadEditorFromSnapshot(HWND hwnd, TransitionSnapshot* snap)
{
    g_syncingEditor = true;

    // Transition settings and notes are in the per-scene Settings popup.
    SetDlgItemText(hwnd, IDC_SNAPNOTES,
                   snap ? NotesToControl(snap->m_notes).c_str() : "");

    // Per-scene layer selector. This lists exactly the layers that exist right
    // now — never the scene's captured copy, which is what used to hide layers
    // created after the scene was saved. Each entry carries its layer uid as
    // item data, so the selection survives renames and reordering.
    {
        HWND hCb = GetDlgItem(hwnd, IDC_SNAP_LAYER);
        g_syncingEditor = true;  // keep the guard while filling combobox
        SendMessage(hCb, CB_RESETCONTENT, 0, 0);
        int noneItem = (int)SendMessage(hCb, CB_ADDSTRING, 0, (LPARAM)"(no layer recall)");
        SendMessage(hCb, CB_SETITEMDATA, (WPARAM)noneItem, (LPARAM)0);
        if (snap && !snap->m_isSpacer)
        {
            ResolveSceneLayer(snap);

            LayersEngine& le = LayersEngine::Get();
            int sel = 0;

            for (int li = 0; li < le.GetLayerCount(); li++)
            {
                const LayerDef& ld = le.GetLayer(li);
                int item = (int)SendMessage(hCb, CB_ADDSTRING, 0, (LPARAM)ld.name);
                SendMessage(hCb, CB_SETITEMDATA, (WPARAM)item, (LPARAM)ld.uid);
                if (ld.uid > 0 && ld.uid == snap->m_layerUid) sel = item;
            }

            SendMessage(hCb, CB_SETCURSEL, (WPARAM)sel, 0);
            EnableWindow(hCb, TRUE);
        }
        else
        {
            SendMessage(hCb, CB_SETCURSEL, 0, 0);
            EnableWindow(hCb, FALSE);
        }
    }

    // Update current layer indicator
    {
        char layerBuf[128] = "Layer: -";
        int activeLyr = LayersEngine::Get().GetActiveLayer();
        if (activeLyr >= 0 && activeLyr < LayersEngine::Get().GetLayerCount())
            snprintf(layerBuf, sizeof(layerBuf), "Layer: %s",
                     LayersEngine::Get().GetLayer(activeLyr).name);
        SetDlgItemText(hwnd, IDC_LAYER_STATUS, layerBuf);
    }

    g_syncingEditor = false;
}

// ---------------------------------------------------------------------------
// RefreshListView – rebuild all rows from g_snapshots (scenes mode)
//                   or from g_cueList (cue mode)
// ---------------------------------------------------------------------------
static void RefreshListView(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LIST);
    if (!hList) return;

    int selBefore = GetSelectedListIndex(hwnd);

    ListView_DeleteAllItems(hList);

    if (g_cueMode)
    {
        // Remove stale indices (scenes that were deleted)
        g_cueList.erase(
            std::remove_if(g_cueList.begin(), g_cueList.end(), [](int idx) {
                if (idx == -1) return false; // keep cue spacers
                return idx < 0 || idx >= (int)g_snapshots.size()
                    || g_snapshots[idx]->m_isSpacer;
            }), g_cueList.end());

        for (int ci = 0; ci < (int)g_cueList.size(); ci++)
        {
            int snapIdx = g_cueList[ci];

            LVITEM lvi = {};
            lvi.mask = LVIF_TEXT;
            lvi.iItem = ci;

            if (snapIdx == -1)
            {
                // Cue spacer row
                lvi.pszText = const_cast<char*>("---");
                ListView_InsertItem(hList, &lvi);
                ListView_SetItemText(hList, ci, 1, const_cast<char*>("---"));
                ListView_SetItemText(hList, ci, 2, const_cast<char*>(""));
                continue;
            }

            const auto& ss = g_snapshots[snapIdx];

            char slotBuf[16];
            snprintf(slotBuf, sizeof(slotBuf), "%d", ci + 1);
            lvi.pszText = slotBuf;
            ListView_InsertItem(hList, &lvi);
            ListView_SetItemText(hList, ci, 1, const_cast<char*>(ss->m_name.c_str()));

            char origBuf[16];
            snprintf(origBuf, sizeof(origBuf), "S%d", snapIdx + 1);
            ListView_SetItemText(hList, ci, 2, origBuf);
        }

        int listSize = (int)g_cueList.size();
        if (selBefore >= 0 && selBefore < listSize)
        {
            ListView_SetItemState(hList, selBefore,
                LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(hList, selBefore, FALSE);
        }
        return;
    }

    // --- Scenes mode ---
    int sceneNum = 1;  // running scene number (spacers don't count)
    for (int i = 0; i < (int)g_snapshots.size(); i++)
    {
        const auto& ss = g_snapshots[i];

        LVITEM lvi = {};
        lvi.mask  = LVIF_TEXT;
        lvi.iItem = i;

        if (ss->m_isSpacer)
        {
            lvi.pszText = const_cast<char*>("");
            ListView_InsertItem(hList, &lvi);
            ListView_SetItemText(hList, i, 1, const_cast<char*>("  \xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"));
            ListView_SetItemText(hList, i, 2, const_cast<char*>(""));
        }
        else
        {
            char slotBuf[16];
            snprintf(slotBuf, sizeof(slotBuf), "%d", sceneNum++);
            lvi.pszText = slotBuf;
            ListView_InsertItem(hList, &lvi);

            ListView_SetItemText(hList, i, 1, const_cast<char*>(ss->m_name.c_str()));

            char timeBuf[32] = "";
            if (ss->m_time)
            {
                struct tm* lt = localtime((const time_t*)&ss->m_time);
                if (lt) strftime(timeBuf, sizeof(timeBuf), "%m/%d %H:%M", lt);
            }
            ListView_SetItemText(hList, i, 2, timeBuf);
        }
    }

    if (selBefore >= 0 && selBefore < (int)g_snapshots.size())
    {
        ListView_SetItemState(hList, selBefore,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hList, selBefore, FALSE);
    }
}

// ---------------------------------------------------------------------------
// DoSave – capture and add new snapshot
// ---------------------------------------------------------------------------
static void DoSave(HWND hwnd)
{
    // Always auto-generate an incremented name; user renames inline via the list.
    char name[256] = {};
    {
        int maxN = (int)g_snapshots.size();
        for (auto& s : g_snapshots) {
            int n = 0;
            if (sscanf(s->m_name.c_str(), "Scene %d", &n) == 1 && n > maxN)
                maxN = n;
        }
        snprintf(name, sizeof(name), "Scene %d", maxN + 1);
    }

    int slot = (int)g_snapshots.size();
    auto ss  = std::make_unique<TransitionSnapshot>(slot, name);
    // Use global default transition settings; user adjusts per-scene via context menu
    ss->m_duration = g_defaultDuration;
    ss->m_taper    = g_defaultTaper;
    ss->m_taperExp = g_defaultTaperExp;
    ss->Capture(TS_CAPTURE_ALL);  // also captures full layer state
    ss->m_slot = slot;

    g_snapshots.push_back(std::move(ss));
    RefreshListView(hwnd);

    HWND hList = GetDlgItem(hwnd, IDC_LIST);
    int  newIdx = (int)g_snapshots.size() - 1;
    ListView_SetItemState(hList, newIdx,
        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(hList, newIdx, FALSE);
    LoadEditorFromSnapshot(hwnd, g_snapshots[newIdx].get());
    MarkTouched(newIdx);

    Undo_OnStateChangeEx("Save Scene", -1, -1);

    // Immediately drop into inline rename so the user can name the new scene.
    ListView_EditLabel(hList, newIdx);
}

// ---------------------------------------------------------------------------
// RestoreLayerState – apply full layer state from a snapshot on recall.
// Skipped when the TS_LAYERS safe bit is set, when no specific layer was
// designated for recall (m_layerUid <= 0), or when the scene has no captured
// layer data (m_layers empty — e.g. old-format scenes).
// ---------------------------------------------------------------------------
static void RestoreLayerState(TransitionSnapshot* snap)
{
    if (!snap) return;
    if (g_globalSafeMask & TS_LAYERS) return;
    if (snap->m_layers.empty()) return;

    // Number any pre-uid layers so the reference below is by uid, not index,
    // and replace a reference to a layer that has since been deleted.
    ResolveSceneLayer(snap);

    // If the scene has no layer to recall (user chose "(no layer recall)" or
    // the scene was saved before layer capture was introduced) leave the
    // current layer system untouched.
    if (snap->m_layerUid <= 0) return;

    std::vector<LayerDef> newLayers;
    for (const auto& cl : snap->m_layers)
    {
        LayerDef ld;
        strncpy(ld.name, cl.name.c_str(), sizeof(ld.name) - 1);
        ld.name[sizeof(ld.name) - 1] = '\0';
        ld.maxChannels = cl.maxChannels;
        ld.uid         = cl.uid;   // preserved across the replace
        for (const auto& clt : cl.tracks)
        {
            LayerTrack lt;
            lt.guid        = clt.guid;
            lt.isSpacer    = clt.isSpacer;
            lt.name[0]     = '\0';
            lt.folderCompact = 0;
            ld.tracks.push_back(lt);
        }
        newLayers.push_back(ld);
    }
    LayersEngine::Get().ReplaceAllLayers(newLayers, snap->m_layerUid);
    LayersEngine::Get().RefreshAllTrackNames();
}

// ---------------------------------------------------------------------------
// DoRecall – run engine on selected snapshot
// ---------------------------------------------------------------------------
static void DoRecall(HWND hwnd, int listIndex)
{
    // Map list position → snapshot index (differs in cue mode)
    int snapIdx;
    if (g_cueMode)
    {
        if (listIndex < 0 || listIndex >= (int)g_cueList.size()) return;
        snapIdx = g_cueList[listIndex];
        if (snapIdx == -1)
        {
            // Cue spacer: just advance selection to the next item
            int nextCue = listIndex + 1;
            if (nextCue < (int)g_cueList.size())
            {
                HWND hList = GetDlgItem(hwnd, IDC_LIST);
                ListView_SetItemState(hList, nextCue,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(hList, nextCue, FALSE);
                int nsi = g_cueList[nextCue];
                if (nsi >= 0 && nsi < (int)g_snapshots.size())
                    LoadEditorFromSnapshot(hwnd, g_snapshots[nsi].get());
                else
                    LoadEditorFromSnapshot(hwnd, nullptr);
            }
            return;
        }
    }
    else
    {
        snapIdx = listIndex;
    }

    if (snapIdx < 0 || snapIdx >= (int)g_snapshots.size()) return;
    if (g_snapshots[snapIdx]->m_isSpacer) return;

    TransitionSnapshot* snap = g_snapshots[snapIdx].get();
    ResolveSceneLayer(snap);   // migrate pre-uid scenes; drop dangling references
    // m_duration == 0 means instant (set via the Scene Settings popup)
    double duration = snap->m_duration;

    // Place a named marker at the play cursor position if option is enabled
    if (g_placeMarker)
    {
        double pos = GetPlayPosition();
        AddProjectMarker2(nullptr, false, pos, 0.0, snap->m_name.c_str(), -1, 0);
    }

    // Stop recording before recall (marker placed first to capture correct play position)
    if (g_stopRecBeforeRecall && (GetPlayState() & 4))
        Main_OnCommand(1016, 0);  // Stop transport

    // When a layer is being recalled, layers manage track visibility.
    // Strip TS_VIS from the engine mask so the two systems don't fight.
    int effectiveMask = snap->m_mask;
    if (!snap->m_layers.empty() && snap->m_layerUid > 0)
        effectiveMask &= ~TS_VIS;

    // --- Duration debug: record step timings if enabled ---
    LARGE_INTEGER freq = {}, t0 = {}, t1 = {}, t2 = {}, t3 = {};
    if (g_durationDebug)
        QueryPerformanceFrequency(&freq);

    if (g_durationDebug) QueryPerformanceCounter(&t0);
    TransitionEngine::Get().Recall(snap, effectiveMask, duration);
    if (g_durationDebug) QueryPerformanceCounter(&t1);

    TransitionEngine::Get().SetCurrentSlot(snapIdx);
    MarkTouched(snapIdx);
    Undo_OnStateChangeEx("Recall Scene", -1, -1);

    // Restore full layer state
    if (g_durationDebug) QueryPerformanceCounter(&t2);
    RestoreLayerState(snap);
    if (g_durationDebug) QueryPerformanceCounter(&t3);

    if (g_durationDebug && freq.QuadPart > 0)
    {
        const auto& t = TransitionEngine::Get().lastTimings;
        double msLayers = (double)(t3.QuadPart - t2.QuadPart) * 1000.0 / (double)freq.QuadPart;
        double msTotal  = (double)(t3.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;

        // Settings line (shown for both instant and timed)
        char settingsBuf[256];
        snprintf(settingsBuf, sizeof(settingsBuf),
            "  Settings:  ShadowParams=%s  ChunkInstant=%s  SkipUnchanged=%s  PreloadOffline=%s\n",
            t.s_shadowParams    ? "ON" : "off",
            t.s_chunkAllInstant ? "ON" : "off",
            t.s_skipUnchanged   ? "ON" : "off",
            t.s_preloadOffline  ? "ON" : "off");

        char buf[2048];
        if (t.instantPath)
        {
            snprintf(buf, sizeof(buf),
                "[Live Tools] Scene recall timing: \"%s\"  [INSTANT]\n"
                "%s"
                "  BuildTrackMap:     %6.2f ms  (%d tracks)\n"
                "  ApplyImmediate:    %6.2f ms\n"
                "    VolPan:          %6.2f ms\n"
                "    Mute/Solo/Phase: %6.2f ms\n"
                "    Vis/Sel/Offset:  %6.2f ms\n"
                "    Layout:          %6.2f ms\n"
                "    FX chains:       %6.2f ms\n"
                "    Sends:           %6.2f ms\n"
                "  Engine total:      %6.2f ms\n"
                "  RestoreLayerState: %6.2f ms\n"
                "  ── TOTAL ──        %6.2f ms\n",
                snap->m_name.c_str(),
                settingsBuf,
                t.buildTrackMap,  t.tracksMatched,
                t.discreteParams,
                t.i_volPan,
                t.i_muteSolo,
                t.i_vis,
                t.i_layout,
                t.i_fx,
                t.i_sends,
                t.total,
                msLayers,
                msTotal);
        }
        else
        {
            snprintf(buf, sizeof(buf),
                "[Live Tools] Scene recall timing: \"%s\"  [%.2fs timed]\n"
                "%s"
                "  SnapToEnd:         %6.2f ms\n"
                "  BuildTrackMap:     %6.2f ms  (%d matched, %d skipped)\n"
                "  DiscreteParams:    %6.2f ms  (mute/solo/vis/name/height/color)\n"
                "  FXChainSync:       %6.2f ms\n"
                "  SendsSetup:        %6.2f ms\n"
                "  TrackReorder:      %6.2f ms\n"
                "  BuildLerpLists:    %6.2f ms  (vol/pan:%d  fx:%d  wet:%d  send:%d)\n"
                "  Engine total:      %6.2f ms\n"
                "  RestoreLayerState: %6.2f ms\n"
                "  ── TOTAL ──        %6.2f ms\n",
                snap->m_name.c_str(), duration,
                settingsBuf,
                t.snapToEnd,
                t.buildTrackMap,  t.tracksMatched,  t.tracksSkipped,
                t.discreteParams,
                t.fxChainSync,
                t.sendsSetup,
                t.trackReorder,
                t.buildLerpLists, t.volPanLerps, t.paramLerps, t.wetLerps, t.sendLerps,
                t.total,
                msLayers,
                msTotal);
        }
        ShowConsoleMsg(buf);

        // FX detail: list top-5 slowest tracks and their per-FX op costs
        if (t.instantPath && !t.fxDetail.empty())
        {
            const int maxTracks = 5;
            const int maxOps    = 4;
            const int nTracks   = (int)t.fxDetail.size() < maxTracks
                                  ? (int)t.fxDetail.size() : maxTracks;

            std::string detail;
            char tmp[512];
            snprintf(tmp, sizeof(tmp),
                "  ── FX detail (top %d slowest track%s) ──\n",
                nTracks, nTracks != 1 ? "s" : "");
            detail += tmp;

            for (int ti = 0; ti < nTracks; ++ti)
            {
                const auto& tft = t.fxDetail[ti];
                snprintf(tmp, sizeof(tmp),
                    "    %-36s %8.2f ms\n",
                    tft.trackName.c_str(), tft.total_ms);
                detail += tmp;

                const int nOps = (int)tft.fxOps.size() < maxOps
                                 ? (int)tft.fxOps.size() : maxOps;
                for (int oi = 0; oi < nOps; ++oi)
                {
                    const auto& op = tft.fxOps[oi];
                    const char* tag = op.wasNew    ? "[new]   "
                                    : op.wasPrimed ? "[primed]"
                                    :                "[exist] ";
                    // Build cost string from non-trivial fields only
                    char costs[256] = {};
                    int cpos = 0;
                    if (op.addByName_ms > 0.01)
                        cpos += snprintf(costs + cpos, sizeof(costs) - cpos,
                            "  AddByName: %.2f ms", op.addByName_ms);
                    if (op.offlineSandwich_ms > 0.01)
                        cpos += snprintf(costs + cpos, sizeof(costs) - cpos,
                            "  Offline: %.2f ms", op.offlineSandwich_ms);
                    if (op.setChunk_ms > 0.01)
                        cpos += snprintf(costs + cpos, sizeof(costs) - cpos,
                            "  SetChunk: %.2f ms", op.setChunk_ms);
                    if (op.paramLoop_ms > 0.01)
                        cpos += snprintf(costs + cpos, sizeof(costs) - cpos,
                            "  Params: %.2f ms", op.paramLoop_ms);
                    snprintf(tmp, sizeof(tmp),
                        "      %s  %-30s%s\n",
                        tag, op.fxName.c_str(), costs);
                    detail += tmp;
                }
            }
            ShowConsoleMsg(detail.c_str());
        }
    }  // end if (g_durationDebug)

    // Cue mode: auto-advance to the next item in the cue list
    if (g_cueMode)
    {
        int nextCue = listIndex + 1;
        if (nextCue < (int)g_cueList.size())
        {
            HWND hList = GetDlgItem(hwnd, IDC_LIST);
            ListView_SetItemState(hList, nextCue,
                LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(hList, nextCue, FALSE);
            int nextSnapIdx = g_cueList[nextCue];
            if (nextSnapIdx >= 0 && nextSnapIdx < (int)g_snapshots.size())
                LoadEditorFromSnapshot(hwnd, g_snapshots[nextSnapIdx].get());
            else
                LoadEditorFromSnapshot(hwnd, nullptr);
        }
    }
    else
    {
        // Scenes mode: update list selection to reflect current scene
        HWND hList = GetDlgItem(hwnd, IDC_LIST);
        ListView_SetItemState(hList, snapIdx,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hList, snapIdx, FALSE);
    }

    // A recall can change the whole layer set. Repaint the sidebar so the
    // "Layer:" readout matches, and the Layers window so its list does too —
    // neither happened before, which is why the layer UI only caught up the
    // next time something else touched a layer.
    LoadEditorFromSnapshot(hwnd, snap);
    LayersWnd_Refresh();

    // Start recording after recall if enabled
    if (g_startRecAfterRecall)
        Main_OnCommand(1013, 0);  // Record
}

// ---------------------------------------------------------------------------
// File I/O ProjectStateContext helpers for export/import
// ---------------------------------------------------------------------------
class FileWriteCtx : public ProjectStateContext
{
    FILE* fp_;
public:
    explicit FileWriteCtx(FILE* f) : fp_(f) {}
    void  AddLine(const char* fmt, ...) override
    {
        va_list args; va_start(args, fmt);
        vfprintf(fp_, fmt, args);
        va_end(args);
        fputc('\n', fp_);
    }
    int   GetLine(char*, int)   override { return -1; }
    INT64 GetOutputSize()       override { return 0; }
    int   GetTempFlag()         override { return 0; }
    void  SetTempFlag(int)      override {}
};

class FileReadCtx : public ProjectStateContext
{
    FILE* fp_;
public:
    explicit FileReadCtx(FILE* f) : fp_(f) {}
    void  AddLine(const char*, ...) override {}
    int   GetLine(char* buf, int len) override
    {
        if (!fgets(buf, len, fp_)) return -1;
        int l = (int)strlen(buf);
        while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r')) buf[--l] = '\0';
        return 0;
    }
    INT64 GetOutputSize()       override { return 0; }
    int   GetTempFlag()         override { return 0; }
    void  SetTempFlag(int)      override {}
};

// ---------------------------------------------------------------------------
// ExportScene – write a single scene to a .lts file
// ---------------------------------------------------------------------------
static void ExportScene(HWND hwnd, int item)
{
    if (item < 0 || item >= (int)g_snapshots.size()) return;
    if (g_snapshots[item]->m_isSpacer) return;

    char szFile[MAX_PATH] = {};
    strncpy_s(szFile, g_snapshots[item]->m_name.c_str(), MAX_PATH - 1);
    for (char& c : szFile)
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"'  || c == '<' || c == '>' || c == '|')
            c = '_';

    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = "Scene Files (*.lts)\0*.lts\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = szFile;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = "lts";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle  = "Export Scene";

    if (!GetSaveFileNameA(&ofn)) return;

    FILE* fp = nullptr;
    fopen_s(&fp, szFile, "w");
    if (!fp)
    {
        MessageBoxA(hwnd, "Could not create file.", "Export Error", MB_OK | MB_ICONERROR);
        return;
    }
    FileWriteCtx ctx(fp);
    g_snapshots[item]->Serialize(&ctx);
    fclose(fp);
}

// ---------------------------------------------------------------------------
// ImportScene – load a .lts file and append to g_snapshots
// ---------------------------------------------------------------------------
static void ImportScene(HWND hwnd)
{
    char szFile[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = "Scene Files (*.lts)\0*.lts\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = szFile;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = "lts";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle  = "Import Scene";

    if (!GetOpenFileNameA(&ofn)) return;

    FILE* fp = nullptr;
    fopen_s(&fp, szFile, "r");
    if (!fp)
    {
        MessageBoxA(hwnd, "Could not open file.", "Import Error", MB_OK | MB_ICONERROR);
        return;
    }

    char headerLine[512] = {};
    if (!fgets(headerLine, sizeof(headerLine), fp)) { fclose(fp); return; }
    int l = (int)strlen(headerLine);
    while (l > 0 && (headerLine[l-1] == '\n' || headerLine[l-1] == '\r')) headerLine[--l] = '\0';

    FileReadCtx readCtx(fp);
    TransitionSnapshot* ss = TransitionSnapshot::Deserialize(headerLine, &readCtx);
    fclose(fp);

    if (!ss)
    {
        MessageBoxA(hwnd, "Invalid or unrecognised scene file.", "Import Error", MB_OK | MB_ICONERROR);
        return;
    }

    ss->m_slot = (int)g_snapshots.size();
    g_snapshots.push_back(std::unique_ptr<TransitionSnapshot>(ss));
    RefreshListView(hwnd);

    HWND hList  = GetDlgItem(hwnd, IDC_LIST);
    int  newIdx = (int)g_snapshots.size() - 1;
    ListView_SetItemState(hList, newIdx,
        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(hList, newIdx, FALSE);
    LoadEditorFromSnapshot(hwnd, g_snapshots[newIdx].get());
    Undo_OnStateChangeEx("Import Scene", -1, -1);
}

// ---------------------------------------------------------------------------
// DoEndDrag – finalize a drag-and-drop reorder
// ---------------------------------------------------------------------------
static void DoEndDrag(HWND hwnd)
{
#ifdef _WIN32
    if (g_hDragImages)
    {
        ImageList_DragLeave(hwnd);
        ImageList_EndDrag();
        ImageList_Destroy(g_hDragImages);
        g_hDragImages = nullptr;
    }
#endif
    ReleaseCapture();

    HWND hList = GetDlgItem(hwnd, IDC_LIST);
    ListView_SetItemState(hList, -1, 0, LVIS_DROPHILITED);

    int src = g_dragSrc;
    int tgt = g_dragTarget;
    g_dragSrc    = -1;
    g_dragTarget = -1;

    if (src < 0 || tgt < 0 || tgt == src || src >= (int)g_snapshots.size()) return;

    auto moved = std::move(g_snapshots[src]);
    g_snapshots.erase(g_snapshots.begin() + src);

    int insertAt = (tgt > src) ? tgt - 1 : tgt;
    if (insertAt < 0) insertAt = 0;
    if (insertAt > (int)g_snapshots.size()) insertAt = (int)g_snapshots.size();

    g_snapshots.insert(g_snapshots.begin() + insertAt, std::move(moved));
    for (int i = 0; i < (int)g_snapshots.size(); i++)
        g_snapshots[i]->m_slot = i;

    RefreshListView(hwnd);
    ListView_SetItemState(hList, insertAt,
        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(hList, insertAt, FALSE);
    if (insertAt < (int)g_snapshots.size() && !g_snapshots[insertAt]->m_isSpacer)
        LoadEditorFromSnapshot(hwnd, g_snapshots[insertAt].get());
}

// ---------------------------------------------------------------------------
// SnapSettings – data passed to and from the settings popup
// ---------------------------------------------------------------------------
struct SnapSettingsData
{
    // In/out
    std::string notes;
    double      duration = 2.0;
    int         taper    = TAPER_SCURVE;
    double      taperExp = 2.0;
    bool        instant  = false;   // true when duration == 0
};

// ---------------------------------------------------------------------------
// SnapSettingsDialogProc – modal IDD_SNAP_SETTINGS dialog
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK SnapSettingsDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SnapSettingsData* d = reinterpret_cast<SnapSettingsData*>(lParam);
        SetWindowLongPtr(hwnd, DWLP_USER, (LONG_PTR)d);

        // Populate taper combobox
        const char* taperItems[] = {
            "Linear", "S-Curve", "Logarithmic", "Exponential", "Custom..."
        };
        for (const char* n : taperItems)
            SendDlgItemMessage(hwnd, IDC_TAPER, CB_ADDSTRING, 0, (LPARAM)n);

        // Fill from data
        SetDlgItemText(hwnd, IDC_SNAPNOTES, NotesToControl(d->notes).c_str());
        CheckDlgButton(hwnd, IDC_INSTANT, d->instant ? BST_CHECKED : BST_UNCHECKED);

        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f", d->instant ? 2.0 : d->duration);
        SetDlgItemText(hwnd, IDC_DURATION, buf);

        int taperSel = (d->taper >= 0 && d->taper <= TAPER_CUSTOM) ? d->taper : TAPER_SCURVE;
        SendDlgItemMessage(hwnd, IDC_TAPER, CB_SETCURSEL, taperSel, 0);

        snprintf(buf, sizeof(buf), "%.2f", d->taperExp);
        SetDlgItemText(hwnd, IDC_TAPER_CUSTOM, buf);

        // Enable/disable based on instant
        EnableWindow(GetDlgItem(hwnd, IDC_DURATION),    !d->instant);
        EnableWindow(GetDlgItem(hwnd, IDC_TAPER),       !d->instant);
        EnableWindow(GetDlgItem(hwnd, IDC_TAPER_CUSTOM),
                     !d->instant && d->taper == TAPER_CUSTOM);
        return TRUE;
    }

    case WM_COMMAND:
    {
        int id  = LOWORD(wParam);
        int evt = HIWORD(wParam);

        if (id == IDC_INSTANT)
        {
            bool instant = (IsDlgButtonChecked(hwnd, IDC_INSTANT) == BST_CHECKED);
            EnableWindow(GetDlgItem(hwnd, IDC_DURATION),    !instant);
            EnableWindow(GetDlgItem(hwnd, IDC_TAPER),       !instant);
            int taperSel = (int)SendDlgItemMessage(hwnd, IDC_TAPER, CB_GETCURSEL, 0, 0);
            EnableWindow(GetDlgItem(hwnd, IDC_TAPER_CUSTOM),
                         !instant && taperSel == TAPER_CUSTOM);
            return TRUE;
        }

        if (id == IDC_TAPER && evt == CBN_SELCHANGE)
        {
            int sel = (int)SendDlgItemMessage(hwnd, IDC_TAPER, CB_GETCURSEL, 0, 0);
            bool instant = (IsDlgButtonChecked(hwnd, IDC_INSTANT) == BST_CHECKED);
            EnableWindow(GetDlgItem(hwnd, IDC_TAPER_CUSTOM),
                         !instant && sel == TAPER_CUSTOM);
            return TRUE;
        }

        if (id == IDOK)
        {
            SnapSettingsData* d = reinterpret_cast<SnapSettingsData*>(
                GetWindowLongPtr(hwnd, DWLP_USER));

            char buf[4096] = {};
            GetDlgItemText(hwnd, IDC_SNAPNOTES, buf, sizeof(buf));
            d->notes = NotesFromControl(buf);

            d->instant = (IsDlgButtonChecked(hwnd, IDC_INSTANT) == BST_CHECKED);

            char durBuf[64] = {};
            GetDlgItemText(hwnd, IDC_DURATION, durBuf, sizeof(durBuf));
            double dur = atof(durBuf);
            d->duration = d->instant ? 0.0 : (dur > 0.0 ? dur : 2.0);

            d->taper = (int)SendDlgItemMessage(hwnd, IDC_TAPER, CB_GETCURSEL, 0, 0);
            if (d->taper < 0 || d->taper > TAPER_CUSTOM) d->taper = TAPER_SCURVE;

            char expBuf[64] = {};
            GetDlgItemText(hwnd, IDC_TAPER_CUSTOM, expBuf, sizeof(expBuf));
            double ex = atof(expBuf);
            d->taperExp = (ex > 0.0) ? ex : 2.0;

            EndDialog(hwnd, IDOK);
            return TRUE;
        }

        if (id == IDCANCEL)
        {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// RefreshChunkRecallList – repopulate the list box in IDD_CHUNK_RECALL_PLUGINS
// ---------------------------------------------------------------------------
static void RefreshChunkRecallList(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_CRP_LIST);
    SendMessage(hList, LB_RESETCONTENT, 0, 0);

    for (const auto& kw : g_chunkRecallKeywords)
        SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)kw.c_str());

    EnableWindow(GetDlgItem(hwnd, IDC_CRP_REMOVE), FALSE);
}

// ---------------------------------------------------------------------------
// AddKeywordDlgProc – simple text-input prompt (IDD_ADD_KEYWORD)
// ---------------------------------------------------------------------------
static char s_newKeywordBuf[256] = {};

static INT_PTR CALLBACK AddKeywordDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM /*lParam*/)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        s_newKeywordBuf[0] = '\0';
        SetDlgItemText(hwnd, IDC_AK_EDIT, "");
        SendDlgItemMessage(hwnd, IDC_AK_EDIT, EM_LIMITTEXT, (WPARAM)(sizeof(s_newKeywordBuf) - 1), 0);
        return TRUE;
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == IDOK)
        {
            GetDlgItemText(hwnd, IDC_AK_EDIT, s_newKeywordBuf, (int)sizeof(s_newKeywordBuf));
            // Trim leading whitespace
            char* p = s_newKeywordBuf;
            while (*p == ' ' || *p == '\t') p++;
            if (p != s_newKeywordBuf) memmove(s_newKeywordBuf, p, strlen(p) + 1);
            // Trim trailing whitespace
            size_t len = strlen(s_newKeywordBuf);
            while (len > 0 && (s_newKeywordBuf[len - 1] == ' ' || s_newKeywordBuf[len - 1] == '\t'))
                s_newKeywordBuf[--len] = '\0';
            if (s_newKeywordBuf[0] == '\0') return TRUE; // reject empty
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) { EndDialog(hwnd, IDCANCEL); return TRUE; }
        break;
    }
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// ChunkRecallPluginsDlgProc – IDD_CHUNK_RECALL_PLUGINS dialog
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK ChunkRecallPluginsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM /*lParam*/)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        RefreshChunkRecallList(hwnd);
        CheckDlgButton(hwnd, IDC_CRP_NOTIFY, g_chunkRecallNotify ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;

    case WM_COMMAND:
    {
        int id = LOWORD(wParam), evt = HIWORD(wParam);

        if (id == IDC_CRP_LIST && evt == LBN_SELCHANGE)
        {
            int sel = (int)SendDlgItemMessage(hwnd, IDC_CRP_LIST, LB_GETCURSEL, 0, 0);
            EnableWindow(GetDlgItem(hwnd, IDC_CRP_REMOVE), (sel >= 0) ? TRUE : FALSE);
            return TRUE;
        }

        if (id == IDC_CRP_ADD)
        {
            if (DialogBoxParam(g_hInstance, MAKEINTRESOURCE(IDD_ADD_KEYWORD),
                               hwnd, AddKeywordDlgProc, 0) == IDOK
                && s_newKeywordBuf[0])
            {
                // Reject duplicates (case-sensitive exact match)
                bool found = false;
                for (const auto& kw : g_chunkRecallKeywords)
                    if (kw == s_newKeywordBuf) { found = true; break; }
                if (!found)
                {
                    g_chunkRecallKeywords.push_back(s_newKeywordBuf);
                    RefreshChunkRecallList(hwnd);
                    // Select the newly added entry
                    int newIdx = (int)g_chunkRecallKeywords.size() - 1;
                    SendDlgItemMessage(hwnd, IDC_CRP_LIST, LB_SETCURSEL, (WPARAM)newIdx, 0);
                    EnableWindow(GetDlgItem(hwnd, IDC_CRP_REMOVE), TRUE);
                    MarkProjectDirty(nullptr);
                }
            }
            return TRUE;
        }

        if (id == IDC_CRP_REMOVE)
        {
            int sel = (int)SendDlgItemMessage(hwnd, IDC_CRP_LIST, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)g_chunkRecallKeywords.size())
            {
                g_chunkRecallKeywords.erase(g_chunkRecallKeywords.begin() + sel);
                RefreshChunkRecallList(hwnd);
                MarkProjectDirty(nullptr);
            }
            return TRUE;
        }

        if (id == IDC_CRP_NOTIFY)
        {
            g_chunkRecallNotify = (IsDlgButtonChecked(hwnd, IDC_CRP_NOTIFY) == BST_CHECKED);
            return TRUE;
        }

        if (id == IDCANCEL || id == IDOK) { EndDialog(hwnd, id); return TRUE; }
        break;
    }
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// GlobalSettingsDialogProc – modal IDD_GLOBAL_SETTINGS dialog
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK GlobalSettingsDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        // Populate taper combobox
        const char* taperItems[] = {
            "Linear", "S-Curve", "Logarithmic", "Exponential", "Custom..."
        };
        for (const char* n : taperItems)
            SendDlgItemMessage(hwnd, IDC_GSET_TAPER, CB_ADDSTRING, 0, (LPARAM)n);

        bool instant = (g_defaultDuration == 0.0);
        CheckDlgButton(hwnd, IDC_GSET_INSTANT, instant ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_MARKER,            g_placeMarker           ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_STOP_REC_BEFORE,   g_stopRecBeforeRecall   ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_START_REC_AFTER,   g_startRecAfterRecall   ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_SINGLE_CLICK,    g_singleClickRecall   ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_ALT_DELETE,      g_altClickDelete      ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_CTRL_OVERWRITE,  g_ctrlClickOverwrite  ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_PRELOAD_OFFLINE,  g_preloadOffline      ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_SKIP_UNCHANGED,   g_skipUnchangedParams ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_DURATION_DEBUG,   g_durationDebug       ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_SHADOW_PARAMS,     g_shadowParams       ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_CHUNK_ALL_INSTANT, g_chunkAllInstant    ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_RECALL_LOG,        g_recallLog          ? BST_CHECKED : BST_UNCHECKED);

        // Tooltip for the preload offline checkbox
        HWND hwndTip = CreateWindowEx(0, TOOLTIPS_CLASS, NULL,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hwnd, NULL, g_hInstance, NULL);
        if (hwndTip)
        {
            TOOLINFO ti = {};
            ti.cbSize   = sizeof(ti);
            ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd     = hwnd;
            ti.uId      = (UINT_PTR)GetDlgItem(hwnd, IDC_GSET_PRELOAD_OFFLINE);
            ti.lpszText = (LPSTR)"Preloads newly added plugins offline ahead of time to reduce "
                                 "live audio stuttering, but may delay the scene recall slightly "
                                 "depending on the plugin. Use \"Prime Scenes...\" to pre-load "
                                 "all plugins before the show instead.";
            SendMessage(hwndTip, TTM_ADDTOOL, 0, (LPARAM)&ti);
            SendMessage(hwndTip, TTM_SETMAXTIPWIDTH, 0, 300);
        }

        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f", instant ? 2.0 : g_defaultDuration);
        SetDlgItemText(hwnd, IDC_GSET_DURATION, buf);

        int taperSel = (g_defaultTaper >= 0 && g_defaultTaper <= TAPER_CUSTOM)
                       ? g_defaultTaper : TAPER_SCURVE;
        SendDlgItemMessage(hwnd, IDC_GSET_TAPER, CB_SETCURSEL, taperSel, 0);

        snprintf(buf, sizeof(buf), "%.2f", g_defaultTaperExp);
        SetDlgItemText(hwnd, IDC_GSET_TAPER_CUSTOM, buf);

        EnableWindow(GetDlgItem(hwnd, IDC_GSET_DURATION),    !instant);
        EnableWindow(GetDlgItem(hwnd, IDC_GSET_TAPER),       !instant);
        EnableWindow(GetDlgItem(hwnd, IDC_GSET_TAPER_CUSTOM),
                     !instant && g_defaultTaper == TAPER_CUSTOM);
        return TRUE;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wParam), evt = HIWORD(wParam);
        if (id == IDC_GSET_INSTANT)
        {
            bool instant = (IsDlgButtonChecked(hwnd, IDC_GSET_INSTANT) == BST_CHECKED);
            EnableWindow(GetDlgItem(hwnd, IDC_GSET_DURATION),    !instant);
            EnableWindow(GetDlgItem(hwnd, IDC_GSET_TAPER),       !instant);
            int sel = (int)SendDlgItemMessage(hwnd, IDC_GSET_TAPER, CB_GETCURSEL, 0, 0);
            EnableWindow(GetDlgItem(hwnd, IDC_GSET_TAPER_CUSTOM),
                         !instant && sel == TAPER_CUSTOM);
            return TRUE;
        }
        if (id == IDC_GSET_TAPER && evt == CBN_SELCHANGE)
        {
            int sel = (int)SendDlgItemMessage(hwnd, IDC_GSET_TAPER, CB_GETCURSEL, 0, 0);
            bool instant = (IsDlgButtonChecked(hwnd, IDC_GSET_INSTANT) == BST_CHECKED);
            EnableWindow(GetDlgItem(hwnd, IDC_GSET_TAPER_CUSTOM),
                         !instant && sel == TAPER_CUSTOM);
            return TRUE;
        }
        if (id == IDC_GSET_CHUNK_BTN)
        {
            DialogBoxParam(g_hInstance, MAKEINTRESOURCE(IDD_CHUNK_RECALL_PLUGINS),
                           hwnd, ChunkRecallPluginsDlgProc, 0);
            return TRUE;
        }
        if (id == IDOK)
        {
            bool instant = (IsDlgButtonChecked(hwnd, IDC_GSET_INSTANT) == BST_CHECKED);            if (instant)
            {
                g_defaultDuration = 0.0;
            }
            else
            {
                char buf[64] = {};
                GetDlgItemText(hwnd, IDC_GSET_DURATION, buf, sizeof(buf));
                double dur = atof(buf);
                g_defaultDuration = (dur > 0.0) ? dur : 2.0;
            }
            g_defaultTaper = (int)SendDlgItemMessage(hwnd, IDC_GSET_TAPER, CB_GETCURSEL, 0, 0);
            if (g_defaultTaper < 0 || g_defaultTaper > TAPER_CUSTOM) g_defaultTaper = TAPER_SCURVE;
            char exBuf[64] = {};
            GetDlgItemText(hwnd, IDC_GSET_TAPER_CUSTOM, exBuf, sizeof(exBuf));
            double ex = atof(exBuf);
            g_defaultTaperExp = (ex > 0.0) ? ex : 2.0;

            g_placeMarker         = (IsDlgButtonChecked(hwnd, IDC_GSET_MARKER)            == BST_CHECKED);
            g_stopRecBeforeRecall = (IsDlgButtonChecked(hwnd, IDC_GSET_STOP_REC_BEFORE)   == BST_CHECKED);
            g_startRecAfterRecall = (IsDlgButtonChecked(hwnd, IDC_GSET_START_REC_AFTER)   == BST_CHECKED);
            g_singleClickRecall   = (IsDlgButtonChecked(hwnd, IDC_GSET_SINGLE_CLICK)    == BST_CHECKED);
            g_altClickDelete      = (IsDlgButtonChecked(hwnd, IDC_GSET_ALT_DELETE)       == BST_CHECKED);
            g_ctrlClickOverwrite  = (IsDlgButtonChecked(hwnd, IDC_GSET_CTRL_OVERWRITE)   == BST_CHECKED);
            g_preloadOffline      = (IsDlgButtonChecked(hwnd, IDC_GSET_PRELOAD_OFFLINE)   == BST_CHECKED);
            g_skipUnchangedParams = (IsDlgButtonChecked(hwnd, IDC_GSET_SKIP_UNCHANGED)    == BST_CHECKED);
            g_durationDebug       = (IsDlgButtonChecked(hwnd, IDC_GSET_DURATION_DEBUG)    == BST_CHECKED);
            g_shadowParams        = (IsDlgButtonChecked(hwnd, IDC_GSET_SHADOW_PARAMS)     == BST_CHECKED);
            g_chunkAllInstant     = (IsDlgButtonChecked(hwnd, IDC_GSET_CHUNK_ALL_INSTANT) == BST_CHECKED);
            g_recallLog           = (IsDlgButtonChecked(hwnd, IDC_GSET_RECALL_LOG)        == BST_CHECKED);
            MarkProjectDirty(nullptr);  // settings are saved per-project via SaveExtensionConfig

            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) { EndDialog(hwnd, IDCANCEL); return TRUE; }
        break;
    }
    }
    return FALSE;
}

// Vista+ ListView insert-mark constants (guard for older SDK targets)
#ifndef LVIMF_AFTER
#define LVIMF_AFTER 0x00000001
#endif

// ---------------------------------------------------------------------------
// CueLvSubclassProc – subclass proc for list views inside IDD_CUE_SETUP.
// Owns the full drag lifecycle: threshold detection → SetCapture(hList) →
// insert-mark update → drop on WM_LBUTTONUP.  The dialog proc itself
// needs no mouse handling at all.
// ---------------------------------------------------------------------------
static LRESULT CALLBACK CueLvSubclassProc(HWND hList, UINT msg,
                                           WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_RBUTTONDOWN:
    {
        // Right-click on the cue-order (right) list: offer "Remove from Cue"
        if (hList == s_cueRight && s_cueEditList)
        {
            LVHITTESTINFO htiR = {};
            htiR.pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int item = ListView_HitTest(hList, &htiR);
            if (item >= 0 && item < (int)s_cueEditList->size())
            {
                ListView_SetItemState(hList, item,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                POINT ptScreen = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ClientToScreen(hList, &ptScreen);
                HMENU hMenu = CreatePopupMenu();
                AppendMenu(hMenu, MF_STRING, 1, "Remove from Cue");
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                         ptScreen.x, ptScreen.y, 0, hList, nullptr);
                DestroyMenu(hMenu);
                if (cmd == 1)
                {
                    s_cueEditList->erase(s_cueEditList->begin() + item);
                    RefillCueRightList(s_cueRight, *s_cueEditList);
                }
                return 0;
            }
        }
        break;
    }

    case WM_LBUTTONDOWN:
    {
        LRESULT r = CallWindowProc(s_origCueLvProc, hList, msg, wParam, lParam);
        LVHITTESTINFO hti = {};
        hti.pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int item = ListView_HitTest(hList, &hti);
        if (item >= 0)
        {
            s_cueDrag.tracking = true;
            s_cueDrag.active   = false;
            s_cueDrag.srcList  = hList;
            s_cueDrag.srcItem  = item;
            s_cueDrag.downPt   = hti.pt;
            s_cueDrag.downTime = GetTickCount();
        }
        return r;
    }

    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (s_cueDrag.tracking && !(wParam & MK_LBUTTON))
            s_cueDrag.tracking = false;

        // Once threshold is crossed, take capture on this list.
        if (s_cueDrag.tracking && !s_cueDrag.active && s_cueDrag.srcList == hList)
        {
            bool moved = (abs(pt.x - s_cueDrag.downPt.x) > GetSystemMetrics(SM_CXDRAG) ||
                          abs(pt.y - s_cueDrag.downPt.y) > GetSystemMetrics(SM_CYDRAG));
            if (moved)
            {
                s_cueDrag.tracking = false;
                s_cueDrag.active   = true;
                SetCapture(hList); // capture on this list – all subsequent msgs come here
            }
        }

        // While dragging, update insert mark on s_cueRight.
        if (s_cueDrag.active && s_cueRight)
        {
            POINT ptScreen = pt;
            ClientToScreen(hList, &ptScreen);

            RECT rcRight = {};
            GetWindowRect(s_cueRight, &rcRight);

            LVINSERTMARK im = {};
            im.cbSize = sizeof(im);
            im.iItem  = -1;

            if (PtInRect(&rcRight, ptScreen))
            {
                POINT ptRight = ptScreen;
                ScreenToClient(s_cueRight, &ptRight);

                LVHITTESTINFO hti = {}; hti.pt = ptRight;
                int hitItem = ListView_HitTest(s_cueRight, &hti);
                int count   = ListView_GetItemCount(s_cueRight);

                if (hitItem >= 0)
                {
                    RECT ir = {};
                    ListView_GetItemRect(s_cueRight, hitItem, &ir, LVIR_BOUNDS);
                    if (ptRight.y > (ir.top + ir.bottom) / 2)
                        { im.iItem = hitItem; im.dwFlags = LVIMF_AFTER; }
                    else
                        { im.iItem = hitItem; }
                }
                else if (count > 0)
                {
                    im.iItem = count - 1; im.dwFlags = LVIMF_AFTER;
                }
            }
            SendMessage(s_cueRight, LVM_SETINSERTMARK, 0, (LPARAM)&im);
        }
        break;
    }

    case WM_LBUTTONUP:
    {
        // Save drag state and clear BEFORE ReleaseCapture so that the
        // synchronous WM_CAPTURECHANGED it fires is harmless.
        CueDragState drag = s_cueDrag;
        s_cueDrag = {};

        if (drag.active && s_cueRight && s_cueEditList)
        {
            // Clear insert mark.
            LVINSERTMARK imClear = {}; imClear.cbSize = sizeof(imClear); imClear.iItem = -1;
            SendMessage(s_cueRight, LVM_SETINSERTMARK, 0, (LPARAM)&imClear);

            POINT ptScreen = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hList, &ptScreen);

            RECT rcRight = {};
            GetWindowRect(s_cueRight, &rcRight);

            if (PtInRect(&rcRight, ptScreen))
            {
                POINT ptRight = ptScreen;
                ScreenToClient(s_cueRight, &ptRight);

                LVHITTESTINFO hti = {}; hti.pt = ptRight;
                int hitItem = ListView_HitTest(s_cueRight, &hti);
                int count   = ListView_GetItemCount(s_cueRight);

                int insertAt;
                if (hitItem >= 0)
                {
                    RECT ir = {};
                    ListView_GetItemRect(s_cueRight, hitItem, &ir, LVIR_BOUNDS);
                    insertAt = (ptRight.y > (ir.top + ir.bottom) / 2) ? hitItem + 1 : hitItem;
                }
                else
                {
                    insertAt = count;
                }

                if (drag.srcList == s_cueLeft)
                {
                    LVITEM lvi = {}; lvi.mask = LVIF_PARAM; lvi.iItem = drag.srcItem;
                    ListView_GetItem(s_cueLeft, &lvi);
                    int snapIdx = (int)lvi.lParam;
                    if (insertAt < 0) insertAt = 0;
                    if (insertAt > (int)s_cueEditList->size()) insertAt = (int)s_cueEditList->size();
                    s_cueEditList->insert(s_cueEditList->begin() + insertAt, snapIdx);
                    RefillCueRightList(s_cueRight, *s_cueEditList);
                }
                else if (drag.srcList == s_cueRight)
                {
                    int from = drag.srcItem;
                    if (insertAt != from && insertAt != from + 1)
                    {
                        int snapIdx = (*s_cueEditList)[from];
                        s_cueEditList->erase(s_cueEditList->begin() + from);
                        if (insertAt > from) insertAt--;
                        if (insertAt < 0) insertAt = 0;
                        s_cueEditList->insert(s_cueEditList->begin() + insertAt, snapIdx);
                        RefillCueRightList(s_cueRight, *s_cueEditList);
                    }
                }
            }
        }
        ReleaseCapture();
        break;
    }

    case WM_CAPTURECHANGED:
    {
        if (s_cueDrag.active)
        {
            s_cueDrag = {};
            if (s_cueRight)
            {
                LVINSERTMARK im = {}; im.cbSize = sizeof(im); im.iItem = -1;
                SendMessage(s_cueRight, LVM_SETINSERTMARK, 0, (LPARAM)&im);
            }
        }
        else
        {
            s_cueDrag.tracking = false;
        }
        break;
    }
    }
    return CallWindowProc(s_origCueLvProc, hList, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// RefillCueRightList – helper used by CueSetupDialogProc to repopulate the
// right (cue-order) ListView from the current editList vector.
// ---------------------------------------------------------------------------
static void RefillCueRightList(HWND hRight, const std::vector<int>& list)
{
    ListView_DeleteAllItems(hRight);
    for (int ci = 0; ci < (int)list.size(); ci++)
    {
        int snapIdx = list[ci];
        LVITEM lvi = {};
        lvi.mask    = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem   = ci;
        lvi.lParam  = (LPARAM)snapIdx;
        if (snapIdx == -1)
        {
            // Cue spacer
            lvi.pszText = const_cast<char*>("---");
            ListView_InsertItem(hRight, &lvi);
            ListView_SetItemText(hRight, ci, 1, const_cast<char*>("--- spacer ---"));
            continue;
        }
        if (snapIdx < 0 || snapIdx >= (int)g_snapshots.size()) continue;
        char buf[16]; snprintf(buf, sizeof(buf), "%d", ci + 1);
        lvi.pszText = buf;
        ListView_InsertItem(hRight, &lvi);
        ListView_SetItemText(hRight, ci, 1,
            const_cast<char*>(g_snapshots[snapIdx]->m_name.c_str()));
    }
}

// ---------------------------------------------------------------------------
// CueSetupDialogProc – modal IDD_CUE_SETUP dialog
// Two list views: left = spacer + all scenes, right = cue list order.
// All drag logic is in CueLvSubclassProc; this proc only handles
// double-click, ESC close, and WM_DESTROY cleanup.
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK CueSetupDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        s_cueEditList = reinterpret_cast<std::vector<int>*>(lParam);
        s_cueDrag     = {};
        SetWindowLongPtr(hwnd, DWLP_USER, lParam);

        // Replace LTEXT placeholders with real SysListView32 controls
        auto CreateList = [&](int placeholderId) -> HWND {
            HWND hPh = GetDlgItem(hwnd, placeholderId);
            if (!hPh) return nullptr;
            RECT r;
            GetWindowRect(hPh, &r);
            MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&r, 2);
            ShowWindow(hPh, SW_HIDE);
            HWND hLv = CreateWindowEx(WS_EX_CLIENTEDGE, WC_LISTVIEW, "",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                r.left, r.top, r.right - r.left, r.bottom - r.top,
                hwnd, (HMENU)(UINT_PTR)placeholderId, g_hInstance, nullptr);
            SendMessage(hLv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            return hLv;
        };

        s_cueLeft  = CreateList(IDC_CUE_LEFT_LIST);
        s_cueRight = CreateList(IDC_CUE_RIGHT_LIST);

        if (s_cueLeft)
        {
            LVCOLUMN lvc = {};
            lvc.mask = LVCF_TEXT | LVCF_WIDTH;
            lvc.cx = 44;  lvc.pszText = const_cast<char*>("#");
            ListView_InsertColumn(s_cueLeft, 0, &lvc);
            lvc.cx = 230; lvc.pszText = const_cast<char*>("Scene Name");
            ListView_InsertColumn(s_cueLeft, 1, &lvc);
        }
        if (s_cueRight)
        {
            LVCOLUMN lvc = {};
            lvc.mask = LVCF_TEXT | LVCF_WIDTH;
            lvc.cx = 40;  lvc.pszText = const_cast<char*>("#");
            ListView_InsertColumn(s_cueRight, 0, &lvc);
            lvc.cx = 230; lvc.pszText = const_cast<char*>("Scene Name");
            ListView_InsertColumn(s_cueRight, 1, &lvc);
        }

        // Left list: permanent spacer entry at row 0, then all non-spacer scenes
        if (s_cueLeft)
        {
            LVITEM spacerLvi = {};
            spacerLvi.mask   = LVIF_TEXT | LVIF_PARAM;
            spacerLvi.iItem  = 0;
            spacerLvi.lParam = (LPARAM)-1;
            spacerLvi.pszText = const_cast<char*>("---");
            ListView_InsertItem(s_cueLeft, &spacerLvi);
            ListView_SetItemText(s_cueLeft, 0, 1, const_cast<char*>("--- spacer ---"));

            int row = 1;
            for (int i = 0; i < (int)g_snapshots.size(); i++)
            {
                if (g_snapshots[i]->m_isSpacer) continue;
                LVITEM lvi = {};
                lvi.mask   = LVIF_TEXT | LVIF_PARAM;
                lvi.iItem  = row++;
                lvi.lParam = (LPARAM)i;
                char buf[16]; snprintf(buf, sizeof(buf), "S%d", i + 1);
                lvi.pszText = buf;
                ListView_InsertItem(s_cueLeft, &lvi);
                ListView_SetItemText(s_cueLeft, row - 1, 1,
                    const_cast<char*>(g_snapshots[i]->m_name.c_str()));
            }
        }

        if (s_cueRight && s_cueEditList)
            RefillCueRightList(s_cueRight, *s_cueEditList);

        // Subclass both list views for drag detection
        if (s_cueLeft)
        {
            s_origCueLvProc = (WNDPROC)SetWindowLongPtr(
                s_cueLeft, GWLP_WNDPROC, (LONG_PTR)CueLvSubclassProc);
        }
        if (s_cueRight && s_origCueLvProc)
        {
            SetWindowLongPtr(s_cueRight, GWLP_WNDPROC, (LONG_PTR)CueLvSubclassProc);
        }

        return TRUE;
    }

    case WM_NOTIFY:
    {
        NMHDR* hdr = (NMHDR*)lParam;

        // Double-click on left list → append to cue
        if (hdr->hwndFrom == s_cueLeft && hdr->code == NM_DBLCLK && s_cueEditList)
        {
            NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
            if (nia->iItem >= 0)
            {
                LVITEM lvi = {}; lvi.mask = LVIF_PARAM; lvi.iItem = nia->iItem;
                ListView_GetItem(s_cueLeft, &lvi);
                s_cueEditList->push_back((int)lvi.lParam);
                RefillCueRightList(s_cueRight, *s_cueEditList);
            }
        }
        break;
    }

    case WM_DESTROY:
    {
        // Restore original list view proc before the windows are destroyed
        if (s_cueLeft  && s_origCueLvProc)
            SetWindowLongPtr(s_cueLeft,  GWLP_WNDPROC, (LONG_PTR)s_origCueLvProc);
        if (s_cueRight && s_origCueLvProc)
            SetWindowLongPtr(s_cueRight, GWLP_WNDPROC, (LONG_PTR)s_origCueLvProc);
        s_origCueLvProc = nullptr;
        s_cueDrag       = {};
        s_cueLeft       = nullptr;
        s_cueRight      = nullptr;
        s_cueEditList   = nullptr;
        break;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == IDCANCEL) { EndDialog(hwnd, 0); return TRUE; }
        break;
    }
    }
    return FALSE;
}


// ---------------------------------------------------------------------------
// ShowContextMenu
// ---------------------------------------------------------------------------
static void ShowContextMenu(HWND hwnd, int item, POINT pt)
{
    // In cue mode, item is a cue list position; map to snapshot index
    int snapIdx = -1;
    bool isCueMode = g_cueMode;
    if (isCueMode)
    {
        if (item >= 0 && item < (int)g_cueList.size())
            snapIdx = g_cueList[item];
    }
    else
    {
        snapIdx = item;
    }

    bool hasItem   = (snapIdx >= 0 && snapIdx < (int)g_snapshots.size());
    bool isSpacer  = hasItem && g_snapshots[snapIdx]->m_isSpacer;
    bool hasClip   = (g_clipboard != nullptr);

    HMENU hMenu = CreatePopupMenu();

    if (isCueMode)
    {
        // Cue mode context menu: simpler
        AppendMenu(hMenu, MF_STRING | (!hasItem ? MF_GRAYED : 0), CTX_RECALL_CTX, "Recall");
        AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenu(hMenu, MF_STRING | ((!hasItem && snapIdx != -1) ? MF_GRAYED : 0), CTX_CUE_REMOVE, "Remove from Cue");
    }
    else
    {
        AppendMenu(hMenu, MF_STRING, CTX_NEW, "New");
        AppendMenu(hMenu, MF_STRING | ((!hasItem || isSpacer) ? MF_GRAYED : 0), CTX_RECALL_CTX, "Recall");
        AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenu(hMenu, MF_STRING | ((!hasItem || isSpacer) ? MF_GRAYED : 0), CTX_COPY_CTX,  "Copy");
        AppendMenu(hMenu, MF_STRING | (!hasClip ? MF_GRAYED : 0), CTX_PASTE_CTX, "Paste");
        AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenu(hMenu, MF_STRING | ((!hasItem || isSpacer) ? MF_GRAYED : 0), CTX_OVERWRITE, "Overwrite");
        AppendMenu(hMenu, MF_STRING | ((!hasItem || isSpacer) ? MF_GRAYED : 0), CTX_SCENE_SETTINGS, "Scene Settings...");
        AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenu(hMenu, MF_STRING | (!hasItem ? MF_GRAYED : 0), CTX_DELETE, "Delete");
        AppendMenu(hMenu, MF_STRING | (g_snapshots.empty() ? MF_GRAYED : 0), CTX_DELETE_ALL, "Delete All Scenes");
        AppendMenu(hMenu, MF_STRING | ((!hasItem || isSpacer) ? MF_GRAYED : 0), CTX_EXPORT, "Export...");
        AppendMenu(hMenu, MF_STRING, CTX_IMPORT, "Import...");
        AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenu(hMenu, MF_STRING, CTX_ADDSPACER, "Add Spacer");
    }

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                              pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);

    switch (cmd)
    {
    case CTX_NEW:
        DoSave(hwnd);
        break;

    case CTX_RECALL_CTX:
        if (hasItem && !isSpacer) DoRecall(hwnd, item);
        break;

    case CTX_CUE_REMOVE:
        if (isCueMode && item >= 0 && item < (int)g_cueList.size())
        {
            g_cueList.erase(g_cueList.begin() + item);
            RefreshListView(hwnd);
        }
        break;

    case CTX_SCENE_SETTINGS:
        if (hasItem && !isSpacer)
        {
            TransitionSnapshot* snap = g_snapshots[snapIdx].get();
            SnapSettingsData d;
            d.notes    = snap->m_notes;
            d.duration = snap->m_duration;
            d.taper    = snap->m_taper;
            d.taperExp = snap->m_taperExp;
            d.instant  = (snap->m_duration == 0.0);
            if (DialogBoxParam(g_hInstance,
                               MAKEINTRESOURCE(IDD_SNAP_SETTINGS),
                               hwnd,
                               SnapSettingsDialogProc,
                               (LPARAM)&d) == IDOK)
            {
                snap->m_notes    = d.notes;
                snap->m_duration = d.duration;
                snap->m_taper    = d.taper;
                snap->m_taperExp = d.taperExp;
            }
        }
        break;

    case CTX_COPY_CTX:
        if (hasItem && !isSpacer)
            g_clipboard = std::make_unique<TransitionSnapshot>(*g_snapshots[snapIdx]);
        break;

    case CTX_PASTE_CTX:
        if (g_clipboard)
        {
            int insertAfter = hasItem ? snapIdx + 1 : (int)g_snapshots.size();
            auto copy = std::make_unique<TransitionSnapshot>(*g_clipboard);
            copy->m_slot = insertAfter;
            if (copy->m_name.find(" (copy)") == std::string::npos)
                copy->m_name += " (copy)";
            copy->m_time = (int)std::time(nullptr);
            g_snapshots.insert(g_snapshots.begin() + insertAfter, std::move(copy));
            for (int i = 0; i < (int)g_snapshots.size(); i++) g_snapshots[i]->m_slot = i;
            RefreshListView(hwnd);
            HWND hList = GetDlgItem(hwnd, IDC_LIST);
            ListView_SetItemState(hList, insertAfter,
                LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(hList, insertAfter, FALSE);
            LoadEditorFromSnapshot(hwnd, g_snapshots[insertAfter].get());
            Undo_OnStateChangeEx("Paste Scene", -1, -1);
        }
        break;

    case CTX_OVERWRITE:
        if (hasItem && !isSpacer)
        {
            g_snapshots[snapIdx]->Capture(TS_CAPTURE_ALL);
            g_snapshots[snapIdx]->m_time = (int)std::time(nullptr);
            MarkTouched(snapIdx);
            RefreshListView(hwnd);
            Undo_OnStateChangeEx("Overwrite Scene", -1, -1);
        }
        break;

    case CTX_DELETE:
        if (hasItem)
        {
            // Remove from cue list too (update indices)
            g_cueList.erase(
                std::remove(g_cueList.begin(), g_cueList.end(), snapIdx),
                g_cueList.end());
            for (auto& ci : g_cueList)
                if (ci > snapIdx) ci--;
            g_snapshots.erase(g_snapshots.begin() + snapIdx);
            for (int i = 0; i < (int)g_snapshots.size(); i++) g_snapshots[i]->m_slot = i;
            RefreshListView(hwnd);
            LoadEditorFromSnapshot(hwnd, nullptr);
            Undo_OnStateChangeEx("Delete Scene", -1, -1);
        }
        break;

    case CTX_DELETE_ALL:
        if (!g_snapshots.empty())
        {
            if (MessageBoxA(hwnd, "Delete all scenes? This cannot be undone.",
                            "Delete All Scenes", MB_OKCANCEL | MB_ICONWARNING) == IDOK)
            {
                g_cueList.clear();
                g_snapshots.clear();
                RefreshListView(hwnd);
                LoadEditorFromSnapshot(hwnd, nullptr);
                Undo_OnStateChangeEx("Delete All Scenes", -1, -1);
            }
        }
        break;

    case CTX_EXPORT:
        if (hasItem && !isSpacer) ExportScene(hwnd, snapIdx);
        break;

    case CTX_IMPORT:
        ImportScene(hwnd);
        break;

    case CTX_RENAME:
        if (hasItem && !isSpacer)
            ListView_EditLabel(GetDlgItem(hwnd, IDC_LIST), item);
        break;

    case CTX_ADDSPACER:
    {
        int insertAfter = hasItem ? snapIdx + 1 : (int)g_snapshots.size();
        auto spacer = std::make_unique<TransitionSnapshot>(insertAfter, "");
        spacer->m_isSpacer = true;
        g_snapshots.insert(g_snapshots.begin() + insertAfter, std::move(spacer));
        for (int i = 0; i < (int)g_snapshots.size(); i++) g_snapshots[i]->m_slot = i;
        RefreshListView(hwnd);
        MarkProjectDirty(nullptr);
        break;
    }

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// ListSubclassProc – handles mouse events directly on the list view so that
// drag-and-drop reordering works reliably even when the window is docked.
// ---------------------------------------------------------------------------
static LRESULT CALLBACK ListSubclassProc(HWND hList, UINT msg,
                                          WPARAM wParam, LPARAM lParam)
{
    HWND dlg = GetParent(hList);

    switch (msg)
    {
    case WM_LBUTTONDOWN:
    {
        // Alt+click → delete scene (scenes mode only)
        if (g_altClickDelete && !g_cueMode)
        {
            LVHITTESTINFO htiMod = {};
            htiMod.pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int clickedMod = ListView_HitTest(hList, &htiMod);
            if ((GetKeyState(VK_MENU) & 0x8000) && clickedMod >= 0 &&
                clickedMod < (int)g_snapshots.size())
            {
                int snapIdx = clickedMod;
                g_cueList.erase(
                    std::remove(g_cueList.begin(), g_cueList.end(), snapIdx),
                    g_cueList.end());
                for (auto& ci : g_cueList)
                    if (ci > snapIdx) ci--;
                g_snapshots.erase(g_snapshots.begin() + snapIdx);
                for (int i = 0; i < (int)g_snapshots.size(); i++) g_snapshots[i]->m_slot = i;
                RefreshListView(dlg);
                LoadEditorFromSnapshot(dlg, nullptr);
                Undo_OnStateChangeEx("Delete Scene", -1, -1);
                return 0;
            }
        }
        // Ctrl+click → overwrite scene (scenes mode only, non-spacer)
        if (g_ctrlClickOverwrite && !g_cueMode && (wParam & MK_CONTROL))
        {
            LVHITTESTINFO htiMod = {};
            htiMod.pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int clickedMod = ListView_HitTest(hList, &htiMod);
            if (clickedMod >= 0 && clickedMod < (int)g_snapshots.size() &&
                !g_snapshots[clickedMod]->m_isSpacer)
            {
                g_snapshots[clickedMod]->Capture(TS_CAPTURE_ALL);
                g_snapshots[clickedMod]->m_time = (int)std::time(nullptr);
                RefreshListView(dlg);
                Undo_OnStateChangeEx("Overwrite Scene", -1, -1);
                return 0;
            }
        }
        // Let the list handle selection first
        LRESULT r = CallWindowProc(s_origListProc, hList, msg, wParam, lParam);
        // Record position for potential drag – do NOT capture yet (avoid
        // treating a plain click as a drag initiation).
        LVHITTESTINFO hti = {};
        hti.pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int item = ListView_HitTest(hList, &hti);
        if (item >= 0)
        {
            s_lbTracking  = true;
            s_lbDownPt    = hti.pt;
            s_lbDownItem  = item;
            s_lbDownTime  = GetTickCount();
        }
        return r;
    }

    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        // If the left button is no longer held (WM_LBUTTONUP was missed while
        // the cursor was outside the window and before capture was set), cancel
        // tracking immediately so no spurious drag can start.
        if (s_lbTracking && !(wParam & MK_LBUTTON))
            s_lbTracking = false;

        // Upgrade tracking to an active drag once threshold is crossed.
        // Require both a minimum distance AND a minimum hold time (200 ms)
        // so that a quick click never accidentally initiates a drag.
        if (s_lbTracking && g_dragSrc < 0)
        {
            bool movedEnough = (abs(pt.x - s_lbDownPt.x) > GetSystemMetrics(SM_CXDRAG) ||
                                abs(pt.y - s_lbDownPt.y) > GetSystemMetrics(SM_CYDRAG));
            bool heldLongEnough = (GetTickCount() - s_lbDownTime >= 200);
            if (movedEnough && heldLongEnough)
            {
                g_dragSrc    = s_lbDownItem;
                g_dragTarget = -1;
                s_lbTracking = false;
                SetCapture(hList);  // capture now that drag is confirmed

                POINT ptOffset = { 8, 8 };
#ifdef _WIN32
                g_hDragImages = ListView_CreateDragImage(hList, g_dragSrc, &ptOffset);
                if (g_hDragImages)
                {
                    POINT dlgPt = pt;
                    ClientToScreen(hList, &dlgPt);
                    ScreenToClient(dlg, &dlgPt);
                    ImageList_BeginDrag(g_hDragImages, 0, 8, 8);
                    ImageList_DragEnter(dlg, dlgPt.x, dlgPt.y);
                }
#endif
            }
        }

        // Drag in progress – update image and drop-highlight
        if (g_dragSrc >= 0)
        {
            POINT dlgPt = pt;
            ClientToScreen(hList, &dlgPt);
            ScreenToClient(dlg, &dlgPt);
#ifdef _WIN32
            if (g_hDragImages)
            {
                ImageList_DragMove(dlgPt.x, dlgPt.y);
                ImageList_DragShowNolock(FALSE);
            }
#endif
            LVHITTESTINFO hti = {};
            hti.pt = pt;
            int newTgt = ListView_HitTest(hList, &hti);
            if (newTgt != g_dragTarget)
            {
                g_dragTarget = newTgt;
                ListView_SetItemState(hList, -1, 0, LVIS_DROPHILITED);
                if (g_dragTarget >= 0)
                    ListView_SetItemState(hList, g_dragTarget,
                                         LVIS_DROPHILITED, LVIS_DROPHILITED);
            }
#ifdef _WIN32
            if (g_hDragImages)
                ImageList_DragShowNolock(TRUE);
#endif
            return 0;
        }
        break;
    }

    case WM_RBUTTONUP:
    {
        // Handle right-click directly here rather than relying on the
        // NM_RCLICK → WM_NOTIFY → WM_CONTEXTMENU chain, which is unreliable
        // when REAPER's message hook intercepts WM_CONTEXTMENU.
        LVHITTESTINFO hti = {};
        hti.pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int item = ListView_HitTest(hList, &hti);
        int safeItem = (item >= 0 && item < (int)g_snapshots.size()) ? item : -1;
        if (safeItem >= 0 && !g_snapshots[safeItem]->m_isSpacer)
        {
            ListView_SetItemState(hList, safeItem,
                LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            LoadEditorFromSnapshot(dlg, g_snapshots[safeItem].get());
        }
        POINT ptScreen = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hList, &ptScreen);
        // Set flag before ShowContextMenu so any REAPER-posted WM_CONTEXTMENU
        // that arrives during TrackPopupMenu's loop is eaten by the dialog proc.
        g_skipNextContextMenu = true;
        ShowContextMenu(dlg, safeItem, ptScreen);
        // Clear in case it wasn't consumed during the menu (e.g. no REAPER hook).
        g_skipNextContextMenu = false;
        return 0;   // prevent original proc from generating NM_RCLICK / WM_CONTEXTMENU
    }

    case WM_CONTEXTMENU:
    {
        // Forward keyboard-menu-key invocations (-1,-1) to the parent dialog
        // so the context menu still works from the keyboard.
        // Eat mouse-generated WM_CONTEXTMENU (we handle those via WM_RBUTTONUP).
        int cx = GET_X_LPARAM(lParam);
        int cy = GET_Y_LPARAM(lParam);
        if (cx == -1 && cy == -1)
            return SendMessage(dlg, WM_CONTEXTMENU, (WPARAM)hList, lParam);
        return 0;
    }

    case WM_LBUTTONUP:
    {
        const bool wasTracking = s_lbTracking;
        s_lbTracking = false;
        if (g_dragSrc >= 0)
        {
            ReleaseCapture();
            DoEndDrag(dlg);
            return 0;
        }
        // Single-click recall: fire on release when no drag, no modifiers,
        // and the mouse didn't move past the drag threshold (wasTracking).
        if (g_singleClickRecall && wasTracking && s_lbDownItem >= 0 &&
            s_lbDownItem < (int)g_snapshots.size() &&
            !g_snapshots[s_lbDownItem]->m_isSpacer &&
            !(GetKeyState(VK_MENU)    & 0x8000) &&
            !(GetKeyState(VK_CONTROL) & 0x8000) &&
            !(GetKeyState(VK_SHIFT)   & 0x8000))
        {
            DoRecall(dlg, s_lbDownItem);
            return 0;
        }
        break;
    }

    case WM_CAPTURECHANGED:
        s_lbTracking = false;
        if (g_dragSrc >= 0)
            DoEndDrag(dlg);
        break;

    case WM_KEYDOWN:
        if (wParam == VK_DELETE && !g_cueMode)
        {
            int selIdx = GetSelectedListIndex(dlg);
            if (selIdx >= 0 && selIdx < (int)g_snapshots.size())
            {
                // Remove from cue list too (update indices)
                g_cueList.erase(
                    std::remove(g_cueList.begin(), g_cueList.end(), selIdx),
                    g_cueList.end());
                for (auto& ci : g_cueList)
                    if (ci > selIdx) ci--;
                g_snapshots.erase(g_snapshots.begin() + selIdx);
                for (int i = 0; i < (int)g_snapshots.size(); i++) g_snapshots[i]->m_slot = i;
                RefreshListView(dlg);
                LoadEditorFromSnapshot(dlg, nullptr);
                Undo_OnStateChangeEx("Delete Scene", -1, -1);
                return 0;
            }
        }
        break;
    }

    return CallWindowProc(s_origListProc, hList, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// DialogProc
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS;
        InitCommonControlsEx(&icc);

        // ---- Create ListView dynamically ----------------------------------
        HWND hListPH = GetDlgItem(hwnd, IDC_LIST);
        RECT rList = {};
        GetWindowRect(hListPH, &rList);
        MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&rList, 2);
        DestroyWindow(hListPH);

        HWND hList = CreateWindowExA(0, "SysListView32", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
            LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | LVS_EDITLABELS,
            rList.left, rList.top,
            rList.right - rList.left, rList.bottom - rList.top,
            hwnd, (HMENU)(INT_PTR)IDC_LIST, g_hInstance, nullptr);

        if (hList)
        {
            ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

            LVCOLUMN col = {};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.cx = 28;  col.pszText = const_cast<char*>("#");
            ListView_InsertColumn(hList, 0, &col);
            col.cx = 110; col.pszText = const_cast<char*>("Name");
            ListView_InsertColumn(hList, 1, &col);
            col.cx = 75;  col.pszText = const_cast<char*>("Saved");
            ListView_InsertColumn(hList, 2, &col);

            // Subclass the list for reliable drag-drop mouse tracking
            s_origListProc = (WNDPROC)(LONG_PTR)SetWindowLongPtr(
                hList, GWLP_WNDPROC, (LONG_PTR)ListSubclassProc);
        }

        // ---- Create ProgressBar dynamically --------------------------------
        HWND hProgPH = GetDlgItem(hwnd, IDC_PROGRESS);
        RECT rProg = {};
        GetWindowRect(hProgPH, &rProg);
        MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&rProg, 2);
        DestroyWindow(hProgPH);

        HWND hProg = CreateWindowExA(0, "msctls_progressbar32", "",
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            rProg.left, rProg.top,
            rProg.right - rProg.left, rProg.bottom - rProg.top,
            hwnd, (HMENU)(INT_PTR)IDC_PROGRESS, g_hInstance, nullptr);
        if (hProg)
        {
            SendMessage(hProg, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            SendMessage(hProg, PBM_SETPOS,   0, 0);
        }

        // ---- Mode toggle initial state -----------------------------------
        CheckDlgButton(hwnd, IDC_MODE_SCENES, BST_CHECKED);
        CheckDlgButton(hwnd, IDC_MODE_CUE,    BST_UNCHECKED);

        // ---- Default transition settings are loaded per-project via
        //      TransitionWnd_ProcessSettingsLine (called from ProcessExtensionLine).

        // ---- Initial editor state ----------------------------------------
        LoadEditorFromSnapshot(hwnd, nullptr);

        // ---- Populate from already-loaded snapshots ----------------------
        RefreshListView(hwnd);

        // ---- Engine completion callback ----------------------------------
        TransitionEngine::Get().onTransitionComplete = [hwnd]() {
            PostMessage(hwnd, WM_USER + 1, 0, 0);
        };

        SetTimer(hwnd, UI_TIMER_ID, 100, nullptr);

        SetDlgItemTextA(hwnd, IDC_SAVE, "New");

        // ---- Record initial layout for WM_SIZE --------------------------
        {
            RECT cr;
            GetClientRect(hwnd, &cr);
            g_initCx = cr.right;
            g_initCy = cr.bottom;

            HWND hListSz = GetDlgItem(hwnd, IDC_LIST);
            if (hListSz)
            {
                GetWindowRect(hListSz, &g_listInitRect);
                MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&g_listInitRect, 2);
            }

            int threshold = g_listInitRect.right - 5;
            g_sidebarCtrls.clear();
            g_leftCtrls.clear();
            HWND hChild = GetWindow(hwnd, GW_CHILD);
            while (hChild)
            {
                RECT r;
                GetWindowRect(hChild, &r);
                MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&r, 2);
                // The divider sits past the threshold too, but it is placed
                // by LayoutMain rather than carried along with the sidebar.
                int childId = GetDlgCtrlID(hChild);
                SidebarCtrl sc;
                sc.hwnd     = hChild;
                sc.origLeft = r.left;
                sc.origTop  = r.top;
                sc.w        = r.right  - r.left;
                sc.h        = r.bottom - r.top;

                if (r.left > threshold && childId != IDC_SPLITTER)
                    g_sidebarCtrls.push_back(sc);
                else if (childId != IDC_SPLITTER && childId != IDC_LIST &&
                         childId > 0)
                    g_leftCtrls.push_back(sc);
                hChild = GetWindow(hChild, GW_HWNDNEXT);
            }

            // ---- Column divider ---------------------------------------
            // The sidebar's design-size bounding box is what LayoutMain
            // maps each control's slot out of.
            g_sbInitLeft  = 0;
            g_sbInitRight = 0;
            for (const auto& sc : g_sidebarCtrls)
            {
                if (!g_sbInitRight || sc.origLeft < g_sbInitLeft)
                    g_sbInitLeft = sc.origLeft;
                if (sc.origLeft + sc.w > g_sbInitRight)
                    g_sbInitRight = sc.origLeft + sc.w;
            }

            HWND hSplit = GetDlgItem(hwnd, IDC_SPLITTER);
            if (hSplit)
            {
                GetWindowRect(hSplit, &g_splitInitRect);
                MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&g_splitInitRect, 2);
                g_splitOldProc = (WNDPROC)SetWindowLongPtr(
                    hSplit, GWLP_WNDPROC, (LONG_PTR)SplitterProc);
            }

            // ---- Version footer ---------------------------------------
            HWND hVer = GetDlgItem(hwnd, IDC_VERSION);
            if (hVer)
            {
                SetDlgItemText(hwnd, IDC_VERSION, LT_VERSION_STR);
                GetWindowRect(hVer, &g_versionInitRect);
                MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&g_versionInitRect, 2);
                LayoutVersion(hwnd);
            }

            // ---- Notes resizer ----------------------------------------
            HWND hNotes = GetDlgItem(hwnd, IDC_SNAPNOTES);
            HWND hGrip  = GetDlgItem(hwnd, IDC_NOTES_GRIP);
            if (hNotes && hGrip)
            {
                GetWindowRect(hNotes, &g_notesInitRect);
                MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&g_notesInitRect, 2);
                GetWindowRect(hGrip, &g_gripInitRect);
                MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&g_gripInitRect, 2);

                g_gripOldProc = (WNDPROC)SetWindowLongPtr(
                    hGrip, GWLP_WNDPROC, (LONG_PTR)NotesGripProc);

                if (!g_notesAccelRegistered)
                {
                    memset(&g_notesAccel, 0, sizeof(g_notesAccel));
                    g_notesAccel.translateAccel = NotesTranslateAccel;
                    g_notesAccel.isLocal        = true;
                    plugin_register("accelerator", &g_notesAccel);
                    g_notesAccelRegistered = true;
                }
            }

            // The window may already have been resized to the saved rect.
            LayoutMain(hwnd);
        }
        return TRUE;
    }

    case WM_GETMINMAXINFO:
    {
        if (g_initCx > 0)
        {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            RECT r = { 0, 0, g_initCx, g_initCy };
            AdjustWindowRectEx(&r,
                (DWORD)GetWindowLong(hwnd, GWL_STYLE),
                FALSE,
                (DWORD)GetWindowLong(hwnd, GWL_EXSTYLE));
            mmi->ptMinTrackSize.x = r.right  - r.left;
            mmi->ptMinTrackSize.y = r.bottom - r.top;
        }
        return 0;
    }

    case WM_SIZE:
    {
        int newCx = (int)(short)LOWORD(lParam);
        int newCy = (int)(short)HIWORD(lParam);
        if (g_initCx <= 0 || newCx <= 0 || newCy <= 0) break;

        LayoutMain(hwnd);
        break;
    }

    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
        if (dis && dis->CtlID == IDC_SPLITTER)
        {
            // A single hairline down the gutter: enough to read as a divider
            // without drawing a heavy bar between the two columns.
            FillRect(dis->hDC, &dis->rcItem, GetSysColorBrush(COLOR_BTNFACE));
            int cx = (dis->rcItem.left + dis->rcItem.right) / 2;
            HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNSHADOW));
            HGDIOBJ old = SelectObject(dis->hDC, pen);
            MoveToEx(dis->hDC, cx, dis->rcItem.top + 2, nullptr);
            LineTo  (dis->hDC, cx, dis->rcItem.bottom - 2);
            SelectObject(dis->hDC, old);
            DeleteObject(pen);
            return TRUE;
        }
        if (dis && dis->CtlID == IDC_NOTES_GRIP)
        {
            // Two short rules centred in the strip, the usual "drag me" cue.
            FillRect(dis->hDC, &dis->rcItem, GetSysColorBrush(COLOR_BTNFACE));
            int midY = (dis->rcItem.top + dis->rcItem.bottom) / 2;
            int cx   = (dis->rcItem.left + dis->rcItem.right) / 2;
            HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNSHADOW));
            HGDIOBJ old = SelectObject(dis->hDC, pen);
            for (int i = 0; i < 2; i++)
            {
                MoveToEx(dis->hDC, cx - 14, midY - 1 + i * 2, nullptr);
                LineTo  (dis->hDC, cx + 14, midY - 1 + i * 2);
            }
            SelectObject(dis->hDC, old);
            DeleteObject(pen);
            return TRUE;
        }
        break;
    }

    case WM_TIMER:
        if (wParam == UI_TIMER_ID)
        {
            TransitionEngine& eng = TransitionEngine::Get();

            HWND hProg = GetDlgItem(hwnd, IDC_PROGRESS);
            int  pct   = (int)(eng.GetProgress() * 100.0 + 0.5);
            if (pct > 100) pct = 100;
            SendMessage(hProg, PBM_SETPOS, (WPARAM)pct, 0);

            SetDlgItemText(hwnd, IDC_STATUS, eng.GetStatus());

            // Update layer status indicator
            {
                char layerBuf[128] = "Layer: -";
                int activeLyr = LayersEngine::Get().GetActiveLayer();
                if (activeLyr >= 0 && activeLyr < LayersEngine::Get().GetLayerCount())
                    snprintf(layerBuf, sizeof(layerBuf), "Layer: %s",
                             LayersEngine::Get().GetLayer(activeLyr).name);
                SetDlgItemText(hwnd, IDC_LAYER_STATUS, layerBuf);
            }
        }
        return TRUE;

    case WM_USER + 1:
        {
            HWND hProg = GetDlgItem(hwnd, IDC_PROGRESS);
            SendMessage(hProg, PBM_SETPOS, 100, 0);
            SetDlgItemText(hwnd, IDC_STATUS, TransitionEngine::Get().GetStatus());
        }
        return TRUE;

    case WM_MOUSEMOVE:
        // Drag is handled by the list subclass proc.
        break;

    case WM_LBUTTONUP:
        // Drag is handled by the list subclass proc.
        break;

    case WM_COMMAND:
    {
        int id  = LOWORD(wParam);
        int evt = HIWORD(wParam);

        // ---- Snapshot editor live-update handlers -----------------------
        if (id == IDC_SNAPNOTES && evt == EN_CHANGE && !g_syncingEditor)
        {
            int idx = GetSelectedListIndex(hwnd);
            if (idx >= 0 && idx < (int)g_snapshots.size())
            {
                char buf[4096] = {};
                GetDlgItemText(hwnd, IDC_SNAPNOTES, buf, sizeof(buf));
                g_snapshots[idx]->m_notes = NotesFromControl(buf);
            }
            return TRUE;
        }

        if (id == IDC_SNAP_LAYER && evt == CBN_SELCHANGE && !g_syncingEditor)
        {
            int idx = GetSelectedListIndex(hwnd);
            if (idx >= 0 && idx < (int)g_snapshots.size() && !g_snapshots[idx]->m_isSpacer)
            {
                TransitionSnapshot* snap = g_snapshots[idx].get();
                int sel = (int)SendDlgItemMessage(hwnd, IDC_SNAP_LAYER, CB_GETCURSEL, 0, 0);
                int uid = (sel < 0) ? 0
                        : (int)SendDlgItemMessage(hwnd, IDC_SNAP_LAYER, CB_GETITEMDATA,
                                                  (WPARAM)sel, 0);
                if (uid < 0) uid = 0;   // CB_ERR

                // Picking a layer the scene never captured (created after the
                // scene was saved) means its stored layer set is out of date.
                // Refresh it now, or recall would have nothing to apply.
                if (uid > 0 && FindCapturedLayerByUid(snap, uid) < 0)
                    CaptureLayersFromEngine(snap);

                snap->m_layerUid = uid;
                snap->m_layerIdx = FindCapturedLayerByUid(snap, uid);  // old-format compat
                MarkProjectDirty(nullptr);
            }
            return TRUE;
        }

        // ---- Button / checkbox handlers ----------------------------------
        switch (id)
        {
        case IDC_SAVE:
            DoSave(hwnd);
            break;

        case IDC_RECALL:
            DoRecall(hwnd, GetSelectedListIndex(hwnd));
            break;

        case IDC_SETTINGS_BTN:
            // Opens global default transition settings
            DialogBoxParam(g_hInstance, MAKEINTRESOURCE(IDD_GLOBAL_SETTINGS),
                           hwnd, GlobalSettingsDialogProc, 0);
            break;

        case IDC_LAYERS_BTN:
            LayersWnd_ShowHide();
            break;

        case IDC_CUE_SETUP_BTN:
        {
            // Pass g_cueList directly so all edits are applied live
            DialogBoxParam(g_hInstance, MAKEINTRESOURCE(IDD_CUE_SETUP),
                           hwnd, CueSetupDialogProc,
                           (LPARAM)&g_cueList);
            // Cue list was updated in place; refresh if cue mode is active
            if (g_cueMode) RefreshListView(hwnd);
            break;
        }

        case IDC_SAFES_BTN:
            SafesWnd_ShowHide();
            break;

        case IDC_MODE_SCENES:
            g_cueMode = false;
            CheckDlgButton(hwnd, IDC_MODE_SCENES, BST_CHECKED);
            CheckDlgButton(hwnd, IDC_MODE_CUE,    BST_UNCHECKED);
            RefreshListView(hwnd);
            break;

        case IDC_MODE_CUE:
            g_cueMode = true;
            CheckDlgButton(hwnd, IDC_MODE_SCENES, BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_MODE_CUE,    BST_CHECKED);
            RefreshListView(hwnd);
            break;

        default:
            (void)evt;
            break;
        }
        return TRUE;
    }

    case WM_NOTIFY:
    {
        NMHDR* hdr = (NMHDR*)lParam;
        if (hdr->idFrom == IDC_LIST)
        {
            if (hdr->code == NM_DBLCLK)
            {
                // Skip double-click recall when single-click recall is active
                // (single-click already fired on the first button release)
                if (!g_singleClickRecall)
                {
                    NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
                    if (nia->iItem >= 0) DoRecall(hwnd, nia->iItem);
                }
            }
            else if (hdr->code == LVN_ITEMCHANGED)
            {
                NMLISTVIEW* nlv = (NMLISTVIEW*)lParam;
                if ((nlv->uNewState & LVIS_SELECTED) && nlv->iItem >= 0 &&
                    nlv->iItem < (int)g_snapshots.size())
                {
                    if (!g_snapshots[nlv->iItem]->m_isSpacer)
                        LoadEditorFromSnapshot(hwnd, g_snapshots[nlv->iItem].get());
                    else
                        LoadEditorFromSnapshot(hwnd, nullptr);
                }
            }
            else if (hdr->code == LVN_BEGINLABELEDIT)
            {
                // Block label editing on spacer rows
                NMLVDISPINFO* di = (NMLVDISPINFO*)lParam;
                if (di->item.iItem >= 0 && di->item.iItem < (int)g_snapshots.size() &&
                    g_snapshots[di->item.iItem]->m_isSpacer)
                {
                    SetWindowLongPtr(hwnd, DWLP_MSGRESULT, TRUE);
                    return TRUE;
                }
            }
            else if (hdr->code == NM_RCLICK)
            {
                // Handle right-click here (before WM_CONTEXTMENU is generated).
                // Returning TRUE prevents the ListView from generating WM_CONTEXTMENU,
                // so REAPER's hook never intercepts it and shows the dock menu.
                NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
                int item = (nia->iItem >= 0 && nia->iItem < (int)g_snapshots.size())
                               ? nia->iItem : -1;
                // ptAction is in list-client coordinates
                POINT ptScreen = nia->ptAction;
                ClientToScreen(hdr->hwndFrom, &ptScreen);

                if (item >= 0 && !g_snapshots[item]->m_isSpacer)
                {
                    HWND hListN = GetDlgItem(hwnd, IDC_LIST);
                    ListView_SetItemState(hListN, item,
                        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    LoadEditorFromSnapshot(hwnd, g_snapshots[item].get());
                }
                // Set flag before ShowContextMenu so any REAPER-posted
                // WM_CONTEXTMENU arriving during TrackPopupMenu is eaten.
                g_skipNextContextMenu = true;
                ShowContextMenu(hwnd, item, ptScreen);
                g_skipNextContextMenu = false;  // clear if not consumed during menu

                // Return non-zero → ListView skips WM_CONTEXTMENU generation
                SetWindowLongPtr(hwnd, DWLP_MSGRESULT, TRUE);
                return TRUE;
            }
            else if (hdr->code == LVN_BEGINDRAG)
            {
                // Drag is handled by the list subclass proc; nothing to do here.
                (void)lParam;
            }
            else if (hdr->code == NM_CUSTOMDRAW)
            {
                NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lParam;
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
                {
                    SetWindowLongPtr(hwnd, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                    return TRUE;
                }
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
                {
                    int itm = (int)cd->nmcd.dwItemSpec;
                    if (itm >= 0 && itm < (int)g_snapshots.size() &&
                        g_snapshots[itm]->m_isSpacer)
                    {
                        cd->clrTextBk = GetSysColor(COLOR_BTNFACE);
                        cd->clrText   = GetSysColor(COLOR_GRAYTEXT);
                        SetWindowLongPtr(hwnd, DWLP_MSGRESULT, CDRF_NEWFONT);
                        return TRUE;
                    }
                    SetWindowLongPtr(hwnd, DWLP_MSGRESULT, CDRF_DODEFAULT);
                    return TRUE;
                }
                SetWindowLongPtr(hwnd, DWLP_MSGRESULT, CDRF_DODEFAULT);
                return TRUE;
            }
            else if (hdr->code == LVN_ENDLABELEDIT)
            {
                NMLVDISPINFO* di = (NMLVDISPINFO*)lParam;
                if (di->item.pszText && di->item.iItem >= 0 &&
                    di->item.iItem < (int)g_snapshots.size())
                {
                    g_snapshots[di->item.iItem]->m_name = di->item.pszText;
                    SetWindowLongPtr(hwnd, DWLP_MSGRESULT, TRUE);
                    RefreshListView(hwnd);
                    MarkProjectDirty(nullptr);
                }
                return TRUE;
            }
        }
        return FALSE;
    }

    case WM_CONTEXTMENU:
    {
        int  x = GET_X_LPARAM(lParam);
        int  y = GET_Y_LPARAM(lParam);
        HWND hListCtx = GetDlgItem(hwnd, IDC_LIST);

        // If NM_RCLICK already showed our scene menu, eat any WM_CONTEXTMENU
        // that REAPER's hook may have queued before NM_RCLICK was processed.
        if (g_skipNextContextMenu)
        {
            g_skipNextContextMenu = false;
            return TRUE;
        }

        // Detect whether the click was over the list by screen position.
        // wParam may be the dialog rather than the list (REAPER routing), so
        // also check IsChild in case WindowFromPoint hits the header control.
        bool onList = ((HWND)wParam == hListCtx || IsChild(hListCtx, (HWND)wParam));
        if (!onList && x != -1 && y != -1)
        {
            POINT pt = { x, y };
            HWND atPt = WindowFromPoint(pt);
            onList = (atPt == hListCtx || IsChild(hListCtx, atPt));
        }

        if (onList)
        {
            POINT ptClient = { x, y };
            if (x == -1 || y == -1)
            {
                // Keyboard menu key — use selected item position
                int sel = GetSelectedListIndex(hwnd);
                RECT r = {};
                if (sel >= 0) ListView_GetItemRect(hListCtx, sel, &r, LVIR_BOUNDS);
                ptClient = { r.left + 4, (r.top + r.bottom) / 2 };
                ClientToScreen(hListCtx, &ptClient);
                x = ptClient.x; y = ptClient.y;
                ptClient = { r.left + 4, (r.top + r.bottom) / 2 };
            }
            else
            {
                ScreenToClient(hListCtx, &ptClient);
            }
            LVHITTESTINFO hti = {};
            hti.pt = ptClient;
            int item = ListView_HitTest(hListCtx, &hti);
            // Select the item under the cursor
            if (item >= 0)
            {
                ListView_SetItemState(hListCtx, item,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                if (item < (int)g_snapshots.size())
                    LoadEditorFromSnapshot(hwnd, g_snapshots[item].get());
            }
            POINT ptScreen = { x, y };
            g_skipNextContextMenu = true;
            ShowContextMenu(hwnd, item, ptScreen);
            g_skipNextContextMenu = false;
            return TRUE;
        }

        // Right-click on title bar / window background → dock/close menu
        if (x == -1 || y == -1)
        {
            RECT r;
            GetWindowRect(hwnd, &r);
            x = r.left; y = r.top;
        }
        HMENU hMenu = CreatePopupMenu();
        bool isFloat = false;
        bool docked  = (DockIsChildOfDock(hwnd, &isFloat) >= 0);
        AppendMenuA(hMenu, MF_STRING | (docked ? MF_CHECKED : 0), CTX_DOCK,  "Dock Scenes in Docker");
        AppendMenuA(hMenu, MF_STRING, CTX_CLOSE, "Close window");
        int id = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, hwnd, nullptr);
        DestroyMenu(hMenu);
        if (id == CTX_DOCK)
            ToggleDocking();
        else if (id == CTX_CLOSE)
            DestroyWindow(hwnd);
        return TRUE;
    }

    case WM_CLOSE:
        // Actually close (and, if docked, unregister from the docker) rather than just
        // hiding — matches the native "x" close behaviour REAPER expects from dockable
        // windows. WM_DESTROY below persists the dock-state pref and calls
        // DockWindowRemove before g_wnd is cleared, so re-opening restores the same state.
        DestroyWindow(hwnd);
        return TRUE;

    case WM_DESTROY:
        KillTimer(hwnd, UI_TIMER_ID);
        // Restore list subclass
        if (s_origListProc)
        {
            HWND hListD = GetDlgItem(hwnd, IDC_LIST);
            if (hListD) SetWindowLongPtr(hListD, GWLP_WNDPROC, (LONG_PTR)s_origListProc);
            s_origListProc = nullptr;
        }
        if (!g_suppressDockStateSave)
        {
            bool isFloat = false;
            bool wasDocked = (DockIsChildOfDock(hwnd, &isFloat) >= 0);
            SetExtState("reaper_transitions", "scenes_docked", wasDocked ? "1" : "0", true);
            if (wasDocked)
                DockWindowRemove(hwnd);
        }
        else if (DockIsChildOfDock(hwnd, nullptr) >= 0)
        {
            DockWindowRemove(hwnd);
        }
        if (g_notesAccelRegistered)
        {
            plugin_register("-accelerator", &g_notesAccel);
            g_notesAccelRegistered = false;
        }
        g_gripOldProc  = nullptr;
        g_splitOldProc = nullptr;
        TransitionEngine::Get().onTransitionComplete = nullptr;
        g_wnd = nullptr;
        return TRUE;

    default:
        break;
    }

    return FALSE;
}
