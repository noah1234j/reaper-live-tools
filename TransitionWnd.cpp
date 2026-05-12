#include "TransitionWnd.h"
#include "TransitionEngine.h"
#include "SafesWnd.h"
#include "LayersEngine.h"
#include "api.h"
#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>
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

// Guard: set true when programmatically updating editor fields to prevent
// EN_CHANGE / CBN_SELCHANGE from writing back to the snapshot.
static bool g_syncingEditor = false;

// Guard: when true, WM_DESTROY skips overwriting the dock-state pref (used by ToggleDocking)
static bool g_suppressDockStateSave = false;

// UI timer ID
static const UINT UI_TIMER_ID = 1;

// Context menu item IDs
enum { CTX_RENAME = 100, CTX_OVERWRITE, CTX_DELETE,
       CTX_NEW, CTX_RECALL_CTX, CTX_COPY_CTX, CTX_PASTE_CTX,
       CTX_EXPORT, CTX_IMPORT, CTX_ADDSPACER, CTX_SCENE_SETTINGS,
       CTX_CUE_REMOVE };

// Docker context menu IDs
enum { CTX_DOCK = 200, CTX_CLOSE };

// Cue-list mode flag
static bool g_cueMode = false;

// Ordered cue list (each entry is an index into g_snapshots)
static std::vector<int> g_cueList;

// Whether to place a project marker at the play-cursor on each recall
static bool g_placeMarker = false;

// Global default transition settings for newly created scenes
static double g_defaultDuration = 2.0;
static int    g_defaultTaper    = TAPER_SCURVE;
static double g_defaultTaperExp = 2.0;

// Drag-drop state
static int        g_dragSrc     = -1;
static int        g_dragTarget  = -1;
static HIMAGELIST g_hDragImages = nullptr;

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
static void LoadEditorFromSnapshot(HWND hwnd, const TransitionSnapshot* snap);
static void ExportScene(HWND hwnd, int item);
static void ImportScene(HWND hwnd);
static void DoEndDrag(HWND hwnd);
static TransitionSnapshot* GetSelectedSnapshot(HWND hwnd);
static int  GetSelectedListIndex(HWND hwnd);
static LRESULT CALLBACK ListSubclassProc(HWND hList, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK CueLvSubclassProc(HWND hList, UINT msg, WPARAM wParam, LPARAM lParam);
static void RefillCueRightList(HWND hRight, const std::vector<int>& list);
static void RestoreLayerState(const TransitionSnapshot* snap);
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

void TransitionWnd_ShowHide()
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
            return;
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
        return;
    }

    // Window already exists – activate if docked, otherwise toggle visibility
    bool isFloat = false;
    if (DockIsChildOfDock(g_wnd, &isFloat) >= 0)
    {
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
    const TransitionSnapshot* snap = g_snapshots[index].get();
    // m_duration == 0 means instant
    double duration = snap->m_duration;
    if (g_placeMarker)
    {
        double pos = GetPlayPosition();
        AddProjectMarker2(nullptr, false, pos, 0.0, snap->m_name.c_str(), -1, 0);
    }
    TransitionEngine::Get().Recall(snap, snap->m_mask, duration);
    TransitionEngine::Get().SetCurrentSlot(index);
    // Restore full layer state (always, unless TS_LAYERS safe bit is set)
    RestoreLayerState(snap);
    if (g_wnd && IsWindow(g_wnd))
    {
        HWND hList = GetDlgItem(g_wnd, IDC_LIST);
        ListView_SetItemState(hList, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hList, index, FALSE);
    }
    Undo_OnStateChangeEx("Recall Scene", -1, -1);
}

void TransitionWnd_OverwriteScene(int index)
{
    if (index < 0 || index >= (int)g_snapshots.size()) return;
    g_snapshots[index]->Capture(TS_CAPTURE_ALL);
    if (g_wnd && IsWindow(g_wnd))
        RefreshListView(g_wnd);
    Undo_OnStateChangeEx("Save Scene", -1, -1);
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
// LoadEditorFromSnapshot – fill right-panel controls from a snapshot
// ---------------------------------------------------------------------------
static void LoadEditorFromSnapshot(HWND hwnd, const TransitionSnapshot* snap)
{
    g_syncingEditor = true;

    // Only IDC_SNAPNAME lives in the main dialog now; transition settings
    // and notes are in the per-scene Settings context-menu popup.
    SetDlgItemText(hwnd, IDC_SNAPNAME,  snap ? snap->m_name.c_str()  : "");
    SetDlgItemText(hwnd, IDC_SNAPNOTES, snap ? snap->m_notes.c_str() : "");

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
    char name[256] = {};
    GetDlgItemText(hwnd, IDC_SNAPNAME, name, sizeof(name));
    // If the field is empty, OR contains an existing snapshot's name (meaning
    // the user didn't retype it), auto-generate a new incremented name.
    bool nameIsNew = (name[0] != '\0');
    if (nameIsNew) {
        for (auto& s : g_snapshots)
            if (s->m_name == name) { nameIsNew = false; break; }
    }
    if (!nameIsNew) {
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

    Undo_OnStateChangeEx("Save Scene", -1, -1);
}

// ---------------------------------------------------------------------------
// RestoreLayerState – apply full layer state from a snapshot on recall.
// Skipped only when the TS_LAYERS safe bit is set.
// ---------------------------------------------------------------------------
static void RestoreLayerState(const TransitionSnapshot* snap)
{
    if (!snap) return;
    if (g_globalSafeMask & TS_LAYERS) return;

    if (!snap->m_layers.empty())
    {
        // Build a vector<LayerDef> from the captured CapturedLayer data
        std::vector<LayerDef> newLayers;
        for (const auto& cl : snap->m_layers)
        {
            LayerDef ld;
            strncpy(ld.name, cl.name.c_str(), sizeof(ld.name) - 1);
            ld.name[sizeof(ld.name) - 1] = '\0';
            ld.maxChannels = cl.maxChannels;
            for (const auto& clt : cl.tracks)
            {
                LayerTrack lt;
                lt.guid     = clt.guid;
                lt.isSpacer = clt.isSpacer;
                lt.name[0]  = '\0';
                ld.tracks.push_back(lt);
            }
            newLayers.push_back(ld);
        }
        LayersEngine::Get().ReplaceAllLayers(newLayers, snap->m_layerIdx);
        LayersEngine::Get().RefreshAllTrackNames();
    }
    else if (snap->m_layerIdx >= 0)
    {
        // Backward compat (old snapshots with only an index, no full layer data)
        LayersEngine::Get().ActivateLayer(snap->m_layerIdx);
    }
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

    const TransitionSnapshot* snap = g_snapshots[snapIdx].get();
    // m_duration == 0 means instant (set via the Scene Settings popup)
    double duration = snap->m_duration;

    // Place a named marker at the play cursor position if option is enabled
    if (g_placeMarker)
    {
        double pos = GetPlayPosition();
        AddProjectMarker2(nullptr, false, pos, 0.0, snap->m_name.c_str(), -1, 0);
    }

    TransitionEngine::Get().Recall(snap, snap->m_mask, duration);
    TransitionEngine::Get().SetCurrentSlot(snapIdx);
    Undo_OnStateChangeEx("Recall Scene", -1, -1);

    // Restore full layer state if "Recall layer with scene" is enabled
    RestoreLayerState(snap);

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
// ExportScene – write a single scene to a .tscene file
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
    ofn.lpstrFilter = "Scene Files (*.tscene)\0*.tscene\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = szFile;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = "tscene";
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
// ImportScene – load a .tscene file and append to g_snapshots
// ---------------------------------------------------------------------------
static void ImportScene(HWND hwnd)
{
    char szFile[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = "Scene Files (*.tscene)\0*.tscene\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = szFile;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = "tscene";
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
    if (g_hDragImages)
    {
        ImageList_DragLeave(hwnd);
        ImageList_EndDrag();
        ImageList_Destroy(g_hDragImages);
        g_hDragImages = nullptr;
    }
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
        SetDlgItemText(hwnd, IDC_SNAPNOTES, d->notes.c_str());
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
            d->notes = buf;

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
        CheckDlgButton(hwnd, IDC_GSET_MARKER,  g_placeMarker ? BST_CHECKED : BST_UNCHECKED);

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
        if (id == IDOK)
        {
            bool instant = (IsDlgButtonChecked(hwnd, IDC_GSET_INSTANT) == BST_CHECKED);
            if (instant)
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

            // Persist
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%.4f", g_defaultDuration);
            SetExtState("reaper_transitions", "def_duration", tmp, true);
            snprintf(tmp, sizeof(tmp), "%d", g_defaultTaper);
            SetExtState("reaper_transitions", "def_taper", tmp, true);
            snprintf(tmp, sizeof(tmp), "%.4f", g_defaultTaperExp);
            SetExtState("reaper_transitions", "def_taper_exp", tmp, true);

            g_placeMarker = (IsDlgButtonChecked(hwnd, IDC_GSET_MARKER) == BST_CHECKED);
            SetExtState("reaper_transitions", "place_marker", g_placeMarker ? "1" : "0", true);

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
        AppendMenu(hMenu, MF_STRING | ((!hasItem || isSpacer) ? MF_GRAYED : 0), CTX_EXPORT, "Export...");
        AppendMenu(hMenu, MF_STRING, CTX_IMPORT, "Import...");
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
                g_hDragImages = ListView_CreateDragImage(hList, g_dragSrc, &ptOffset);
                if (g_hDragImages)
                {
                    POINT dlgPt = pt;
                    ClientToScreen(hList, &dlgPt);
                    ScreenToClient(dlg, &dlgPt);
                    ImageList_BeginDrag(g_hDragImages, 0, 8, 8);
                    ImageList_DragEnter(dlg, dlgPt.x, dlgPt.y);
                }
            }
        }

        // Drag in progress – update image and drop-highlight
        if (g_dragSrc >= 0)
        {
            POINT dlgPt = pt;
            ClientToScreen(hList, &dlgPt);
            ScreenToClient(dlg, &dlgPt);
            if (g_hDragImages)
            {
                ImageList_DragMove(dlgPt.x, dlgPt.y);
                ImageList_DragShowNolock(FALSE);
            }
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
            if (g_hDragImages)
                ImageList_DragShowNolock(TRUE);
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
        s_lbTracking = false;
        if (g_dragSrc >= 0)
        {
            ReleaseCapture();
            DoEndDrag(dlg);
            return 0;
        }
        break;  // let the original list proc handle the normal click/release

    case WM_CAPTURECHANGED:
        s_lbTracking = false;
        if (g_dragSrc >= 0)
            DoEndDrag(dlg);
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

        // ---- Global default transition settings --------------------------
        {
            const char* dd = GetExtState("reaper_transitions", "def_duration");
            if (dd && dd[0]) { double d = atof(dd); g_defaultDuration = (d >= 0.0) ? d : 2.0; }
            const char* dt = GetExtState("reaper_transitions", "def_taper");
            if (dt && dt[0]) { int t = atoi(dt); g_defaultTaper = (t >= 0 && t <= TAPER_CUSTOM) ? t : TAPER_SCURVE; }
            const char* de = GetExtState("reaper_transitions", "def_taper_exp");
            if (de && de[0]) { double e = atof(de); g_defaultTaperExp = (e > 0.0) ? e : 2.0; }
            const char* dm = GetExtState("reaper_transitions", "place_marker");
            g_placeMarker = (dm && dm[0] == '1');
        }

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
            HWND hChild = GetWindow(hwnd, GW_CHILD);
            while (hChild)
            {
                RECT r;
                GetWindowRect(hChild, &r);
                MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&r, 2);
                if (r.left > threshold)
                {
                    SidebarCtrl sc;
                    sc.hwnd     = hChild;
                    sc.origLeft = r.left;
                    sc.origTop  = r.top;
                    sc.w        = r.right  - r.left;
                    sc.h        = r.bottom - r.top;
                    g_sidebarCtrls.push_back(sc);
                }
                hChild = GetWindow(hChild, GW_HWNDNEXT);
            }
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

        int dx = newCx - g_initCx;
        int dy = newCy - g_initCy;

        // Resize the ListView to fill extra width and height
        HWND hListSz = GetDlgItem(hwnd, IDC_LIST);
        if (hListSz && g_listInitRect.right > g_listInitRect.left)
        {
            int newW = (g_listInitRect.right  - g_listInitRect.left) + dx;
            int newH = (g_listInitRect.bottom - g_listInitRect.top)  + dy;
            if (newW > 10 && newH > 10)
            {
                SetWindowPos(hListSz, nullptr,
                    g_listInitRect.left, g_listInitRect.top, newW, newH,
                    SWP_NOZORDER | SWP_NOACTIVATE);

                // Stretch "Name" column to fill available list width
                int col0W = ListView_GetColumnWidth(hListSz, 0);
                int col2W = ListView_GetColumnWidth(hListSz, 2);
                int col1W = newW - col0W - col2W
                            - GetSystemMetrics(SM_CXVSCROLL) - 4;
                if (col1W > 20)
                    ListView_SetColumnWidth(hListSz, 1, col1W);
            }
        }

        // Shift right-sidebar controls by dx (keep same y, w, h)
        if (!g_sidebarCtrls.empty())
        {
            HDWP hdwp = BeginDeferWindowPos((int)g_sidebarCtrls.size());
            for (const auto& sc : g_sidebarCtrls)
            {
                hdwp = DeferWindowPos(hdwp, sc.hwnd, nullptr,
                    sc.origLeft + dx, sc.origTop, sc.w, sc.h,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            EndDeferWindowPos(hdwp);
        }

        InvalidateRect(hwnd, nullptr, TRUE);
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
        if (id == IDC_SNAPNAME && evt == EN_CHANGE && !g_syncingEditor)
        {
            int idx = GetSelectedListIndex(hwnd);
            if (idx >= 0 && idx < (int)g_snapshots.size())
            {
                char buf[256] = {};
                GetDlgItemText(hwnd, IDC_SNAPNAME, buf, sizeof(buf));
                g_snapshots[idx]->m_name = buf;
                // Update name column inline (no full refresh)
                ListView_SetItemText(GetDlgItem(hwnd, IDC_LIST), idx, 1, buf);
            }
            return TRUE;
        }

        if (id == IDC_SNAPNOTES && evt == EN_CHANGE && !g_syncingEditor)
        {
            int idx = GetSelectedListIndex(hwnd);
            if (idx >= 0 && idx < (int)g_snapshots.size())
            {
                char buf[4096] = {};
                GetDlgItemText(hwnd, IDC_SNAPNOTES, buf, sizeof(buf));
                g_snapshots[idx]->m_notes = buf;
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
                NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
                if (nia->iItem >= 0) DoRecall(hwnd, nia->iItem);
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
                    g_syncingEditor = true;
                    SetDlgItemText(hwnd, IDC_SNAPNAME, di->item.pszText);
                    g_syncingEditor = false;
                    SetWindowLongPtr(hwnd, DWLP_MSGRESULT, TRUE);
                    RefreshListView(hwnd);
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
            ShowWindow(hwnd, SW_HIDE);
        return TRUE;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
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
        TransitionEngine::Get().onTransitionComplete = nullptr;
        g_wnd = nullptr;
        return TRUE;

    default:
        break;
    }

    return FALSE;
}
