// ---------------------------------------------------------------------------
// LayersWnd.cpp  –  Live Tools: Layers  –  dockable window
//
// Left column  : ListView of the layers (drag-to-reorder, F2 to rename).
//                Columns: Name  Trks.  The active layer is drawn bold.
// Right column : ListView of every track in the project, in the project's own
//                order, laid out like REAPER's Track Manager. Columns:
//                #  Name  TCP  MCP  Spacer. # is the track number on the
//                track's color; Name is indented by folder, with a folder's
//                children marked like the Scenes list's subscenes. A dot in TCP/MCP means the
//                selected layer shows that channel in that panel; click one to
//                toggle it, which is also what puts the channel in the layer
//                or takes it out — live, when the layer is the active one. A
//                dash in Spacer marks the gap above a channel: click it, press
//                Ins, or use the right-click menu. Spacers add no rows; the
//                rows are the project's tracks, so they do not drag.
// Bottom bar   : status. The layer settings live in Global Settings.
//
// Everything else lives in the two lists' right-click menus: activate, show
// all, add/update/clear a layer, and the per-track actions.
// ---------------------------------------------------------------------------
#include "LayersWnd.h"
#include "LayersEngine.h"
#include "api.h"
#include "resource.h"
#include "ReaperTheme.h"

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
//
// The first column is the track number on the track's own color, as in the
// Track Manager, and Name is indented by folder depth, each child marked
// with the same corner the Scenes list puts in front of a subscene. Folders
// do not fold here: every project track always has a row.
enum {
    LYRCOL_NUM    = 0,
    LYRCOL_NAME   = 1,
    LYRCOL_TCP    = 2,
    LYRCOL_MCP    = 3,
    LYRCOL_SPACER = 4,
};

// Folder tree geometry in the Name cell.
static const int kFolderIndent = 12;   // per nesting level

// Track color box in the # cell, left of the number.
static const int kColorBoxSize = 10;

// The Spacer column: a dash marks the gap above a channel, and clicking (or
// dragging down) the column adds or removes gaps. Ins and the right-click
// menu do the same job, so this switch can hide the column without losing
// any way to manage spacers.
static const bool kShowSpacerColumn = true;

// Columns that hold a clickable dot.
static bool IsDotCol(int col)
{
    if (col == LYRCOL_TCP || col == LYRCOL_MCP) return true;
    return kShowSpacerColumn && col == LYRCOL_SPACER;
}

// Drag-to-toggle across the dot columns.
//
// Ticking a dozen channels into a layer one click at a time is the single
// most common thing this window is for, so the dots follow the pointer while
// the button is down, the way the Safes grid's checkboxes do. The direction
// is decided by the first cell — press on an empty dot and the drag fills,
// press on a filled one and it clears — so a drag can never do both at once.
static bool s_dotDragActive   = false;
static bool s_dotDragOn       = false;  // what the drag is writing
static int  s_dotDragLastRow  = -1;
static int  s_dotDragLastCol  = -1;
// Adding or removing a channel changes the slot list's shape, which the
// bracketed slot-limit marks and the layer list's channel count are drawn
// from. Rebuilding those per cell would be wasted work and would fight the
// drag, so it is deferred to the button coming up.
static bool s_dotDragDirty    = false;

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
    GUID     guid      = {};
    char     name[160] = {};
    int      trackNum  = 0;      // 1-based project position
    int      depth     = 0;      // folder nesting level
    bool     isFolder  = false;  // opens a folder
    int      folderCompact = 0;  // the project's I_FOLDERCOMPACT, for a folder joining a layer
    bool     hasColor  = false;
    COLORREF color     = 0;
};
static std::vector<LyrRow> s_trackRows;

// Every project track, in project order. The same tracks as the rows above,
// for code that wants just the project's own order.
static std::vector<GUID> s_projGuids;

// A row in range.
static bool IsTrackRow(int row)
{
    return row >= 0 && row < (int)s_trackRows.size();
}

// `a` moved pctB percent of the way toward `b`.
static COLORREF BlendColor(COLORREF a, COLORREF b, int pctB)
{
    const int pa = 100 - pctB;
    return RGB((GetRValue(a) * pa + GetRValue(b) * pctB) / 100,
               (GetGValue(a) * pa + GetGValue(b) * pctB) / 100,
               (GetBValue(a) * pa + GetBValue(b) * pctB) / 100);
}

// Background for a row's cells, in the REAPER theme's list colours like the
// other windows. Selected rows take the selection color; a colored track is
// tinted toward its color the way the TCP tints it.
static COLORREF RowBg(HWND hList, const LyrRow& lr, int row)
{
    const COLORREF bg = ReaperTheme_ListCellBg(hList, row);
    if (bg != ReaperTheme_List().bg) return bg;   // selected
    if (lr.hasColor) return BlendColor(bg, lr.color, 35);
    return bg;
}

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
    // track list, which would throw the layer's channels away.
    if (s_projGuids.empty()) return;

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
    for (const GUID& pg : s_projGuids)
    {
        const int idx = FindLayerTrackIdx(ld, pg);
        if (idx < 0) continue;
        if (hasSpacer(pg))
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
        for (const GUID& pg : s_projGuids)
            if (memcmp(&pg, &lt.guid, sizeof(GUID)) == 0) { present = true; break; }
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
static bool ApplyDotCell(HWND hwnd, HWND hList, int row, int col, bool on,
                         bool deferRebuild);

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

// Writes a cell only when its text differs, so an unchanged cell is not
// invalidated. Both lists are refreshed on every project change.
static void SetItemTextIfChanged(HWND hList, int item, int sub, const char* text)
{
    char cur[256] = {};
    ListView_GetItemText(hList, item, sub, cur, sizeof(cur));
    if (strcmp(cur, text) != 0)
        ListView_SetItemText(hList, item, sub, const_cast<char*>(text));
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
    const bool hadSel = prevSel >= 0;
    if (prevSel < 0) prevSel = s_selLayer;

    // Same row count: rewrite the text in place.
    // The timer refreshes this list on every project change, most of which
    // have nothing to do with layers, and deleting every row each time made
    // the window flicker and cancelled an in-progress rename.
    const bool rebuild = ListView_GetItemCount(hList) != count;
    SendMessage(hList, WM_SETREDRAW, FALSE, 0);
    if (rebuild) ListView_DeleteAllItems(hList);

    for (int i = 0; i < count; i++)
    {
        const LayerDef& ld = LayersEngine::Get().GetLayer(i);

        // Column 0 is the layer name, nothing else. The active layer used to
        // get a " *" suffix here, but ListView_EditLabel seeds the edit box
        // from the label — so renaming the active layer handed the user
        // "Name *" and stored the asterisk as part of the name. The active
        // row is drawn bold instead (NM_CUSTOMDRAW below), which cannot leak
        // into the data.
        char nameBuf[70];
        strncpy(nameBuf, ld.name, sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';

        // Track count = non-spacer entries
        int realTracks = 0;
        for (const auto& lt : ld.tracks)
            if (!lt.isSpacer) realTracks++;
        char trBuf[16];
        snprintf(trBuf, sizeof(trBuf), "%d", realTracks);

        if (rebuild)
        {
            LVITEMA lvi = {};
            lvi.mask    = LVIF_TEXT;
            lvi.iItem   = i;
            lvi.pszText = nameBuf;
            ListView_InsertItem(hList, &lvi);
            ListView_SetItemText(hList, i, 1, trBuf);
        }
        else
        {
            SetItemTextIfChanged(hList, i, 0, nameBuf);
            SetItemTextIfChanged(hList, i, 1, trBuf);
        }
    }

    // An in-place update keeps whatever the user had selected; only a rebuild
    // (or a list with nothing selected) needs the selection put back.
    int sel = (prevSel >= 0 && prevSel < count) ? prevSel : 0;
    if (count > 0 && (rebuild || !hadSel))
    {
        ListView_SetItemState(hList, sel,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hList, sel, FALSE);
    }

    SendMessage(hList, WM_SETREDRAW, TRUE, 0);
    // The bold active row is decided at paint time, so repaint even when no
    // text changed.
    InvalidateRect(hList, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// RefreshTrackList
// ---------------------------------------------------------------------------
static void RefreshTrackList(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LYR_TRACK_LIST);
    if (!hList) return;

    int prevSel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);

    const int n = LayersEngine::Get().GetLayerCount();
    LayerDef* ld = (s_selLayer >= 0 && s_selLayer < n)
                   ? &LayersEngine::Get().GetLayer(s_selLayer) : nullptr;

    // Rows are the project's tracks, in the project's order, whatever layer is
    // selected — and whether or not one is. Only the dots change with the
    // selection, so the list reads the same way every time it is opened.
    s_trackRows.clear();
    s_projGuids.clear();
    {
        const int nt = CountTracks(0);
        s_trackRows.reserve((size_t)nt);
        s_projGuids.reserve((size_t)nt);
        int depth = 0;                   // enclosing folders
        for (int t = 0; t < nt; t++)
        {
            MediaTrack* tr = GetTrack(0, t);
            if (!tr) continue;
            GUID* tg = GetTrackGUID(tr);
            int fd = 0;
            if (int* p = (int*)GetSetMediaTrackInfo(tr, "I_FOLDERDEPTH", nullptr)) fd = *p;

            if (tg) s_projGuids.push_back(*tg);

            LyrRow row;
            if (tg) row.guid = *tg;
            row.trackNum = t + 1;
            row.depth    = depth;
            row.isFolder = fd >= 1;
            if (row.isFolder)
                if (int* p = (int*)GetSetMediaTrackInfo(tr, "I_FOLDERCOMPACT", nullptr))
                    row.folderCompact = *p;

            if (tg)
            {
                char nm[128] = {};
                if (!GetTrackName(tr, nm, sizeof(nm)) || nm[0] == '\0')
                    snprintf(nm, sizeof(nm), "Track %d", t + 1);
                snprintf(row.name, sizeof(row.name), "%s", nm);

                // Same convention as GetTrackColor: 0 is "no color", anything
                // else is a native color with 0x1000000 set.
                const int nc = GetTrackColor(tr);
                if (nc != 0)
                {
                    int r = 0, g = 0, b = 0;
                    ColorFromNative(nc & 0xFFFFFF, &r, &g, &b);
                    row.hasColor = true;
                    row.color    = RGB(r, g, b);
                }

                s_trackRows.push_back(row);
            }

            if (row.isFolder)  depth++;
            else if (fd < 0)   depth = (depth + fd > 0) ? depth + fd : 0;
        }
    }

    // Channels past the layer's slot limit are bracketed, the same as before.
    const LayersSettings& cfg = LayersEngine::Get().GetSettings();
    const int limit = cfg.globalMaxChannels;

    // Same row count: rewrite the text in place instead of deleting every
    // row. The list view holds only the number and Name text — dots, colors
    // and folder marks are read from s_trackRows at paint time — so this is
    // all a rebuild would change.
    const bool rebuild = ListView_GetItemCount(hList) != (int)s_trackRows.size();
    SendMessage(hList, WM_SETREDRAW, FALSE, 0);
    if (rebuild) ListView_DeleteAllItems(hList);

    for (int i = 0; i < (int)s_trackRows.size(); i++)
    {
        const LyrRow& lr = s_trackRows[i];
        // Past the limit by the slot the channel holds, which is what recall
        // counts, not the row.
        const int slot = ld ? FindLayerTrackIdx(*ld, lr.guid) : -1;

        char dispName[200] = "";
        char numBuf[16]    = "";
        // ASCII on purpose: this is an ANSI list view, so a real em dash would
        // arrive as whatever the system code page makes of its UTF-8 bytes.
        if (slot >= 0 && limit > 0 && slot >= limit)
            snprintf(dispName, sizeof(dispName), "[%s]", lr.name);
        else
            snprintf(dispName, sizeof(dispName), "%s", lr.name);
        snprintf(numBuf, sizeof(numBuf), "%d", lr.trackNum);

        if (rebuild)
        {
            LVITEMA lvi = {};
            lvi.mask     = LVIF_TEXT;
            lvi.iItem    = i;
            lvi.iSubItem = LYRCOL_NUM;
            lvi.pszText  = numBuf;
            ListView_InsertItem(hList, &lvi);
            ListView_SetItemText(hList, i, LYRCOL_NAME, dispName);
        }
        else
        {
            SetItemTextIfChanged(hList, i, LYRCOL_NUM,  numBuf);
            SetItemTextIfChanged(hList, i, LYRCOL_NAME, dispName);
        }
    }

    if (rebuild && prevSel >= 0 && prevSel < (int)s_trackRows.size())
    {
        ListView_SetItemState(hList, prevSel,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    SendMessage(hList, WM_SETREDRAW, TRUE, 0);
    // The dots are drawn from the layer at paint time, so repaint even when
    // no name changed.
    InvalidateRect(hList, nullptr, FALSE);
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
// CommitVisibilityEdit - store a change to which channels a layer shows where.
//
// On the active layer the change goes straight onto the tracks: that layer is
// what is on screen, and a dot that does not move the screen reads as broken.
// On any other layer it is stored only and waits for that layer's recall.
// Order edits never come through here — they stay stored-only even on the
// active layer. Spacers have their own path (CommitSpacerEdit).
// ---------------------------------------------------------------------------
static void CommitVisibilityEdit(int layerIdx)
{
    LayersEngine& le = LayersEngine::Get();
    le.SaveExtState();
    if (layerIdx >= 0 && layerIdx == le.GetActiveLayer())
        le.ApplyVisibilityNow(layerIdx);
    else
        le.MarkLayoutEdited(layerIdx);
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
    CommitVisibilityEdit(s_selLayer);
    RefreshTrackList(hwnd);
    RefreshLayerList(hwnd);
}

// ---------------------------------------------------------------------------
// Spacers
//
// A spacer is a mark on the channel it sits above — the dash in the Spacer
// column — never a row of its own. Besides the column, Ins adds one above
// each selected channel and the right-click menu adds, removes and removes
// all. On the active layer a spacer edit goes onto the tracks at once; on any
// other layer it reaches them on that layer's next recall.
// ---------------------------------------------------------------------------

// Store a spacer edit. On the active layer it also goes onto the tracks, the
// same as a TCP/MCP dot: that layer is what is on screen, and a spacer that
// does not appear reads as broken. On any other layer it waits for a recall.
static void CommitSpacerEdit(int layerIdx)
{
    LayersEngine& le = LayersEngine::Get();
    le.SaveExtState();
    if (layerIdx >= 0 && layerIdx == le.GetActiveLayer())
        le.ApplySpacersNow(layerIdx);
    else
        le.MarkLayoutEdited(layerIdx);
}

// After a spacer edit: store it and repaint. No row moves, so the selection
// stays where it is.
static void FinishSpacerEdit(HWND hwnd)
{
    CommitSpacerEdit(s_selLayer);
    RefreshTrackList(hwnd);
    RefreshLayerList(hwnd);
}

// Add a spacer above every selected channel the layer holds that does not
// already have one.
static void AddSpacersAboveSelection(HWND hwnd)
{
    const int n = LayersEngine::Get().GetLayerCount();
    if (s_selLayer < 0 || s_selLayer >= n) return;
    LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);
    HWND hList = GetDlgItem(hwnd, IDC_LYR_TRACK_LIST);
    if (!hList) return;

    bool changed = false;
    int i = -1;
    while ((i = ListView_GetNextItem(hList, i, LVNI_SELECTED)) >= 0)
    {
        if (i >= (int)s_trackRows.size()) continue;
        const GUID& g = s_trackRows[i].guid;
        if (FindLayerTrackIdx(ld, g) < 0 || LayerHasSpacerAbove(ld, g)) continue;
        SetLayerSpacerAbove(ld, g, true);
        changed = true;
    }
    if (changed) FinishSpacerEdit(hwnd);
}

static int CountLayerSpacers(const LayerDef& ld)
{
    int c = 0;
    for (const auto& lt : ld.tracks)
        if (lt.isSpacer) c++;
    return c;
}

static void RemoveAllSpacers(HWND hwnd)
{
    const int n = LayersEngine::Get().GetLayerCount();
    if (s_selLayer < 0 || s_selLayer >= n) return;
    LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);
    const size_t before = ld.tracks.size();
    ld.tracks.erase(std::remove_if(ld.tracks.begin(), ld.tracks.end(),
                                   [](const LayerTrack& lt) { return lt.isSpacer; }),
                    ld.tracks.end());
    if (ld.tracks.size() == before) return;
    FinishSpacerEdit(hwnd);
}

// ---------------------------------------------------------------------------
// The Track-Manager-style dot columns
//
// A cell is written to a definite state rather than flipped, because the drag
// (see TrackListSubclassProc) picks one direction from the cell it starts on
// and writes that to every cell it crosses. A flip per cell would undo itself
// the moment the pointer wandered back over a row it had already passed.
//
// A write applies to the cell under the pointer only, never to the whole
// selection: an edit that silently rewrote every selected row would be far
// too easy to fire by accident in the middle of a show.
// ---------------------------------------------------------------------------

// Current state of one dot cell.
static bool DotState(const LayerDef& ld, const GUID& g, int col)
{
    const int idx = FindLayerTrackIdx(ld, g);
    if (idx < 0) return false;
    if (col == LYRCOL_TCP)    return ld.tracks[idx].showTcp;
    if (col == LYRCOL_MCP)    return ld.tracks[idx].showMcp;
    /* LYRCOL_SPACER */       return idx > 0 && ld.tracks[idx - 1].isSpacer;
}

// ---------------------------------------------------------------------------
// SetDotCell - write one dot cell to a definite state.
//
// TCP and MCP are also what membership is, because a row is a project track
// rather than a slot the layer owns: lighting either one on a channel the
// layer does not hold adds it, and clearing a channel's last dot removes it.
// That is not a new rule, only a visible one — recall has always computed
// showTcp as (in layer && showTcp), so a member with no dots and a non-member
// were already the same thing.
//
// Returns true when the slot list changed shape, i.e. a channel joined or
// left the layer, so the caller knows the list needs rebuilding rather than
// just the one row repainting.
// ---------------------------------------------------------------------------
static bool SetDotCell(LayerDef& ld, const LyrRow& row, int col, bool on)
{
    const GUID& g = row.guid;

    if (col == LYRCOL_SPACER)
    {
        // A gap above a channel the layer does not show has nowhere to be.
        if (FindLayerTrackIdx(ld, g) < 0) return false;
        SetLayerSpacerAbove(ld, g, on);
        return true;
    }

    LayerTrack* lt = FindLayerTrack(ld, g);
    if (!lt)
    {
        if (!on) return false;              // already not in the layer
        LayerTrack nt = {};
        nt.guid    = g;
        snprintf(nt.name, sizeof(nt.name), "%s", row.name);
        nt.showTcp = (col == LYRCOL_TCP);
        nt.showMcp = (col == LYRCOL_MCP);
        // A folder joins folded the way the project has it right now.
        nt.folderCompact = row.isFolder ? row.folderCompact : 0;
        ld.tracks.push_back(nt);
        NormalizeLayerOrder(ld);
        return true;
    }

    if (col == LYRCOL_TCP)
    {
        if (lt->showTcp == on) return false;
        lt->showTcp = on;
    }
    else
    {
        if (lt->showMcp == on) return false;
        lt->showMcp = on;
    }

    if (!lt->showTcp && !lt->showMcp)
    {
        // Last dot cleared: the layer shows this channel nowhere, which is the
        // same thing as not holding it. Drop the slot rather than leave a
        // member that does nothing and still counts against the slot limit.
        const int idx = FindLayerTrackIdx(ld, g);
        if (idx >= 0)
        {
            // Take the gap above it too — a spacer left behind would attach
            // itself to whichever channel ends up below it.
            if (idx > 0 && ld.tracks[idx - 1].isSpacer)
                ld.tracks.erase(ld.tracks.begin() + (idx - 1),
                                ld.tracks.begin() + (idx + 1));
            else
                ld.tracks.erase(ld.tracks.begin() + idx);
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// ApplyDotCell - write one cell and repaint its row.
//
// A TCP/MCP dot on the active layer shows or hides the track right away; on
// any other layer the edit is stored and reaches the project on that layer's
// recall (CommitVisibilityEdit). The Spacer cell follows the same rule
// through CommitSpacerEdit.
// `deferRebuild` is for the drag, which rebuilds once at the end instead of
// per cell.
//
// Returns true when the list needs rebuilding.
// ---------------------------------------------------------------------------
static bool ApplyDotCell(HWND hwnd, HWND hList, int row, int col, bool on,
                         bool deferRebuild)
{
    int n = LayersEngine::Get().GetLayerCount();
    if (s_selLayer < 0 || s_selLayer >= n) return false;
    if (!IsTrackRow(row)) return false;
    if (!IsDotCol(col)) return false;

    LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);
    const bool shapeChanged = SetDotCell(ld, s_trackRows[row], col, on);

    if (col == LYRCOL_SPACER)
    {
        CommitSpacerEdit(s_selLayer);
    }
    else
    {
        CommitVisibilityEdit(s_selLayer);
    }

    RECT rcRow;
    if (ListView_GetItemRect(hList, row, &rcRow, LVIR_BOUNDS))
        InvalidateRect(hList, &rcRow, FALSE);

    if (shapeChanged && !deferRebuild)
    {
        RefreshTrackList(hwnd);
        RefreshLayerList(hwnd);
    }
    else if (!deferRebuild)
    {
        RefreshLayerList(hwnd);   // channel count may still have moved
    }
    return shapeChanged;
}

// ---------------------------------------------------------------------------
// OnTrackListClick - a plain click on a dot cell, i.e. a drag of one cell.
// ---------------------------------------------------------------------------
static void OnTrackListClick(HWND hwnd, HWND hList, POINT pt)
{
    int n = LayersEngine::Get().GetLayerCount();
    if (s_selLayer < 0 || s_selLayer >= n) return;

    LVHITTESTINFO ht = {};
    ht.pt = pt;
    ListView_SubItemHitTest(hList, &ht);
    if (!IsTrackRow(ht.iItem)) return;
    if (!IsDotCol(ht.iSubItem)) return;

    const LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);
    const bool now = DotState(ld, s_trackRows[ht.iItem].guid, ht.iSubItem);
    ApplyDotCell(hwnd, hList, ht.iItem, ht.iSubItem, !now, false);
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
    case WM_LBUTTONDBLCLK:
    {
        LVHITTESTINFO hti = {};
        hti.pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ListView_SubItemHitTest(hList, &hti);

        // A double-click on a dot is just a second press. Passing it to the
        // list instead swallowed it, so toggling a cell back and forth at
        // ordinary clicking speed missed every other click.
        if (msg == WM_LBUTTONDBLCLK && !(hti.iItem >= 0 && IsDotCol(hti.iSubItem)))
            return CallWindowProc(s_origTrackListProc, hList, msg, wParam, lParam);

        // A press on a dot starts a drag that writes the same state to every
        // cell it crosses, so a run of channels goes into a layer in one
        // gesture. The direction comes from this first cell: press an empty
        // dot and the drag fills, press a filled one and it clears.
        //
        // The press is swallowed rather than passed on, which is also what
        // stops the list from generating the NM_CLICK the dialog turns into a
        // second toggle. Selection is deliberately left alone: clicking a dot
        // is an edit, not a way of choosing rows, and clobbering a
        // multi-selection to tick one channel would be its own bug.
        if (hti.iItem >= 0 && IsDotCol(hti.iSubItem))
        {
            int n = LayersEngine::Get().GetLayerCount();
            if (s_selLayer < 0 || s_selLayer >= n) return 0;
            if (!IsTrackRow(hti.iItem))  return 0;

            const LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);
            s_dotDragOn      = !DotState(ld, s_trackRows[hti.iItem].guid, hti.iSubItem);
            s_dotDragActive  = true;
            s_dotDragLastRow = hti.iItem;
            s_dotDragLastCol = hti.iSubItem;
            s_dotDragDirty   = ApplyDotCell(dlg, hList, hti.iItem, hti.iSubItem,
                                            s_dotDragOn, true);
            // The press is swallowed, so nothing else will give the list the
            // focus — and without it Del would not reach the list's own
            // handler after a channel had just been ticked in.
            SetFocus(hList);
            SetCapture(hList);
            return 0;
        }

        // Drag-to-reorder is gone from this list. Its rows are the project's
        // tracks in the project's order, so there is no per-layer order left
        // for a drag to express — reordering happens in REAPER and this list
        // follows on the next refresh.
        return CallWindowProc(s_origTrackListProc, hList, msg, wParam, lParam);
    }
    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        if (s_dotDragActive)
        {
            if (!(wParam & MK_LBUTTON))
            {
                // Button released somewhere we never saw it happen.
                ReleaseCapture();
                break;
            }
            LVHITTESTINFO ht = {};
            ht.pt = pt;
            ListView_SubItemHitTest(hList, &ht);
            if (ht.iItem >= 0 && IsDotCol(ht.iSubItem) &&
                (ht.iItem != s_dotDragLastRow || ht.iSubItem != s_dotDragLastCol))
            {
                s_dotDragLastRow = ht.iItem;
                s_dotDragLastCol = ht.iSubItem;
                if (ApplyDotCell(dlg, hList, ht.iItem, ht.iSubItem, s_dotDragOn, true))
                    s_dotDragDirty = true;
            }
            return 0;
        }

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
        if (s_dotDragActive)
        {
            ReleaseCapture();   // WM_CAPTURECHANGED finishes the drag
            return 0;
        }
        if (s_draggingTrack)
        {
            s_draggingTrack = false;
            EndTrackDrag(dlg, true);
        }
        break;
    case WM_CAPTURECHANGED:
        s_trkLbTracking = false;
        if (s_dotDragActive)
        {
            s_dotDragActive  = false;
            s_dotDragLastRow = s_dotDragLastCol = -1;
            // One rebuild for the whole gesture: channels joining or leaving
            // move the bracketed slot-limit marks and the layer list's count,
            // and doing that per cell would fight the drag.
            if (s_dotDragDirty)
            {
                s_dotDragDirty = false;
                RefreshTrackList(dlg);
            }
            RefreshLayerList(dlg);
        }
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
        if (wParam == VK_INSERT)
        {
            AddSpacersAboveSelection(dlg);
            return 0;
        }
        break;
    }
    return CallWindowProc(s_origTrackListProc, hList, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Window layout
//
// Everything is placed from the design-size geometry recorded at
// WM_INITDIALOG: the two lists share the width left after the margins and
// the divider gutter, in the proportion the divider sets, and take all the
// height between the labels and the status line. Each list's Name column
// stretches to fill its list.
// ---------------------------------------------------------------------------
struct LyrLayoutInit {
    bool valid        = false;
    int  margin       = 0;   // left/right edge to the lists
    int  gutter       = 0;   // between the lists; the divider fills it
    int  labelTop     = 0;
    int  labelH       = 0;
    int  listTop      = 0;
    int  bottomGap    = 0;   // list bottom to client bottom
    int  statusH      = 0;
    int  statusGap    = 0;   // status bottom to client bottom
};
static LyrLayoutInit s_lyInit;

// Share of the lists' combined width the layer list takes. Saved globally so
// the window opens split the way it was left.
static double s_splitRatio = -1.0;
static const char* kLyrSplitSec = "reaper_transitions";
static const char* kLyrSplitKey = "lyr_wnd_split";

static bool    s_splitDragging = false;
static WNDPROC s_splitOldProc  = nullptr;

static const int kMinLayerListW = 120;
static const int kMinTrackListW = 200;

static RECT ChildRect(HWND dlg, int id)
{
    RECT r = {};
    if (HWND h = GetDlgItem(dlg, id))
    {
        GetWindowRect(h, &r);
        MapWindowPoints(HWND_DESKTOP, dlg, (POINT*)&r, 2);
    }
    return r;
}

// Stretch one column to whatever the others leave.
static void StretchColumn(HWND hList, int stretchCol, int minW)
{
    if (!hList) return;
    HWND hHdr = ListView_GetHeader(hList);
    const int cols = hHdr ? Header_GetItemCount(hHdr) : 0;
    RECT rc;
    GetClientRect(hList, &rc);
    int others = 0;
    for (int c = 0; c < cols; ++c)
        if (c != stretchCol) others += ListView_GetColumnWidth(hList, c);
    // The client width already excludes a visible scrollbar; leave room for
    // one anyway so a list that grows a scrollbar does not grow a horizontal
    // one too.
    int w = (rc.right - rc.left) - others - GetSystemMetrics(SM_CXVSCROLL);
    if (w < minW) w = minW;
    ListView_SetColumnWidth(hList, stretchCol, w);
}

static void LayoutLayersWnd(HWND hwnd)
{
    if (!s_lyInit.valid) return;
    RECT cr;
    GetClientRect(hwnd, &cr);
    const int W = cr.right, H = cr.bottom;
    if (W <= 0 || H <= 0) return;

    const LyrLayoutInit& g = s_lyInit;
    const int avail = W - 2 * g.margin - g.gutter;   // both lists together
    int leftW = (int)(avail * s_splitRatio + 0.5);
    if (leftW > avail - kMinTrackListW) leftW = avail - kMinTrackListW;
    if (leftW < kMinLayerListW)         leftW = kMinLayerListW;
    const int rightX = g.margin + leftW + g.gutter;
    int rightW = W - g.margin - rightX;
    if (rightW < 1) rightW = 1;

    int listH = H - g.listTop - g.bottomGap;
    if (listH < 40) listH = 40;

    const int statusTop = H - g.statusGap - g.statusH;

    HDWP dw = BeginDeferWindowPos(6);
    auto place = [&](int id, int x, int y, int w, int h) {
        if (HWND c = GetDlgItem(hwnd, id))
            dw = DeferWindowPos(dw, c, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    place(IDC_LYR_LAYER_LBL,  g.margin, g.labelTop, leftW,  g.labelH);
    place(IDC_LYR_TRACK_LBL,  rightX,   g.labelTop, rightW, g.labelH);
    place(IDC_LYR_LAYER_LIST, g.margin, g.listTop,  leftW,  listH);
    place(IDC_LYR_SPLITTER,   g.margin + leftW, g.listTop, g.gutter, listH);
    place(IDC_LYR_TRACK_LIST, rightX,   g.listTop,  rightW, listH);
    place(IDC_LYR_STATUS,     g.margin, statusTop,  W - 2 * g.margin, g.statusH);
    EndDeferWindowPos(dw);

    StretchColumn(GetDlgItem(hwnd, IDC_LYR_LAYER_LIST), 0, 60);            // Name
    StretchColumn(GetDlgItem(hwnd, IDC_LYR_TRACK_LIST), LYRCOL_NAME, 80);  // Name
    InvalidateRect(hwnd, nullptr, TRUE);
}

// Divider drag: the ratio follows the pointer, clamped by the layout.
static LRESULT CALLBACK LyrSplitterProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
        return TRUE;

    case WM_LBUTTONDOWN:
        s_splitDragging = true;
        SetCapture(h);
        return 0;

    case WM_MOUSEMOVE:
        if (s_splitDragging && s_lyInit.valid)
        {
            HWND dlg = GetParent(h);
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(dlg, &pt);
            RECT cr;
            GetClientRect(dlg, &cr);
            const int avail = cr.right - 2 * s_lyInit.margin - s_lyInit.gutter;
            if (avail > 0)
            {
                // The pointer sits on the middle of the gutter.
                const int leftW = pt.x - s_lyInit.margin - s_lyInit.gutter / 2;
                double r = (double)leftW / avail;
                if (r < 0.05) r = 0.05;
                if (r > 0.95) r = 0.95;
                s_splitRatio = r;
                LayoutLayersWnd(dlg);
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (s_splitDragging)
        {
            s_splitDragging = false;
            ReleaseCapture();
            char buf[32];
            snprintf(buf, sizeof(buf), "%.4f", s_splitRatio);
            SetExtState(kLyrSplitSec, kLyrSplitKey, buf, true);
        }
        return 0;

    case WM_CAPTURECHANGED:
        s_splitDragging = false;
        return 0;
    }
    return CallWindowProc(s_splitOldProc, h, msg, wp, lp);
}

// Record the design-size geometry, hook the divider and do the first layout.
// Runs after the placeholders have been replaced by the real lists.
static void InitLayersLayout(HWND hwnd)
{
    RECT cr;
    GetClientRect(hwnd, &cr);
    const RECT ll = ChildRect(hwnd, IDC_LYR_LAYER_LIST);
    const RECT tl = ChildRect(hwnd, IDC_LYR_TRACK_LIST);
    const RECT lb = ChildRect(hwnd, IDC_LYR_LAYER_LBL);
    const RECT st = ChildRect(hwnd, IDC_LYR_STATUS);

    LyrLayoutInit& g = s_lyInit;
    g.margin    = ll.left;
    g.gutter    = tl.left - ll.right;
    if (g.gutter < 4) g.gutter = 4;
    // The headings and the status line read across a stage, so they are a
    // couple of points up from the dialog's 8pt. The template's gaps are
    // recorded above, before the font goes on, and the rows grow from there:
    // the headings push the lists down by what they gained, and the status
    // line grows upward into the sidebar because its bottom is anchored.
    ReaperTheme_ApplyHeadingFont(hwnd, IDC_LYR_LAYER_LBL);
    ReaperTheme_ApplyHeadingFont(hwnd, IDC_LYR_TRACK_LBL);
    ReaperTheme_ApplyHeadingFont(hwnd, IDC_LYR_STATUS);
    const int headH = ReaperTheme_HeadingHeight();

    g.labelTop  = lb.top;
    g.labelH    = (std::max)((int)(lb.bottom - lb.top), headH);
    g.listTop   = ll.top + (g.labelH - (int)(lb.bottom - lb.top));
    g.bottomGap = cr.bottom - ll.bottom;
    g.statusH   = (std::max)((int)(st.bottom - st.top), headH);
    g.statusGap = cr.bottom - st.bottom;
    g.valid     = (ll.right > ll.left && tl.right > tl.left);

    if (s_splitRatio < 0.0)
    {
        const char* v = GetExtState(kLyrSplitSec, kLyrSplitKey);
        const double saved = (v && v[0]) ? atof(v) : 0.0;
        const int lw = ll.right - ll.left, tw = tl.right - tl.left;
        s_splitRatio = (saved > 0.02 && saved < 0.98) ? saved
                     : (lw + tw > 0 ? (double)lw / (lw + tw) : 0.45);
    }

    if (HWND hSplit = GetDlgItem(hwnd, IDC_LYR_SPLITTER))
        s_splitOldProc = (WNDPROC)SetWindowLongPtr(hSplit, GWLP_WNDPROC, (LONG_PTR)LyrSplitterProc);

    LayoutLayersWnd(hwnd);
}

// ---------------------------------------------------------------------------
// Main dialog proc
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK LayersDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Dialog colours and controls from the REAPER theme, as in the Scenes
    // window and its other popups (see ReaperTheme.h).
    if (INT_PTR r = ReaperTheme_CtlColor(hwnd, msg, wParam, lParam)) return r;
    if (msg == WM_INITDIALOG) ReaperTheme_ApplyDialog(hwnd);

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
                    LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

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
                ReaperTheme_ApplyListView(hList);
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
                    LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

                // Laid out like REAPER's Track Manager: the project track
                // number on the track's color, the channel (indented by
                // folder), then a dot per panel, then the spacer marker.
                LVCOLUMNA col = {};
                col.mask = LVCF_TEXT | LVCF_WIDTH;
                col.cx = 44;   // color box + number
                col.pszText = const_cast<char*>("#");
                ListView_InsertColumn(hList, LYRCOL_NUM, &col);
                // Name takes the width the Spacer column would have had
                // while that column is hidden (kShowSpacerColumn).
                col.cx = kShowSpacerColumn ? 156 : 204;
                col.pszText = const_cast<char*>("Name");
                ListView_InsertColumn(hList, LYRCOL_NAME, &col);
                col.mask |= LVCF_FMT;
                col.fmt = LVCFMT_CENTER;
                col.cx = 42; col.pszText = const_cast<char*>("TCP");
                ListView_InsertColumn(hList, LYRCOL_TCP, &col);
                col.cx = 42; col.pszText = const_cast<char*>("MCP");
                ListView_InsertColumn(hList, LYRCOL_MCP, &col);
                if (kShowSpacerColumn)
                {
                    col.cx = 48; col.pszText = const_cast<char*>("Spacer");
                    ListView_InsertColumn(hList, LYRCOL_SPACER, &col);
                }
                s_origTrackListProc = (WNDPROC)(LONG_PTR)SetWindowLongPtr(
                    hList, GWLP_WNDPROC, (LONG_PTR)TrackListSubclassProc);
                ReaperTheme_ApplyListView(hList);
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

        InitLayersLayout(hwnd);
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
            CTX_TRK_SPACER_ADD = 3021,
            CTX_TRK_SPACER_ALL = 3022,
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
                LayersEngine::Get().CaptureVisibleInto(idx);
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
                LayersEngine::Get().CaptureVisibleInto(item);
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
                CommitVisibilityEdit(item);
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
            const bool haveLayer = s_selLayer >= 0 && s_selLayer < n;

            // Spacer entries. A gap can only sit above a channel the layer
            // actually shows; Add acts on every selected channel that can
            // take one, the same as the Ins key.
            bool canAddSpacer = false, rowHasSpacer = false;
            int  spacerCount  = 0;
            if (haveLayer)
            {
                const LayerDef& cl = LayersEngine::Get().GetLayer(s_selLayer);
                spacerCount = CountLayerSpacers(cl);
                int si = -1;
                while ((si = ListView_GetNextItem(hTrackList, si, LVNI_SELECTED)) >= 0)
                {
                    if (si >= (int)s_trackRows.size()) continue;
                    const GUID& g = s_trackRows[si].guid;
                    if (FindLayerTrackIdx(cl, g) >= 0 && !LayerHasSpacerAbove(cl, g))
                        canAddSpacer = true;
                }
                if (item >= 0 && item < (int)s_trackRows.size())
                    rowHasSpacer = LayerHasSpacerAbove(cl, s_trackRows[item].guid);
            }

            HMENU hMenu = CreatePopupMenu();
            AppendMenuA(hMenu, MF_STRING | (s_selLayer < 0 ? MF_GRAYED : 0),
                CTX_TRK_ADD_SEL, "Add Selected Tracks");
            AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(hMenu, MF_STRING | (!canAddSpacer ? MF_GRAYED : 0),
                CTX_TRK_SPACER_ADD, "Add Spacer Above\tIns");
            AppendMenuA(hMenu, MF_STRING | (!rowHasSpacer ? MF_GRAYED : 0),
                CTX_TRK_SPACER_BEF, "Remove Spacer Above");
            AppendMenuA(hMenu, MF_STRING | (spacerCount == 0 ? MF_GRAYED : 0),
                CTX_TRK_SPACER_ALL, "Remove All Spacers");
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
            case CTX_TRK_SPACER_ADD:
                AddSpacersAboveSelection(hwnd);
                break;
            case CTX_TRK_SPACER_BEF:
                // Remove the spacer above the right-clicked row. Live on the
                // active layer, otherwise stored for the layer's next recall.
                if (item >= 0 && item < (int)s_trackRows.size())
                {
                    const GUID g = s_trackRows[item].guid;   // rows are rebuilt below
                    if (LayerHasSpacerAbove(ld, g))
                    {
                        SetLayerSpacerAbove(ld, g, false);
                        FinishSpacerEdit(hwnd);
                    }
                }
                break;
            case CTX_TRK_SPACER_ALL:
                RemoveAllSpacers(hwnd);
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
                        CommitVisibilityEdit(s_selLayer);
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
                    CommitVisibilityEdit(s_selLayer);
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
                LayersEngine::Get().CaptureVisibleInto(s_selLayer);
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
                CommitVisibilityEdit(s_selLayer);
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
                    res = CDRF_NOTIFYITEMDRAW;
                    break;
                case CDDS_ITEMPREPAINT:
                    // Row colours, selection included, from the REAPER theme.
                    ReaperTheme_ListItemPrePaint(cd, hdr->hwndFrom, false);
                    if (s_boldFont &&
                        (int)cd->nmcd.dwItemSpec == LayersEngine::Get().GetActiveLayer())
                        SelectObject(cd->nmcd.hdc, s_boldFont);
                    res = CDRF_NEWFONT;
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
                    // Theme colours for anything left to default drawing, and
                    // the selection cleared so Windows adds no highlight.
                    ReaperTheme_ListItemPrePaint(cd, hdr->hwndFrom, false);
                    res = CDRF_NOTIFYSUBITEMDRAW;
                    break;

                case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
                {
                    const int col = (int)cd->iSubItem;
                    const int row = (int)cd->nmcd.dwItemSpec;

                    if ((col == LYRCOL_NUM || col == LYRCOL_NAME) &&
                        row >= 0 && row < (int)s_trackRows.size())
                    {
                        const LyrRow& lr = s_trackRows[row];
                        HDC  hdc  = cd->nmcd.hdc;
                        HWND hLst = hdr->hwndFrom;
                        const COLORREF bg = RowBg(hLst, lr, row);
                        const COLORREF fg = ReaperTheme_ListCellFg(hLst, row);

                        // Column 0's custom-draw rect spans the whole row on
                        // some comctl versions, so both cells are measured.
                        RECT rc;
                        if (col == LYRCOL_NUM)
                        {
                            if (!ListView_GetItemRect(hLst, row, &rc, LVIR_BOUNDS)) break;
                            rc.right = rc.left + ListView_GetColumnWidth(hLst, LYRCOL_NUM);
                        }
                        else if (!ListView_GetSubItemRect(hLst, row, LYRCOL_NAME, LVIR_BOUNDS, &rc))
                            break;

                        char text[200] = {};
                        ListView_GetItemText(hLst, row, col, text, sizeof(text));
                        SetBkMode(hdc, TRANSPARENT);

                        if (col == LYRCOL_NUM)
                        {
                            FillSolid(hdc, &rc, bg);

                            // A color box on every track row, then the track
                            // number. The box is drawn whether or not the
                            // track has a color of its own — an uncolored
                            // track gets an empty outline — so the column
                            // reads the same all the way down.
                            RECT rn = rc;
                            {
                                const int bs = kColorBoxSize;
                                const int by = (rc.top + rc.bottom - bs) / 2;
                                RECT rb = { rc.left + 4, by, rc.left + 4 + bs, by + bs };
                                HPEN    hp = CreatePen(PS_SOLID, 1, BlendColor(bg, fg, 60));
                                HBRUSH  hb = lr.hasColor ? CreateSolidBrush(lr.color) : nullptr;
                                HGDIOBJ op = SelectObject(hdc, hp);
                                HGDIOBJ ob = SelectObject(hdc,
                                    hb ? (HGDIOBJ)hb : GetStockObject(NULL_BRUSH));
                                Rectangle(hdc, rb.left, rb.top, rb.right, rb.bottom);
                                SelectObject(hdc, op);
                                SelectObject(hdc, ob);
                                DeleteObject(hp);
                                if (hb) DeleteObject(hb);
                                rn.left = rb.right + 2;
                            }
                            SetTextColor(hdc, fg);
                            DrawTextA(hdc, text, -1, &rn,
                                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                        }
                        else
                        {
                            FillSolid(hdc, &rc, bg);

                            // Indent, then U+2514 U+2500 (box-drawing
                            // corner) in front of a folder's children: the
                            // same mark the Scenes list puts in front of a
                            // subscene, and the Safes window in front of a
                            // child track.
                            RECT rt = rc;
                            rt.left += 4;
                            SetTextColor(hdc, fg);
                            if (lr.depth > 0)
                            {
                                rt.left += (lr.depth - 1) * kFolderIndent;
                                RECT rm = rt;
#ifdef _WIN32
                                DrawTextW(hdc, L"\u2514\u2500 ", -1, &rm,
                                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
                                DrawTextW(hdc, L"\u2514\u2500 ", -1, &rt,
                                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
#else
                                DrawTextA(hdc, "\xE2\x94\x94\xE2\x94\x80 ", -1, &rm,
                                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
                                DrawTextA(hdc, "\xE2\x94\x94\xE2\x94\x80 ", -1, &rt,
                                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
#endif
                                rt.left += rm.right - rm.left;
                            }
                            DrawTextA(hdc, text, -1, &rt,
                                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                                      DT_END_ELLIPSIS);
                        }
                        res = CDRF_SKIPDEFAULT;
                        break;
                    }

                    if (!IsDotCol(col))
                        break;
                    int n = LayersEngine::Get().GetLayerCount();
                    if (s_selLayer < 0 || s_selLayer >= n) break;
                    const LayerDef& ld = LayersEngine::Get().GetLayer(s_selLayer);
                    if (row < 0 || row >= (int)s_trackRows.size()) break;

                    // A row is a project track; the layer's entry for it is
                    // what the dots read, and its absence is what an empty
                    // cell means.
                    const LyrRow& lr      = s_trackRows[row];
                    const GUID&   rowGuid = lr.guid;
                    const int     ltIdx   = FindLayerTrackIdx(ld, rowGuid);

                    HDC  hdc  = cd->nmcd.hdc;
                    RECT rc   = cd->nmcd.rc;
                    HWND hLst = hdr->hwndFrom;
                    FillSolid(hdc, &rc, RowBg(hLst, lr, row));

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
                        const COLORREF fg = ReaperTheme_ListCellFg(hLst, row);
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
                            const int r = 2;
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
        LayoutLayersWnd(hwnd);
        break;

    case WM_GETMINMAXINFO:
        if (s_lyInit.valid)
        {
            // Room for both lists at their minimum widths and a few rows.
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            RECT r = { 0, 0,
                       2 * s_lyInit.margin + s_lyInit.gutter + kMinLayerListW + kMinTrackListW,
                       s_lyInit.listTop + 120 + s_lyInit.bottomGap };
            AdjustWindowRectEx(&r, (DWORD)GetWindowLong(hwnd, GWL_STYLE), FALSE,
                               (DWORD)GetWindowLong(hwnd, GWL_EXSTYLE));
            mmi->ptMinTrackSize.x = r.right  - r.left;
            mmi->ptMinTrackSize.y = r.bottom - r.top;
        }
        return 0;

    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
        if (dis && dis->CtlID == IDC_LYR_SPLITTER)
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

    // -----------------------------------------------------------------------
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return TRUE;

    case WM_DESTROY:
        if (s_boldFont) { DeleteObject(s_boldFont); s_boldFont = nullptr; }
        s_lyInit.valid = false;
        s_hwnd = nullptr;
        return TRUE;
    }

    return FALSE;
}
