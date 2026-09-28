#include "TransitionWnd.h"
#include "TransitionEngine.h"
#include "ChunkRecallList.h"
#include "SafesWnd.h"
#include "LayersEngine.h"
#include "../layers/LayersWnd.h"
#include "api.h"
#include "resource.h"
#include "ReaperTheme.h"

#ifdef _WIN32
#  include <commctrl.h>
#  include <commdlg.h>
#  include <windowsx.h>
#else
// SWELL has no GetKeyState; the async form reports the same modifiers
// (VK_CONTROL is Cmd, VK_MENU is Option).
#  define GetKeyState GetAsyncKeyState
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
//
// This is a position, not an identity, so every edit that moves rows around has
// to move it too: without the fix-ups below it kept its old number and quietly
// started pointing at whatever scene slid into that slot, so the action
// overwrote the wrong scene after a delete, a drag-reorder or a paste.
static int g_lastTouchedIdx = -1;
static void MarkTouched(int idx)
{
    if (idx >= 0 && idx < (int)g_snapshots.size())
        g_lastTouchedIdx = idx;
}

// Call immediately BEFORE erasing row idx from g_snapshots.
static void TouchedOnErase(int idx)
{
    if      (g_lastTouchedIdx == idx) g_lastTouchedIdx = -1;
    else if (g_lastTouchedIdx >  idx) g_lastTouchedIdx--;
}

// Call immediately AFTER inserting a row at idx in g_snapshots.
static void TouchedOnInsert(int idx)
{
    if (g_lastTouchedIdx >= idx) g_lastTouchedIdx++;
}

// Guard: set true when programmatically updating editor fields to prevent
// EN_CHANGE / CBN_SELCHANGE from writing back to the snapshot.
static bool g_syncingEditor = false;

// Bold copy of the scene list's font, used by NM_CUSTOMDRAW to mark the scene
// that was recalled last — the same treatment the active layer gets.
static HFONT g_sceneBoldFont = nullptr;

// Guard: when true, WM_DESTROY skips overwriting the dock-state pref (used by ToggleDocking)
static bool g_suppressDockStateSave = false;

// Per-project window state (loaded from LTSCENESWND line, applied in TransitionWnd_OnProjectLoad)
static bool s_hasSavedWndState = false;
static bool s_savedWndVisible  = false;
static bool s_savedWndDocked   = false;
static int  s_savedWndX = 0, s_savedWndY = 0, s_savedWndW = 0, s_savedWndH = 0;

// UI timer ID
static const UINT UI_TIMER_ID = 1;

// Row whose label is currently being edited in place, so the deferred
// reposition in WM_USER + 2 knows which cell to move the edit box over.
static int g_labelEditItem = -1;

// Where the in-place edit belongs (list-client coordinates). The list view
// re-sizes its edit box on every keystroke (EN_UPDATE), and it sizes it for the
// item label — column 0 — so without this the box jumps back left as soon as
// the user types. LabelEditProc holds the box to this rect instead.
static RECT    g_labelEditRect      = {};
static bool    g_labelEditRectValid = false;
static WNDPROC s_origLabelEditProc  = nullptr;

static LRESULT CALLBACK LabelEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC orig = s_origLabelEditProc;
    if (msg == WM_WINDOWPOSCHANGING && g_labelEditRectValid)
    {
        WINDOWPOS* wpos = (WINDOWPOS*)lp;
        if (!(wpos->flags & SWP_NOMOVE))
        {
            wpos->x = g_labelEditRect.left;
            wpos->y = g_labelEditRect.top;
        }
        if (!(wpos->flags & SWP_NOSIZE))
        {
            wpos->cx = g_labelEditRect.right - g_labelEditRect.left;
            wpos->cy = g_labelEditRect.bottom - g_labelEditRect.top;
        }
    }
    else if (msg == WM_NCDESTROY)
    {
        SetWindowLongPtr(h, GWLP_WNDPROC, (LONG_PTR)orig);
        s_origLabelEditProc  = nullptr;
        g_labelEditRectValid = false;
    }
    return CallWindowProc(orig, h, msg, wp, lp);
}

// Pending selection restore for a scene that was just added: the row the
// rename box is open on, and the row that was selected before the add. The
// restore is deferred to WM_USER + 3 (posted from LVN_ENDLABELEDIT) because
// touching a row's state while the in-place edit is open closes the box.
static int g_addRestoreRow = -1;
static int g_addRestoreSel = -1;

// Context menu item IDs
enum { CTX_RENAME = 100, CTX_OVERWRITE, CTX_DELETE,
       CTX_NEW, CTX_RECALL_CTX, CTX_COPY_CTX, CTX_PASTE_CTX,
       CTX_EXPORT, CTX_IMPORT, CTX_ADDSPACER, CTX_SCENE_SETTINGS,
       CTX_CUE_REMOVE, CTX_DELETE_ALL,
       CTX_ADDSUB, CTX_SCENE_SAFES, CTX_PROMOTE,
       CTX_TOGGLE_FOLD, CTX_COLLAPSE_ALL, CTX_EXPAND_ALL };

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
// Recall settings (non-static so TransitionEngine.cpp can extern it)
bool g_skipUnchangedParams        = false;  // skip writing params that haven't changed
bool g_durationDebug              = false;  // print step-timing report to REAPER console on recall
bool g_chunkAllInstant            = false;  // capture+restore all plugins by chunk on instant path
bool g_recallLog                  = false;  // write a per-recall trace to live_tools_recall.log

// Global default transition settings for newly created scenes
static double g_defaultDuration = 0.0;
static int    g_defaultTaper    = TAPER_SCURVE;
static double g_defaultTaperExp = 2.0;

// The same, for newly created subscenes. Separate because a subscene is a
// small move within a song rather than a change of scene, and usually wants a
// different (often shorter) fade than the scene changes around it.
static double g_defaultSubDuration = 0.0;
static int    g_defaultSubTaper    = TAPER_SCURVE;
static double g_defaultSubTaperExp = 2.0;

// Drag-drop state
static int        g_dragSrc     = -1;
// Where a dragged scene will land, as a gap between rows: gap N is just above
// row N, and gap == row count is after the last row. -1 while not over the
// list. Shown as a bar across the list rather than a highlighted row.
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
// The notes box is last in the sidebar stack and by default fills everything
// down to the footer, at any window size. IDC_NOTES_GRIP lets the user pull
// its bottom edge up; g_notesShrink is how many pixels short of the footer it
// then stops, and is saved with the rest of the window state. Dragging the
// grip back down to the footer returns it to filling.
static int     g_notesShrink    = 0;
static RECT    g_notesInitRect  = {};   // client coords, recorded at WM_INITDIALOG
static RECT    g_gripInitRect   = {};
static WNDPROC g_gripOldProc    = nullptr;
static bool    g_gripDragging   = false;
static int     g_gripDragY0     = 0;
static int     g_gripDragShrink0 = 0;
static void    LayoutNotes(HWND hwnd);

// Footer: the status line and the version, stacked at the very bottom of the
// sidebar. Unlike the rest of the column they track the bottom of the client
// area rather than keeping their y, so they stay last at any window size.
static RECT    g_versionInitRect = {};
static RECT    g_statusInitRect  = {};
static RECT    g_progInitRect    = {};   // progress bar, the footer's top row
static RECT    g_layerInitRect   = {};   // "Layer: ..." line, just above the status
static void    LayoutFooter(HWND hwnd);
static int     FooterStatusTop(HWND hwnd);

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
static void DoSaveAt(HWND hwnd, int insertAt, bool asSubscene);
static void DoAddSubscene(HWND hwnd, int snapIdx);
static bool AddSubsceneToCurrent(HWND hwnd);
static int  SelectedSnapshotIndex(HWND hwnd, int* rowOut);
static void ExportScene(HWND hwnd, int item);
static void ImportScene(HWND hwnd);
static void DoEndDrag(HWND hwnd);
static TransitionSnapshot* GetSelectedSnapshot(HWND hwnd);
static int  GetSelectedListIndex(HWND hwnd);
static int  GetSelectedSnapIndex(HWND hwnd);
static std::vector<int> GetSelectedListIndices(HWND hwnd);
// Row <-> g_snapshots mapping; see the block above GetSelectedListIndex.
static void RebuildSceneNumbers();
static const char* SceneNumber(int idx);
static std::string CueNameLabel(int idx);
static int  RowToSnap(int row);
static int  SnapToRow(int snapIdx);
static bool IsHiddenByCollapse(int idx);
static void DeleteSnapshotsAt(HWND hwnd, const std::vector<int>& idxs);
static void RestoreSelectionAfterAdd(HWND hwnd);
static LRESULT CALLBACK ListSubclassProc(HWND hList, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK CueLvSubclassProc(HWND hList, UINT msg, WPARAM wParam, LPARAM lParam);
static void RefillCueRightList(HWND hRight, const std::vector<int>& list);
static void RestoreLayerState(TransitionSnapshot* snap);
static void EnsureLayerUids(TransitionSnapshot* snap);
static void ResolveSceneLayer(TransitionSnapshot* snap);
static int  SceneRecallLayerIndex(const TransitionSnapshot* snap);
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
    // Callers (the meter bridge's delta display) index g_snapshots with this,
    // so it owes them a snapshot index, not the row it happens to sit on.
    if (!g_wnd || !IsWindow(g_wnd)) return -1;
    return GetSelectedSnapIndex(g_wnd);
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

    // Same per-recall safes overlay the windowed path installs — a scene
    // recalled from a key, MIDI or OSC binding has to obey the same rules.
    SceneSafeScope safeScope(&snap->m_safes,
                             snap->m_isSub);
    if (g_placeMarker)
    {
        double pos = GetPlayPosition();
        AddProjectMarker2(nullptr, false, pos, 0.0, snap->m_name.c_str(), -1, 0);
    }
    // Stop recording before recall (marker placed first to capture correct play position)
    if (g_stopRecBeforeRecall && (GetPlayState() & 4))
        Main_OnCommand(1016, 0);  // Stop transport

    // Strip TS_VIS when a layer is active (layers manage visibility)
    int effectiveMask = snap->m_mask;
    if (SceneRecallLayerIndex(snap) >= 0)
        effectiveMask &= ~TS_VIS;
    TransitionEngine::Get().Recall(snap, effectiveMask, duration);
    TransitionEngine::Get().SetCurrentSlot(index);
    MarkTouched(index);
    if (g_wnd && IsWindow(g_wnd))
        InvalidateRect(GetDlgItem(g_wnd, IDC_LIST), nullptr, FALSE);
    // Restore full layer state (always, unless TS_LAYERS safe bit is set)
    RestoreLayerState(snap);
    if (g_wnd && IsWindow(g_wnd))
    {
        HWND hList  = GetDlgItem(g_wnd, IDC_LIST);
        const int r = SnapToRow(index);
        if (r >= 0)
        {
            ListView_SetItemState(hList, r, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(hList, r, FALSE);
        }
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
    // Capture() leaves m_time alone, so without this the Time column still
    // showed the original capture and the action looked like it had done
    // nothing at all — the only feedback a re-capture ever gets. The context
    // menu's Overwrite and Ctrl+click already stamped it; the actions did not.
    g_snapshots[index]->m_time = (int)std::time(nullptr);
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

void TransitionWnd_AddSubscene()
{
    HWND hwnd = EnsureWndOpen();
    if (!hwnd) return;
    AddSubsceneToCurrent(hwnd);
}

// The list is multi-select, so "the selection" can be several rows. Both
// helpers below act on the first (topmost) selected row — the alternative,
// acting on the whole selection, has no sensible meaning for a recall.
//
// What comes back from the list is a *row*, and in cue mode a row is a
// position in g_cueList, not an index into g_snapshots. Mapping it is the
// reason these go through DoRecall / the mapping below rather than treating
// the row as a snapshot index, which recalled and overwrote the wrong scene
// whenever cue mode was on.
static int SelectedSnapshotIndex(HWND hwnd, int* rowOut)
{
    int row = GetSelectedListIndex(hwnd);
    if (rowOut) *rowOut = row;
    if (row < 0) return -1;
    int snapIdx = g_cueMode
                  ? ((row < (int)g_cueList.size()) ? g_cueList[row] : -1)
                  : RowToSnap(row);
    if (snapIdx < 0 || snapIdx >= (int)g_snapshots.size()) return -1;
    if (g_snapshots[snapIdx]->m_isSpacer) return -1;
    return snapIdx;
}

void TransitionWnd_RecallSelectedScene()
{
    // The ListView holds the selection, so the window has to exist for there
    // to be one; open it on demand like the other headless helpers instead of
    // failing silently when it happens to be closed.
    HWND hwnd = EnsureWndOpen();
    if (!hwnd) return;
    int row = -1;
    if (SelectedSnapshotIndex(hwnd, &row) < 0) return;
    DoRecall(hwnd, row);   // same path as a click: cue mapping, UI refresh, cue advance
}

void TransitionWnd_UpdateSelectedScene()
{
    HWND hwnd = EnsureWndOpen();
    if (!hwnd) return;
    int snapIdx = SelectedSnapshotIndex(hwnd, nullptr);
    if (snapIdx < 0) return;
    TransitionWnd_OverwriteScene(snapIdx);
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
    // Skip spacers, and subscenes folded away under a collapsed parent: "next"
    // means the next row the performer can see.
    while (next < (int)g_snapshots.size() &&
           (g_snapshots[next]->m_isSpacer || IsHiddenByCollapse(next)))
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

// Loading or switching projects replaces g_snapshots wholesale, so a marker
// left over from the previous project would point into the new one's list.
void TransitionWnd_ResetTouchedScene()
{
    g_lastTouchedIdx = -1;
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
    g_defaultSubDuration = 0.0;
    g_defaultSubTaper    = TAPER_SCURVE;
    g_defaultSubTaperExp = 2.0;
    g_placeMarker        = false;
    g_stopRecBeforeRecall = false;
    g_startRecAfterRecall = false;
    g_singleClickRecall  = false;
    g_altClickDelete     = false;
    g_ctrlClickOverwrite = false;
    g_skipUnchangedParams = false;
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
    if (strncmp(line, "LTSUBDEFSETTINGS ", 17) == 0)
    {
        double dur = 0.0, taperExp = 2.0;
        int taper = TAPER_SCURVE;
        sscanf(line + 17, "%lf %d %lf", &dur, &taper, &taperExp);
        g_defaultSubDuration = (dur >= 0.0) ? dur : 0.0;
        g_defaultSubTaper    = (taper >= 0 && taper <= TAPER_CUSTOM) ? taper : TAPER_SCURVE;
        g_defaultSubTaperExp = (taperExp > 0.0) ? taperExp : 2.0;
        return true;
    }
    // Retired "store active layer" setting: recall now follows the active
    // layer index, so the line is only swallowed for older projects.
    if (strncmp(line, "LTSTOREACTIVELAYER ", 19) == 0)
        return true;
    // Retired "preload offline" and "shadow VST3 params" settings: swallowed
    // so older projects load cleanly.
    if (strncmp(line, "LTPRELOADOFFLINE ", 17) == 0
        || strncmp(line, "LTSHADOWPARAMS ", 15) == 0)
        return true;
    if (strncmp(line, "LTSKIPUNCHANGED ", 16) == 0)
    {
        int val = 0;
        sscanf(line + 16, "%d", &val);
        g_skipUnchangedParams = (val != 0);
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
        // Field 7 held how far the notes box had been dragged *past* its
        // default height, from before the box filled the column by default.
        // It is read and dropped, so an older project opens with the box
        // filling; field 9 is how far it has been pulled up since.
        int oldNotesExtra = 0, splitOffset = 0, notesShrink = 0;
        sscanf(line + 12, "%d %d %d %d %d %d %d %d %d",
               &docked, &visible, &x, &y, &w, &h, &oldNotesExtra, &splitOffset, &notesShrink);
        (void)oldNotesExtra;
        g_notesShrink = (notesShrink > 0) ? notesShrink : 0;  // clamped by LayoutNotes
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
    ctx->AddLine("LTSUBDEFSETTINGS %.4f %d %.4f",
                 g_defaultSubDuration, g_defaultSubTaper, g_defaultSubTaperExp);
    ctx->AddLine("LTSKIPUNCHANGED %d", g_skipUnchangedParams ? 1 : 0);
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
        ctx->AddLine("LTSCENESWND %d %d %d %d %d %d %d %d %d",
                     wndDocked ? 1 : 0, wndVisible ? 1 : 0,
                     wx, wy, ww, wh, 0, g_splitOffset, g_notesShrink);
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
// NotesFillHeight – the notes box height that reaches down to the footer.
// Stops the same small margin above the footer's top row that the version
// line keeps from the window edge.
// ---------------------------------------------------------------------------
static int FooterTop(HWND hwnd);

static int NotesFillHeight(HWND hwnd)
{
    if (g_initCy <= 0 || g_gripInitRect.bottom <= g_gripInitRect.top) return 0;
    if (g_versionInitRect.bottom <= g_versionInitRect.top) return 0;

    const int top = FooterTop(hwnd);
    if (top <= 0) return 0;
    const int pad   = (g_versionInitRect.bottom - g_versionInitRect.top) / 3;
    const int gripH = g_gripInitRect.bottom - g_gripInitRect.top;
    const int h     = (top - pad) - g_notesInitRect.top - gripH;
    return h > 0 ? h : 0;
}

// ---------------------------------------------------------------------------
// LayoutNotes – size the notes box to fill down to the footer, less whatever
// the user has pulled it up by, and reseat the grip under it. Runs after the
// WM_SIZE sidebar pass, so x/width are read from the live controls.
// ---------------------------------------------------------------------------
static void LayoutNotes(HWND hwnd)
{
    HWND hNotes = GetDlgItem(hwnd, IDC_SNAPNOTES);
    HWND hGrip  = GetDlgItem(hwnd, IDC_NOTES_GRIP);
    if (!hNotes || !hGrip) return;
    if (g_notesInitRect.bottom <= g_notesInitRect.top) return;

    // Never shorter than half its design height, however far it is pulled up.
    const int minH  = (g_notesInitRect.bottom - g_notesInitRect.top) / 2;
    const int fillH = NotesFillHeight(hwnd);
    int maxShrink = fillH - minH;
    if (maxShrink < 0) maxShrink = 0;
    if (g_notesShrink > maxShrink) g_notesShrink = maxShrink;
    if (g_notesShrink < 0)         g_notesShrink = 0;

    int h = fillH - g_notesShrink;
    if (h < minH) h = minH;

    RECT nr, gr;
    GetWindowRect(hNotes, &nr); MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&nr, 2);
    GetWindowRect(hGrip,  &gr); MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&gr, 2);
    const int gripH = g_gripInitRect.bottom - g_gripInitRect.top;

    SetWindowPos(hNotes, nullptr, nr.left, g_notesInitRect.top,
                 nr.right - nr.left, h, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(hGrip, nullptr, gr.left, g_notesInitRect.top + h,
                 gr.right - gr.left, gripH, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// FooterStatusTop – where LayoutFooter puts the status line for the current
// client height. This is the ceiling the notes box may be dragged up to.
// ---------------------------------------------------------------------------
static int FooterStatusTop(HWND hwnd)
{
    if (g_initCy <= 0) return 0;
    if (g_versionInitRect.bottom <= g_versionInitRect.top) return 0;
    if (g_statusInitRect.bottom  <= g_statusInitRect.top)  return 0;

    RECT cr;
    GetClientRect(hwnd, &cr);

    int verH   = g_versionInitRect.bottom - g_versionInitRect.top;
    int verGap = g_initCy - g_versionInitRect.bottom;          // gap below the version
    int verTop = cr.bottom - verGap - verH;

    int staH   = g_statusInitRect.bottom - g_statusInitRect.top;
    int staGap = g_versionInitRect.top - g_statusInitRect.bottom;  // gap between the two
    return verTop - staGap - staH;
}

// Footer rows above the status line, bottom-up: the layer indicator sits
// directly on the status line and the progress bar on the layer indicator.
// Each keeps the gap to the row below it that it has at the design size.
static int FooterLayerTop(HWND hwnd)
{
    const int staTop = FooterStatusTop(hwnd);
    if (staTop <= 0) return 0;
    if (g_layerInitRect.bottom <= g_layerInitRect.top) return staTop;
    const int h   = g_layerInitRect.bottom - g_layerInitRect.top;
    const int gap = g_statusInitRect.top - g_layerInitRect.bottom;
    return staTop - gap - h;
}

// FooterTop – the top of the footer's highest row (the progress bar), which
// is as far down as the notes box may reach.
static int FooterTop(HWND hwnd)
{
    const int below = FooterLayerTop(hwnd);
    if (below <= 0) return 0;
    if (g_progInitRect.bottom <= g_progInitRect.top) return below;
    const int belowInitTop = (g_layerInitRect.bottom > g_layerInitRect.top)
                             ? g_layerInitRect.top : g_statusInitRect.top;
    const int h   = g_progInitRect.bottom - g_progInitRect.top;
    const int gap = belowInitTop - g_progInitRect.bottom;
    return below - gap - h;
}

// ---------------------------------------------------------------------------
// LayoutFooter – keep the progress bar, layer indicator, status line and
// version pinned to the bottom of the sidebar. The LayoutMain sidebar pass restores their original y, so this runs
// after it.
// ---------------------------------------------------------------------------
static void LayoutFooter(HWND hwnd)
{
    HWND hVer = GetDlgItem(hwnd, IDC_VERSION);
    HWND hSta = GetDlgItem(hwnd, IDC_STATUS);
    if (!hVer || !hSta || g_initCy <= 0) return;
    if (g_versionInitRect.bottom <= g_versionInitRect.top) return;
    if (g_statusInitRect.bottom  <= g_statusInitRect.top)  return;

    RECT cr;
    GetClientRect(hwnd, &cr);

    int verH   = g_versionInitRect.bottom - g_versionInitRect.top;
    int verGap = g_initCy - g_versionInitRect.bottom;
    int verTop = cr.bottom - verGap - verH;
    if (verTop < g_versionInitRect.top) verTop = g_versionInitRect.top;

    int staH   = g_statusInitRect.bottom - g_statusInitRect.top;
    int staTop = FooterStatusTop(hwnd);
    if (staTop < g_statusInitRect.top) staTop = g_statusInitRect.top;

    // x/width come from the live controls: the sidebar pass has already
    // scaled them to the current column width.
    RECT vr, sr;
    GetWindowRect(hVer, &vr); MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&vr, 2);
    GetWindowRect(hSta, &sr); MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&sr, 2);

    SetWindowPos(hVer, nullptr, vr.left, verTop, vr.right - vr.left, verH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(hSta, nullptr, sr.left, staTop, sr.right - sr.left, staH,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    HWND hLayer = GetDlgItem(hwnd, IDC_LAYER_STATUS);
    if (hLayer && g_layerInitRect.bottom > g_layerInitRect.top)
    {
        const int h   = g_layerInitRect.bottom - g_layerInitRect.top;
        int       top = FooterLayerTop(hwnd);
        if (top < g_layerInitRect.top) top = g_layerInitRect.top;
        RECT lr;
        GetWindowRect(hLayer, &lr); MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&lr, 2);
        SetWindowPos(hLayer, nullptr, lr.left, top, lr.right - lr.left, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    HWND hProg = GetDlgItem(hwnd, IDC_PROGRESS);
    if (hProg && g_progInitRect.bottom > g_progInitRect.top)
    {
        const int progH   = g_progInitRect.bottom - g_progInitRect.top;
        int       progTop = FooterTop(hwnd);
        if (progTop < g_progInitRect.top) progTop = g_progInitRect.top;
        RECT pr;
        GetWindowRect(hProg, &pr); MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&pr, 2);
        SetWindowPos(hProg, nullptr, pr.left, progTop, pr.right - pr.left, progH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
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
    LayoutFooter(hwnd);
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
// NotesGripProc – subclass for IDC_NOTES_GRIP: pull the notes box's bottom
// edge up, or back down to the footer.
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
        g_gripDragShrink0 = g_notesShrink;
        SetCapture(h);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g_gripDragging)
        {
            POINT pt;
            GetCursorPos(&pt);
            g_notesShrink = g_gripDragShrink0 - (pt.y - g_gripDragY0);
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
// Subscene parentage
//
// A subscene belongs to the nearest ordinary scene above it in g_snapshots.
// The link is purely positional: nothing stores a parent id, so dragging a row
// reparents it, deleting a parent hands its children to whatever precedes
// them, and a copy/paste needs no fix-up at all. The cost is that these three
// helpers are the only definition of the relationship — everything that cares
// about it goes through them rather than re-deriving it.
// ---------------------------------------------------------------------------

// Index of the scene a row belongs to, or -1 when there is none (the row is
// itself a scene, is a spacer, or no scene precedes it).
static int ParentSceneIndex(int idx)
{
    if (idx < 0 || idx >= (int)g_snapshots.size()) return -1;
    if (!g_snapshots[idx]->m_isSub) return -1;
    for (int i = idx - 1; i >= 0; --i)
    {
        if (g_snapshots[i]->m_isSpacer) continue;
        if (!g_snapshots[i]->m_isSub)   return i;
    }
    return -1;
}

// One past the last row that belongs to the scene at parentIdx — where a new
// subscene of that scene goes, and the end of the range a delete takes with
// it. A spacer inside the block is decoration and does not end it, which is
// the same thing ParentSceneIndex says when it reads upwards.
static int SubsceneBlockEnd(int parentIdx)
{
    if (parentIdx < 0 || parentIdx >= (int)g_snapshots.size())
        return (int)g_snapshots.size();
    int end = parentIdx + 1;
    for (int i = parentIdx + 1; i < (int)g_snapshots.size(); ++i)
    {
        if (g_snapshots[i]->m_isSpacer) continue;
        if (!g_snapshots[i]->m_isSub)   break;
        end = i + 1;
    }
    return end;
}

// How many subscenes hang off the scene at idx. Counts subscenes, not rows —
// SubsceneBlockEnd's range can also contain spacers.
static int SubsceneCount(int idx)
{
    if (idx < 0 || idx >= (int)g_snapshots.size()) return 0;
    if (g_snapshots[idx]->m_isSpacer || g_snapshots[idx]->m_isSub) return 0;
    int n = 0;
    for (int i = idx + 1; i < (int)g_snapshots.size(); ++i)
    {
        if (g_snapshots[i]->m_isSpacer) continue;
        if (!g_snapshots[i]->m_isSub)   break;
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Scene numbering
//
// The number shown against a row: "3" for a scene, "3.2" for its second
// subscene, empty for a spacer. Computed for the whole list at once and kept
// here so the scene list, the cue list, the cue setup dialog and the menu
// labels all quote the same number — they each used to derive their own, and
// the cue list's was a raw g_snapshots index that matched nothing on screen.
// ---------------------------------------------------------------------------
static std::vector<std::string> g_sceneNumbers;

static void RebuildSceneNumbers()
{
    g_sceneNumbers.assign(g_snapshots.size(), std::string());
    int sceneNum = 0, subNum = 0;
    for (int i = 0; i < (int)g_snapshots.size(); ++i)
    {
        if (g_snapshots[i]->m_isSpacer) continue;

        char buf[32];
        if (g_snapshots[i]->m_isSub)
        {
            // A subscene above every scene has no parent to borrow from;
            // number it from 0 rather than pretending it belongs to scene 1.
            snprintf(buf, sizeof(buf), "%d.%d", sceneNum, ++subNum);
        }
        else
        {
            snprintf(buf, sizeof(buf), "%d", ++sceneNum);
            subNum = 0;
        }
        g_sceneNumbers[i] = buf;
    }
}

static const char* SceneNumber(int idx)
{
    if (idx < 0 || idx >= (int)g_sceneNumbers.size()) return "";
    return g_sceneNumbers[idx].c_str();
}

// ---------------------------------------------------------------------------
// CueNameLabel - the name to show where there is no indentation to carry the
// parent/child relationship: the cue list and the cue setup dialog.
//
// A cue list is a flat performance order, so a subscene appears there on its
// own with nothing around it to say which scene it varies. Naming the parent
// inline is the only place that information can go.
// ---------------------------------------------------------------------------
static std::string CueNameLabel(int idx)
{
    if (idx < 0 || idx >= (int)g_snapshots.size()) return "";
    const std::string& name = g_snapshots[idx]->m_name;
    if (!g_snapshots[idx]->m_isSub) return name;

    const int parent = ParentSceneIndex(idx);
    if (parent < 0) return name;

    // U+203A single right-pointing angle quote, as UTF-8.
    return g_snapshots[parent]->m_name + " \xE2\x80\xBA " + name;
}

// Human-readable identification for the safes popup and menu labels:
// "Scene 3  \"Verse\"" / "Subscene 3.2  \"Solo\"".
static std::string SceneDisplayLabel(int idx)
{
    if (idx < 0 || idx >= (int)g_snapshots.size()) return "";
    RebuildSceneNumbers();

    char buf[400];
    snprintf(buf, sizeof(buf), "%s %s  \"%s\"",
             g_snapshots[idx]->m_isSub ? "Subscene" : "Scene",
             SceneNumber(idx), g_snapshots[idx]->m_name.c_str());
    return buf;
}

// ---------------------------------------------------------------------------
// SetItemTextU8 - put UTF-8 text into a list view cell.
//
// The scene list is created with CreateWindowExA, so ListView_SetItemText
// sends LVM_SETITEMTEXTA and the control reads the bytes in the system ANSI
// codepage: UTF-8 in, mojibake out, which is what turned the subscene indent
// into "a""a"EUR and had been quietly mangling the spacer rows since they
// were added. A list view implements both the A and W forms of the message
// whatever the window was created as, so handing it UTF-16 takes the codepage
// out of the loop entirely.
// ---------------------------------------------------------------------------
static void SetItemTextU8(HWND hList, int item, int subItem, const char* utf8)
{
    if (!hList || !utf8) return;
#ifndef _WIN32
    // SWELL list views take UTF-8 as-is; there is no codepage to route around.
    ListView_SetItemText(hList, item, subItem, const_cast<char*>(utf8));
#else
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (wlen <= 0)
    {
        // Not valid UTF-8 (nothing here produces that, but a bad conversion
        // must not silently blank the cell) - fall back to the ANSI path.
        ListView_SetItemText(hList, item, subItem, const_cast<char*>(utf8));
        return;
    }
    std::vector<WCHAR> w((size_t)wlen);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), wlen);

    LVITEMW lv = {};
    lv.iSubItem = subItem;
    lv.pszText  = w.data();
    if (!SendMessageW(hList, LVM_SETITEMTEXTW, (WPARAM)item, (LPARAM)&lv))
    {
        // A control that refuses the W form would otherwise leave the cell
        // blank, which is worse than the mojibake this exists to avoid.
        ListView_SetItemText(hList, item, subItem, const_cast<char*>(utf8));
    }
#endif
}

// ---------------------------------------------------------------------------
// Row <-> snapshot mapping
//
// Until scenes could be collapsed, a row in the scene list *was* an index into
// g_snapshots and the whole file treated the two as interchangeable. Folding a
// scene's subscenes away breaks that: the rows are now whatever is visible.
// RefreshListView rebuilds this table, and everything that starts from a row
// goes through RowToSnap rather than assuming.
//
// Cue mode is unaffected - there a row has always been a position in
// g_cueList, which does the same job.
// ---------------------------------------------------------------------------
static std::vector<int> g_rowToSnap;

// True when idx is a subscene whose parent scene is folded away.
static bool IsHiddenByCollapse(int idx)
{
    if (idx < 0 || idx >= (int)g_snapshots.size()) return false;
    if (!g_snapshots[idx]->m_isSub) return false;
    const int parent = ParentSceneIndex(idx);
    return parent >= 0 && g_snapshots[parent]->m_collapsed;
}

static int RowToSnap(int row)
{
    if (row < 0 || row >= (int)g_rowToSnap.size()) return -1;
    const int si = g_rowToSnap[row];
    return (si >= 0 && si < (int)g_snapshots.size()) ? si : -1;
}

static int SnapToRow(int snapIdx)
{
    for (int r = 0; r < (int)g_rowToSnap.size(); ++r)
        if (g_rowToSnap[r] == snapIdx) return r;
    return -1;
}

// ---------------------------------------------------------------------------
// Collapse toggling
//
// The disclosure arrow is the first two characters of the Name cell rather
// than a real tree control: the list has been a plain LVS_REPORT since the
// start and every one of its behaviours (drag reorder, in-place rename, the
// click-to-recall options) is built on that. ArrowHitTest decides whether a
// click landed on the arrow by measuring against the Name column's own rect,
// so it keeps working when the column is resized or the splitter moves.
// ---------------------------------------------------------------------------
static const int kArrowZoneWidth = 16;

// Snapshot index of the collapsible scene whose arrow is under pt, or -1.
static int ArrowHitTest(HWND hList, POINT pt)
{
    if (g_cueMode) return -1;

    LVHITTESTINFO ht = {};
    ht.pt = pt;
    ListView_SubItemHitTest(hList, &ht);
    if (ht.iItem < 0 || ht.iSubItem != 1) return -1;

    const int si = RowToSnap(ht.iItem);
    if (si < 0) return -1;
    if (g_snapshots[si]->m_isSpacer || g_snapshots[si]->m_isSub) return -1;
    if (SubsceneCount(si) <= 0) return -1;

    RECT rc = {};
    if (!ListView_GetSubItemRect(hList, ht.iItem, 1, LVIR_BOUNDS, &rc)) return -1;
    return (pt.x >= rc.left && pt.x < rc.left + kArrowZoneWidth) ? si : -1;
}

static void SetCollapsed(HWND dlg, int snapIdx, bool collapsed)
{
    if (snapIdx < 0 || snapIdx >= (int)g_snapshots.size()) return;
    if (g_snapshots[snapIdx]->m_collapsed == collapsed) return;
    g_snapshots[snapIdx]->m_collapsed = collapsed;
    RefreshListView(dlg);
    MarkProjectDirty(nullptr);
}

static void SetAllCollapsed(HWND dlg, bool collapsed)
{
    bool any = false;
    for (int i = 0; i < (int)g_snapshots.size(); ++i)
    {
        if (g_snapshots[i]->m_isSpacer || g_snapshots[i]->m_isSub) continue;
        if (SubsceneCount(i) <= 0) continue;
        if (g_snapshots[i]->m_collapsed == collapsed) continue;
        g_snapshots[i]->m_collapsed = collapsed;
        any = true;
    }
    if (!any) return;
    RefreshListView(dlg);
    MarkProjectDirty(nullptr);
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

// The same, mapped to g_snapshots. Scenes mode only — in cue mode a row is a
// cue position and the caller has to decide what it wants.
static int GetSelectedSnapIndex(HWND hwnd)
{
    const int row = GetSelectedListIndex(hwnd);
    if (row < 0) return -1;
    if (g_cueMode)
        return (row < (int)g_cueList.size()) ? g_cueList[row] : -1;
    return RowToSnap(row);
}

// The sidebar's per-scene buttons (Add Subscene, Recall Filters) act on one
// scene, so they are greyed out unless exactly one scene is selected: nothing
// selected, a multi-selection and a spacer row all disable them.
static void UpdateSceneButtons(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LIST);
    const bool one = hList && ListView_GetSelectedCount(hList) == 1 &&
                     SelectedSnapshotIndex(hwnd, nullptr) >= 0;
    EnableWindow(GetDlgItem(hwnd, IDC_ADDSUB_BTN),     one);
    EnableWindow(GetDlgItem(hwnd, IDC_RECALLFILT_BTN), one);
}

// ---------------------------------------------------------------------------
// GetSelectedListIndices – every selected row, ascending.
// GetSelectedListIndex still answers "the first one", which is what the
// single-scene operations want; this is for the ones that act on a group.
// ---------------------------------------------------------------------------
static std::vector<int> GetSelectedListIndices(HWND hwnd)
{
    std::vector<int> out;
    HWND hList = GetDlgItem(hwnd, IDC_LIST);
    if (!hList) return out;
    int i = -1;
    while ((i = ListView_GetNextItem(hList, i, LVNI_SELECTED)) >= 0)
        out.push_back(i);
    return out;
}

// The same, mapped to g_snapshots and with unmappable rows dropped.
static std::vector<int> GetSelectedSnapIndices(HWND hwnd)
{
    std::vector<int> out;
    for (int row : GetSelectedListIndices(hwnd))
    {
        const int si = RowToSnap(row);
        if (si >= 0) out.push_back(si);
    }
    return out;
}

// ---------------------------------------------------------------------------
// DeleteSnapshotsAt - remove the given rows and put the list back in order.
// Shared by the Delete key and the context menu so a group delete behaves
// exactly like repeating the single delete, cue list fix-ups included.
// The caller does any confirming; this just does the work.
// ---------------------------------------------------------------------------
static void DeleteSnapshotsAt(HWND hwnd, const std::vector<int>& idxs)
{
    if (idxs.empty()) return;

    // Highest index first so the lower ones stay valid, and fix up the cue
    // list for each removal the same way a single delete does.
    for (int k = (int)idxs.size() - 1; k >= 0; k--)
    {
        const int si = idxs[k];
        if (si < 0 || si >= (int)g_snapshots.size()) continue;
        g_cueList.erase(
            std::remove(g_cueList.begin(), g_cueList.end(), si),
            g_cueList.end());
        for (auto& ci : g_cueList)
            if (ci > si) ci--;
        TouchedOnErase(si);
        g_snapshots.erase(g_snapshots.begin() + si);
    }
    for (int i = 0; i < (int)g_snapshots.size(); i++)
        g_snapshots[i]->m_slot = i;
    RefreshListView(hwnd);
    LoadEditorFromSnapshot(hwnd, nullptr);
    Undo_OnStateChangeEx(idxs.size() > 1 ? "Delete Scenes" : "Delete Scene", -1, -1);
}

// ---------------------------------------------------------------------------
// GetSelectedSnapshot
// ---------------------------------------------------------------------------
static TransitionSnapshot* GetSelectedSnapshot(HWND hwnd)
{
    int idx = GetSelectedSnapIndex(hwnd);
    if (idx < 0 || idx >= (int)g_snapshots.size()) return nullptr;
    return g_snapshots[idx].get();
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
// SceneRecallLayerIndex – the layer a recall of this scene lands on.
//
// Recall stays on the layer index that is active now: on layer 2, recalling a
// scene brings up that scene's layer 2. -1 when no layer is active, or the
// scene captured no layer at that index — layer state is then left alone.
// ---------------------------------------------------------------------------
static int SceneRecallLayerIndex(const TransitionSnapshot* snap)
{
    if (!snap || snap->m_isSpacer || snap->m_layers.empty()) return -1;
    const int active = LayersEngine::Get().GetActiveLayer();
    if (active < 0 || active >= (int)snap->m_layers.size()) return -1;
    return active;
}

// ---------------------------------------------------------------------------
// ResolveSceneLayer – make sure the scene points at a layer it can recall.
//
// The reference is resolved against the scene's OWN captured layer set, never
// against the live one. Every scene stores a full layer set, and recall
// replaces the live layers with it — so after recalling scene A the live
// layers carry A's uids, and scene B's uid legitimately matches none of them.
// Testing against the live set treated that as a dangling reference and
// "repaired" scene B by overwriting its captured layers with A's, which lost
// B's layers and made B recall A's on the next go.
//
// "(no layer recall)" (uid 0) is a deliberate choice and is left alone.
// ---------------------------------------------------------------------------
static void ResolveSceneLayer(TransitionSnapshot* snap)
{
    if (!snap || snap->m_isSpacer) return;
    EnsureLayerUids(snap);
    if (snap->m_layerUid <= 0) return;

    if (FindCapturedLayerByUid(snap, snap->m_layerUid) >= 0) return;  // fine

    // Genuinely dangling: the scene names a layer its own capture does not
    // contain. Fall back to the first layer it did capture.
    if (snap->m_layers.empty()) return;
    snap->m_layerUid = snap->m_layers[0].uid;
    snap->m_layerIdx = 0;
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

    // Remember the selection as a scene, not a row: collapsing or expanding
    // anything above it changes its row number but not which scene it is.
    const int selRowBefore  = GetSelectedListIndex(hwnd);
    const int selSnapBefore = g_cueMode ? -1 : RowToSnap(selRowBefore);

    RebuildSceneNumbers();
    ListView_DeleteAllItems(hList);

    // Third column means different things in the two modes: when a scene was
    // saved, or which scene a cue points at. Say which.
    {
        LVCOLUMN lvc = {};
        lvc.mask     = LVCF_TEXT;
        lvc.pszText  = const_cast<char*>(g_cueMode ? "Scene" : "Saved");
        ListView_SetColumn(hList, 2, &lvc);
    }

    if (g_cueMode)
    {
        g_rowToSnap.clear();   // cue mode maps through g_cueList instead

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

            char slotBuf[16];
            snprintf(slotBuf, sizeof(slotBuf), "%d", ci + 1);
            lvi.pszText = slotBuf;
            ListView_InsertItem(hList, &lvi);

            // A cue list is flat, so a subscene carries its parent's name
            // inline — there is no indentation here to say what it varies.
            SetItemTextU8(hList, ci, 1, CueNameLabel(snapIdx).c_str());

            // The same number the scene list shows ("3", or "3.2" for a
            // subscene), not the raw g_snapshots index it used to print,
            // which matched nothing the user could see.
            SetItemTextU8(hList, ci, 2, SceneNumber(snapIdx));
        }

        int listSize = (int)g_cueList.size();
        if (selRowBefore >= 0 && selRowBefore < listSize)
        {
            ListView_SetItemState(hList, selRowBefore,
                LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(hList, selRowBefore, FALSE);
        }
        return;
    }

    // --- Scenes mode ---
    // Scenes are numbered 1, 2, 3...; a subscene takes its parent's number
    // with its own position appended (2.1, 2.2) and its name is drawn indented
    // under it, so the grouping reads off the list without a tree control.
    // A scene with subscenes carries a disclosure arrow in the Name cell; the
    // rows it hides while collapsed are simply not inserted, which is why the
    // row number and the g_snapshots index part company here.
    g_rowToSnap.clear();
    g_rowToSnap.reserve(g_snapshots.size());

    for (int i = 0; i < (int)g_snapshots.size(); i++)
    {
        const auto& ss = g_snapshots[i];

        if (IsHiddenByCollapse(i)) continue;

        const int row = (int)g_rowToSnap.size();
        g_rowToSnap.push_back(i);

        LVITEM lvi = {};
        lvi.mask  = LVIF_TEXT;
        lvi.iItem = row;

        if (ss->m_isSpacer)
        {
            lvi.pszText = const_cast<char*>("");
            ListView_InsertItem(hList, &lvi);
            SetItemTextU8(hList, row, 1,
                "  \xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80");
            SetItemTextU8(hList, row, 2, "");
        }
        else
        {
            std::string nameCell;
            if (ss->m_isSub)
            {
                // Indent, then U+2514 U+2500 (box-drawing corner), as UTF-8.
                nameCell = "     \xE2\x94\x94\xE2\x94\x80 ";
                nameCell += ss->m_name;
            }
            else
            {
                // Disclosure arrow, and the hidden count while folded. Two
                // leading characters either way so names stay aligned down
                // the column whether or not a scene has subscenes.
                const int nsub = SubsceneCount(i);
                if (nsub > 0)
                    nameCell = ss->m_collapsed ? "\xE2\x96\xB8 " : "\xE2\x96\xBE ";
                else
                    nameCell = "  ";
                nameCell += ss->m_name;
                if (nsub > 0 && ss->m_collapsed)
                {
                    char badge[24];
                    snprintf(badge, sizeof(badge), "  (%d)", nsub);
                    nameCell += badge;
                }
            }
            lvi.pszText = const_cast<char*>(SceneNumber(i));
            ListView_InsertItem(hList, &lvi);

            SetItemTextU8(hList, row, 1, nameCell.c_str());

            char timeBuf[32] = "";
            if (ss->m_time)
            {
                struct tm* lt = localtime((const time_t*)&ss->m_time);
                if (lt)
                {
                    int hour12 = lt->tm_hour % 12;
                    if (!hour12) hour12 = 12;
                    snprintf(timeBuf, sizeof(timeBuf), "%d:%02d %s %02d/%02d",
                        hour12, lt->tm_min, lt->tm_hour < 12 ? "AM" : "PM",
                        lt->tm_mon + 1, lt->tm_mday);
                }
            }
            SetItemTextU8(hList, row, 2, timeBuf);
        }
    }

    // The selected scene may have moved row, or been folded away with its
    // parent — in which case the parent inherits the selection.
    int selRow = (selSnapBefore >= 0) ? SnapToRow(selSnapBefore) : -1;
    if (selRow < 0 && selSnapBefore >= 0 && IsHiddenByCollapse(selSnapBefore))
        selRow = SnapToRow(ParentSceneIndex(selSnapBefore));
    if (selRow >= 0)
    {
        ListView_SetItemState(hList, selRow,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hList, selRow, FALSE);
    }
    UpdateSceneButtons(hwnd);
}

// ---------------------------------------------------------------------------
// DoSaveAt – capture and insert a new scene or subscene at a given row
//
// insertAt < 0 appends. A subscene captures exactly what a scene does: what
// makes it a subscene is which safes apply when it is recalled (the subscene
// set, plus whatever its own per-scene safes say), not a smaller capture —
// so switching a row between the two kinds never loses data.
// ---------------------------------------------------------------------------
static void DoSaveAt(HWND hwnd, int insertAt, bool asSubscene)
{
    if (insertAt < 0 || insertAt > (int)g_snapshots.size())
        insertAt = (int)g_snapshots.size();

    // Always auto-generate an incremented name; user renames inline via the list.
    const char* kind = asSubscene ? "Subscene" : "Scene";
    char name[256] = {};
    {
        // "Scene %d" does not match "Subscene 4", so the two kinds number
        // independently, which is what the list's own numbering implies.
        char fmt[32];
        snprintf(fmt, sizeof(fmt), "%s %%d", kind);
        int maxN = (int)g_snapshots.size();
        for (auto& s : g_snapshots) {
            int n = 0;
            if (sscanf(s->m_name.c_str(), fmt, &n) == 1 && n > maxN)
                maxN = n;
        }
        snprintf(name, sizeof(name), "%s %d", kind, maxN + 1);
    }

    auto ss  = std::make_unique<TransitionSnapshot>(insertAt, name);
    ss->m_isSub = asSubscene;
    // Use the matching global defaults; user adjusts per-scene via context menu
    ss->m_duration = asSubscene ? g_defaultSubDuration : g_defaultDuration;
    ss->m_taper    = asSubscene ? g_defaultSubTaper    : g_defaultTaper;
    ss->m_taperExp = asSubscene ? g_defaultSubTaperExp : g_defaultTaperExp;

    // A new scene starts from what is on screen: the current TCP/MCP
    // visibility goes into the first layer, which becomes the active one, so
    // the scene captures it.
    // Nothing is applied — the project already looks like this. A safed first
    // layer keeps its definition (and so is not made active, since the screen
    // would not match it). Subscenes leave the layers alone.
    if (!asSubscene && !LayersEngine::IsLayerSafed(0))
    {
        LayersEngine& le = LayersEngine::Get();
        if (le.GetLayerCount() == 0) le.AddLayer(nullptr);
        le.CaptureVisibleInto(0);
        le.SetActiveNoApply(0);
        LayersWnd_Refresh();
    }

    ss->Capture(TS_CAPTURE_ALL);  // also captures full layer state

    // Recall follows the active layer index and no longer reads this, but
    // builds that predate that still do, so a new scene keeps naming the
    // layer that was active when it was saved. Only here, and only for a
    // scene being created — see the note in Capture about why Overwrite must
    // not do this.
    {
        LayersEngine& le = LayersEngine::Get();
        ss->m_layerIdx = le.GetActiveLayer();
        ss->m_layerUid = le.GetLayerUid(ss->m_layerIdx);
    }

    // Adding a scene must not move the selection: whatever the user had
    // selected stays selected, and the new row is only renamed, not selected.
    // Held as a snapshot index, because the insert below renumbers rows.
    const int prevSelSnap = GetSelectedSnapIndex(hwnd);

    g_snapshots.insert(g_snapshots.begin() + insertAt, std::move(ss));
    TouchedOnInsert(insertAt);
    // Everything below the insert point shifted, so the cue list's positional
    // references have to shift with it or they start pointing one scene early.
    for (auto& ci : g_cueList)
        if (ci >= insertAt) ci++;
    for (int i = 0; i < (int)g_snapshots.size(); i++) g_snapshots[i]->m_slot = i;

    RefreshListView(hwnd);

    HWND hList = GetDlgItem(hwnd, IDC_LIST);
    const int newIdx = insertAt;
    const int newRow = SnapToRow(newIdx);
    if (newRow >= 0) ListView_EnsureVisible(hList, newRow, FALSE);
    MarkTouched(newIdx);

    Undo_OnStateChangeEx(asSubscene ? "Save Subscene" : "Save Scene", -1, -1);

    // Immediately drop into inline rename so the user can name the new scene.
    // LVM_EDITLABEL is documented to require the list view to have the focus
    // already — sent while focus is still on the "New" button it does nothing
    // at all, which is why adding a scene never opened the rename box even
    // though F2 and right-click > Rename always did.
    SetFocus(hList);

    // LVM_EDITLABEL implicitly selects and focuses the row it edits, and the
    // list is multi-select, so without a restore the new scene ends up
    // selected alongside the previous one. The restore waits until the edit
    // finishes: changing a row's selection or focus while the in-place edit
    // is open closes the box, which is the whole point of opening it.
    g_addRestoreRow = newIdx;
    g_addRestoreSel = (prevSelSnap >= insertAt) ? prevSelSnap + 1 : prevSelSnap;

    if (newRow < 0 || !ListView_EditLabel(hList, newRow))
        RestoreSelectionAfterAdd(hwnd);   // no rename box: restore right away
}

// Append a new ordinary scene — what the New button and the headless action do.
static void DoSave(HWND hwnd)
{
    DoSaveAt(hwnd, -1, false);
}

// Add a subscene to the scene at snapIdx — the context menu's Add Subscene,
// the sidebar button and the headless action all come through here. snapIdx
// may be the scene itself or any of its subscenes: the new row goes at the
// end of the parent's block either way, so repeatedly adding builds 1.1, 1.2,
// 1.3 in order.
static void DoAddSubscene(HWND hwnd, int snapIdx)
{
    if (snapIdx < 0 || snapIdx >= (int)g_snapshots.size()) return;
    if (g_snapshots[snapIdx]->m_isSpacer) return;

    const int parent = g_snapshots[snapIdx]->m_isSub
                       ? ParentSceneIndex(snapIdx)
                       : snapIdx;
    const int insertAt = (parent >= 0) ? SubsceneBlockEnd(parent)
                                       : snapIdx + 1;
    // Adding into a folded scene would put the new row — and its rename box —
    // somewhere the user cannot see.
    if (parent >= 0) g_snapshots[parent]->m_collapsed = false;
    DoSaveAt(hwnd, insertAt, true);
}

// The button and the action have no row under the pointer to go on, so they
// use the selected scene. The action falls back to the last one created,
// recalled or saved — "a variation of what is on the desk now" is the usual
// intent when nothing is selected; the button is greyed out without exactly
// one selection (UpdateSceneButtons), so it never gets that far. Returns false
// when there is neither.
static bool AddSubsceneToCurrent(HWND hwnd)
{
    int snapIdx = SelectedSnapshotIndex(hwnd, nullptr);
    if (snapIdx < 0) snapIdx = g_lastTouchedIdx;
    if (snapIdx < 0 || snapIdx >= (int)g_snapshots.size() ||
        g_snapshots[snapIdx]->m_isSpacer)
        return false;
    DoAddSubscene(hwnd, snapIdx);
    return true;
}

// A scene's recall filters: its own safes, applied to its recalls only. The
// context menu and the sidebar button both open it.
static void EditRecallFilters(HWND hwnd, int snapIdx)
{
    if (snapIdx < 0 || snapIdx >= (int)g_snapshots.size()) return;
    TransitionSnapshot* snap = g_snapshots[snapIdx].get();
    if (snap->m_isSpacer) return;
    char title[512];
    snprintf(title, sizeof(title), "Recall Filters for %s",
             SceneDisplayLabel(snapIdx).c_str());
    if (SafesWnd_EditSceneSafes(hwnd, snap->m_safes, title))
        MarkProjectDirty(nullptr);
}

// ---------------------------------------------------------------------------
// RestoreSelectionAfterAdd - put the selection back after a newly added
// scene's rename box has closed. See g_addRestoreRow.
// ---------------------------------------------------------------------------
static void RestoreSelectionAfterAdd(HWND hwnd)
{
    const int newIdx  = g_addRestoreRow;
    const int prevSel = g_addRestoreSel;
    g_addRestoreRow = g_addRestoreSel = -1;
    if (newIdx < 0) return;

    HWND hList = GetDlgItem(hwnd, IDC_LIST);
    if (!hList) return;

    // Both are snapshot indices; the rows they sit on are whatever the list
    // looks like now.
    const int newRow = SnapToRow(newIdx);
    if (newRow >= 0) ListView_SetItemState(hList, newRow, 0, LVIS_SELECTED);

    const int prevRow = (prevSel >= 0 && prevSel < (int)g_snapshots.size())
                        ? SnapToRow(prevSel) : -1;
    if (prevRow >= 0)
    {
        ListView_SetItemState(hList, prevRow,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        // Re-selecting a row that is already selected sends no LVN_ITEMCHANGED,
        // so the editor panel would be left showing the new scene.
        LoadEditorFromSnapshot(hwnd,
            g_snapshots[prevSel]->m_isSpacer ? nullptr : g_snapshots[prevSel].get());
    }
    else
    {
        LoadEditorFromSnapshot(hwnd, nullptr);
    }
}

// ---------------------------------------------------------------------------
// RestoreLayerState – apply full layer state from a snapshot on recall.
//
// The layer index that is active stays active: the scene's layer at that index
// is installed and applied. If that slot is safed, nothing is applied — the
// safed layer stays as it is and stays selected — though the scene's other
// layers are still installed around it.
//
// Skipped when the TS_LAYERS safe bit is set, when no layer is active, or when
// the scene has no captured layer at the active index (m_layers empty covers
// old-format scenes).
// ---------------------------------------------------------------------------
static void RestoreLayerState(TransitionSnapshot* snap)
{
    if (!snap) return;
    // Not g_globalSafeMask directly: a subscene or a scene with its own safes
    // can add TS_LAYERS for this one recall, and the layer restore has to see
    // that as much as the engine does.
    if (GetEffectiveGlobalSafeMask() & TS_LAYERS) return;

    const int recallIdx = SceneRecallLayerIndex(snap);
    if (recallIdx < 0) return;

    // Number any pre-uid layers so the installed set keeps stable uids.
    EnsureLayerUids(snap);

    LayersEngine& le = LayersEngine::Get();

    std::vector<LayerDef> newLayers;
    for (int i = 0; i < (int)snap->m_layers.size(); i++)
    {
        // A safed slot keeps the layer that is there now, exactly as it is,
        // instead of taking the scene's captured copy. The safe is by index
        // because that is the only reference that still means anything once
        // the whole layer set is being replaced.
        if (LayersEngine::IsLayerSafed(i) && i < le.GetLayerCount())
        {
            newLayers.push_back(le.GetLayer(i));
            continue;
        }

        const CapturedLayer& cl = snap->m_layers[i];
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
            lt.folderCompact = clt.folderCompact;
            lt.showTcp     = clt.showTcp;
            lt.showMcp     = clt.showMcp;
            ld.tracks.push_back(lt);
        }
        newLayers.push_back(ld);
    }

    // Installed with nothing active, then re-selected by index: the replace
    // keeps order but may re-mint a clashing uid, so the index is the exact
    // reference. A safed slot is only re-selected — the project already
    // shows it.
    le.ReplaceAllLayers(newLayers, 0);
    if (LayersEngine::IsLayerSafed(recallIdx))
        le.SetActiveNoApply(recallIdx);
    else
        le.ActivateLayer(recallIdx);
    le.RefreshAllTrackNames();
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
        snapIdx = RowToSnap(listIndex);
    }

    if (snapIdx < 0 || snapIdx >= (int)g_snapshots.size()) return;
    if (g_snapshots[snapIdx]->m_isSpacer) return;

    TransitionSnapshot* snap = g_snapshots[snapIdx].get();
    ResolveSceneLayer(snap);   // migrate pre-uid scenes; drop dangling references
    // m_duration == 0 means instant (set via the Scene Settings popup)
    double duration = snap->m_duration;

    // Safes for this one recall: the subscene set if this is a subscene, plus
    // the scene's own set. Scoped across RestoreLayerState too, which reads
    // the same state after the engine has returned.
    SceneSafeScope safeScope(&snap->m_safes,
                             snap->m_isSub);

    // Place a named marker at the play cursor position if option is enabled
    if (g_placeMarker)
    {
        double pos = GetPlayPosition();
        AddProjectMarker2(nullptr, false, pos, 0.0, snap->m_name.c_str(), -1, 0);
    }

    // Stop recording before recall (marker placed first to capture correct play position)
    if (g_stopRecBeforeRecall && (GetPlayState() & 4))
        Main_OnCommand(1016, 0);  // Stop transport

    // When a layer is active, layers manage track visibility — including a
    // safed one that recall leaves alone. Strip TS_VIS from the engine mask so
    // the two systems don't fight.
    int effectiveMask = snap->m_mask;
    if (SceneRecallLayerIndex(snap) >= 0)
        effectiveMask &= ~TS_VIS;

    // --- Duration debug: record step timings if enabled ---
    LARGE_INTEGER freq = {}, t0 = {}, t1 = {}, t2 = {}, t3 = {};
    if (g_durationDebug)
        QueryPerformanceFrequency(&freq);

    if (g_durationDebug) QueryPerformanceCounter(&t0);
    TransitionEngine::Get().Recall(snap, effectiveMask, duration);
    if (g_durationDebug) QueryPerformanceCounter(&t1);

    TransitionEngine::Get().SetCurrentSlot(snapIdx);
    InvalidateRect(GetDlgItem(hwnd, IDC_LIST), nullptr, FALSE);
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
            "  Settings:  ChunkInstant=%s  SkipUnchanged=%s\n",
            t.s_chunkAllInstant ? "ON" : "off",
            t.s_skipUnchanged   ? "ON" : "off");

        char buf[2048];
        if (t.instantPath)
        {
            snprintf(buf, sizeof(buf),
                "[Live Tools] Scene recall timing: \"%s\"  [INSTANT]\n"
                "%s"
                "  BuildTrackMap:     %9.2f ms  (%d tracks)\n"
                "  ApplyImmediate:    %9.2f ms\n"
                "    VolPan:          %9.2f ms\n"
                "    Mute/Solo/Phase: %9.2f ms\n"
                "    Vis/Sel/Offset:  %9.2f ms\n"
                "    Layout:          %9.2f ms\n"
                "    FX chains:       %9.2f ms\n"
                "    Sends:           %9.2f ms\n"
                "    Track reorder:   %9.2f ms  (%d moves)\n"
                "      deselect-all:  %9.2f ms  (%d I_SELECTED writes)\n"
                "      ReorderSel:    %9.2f ms\n"
                "    Close FX windows:%9.2f ms\n"
                "    [unaccounted]:   %9.2f ms\n"
                "  Engine total:      %9.2f ms\n"
                "  RestoreLayerState: %9.2f ms\n"
                "  ── TOTAL ──        %9.2f ms\n",
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
                t.i_reorder, t.i_reorderMoves,
                t.i_reorderSel, t.i_reorderSelWrites,
                t.i_reorderMove,
                t.i_closeFX,
                // Whatever ApplyImmediate spent outside every bucket above.
                // Printed rather than left implicit so a future regression
                // cannot hide in the gap the way this one did.
                t.discreteParams - (t.i_volPan + t.i_muteSolo + t.i_vis +
                                    t.i_layout + t.i_fx + t.i_sends +
                                    t.i_reorder + t.i_closeFX),
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
        HWND hList  = GetDlgItem(hwnd, IDC_LIST);
        const int r = SnapToRow(snapIdx);
        if (r >= 0)
        {
            ListView_SetItemState(hList, r,
                LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(hList, r, FALSE);
        }
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
// PickSceneFile – .lts save/open picker. fn holds the suggested name on save
// and receives the chosen path; false if the user cancelled.
// ---------------------------------------------------------------------------
static bool PickSceneFile(HWND hwnd, bool save, char* fn, int fnSize)
{
    static const char kFilter[] = "Scene Files (*.lts)\0*.lts\0All Files (*.*)\0*.*\0";
#ifdef _WIN32
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = kFilter;
    ofn.lpstrFile   = fn;
    ofn.nMaxFile    = (DWORD)fnSize;
    ofn.lpstrDefExt = "lts";
    ofn.Flags       = save ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST)
                           : (OFN_FILEMUSTEXIST  | OFN_PATHMUSTEXIST);
    ofn.lpstrTitle  = save ? "Export Scene" : "Import Scene";
    return save ? GetSaveFileNameA(&ofn) != 0 : GetOpenFileNameA(&ofn) != 0;
#else
    (void)hwnd;
    if (save)
    {
        char suggested[512];
        snprintf(suggested, sizeof(suggested), "%s.lts", fn);
        return BrowseForSaveFile("Export Scene", nullptr, suggested, kFilter, fn, fnSize);
    }
    char* picked = BrowseForFiles("Import Scene", nullptr, nullptr, false, kFilter);
    if (!picked) return false;
    snprintf(fn, (size_t)fnSize, "%s", picked);
    free(picked);
    return true;
#endif
}

// ---------------------------------------------------------------------------
// ExportScene – write a single scene to a .lts file
// ---------------------------------------------------------------------------
static void ExportScene(HWND hwnd, int item)
{
    if (item < 0 || item >= (int)g_snapshots.size()) return;
    if (g_snapshots[item]->m_isSpacer) return;

    char szFile[MAX_PATH] = {};
    snprintf(szFile, sizeof(szFile), "%s", g_snapshots[item]->m_name.c_str());
    for (char& c : szFile)
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"'  || c == '<' || c == '>' || c == '|')
            c = '_';

    if (!PickSceneFile(hwnd, true, szFile, sizeof(szFile))) return;

    FILE* fp = fopen(szFile, "w");
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
    if (!PickSceneFile(hwnd, false, szFile, sizeof(szFile))) return;

    FILE* fp = fopen(szFile, "r");
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
    const int newIdx = (int)g_snapshots.size() - 1;
    const int newRow = SnapToRow(newIdx);
    if (newRow >= 0)
    {
        ListView_SetItemState(hList, newRow,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hList, newRow, FALSE);
    }
    LoadEditorFromSnapshot(hwnd, g_snapshots[newIdx].get());
    Undo_OnStateChangeEx("Import Scene", -1, -1);
}

// ---------------------------------------------------------------------------
// Drop bar – the line drawn between the two rows a dragged scene will land
// between.
// ---------------------------------------------------------------------------

// The gap under a point in list-client coordinates: the upper half of a row
// is the gap above it, the lower half the gap below. Below the last row is
// the end of the list.
static int DropGapFromPoint(HWND hList, POINT pt)
{
    const int count = ListView_GetItemCount(hList);
    if (count <= 0) return 0;

    LVHITTESTINFO hti = {};
    hti.pt = pt;
    const int item = ListView_HitTest(hList, &hti);
    if (item >= 0)
    {
        RECT rc;
        if (!ListView_GetItemRect(hList, item, &rc, LVIR_BOUNDS)) return item;
        return (pt.y < (rc.top + rc.bottom) / 2) ? item : item + 1;
    }

    RECT rcLast;
    if (ListView_GetItemRect(hList, count - 1, &rcLast, LVIR_BOUNDS) && pt.y >= rcLast.bottom)
        return count;
    // Above the first visible row (over the header, or scrolled): the gap
    // above the top row on screen.
    return ListView_GetTopIndex(hList);
}

// The y of a gap's bar, in list-client coordinates.
static bool DropGapY(HWND hList, int gap, int& y)
{
    const int count = ListView_GetItemCount(hList);
    if (gap < 0 || count <= 0) return false;
    RECT rc;
    if (gap < count)
    {
        if (!ListView_GetItemRect(hList, gap, &rc, LVIR_BOUNDS)) return false;
        y = rc.top;
    }
    else
    {
        if (!ListView_GetItemRect(hList, count - 1, &rc, LVIR_BOUNDS)) return false;
        y = rc.bottom;
    }
    return true;
}

static const int kDropBarH = 2;

static void InvalidateDropGap(HWND hList, int gap)
{
    int y;
    if (!DropGapY(hList, gap, y)) return;
    RECT cr;
    GetClientRect(hList, &cr);
    RECT r = { cr.left, y - kDropBarH - 1, cr.right, y + kDropBarH + 1 };
    InvalidateRect(hList, &r, FALSE);
}

// Painted over the list after it has drawn itself.
static void PaintDropBar(HWND hList)
{
    int y;
    if (g_dragSrc < 0 || !DropGapY(hList, g_dragTarget, y)) return;
    RECT cr;
    GetClientRect(hList, &cr);
    // Centred on the row boundary, but kept inside the list at either end.
    int top = y - kDropBarH / 2;
    if (top < 0) top = 0;
    if (top + kDropBarH > cr.bottom) top = cr.bottom - kDropBarH;
    RECT r = { cr.left, top, cr.right, top + kDropBarH };
    HDC hdc = GetDC(hList);
    HBRUSH hb = CreateSolidBrush(ReaperTheme_List().fg);
    FillRect(hdc, &r, hb);
    DeleteObject(hb);
    ReleaseDC(hList, hdc);
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

    const int srcRow = g_dragSrc;
    const int tgtGap = g_dragTarget;
    g_dragSrc    = -1;
    g_dragTarget = -1;
    InvalidateDropGap(hList, tgtGap);   // take the bar down

    if (srcRow < 0 || tgtGap < 0) return;

    // The gap becomes the scene index the block is inserted before. Past the
    // last row is the end of the list — which, with the last scene folded,
    // is after its hidden subscenes too.
    const int src = RowToSnap(srcRow);
    const int tgt = (tgtGap >= ListView_GetItemCount(hList))
                    ? (int)g_snapshots.size() : RowToSnap(tgtGap);
    if (src < 0 || tgt < 0) return;

    // A scene travels with its subscenes. Dragging the parent out from under
    // them and leaving them to re-parent onto whatever scene happened to be
    // above is never what the drag meant, and it is not visible afterwards.
    // The block is the scene plus everything SubsceneBlockEnd counts as its
    // own, which includes any spacer the user drew between two subscenes.
    const int blockStart = src;
    const int blockEnd   = (!g_snapshots[src]->m_isSpacer && !g_snapshots[src]->m_isSub)
                           ? SubsceneBlockEnd(src) : src + 1;
    const int blockLen   = blockEnd - blockStart;

    // A gap inside the block, or either edge of it, leaves it where it is.
    if (tgt >= blockStart && tgt <= blockEnd) return;

    std::vector<std::unique_ptr<TransitionSnapshot>> moved;
    moved.reserve((size_t)blockLen);
    for (int i = blockStart; i < blockEnd; ++i)
        moved.push_back(std::move(g_snapshots[i]));

    // A reorder is an erase plus an insert, but the scenes themselves survive,
    // so remember where the touched one sat inside the block and re-point at
    // it once the block has landed.
    const int touchedOffset = (g_lastTouchedIdx >= blockStart && g_lastTouchedIdx < blockEnd)
                              ? g_lastTouchedIdx - blockStart : -1;
    if (touchedOffset < 0)
        for (int i = blockStart; i < blockEnd; ++i) TouchedOnErase(blockStart);

    g_snapshots.erase(g_snapshots.begin() + blockStart,
                      g_snapshots.begin() + blockEnd);

    // The drop lands before the target row; anything after the removed block
    // has shifted down by its length.
    int insertAt = (tgt > blockStart) ? tgt - blockLen : tgt;
    if (insertAt < 0) insertAt = 0;
    if (insertAt > (int)g_snapshots.size()) insertAt = (int)g_snapshots.size();

    for (int k = 0; k < blockLen; ++k)
        g_snapshots.insert(g_snapshots.begin() + insertAt + k, std::move(moved[k]));

    if (touchedOffset >= 0) g_lastTouchedIdx = insertAt + touchedOffset;
    else for (int k = 0; k < blockLen; ++k) TouchedOnInsert(insertAt);

    // The cue list points at positions, so every entry that was inside the
    // block travels with it and everything the block passed over shifts by
    // its length. Cue spacers (-1) are left alone.
    for (auto& ci : g_cueList)
    {
        if (ci < 0) continue;
        if (ci >= blockStart && ci < blockEnd)      ci += insertAt - blockStart;
        else if (ci >= blockEnd && ci < insertAt + blockLen) ci -= blockLen;
        else if (ci >= insertAt && ci < blockStart) ci += blockLen;
    }

    for (int i = 0; i < (int)g_snapshots.size(); i++)
        g_snapshots[i]->m_slot = i;

    RefreshListView(hwnd);
    const int newRow = SnapToRow(insertAt);
    if (newRow >= 0)
    {
        ListView_SetItemState(hList, newRow,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hList, newRow, FALSE);
    }
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

    // In only: window caption, so the popup says which row it belongs to and
    // whether that row is a scene or a subscene.
    std::string title;
};

// ---------------------------------------------------------------------------
// SnapSettingsDialogProc – modal IDD_SNAP_SETTINGS dialog
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK SnapSettingsDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Dialog colours come from the REAPER theme (see ReaperTheme.h).
    if (INT_PTR r = ReaperTheme_CtlColor(hwnd, msg, wParam, lParam)) return r;
    if (msg == WM_INITDIALOG) ReaperTheme_ApplyDialog(hwnd);

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SnapSettingsData* d = reinterpret_cast<SnapSettingsData*>(lParam);
        SetWindowLongPtr(hwnd, DWLP_USER, (LONG_PTR)d);

        if (!d->title.empty()) SetWindowTextA(hwnd, d->title.c_str());

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

static INT_PTR CALLBACK AddKeywordDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Dialog colours come from the REAPER theme (see ReaperTheme.h).
    if (INT_PTR r = ReaperTheme_CtlColor(hwnd, msg, wParam, lParam)) return r;
    if (msg == WM_INITDIALOG) ReaperTheme_ApplyDialog(hwnd);

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
static INT_PTR CALLBACK ChunkRecallPluginsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Dialog colours come from the REAPER theme (see ReaperTheme.h).
    if (INT_PTR r = ReaperTheme_CtlColor(hwnd, msg, wParam, lParam)) return r;
    if (msg == WM_INITDIALOG) ReaperTheme_ApplyDialog(hwnd);

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
    // Dialog colours come from the REAPER theme (see ReaperTheme.h).
    if (INT_PTR r = ReaperTheme_CtlColor(hwnd, msg, wParam, lParam)) return r;
    if (msg == WM_INITDIALOG) ReaperTheme_ApplyDialog(hwnd);

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        // Populate taper combobox
        const char* taperItems[] = {
            "Linear", "S-Curve", "Logarithmic", "Exponential", "Custom..."
        };
        for (const char* n : taperItems)
        {
            SendDlgItemMessage(hwnd, IDC_GSET_TAPER,     CB_ADDSTRING, 0, (LPARAM)n);
            SendDlgItemMessage(hwnd, IDC_GSET_SUB_TAPER, CB_ADDSTRING, 0, (LPARAM)n);
        }

        bool instant = (g_defaultDuration == 0.0);
        CheckDlgButton(hwnd, IDC_GSET_INSTANT, instant ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_MARKER,            g_placeMarker           ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_STOP_REC_BEFORE,   g_stopRecBeforeRecall   ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_START_REC_AFTER,   g_startRecAfterRecall   ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_SINGLE_CLICK,    g_singleClickRecall   ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_ALT_DELETE,      g_altClickDelete      ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_CTRL_OVERWRITE,  g_ctrlClickOverwrite  ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_SKIP_UNCHANGED,   g_skipUnchangedParams ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_DURATION_DEBUG,   g_durationDebug       ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_CHUNK_ALL_INSTANT, g_chunkAllInstant    ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_RECALL_LOG,        g_recallLog          ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_GSET_MATCH_THEME,       ReaperTheme_MatchTheme() ? BST_CHECKED : BST_UNCHECKED);

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

        // ---- New subscene defaults --------------------------------------
        {
            bool subInstant = (g_defaultSubDuration == 0.0);
            CheckDlgButton(hwnd, IDC_GSET_SUB_INSTANT, subInstant ? BST_CHECKED : BST_UNCHECKED);

            snprintf(buf, sizeof(buf), "%.2f", subInstant ? 2.0 : g_defaultSubDuration);
            SetDlgItemText(hwnd, IDC_GSET_SUB_DURATION, buf);

            int subSel = (g_defaultSubTaper >= 0 && g_defaultSubTaper <= TAPER_CUSTOM)
                         ? g_defaultSubTaper : TAPER_SCURVE;
            SendDlgItemMessage(hwnd, IDC_GSET_SUB_TAPER, CB_SETCURSEL, subSel, 0);

            snprintf(buf, sizeof(buf), "%.2f", g_defaultSubTaperExp);
            SetDlgItemText(hwnd, IDC_GSET_SUB_TAPER_CUSTOM, buf);

            EnableWindow(GetDlgItem(hwnd, IDC_GSET_SUB_DURATION), !subInstant);
            EnableWindow(GetDlgItem(hwnd, IDC_GSET_SUB_TAPER),    !subInstant);
            EnableWindow(GetDlgItem(hwnd, IDC_GSET_SUB_TAPER_CUSTOM),
                         !subInstant && g_defaultSubTaper == TAPER_CUSTOM);
        }

        // Layers (these used to be the Layers window's own Settings dialog).
        {
            const LayersSettings& lc = LayersEngine::Get().GetSettings();
            CheckDlgButton(hwnd, IDC_LYR_SET_MCPVIS,     lc.applyMcpVisibility  ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_LYR_SET_REORDER,    lc.reorderTracks       ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_LYR_SET_RESTORE,    lc.restoreOnDeactivate ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_LYR_SET_TRIGGERMCP, lc.triggerMcpSelect    ? BST_CHECKED : BST_UNCHECKED);
            HWND hSpin = GetDlgItem(hwnd, IDC_LYR_MAXCH_SPIN);
            HWND hEdit = GetDlgItem(hwnd, IDC_LYR_MAXCH_EDIT);
            if (hSpin && hEdit)
            {
                SendMessage(hSpin, UDM_SETRANGE, 0, MAKELONG(512, 0));
                SendMessage(hSpin, UDM_SETBUDDY, (WPARAM)hEdit, 0);
            }
            SetDlgItemInt(hwnd, IDC_LYR_MAXCH_EDIT, lc.globalMaxChannels, FALSE);
        }
        return TRUE;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wParam), evt = HIWORD(wParam);
        if (id == IDC_GSET_SUB_INSTANT)
        {
            bool si = (IsDlgButtonChecked(hwnd, IDC_GSET_SUB_INSTANT) == BST_CHECKED);
            EnableWindow(GetDlgItem(hwnd, IDC_GSET_SUB_DURATION), !si);
            EnableWindow(GetDlgItem(hwnd, IDC_GSET_SUB_TAPER),    !si);
            int sel = (int)SendDlgItemMessage(hwnd, IDC_GSET_SUB_TAPER, CB_GETCURSEL, 0, 0);
            EnableWindow(GetDlgItem(hwnd, IDC_GSET_SUB_TAPER_CUSTOM),
                         !si && sel == TAPER_CUSTOM);
            return TRUE;
        }
        if (id == IDC_GSET_SUB_TAPER && evt == CBN_SELCHANGE)
        {
            int sel = (int)SendDlgItemMessage(hwnd, IDC_GSET_SUB_TAPER, CB_GETCURSEL, 0, 0);
            bool si = (IsDlgButtonChecked(hwnd, IDC_GSET_SUB_INSTANT) == BST_CHECKED);
            EnableWindow(GetDlgItem(hwnd, IDC_GSET_SUB_TAPER_CUSTOM),
                         !si && sel == TAPER_CUSTOM);
            return TRUE;
        }
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

            // ---- New subscene defaults ----------------------------------
            if (IsDlgButtonChecked(hwnd, IDC_GSET_SUB_INSTANT) == BST_CHECKED)
            {
                g_defaultSubDuration = 0.0;
            }
            else
            {
                char sbuf[64] = {};
                GetDlgItemText(hwnd, IDC_GSET_SUB_DURATION, sbuf, sizeof(sbuf));
                double sdur = atof(sbuf);
                g_defaultSubDuration = (sdur > 0.0) ? sdur : 2.0;
            }
            g_defaultSubTaper = (int)SendDlgItemMessage(hwnd, IDC_GSET_SUB_TAPER, CB_GETCURSEL, 0, 0);
            if (g_defaultSubTaper < 0 || g_defaultSubTaper > TAPER_CUSTOM)
                g_defaultSubTaper = TAPER_SCURVE;
            char sexBuf[64] = {};
            GetDlgItemText(hwnd, IDC_GSET_SUB_TAPER_CUSTOM, sexBuf, sizeof(sexBuf));
            double sex = atof(sexBuf);
            g_defaultSubTaperExp = (sex > 0.0) ? sex : 2.0;

            g_placeMarker         = (IsDlgButtonChecked(hwnd, IDC_GSET_MARKER)            == BST_CHECKED);
            g_stopRecBeforeRecall = (IsDlgButtonChecked(hwnd, IDC_GSET_STOP_REC_BEFORE)   == BST_CHECKED);
            g_startRecAfterRecall = (IsDlgButtonChecked(hwnd, IDC_GSET_START_REC_AFTER)   == BST_CHECKED);
            g_singleClickRecall   = (IsDlgButtonChecked(hwnd, IDC_GSET_SINGLE_CLICK)    == BST_CHECKED);
            g_altClickDelete      = (IsDlgButtonChecked(hwnd, IDC_GSET_ALT_DELETE)       == BST_CHECKED);
            g_ctrlClickOverwrite  = (IsDlgButtonChecked(hwnd, IDC_GSET_CTRL_OVERWRITE)   == BST_CHECKED);
            g_skipUnchangedParams = (IsDlgButtonChecked(hwnd, IDC_GSET_SKIP_UNCHANGED)    == BST_CHECKED);
            g_durationDebug       = (IsDlgButtonChecked(hwnd, IDC_GSET_DURATION_DEBUG)    == BST_CHECKED);
            g_chunkAllInstant     = (IsDlgButtonChecked(hwnd, IDC_GSET_CHUNK_ALL_INSTANT) == BST_CHECKED);
            g_recallLog           = (IsDlgButtonChecked(hwnd, IDC_GSET_RECALL_LOG)        == BST_CHECKED);
            MarkProjectDirty(nullptr);  // settings are saved per-project via SaveExtensionConfig

            // Dark mode is machine-wide (ExtState), not per project.
            // Setting it re-reads the colours itself, so the Scenes window's
            // theme timer would see nothing new: repaint it here.
            const bool matchTheme = (IsDlgButtonChecked(hwnd, IDC_GSET_MATCH_THEME) == BST_CHECKED);
            if (matchTheme != ReaperTheme_MatchTheme())
            {
                ReaperTheme_SetMatchTheme(matchTheme);
                if (g_wnd && IsWindow(g_wnd))
                {
                    ReaperTheme_ApplyListView(GetDlgItem(g_wnd, IDC_LIST));
                    RedrawWindow(g_wnd, nullptr, nullptr,
                                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
                }
            }

            // Layers. SetSettings re-applies the active layer, so it is only
            // called when something actually changed — pressing OK here for an
            // unrelated setting must not re-cue the tracks.
            {
                LayersSettings lc = LayersEngine::Get().GetSettings();
                const LayersSettings before = lc;
                lc.applyMcpVisibility  = (IsDlgButtonChecked(hwnd, IDC_LYR_SET_MCPVIS)     == BST_CHECKED);
                lc.reorderTracks       = (IsDlgButtonChecked(hwnd, IDC_LYR_SET_REORDER)    == BST_CHECKED);
                lc.restoreOnDeactivate = (IsDlgButtonChecked(hwnd, IDC_LYR_SET_RESTORE)    == BST_CHECKED);
                lc.triggerMcpSelect    = (IsDlgButtonChecked(hwnd, IDC_LYR_SET_TRIGGERMCP) == BST_CHECKED);
                BOOL ok = FALSE;
                const int mc = (int)GetDlgItemInt(hwnd, IDC_LYR_MAXCH_EDIT, &ok, FALSE);
                lc.globalMaxChannels = (ok && mc >= 0) ? mc : 0;
                if (lc.applyMcpVisibility  != before.applyMcpVisibility  ||
                    lc.reorderTracks       != before.reorderTracks       ||
                    lc.restoreOnDeactivate != before.restoreOnDeactivate ||
                    lc.triggerMcpSelect    != before.triggerMcpSelect    ||
                    lc.globalMaxChannels   != before.globalMaxChannels)
                {
                    LayersEngine::Get().SetSettings(lc);
                    LayersWnd_Refresh();   // max channels moves the slot-limit marks
                }
            }

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
    RebuildSceneNumbers();
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
        // Parent name inline for subscenes: nothing else in a cue order says
        // which scene one belongs to.
        SetItemTextU8(hRight, ci, 1, CueNameLabel(snapIdx).c_str());
    }
}

// ---------------------------------------------------------------------------
// Cue List Setup layout
//
// Same treatment as the Layers window: the two lists share the width left
// after the margins and the divider gutter, in the proportion the divider
// sets, and take all the height under the labels. Each list's Scene Name
// column stretches to fill it. The split is remembered between sessions.
// ---------------------------------------------------------------------------
struct CueLayoutInit {
    bool valid     = false;
    int  margin    = 0;
    int  gutter    = 0;
    int  labelTop  = 0;
    int  labelH    = 0;
    int  listTop   = 0;
    int  bottomGap = 0;
};
static CueLayoutInit s_cueLy;
static double        s_cueSplitRatio   = -1.0;
static WNDPROC       s_cueSplitOldProc = nullptr;
static bool          s_cueSplitDrag    = false;
static const char*   kCueSplitKey      = "cue_setup_split";
static const int     kCueMinListW      = 120;

static RECT CueChildRect(HWND dlg, HWND h)
{
    RECT r = {};
    if (h)
    {
        GetWindowRect(h, &r);
        MapWindowPoints(HWND_DESKTOP, dlg, (POINT*)&r, 2);
    }
    return r;
}

// Scene Name (column 1) takes whatever the # column leaves.
static void CueStretchName(HWND hList)
{
    if (!hList) return;
    RECT rc;
    GetClientRect(hList, &rc);
    int w = (rc.right - rc.left) - ListView_GetColumnWidth(hList, 0)
            - GetSystemMetrics(SM_CXVSCROLL);
    if (w < 60) w = 60;
    ListView_SetColumnWidth(hList, 1, w);
}

static void LayoutCueSetup(HWND hwnd)
{
    if (!s_cueLy.valid || !s_cueLeft || !s_cueRight) return;
    RECT cr;
    GetClientRect(hwnd, &cr);
    const int W = cr.right, H = cr.bottom;
    if (W <= 0 || H <= 0) return;

    const CueLayoutInit& g = s_cueLy;
    const int avail = W - 2 * g.margin - g.gutter;
    int leftW = (int)(avail * s_cueSplitRatio + 0.5);
    if (leftW > avail - kCueMinListW) leftW = avail - kCueMinListW;
    if (leftW < kCueMinListW)         leftW = kCueMinListW;
    const int rightX = g.margin + leftW + g.gutter;
    int rightW = W - g.margin - rightX;
    if (rightW < 1) rightW = 1;
    int listH = H - g.listTop - g.bottomGap;
    if (listH < 40) listH = 40;

    HDWP dw = BeginDeferWindowPos(5);
    auto place = [&](HWND c, int x, int y, int w, int h) {
        if (c) dw = DeferWindowPos(dw, c, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    place(GetDlgItem(hwnd, IDC_CUE_LEFT_LBL),  g.margin, g.labelTop, leftW,  g.labelH);
    place(GetDlgItem(hwnd, IDC_CUE_RIGHT_LBL), rightX,   g.labelTop, rightW, g.labelH);
    place(s_cueLeft,                           g.margin, g.listTop,  leftW,  listH);
    place(GetDlgItem(hwnd, IDC_CUE_SPLITTER),  g.margin + leftW, g.listTop, g.gutter, listH);
    place(s_cueRight,                          rightX,   g.listTop,  rightW, listH);
    EndDeferWindowPos(dw);

    CueStretchName(s_cueLeft);
    CueStretchName(s_cueRight);
    InvalidateRect(hwnd, nullptr, TRUE);
}

static LRESULT CALLBACK CueSplitterProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
        return TRUE;

    case WM_LBUTTONDOWN:
        s_cueSplitDrag = true;
        SetCapture(h);
        return 0;

    case WM_MOUSEMOVE:
        if (s_cueSplitDrag && s_cueLy.valid)
        {
            HWND dlg = GetParent(h);
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(dlg, &pt);
            RECT cr;
            GetClientRect(dlg, &cr);
            const int avail = cr.right - 2 * s_cueLy.margin - s_cueLy.gutter;
            if (avail > 0)
            {
                double r = (double)(pt.x - s_cueLy.margin - s_cueLy.gutter / 2) / avail;
                if (r < 0.05) r = 0.05;
                if (r > 0.95) r = 0.95;
                s_cueSplitRatio = r;
                LayoutCueSetup(dlg);
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (s_cueSplitDrag)
        {
            s_cueSplitDrag = false;
            ReleaseCapture();
            char buf[32];
            snprintf(buf, sizeof(buf), "%.4f", s_cueSplitRatio);
            SetExtState("reaper_transitions", kCueSplitKey, buf, true);
        }
        return 0;

    case WM_CAPTURECHANGED:
        s_cueSplitDrag = false;
        return 0;
    }
    return CallWindowProc(s_cueSplitOldProc, h, msg, wp, lp);
}

// Record the design-size geometry, hook the divider and lay out once. Runs
// after the placeholders have been replaced by the real lists.
static void InitCueSetupLayout(HWND hwnd)
{
    if (!s_cueLeft || !s_cueRight) return;
    RECT cr;
    GetClientRect(hwnd, &cr);
    const RECT ll = CueChildRect(hwnd, s_cueLeft);
    const RECT rl = CueChildRect(hwnd, s_cueRight);
    const RECT lb = CueChildRect(hwnd, GetDlgItem(hwnd, IDC_CUE_LEFT_LBL));

    CueLayoutInit& g = s_cueLy;
    g.margin    = ll.left;
    g.gutter    = rl.left - ll.right;
    if (g.gutter < 4) g.gutter = 4;
    g.labelTop  = lb.top;
    g.labelH    = lb.bottom - lb.top;
    g.listTop   = ll.top;
    g.bottomGap = cr.bottom - ll.bottom;
    g.valid     = (ll.right > ll.left && rl.right > rl.left);

    if (s_cueSplitRatio < 0.0)
    {
        const char* v = GetExtState("reaper_transitions", kCueSplitKey);
        const double saved = (v && v[0]) ? atof(v) : 0.0;
        const int lw = ll.right - ll.left, rw = rl.right - rl.left;
        s_cueSplitRatio = (saved > 0.02 && saved < 0.98) ? saved
                        : (lw + rw > 0 ? (double)lw / (lw + rw) : 0.5);
    }

    if (HWND hSplit = GetDlgItem(hwnd, IDC_CUE_SPLITTER))
        s_cueSplitOldProc = (WNDPROC)SetWindowLongPtr(hSplit, GWLP_WNDPROC, (LONG_PTR)CueSplitterProc);

    LayoutCueSetup(hwnd);
}

// ---------------------------------------------------------------------------
// CueSetupDialogProc – modal IDD_CUE_SETUP dialog
// Two list views: left = spacer + all scenes, right = cue list order.
// All drag logic is in CueLvSubclassProc; this proc only handles
// double-click, ESC close, and WM_DESTROY cleanup.
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK CueSetupDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Dialog colours come from the REAPER theme (see ReaperTheme.h).
    if (INT_PTR r = ReaperTheme_CtlColor(hwnd, msg, wParam, lParam)) return r;
    if (msg == WM_INITDIALOG) ReaperTheme_ApplyDialog(hwnd);

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
            ReaperTheme_ApplyListView(hLv);
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

            RebuildSceneNumbers();
            int row = 1;
            for (int i = 0; i < (int)g_snapshots.size(); i++)
            {
                if (g_snapshots[i]->m_isSpacer) continue;
                LVITEM lvi = {};
                lvi.mask   = LVIF_TEXT | LVIF_PARAM;
                lvi.iItem  = row++;
                lvi.lParam = (LPARAM)i;
                // The scene list's own number ("3", "3.2"), not the raw
                // g_snapshots index this used to print as "S4".
                lvi.pszText = const_cast<char*>(SceneNumber(i));
                ListView_InsertItem(s_cueLeft, &lvi);
                SetItemTextU8(s_cueLeft, row - 1, 1, CueNameLabel(i).c_str());
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

        InitCueSetupLayout(hwnd);
        return TRUE;
    }

    case WM_SIZE:
        LayoutCueSetup(hwnd);
        break;

    case WM_GETMINMAXINFO:
        if (s_cueLy.valid)
        {
            // Room for both lists at their minimum widths and a few rows.
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            RECT r = { 0, 0,
                       2 * s_cueLy.margin + s_cueLy.gutter + 2 * kCueMinListW,
                       s_cueLy.listTop + 120 + s_cueLy.bottomGap };
            AdjustWindowRectEx(&r, (DWORD)GetWindowLong(hwnd, GWL_STYLE), FALSE,
                               (DWORD)GetWindowLong(hwnd, GWL_EXSTYLE));
            mmi->ptMinTrackSize.x = r.right  - r.left;
            mmi->ptMinTrackSize.y = r.bottom - r.top;
        }
        return 0;

    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
        if (dis && dis->CtlID == IDC_CUE_SPLITTER)
        {
            // A single hairline down the gutter, like the Scenes window's.
            HBRUSH face = CreateSolidBrush(ReaperTheme_DialogBg());
            FillRect(dis->hDC, &dis->rcItem, face);
            DeleteObject(face);
            const int cx = (dis->rcItem.left + dis->rcItem.right) / 2;
            HPEN pen = CreatePen(PS_SOLID, 1, ReaperTheme_Sys(COLOR_BTNSHADOW));
            HGDIOBJ old = SelectObject(dis->hDC, pen);
            MoveToEx(dis->hDC, cx, dis->rcItem.top + 2, nullptr);
            LineTo  (dis->hDC, cx, dis->rcItem.bottom - 2);
            SelectObject(dis->hDC, old);
            DeleteObject(pen);
            return TRUE;
        }
        break;
    }

    case WM_NOTIFY:
    {
        NMHDR* hdr = (NMHDR*)lParam;

        // Rows (and the selection) in REAPER theme colours
        if ((hdr->hwndFrom == s_cueLeft || hdr->hwndFrom == s_cueRight) &&
            hdr->code == NM_CUSTOMDRAW)
        {
            NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lParam;
            LRESULT res = CDRF_DODEFAULT;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT)
                res = CDRF_NOTIFYITEMDRAW;
            else if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
            {
                ReaperTheme_ListItemPrePaint(cd, hdr->hwndFrom, false);
                res = CDRF_NEWFONT;
            }
            SetWindowLongPtr(hwnd, DWLP_MSGRESULT, res);
            return TRUE;
        }

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
        s_cueLy.valid   = false;
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
        snapIdx = RowToSnap(item);
    }

    bool hasItem   = (snapIdx >= 0 && snapIdx < (int)g_snapshots.size());
    bool isSpacer  = hasItem && g_snapshots[snapIdx]->m_isSpacer;
    bool hasClip   = (g_clipboard != nullptr);

    HMENU hMenu = CreatePopupMenu();

    // With several rows selected, only the operations that mean something for
    // a group are offered. Recall, Rename, Overwrite and the rest all act on
    // exactly one scene, and silently applying them to the first of a
    // selection would be worse than not offering them.
    const std::vector<int> multiSel = isCueMode ? std::vector<int>()
                                                : GetSelectedSnapIndices(hwnd);
    if (!isCueMode && multiSel.size() > 1)
    {
        char delLabel[48];
        snprintf(delLabel, sizeof(delLabel), "Delete %d Scenes", (int)multiSel.size());
        AppendMenu(hMenu, MF_STRING, CTX_DELETE, delLabel);

        int cmdMulti = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                      pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(hMenu);

        if (cmdMulti == CTX_DELETE)
        {
            char prompt[96];
            snprintf(prompt, sizeof(prompt), "Delete %d scenes?", (int)multiSel.size());
            if (MessageBoxA(hwnd, prompt, "Live Tools - Scenes",
                            MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
                DeleteSnapshotsAt(hwnd, multiSel);
            }
        }
        return;
    }

    if (isCueMode)
    {
        // Cue mode context menu: simpler
        AppendMenu(hMenu, MF_STRING | (!hasItem ? MF_GRAYED : 0), CTX_RECALL_CTX, "Recall");
        AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenu(hMenu, MF_STRING | ((!hasItem && snapIdx != -1) ? MF_GRAYED : 0), CTX_CUE_REMOVE, "Remove from Cue");
    }
    else
    {
        const bool isSub  = hasItem && !isSpacer && g_snapshots[snapIdx]->m_isSub;
        const bool isReal = hasItem && !isSpacer;   // a scene or a subscene

        AppendMenu(hMenu, MF_STRING, CTX_NEW, "New");
        AppendMenu(hMenu, MF_STRING | (!isReal ? MF_GRAYED : 0), CTX_ADDSUB,
                   isSub ? "Add Subscene (sibling)" : "Add Subscene");
        AppendMenu(hMenu, MF_STRING | ((!hasItem || isSpacer) ? MF_GRAYED : 0), CTX_RECALL_CTX, "Recall");
        AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenu(hMenu, MF_STRING | (!hasItem ? MF_GRAYED : 0), CTX_RENAME, "Rename\tF2");
        if (isSub)
            AppendMenu(hMenu, MF_STRING, CTX_PROMOTE, "Promote to Scene");
        AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenu(hMenu, MF_STRING | ((!hasItem || isSpacer) ? MF_GRAYED : 0), CTX_COPY_CTX,  "Copy");
        AppendMenu(hMenu, MF_STRING | (!hasClip ? MF_GRAYED : 0), CTX_PASTE_CTX, "Paste");
        AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenu(hMenu, MF_STRING | ((!hasItem || isSpacer) ? MF_GRAYED : 0), CTX_OVERWRITE, "Overwrite");
        AppendMenu(hMenu, MF_STRING | ((!hasItem || isSpacer) ? MF_GRAYED : 0), CTX_SCENE_SETTINGS,
                   isSub ? "Subscene Settings..." : "Scene Settings...");

        // Recall filters: this row's own safes — what its recall leaves
        // alone — and the set that applies to every subscene, which lives on
        // its own tab of the Safes window.
        {
            UINT flags = MF_STRING | (!isReal ? MF_GRAYED : 0);
            if (isReal && (!g_snapshots[snapIdx]->m_safes.IsEmpty() || g_snapshots[snapIdx]->m_safes.replaceGlobal)) flags |= MF_CHECKED;
            AppendMenu(hMenu, flags, CTX_SCENE_SAFES, "Recall Filters...");
        }
        AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenu(hMenu, MF_STRING | (!hasItem ? MF_GRAYED : 0), CTX_DELETE, "Delete");
        AppendMenu(hMenu, MF_STRING | (g_snapshots.empty() ? MF_GRAYED : 0), CTX_DELETE_ALL, "Delete All Scenes");
        AppendMenu(hMenu, MF_STRING | ((!hasItem || isSpacer) ? MF_GRAYED : 0), CTX_EXPORT, "Export...");
        AppendMenu(hMenu, MF_STRING, CTX_IMPORT, "Import...");
        AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenu(hMenu, MF_STRING, CTX_ADDSPACER, "Add Spacer");

        // Folding. The arrow in the Name column does the same thing for one
        // scene; these are for the keyboard and for doing the whole list.
        {
            const int nsub = hasItem ? SubsceneCount(snapIdx) : 0;
            if (nsub > 0)
                AppendMenu(hMenu, MF_STRING, CTX_TOGGLE_FOLD,
                           g_snapshots[snapIdx]->m_collapsed ? "Expand Subscenes"
                                                             : "Collapse Subscenes");
            AppendMenu(hMenu, MF_STRING, CTX_COLLAPSE_ALL, "Collapse All");
            AppendMenu(hMenu, MF_STRING, CTX_EXPAND_ALL,   "Expand All");
        }
    }

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                              pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);

    switch (cmd)
    {
    case CTX_NEW:
        DoSave(hwnd);
        break;

    case CTX_ADDSUB:
        if (hasItem && !isSpacer) DoAddSubscene(hwnd, snapIdx);
        break;

    case CTX_PROMOTE:
        if (hasItem && !isSpacer && g_snapshots[snapIdx]->m_isSub)
        {
            g_snapshots[snapIdx]->m_isSub = false;
            RefreshListView(hwnd);
            Undo_OnStateChangeEx("Promote Subscene to Scene", -1, -1);
        }
        break;

    case CTX_SCENE_SAFES:
        if (hasItem && !isSpacer) EditRecallFilters(hwnd, snapIdx);
        break;

    case CTX_TOGGLE_FOLD:
        if (hasItem)
            SetCollapsed(hwnd, snapIdx, !g_snapshots[snapIdx]->m_collapsed);
        break;

    case CTX_COLLAPSE_ALL: SetAllCollapsed(hwnd, true);  break;
    case CTX_EXPAND_ALL:   SetAllCollapsed(hwnd, false); break;

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
            d.title    = SceneDisplayLabel(snapIdx) + " - Settings";
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
            // Pasting under a scene makes a scene, pasting under a subscene
            // makes a subscene: the row the user pointed at is the intent,
            // more reliably than whatever the copy happened to be.
            if (hasItem && !isSpacer)
                copy->m_isSub = g_snapshots[snapIdx]->m_isSub;
            if (copy->m_name.find(" (copy)") == std::string::npos)
                copy->m_name += " (copy)";
            copy->m_time = (int)std::time(nullptr);
            g_snapshots.insert(g_snapshots.begin() + insertAfter, std::move(copy));
            TouchedOnInsert(insertAfter);
            for (auto& ci : g_cueList)
                if (ci >= insertAfter) ci++;
            for (int i = 0; i < (int)g_snapshots.size(); i++) g_snapshots[i]->m_slot = i;
            RefreshListView(hwnd);
            HWND hList  = GetDlgItem(hwnd, IDC_LIST);
            const int r = SnapToRow(insertAfter);
            if (r >= 0)
            {
                ListView_SetItemState(hList, r,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(hList, r, FALSE);
            }
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
            // Deleting a scene that has subscenes: they would otherwise fall
            // through to whatever scene precedes them, which is never what was
            // meant and is not obvious from the list afterwards. Ask, and let
            // "No" be the deliberate choice to keep them.
            int count = 1;
            const int nsub = SubsceneCount(snapIdx);
            if (nsub > 0)
            {
                char prompt[256];
                snprintf(prompt, sizeof(prompt),
                    "\"%s\" has %d subscene%s.\n\n"
                    "Yes  - delete the scene and its subscenes\n"
                    "No   - delete only the scene (subscenes move under the scene above)",
                    g_snapshots[snapIdx]->m_name.c_str(), nsub, nsub == 1 ? "" : "s");
                int r = MessageBoxA(hwnd, prompt, "Live Tools - Scenes",
                                    MB_YESNOCANCEL | MB_ICONQUESTION);
                if (r == IDCANCEL) break;
                // The whole block, not 1 + nsub: a spacer the user drew
                // between two subscenes is part of what they are deleting.
                if (r == IDYES) count = SubsceneBlockEnd(snapIdx) - snapIdx;
            }

            // Remove the deleted rows from the cue list, then close the gap
            // the erase leaves in every index above them.
            g_cueList.erase(
                std::remove_if(g_cueList.begin(), g_cueList.end(), [&](int ci) {
                    return ci >= snapIdx && ci < snapIdx + count;
                }), g_cueList.end());
            for (auto& ci : g_cueList)
                if (ci >= snapIdx + count) ci -= count;

            for (int n = 0; n < count; ++n) TouchedOnErase(snapIdx);
            g_snapshots.erase(g_snapshots.begin() + snapIdx,
                              g_snapshots.begin() + snapIdx + count);
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
                g_lastTouchedIdx = -1;
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
        {
            // LVM_EDITLABEL only does anything when the list already has the
            // focus, and the popup menu has had it until just now.
            HWND hListR = GetDlgItem(hwnd, IDC_LIST);
            SetFocus(hListR);
            ListView_EditLabel(hListR, item);
        }
        break;

    case CTX_ADDSPACER:
    {
        int insertAfter = hasItem ? snapIdx + 1 : (int)g_snapshots.size();
        auto spacer = std::make_unique<TransitionSnapshot>(insertAfter, "");
        spacer->m_isSpacer = true;
        g_snapshots.insert(g_snapshots.begin() + insertAfter, std::move(spacer));
        TouchedOnInsert(insertAfter);
        for (auto& ci : g_cueList)
            if (ci >= insertAfter) ci++;
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
        // Disclosure arrow → fold this scene's subscenes away, or bring them
        // back. Checked before everything else so the arrow never doubles as
        // a recall, a delete or the start of a drag.
        {
            POINT ptA = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            const int arrowSnap = ArrowHitTest(hList, ptA);
            if (arrowSnap >= 0)
            {
                SetCollapsed(dlg, arrowSnap, !g_snapshots[arrowSnap]->m_collapsed);
                return 0;
            }
        }
        // Alt+click → delete scene (scenes mode only)
        if (g_altClickDelete && !g_cueMode)
        {
            LVHITTESTINFO htiMod = {};
            htiMod.pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int snapIdx = RowToSnap(ListView_HitTest(hList, &htiMod));
            if ((GetKeyState(VK_MENU) & 0x8000) && snapIdx >= 0)
            {
                g_cueList.erase(
                    std::remove(g_cueList.begin(), g_cueList.end(), snapIdx),
                    g_cueList.end());
                for (auto& ci : g_cueList)
                    if (ci > snapIdx) ci--;
                TouchedOnErase(snapIdx);
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
            int clickedMod = RowToSnap(ListView_HitTest(hList, &htiMod));
            if (clickedMod >= 0 && !g_snapshots[clickedMod]->m_isSpacer)
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
        // The cue list is ordered from the cue setup dialog, not by dragging
        // here, so no drag (or drop bar) starts in cue mode.
        if (s_lbTracking && g_dragSrc < 0 && !g_cueMode)
        {
            bool movedEnough = (abs(pt.x - s_lbDownPt.x) > GetSystemMetrics(SM_CXDRAG) ||
                                abs(pt.y - s_lbDownPt.y) > GetSystemMetrics(SM_CYDRAG));
            bool heldLongEnough = (GetTickCount() - s_lbDownTime >= 200);
            // Reorder moves one row to one place; with several rows selected
            // there is no sensible answer, so don't start a drag at all.
            bool singleRow = (ListView_GetSelectedCount(hList) <= 1);
            if (movedEnough && heldLongEnough && singleRow)
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
            const int newGap = DropGapFromPoint(hList, pt);
            if (newGap != g_dragTarget)
            {
                // Repaint while the drag image is hidden, so the old bar is
                // not left behind in the image's saved background.
                InvalidateDropGap(hList, g_dragTarget);
                g_dragTarget = newGap;
                InvalidateDropGap(hList, g_dragTarget);
                UpdateWindow(hList);
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
        // A row, kept as one for the menu and the selection below; -1 when it
        // does not map to a scene (past the end of the list).
        const int rbSnap = g_cueMode ? -1 : RowToSnap(item);
        int safeItem = (g_cueMode ? (item >= 0 && item < (int)g_cueList.size())
                                  : (rbSnap >= 0)) ? item : -1;
        if (safeItem >= 0 && rbSnap >= 0 && !g_snapshots[rbSnap]->m_isSpacer)
        {
            // Right-clicking inside a selection keeps it, so the menu can act
            // on the group; right-clicking outside one selects just that row.
            const bool alreadySel =
                (ListView_GetItemState(hList, safeItem, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            if (!alreadySel)
            {
                ListView_SetItemState(hList, -1, 0, LVIS_SELECTED);
                ListView_SetItemState(hList, safeItem,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
            LoadEditorFromSnapshot(dlg, g_snapshots[rbSnap].get());
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
        const int clickSnap = g_cueMode ? -1 : RowToSnap(s_lbDownItem);
        if (g_singleClickRecall && wasTracking && s_lbDownItem >= 0 &&
            (g_cueMode || (clickSnap >= 0 && !g_snapshots[clickSnap]->m_isSpacer)) &&
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

    case WM_PAINT:
    {
        // The list paints itself; the drop bar goes on top.
        LRESULT r = CallWindowProc(s_origListProc, hList, msg, wParam, lParam);
        if (g_dragSrc >= 0) PaintDropBar(hList);
        return r;
    }

    case WM_KEYDOWN:
        if (wParam == VK_DELETE && !g_cueMode)
        {
            // Delete every selected row, not just the focused one. A single
            // row goes straight away as it always has; a group asks first,
            // matching the context menu's "Delete N Scenes".
            std::vector<int> sel = GetSelectedSnapIndices(dlg);
            while (!sel.empty() && sel.back() >= (int)g_snapshots.size())
                sel.pop_back();
            if (sel.size() > 1)
            {
                char prompt[96];
                snprintf(prompt, sizeof(prompt), "Delete %d scenes?", (int)sel.size());
                if (MessageBoxA(dlg, prompt, "Live Tools - Scenes",
                                MB_YESNO | MB_ICONQUESTION) != IDYES)
                    return 0;
                DeleteSnapshotsAt(dlg, sel);
                return 0;
            }
            if (sel.size() == 1)
            {
                DeleteSnapshotsAt(dlg, sel);
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
    // Dialog colours come from the REAPER theme (see ReaperTheme.h).
    if (INT_PTR r = ReaperTheme_CtlColor(hwnd, msg, wParam, lParam)) return r;
    if (msg == WM_INITDIALOG) ReaperTheme_ApplyDialog(hwnd);

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
            LVS_REPORT | LVS_SHOWSELALWAYS | LVS_EDITLABELS,
            rList.left, rList.top,
            rList.right - rList.left, rList.bottom - rList.top,
            hwnd, (HMENU)(INT_PTR)IDC_LIST, g_hInstance, nullptr);

        if (hList)
        {
            ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            ReaperTheme_ApplyListView(hList);

            // Bold variant of the list's own font, for the last-recalled scene.
            if (!g_sceneBoldFont)
            {
                HFONT hf = (HFONT)SendMessage(hList, WM_GETFONT, 0, 0);
                LOGFONT lf = {};
                if (hf && GetObject(hf, sizeof(lf), &lf))
                {
                    lf.lfWeight     = FW_BOLD;
                    g_sceneBoldFont = CreateFontIndirect(&lf);
                }
            }

            LVCOLUMN col = {};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            // Wide enough for a two-part subscene number ("12.3"); the Name
            // column stretches to absorb whatever this and "Saved" leave.
            col.cx = 40;  col.pszText = const_cast<char*>("#");
            ListView_InsertColumn(hList, 0, &col);
            col.cx = 110; col.pszText = const_cast<char*>("Name");
            ListView_InsertColumn(hList, 1, &col);
            col.cx = 98;  col.pszText = const_cast<char*>("Saved");
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

            // ---- Footer (status line + version) -----------------------
            HWND hVer = GetDlgItem(hwnd, IDC_VERSION);
            HWND hSta = GetDlgItem(hwnd, IDC_STATUS);
            if (hVer && hSta)
            {
                SetDlgItemText(hwnd, IDC_VERSION, LT_VERSION_STR);
                GetWindowRect(hVer, &g_versionInitRect);
                MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&g_versionInitRect, 2);
                GetWindowRect(hSta, &g_statusInitRect);
                MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&g_statusInitRect, 2);
            }
            if (HWND hProgF = GetDlgItem(hwnd, IDC_PROGRESS))
            {
                GetWindowRect(hProgF, &g_progInitRect);
                MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&g_progInitRect, 2);
            }
            if (HWND hLayerF = GetDlgItem(hwnd, IDC_LAYER_STATUS))
            {
                GetWindowRect(hLayerF, &g_layerInitRect);
                MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&g_layerInitRect, 2);
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
            HBRUSH face = CreateSolidBrush(ReaperTheme_DialogBg());
            FillRect(dis->hDC, &dis->rcItem, face);
            DeleteObject(face);
            int cx = (dis->rcItem.left + dis->rcItem.right) / 2;
            HPEN pen = CreatePen(PS_SOLID, 1, ReaperTheme_Sys(COLOR_BTNSHADOW));
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
            HBRUSH face = CreateSolidBrush(ReaperTheme_DialogBg());
            FillRect(dis->hDC, &dis->rcItem, face);
            DeleteObject(face);
            int midY = (dis->rcItem.top + dis->rcItem.bottom) / 2;
            int cx   = (dis->rcItem.left + dis->rcItem.right) / 2;
            HPEN pen = CreatePen(PS_SOLID, 1, ReaperTheme_Sys(COLOR_BTNSHADOW));
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
            // Follow a REAPER theme switch while the window is open.
            static int s_themeTick = 0;
            if (++s_themeTick >= 10)
            {
                s_themeTick = 0;
                if (ReaperTheme_Refresh())
                {
                    ReaperTheme_ApplyListView(GetDlgItem(hwnd, IDC_LIST));
                    RedrawWindow(hwnd, nullptr, nullptr,
                                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
                }
            }

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

    case WM_USER + 2:
    {
        // Deferred from LVN_BEGINLABELEDIT: put the in-place edit over the
        // Name column rather than the "#" column the list view chose.
        HWND hList = GetDlgItem(hwnd, IDC_LIST);
        HWND hEdit = hList ? ListView_GetEditControl(hList) : nullptr;
        if (hEdit && g_labelEditItem >= 0)
        {
            RECT rc = {};
            if (ListView_GetSubItemRect(hList, g_labelEditItem, 1, LVIR_BOUNDS, &rc))
            {
                g_labelEditRect      = rc;
                g_labelEditRectValid = true;
                if (!s_origLabelEditProc)
                    s_origLabelEditProc = (WNDPROC)SetWindowLongPtr(
                        hEdit, GWLP_WNDPROC, (LONG_PTR)LabelEditProc);
                SetWindowPos(hEdit, nullptr, rc.left, rc.top,
                             rc.right - rc.left, rc.bottom - rc.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            // The edit is created by the list view, which had to be focused
            // for LVM_EDITLABEL to work at all; make sure the box itself is
            // what the keyboard is talking to, so typing replaces the
            // auto-generated name straight away.
            SetFocus(hEdit);
            SendMessage(hEdit, EM_SETSEL, 0, (LPARAM)-1);
        }
        return TRUE;
    }

    case WM_USER + 3:
        // Deferred from LVN_ENDLABELEDIT: the rename box on a newly added
        // scene has closed, so the selection can be put back without the
        // state change closing the box out from under the user.
        RestoreSelectionAfterAdd(hwnd);
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
            int idx = GetSelectedSnapIndex(hwnd);
            if (idx >= 0 && idx < (int)g_snapshots.size())
            {
                char buf[4096] = {};
                GetDlgItemText(hwnd, IDC_SNAPNOTES, buf, sizeof(buf));
                g_snapshots[idx]->m_notes = NotesFromControl(buf);
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

        case IDC_ADDSUB_BTN:
            AddSubsceneToCurrent(hwnd);
            break;

        case IDC_RECALLFILT_BTN:
        {
            const int snapIdx = SelectedSnapshotIndex(hwnd, nullptr);
            if (snapIdx >= 0) EditRecallFilters(hwnd, snapIdx);
            break;
        }

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
            if (hdr->code == NM_CUSTOMDRAW)
            {
                // Mark the last-recalled scene by weight, the same way the
                // Layers window marks the active layer. Weight rather than a
                // text marker: anything written into the label would land in
                // the rename box and end up in the scene name.
                NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lParam;
                LRESULT res = CDRF_DODEFAULT;
                switch (cd->nmcd.dwDrawStage)
                {
                case CDDS_PREPAINT:
                    res = CDRF_NOTIFYITEMDRAW;
                    break;
                case CDDS_ITEMPREPAINT:
                {
                    const int row  = (int)cd->nmcd.dwItemSpec;
                    const int slot = TransitionEngine::Get().GetCurrentSlot();
                    // In cue mode a row is a cue position, so map it back to
                    // the snapshot it points at before comparing.
                    int rowSnap = g_cueMode
                        ? ((row >= 0 && row < (int)g_cueList.size()) ? g_cueList[row] : -1)
                        : RowToSnap(row);
                    const bool spacer = rowSnap >= 0 && rowSnap < (int)g_snapshots.size() &&
                                        g_snapshots[rowSnap]->m_isSpacer;
                    // Row colours, selection included, from the REAPER theme;
                    // spacer rows get the muted text colour.
                    ReaperTheme_ListItemPrePaint(cd, hdr->hwndFrom, spacer);
                    if (g_sceneBoldFont && rowSnap >= 0 && rowSnap == slot)
                        SelectObject(cd->nmcd.hdc, g_sceneBoldFont);
                    res = CDRF_NEWFONT;
                    break;
                }
                }
                SetWindowLongPtr(hwnd, DWLP_MSGRESULT, res);
                return TRUE;
            }
            else if (hdr->code == NM_DBLCLK)
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
                const int si = g_cueMode ? -1 : RowToSnap(nlv->iItem);
                if ((nlv->uNewState & LVIS_SELECTED) && si >= 0)
                {
                    if (!g_snapshots[si]->m_isSpacer)
                        LoadEditorFromSnapshot(hwnd, g_snapshots[si].get());
                    else
                        LoadEditorFromSnapshot(hwnd, nullptr);
                }
                if (nlv->uChanged & LVIF_STATE) UpdateSceneButtons(hwnd);
            }
            else if (hdr->code == LVN_BEGINLABELEDIT)
            {
                // Block label editing on spacer rows
                NMLVDISPINFO* di = (NMLVDISPINFO*)lParam;
                const int si = g_cueMode ? -1 : RowToSnap(di->item.iItem);
                if (si < 0 || g_snapshots[si]->m_isSpacer)
                {
                    SetWindowLongPtr(hwnd, DWLP_MSGRESULT, TRUE);
                    return TRUE;
                }

                // A list view edits the item *label*, which is column 0 — the
                // "#" column — so the box opened over the row number showing
                // the index. Seed it with the name instead and move it onto
                // the Name column, deferred because the list view sizes the
                // edit control after this notification returns.
                {
                    HWND hEdit = ListView_GetEditControl(hdr->hwndFrom);
                    if (hEdit)
                    {
                        // The stored name, not the cell text: the cell carries
                        // the disclosure arrow and the subscene indent, and
                        // neither belongs in the rename box.
                        SetWindowText(hEdit, g_snapshots[si]->m_name.c_str());
                        g_labelEditItem = di->item.iItem;   // a row: it positions the box
                        PostMessage(hwnd, WM_USER + 2, 0, 0);
                    }
                }
            }
            else if (hdr->code == NM_RCLICK)
            {
                // Handle right-click here (before WM_CONTEXTMENU is generated).
                // Returning TRUE prevents the ListView from generating WM_CONTEXTMENU,
                // so REAPER's hook never intercepts it and shows the dock menu.
                NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
                // A row for ShowContextMenu (which maps it itself); the scene
                // it points at for everything done here.
                const int rcSnap = g_cueMode ? -1 : RowToSnap(nia->iItem);
                int item = (g_cueMode ? (nia->iItem >= 0 && nia->iItem < (int)g_cueList.size())
                                      : (rcSnap >= 0)) ? nia->iItem : -1;
                // ptAction is in list-client coordinates
                POINT ptScreen = nia->ptAction;
                ClientToScreen(hdr->hwndFrom, &ptScreen);

                if (item >= 0 && rcSnap >= 0 && !g_snapshots[rcSnap]->m_isSpacer)
                {
                    HWND hListN = GetDlgItem(hwnd, IDC_LIST);
                    ListView_SetItemState(hListN, item,
                        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    LoadEditorFromSnapshot(hwnd, g_snapshots[rcSnap].get());
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
            else if (hdr->code == LVN_ENDLABELEDIT)
            {
                NMLVDISPINFO* di = (NMLVDISPINFO*)lParam;
                g_labelEditItem = -1;
                // Posted rather than done here: the list view is still tearing
                // the edit control down, and RefreshListView below reselects
                // whatever was selected during the edit.
                if (g_addRestoreRow >= 0)
                    PostMessage(hwnd, WM_USER + 3, 0, 0);
                const int si = g_cueMode ? -1 : RowToSnap(di->item.iItem);
                if (di->item.pszText && si >= 0)
                {
                    g_snapshots[si]->m_name = di->item.pszText;
                    // FALSE: do not let the list write the typed text into the
                    // item label ("#"). RefreshListView rebuilds the row with
                    // the name in the right column.
                    SetWindowLongPtr(hwnd, DWLP_MSGRESULT, FALSE);
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
        if (g_sceneBoldFont) { DeleteObject(g_sceneBoldFont); g_sceneBoldFont = nullptr; }
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
