// ---------------------------------------------------------------------------
// LayersWnd.cpp  –  Live Tools: Layers  –  dockable window
//
// Left column  : ListView of the layers (drag-to-reorder, F2 to rename).
//                Columns: Name  Trks.  The active layer is drawn bold.
// Right column : ListView of every track in the project, in the project's own
//                order, laid out like REAPER's Track Manager. Columns:
//                Name  TCP  MCP  Spacer. A dot in TCP/MCP means the selected
//                layer shows that channel in that panel; click one to toggle
//                it, which is also what puts the channel in the layer or
//                takes it out. The Spacer cell adds or removes the gap above
//                the channel. Rows are the project's, so they do not drag.
// Bottom bar   : Settings... | status
//
// Everything else lives in the two lists' right-click menus: activate, show
// all, add/update/clear a layer, and the per-track actions.
//
// Settings modal (IDD_LAYERS_SETTINGS):
//   Apply track visibility (TCP and MCP) | Reorder tracks |
//   Restore on deactivate | Trigger MCP select | Manage spacers | Max channels
// ---------------------------------------------------------------------------
#include "LayersWnd.h"
#include "LayersEngine.h"
#include "api.h"
#include "resource.h"

#ifdef _WIN32
#  include <windowsx.h>
#  include <commctrl.h>
#endif
#include <cstdio>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static HINSTANCE s_hInst  = nullptr;
static HWND      s_hwnd   = nullptr;
static HWND      s_hSettingsDlg = nullptr;  // settings modal while it is open
static int       s_selLayer = 0;   // index of layer selected in list (0-based)

// Bold copy of the layer list's own font, used by NM_CUSTOMDRAW to mark the
// active layer. Built once the list exists and released with the window.
static HFONT     s_boldFont = nullptr;

// Drag state – layer list
static bool s_draggingLayer  = false;
static int  s_dragLayerSrc   = -1;
static int  s_dragLayerDst   = -1;
#ifdef _WIN32
static HIMAGELIST s_hLayerDragImg = nullptr;
#endif

// Drag state – track list within a layer
static bool s_draggingTrack  = false;
static int  s_dragTrackSrc   = -1;
static int  s_dragTrackDst   = -1;
#ifdef _WIN32
static HIMAGELIST s_hTrackDragImg = nullptr;
#endif

// Subclass state – layer list
static WNDPROC s_origLayerListProc = nullptr;
static bool    s_lyrLbTracking     = false;
static POINT   s_lyrLbDownPt       = {};
static int     s_lyrLbDownItem     = -1;
static DWORD   s_lyrLbDownTime     = 0;

// Subclass state – track list
static WNDPROC s_origTrackListProc = nullptr;
static bool    s_trkLbTracking     = false;
static POINT   s_trkLbDownPt       = {};
static int     s_trkLbDownItem     = -1;
static DWORD   s_trkLbDownTime     = 0;

// Track list columns. The list is modelled on REAPER's Track Manager: one
// row per project channel, a dot in TCP/MCP for each panel the layer shows
// that channel in, and a marker in Spacer for the visual gaps.
enum {
    LYRCOL_NAME   = 0,
    LYRCOL_TCP    = 1,
    LYRCOL_MCP    = 2,
    LYRCOL_SPACER = 3,
};

// ---------------------------------------------------------------------------
// Track list rows
//
// The list shows every track in the project, in the project's own order, for
// every layer — the dots say which of them the layer holds. It used to list
// only the layer's own channels, which meant a layer could only be built by
// first adding tracks to it and then ticking them, and there was no one view
// of "what is and is not in this layer".
//
// So a row is a project track, not a slot in LayerDef::tracks, and the two
// are joined by GUID. LayerDef::tracks still holds only the channels the
// layer actually contains: an entry is what membership *is*, so clearing a
// channel's last dot removes its entry, and the layer's channel count keeps
// meaning what it always did.
// ---------------------------------------------------------------------------
struct LyrRow {
    GUID guid = {};
    char name[160] = {};
};
static std::vector<LyrRow> s_trackRows;

// The layer's entry for a project track, or null when the layer does not
// hold it.
static LayerTrack* FindLayerTrack(LayerDef& ld, const GUID& g)
{
    for (auto& lt : ld.tracks)
        if (!lt.isSpacer && memcmp(&lt.guid, &g, sizeof(GUID)) == 0) return &lt;
    return nullptr;
}

// Index of that entry, or -1.
static int FindLayerTrackIdx(const LayerDef& ld, const GUID& g)
{
    for (int i = 0; i < (int)ld.tracks.size(); ++i)
        if (!ld.tracks[i].isSpacer &&
            memcmp(&ld.tracks[i].guid, &g, sizeof(GUID)) == 0) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// NormalizeLayerOrder - put a layer's stored slots into project order.
//
// The window no longer lets a layer define its own channel order: the list is
// the project's track list, so the order the slots are stored in has to agree
// with it or the spacer marks ("gap above this channel") would be drawn
// against one order and applied in another.
//
// Spacers travel with the channel they sit above, which is how the Spacer
// column has always read them. A spacer at the very end of the list has no
// channel to belong to and is dropped.
//
// Run after an edit, never merely on opening the window: a layer built under
// the old model keeps its hand-made order until the user actually changes
// something about it.
// ---------------------------------------------------------------------------
static void NormalizeLayerOrder(LayerDef& ld)
{
    // Nothing to sort against. Bailing rather than rebuilding from an empty
    // row list, which would throw the layer's channels away.
    if (s_trackRows.empty()) return;

    // Which channels carry a gap above them, by GUID.
    std::vector<GUID> spacedAbove;
    for (int i = 0; i + 1 < (int)ld.tracks.size(); ++i)
        if (ld.tracks[i].isSpacer && !ld.tracks[i + 1].isSpacer)
            spacedAbove.push_back(ld.tracks[i + 1].guid);

    auto hasSpacer = [&](const GUID& g) {
        for (const auto& sg : spacedAbove)
            if (memcmp(&sg, &g, sizeof(GUID)) == 0) return true;
        return false;
    };

    std::vector<LayerTrack> rebuilt;
    rebuilt.reserve(ld.tracks.size());
    for (const auto& row : s_trackRows)
    {
        const int idx = FindLayerTrackIdx(ld, row.guid);
        if (idx < 0) continue;
        if (hasSpacer(row.guid))
        {
            LayerTrack sp = {};
            sp.isSpacer = true;
            strncpy(sp.name, "--- Spacer ---", sizeof(sp.name) - 1);
            rebuilt.push_back(sp);
        }
        rebuilt.push_back(ld.tracks[idx]);
    }

    // Channels the project no longer has keep their slots, at the end. A
    // track can be missing because it was deleted, but it can equally be
    // missing because this ran against a project that has not finished
    // loading — dropping the slot would quietly shrink the layer either way,
    // and there is no undo for that.
    for (const auto& lt : ld.tracks)
    {
        if (lt.isSpacer) continue;
        bool present = false;
        for (const auto& row : s_trackRows)
            if (memcmp(&row.guid, &lt.guid, sizeof(GUID)) == 0) { present = true; break; }
        if (!present) rebuilt.push_back(lt);
    }

    ld.tracks.swap(rebuilt);
}

// Set or clear the gap above a channel. The channel has to be in the layer —
// a gap above something the layer does not show means nothing.
static void SetLayerSpacerAbove(LayerDef& ld, const GUID& g, bool on)
{
    const int idx = FindLayerTrackIdx(ld, g);
    if (idx < 0) return;
    const bool has = (idx > 0 && ld.tracks[idx - 1].isSpacer);
    if (has == on) return;
    if (on)
    {
        LayerTrack sp = {};
        sp.isSpacer = true;
        strncpy(sp.name, "--- Spacer ---", sizeof(sp.name) - 1);
        ld.tracks.insert(ld.tracks.begin() + idx, sp);
    }
    else
    {
        ld.tracks.erase(ld.tracks.begin() + (idx - 1));
    }
}

static bool LayerHasSpacerAbove(const LayerDef& ld, const GUID& g)
{
    const int idx = FindLayerTrackIdx(ld, g);
    return idx > 0 && ld.tracks[idx - 1].isSpacer;
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK LayersDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static void RefreshLayerList(HWND hwnd);
static void RefreshTrackList(HWND hwnd);
static void UpdateStatus(HWND hwnd);
static int  GetTrackListSel(HWND hwnd);
static void EndLayerDrag(HWND hwnd, bool apply);
static void EndTrackDrag(HWND hwnd, bool apply);
static LRESULT CALLBACK LayerListSubclassProc(HWND hList, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK TrackListSubclassProc(HWND hList, UINT msg, WPARAM wParam, LPARAM lParam);
static void DeleteSelectedLayer(HWND hwnd);
static void DeleteAllLayers(HWND hwnd);
static void RemoveSelectedTrack(HWND hwnd);
static void OnTrackListClick(HWND hwnd, HWND hList, POINT pt);

// ---------------------------------------------------------------------------
// ReadPanelVis - where a track is on screen right now
//
// Capture used to read the one panel the Target setting pointed at, so a
// layer taken from the mixer said nothing about the track panel and vice
// versa. Both are read now and a track counts as captured if either shows it,
// which is what lets a captured layer reproduce a TCP/MCP split instead of
// flattening it.
// ---------------------------------------------------------------------------
static void ReadPanelVis(MediaTrack* tr, bool& tcp, bool& mcp)
{
    tcp = mcp = false;
    if (!tr) return;
    if (bool* p = (bool*)GetSetMediaTrackInfo(tr, "B_SHOWINTCP",   nullptr)) tcp = *p;
    if (bool* p = (bool*)GetSetMediaTrackInfo(tr, "B_SHOWINMIXER", nullptr)) mcp = *p;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void LayersWnd_Init(HINSTANCE hInst)
{
    s_hInst = hInst;
}

void LayersWnd_Cleanup()
{
    if (s_hwnd && IsWindow(s_hwnd))
    {
        DestroyWindow(s_hwnd);
        s_hwnd = nullptr;
    }
}

void LayersWnd_ShowHide()
{
    if (!s_hwnd || !IsWindow(s_hwnd))
    {
        HWND hMain = GetMainHwnd();
        s_hwnd = CreateDialogParam(s_hInst,
                                   MAKEINTRESOURCE(IDD_LAYERS),
                                   hMain,
                                   LayersDlgProc,
                                   0);
        if (!s_hwnd)
        {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "CreateDialogParam failed for IDD_LAYERS. Error=%lu", GetLastError());
            MessageBoxA(hMain, buf, "Layers", MB_OK | MB_ICONERROR);
            return;
        }
    }

    if (IsWindowVisible(s_hwnd))
        ShowWindow(s_hwnd, SW_HIDE);
    else
    {
        LayersEngine::Get().RefreshAllTrackNames();
        ShowWindow(s_hwnd, SW_SHOW);
    }
}

int LayersWnd_IsVisible()
{
    return (s_hwnd && IsWindow(s_hwnd) && IsWindowVisible(s_hwnd)) ? 1 : 0;
}

void LayersWnd_Refresh()
{
    // Deliberately not gated on IsWindowVisible: a hidden window that is
    // refreshed anyway is correct the instant it is shown, whereas skipping
    // the refresh leaves it showing whatever it held when it was hidden.
    if (s_hwnd && IsWindow(s_hwnd))
    {
        LayersEngine::Get().RefreshAllTrackNames();
        RefreshLayerList(s_hwnd);
        RefreshTrackList(s_hwnd);
        UpdateStatus(s_hwnd);
    }
}

// ---------------------------------------------------------------------------
// RefreshLayerList
// ---------------------------------------------------------------------------
static void RefreshLayerList(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LYR_LAYER_LIST);
    if (!hList) return;

    int count  = LayersEngine::Get().GetLayerCount();

    int prevSel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (prevSel < 0) prevSel = s_selLayer;

    ListView_DeleteAllItems(hList);

    for (int i = 0; i < count; i++)
    {
        const LayerDef& ld = LayersEngine::Get().GetLayer(i);

        LVITEMA lvi = {};
        lvi.mask  = LVIF_TEXT;
        lvi.iItem = i;
        // Column 0 is the layer name, nothing else. The active layer used to
        // get a " *" suffix here, but ListView_EditLabel seeds the edit box
        // from the label — so renaming the active layer handed the user
        // "Name *" and stored the asterisk as part of the name. The active
        // row is drawn bold instead (NM_CUSTOMDRAW below), which cannot leak
        // into the data.
        char nameBuf[70];
        strncpy(nameBuf, ld.name, sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        lvi.pszText = nameBuf;
        ListView_InsertItem(hList, &lvi);

        // Track count = non-spacer entries
        int realTracks = 0;
        for (const auto& lt : ld.tracks)
            if (!lt.isSpacer) realTracks++;
        char trBuf[16];
        snprintf(trBuf, sizeof(trBuf), "%d", realTracks);
        ListView_SetItemText(hList, i, 1, trBuf);
    }

    int sel = (prevSel >= 0 && prevSel < count) ? prevSel : 0;
    if (count > 0)
    {
        ListView_SetItemState(hList, sel,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hList, sel, FALSE);
    }
}

// ---------------------------------------------------------------------------
// RefreshTrackList
// ---------------------------------------------------------------------------
static void RefreshTrackList(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LYR_TRACK_LIST);
    if (!hList) return;

    int prevSel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    ListView_DeleteAllItems(hList);

    // Rows are the project's tracks, in the project's order, whatever layer is
    // selected — and whether or not one is. Only the dots change with the
    // selection, so the list reads the same way every time it is opened.
    s_trackRows.clear();
    {
        const int nt = CountTracks(0);
        s_trackRows.reserve((size_t)nt);
        for (int t = 0; t < nt; t++)
        {
            MediaTrack* tr = GetTrack(0, t);
            if (!tr) continue;
            GUID* tg = GetTrackGUID(tr);
            if (!tg) continue;

            LyrRow row;
            row.guid = *tg;
            char nm[128] = {};
            if (!GetTrackName(tr, nm, sizeof(nm)) || nm[0] == '\0')
                snprintf(nm, sizeof(nm), "Track %d", t + 1);
            snprintf(row.name, sizeof(row.name), "%s", nm);
            s_trackRows.push_back(row);
        }
    }

    const int n = LayersEngine::Get().GetLayerCount();
    LayerDef* ld = (s_selLayer >= 0 && s_selLayer < n)
                   ? &LayersEngine::Get().GetLayer(s_selLayer) : nullptr;

    // Channels past the layer's slot limit are bracketed, the same as before.
    // Counted among the layer's members, not among the rows: the rows now
    // include every track the layer does not hold.
    const LayersSettings& cfg = LayersEngine::Get().GetSettings();
    const int limit = cfg.globalMaxChannels;
    int memberOrdinal = 0;

    for (int i = 0; i < (int)s_trackRows.size(); i++)
    {
        const bool member = ld && FindLayerTrackIdx(*ld, s_trackRows[i].guid) >= 0;

        char dispName[200];
        // ASCII on purpose: this is an ANSI list view, so a real em dash would
        // arrive as whatever the system code page makes of its UTF-8 bytes.
        if (member && limit > 0 && memberOrdinal >= limit)
            snprintf(dispName, sizeof(dispName), "[%s]", s_trackRows[i].name);
        else
            snprintf(dispName, sizeof(dispName), "%s", s_trackRows[i].name);
        if (member) memberOrdinal++;

        LVITEMA lvi = {};
        lvi.mask     = LVIF_TEXT;
        lvi.iItem    = i;
        lvi.iSubItem = LYRCOL_NAME;
        lvi.pszText  = dispName;
        ListView_InsertItem(hList, &lvi);
    }

    if (prevSel >= 0 && prevSel < (int)s_trackRows.size())
    {
        ListView_SetItemState(hList, prevSel,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
}

// ---------------------------------------------------------------------------
// Delete selected layer(s) – called from context menu + Del key
// ---------------------------------------------------------------------------
static void DeleteSelectedLayer(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LYR_LAYER_LIST);
    if (!hList) return;
    int n = LayersEngine::Get().GetLayerCount();
    if (n == 0) return;

    // Collect all selected indices
    std::vector<int> sel;
    int i = -1;
    while ((i = ListView_GetNextItem(hList, i, LVNI_SELECTED)) >= 0)
        sel.push_back(i);
    if (sel.empty()) return;

    char msg[128];
    if (sel.size() == 1)
    {
        const LayerDef& ld = LayersEngine::Get().GetLayer(sel[0]);
        snprintf(msg, sizeof(msg), "Delete layer \"%.60s\"?", ld.name);
    }
    else
        snprintf(msg, sizeof(msg), "Delete %d selected layers?", (int)sel.size());
    if (MessageBoxA(hwnd, msg, "Layers", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    // Delete from highest index downward to preserve lower indices
    for (int j = (int)sel.size() - 1; j >= 0; j--)
        LayersEngine::Get().RemoveLayer(sel[j]);

    s_selLayer = (s_selLayer > 0) ? s_selLayer - 1 : 0;
    int remaining = LayersEngine::Get().GetLayerCount();
    if (s_selLayer >= remaining) s_selLayer = remaining > 0 ? remaining - 1 : 0;
    RefreshLayerList(hwnd);
    RefreshTrackList(hwnd);
    UpdateStatus(hwnd);
}

// Delete ALL layers
static void DeleteAllLayers(HWND hwnd)
{
    if (LayersEngine::Get().GetLayerCount() == 0) return;
    if (MessageBoxA(hwnd, "Delete ALL layers?", "Layers", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    while (LayersEngine::Get().GetLayerCount() > 0)
        LayersEngine::Get().RemoveLayer(0);
    s_selLayer = 0;
    RefreshLayerList(hwnd);
    RefreshTrackList(hwnd);
    UpdateStatus(hwnd);
}

// ---------------------------------------------------------------------------
// Remove selected track(s) from the current layer
// ---------------------------------------------------------------------------
static void RemoveSelectedTrack(HWND hwnd)
{
    int n = LayersEngine::Get().GetLayerCount();
    if (s_selLayer < 0 || s_selLayer >= n) return;
    LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);

    HWND hList = GetDlgItem(hwnd, IDC_LYR_TRACK_LIST);
    if (!hList) return;

    // The rows are the project's tracks and always will be, so removing one
    // cannot mean removing its row. It clears the channel's dots, which is
    // what takes it out of the layer.
    bool changed = false;
    int i = -1;
    while ((i = ListView_GetNextItem(hList, i, LVNI_SELECTED)) >= 0)
    {
        if (i < 0 || i >= (int)s_trackRows.size()) continue;
        const int idx = FindLayerTrackIdx(ld, s_trackRows[i].guid);
        if (idx < 0) continue;
        // Take the gap above it with it: a spacer left behind would attach
        // itself to whichever channel ends up below it.
        if (idx > 0 && ld.tracks[idx - 1].isSpacer)
            ld.tracks.erase(ld.tracks.begin() + (idx - 1), ld.tracks.begin() + (idx + 1));
        else
            ld.tracks.erase(ld.tracks.begin() + idx);
        changed = true;
    }
    if (!changed) return;

    NormalizeLayerOrder(ld);
    LayersEngine::Get().SaveExtState();
    LayersEngine::Get().MarkLayoutEdited(s_selLayer);
    RefreshTrackList(hwnd);
    RefreshLayerList(hwnd);
}
// ---------------------------------------------------------------------------
// OnTrackListClick – handle a click in one of the Track-Manager-style columns
//
// TCP and MCP toggle where the clicked channel appears when the layer is
// recalled — and, because a row is a project track rather than a slot the
// layer owns, they are also what puts a channel into the layer and takes it
// out. Lighting either dot on a channel the layer does not hold adds it;
// clearing its last dot removes it, so an entry in LayerDef::tracks keeps
// meaning exactly "this layer shows this channel somewhere".
//
// Spacer adds or removes the gap above the clicked channel, and only works on
// a channel the layer holds: a gap above something it does not show has
// nowhere to be.
//
// A toggle applies to the clicked row only, never to the whole selection: a
// click that silently rewrote every selected row would be far too easy to
// fire by accident in the middle of a show.
// ---------------------------------------------------------------------------
static void OnTrackListClick(HWND hwnd, HWND hList, POINT pt)
{
    int n = LayersEngine::Get().GetLayerCount();
    if (s_selLayer < 0 || s_selLayer >= n) return;
    LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);

    LVHITTESTINFO ht = {};
    ht.pt = pt;
    ListView_SubItemHitTest(hList, &ht);
    const int row = ht.iItem;
    const int col = ht.iSubItem;
    if (row < 0 || row >= (int)s_trackRows.size()) return;
    if (col != LYRCOL_TCP && col != LYRCOL_MCP && col != LYRCOL_SPACER) return;

    const GUID& g = s_trackRows[row].guid;
    bool rebuildList = false;

    if (col == LYRCOL_SPACER)
    {
        if (FindLayerTrackIdx(ld, g) < 0) return;   // not in this layer
        SetLayerSpacerAbove(ld, g, !LayerHasSpacerAbove(ld, g));
    }
    else
    {
        LayerTrack* lt = FindLayerTrack(ld, g);
        if (!lt)
        {
            // First dot on a channel the layer did not hold: add it, showing
            // it only in the panel that was clicked.
            LayerTrack nt = {};
            nt.guid     = g;
            snprintf(nt.name, sizeof(nt.name), "%s", s_trackRows[row].name);
            nt.showTcp  = (col == LYRCOL_TCP);
            nt.showMcp  = (col == LYRCOL_MCP);
            ld.tracks.push_back(nt);
            NormalizeLayerOrder(ld);
            rebuildList = true;   // the bracketed slot-limit marks can shift
        }
        else
        {
            if (col == LYRCOL_TCP) lt->showTcp = !lt->showTcp;
            else                   lt->showMcp = !lt->showMcp;

            if (!lt->showTcp && !lt->showMcp)
            {
                // Last dot cleared: the layer no longer shows this channel
                // anywhere, which is the same thing as not holding it. Drop
                // the entry rather than leaving a member that does nothing
                // and still counts against the slot limit.
                const int idx = FindLayerTrackIdx(ld, g);
                if (idx >= 0)
                {
                    if (idx > 0 && ld.tracks[idx - 1].isSpacer)
                        ld.tracks.erase(ld.tracks.begin() + (idx - 1),
                                        ld.tracks.begin() + (idx + 1));
                    else
                        ld.tracks.erase(ld.tracks.begin() + idx);
                }
                rebuildList = true;
            }
        }
    }

    LayersEngine::Get().SaveExtState();
    // Stored only, like every other edit in this list — it reaches the
    // project on the layer's next recall, never mid-show under the pointer.
    LayersEngine::Get().MarkLayoutEdited(s_selLayer);

    if (rebuildList || col == LYRCOL_SPACER)
    {
        RefreshTrackList(hwnd);
        RefreshLayerList(hwnd);
    }
    else
    {
        RECT rcRow;
        if (ListView_GetItemRect(hList, row, &rcRow, LVIR_BOUNDS))
            InvalidateRect(hList, &rcRow, FALSE);
        RefreshLayerList(hwnd);   // the layer's channel count may have changed
    }
}

static void UpdateStatus(HWND hwnd)
{
    int active = LayersEngine::Get().GetActiveLayer();
    int n      = LayersEngine::Get().GetLayerCount();
    char buf[128];
    if (active >= 0 && active < n)
        snprintf(buf, sizeof(buf), "Active: %s (Layer %d)",
            LayersEngine::Get().GetLayer(active).name, active + 1);
    else
        strcpy(buf, "No layer active (all tracks shown)");
    SetDlgItemText(hwnd, IDC_LYR_STATUS, buf);
}

// ---------------------------------------------------------------------------
// Get selected index in track list
// ---------------------------------------------------------------------------
static int GetTrackListSel(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LYR_TRACK_LIST);
    if (!hList) return -1;
    return ListView_GetNextItem(hList, -1, LVNI_SELECTED);
}

// ---------------------------------------------------------------------------
// Drag helpers
// ---------------------------------------------------------------------------
static void EndLayerDrag(HWND hwnd, bool apply)
{
#ifdef _WIN32
    if (s_hLayerDragImg)
    {
        ImageList_DragLeave(hwnd);
        ImageList_EndDrag();
        ImageList_Destroy(s_hLayerDragImg);
        s_hLayerDragImg = nullptr;
    }
#endif
    ReleaseCapture();

    HWND hList = GetDlgItem(hwnd, IDC_LYR_LAYER_LIST);
    if (hList)
    {
        int cnt = LayersEngine::Get().GetLayerCount();
        for (int i = 0; i < cnt; i++)
            ListView_SetItemState(hList, i, 0, LVIS_DROPHILITED);
    }

    if (apply && s_dragLayerDst >= 0 && s_dragLayerDst != s_dragLayerSrc)
    {
        LayersEngine::Get().MoveLayer(s_dragLayerSrc, s_dragLayerDst);
        s_selLayer = s_dragLayerDst;
        RefreshLayerList(hwnd);
        RefreshTrackList(hwnd);
    }

    s_draggingLayer = false;
    s_dragLayerSrc  = s_dragLayerDst = -1;
}

// Unreachable since drag-to-reorder left this list (see TrackListSubclassProc);
// kept so the capture/cancel paths still tear a drag down cleanly if one is
// ever armed again.
static void EndTrackDrag(HWND hwnd, bool apply)
{
#ifdef _WIN32
    if (s_hTrackDragImg)
    {
        ImageList_DragLeave(hwnd);
        ImageList_EndDrag();
        ImageList_Destroy(s_hTrackDragImg);
        s_hTrackDragImg = nullptr;
    }
#endif
    ReleaseCapture();

    HWND hListT = GetDlgItem(hwnd, IDC_LYR_TRACK_LIST);
    if (hListT)
        ListView_SetItemState(hListT, -1, 0, LVIS_DROPHILITED);

    int n = LayersEngine::Get().GetLayerCount();
    if (apply && s_dragTrackDst >= 0 && s_dragTrackDst != s_dragTrackSrc &&
        s_selLayer >= 0 && s_selLayer < n)
    {
        LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);
        int sz = (int)ld.tracks.size();
        int from = s_dragTrackSrc;
        int to   = s_dragTrackDst;
        if (from >= 0 && from < sz && to >= 0 && to < sz)
        {
            LayerTrack temp = ld.tracks[from];
            if (from < to)
                for (int i = from; i < to; i++) ld.tracks[i] = ld.tracks[i + 1];
            else
                for (int i = from; i > to; i--) ld.tracks[i] = ld.tracks[i - 1];
            ld.tracks[to] = temp;
            LayersEngine::Get().SaveExtState();
            // Stored only — dragging a row here does not move the project's
            // tracks. The new order goes live the next time the layer is
            // recalled.
            LayersEngine::Get().MarkLayoutEdited(s_selLayer);
        }
        RefreshTrackList(hwnd);
        RefreshLayerList(hwnd);
    }

    s_draggingTrack = false;
    s_dragTrackSrc  = s_dragTrackDst = -1;
}

// ---------------------------------------------------------------------------
// Settings dialog proc
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        const LayersSettings& cfg = LayersEngine::Get().GetSettings();
        // "Layers control: MCP/TCP" and "Also hide in the other panel" are
        // gone. A layer drives both panels now, and which of them a given
        // channel appears in is a per-track choice made in the TCP/MCP columns
        // of the Layers window.
        CheckDlgButton(hwnd, IDC_LYR_SET_MCPVIS,  cfg.applyMcpVisibility  ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_LYR_SET_REORDER,  cfg.reorderTracks       ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_LYR_SET_RESTORE,  cfg.restoreOnDeactivate ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_LYR_SET_TRIGGERMCP, cfg.triggerMcpSelect  ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, IDC_LYR_SET_SPACERS,    cfg.manageSpacers     ? BST_CHECKED : BST_UNCHECKED);
        // Set up max channels spin
        {
            HWND hSpin = GetDlgItem(hwnd, IDC_LYR_MAXCH_SPIN);
            HWND hEdit = GetDlgItem(hwnd, IDC_LYR_MAXCH_EDIT);
            if (hSpin && hEdit)
            {
                SendMessage(hSpin, UDM_SETRANGE, 0, MAKELONG(512, 0));
                SendMessage(hSpin, UDM_SETBUDDY, (WPARAM)hEdit, 0);
            }
        }
        SetDlgItemInt(hwnd, IDC_LYR_MAXCH_EDIT, cfg.globalMaxChannels, FALSE);

        s_hSettingsDlg = hwnd;
        return TRUE;
    }


    case WM_DESTROY:
        s_hSettingsDlg = nullptr;
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
        {
            LayersSettings cfg;
            cfg.applyMcpVisibility  = (IsDlgButtonChecked(hwnd, IDC_LYR_SET_MCPVIS)  == BST_CHECKED);
            cfg.reorderTracks       = (IsDlgButtonChecked(hwnd, IDC_LYR_SET_REORDER)  == BST_CHECKED);
            cfg.restoreOnDeactivate = (IsDlgButtonChecked(hwnd, IDC_LYR_SET_RESTORE)  == BST_CHECKED);
            cfg.triggerMcpSelect    = (IsDlgButtonChecked(hwnd, IDC_LYR_SET_TRIGGERMCP) == BST_CHECKED);
            cfg.manageSpacers       = (IsDlgButtonChecked(hwnd, IDC_LYR_SET_SPACERS)    == BST_CHECKED);
            BOOL ok = FALSE;
            int val = (int)GetDlgItemInt(hwnd, IDC_LYR_MAXCH_EDIT, &ok, FALSE);
            cfg.globalMaxChannels   = (ok && val >= 0) ? val : 0;
            LayersEngine::Get().SetSettings(cfg);
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Layer list subclass – smooth drag-to-reorder via mouse-move threshold
// ---------------------------------------------------------------------------
static LRESULT CALLBACK LayerListSubclassProc(HWND hList, UINT msg, WPARAM wParam, LPARAM lParam)
{
    HWND dlg = GetParent(hList);
    switch (msg)
    {
    case WM_LBUTTONDOWN:
    {
        LRESULT r = CallWindowProc(s_origLayerListProc, hList, msg, wParam, lParam);
        LVHITTESTINFO hti = {};
        hti.pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (ListView_HitTest(hList, &hti) >= 0)
        {
            s_lyrLbTracking = true;
            s_lyrLbDownPt   = hti.pt;
            s_lyrLbDownItem = hti.iItem;
            s_lyrLbDownTime = GetTickCount();
        }
        return r;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (s_lyrLbTracking && !(wParam & MK_LBUTTON))
            s_lyrLbTracking = false;
        if (s_lyrLbTracking && !s_draggingLayer)
        {
            bool moved = (abs(pt.x - s_lyrLbDownPt.x) > GetSystemMetrics(SM_CXDRAG) ||
                          abs(pt.y - s_lyrLbDownPt.y) > GetSystemMetrics(SM_CYDRAG));
            bool held  = (GetTickCount() - s_lyrLbDownTime >= 200);
            if (moved && held)
            {
                s_dragLayerSrc  = s_lyrLbDownItem;
                s_dragLayerDst  = -1;
                s_lyrLbTracking = false;
                SetCapture(hList);
                POINT off = { 8, 8 };
#ifdef _WIN32
                s_hLayerDragImg = ListView_CreateDragImage(hList, s_dragLayerSrc, &off);
                if (s_hLayerDragImg)
                {
                    POINT dlgPt = pt;
                    ClientToScreen(hList, &dlgPt);
                    ScreenToClient(dlg, &dlgPt);
                    ImageList_BeginDrag(s_hLayerDragImg, 0, 8, 8);
                    ImageList_DragEnter(dlg, dlgPt.x, dlgPt.y);
                }
#endif
                s_draggingLayer = true;
            }
        }
        if (s_draggingLayer)
        {
            POINT dlgPt = pt;
            ClientToScreen(hList, &dlgPt);
            ScreenToClient(dlg, &dlgPt);
#ifdef _WIN32
            if (s_hLayerDragImg)
            {
                ImageList_DragMove(dlgPt.x, dlgPt.y);
                ImageList_DragShowNolock(FALSE);
            }
#endif
            LVHITTESTINFO hti = {};
            hti.pt = pt;
            int dst = ListView_HitTest(hList, &hti);
            if (dst != s_dragLayerDst)
            {
                if (s_dragLayerDst >= 0)
                    ListView_SetItemState(hList, s_dragLayerDst, 0, LVIS_DROPHILITED);
                s_dragLayerDst = dst;
                if (dst >= 0)
                    ListView_SetItemState(hList, dst, LVIS_DROPHILITED, LVIS_DROPHILITED);
            }
#ifdef _WIN32
            if (s_hLayerDragImg)
                ImageList_DragShowNolock(TRUE);
#endif
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP:
        s_lyrLbTracking = false;
        if (s_draggingLayer)
        {
            s_draggingLayer = false;
            EndLayerDrag(dlg, true);
        }
        break;
    case WM_CAPTURECHANGED:
        s_lyrLbTracking = false;
        if (s_draggingLayer)
        {
            s_draggingLayer = false;
            EndLayerDrag(dlg, false);
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_DELETE)
        {
            DeleteSelectedLayer(dlg);
            return 0;
        }
        if (wParam == VK_F2)
        {
            HWND hLayerList = GetDlgItem(dlg, IDC_LYR_LAYER_LIST);
            int sel = ListView_GetNextItem(hLayerList, -1, LVNI_SELECTED);
            if (sel >= 0)
                ListView_EditLabel(hLayerList, sel);
            return 0;
        }
        break;
    }
    return CallWindowProc(s_origLayerListProc, hList, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Track list subclass – smooth drag-to-reorder via mouse-move threshold
// ---------------------------------------------------------------------------
static LRESULT CALLBACK TrackListSubclassProc(HWND hList, UINT msg, WPARAM wParam, LPARAM lParam)
{
    HWND dlg = GetParent(hList);
    switch (msg)
    {
    case WM_LBUTTONDOWN:
    {
        LRESULT r = CallWindowProc(s_origTrackListProc, hList, msg, wParam, lParam);
        LVHITTESTINFO hti = {};
        hti.pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ListView_SubItemHitTest(hList, &hti);
        // Drag-to-reorder is gone from this list. Its rows are the project's
        // tracks in the project's order, so there is no per-layer order left
        // for a drag to express — reordering happens in REAPER and this list
        // follows on the next refresh. The tracking state stays wired up
        // below so the drag code paths remain a single dead branch rather
        // than a half-removed feature.
        (void)hti;
        return r;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (s_trkLbTracking && !(wParam & MK_LBUTTON))
            s_trkLbTracking = false;
        if (s_trkLbTracking && !s_draggingTrack)
        {
            bool moved = (abs(pt.x - s_trkLbDownPt.x) > GetSystemMetrics(SM_CXDRAG) ||
                          abs(pt.y - s_trkLbDownPt.y) > GetSystemMetrics(SM_CYDRAG));
            bool held  = (GetTickCount() - s_trkLbDownTime >= 200);
            if (moved && held)
            {
                s_dragTrackSrc  = s_trkLbDownItem;
                s_dragTrackDst  = -1;
                s_trkLbTracking = false;
                SetCapture(hList);
                POINT off = { 8, 8 };
#ifdef _WIN32
                s_hTrackDragImg = ListView_CreateDragImage(hList, s_dragTrackSrc, &off);
                if (s_hTrackDragImg)
                {
                    POINT dlgPt = pt;
                    ClientToScreen(hList, &dlgPt);
                    ScreenToClient(dlg, &dlgPt);
                    ImageList_BeginDrag(s_hTrackDragImg, 0, 8, 8);
                    ImageList_DragEnter(dlg, dlgPt.x, dlgPt.y);
                }
#endif
                s_draggingTrack = true;
            }
        }
        if (s_draggingTrack)
        {
            POINT dlgPt = pt;
            ClientToScreen(hList, &dlgPt);
            ScreenToClient(dlg, &dlgPt);
#ifdef _WIN32
            if (s_hTrackDragImg)
            {
                ImageList_DragMove(dlgPt.x, dlgPt.y);
                ImageList_DragShowNolock(FALSE);
            }
#endif
            LVHITTESTINFO hti = {};
            hti.pt = pt;
            int dst = ListView_HitTest(hList, &hti);
            if (dst != s_dragTrackDst)
            {
                if (s_dragTrackDst >= 0)
                    ListView_SetItemState(hList, s_dragTrackDst, 0, LVIS_DROPHILITED);
                s_dragTrackDst = dst;
                if (dst >= 0)
                    ListView_SetItemState(hList, dst, LVIS_DROPHILITED, LVIS_DROPHILITED);
            }
#ifdef _WIN32
            if (s_hTrackDragImg)
                ImageList_DragShowNolock(TRUE);
#endif
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP:
        s_trkLbTracking = false;
        if (s_draggingTrack)
        {
            s_draggingTrack = false;
            EndTrackDrag(dlg, true);
        }
        break;
    case WM_CAPTURECHANGED:
        s_trkLbTracking = false;
        if (s_draggingTrack)
        {
            s_draggingTrack = false;
            EndTrackDrag(dlg, false);
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_DELETE)
        {
            RemoveSelectedTrack(dlg);
            return 0;
        }
        break;
    }
    return CallWindowProc(s_origTrackListProc, hList, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Main dialog proc
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK LayersDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    // -----------------------------------------------------------------------
    case WM_INITDIALOG:
    {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS };
        InitCommonControlsEx(&icc);

        // ---- Create layer ListView (left panel) ----------------------------
        {
            HWND hPh = GetDlgItem(hwnd, IDC_LYR_LAYER_LIST);
            RECT r = {};
            GetWindowRect(hPh, &r);
            MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&r, 2);
            DestroyWindow(hPh);

            HWND hList = CreateWindowExA(0, "SysListView32", "",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
                LVS_REPORT | LVS_SHOWSELALWAYS | LVS_EDITLABELS,
                r.left, r.top, r.right - r.left, r.bottom - r.top,
                hwnd, (HMENU)(INT_PTR)IDC_LYR_LAYER_LIST, s_hInst, nullptr);

            if (hList)
            {
                ListView_SetExtendedListViewStyle(hList,
                    LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

                // Bold variant of the list's own font, for the active layer.
                if (!s_boldFont)
                {
                    HFONT hf = (HFONT)SendMessage(hList, WM_GETFONT, 0, 0);
                    LOGFONT lf = {};
                    if (hf && GetObject(hf, sizeof(lf), &lf))
                    {
                        lf.lfWeight = FW_BOLD;
                        s_boldFont  = CreateFontIndirect(&lf);
                    }
                }

                LVCOLUMNA col = {};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.cx = 166; col.pszText = const_cast<char*>("Name");
                ListView_InsertColumn(hList, 0, &col);
                col.cx = 40;  col.pszText = const_cast<char*>("Trks");
                ListView_InsertColumn(hList, 1, &col);
                s_origLayerListProc = (WNDPROC)(LONG_PTR)SetWindowLongPtr(
                    hList, GWLP_WNDPROC, (LONG_PTR)LayerListSubclassProc);
            }
        }

        // ---- Create track ListView (right panel) --------------------------
        {
            HWND hPh = GetDlgItem(hwnd, IDC_LYR_TRACK_LIST);
            RECT r = {};
            GetWindowRect(hPh, &r);
            MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&r, 2);
            DestroyWindow(hPh);

            HWND hList = CreateWindowExA(0, "SysListView32", "",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
                LVS_REPORT | LVS_SHOWSELALWAYS,
                r.left, r.top, r.right - r.left, r.bottom - r.top,
                hwnd, (HMENU)(INT_PTR)IDC_LYR_TRACK_LIST, s_hInst, nullptr);

            if (hList)
            {
                ListView_SetExtendedListViewStyle(hList,
                    LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

                // Laid out like REAPER's Track Manager: the channel, then a
                // dot per panel, then the spacer marker. The row number the
                // first column used to hold is gone — it numbered slots in a
                // list the user reorders by dragging, so it never said
                // anything the row's position did not already say.
                LVCOLUMNA col = {};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.cx = 200; col.pszText = const_cast<char*>("Name");
                ListView_InsertColumn(hList, LYRCOL_NAME, &col);
                col.mask |= LVCF_FMT;
                col.fmt = LVCFMT_CENTER;
                col.cx = 42; col.pszText = const_cast<char*>("TCP");
                ListView_InsertColumn(hList, LYRCOL_TCP, &col);
                col.cx = 42; col.pszText = const_cast<char*>("MCP");
                ListView_InsertColumn(hList, LYRCOL_MCP, &col);
                col.cx = 48; col.pszText = const_cast<char*>("Spacer");
                ListView_InsertColumn(hList, LYRCOL_SPACER, &col);
                s_origTrackListProc = (WNDPROC)(LONG_PTR)SetWindowLongPtr(
                    hList, GWLP_WNDPROC, (LONG_PTR)TrackListSubclassProc);
            }
        }

        // ---- Max channels spin -------------------------------------------
        // (spin is now in the Settings dialog; nothing to set up here)

        // ---- Populate -------------------------------------------------------
        LayersEngine::Get().RefreshAllTrackNames();
        s_selLayer = 0;
        RefreshLayerList(hwnd);
        RefreshTrackList(hwnd);
        UpdateStatus(hwnd);


        return TRUE;
    }

    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);

        switch (id)
        {
        // "Show All" lost its bottom-bar button; it lives in the layer list's
        // right-click menu now (CTX_LYR_SHOW_ALL).

        case IDC_LYR_SETTINGS_BTN:
            DialogBox(s_hInst,
                      MAKEINTRESOURCE(IDD_LAYERS_SETTINGS),
                      hwnd,
                      SettingsDlgProc);
            RefreshTrackList(hwnd);   // global max channels may have changed
            UpdateStatus(hwnd);
            break;

        case IDCANCEL:
            ShowWindow(hwnd, SW_HIDE);
            return TRUE;
        }
        return TRUE;
    }

    // -----------------------------------------------------------------------
    case WM_CONTEXTMENU:
    {
        // Context menu IDs
        enum {
            CTX_LYR_ACTIVATE     = 3001,
            CTX_LYR_DELETE       = 3004,
            CTX_LYR_ADD_LAYER    = 3005,
            CTX_LYR_CAPTURE      = 3006,
            CTX_LYR_CLEAR        = 3007,
            CTX_LYR_RENAME       = 3008,
            CTX_LYR_ADD_FROM_MCP = 3009,
            CTX_TRK_MOVE_UP    = 3010,
            CTX_TRK_MOVE_DOWN  = 3011,
            CTX_TRK_SPACER_BEF = 3012,
            CTX_TRK_SPACER_AFT = 3013,
            CTX_TRK_REMOVE     = 3014,
            CTX_TRK_CAPTURE    = 3015,
            CTX_TRK_CLEAR      = 3016,
            CTX_TRK_ADD_SEL    = 3017,
            CTX_LYR_DELETE_ALL = 3018,
            CTX_TRK_DELETE_ALL = 3019,
            CTX_LYR_SHOW_ALL   = 3020,
        };

        HWND hCtrl = (HWND)wParam;
        int  sx    = GET_X_LPARAM(lParam);
        int  sy    = GET_Y_LPARAM(lParam);

        HWND hLayerList = GetDlgItem(hwnd, IDC_LYR_LAYER_LIST);
        HWND hTrackList = GetDlgItem(hwnd, IDC_LYR_TRACK_LIST);

        if (hCtrl == hLayerList)
        {
            // Hit-test to find which item was right-clicked
            POINT ptC = { sx, sy };
            if (sx == -1 && sy == -1)
                GetCursorPos(&ptC);
            POINT ptL = ptC;
            ScreenToClient(hLayerList, &ptL);
            LVHITTESTINFO ht = {};
            ht.pt = ptL;
            int item = ListView_HitTest(hLayerList, &ht);

            if (item >= 0)
            {
                // Select the item
                ListView_SetItemState(hLayerList, item,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                s_selLayer = item;
                RefreshTrackList(hwnd);
            }

            int n = LayersEngine::Get().GetLayerCount();
            HMENU hMenu = CreatePopupMenu();
            AppendMenuA(hMenu, MF_STRING | (item < 0 ? MF_GRAYED : 0),
                CTX_LYR_ACTIVATE, "Activate\tDbl-click");
            AppendMenuA(hMenu, MF_STRING, CTX_LYR_SHOW_ALL, "Show All Tracks");
            AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(hMenu, MF_STRING,
                CTX_LYR_ADD_LAYER, "Add Layer");
            // Both capture items read the TCP and the MCP together and record
            // the split per track, so neither label names a panel any more.
            AppendMenuA(hMenu, MF_STRING, CTX_LYR_ADD_FROM_MCP,
                        "Add Layer (Current Visibility)");
            const char* updLabel = "Update Layer (Capture Visible Tracks)";
            AppendMenuA(hMenu, MF_STRING | (item < 0 ? MF_GRAYED : 0),
                CTX_LYR_RENAME, "Rename\tF2");
            AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(hMenu, MF_STRING | (item < 0 ? MF_GRAYED : 0),
                CTX_LYR_CAPTURE, updLabel);
            AppendMenuA(hMenu, MF_STRING | (item < 0 ? MF_GRAYED : 0),
                CTX_LYR_CLEAR, "Clear Tracks");
            AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(hMenu, MF_STRING | (item < 0 ? MF_GRAYED : 0),
                CTX_LYR_DELETE, "Delete Layer\tDel");
            AppendMenuA(hMenu, MF_STRING | (n == 0 ? MF_GRAYED : 0),
                CTX_LYR_DELETE_ALL, "Delete All Layers");

            int cmd = (int)TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                ptC.x, ptC.y, hwnd, nullptr);
            DestroyMenu(hMenu);

            switch (cmd)
            {
            case CTX_LYR_ACTIVATE:
                if (item >= 0 && item < n)
                {
                    LayersEngine::Get().ActivateLayer(item);
                    RefreshLayerList(hwnd);
                    UpdateStatus(hwnd);
                }
                break;
            case CTX_LYR_ADD_LAYER:
            {
                int idx = LayersEngine::Get().AddLayer(nullptr);
                s_selLayer = idx;
                HWND hLayerList = GetDlgItem(hwnd, IDC_LYR_LAYER_LIST);
                RefreshLayerList(hwnd);
                RefreshTrackList(hwnd);
                UpdateStatus(hwnd);
                // Start inline rename immediately
                ListView_SetItemState(hLayerList, idx,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EditLabel(hLayerList, idx);
                break;
            }
            case CTX_LYR_RENAME:
            {
                if (item >= 0)
                {
                    HWND hLayerList = GetDlgItem(hwnd, IDC_LYR_LAYER_LIST);
                    ListView_EditLabel(hLayerList, item);
                }
                break;
            }
            case CTX_LYR_ADD_FROM_MCP:
            {
                int idx = LayersEngine::Get().AddLayer(nullptr);
                s_selLayer = idx;
                LayerDef& newLd = LayersEngine::Get().GetLayer(idx);
                int numTracks = CountTracks(0);
                for (int t = 0; t < numTracks; t++)
                {
                    MediaTrack* tr = GetTrack(0, t);
                    if (!tr) continue;
                    bool visTcp = false, visMcp = false;
                    ReadPanelVis(tr, visTcp, visMcp);
                    int spacerVal = 0;
                    int* sp = (int*)GetSetMediaTrackInfo(tr, "I_SPACER", nullptr);
                    if (sp) spacerVal = *sp;
                    if (!visTcp && !visMcp) continue;
                    GUID* tg = GetTrackGUID(tr);
                    if (!tg) continue;
                    if (spacerVal > 0)
                    {
                        LayerTrack spacerLt = {};
                        spacerLt.isSpacer = true;
                        strncpy(spacerLt.name, "--- Spacer ---", sizeof(spacerLt.name) - 1);
                        newLd.tracks.push_back(spacerLt);
                    }
                    LayerTrack lt = {};
                    lt.guid = *tg;
                    lt.showTcp = visTcp;
                    lt.showMcp = visMcp;
                    int* pfc = (int*)GetSetMediaTrackInfo(tr, "I_FOLDERCOMPACT", nullptr);
                    if (pfc) lt.folderCompact = *pfc;
                    char buf[128] = {};
                    GetTrackName(tr, buf, (int)sizeof(buf));
                    strncpy(lt.name, buf, sizeof(lt.name) - 1);
                    newLd.tracks.push_back(lt);
                }
                LayersEngine::Get().SaveExtState();
                HWND hLL = GetDlgItem(hwnd, IDC_LYR_LAYER_LIST);
                RefreshLayerList(hwnd);
                RefreshTrackList(hwnd);
                UpdateStatus(hwnd);
                ListView_SetItemState(hLL, idx,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EditLabel(hLL, idx);
                break;
            }
            case CTX_LYR_CAPTURE:
            {
                if (item < 0 || item >= n) break;
                LayerDef& ld = LayersEngine::Get().GetLayer(item);
                // Right-clicking the wrong row here silently discards a track
                // list, so confirm which layer is about to be replaced.
                {
                    char prompt[224];
                    snprintf(prompt, sizeof(prompt),
                        "Update \"%s\" to the tracks currently visible in the "
                        "track panel or the mixer?\n\n"
                        "The layer's existing track list will be replaced.",
                        ld.name);
                    if (MessageBoxA(hwnd, prompt, "Layers",
                                    MB_YESNO | MB_ICONQUESTION) != IDYES) break;
                }
                ld.tracks.clear();
                int numTracks = CountTracks(0);
                for (int t = 0; t < numTracks; t++)
                {
                    MediaTrack* tr = GetTrack(0, t);
                    if (!tr) continue;
                    bool visTcp = false, visMcp = false;
                    ReadPanelVis(tr, visTcp, visMcp);
                    if (!visTcp && !visMcp) continue;
                    GUID* tg = GetTrackGUID(tr);
                    if (!tg) continue;
                    // Capture any native REAPER spacer above this track
                    int* sp = (int*)GetSetMediaTrackInfo(tr, "I_SPACER", nullptr);
                    if (sp && *sp > 0)
                    {
                        LayerTrack spacerLt = {};
                        spacerLt.isSpacer = true;
                        strncpy(spacerLt.name, "--- Spacer ---", sizeof(spacerLt.name) - 1);
                        ld.tracks.push_back(spacerLt);
                    }
                    LayerTrack lt = {};
                    lt.guid = *tg;
                    lt.showTcp = visTcp;
                    lt.showMcp = visMcp;
                    int* pfc = (int*)GetSetMediaTrackInfo(tr, "I_FOLDERCOMPACT", nullptr);
                    if (pfc) lt.folderCompact = *pfc;
                    char buf[128] = {};
                    GetTrackName(tr, buf, (int)sizeof(buf));
                    strncpy(lt.name, buf, sizeof(lt.name) - 1);
                    ld.tracks.push_back(lt);
                }
                LayersEngine::Get().SaveExtState();
                RefreshTrackList(hwnd);
                RefreshLayerList(hwnd);
                {
                    char status[96];
                    snprintf(status, sizeof(status), "Updated \"%s\" - %d tracks",
                             ld.name, (int)ld.tracks.size());
                    SetDlgItemText(hwnd, IDC_LYR_STATUS, status);
                }
                break;
            }
            case CTX_LYR_CLEAR:
            {
                if (item < 0 || item >= n) break;
                if (MessageBoxA(hwnd, "Clear all tracks from this layer?",
                    "Layers", MB_YESNO | MB_ICONQUESTION) != IDYES) break;
                LayersEngine::Get().GetLayer(item).tracks.clear();
                LayersEngine::Get().SaveExtState();
                RefreshTrackList(hwnd);
                RefreshLayerList(hwnd);
                break;
            }
            case CTX_LYR_SHOW_ALL:
                LayersEngine::Get().Deactivate();
                RefreshLayerList(hwnd);
                UpdateStatus(hwnd);
                break;
            case CTX_LYR_DELETE:
                DeleteSelectedLayer(hwnd);
                break;
            case CTX_LYR_DELETE_ALL:
                DeleteAllLayers(hwnd);
                break;
            }
        }
        else if (hCtrl == hTrackList)
        {
            POINT ptC = { sx, sy };
            if (sx == -1 && sy == -1)
                GetCursorPos(&ptC);
            POINT ptL = ptC;
            ScreenToClient(hTrackList, &ptL);
            LVHITTESTINFO ht = {};
            ht.pt = ptL;
            int item = ListView_HitTest(hTrackList, &ht);

            int n = LayersEngine::Get().GetLayerCount();
            int numTracks = (s_selLayer >= 0 && s_selLayer < n)
                ? (int)LayersEngine::Get().GetLayer(s_selLayer).tracks.size() : 0;

            if (item >= 0)
            {
                // Right-clicking inside an existing selection keeps it, so the
                // menu acts on the whole selection; right-clicking outside one
                // selects just that row first. Selecting unconditionally, as
                // this did before, bolted the clicked row onto an unrelated
                // selection and made "Remove" ambiguous.
                const bool alreadySel =
                    (ListView_GetItemState(hTrackList, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                if (!alreadySel)
                {
                    const int cnt = ListView_GetItemCount(hTrackList);
                    for (int k = 0; k < cnt; k++)
                        ListView_SetItemState(hTrackList, k, 0, LVIS_SELECTED);
                    ListView_SetItemState(hTrackList, item,
                        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                }
            }

            // How many rows the menu will act on, for the Remove label.
            int selCount = 0;
            {
                int si = -1;
                while ((si = ListView_GetNextItem(hTrackList, si, LVNI_SELECTED)) >= 0)
                    selCount++;
            }

            // Move Up / Move Down are gone: the rows are the project's track
            // list in the project's order, so there is nothing here to move.
            // Reorder tracks in REAPER and this list follows.
            const bool rowInLayer =
                (item >= 0 && item < (int)s_trackRows.size() && s_selLayer >= 0 &&
                 s_selLayer < n &&
                 FindLayerTrackIdx(LayersEngine::Get().GetLayer(s_selLayer),
                                   s_trackRows[item].guid) >= 0);

            HMENU hMenu = CreatePopupMenu();
            AppendMenuA(hMenu, MF_STRING | (s_selLayer < 0 ? MF_GRAYED : 0),
                CTX_TRK_ADD_SEL, "Add Selected Tracks");
            AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
            // A gap can only sit above a channel the layer actually shows.
            AppendMenuA(hMenu, MF_STRING | (!rowInLayer ? MF_GRAYED : 0),
                CTX_TRK_SPACER_BEF, "Toggle Spacer Above");
            AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
            char rmLabel[64];
            if (selCount > 1)
                snprintf(rmLabel, sizeof(rmLabel),
                         "Remove %d Tracks from Layer\tDel", selCount);
            else
                snprintf(rmLabel, sizeof(rmLabel), "Remove from Layer\tDel");
            AppendMenuA(hMenu, MF_STRING | (selCount < 1 ? MF_GRAYED : 0),
                CTX_TRK_REMOVE, rmLabel);
            AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(hMenu, MF_STRING | (s_selLayer < 0 ? MF_GRAYED : 0),
                CTX_TRK_CAPTURE, "Capture Visible Tracks");
            AppendMenuA(hMenu, MF_STRING | (s_selLayer < 0 ? MF_GRAYED : 0),
                CTX_TRK_CLEAR, "Clear All Tracks");

            int cmd = (int)TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                ptC.x, ptC.y, hwnd, nullptr);
            DestroyMenu(hMenu);

            if (s_selLayer < 0 || s_selLayer >= n) break;
            LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);

            switch (cmd)
            {
            case CTX_TRK_SPACER_BEF:
                // Same toggle the Spacer cell performs; the menu is here for
                // the keyboard and for a narrow window where the column is
                // scrolled out of sight.
                if (item >= 0 && item < (int)s_trackRows.size())
                {
                    const GUID& g = s_trackRows[item].guid;
                    if (FindLayerTrackIdx(ld, g) >= 0)
                    {
                        SetLayerSpacerAbove(ld, g, !LayerHasSpacerAbove(ld, g));
                        LayersEngine::Get().SaveExtState();
                        // Stored only — adding a spacer here used to re-apply
                        // the whole layer to the project. It shows up on the
                        // tracks the next time the layer is recalled.
                        LayersEngine::Get().MarkLayoutEdited(s_selLayer);
                        RefreshTrackList(hwnd);
                        RefreshLayerList(hwnd);
                        ListView_SetItemState(hTrackList, item,
                            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    }
                }
                break;
            case CTX_TRK_REMOVE:
                // Every selected row, not just the one that was right-clicked.
                // RemoveSelectedTrack already walks the selection and erases
                // from the highest index down; this case used to ignore it and
                // delete the single hit-tested row instead, so removing a
                // multi-track selection silently dropped one track.
                RemoveSelectedTrack(hwnd);
                break;
            case CTX_TRK_DELETE_ALL:
                if (!ld.tracks.empty())
                {
                    if (MessageBoxA(hwnd, "Delete ALL tracks from this layer?",
                        "Layers", MB_YESNO | MB_ICONQUESTION) == IDYES)
                    {
                        ld.tracks.clear();
                        LayersEngine::Get().SaveExtState();
                        RefreshTrackList(hwnd);
                        RefreshLayerList(hwnd);
                    }
                }
                break;
            case CTX_TRK_ADD_SEL:
            {
                int added = 0;
                int numSel = CountSelectedTracks(0);
                for (int i = 0; i < numSel; i++)
                {
                    MediaTrack* tr = GetSelectedTrack(0, i);
                    if (!tr) continue;
                    GUID* tg = GetTrackGUID(tr);
                    if (!tg) continue;
                    bool dup = false;
                    for (const auto& lt : ld.tracks)
                        if (!lt.isSpacer && memcmp(&lt.guid, tg, sizeof(GUID)) == 0) { dup = true; break; }
                    if (dup) continue;
                    LayerTrack lt = {};
                    lt.guid = *tg;
                    char buf[128] = {};
                    GetTrackName(tr, buf, (int)sizeof(buf));
                    strncpy(lt.name, buf, sizeof(lt.name) - 1);
                    ld.tracks.push_back(lt);
                    added++;
                }
                if (added)
                {
                    // Keep the stored slots in project order, which is the
                    // order the list draws them in and the order the spacer
                    // marks are read against.
                    NormalizeLayerOrder(ld);
                    LayersEngine::Get().SaveExtState();
                    LayersEngine::Get().MarkLayoutEdited(s_selLayer);
                    RefreshTrackList(hwnd);
                    RefreshLayerList(hwnd);
                    char status[64];
                    snprintf(status, sizeof(status), "Added %d track(s)", added);
                    SetDlgItemText(hwnd, IDC_LYR_STATUS, status);
                }
                break;
            }
            case CTX_TRK_CAPTURE:
            {
                ld.tracks.clear();
                int numTracks = CountTracks(0);
                for (int t = 0; t < numTracks; t++)
                {
                    MediaTrack* tr = GetTrack(0, t);
                    if (!tr) continue;
                    bool visTcp = false, visMcp = false;
                    ReadPanelVis(tr, visTcp, visMcp);
                    if (!visTcp && !visMcp) continue;
                    GUID* tg = GetTrackGUID(tr);
                    if (!tg) continue;
                    // Capture any native REAPER spacer above this track
                    int* sp = (int*)GetSetMediaTrackInfo(tr, "I_SPACER", nullptr);
                    if (sp && *sp > 0)
                    {
                        LayerTrack spacerLt = {};
                        spacerLt.isSpacer = true;
                        strncpy(spacerLt.name, "--- Spacer ---", sizeof(spacerLt.name) - 1);
                        ld.tracks.push_back(spacerLt);
                    }
                    LayerTrack lt = {};
                    lt.guid = *tg;
                    lt.showTcp = visTcp;
                    lt.showMcp = visMcp;
                    int* pfc = (int*)GetSetMediaTrackInfo(tr, "I_FOLDERCOMPACT", nullptr);
                    if (pfc) lt.folderCompact = *pfc;
                    char buf[128] = {};
                    GetTrackName(tr, buf, (int)sizeof(buf));
                    strncpy(lt.name, buf, sizeof(lt.name) - 1);
                    ld.tracks.push_back(lt);
                }
                LayersEngine::Get().SaveExtState();
                RefreshTrackList(hwnd);
                RefreshLayerList(hwnd);
                {
                    char status[64];
                    snprintf(status, sizeof(status), "Captured %d tracks", (int)ld.tracks.size());
                    SetDlgItemText(hwnd, IDC_LYR_STATUS, status);
                }
                break;
            }
            case CTX_TRK_CLEAR:
                if (MessageBoxA(hwnd, "Clear all tracks from this layer?",
                    "Layers", MB_YESNO | MB_ICONQUESTION) != IDYES) break;
                ld.tracks.clear();
                LayersEngine::Get().SaveExtState();
                RefreshTrackList(hwnd);
                RefreshLayerList(hwnd);
                break;
            }
        }
        break;
    }

    // -----------------------------------------------------------------------
    case WM_NOTIFY:
    {
        NMHDR* hdr = (NMHDR*)lParam;

        // ---- Layer list notifications ------------------------------------
        if (hdr->idFrom == IDC_LYR_LAYER_LIST)
        {
            if (hdr->code == NM_CUSTOMDRAW)
            {
                // Mark the active layer by weight rather than by decorating
                // its text: anything written into the label ends up in the
                // rename box, and from there in the layer name.
                NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lParam;
                LRESULT res = CDRF_DODEFAULT;
                switch (cd->nmcd.dwDrawStage)
                {
                case CDDS_PREPAINT:
                    res = s_boldFont ? CDRF_NOTIFYITEMDRAW : CDRF_DODEFAULT;
                    break;
                case CDDS_ITEMPREPAINT:
                    if (s_boldFont &&
                        (int)cd->nmcd.dwItemSpec == LayersEngine::Get().GetActiveLayer())
                    {
                        SelectObject(cd->nmcd.hdc, s_boldFont);
                        res = CDRF_NEWFONT;
                    }
                    break;
                }
                SetWindowLongPtr(hwnd, DWLP_MSGRESULT, res);
                return TRUE;
            }
            else if (hdr->code == LVN_ITEMCHANGED)
            {
                NMLISTVIEW* nlv = (NMLISTVIEW*)lParam;
                if ((nlv->uNewState & LVIS_SELECTED) && nlv->iItem >= 0)
                {
                    s_selLayer = nlv->iItem;
                    RefreshTrackList(hwnd);
                }
            }
            else if (hdr->code == NM_DBLCLK)
            {
                NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
                if (nia->iItem >= 0 && nia->iItem < LayersEngine::Get().GetLayerCount())
                {
                    s_selLayer = nia->iItem;
                    LayersEngine::Get().ActivateLayer(s_selLayer);
                    RefreshLayerList(hwnd);
                    UpdateStatus(hwnd);
                }
            }
            else if (hdr->code == LVN_ENDLABELEDIT)
            {
                NMLVDISPINFOA* di = (NMLVDISPINFOA*)lParam;
                // pszText == nullptr means user cancelled
                if (di->item.pszText && di->item.pszText[0] != '\0')
                {
                    int idx = di->item.iItem;
                    int n   = LayersEngine::Get().GetLayerCount();
                    if (idx >= 0 && idx < n)
                    {
                        LayerDef& ld = LayersEngine::Get().GetLayer(idx);
                        strncpy(ld.name, di->item.pszText, sizeof(ld.name) - 1);
                        ld.name[sizeof(ld.name) - 1] = '\0';
                        LayersEngine::Get().SaveExtState();
                        LayersEngine::Get().UpdateLayerActionDesc(idx);
                        // Returning TRUE tells ListView to accept the edit
                        SetWindowLongPtr(hwnd, DWLP_MSGRESULT, TRUE);
                        RefreshLayerList(hwnd);
                        return TRUE;
                    }
                }
                SetWindowLongPtr(hwnd, DWLP_MSGRESULT, FALSE);
                return TRUE;
            }
        }

        // ---- Track list notifications ------------------------------------
        else if (hdr->idFrom == IDC_LYR_TRACK_LIST)
        {
            if (hdr->code == NM_CLICK)
            {
                NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
                OnTrackListClick(hwnd, hdr->hwndFrom, nia->ptAction);
            }
            else if (hdr->code == NM_CUSTOMDRAW)
            {
                // TCP, MCP and Spacer are drawn rather than spelled out: a dot
                // reads at a glance across a long channel list, and it is the
                // same shorthand REAPER's own Track Manager uses, so the two
                // windows can be read the same way.
                // Every path out of here has to set DWLP_MSGRESULT, because
                // this case ends in `return TRUE` and the list view then reads
                // whatever was left there last — which, after one painted
                // dot, is CDRF_SKIPDEFAULT. Leaving it stale blanked the Name
                // column, so the result is tracked in one variable and written
                // once at the bottom.
                NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lParam;
                LRESULT res = CDRF_DODEFAULT;
                switch (cd->nmcd.dwDrawStage)
                {
                case CDDS_PREPAINT:
                    res = CDRF_NOTIFYITEMDRAW;
                    break;

                case CDDS_ITEMPREPAINT:
                    res = CDRF_NOTIFYSUBITEMDRAW;
                    break;

                case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
                {
                    const int col = (int)cd->iSubItem;
                    if (col != LYRCOL_TCP && col != LYRCOL_MCP && col != LYRCOL_SPACER)
                        break;   // Name draws normally

                    const int row = (int)cd->nmcd.dwItemSpec;
                    int n = LayersEngine::Get().GetLayerCount();
                    if (s_selLayer < 0 || s_selLayer >= n) break;
                    const LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);
                    if (row < 0 || row >= (int)s_trackRows.size()) break;

                    // A row is a project track; the layer's entry for it is
                    // what the dots read, and its absence is what an empty
                    // cell means.
                    const GUID& rowGuid = s_trackRows[row].guid;
                    const int   ltIdx   = FindLayerTrackIdx(ld, rowGuid);

                    HDC  hdc  = cd->nmcd.hdc;
                    RECT rc   = cd->nmcd.rc;
                    HWND hLst = hdr->hwndFrom;
                    const bool sel =
                        (ListView_GetItemState(hLst, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;

                    SetBkColor(hdc, GetSysColor(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW));
                    ExtTextOutA(hdc, 0, 0, ETO_OPAQUE, &rc, "", 0, nullptr);

                    bool on = false;
                    if (ltIdx >= 0)
                    {
                        const LayerTrack& lt = ld.tracks[ltIdx];
                        if      (col == LYRCOL_TCP) on = lt.showTcp;
                        else if (col == LYRCOL_MCP) on = lt.showMcp;
                        // Marked against the channel the gap sits above, the
                        // way the Track Manager does it.
                        else /* LYRCOL_SPACER */    on = (ltIdx > 0 &&
                                                          ld.tracks[ltIdx - 1].isSpacer);
                    }

                    if (on)
                    {
                        const COLORREF fg = GetSysColor(sel ? COLOR_HIGHLIGHTTEXT
                                                            : COLOR_WINDOWTEXT);
                        const int cx = (rc.left + rc.right)  / 2;
                        const int cy = (rc.top  + rc.bottom) / 2;

                        if (col == LYRCOL_SPACER)
                        {
                            // A dash, not a dot: a spacer is a gap between
                            // channels, not a panel the channel lives in, and
                            // it should not read as a third toggle of the same
                            // kind as TCP and MCP.
                            RECT rcd = { cx - 5, cy - 1, cx + 5, cy + 1 };
                            HBRUSH hb = CreateSolidBrush(fg);
                            FillRect(hdc, &rcd, hb);
                            DeleteObject(hb);
                        }
                        else
                        {
                            const int r = 3;
                            HBRUSH hb  = CreateSolidBrush(fg);
                            HPEN   hp  = CreatePen(PS_SOLID, 1, fg);
                            HGDIOBJ ob = SelectObject(hdc, hb);
                            HGDIOBJ op = SelectObject(hdc, hp);
                            Ellipse(hdc, cx - r, cy - r, cx + r + 1, cy + r + 1);
                            SelectObject(hdc, ob);
                            SelectObject(hdc, op);
                            DeleteObject(hb);
                            DeleteObject(hp);
                        }
                    }

                    res = CDRF_SKIPDEFAULT;
                    break;
                }
                }
                SetWindowLongPtr(hwnd, DWLP_MSGRESULT, res);
                return TRUE;
            }
        }

        return TRUE;
    }

    // -----------------------------------------------------------------------
    case WM_SIZE:
    {
        // Stretch layer list and track list to fill available height
        RECT rc;
        GetClientRect(hwnd, &rc);

        // We rely on the dialog's resizeable frame; no dynamic re-layout needed
        // for now – keeping it simple.
        (void)rc;
        break;
    }

    // -----------------------------------------------------------------------
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return TRUE;

    case WM_DESTROY:
        if (s_boldFont) { DeleteObject(s_boldFont); s_boldFont = nullptr; }
        s_hwnd = nullptr;
        return TRUE;
    }

    return FALSE;
}
