// ---------------------------------------------------------------------------
// LayersEngine.cpp  –  Channel-strip layer management engine
//
// Manages up to 10 named "layers", each containing an ordered list of tracks.
// Activating a layer sets both MCP and TCP visibility so only those tracks
// appear, up to a configurable max-channel count. Each track carries its own
// showTcp/showMcp, so one layer can hand the mixer and the track panel
// different channel sets.
// ---------------------------------------------------------------------------
#include "LayersEngine.h"
#include "LayersWnd.h"
#include "api.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <unordered_map>

static const char* k_Sec = "reaper_transitions";

// ---------------------------------------------------------------------------
// GUID hash/equality for O(1) unordered_map lookups
// ---------------------------------------------------------------------------
struct GUIDHash {
    size_t operator()(const GUID& g) const noexcept {
        size_t h1, h2;
        memcpy(&h1, &g,         8);
        memcpy(&h2, reinterpret_cast<const char*>(&g) + 8, 8);
        return h1 ^ (h2 * 2654435761ULL);
    }
};
struct GUIDEqual {
    bool operator()(const GUID& a, const GUID& b) const noexcept {
        return memcmp(&a, &b, sizeof(GUID)) == 0;
    }
};
// GUID → MediaTrack* — built once per operation and reused across loops
using GUIDTrackMap = std::unordered_map<GUID, MediaTrack*, GUIDHash, GUIDEqual>;

// ---------------------------------------------------------------------------
// LayerDef
// ---------------------------------------------------------------------------
LayerDef::LayerDef()
    : maxChannels(0), uid(0)
{
    name[0] = '\0';
}

// ---------------------------------------------------------------------------
// LayersSettings
// ---------------------------------------------------------------------------
void LayersSettings::Load()
{
    const char* mv = GetExtState(k_Sec, "lyr_mcpvis");
    applyMcpVisibility  = (mv[0] == '\0') ? true  : (mv[0] != '0');

    const char* ro = GetExtState(k_Sec, "lyr_reorder");
    reorderTracks       = (ro[0] == '1');

    const char* rd = GetExtState(k_Sec, "lyr_restore");
    restoreOnDeactivate = (rd[0] == '\0') ? true : (rd[0] != '0');

    const char* gm = GetExtState(k_Sec, "lyr_globalMaxCh");
    globalMaxChannels   = (gm && gm[0]) ? atoi(gm) : 0;
    if (globalMaxChannels < 0) globalMaxChannels = 0;
}

void LayersSettings::Save() const
{
    // Settings are project-specific; persisted via SaveExtensionConfig / SaveConfig.
    MarkProjectDirty(nullptr);
}

// ---------------------------------------------------------------------------
// LayersEngine
// ---------------------------------------------------------------------------
LayersEngine::LayersEngine() {}

LayersEngine& LayersEngine::Get()
{
    static LayersEngine s;
    return s;
}

void LayersEngine::Init()
{
    // Do NOT call LoadExtState() here – layer data is project-specific and
    // will be loaded via BeginLoadProjectState → ProcessLine().  Actions
    // are registered below; the initial project state arrives through the
    // project_config_extension_t callbacks shortly after plugin registration.
    RegisterAllActions();
}

void LayersEngine::Cleanup()
{
    UnregisterAllActions();
}

// ---------------------------------------------------------------------------
// GUID helpers
// ---------------------------------------------------------------------------
void LayersEngine::GuidToStr(const GUID& g, char out[40])
{
    snprintf(out, 40,
        "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        (unsigned)g.Data1, (unsigned)g.Data2, (unsigned)g.Data3,
        (unsigned)g.Data4[0], (unsigned)g.Data4[1],
        (unsigned)g.Data4[2], (unsigned)g.Data4[3],
        (unsigned)g.Data4[4], (unsigned)g.Data4[5],
        (unsigned)g.Data4[6], (unsigned)g.Data4[7]);
}

bool LayersEngine::StrToGuid(const char* s, GUID& out)
{
    if (!s || strlen(s) < 36) return false;
    const char* p = (*s == '{') ? s + 1 : s;
    unsigned d1, d2, d3, b0, b1, b2, b3, b4, b5, b6, b7;
    int r = sscanf(p, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                   &d1, &d2, &d3, &b0, &b1, &b2, &b3, &b4, &b5, &b6, &b7);
    if (r != 11) return false;
    out.Data1    = (DWORD)d1;
    out.Data2    = (WORD)d2;
    out.Data3    = (WORD)d3;
    out.Data4[0] = (BYTE)b0; out.Data4[1] = (BYTE)b1;
    out.Data4[2] = (BYTE)b2; out.Data4[3] = (BYTE)b3;
    out.Data4[4] = (BYTE)b4; out.Data4[5] = (BYTE)b5;
    out.Data4[6] = (BYTE)b6; out.Data4[7] = (BYTE)b7;
    return true;
}

// ---------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Folder safety for track reordering
//
// REAPER stores no per-track parent: folder membership is purely positional.
// A track belongs to the innermost folder whose parent sits above it and whose
// matching negative I_FOLDERDEPTH sits below it. Moving a track to an absolute
// index therefore silently adopts it into — or evicts it from — whatever
// folder happens to span the destination. That is how layer reordering was
// moving tracks between folders: nothing in the move said "change the folder",
// the new position simply meant a different one.
//
// Reordering must never restructure the project, so tracks are permuted only
// among slots that already belong to the same folder parent, and a track that
// opens or closes a folder is never relocated at all. Folder membership is
// then a property of how the moves are chosen rather than a check that can be
// skipped.
// ---------------------------------------------------------------------------

// Innermost folder parent for every project index, derived by walking the
// running I_FOLDERDEPTH total once.
static void LayersBuildFolderParentMap(std::vector<MediaTrack*>& parentAt)
{
    const int n = CountTracks(0);
    parentAt.assign(n, nullptr);

    std::vector<MediaTrack*> open;
    for (int t = 0; t < n; t++)
    {
        MediaTrack* tr = GetTrack(0, t);
        if (!tr) continue;
        parentAt[t] = open.empty() ? nullptr : open.back();

        int fd = 0;
        int* p = (int*)GetSetMediaTrackInfo(tr, "I_FOLDERDEPTH", nullptr);
        if (p) fd = *p;
        if (fd >= 1)
            open.push_back(tr);
        else if (fd < 0)
            for (int k = 0; k < -fd && !open.empty(); k++) open.pop_back();
    }
}

// 0-based project position of a track right now.
static int LayersLiveTrackIndex(MediaTrack* tr)
{
    if (!tr) return -1;
    return (int)(intptr_t)GetSetMediaTrackInfo(tr, "IP_TRACKNUMBER", nullptr) - 1;
}

// ---------------------------------------------------------------------------
// ApplyLayerTrackOrder – put the layer's tracks into the layer's order
//
// A layer records the order of its own channels relative to each other, not
// where they sit in the project: the slot index of a channel in the Layers
// window is a row number in that window, and the window lists only the
// layer's own tracks plus its spacer rows. Recall used to pass that row number
// straight to ReorderSelectedTracks as an absolute project index, which is why
// recalling a layer hauled its tracks to the top of the project and shoved
// everything else down — the whole project got rearranged to reproduce an
// order the user had only ever expressed among a handful of channels. Spacer
// rows made it worse, since they occupy a row but no track, so every slot
// after one was off by a further place.
//
// The layer's tracks are instead permuted among the project slots they already
// occupy. Only channels that are genuinely out of order relative to each other
// move; every track the layer does not hold keeps its position, so recalling a
// layer whose order already matches the project performs no moves at all.
// ---------------------------------------------------------------------------
static void ApplyLayerTrackOrder(const LayerDef& layer, int limit,
                                 const GUIDTrackMap& projByGUID)
{
    std::vector<MediaTrack*> parentAt;
    LayersBuildFolderParentMap(parentAt);
    const int nTracks = (int)parentAt.size();

    // Candidates, in the layer's own order: tracks the layer holds that are in
    // the project and carry no folder structure of their own. A non-zero
    // I_FOLDERDEPTH opens or closes a folder, and moving one without its
    // siblings rewrites the tree, so those never move.
    struct Cand { MediaTrack* tr; MediaTrack* parent; };
    std::vector<Cand> cands;
    cands.reserve(limit);
    for (int li = 0; li < limit; li++)
    {
        if (layer.tracks[li].isSpacer) continue;   // a row, not a track
        auto it = projByGUID.find(layer.tracks[li].guid);
        if (it == projByGUID.end() || !it->second) continue;
        MediaTrack* tr = it->second;

        const int cur = LayersLiveTrackIndex(tr);
        if (cur < 0 || cur >= nTracks) continue;

        int fd = 0;
        int* pfd = (int*)GetSetMediaTrackInfo(tr, "I_FOLDERDEPTH", nullptr);
        if (pfd) fd = *pfd;
        if (fd != 0) continue;

        cands.push_back({ tr, parentAt[cur] });
    }
    if (cands.size() < 2) return;

    // Group by folder parent. Permuting within a group can only ever put a
    // track at an index another member of the same group occupies, so the
    // track keeps its parent by construction.
    std::vector<MediaTrack*> parents;
    for (const auto& c : cands)
        if (std::find(parents.begin(), parents.end(), c.parent) == parents.end())
            parents.push_back(c.parent);

    for (MediaTrack* parent : parents)
    {
        std::vector<MediaTrack*> group;   // already in the layer's order
        for (const auto& c : cands)
            if (c.parent == parent) group.push_back(c.tr);
        if (group.size() < 2) continue;

        for (size_t k = 0; k < group.size(); ++k)
        {
            // The slots this group occupies right now, ascending. Read fresh
            // every step: a move renumbers everything between source and
            // destination, and acting on stale numbers is exactly what put
            // tracks in the wrong places before.
            std::vector<int> slots;
            slots.reserve(group.size());
            for (MediaTrack* tr : group)
            {
                const int idx = LayersLiveTrackIndex(tr);
                if (idx >= 0) slots.push_back(idx);
            }
            if (slots.size() != group.size()) break;   // list changed underneath
            std::sort(slots.begin(), slots.end());

            const int dest = slots[k];
            const int cur  = LayersLiveTrackIndex(group[k]);
            if (cur < 0 || cur == dest) continue;      // already right: no move

            // Insert-before semantics: a track moving down vacates a slot above
            // the destination first, so aim one past it.
            const int beforeIdx = (cur < dest) ? dest + 1 : dest;

            SetOnlyTrackSelected(group[k]);
            ReorderSelectedTracks(beforeIdx, 0);
        }
    }
}

void LayersEngine::DoApplyLayer(int idx)
{
    if (idx < 0 || idx >= (int)m_layers.size()) return;
    m_suppressCooldown = 10;  // ~300ms: prevent sync-back while REAPER processes the changes
    const LayerDef&       layer = m_layers[idx];
    const LayersSettings& cfg   = m_settings;

    // Determine slot limit (spacers count as slots)
    int limit = (int)layer.tracks.size();
    if (cfg.globalMaxChannels > 0 && cfg.globalMaxChannels < limit)
        limit = cfg.globalMaxChannels;

    // -----------------------------------------------------------------------
    // Build O(1) lookup maps up front — one pass over project tracks,
    // one pass over layer tracks.  All inner-loop linear scans below are
    // replaced with map lookups.
    // -----------------------------------------------------------------------

    // project GUID → MediaTrack* (one GetTrack + GetTrackGUID per project track)
    GUIDTrackMap projByGUID;
    {
        int n = CountTracks(0);
        projByGUID.reserve(n);
        for (int t = 0; t < n; t++)
        {
            MediaTrack* tr = GetTrack(0, t);
            if (!tr) continue;
            GUID* tg = GetTrackGUID(tr);
            if (tg) projByGUID[*tg] = tr;
        }
    }

    // layer GUID → slot index (for O(1) membership test and folderCompact lookup)
    std::unordered_map<GUID, int, GUIDHash, GUIDEqual> layerSlotMap;
    layerSlotMap.reserve(limit);
    for (int li = 0; li < limit; li++)
    {
        if (!layer.tracks[li].isSpacer)
            layerSlotMap[layer.tracks[li].guid] = li;
    }

    // -----------------------------------------------------------------------
    // Save current track selection so spacer/reorder actions don't corrupt it
    // -----------------------------------------------------------------------
    std::unordered_map<GUID, bool, GUIDHash, GUIDEqual> savedSelGUIDs;
    {
        int n = CountTracks(0);
        for (int t = 0; t < n; t++)
        {
            MediaTrack* tr = GetTrack(0, t);
            if (!tr) continue;
            int* ps = (int*)GetSetMediaTrackInfo(tr, "I_SELECTED", nullptr);
            if (ps && *ps)
            {
                GUID* tg = GetTrackGUID(tr);
                if (tg) savedSelGUIDs[*tg] = true;
            }
        }
    }

    PreventUIRefresh(1);

    if (cfg.applyMcpVisibility)
    {
        // ----- MCP/TCP visibility — O(numTracks) with O(1) map lookup -----
        // Both panels are written on every recall. A track the layer does not
        // hold is hidden in both; a track it does hold goes wherever its own
        // showTcp/showMcp say, so one layer can hand the mixer and the track
        // panel different channel sets.
        int numTracks = CountTracks(0);
        for (int t = 0; t < numTracks; t++)
        {
            MediaTrack* track = GetTrack(0, t);
            if (!track) continue;
            GUID* tg = GetTrackGUID(track);
            if (!tg) continue;

            auto it = layerSlotMap.find(*tg);
            const bool inLayer = (it != layerSlotMap.end());

            bool showTcp = inLayer && layer.tracks[it->second].showTcp;
            bool showMcp = inLayer && layer.tracks[it->second].showMcp;
            GetSetMediaTrackInfo(track, "B_SHOWINTCP",   &showTcp);
            GetSetMediaTrackInfo(track, "B_SHOWINMIXER", &showMcp);

            // Restore folder open/closed state — no second scan needed
            if (inLayer)
            {
                int fc = layer.tracks[it->second].folderCompact;
                GetSetMediaTrackInfo(track, "I_FOLDERCOMPACT", &fc);
            }
        }
    }

    // ---- Track reorder ----------------------------------------------------
    // Permutes the layer's own channels among the slots they already hold.
    // Tracks the layer does not list never move.
    //
    // Outside the visibility block on purpose: applyMcpVisibility is the switch
    // for writing the two panels, and a user who has turned it off still asked
    // for their channel order back.
    if (cfg.reorderTracks && limit > 0)
        ApplyLayerTrackOrder(layer, limit, projByGUID);

    // ---- Set REAPER visual spacers ----------------------------------------
    // Skipped entirely when the user has turned spacer management off: there
    // is one I_SPACER flag shared by both panels, so any write here is visible
    // in the panel the layer is not targeting too.
    // Spacers used to be cleared with a direct I_SPACER write but *set* by
    // firing action 42665 with the track selected. That asymmetry is why they
    // went missing: clearing always worked, while setting depended on the
    // action id resolving in the running REAPER and on the selection surviving
    // long enough to be acted on — and when it did not, the layer's spacers
    // silently never came back. Both directions now write I_SPACER, so setting
    // a spacer is exactly as reliable as clearing one, needs no selection, and
    // does not have to happen with UI refresh enabled.
    if (cfg.manageSpacers)
    {
        int numAllTracks = CountTracks(0);

        // Clear spacers before re-applying the layer's own, but only on tracks
        // the layer is actually showing.
        //
        // REAPER exposes a single I_SPACER flag per track and renders it in
        // both panels, so clearing it project-wide took out spacers in the
        // panel the layer is not targeting: hiding a stack of tracks from the
        // TCP wiped their spacers out of the MCP, where those tracks were
        // still on screen. Clearing a hidden track's spacer buys nothing in
        // the target panel — the track is not drawn there at all — so skipping
        // them leaves the target panel identical and stops the collateral
        // damage in the other one. A track's spacer is restored to the layer's
        // idea of it the moment the layer shows it again.
        int zeroVal = 0;
        for (int t = 0; t < numAllTracks; t++)
        {
            MediaTrack* tr = GetTrack(0, t);
            if (!tr) continue;

            // On screen in either panel counts: the spacer is shared between
            // the two, so a track still drawn in one of them is one whose
            // spacer the layer is entitled to rewrite.
            bool visTcp = true, visMcp = true;
            if (bool* pt = (bool*)GetSetMediaTrackInfo(tr, "B_SHOWINTCP",   nullptr)) visTcp = *pt;
            if (bool* pm = (bool*)GetSetMediaTrackInfo(tr, "B_SHOWINMIXER", nullptr)) visMcp = *pm;
            if (!visTcp && !visMcp) continue;

            int* sp = (int*)GetSetMediaTrackInfo(tr, "I_SPACER", nullptr);
            if (sp && *sp > 0)
                GetSetMediaTrackInfo(tr, "I_SPACER", &zeroVal);
        }

        // Then put one above each real track that follows a spacer entry in
        // the layer. Track lookup is O(1) via projByGUID — no inner scan.
        int oneVal = 1;
        for (int li = 1; li < limit; li++)
        {
            if (layer.tracks[li].isSpacer) continue;
            if (!layer.tracks[li - 1].isSpacer) continue;

            auto it = projByGUID.find(layer.tracks[li].guid);
            if (it == projByGUID.end()) continue;
            GetSetMediaTrackInfo(it->second, "I_SPACER", &oneVal);
        }

        // Restore selection using the GUID set built earlier — O(n) with O(1) lookups
        {
            int n = CountTracks(0);
            for (int t = 0; t < n; t++)
            {
                MediaTrack* tr = GetTrack(0, t);
                if (!tr) continue;
                GUID* tg = GetTrackGUID(tr);
                const bool wasSel = tg && savedSelGUIDs.count(*tg) > 0;
                int sel = wasSel ? 1 : 0;
                GetSetMediaTrackInfo(tr, "I_SELECTED", &sel);
            }
        }
    }

    // Nothing above fires a REAPER action any more, so the whole apply can run
    // under one suppression and repaint once at the end.
    PreventUIRefresh(-1);
    TrackList_AdjustWindows(false);
    UpdateArrange();
}

void LayersEngine::ActivateLayer(int idx)
{
    int n = (int)m_layers.size();
    if (idx < 0 || idx >= n) return;
    m_activeLayer = idx;
    // Recalling the layer is what makes a window edit take effect, so the edit
    // stops being pending here.
    m_pendingEditUid = 0;
    DoApplyLayer(idx);
    MarkProjectDirty(nullptr);  // active layer saved per-project via SaveConfig
}

void LayersEngine::Deactivate()
{
    m_activeLayer = -1;
    if (m_settings.restoreOnDeactivate)
        RestoreAllVisible();
    else if (m_settings.manageSpacers)
    {
        // Clear spacers even though track visibility is not being restored —
        // but only if layers are managing spacers at all.
        int numTracks = CountTracks(0);
        int zeroVal = 0;
        for (int t = 0; t < numTracks; t++)
        {
            MediaTrack* track = GetTrack(0, t);
            if (track) GetSetMediaTrackInfo(track, "I_SPACER", &zeroVal);
        }
        TrackList_AdjustWindows(false);
    }
    MarkProjectDirty(nullptr);  // active layer saved per-project via SaveConfig
}

void LayersEngine::NextLayer()
{
    int n = (int)m_layers.size();
    if (n == 0) return;
    int start = (m_activeLayer < 0) ? -1 : m_activeLayer;
    int idx = (start + 1 + n) % n;
    ActivateLayer(idx);
}

void LayersEngine::PrevLayer()
{
    int n = (int)m_layers.size();
    if (n == 0) return;
    int start = (m_activeLayer < 0) ? 0 : m_activeLayer;
    int idx = (start - 1 + n) % n;
    ActivateLayer(idx);
}

void LayersEngine::RestoreAllVisible()
{
    int numTracks = CountTracks(0);
    int zeroVal = 0;
    for (int t = 0; t < numTracks; t++)
    {
        MediaTrack* track = GetTrack(0, t);
        if (!track) continue;
        // Layers write both panels on apply, so deactivating has to give both
        // of them back.
        bool show = true;
        GetSetMediaTrackInfo(track, "B_SHOWINTCP",   &show);
        GetSetMediaTrackInfo(track, "B_SHOWINMIXER", &show);
        // Same shared I_SPACER flag as on apply: leave it alone unless the
        // user has asked layers to manage spacers.
        if (m_settings.manageSpacers)
            GetSetMediaTrackInfo(track, "I_SPACER", &zeroVal);
    }
    TrackList_AdjustWindows(false);
    UpdateArrange();
}

// ---------------------------------------------------------------------------
// Layer management
// ---------------------------------------------------------------------------
void LayersEngine::MoveLayer(int from, int to)
{
    int n = (int)m_layers.size();
    if (from == to) return;
    if (from < 0 || from >= n) return;
    if (to   < 0 || to   >= n) return;

    LayerDef temp = m_layers[from];
    if (from < to)
        for (int i = from; i < to; i++) m_layers[i] = m_layers[i + 1];
    else
        for (int i = from; i > to; i--) m_layers[i] = m_layers[i - 1];
    m_layers[to] = temp;

    if (m_activeLayer == from)
        m_activeLayer = to;
    else if (from < to && m_activeLayer > from && m_activeLayer <= to)
        m_activeLayer--;
    else if (from > to && m_activeLayer >= to && m_activeLayer < from)
        m_activeLayer++;

    SaveExtState();
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
void LayersEngine::SetSettings(const LayersSettings& s)
{
    // There is no "which panel do layers control" setting to change any more,
    // so nothing here has to un-hide a panel layers just stopped driving —
    // both are written on every apply.
    m_settings = s;
    m_settings.Save();
    // Re-apply if active
    if (m_activeLayer >= 0)
        DoApplyLayer(m_activeLayer);
}

// ---------------------------------------------------------------------------
// MarkLayoutEdited
// ---------------------------------------------------------------------------
// Editing a layer in the Layers window used to reach into the project right
// away: dragging a row ran a PhysicallyReorderLayer pass that moved tracks and
// ignored the "reorder tracks" setting while doing it, and adding a spacer row
// re-applied the whole layer. Editing a layer is not a cue. Both now only
// change the stored layer, and the next recall of it is what puts the result
// on the tracks. This records which layer was edited so that the sync pass
// below leaves it alone in the meantime.
void LayersEngine::MarkLayoutEdited(int idx)
{
    if (idx < 0 || idx >= (int)m_layers.size()) return;
    m_pendingEditUid = m_layers[idx].uid;
}

// ---------------------------------------------------------------------------
// SyncLayerOrderFromReaper  –  update the layer's track list to match the
// current REAPER track positions (called from the timer when the project
// state changes while a layer is active).
// ---------------------------------------------------------------------------
void LayersEngine::SyncLayerOrderFromReaper(int idx)
{
    if (idx < 0 || idx >= (int)m_layers.size()) return;
    LayerDef& ld = m_layers[idx];

    // A layer edited in the Layers window is deliberately out of step with the
    // project until it is recalled. Both halves of this pass read the project
    // and write the layer — Part 1 sorts the layer's slots into the project's
    // track order, Part 2 rebuilds its spacer rows from the project's
    // I_SPACER flags — so either one would silently undo a window edit before
    // the user ever got to recall it. Nothing to sync until then.
    if (m_pendingEditUid != 0 && m_pendingEditUid == ld.uid) return;

    int numTracks = CountTracks(0);
    bool changed = false;

    // ---- Part 1: sync track order ------------------------------------------
    // Gather the non-spacer slot indices and their current REAPER positions
    std::vector<int>  nonSpacerSlots;
    for (int li = 0; li < (int)ld.tracks.size(); li++)
        if (!ld.tracks[li].isSpacer) nonSpacerSlots.push_back(li);

    if (nonSpacerSlots.size() >= 2)
    {
        std::vector<std::pair<int, int>> slotAndPos;   // (slotIdx, reaperPos)
        slotAndPos.reserve(nonSpacerSlots.size());
        for (int slot : nonSpacerSlots)
        {
            int rpos = -1;
            for (int t = 0; t < numTracks; t++)
            {
                MediaTrack* tr = GetTrack(0, t);
                if (!tr) continue;
                GUID* tg = GetTrackGUID(tr);
                if (tg && memcmp(tg, &ld.tracks[slot].guid, sizeof(GUID)) == 0)
                { rpos = t; break; }
            }
            slotAndPos.push_back({slot, rpos});
        }

        bool needsSort = false;
        for (int i = 1; i < (int)slotAndPos.size(); i++)
        {
            if (slotAndPos[i - 1].second >= 0 && slotAndPos[i].second >= 0 &&
                slotAndPos[i - 1].second > slotAndPos[i].second)
            { needsSort = true; break; }
        }

        if (needsSort)
        {
            std::stable_sort(slotAndPos.begin(), slotAndPos.end(),
                [](const std::pair<int,int>& a, const std::pair<int,int>& b)
                {
                    if (a.second < 0) return false;
                    if (b.second < 0) return true;
                    return a.second < b.second;
                });

            std::vector<LayerTrack> sorted;
            sorted.reserve(nonSpacerSlots.size());
            for (auto& sp : slotAndPos)
                sorted.push_back(ld.tracks[sp.first]);

            int si = 0;
            for (int li = 0; li < (int)ld.tracks.size(); li++)
                if (!ld.tracks[li].isSpacer)
                    ld.tracks[li] = sorted[si++];
            changed = true;
        }
    }

    // ---- Part 2: sync I_SPACER state from REAPER into layer spacer slots ---
    // Build per-track REAPER position → I_SPACER value map
    std::vector<int> spacerAtPos(numTracks, 0);
    for (int t = 0; t < numTracks; t++)
    {
        MediaTrack* tr = GetTrack(0, t);
        if (!tr) continue;
        int* sp = (int*)GetSetMediaTrackInfo(tr, "I_SPACER", nullptr);
        spacerAtPos[t] = sp ? *sp : 0;
    }

    // Rebuild the track list, deriving spacer entries from REAPER's I_SPACER.
    // A spacer slot is placed before a real track whenever REAPER reports
    // I_SPACER > 0 on that track's position.
    std::vector<LayerTrack> rebuilt;
    rebuilt.reserve(ld.tracks.size());
    for (int li = 0; li < (int)ld.tracks.size(); li++)
    {
        if (ld.tracks[li].isSpacer) continue;  // will be re-derived below

        // Find REAPER position of this real track
        int rpos = -1;
        for (int t = 0; t < numTracks; t++)
        {
            MediaTrack* tr = GetTrack(0, t);
            if (!tr) continue;
            GUID* tg = GetTrackGUID(tr);
            if (tg && memcmp(tg, &ld.tracks[li].guid, sizeof(GUID)) == 0)
            { rpos = t; break; }
        }

        // If REAPER has a spacer on this track's position, inject one before it
        if (rpos >= 0 && spacerAtPos[rpos] > 0)
        {
            LayerTrack sp;
            sp.isSpacer = true;
            strncpy(sp.name, "--- Spacer ---", sizeof(sp.name) - 1);
            rebuilt.push_back(sp);
        }

        rebuilt.push_back(ld.tracks[li]);
    }

    // Compare with current track list (ignoring existing spacers)
    // to see if spacer positions actually changed
    std::vector<LayerTrack> currentNonSpacer;
    for (auto& lt : ld.tracks) if (!lt.isSpacer) currentNonSpacer.push_back(lt);
    std::vector<LayerTrack> rebuiltNonSpacer;
    for (auto& lt : rebuilt)   if (!lt.isSpacer) rebuiltNonSpacer.push_back(lt);

    // Count spacers in each to detect change
    int oldSpacerCount = 0, newSpacerCount = 0;
    for (auto& lt : ld.tracks) if (lt.isSpacer) oldSpacerCount++;
    for (auto& lt : rebuilt)   if (lt.isSpacer) newSpacerCount++;

    if (oldSpacerCount != newSpacerCount || rebuilt.size() != ld.tracks.size())
    {
        ld.tracks = rebuilt;
        changed = true;
    }

    if (changed) SaveExtState();
}

// ---------------------------------------------------------------------------
// TimerCallback  –  registered with plugin_register("timer", ...)
// Polls REAPER's project-state counter and syncs the active layer's track
// ordering when the user reorders tracks in the mixer / TCP.
// ---------------------------------------------------------------------------
void LayersEngine::TimerCallback()
{
    LayersEngine& eng = Get();
    int active = eng.m_activeLayer;
    if (active < 0 || active >= (int)eng.m_layers.size()) return;

    if (eng.m_suppressCooldown > 0)
    {
        --eng.m_suppressCooldown;
        return;
    }

    int stateCount = GetProjectStateChangeCount(nullptr);
    if (stateCount == eng.m_lastStateCount) return;
    eng.m_lastStateCount = stateCount;

    // Sync track order, refresh names (catches renames too), and update window
    eng.SyncLayerOrderFromReaper(active);
    eng.RefreshAllTrackNames();
    LayersWnd_Refresh();
}

// ---------------------------------------------------------------------------
// Track name refresh
// ---------------------------------------------------------------------------
void LayersEngine::RefreshTrackNames(int layerIdx)
{
    if (layerIdx < 0 || layerIdx >= (int)m_layers.size()) return;
    LayerDef& layer = m_layers[layerIdx];
    int numTracks = CountTracks(0);

    for (auto& lt : layer.tracks)
    {
        if (lt.isSpacer)
        {
            strncpy(lt.name, "--- Spacer ---", sizeof(lt.name) - 1);
            continue;
        }
        lt.name[0] = '\0';
        for (int t = 0; t < numTracks; t++)
        {
            MediaTrack* track = GetTrack(0, t);
            if (!track) continue;
            GUID* tg = GetTrackGUID(track);
            if (tg && memcmp(tg, &lt.guid, sizeof(GUID)) == 0)
            {
                char buf[128] = {};
                GetTrackName(track, buf, (int)sizeof(buf));
                strncpy(lt.name, buf, sizeof(lt.name) - 1);
                break;
            }
        }
        if (!lt.name[0])
            strncpy(lt.name, "(not in project)", sizeof(lt.name) - 1);
    }
}

void LayersEngine::RefreshAllTrackNames()
{
    // Build GUID→MediaTrack* map once (O(n)) and reuse it across all layers,
    // replacing the O(layers × layer_tracks × n) nested scan in the original.
    int n = CountTracks(0);
    GUIDTrackMap tmap;
    tmap.reserve(n);
    for (int t = 0; t < n; t++)
    {
        MediaTrack* tr = GetTrack(0, t);
        if (!tr) continue;
        GUID* tg = GetTrackGUID(tr);
        if (tg) tmap[*tg] = tr;
    }

    for (int i = 0; i < (int)m_layers.size(); i++)
    {
        LayerDef& layer = m_layers[i];
        for (auto& lt : layer.tracks)
        {
            if (lt.isSpacer)
            {
                strncpy(lt.name, "--- Spacer ---", sizeof(lt.name) - 1);
                lt.name[sizeof(lt.name) - 1] = '\0';
                continue;
            }
            lt.name[0] = '\0';
            auto it = tmap.find(lt.guid);
            if (it != tmap.end())
            {
                char buf[128] = {};
                GetTrackName(it->second, buf, (int)sizeof(buf));
                strncpy(lt.name, buf, sizeof(lt.name) - 1);
                lt.name[sizeof(lt.name) - 1] = '\0';
            }
            else
            {
                strncpy(lt.name, "(not in project)", sizeof(lt.name) - 1);
                lt.name[sizeof(lt.name) - 1] = '\0';
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
void LayersEngine::SaveExtState()
{
    // Layer data is now project-specific; just mark the project dirty so that
    // REAPER will call SaveExtensionConfig -> SaveConfig() on next save.
    MarkProjectDirty(nullptr);
}

void LayersEngine::LoadExtState()
{
    const char* cv = GetExtState(k_Sec, "lyr_count");
    int count = (cv && cv[0]) ? atoi(cv) : -1;

    const char* nv = GetExtState(k_Sec, "lyr_nextuid");
    m_nextUid = (nv && nv[0]) ? atoi(nv) : 1;
    if (m_nextUid < 1) m_nextUid = 1;

    m_layers.clear();

    if (count < 0)
    {
        // Migrate from old fixed-10 format
        count = 10;
        for (int i = 0; i < count; i++)
        {
            char key[64];
            LayerDef ld;
            snprintf(key, sizeof(key), "lyr_%d_name", i);
            const char* nm = GetExtState(k_Sec, key);
            if (nm && nm[0])
                strncpy(ld.name, nm, sizeof(ld.name) - 1);
            else
                snprintf(ld.name, sizeof(ld.name), "Layer %d", i + 1);

            snprintf(key, sizeof(key), "lyr_%d_maxch", i);
            const char* mc = GetExtState(k_Sec, key);
            ld.maxChannels = (mc && mc[0]) ? atoi(mc) : 0;

            snprintf(key, sizeof(key), "lyr_%d_tracks", i);
            const char* td = GetExtState(k_Sec, key);
            if (td && td[0])
            {
                char buf[8192];
                strncpy(buf, td, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                char* p = buf;
                while (*p)
                {
                    char* sep = strchr(p, '|');
                    if (sep) *sep = '\0';
                    if (strcmp(p, "SPACER") == 0)
                    {
                        LayerTrack lt = {};
                        lt.isSpacer = true;
                        strncpy(lt.name, "--- Spacer ---", sizeof(lt.name) - 1);
                        ld.tracks.push_back(lt);
                    }
                    else
                    {
                        GUID g = {};
                        if (StrToGuid(p, g))
                        {
                            LayerTrack lt = {};
                            lt.guid = g;
                            ld.tracks.push_back(lt);
                        }
                    }
                    if (sep) p = sep + 1; else break;
                }
            }
            ld.uid = m_nextUid++;
            m_layers.push_back(ld);
        }
    }
    else
    {
        for (int i = 0; i < count; i++)
        {
            char key[64];
            LayerDef ld;

            snprintf(key, sizeof(key), "lyr_%d_name", i);
            const char* nm = GetExtState(k_Sec, key);
            if (nm && nm[0])
                strncpy(ld.name, nm, sizeof(ld.name) - 1);
            else
                snprintf(ld.name, sizeof(ld.name), "Layer %d", i + 1);

            snprintf(key, sizeof(key), "lyr_%d_uid", i);
            const char* uv = GetExtState(k_Sec, key);
            ld.uid = (uv && uv[0]) ? atoi(uv) : 0;

            snprintf(key, sizeof(key), "lyr_%d_maxch", i);
            const char* mc = GetExtState(k_Sec, key);
            ld.maxChannels = (mc && mc[0]) ? atoi(mc) : 0;

            snprintf(key, sizeof(key), "lyr_%d_tracks", i);
            const char* td = GetExtState(k_Sec, key);
            if (td && td[0])
            {
                char buf[8192];
                strncpy(buf, td, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                char* p = buf;
                while (*p)
                {
                    char* sep = strchr(p, '|');
                    if (sep) *sep = '\0';
                    if (strcmp(p, "SPACER") == 0)
                    {
                        LayerTrack lt = {};
                        lt.isSpacer = true;
                        strncpy(lt.name, "--- Spacer ---", sizeof(lt.name) - 1);
                        ld.tracks.push_back(lt);
                    }
                    else
                    {
                        GUID g = {};
                        if (StrToGuid(p, g))
                        {
                            LayerTrack lt = {};
                            lt.guid = g;
                            ld.tracks.push_back(lt);
                        }
                    }
                    if (sep) p = sep + 1; else break;
                }
            }

            // Ensure all layers have a uid
            if (ld.uid <= 0)
                ld.uid = m_nextUid++;

            m_layers.push_back(ld);
        }
    }

    // First run: seed 5 default layers
    if (m_layers.empty())
    {
        for (int i = 0; i < 5; i++)
        {
            LayerDef ld;
            ld.uid = m_nextUid++;
            snprintf(ld.name, sizeof(ld.name), "Layer %d", i + 1);
            m_layers.push_back(ld);
        }
    }

    // Guarantee nextUid is above all existing uids
    for (const auto& ld : m_layers)
        if (ld.uid >= m_nextUid) m_nextUid = ld.uid + 1;

    const char* ac = GetExtState(k_Sec, "lyr_active");
    m_activeLayer = (ac && ac[0]) ? atoi(ac) : -1;
    if (m_activeLayer < -1 || m_activeLayer >= (int)m_layers.size())
        m_activeLayer = -1;

    RefreshAllTrackNames();
}

// ---------------------------------------------------------------------------
// Layer management  –  add / remove / spacer
// ---------------------------------------------------------------------------
int LayersEngine::AddLayer(const char* name)
{
    LayerDef ld;
    ld.uid = m_nextUid++;
    if (name && name[0])
        strncpy(ld.name, name, sizeof(ld.name) - 1);
    else
        snprintf(ld.name, sizeof(ld.name), "Layer %d", (int)m_layers.size() + 1);
    m_layers.push_back(ld);
    int idx = (int)m_layers.size() - 1;
    RegisterLayerAction(idx);
    SaveExtState();
    return idx;
}

int LayersEngine::AddSpacerTrack(int layerIdx)
{
    if (layerIdx < 0 || layerIdx >= (int)m_layers.size()) return -1;
    LayerTrack lt = {};
    lt.isSpacer = true;
    strncpy(lt.name, "--- Spacer ---", sizeof(lt.name) - 1);
    m_layers[layerIdx].tracks.push_back(lt);
    SaveExtState();
    return (int)m_layers[layerIdx].tracks.size() - 1;
}

void LayersEngine::RemoveLayer(int idx)
{
    if (idx < 0 || idx >= (int)m_layers.size()) return;

    int uid = m_layers[idx].uid;
    if (uid > 0)
        UnregisterLayerAction(uid);

    m_layers.erase(m_layers.begin() + idx);

    if (m_activeLayer == idx)
        m_activeLayer = -1;
    else if (m_activeLayer > idx)
        m_activeLayer--;

    SaveExtState();
}

int LayersEngine::FindLayerByUid(int uid) const
{
    if (uid <= 0) return -1;
    for (int i = 0; i < (int)m_layers.size(); i++)
        if (m_layers[i].uid == uid) return i;
    return -1;
}

bool LayersEngine::ActivateLayerByUid(int uid)
{
    int idx = FindLayerByUid(uid);
    if (idx < 0) return false;
    ActivateLayer(idx);
    return true;
}

void LayersEngine::ReplaceAllLayers(const std::vector<LayerDef>& newLayers, int activeUid)
{
    // Unregister all existing layer actions
    for (auto& ld : m_layers)
        if (ld.uid > 0)
            UnregisterLayerAction(ld.uid);
    m_layers.clear();
    m_activeLayer = -1;
    m_pendingEditUid = 0;

    // Add new layers, preserving each incoming uid. Re-minting on every recall
    // would orphan the layer's registered action and every scene reference to
    // it, so a uid is only allocated for entries that arrive without one.
    std::vector<int> seenUids;
    for (const LayerDef& src : newLayers)
    {
        LayerDef ld;
        ld.uid = src.uid;
        // A missing or duplicated uid would make FindLayerByUid ambiguous.
        if (ld.uid <= 0 ||
            std::find(seenUids.begin(), seenUids.end(), ld.uid) != seenUids.end())
            ld.uid = m_nextUid++;
        seenUids.push_back(ld.uid);
        if (ld.uid >= m_nextUid) m_nextUid = ld.uid + 1;

        strncpy(ld.name, src.name, sizeof(ld.name) - 1);
        ld.name[sizeof(ld.name) - 1] = '\0';
        ld.maxChannels = src.maxChannels;
        ld.tracks      = src.tracks;
        m_layers.push_back(ld);
        RegisterLayerAction((int)m_layers.size() - 1);
    }

    // Persist once (avoids per-layer saves)
    SaveExtState();

    // Activate the requested layer (also calls DoApplyLayer). A uid that no
    // longer resolves means the scene pointed at a layer that has since been
    // deleted — fall back to the first layer rather than activating nothing.
    // activeUid <= 0 is the deliberate "no layer recall" choice, left alone.
    if (activeUid > 0 && !ActivateLayerByUid(activeUid) && !m_layers.empty())
        ActivateLayer(0);

    // Refresh the layers window to show the new state
    LayersWnd_Refresh();
}

// ---------------------------------------------------------------------------
// Dynamic action dispatch
// ---------------------------------------------------------------------------
bool LayersEngine::HandleLayerCommand(int cmdId)
{
    auto it = m_cmdToUid.find(cmdId);
    if (it == m_cmdToUid.end()) return false;

    int uid = it->second;
    for (int i = 0; i < (int)m_layers.size(); i++)
    {
        if (m_layers[i].uid == uid)
        {
            ActivateLayer(i);
            return true;
        }
    }
    return false;
}

void LayersEngine::UpdateLayerActionDesc(int idx)
{
    if (idx < 0 || idx >= (int)m_layers.size()) return;
    int uid = m_layers[idx].uid;
    if (uid <= 0) return;

    char desc[80];
    snprintf(desc, sizeof(desc), "Live Tools: Layers - Activate \"%s\"", m_layers[idx].name);
    m_cmdDescs[uid] = desc;
    // m_accels[uid].desc already points into m_cmdDescs[uid].c_str(); update it
    if (m_accels.count(uid))
        m_accels[uid].desc = m_cmdDescs[uid].c_str();
}

// ---------------------------------------------------------------------------
// Action registration helpers
// ---------------------------------------------------------------------------
void LayersEngine::RegisterLayerAction(int idx)
{
    if (idx < 0 || idx >= (int)m_layers.size()) return;
    LayerDef& ld = m_layers[idx];
    if (ld.uid <= 0) return;

    int uid = ld.uid;

    char cmdStr[32];
    snprintf(cmdStr, sizeof(cmdStr), "LT_LAYER_UID_%04d", uid);
    char cmdDesc[80];
    snprintf(cmdDesc, sizeof(cmdDesc), "Live Tools: Layers - Activate \"%s\"", ld.name);

    m_cmdStrs[uid]  = cmdStr;
    m_cmdDescs[uid] = cmdDesc;

    int cmdId = plugin_register("command_id", (void*)m_cmdStrs[uid].c_str());
    m_cmdIds[uid]     = cmdId;
    m_cmdToUid[cmdId] = uid;

    gaccel_register_t& ga = m_accels[uid];
    memset(&ga, 0, sizeof(ga));
    ga.desc      = m_cmdDescs[uid].c_str();
    ga.accel.cmd = (WORD)cmdId;
    plugin_register("gaccel", &ga);
}

void LayersEngine::UnregisterLayerAction(int uid)
{
    auto itA = m_accels.find(uid);
    if (itA != m_accels.end())
        plugin_register("-gaccel", &itA->second);

    auto itS = m_cmdStrs.find(uid);
    if (itS != m_cmdStrs.end())
        plugin_register("-command_id", (void*)itS->second.c_str());

    auto itI = m_cmdIds.find(uid);
    if (itI != m_cmdIds.end())
        m_cmdToUid.erase(itI->second);

    m_accels.erase(uid);
    m_cmdStrs.erase(uid);
    m_cmdDescs.erase(uid);
    m_cmdIds.erase(uid);
}

void LayersEngine::RegisterAllActions()
{
    for (int i = 0; i < (int)m_layers.size(); i++)
        RegisterLayerAction(i);
}

void LayersEngine::UnregisterAllActions()
{
    std::vector<int> uids;
    uids.reserve(m_cmdIds.size());
    for (auto& p : m_cmdIds) uids.push_back(p.first);
    for (int uid : uids) UnregisterLayerAction(uid);
}

// ---------------------------------------------------------------------------
// Project-specific persistence (project_config_extension_t hooks)
// ---------------------------------------------------------------------------
void LayersEngine::ResetForProject()
{
    m_settings = LayersSettings{};
    m_layers.clear();
    m_nextUid     = 1;
    m_activeLayer = -1;
    m_pendingEditUid = 0;

    // Seed 5 default empty layers for brand-new projects
    for (int i = 0; i < 5; i++)
    {
        LayerDef ld;
        ld.uid = m_nextUid++;
        snprintf(ld.name, sizeof(ld.name), "Layer %d", i + 1);
        m_layers.push_back(ld);
    }

    LayersWnd_Refresh();
}

// Format:
//   <LTLAYERS nextuid=N active=N mcpvis=1 hidetcp=0 reorder=0 restore=1
//   LAYER uid=N maxch=N name="..." tracks={guid}|SPACER|{guid2}:v=2
//   ...
//   >
void LayersEngine::SaveConfig(ProjectStateContext* ctx)
{
    // hidetcp and targettcp are retired settings, written as a constant 0 so
    // the header keeps its field order. The whole line is read back with one
    // positional sscanf, here and in every already-saved project, so dropping
    // a field mid-line would silently shift everything after it.
    ctx->AddLine("<LTLAYERS nextuid=%d active=%d mcpvis=%d hidetcp=%d reorder=%d restore=%d globalmaxch=%d trigmcpsel=%d targettcp=%d managespacers=%d",
                 m_nextUid, m_activeLayer,
                 m_settings.applyMcpVisibility  ? 1 : 0,
                 0,
                 m_settings.reorderTracks       ? 1 : 0,
                 m_settings.restoreOnDeactivate ? 1 : 0,
                 m_settings.globalMaxChannels,
                 m_settings.triggerMcpSelect    ? 1 : 0,
                 0,
                 m_settings.manageSpacers       ? 1 : 0);

    for (const auto& ld : m_layers)
    {
        // Build pipe-separated track list; each entry is SPACER or a GUID
        // with optional :fc=N (folder compact) and :v=N (panel bitmask)
        std::string trackData;
        trackData.reserve(ld.tracks.size() * 48);
        for (const auto& lt : ld.tracks)
        {
            if (!trackData.empty()) trackData += "|";
            if (lt.isSpacer)
                trackData += "SPACER";
            else
            {
                char gs[40];
                GuidToStr(lt.guid, gs);
                trackData += gs;
                if (lt.folderCompact != 0)
                {
                    char fc[8];
                    snprintf(fc, sizeof(fc), ":fc=%d", lt.folderCompact);
                    trackData += fc;
                }
                // Panel bitmask: 1=TCP, 2=MCP. Omitted for the common "both"
                // case, which is also what a reader that predates the flags
                // assumes, so a project only grows this field where it says
                // something the old format could not.
                const int vis = (lt.showTcp ? 1 : 0) | (lt.showMcp ? 2 : 0);
                if (vis != 3)
                {
                    char vb[8];
                    snprintf(vb, sizeof(vb), ":v=%d", vis);
                    trackData += vb;
                }
            }
        }

        // Escape double-quotes in name
        std::string safeName = ld.name;
        for (char& c : safeName) if (c == '"') c = '\'';

        ctx->AddLine("LAYER uid=%d maxch=%d name=\"%s\" tracks=%s",
                     ld.uid, ld.maxChannels, safeName.c_str(), trackData.c_str());
    }

    ctx->AddLine(">");
}

bool LayersEngine::ProcessLine(const char* line, ProjectStateContext* ctx)
{
    if (!line || strncmp(line, "<LTLAYERS", 9) != 0) return false;

    int nextuid = 1, active = -1, mcpvis = 1, hidetcp = 0, reorder = 0, restore = 1, globalmaxch = 0, trigmcpsel = 0, targettcp = 0;
    int managespacers = 0;   // absent in projects written before the setting existed
    sscanf(line, "<LTLAYERS nextuid=%d active=%d mcpvis=%d hidetcp=%d reorder=%d restore=%d globalmaxch=%d trigmcpsel=%d targettcp=%d managespacers=%d",
           &nextuid, &active, &mcpvis, &hidetcp, &reorder, &restore, &globalmaxch, &trigmcpsel, &targettcp, &managespacers);

    m_nextUid = (nextuid >= 1) ? nextuid : 1;
    m_settings.applyMcpVisibility  = (mcpvis  != 0);
    m_settings.reorderTracks       = (reorder != 0);
    m_settings.restoreOnDeactivate = (restore != 0);
    m_settings.globalMaxChannels   = (globalmaxch >= 0) ? globalmaxch : 0;
    m_settings.triggerMcpSelect    = (trigmcpsel != 0);
    m_settings.manageSpacers       = (managespacers != 0);
    // hidetcp and targettcp are parsed only to keep the positional scan lined
    // up; the settings they fed are gone. A project saved by an older build
    // with targettcp=1 held a layer set that drove the TCP alone, and it comes
    // back driving both panels — every track defaults to showTcp && showMcp.
    (void)hidetcp; (void)targettcp;
    m_layers.clear();

    char subline[4096];
    while (ctx->GetLine(subline, sizeof(subline)) == 0)
    {
        char* trimmed = subline;
        while (*trimmed == ' ' || *trimmed == '\t') ++trimmed;

        if (strcmp(trimmed, ">") == 0) break;

        if (strncmp(trimmed, "LAYER ", 6) == 0)
        {
            LayerDef ld = {};

            // Parse uid and maxch
            int uid = 0, maxch = 0;
            sscanf(trimmed, "LAYER uid=%d maxch=%d", &uid, &maxch);
            ld.uid        = uid;
            ld.maxChannels = maxch;

            // Extract quoted name
            const char* nq = strchr(trimmed, '"');
            if (nq)
            {
                ++nq;
                int ni = 0;
                while (*nq && *nq != '"' && ni < (int)sizeof(ld.name) - 1)
                    ld.name[ni++] = *nq++;
                ld.name[ni] = '\0';
            }
            if (!ld.name[0])
                snprintf(ld.name, sizeof(ld.name), "Layer %d", (int)m_layers.size() + 1);

            // Parse tracks= portion
            const char* tp = strstr(trimmed, "tracks=");
            if (tp)
            {
                tp += 7; // skip "tracks="
                char tdata[8192];
                strncpy(tdata, tp, sizeof(tdata) - 1);
                tdata[sizeof(tdata) - 1] = '\0';

                char* p = tdata;
                while (*p)
                {
                    char* sep = strchr(p, '|');
                    if (sep) *sep = '\0';

                    if (strcmp(p, "SPACER") == 0)
                    {
                        LayerTrack lt = {};
                        lt.isSpacer = true;
                        strncpy(lt.name, "--- Spacer ---", sizeof(lt.name) - 1);
                        ld.tracks.push_back(lt);
                    }
                    else if (p[0])
                    {
                        // Token format: GUID followed by any number of
                        // ":key=value" attributes — fc (folder compact) and v
                        // (panel bitmask, 1=TCP 2=MCP). Both are optional and
                        // order does not matter, so a project written by an
                        // older build, which only ever emitted ":fc=", reads
                        // back with v defaulting to 3 (both panels).
                        char guidBuf[40] = {};
                        int  fc  = 0;
                        int  vis = 3;
                        const char* colon = strchr(p, ':');
                        if (colon)
                        {
                            size_t glen = (size_t)(colon - p);
                            if (glen < sizeof(guidBuf))
                            {
                                memcpy(guidBuf, p, glen);
                                guidBuf[glen] = '\0';
                            }
                            for (const char* a = colon; a; a = strchr(a + 1, ':'))
                            {
                                int v = 0;
                                if      (sscanf(a, ":fc=%d", &v) == 1) fc  = v;
                                else if (sscanf(a, ":v=%d",  &v) == 1) vis = v;
                            }
                        }
                        else
                        {
                            strncpy(guidBuf, p, sizeof(guidBuf) - 1);
                        }
                        GUID g = {};
                        if (StrToGuid(guidBuf, g))
                        {
                            LayerTrack lt = {};
                            lt.guid = g;
                            lt.folderCompact = fc;
                            lt.showTcp = (vis & 1) != 0;
                            lt.showMcp = (vis & 2) != 0;
                            ld.tracks.push_back(lt);
                        }
                    }

                    if (sep) p = sep + 1; else break;
                }
            }

            if (ld.uid <= 0)        ld.uid = m_nextUid++;
            if (ld.uid >= m_nextUid) m_nextUid = ld.uid + 1;
            m_layers.push_back(ld);
        }
    }

    // If block was empty (or all layers stripped) seed defaults
    if (m_layers.empty())
    {
        for (int i = 0; i < 5; i++)
        {
            LayerDef ld;
            ld.uid = m_nextUid++;
            snprintf(ld.name, sizeof(ld.name), "Layer %d", i + 1);
            m_layers.push_back(ld);
        }
    }

    m_activeLayer = active;
    if (m_activeLayer < -1 || m_activeLayer >= (int)m_layers.size())
        m_activeLayer = -1;

    RefreshAllTrackNames();
    LayersWnd_Refresh();
    return true;
}

// ---------------------------------------------------------------------------
// Module init / cleanup
// ---------------------------------------------------------------------------
void LayersEngine_Init()
{
    LayersEngine::Get().Init();
}

void LayersEngine_Cleanup()
{
    LayersEngine::Get().Cleanup();
}
