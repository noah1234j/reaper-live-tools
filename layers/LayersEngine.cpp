// ---------------------------------------------------------------------------
// LayersEngine.cpp  –  Channel-strip layer management engine
//
// Manages up to 10 named "layers", each containing an ordered list of tracks.
// Activating a layer sets MCP (and optionally TCP) visibility so only those
// tracks appear in the mixer, up to a configurable max-channel count.
// ---------------------------------------------------------------------------
#include "LayersEngine.h"
#include "LayersWnd.h"
#include "api.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>

static const char* k_Sec = "reaper_transitions";

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

    const char* ht = GetExtState(k_Sec, "lyr_hidetcp");
    hideTcpToo          = (ht[0] == '1');

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
void LayersEngine::DoApplyLayer(int idx)
{
    if (idx < 0 || idx >= (int)m_layers.size()) return;
    m_suppressCooldown = 10;  // ~300ms: prevent sync-back while REAPER processes the changes
    const LayerDef&       layer = m_layers[idx];
    const LayersSettings& cfg   = m_settings;

    // Snapshot the current track selection so we can restore it unchanged.
    // Internal calls to SetOnlyTrackSelected (reorder + spacer actions) must
    // not affect what the user had selected.
    std::vector<MediaTrack*> savedSelection;
    {
        int n = CountTracks(0);
        for (int t = 0; t < n; t++)
        {
            MediaTrack* tr = GetTrack(0, t);
            if (!tr) continue;
            int* ps = (int*)GetSetMediaTrackInfo(tr, "I_SELECTED", nullptr);
            if (ps && *ps) savedSelection.push_back(tr);
        }
    }

    PreventUIRefresh(1);

    // Determine slot limit (spacers count as slots)
    int limit = (int)layer.tracks.size();
    if (cfg.globalMaxChannels > 0 && cfg.globalMaxChannels < limit)
        limit = cfg.globalMaxChannels;

    if (cfg.applyMcpVisibility)
    {
        int numTracks = CountTracks(0);

        for (int t = 0; t < numTracks; t++)
        {
            MediaTrack* track = GetTrack(0, t);
            if (!track) continue;

            GUID* tg = GetTrackGUID(track);
            if (!tg) continue;

            // Check membership in active range
            bool inLayer = false;
            for (int li = 0; li < limit; li++)
            {
                if (layer.tracks[li].isSpacer) continue;  // spacer has no GUID
                if (memcmp(tg, &layer.tracks[li].guid, sizeof(GUID)) == 0)
                {
                    inLayer = true;
                    break;
                }
            }

            bool showMixer = inLayer;
            GetSetMediaTrackInfo(track, "B_SHOWINMIXER", &showMixer);

            if (cfg.hideTcpToo)
            {
                bool showTcp = inLayer;
                GetSetMediaTrackInfo(track, "B_SHOWINTCP", &showTcp);
            }

            // Restore folder open/closed state for tracks in the layer
            if (inLayer)
            {
                for (int li = 0; li < limit; li++)
                {
                    if (layer.tracks[li].isSpacer) continue;
                    if (memcmp(tg, &layer.tracks[li].guid, sizeof(GUID)) == 0)
                    {
                        int fc = layer.tracks[li].folderCompact;
                        GetSetMediaTrackInfo(track, "I_FOLDERCOMPACT", &fc);
                        break;
                    }
                }
            }
        }

        // Optional: reorder tracks to match layer ordering
        if (cfg.reorderTracks && limit > 0)
        {
            for (int li = 0; li < limit; li++)
            {
                if (layer.tracks[li].isSpacer) continue;  // spacers have no GUID, skip
                int curPos = -1;
                int now = CountTracks(0);
                for (int t = 0; t < now; t++)
                {
                    MediaTrack* tr = GetTrack(0, t);
                    GUID* tg = GetTrackGUID(tr);
                    if (tg && memcmp(tg, &layer.tracks[li].guid, sizeof(GUID)) == 0)
                    {
                        curPos = t;
                        break;
                    }
                }
                if (curPos < 0 || curPos == li) continue;
                MediaTrack* tr = GetTrack(0, curPos);
                SetOnlyTrackSelected(tr);
                ReorderSelectedTracks(li, 0);
            }
        }
    }

    // End the UI-refresh suppression before firing REAPER actions.
    // Main_OnCommand needs UI refresh active to correctly write I_SPACER.
    PreventUIRefresh(-1);
    TrackList_AdjustWindows(false);

    // ---- Set REAPER visual spacers via built-in actions --------------------
    // Action 42665 = "Track: Insert visual spacer before tracks"
    // Must run AFTER PreventUIRefresh(-1) — REAPER actions need UI refresh
    // active to correctly detect selection and write I_SPACER values.
    {
        int numAllTracks = CountTracks(0);

        // Clear all existing spacers directly.
        int zeroVal = 0;
        for (int t = 0; t < numAllTracks; t++)
        {
            MediaTrack* tr = GetTrack(0, t);
            if (!tr) continue;
            int* sp = (int*)GetSetMediaTrackInfo(tr, "I_SPACER", nullptr);
            if (sp && *sp > 0)
                GetSetMediaTrackInfo(tr, "I_SPACER", &zeroVal);
        }

        // For each real track that immediately follows a spacer entry in the
        // layer, select it and fire the "insert spacer before" action (42665).
        for (int li = 0; li < limit; li++)
        {
            if (layer.tracks[li].isSpacer) continue;

            bool hasPrecedingSpacers = false;
            for (int k = li - 1; k >= 0; k--)
            {
                if (layer.tracks[k].isSpacer) { hasPrecedingSpacers = true; break; }
                else break;
            }
            if (!hasPrecedingSpacers) continue;

            for (int t = 0; t < numAllTracks; t++)
            {
                MediaTrack* tr = GetTrack(0, t);
                if (!tr) continue;
                GUID* tg = GetTrackGUID(tr);
                if (tg && memcmp(tg, &layer.tracks[li].guid, sizeof(GUID)) == 0)
                {
                    SetOnlyTrackSelected(tr);
                    Main_OnCommand(42665, 0);  // Insert visual spacer before tracks
                    break;
                }
            }
        }

        // Restore selection — undo any SetOnlyTrackSelected calls made above.
        {
            int n = CountTracks(0);
            for (int t = 0; t < n; t++)
            {
                MediaTrack* tr = GetTrack(0, t);
                if (!tr) continue;
                bool wasSel = std::find(savedSelection.begin(), savedSelection.end(), tr)
                              != savedSelection.end();
                int sel = wasSel ? 1 : 0;
                GetSetMediaTrackInfo(tr, "I_SELECTED", &sel);
            }
        }
    }

    UpdateArrange();
}

void LayersEngine::ActivateLayer(int idx)
{
    int n = (int)m_layers.size();
    if (idx < 0 || idx >= n) return;
    m_activeLayer = idx;
    DoApplyLayer(idx);
    MarkProjectDirty(nullptr);  // active layer saved per-project via SaveConfig
}

void LayersEngine::Deactivate()
{
    m_activeLayer = -1;
    if (m_settings.restoreOnDeactivate)
        RestoreAllVisible();
    else
    {
        // Always clear spacers even if track visibility is not restored
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
        bool show = true;
        GetSetMediaTrackInfo(track, "B_SHOWINMIXER", &show);
        if (m_settings.hideTcpToo)
            GetSetMediaTrackInfo(track, "B_SHOWINTCP", &show);
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
    m_settings = s;
    m_settings.Save();
    // Re-apply if active
    if (m_activeLayer >= 0)
        DoApplyLayer(m_activeLayer);
}

// ---------------------------------------------------------------------------
// ReapplyActive / PhysicallyReorderLayer
// ---------------------------------------------------------------------------
void LayersEngine::ReapplyActive()
{
    if (m_activeLayer >= 0 && m_activeLayer < (int)m_layers.size())
        DoApplyLayer(m_activeLayer);
}

void LayersEngine::PhysicallyReorderLayer(int idx)
{
    if (idx < 0 || idx >= (int)m_layers.size()) return;
    const LayerDef& layer = m_layers[idx];

    int limit = (int)layer.tracks.size();
    if (m_settings.globalMaxChannels > 0 && m_settings.globalMaxChannels < limit)
        limit = m_settings.globalMaxChannels;

    if (limit <= 0) return;

    // Save current selection
    std::vector<MediaTrack*> prevSel;
    {
        int nSel = CountSelectedTracks(0);
        for (int i = 0; i < nSel; i++)
            prevSel.push_back(GetSelectedTrack(0, i));
    }

    for (int li = 0; li < limit; li++)
    {
        if (layer.tracks[li].isSpacer) continue;
        int now = CountTracks(0);
        int curPos = -1;
        for (int t = 0; t < now; t++)
        {
            MediaTrack* tr = GetTrack(0, t);
            GUID* tg = GetTrackGUID(tr);
            if (tg && memcmp(tg, &layer.tracks[li].guid, sizeof(GUID)) == 0)
            {
                curPos = t;
                break;
            }
        }
        if (curPos < 0 || curPos == li) continue;
        MediaTrack* tr = GetTrack(0, curPos);
        SetOnlyTrackSelected(tr);
        ReorderSelectedTracks(li, 0);
    }

    // Restore selection
    int cur = CountTracks(0);
    for (int t = 0; t < cur; t++)
    {
        MediaTrack* tr = GetTrack(0, t);
        if (!tr) continue;
        bool was = std::find(prevSel.begin(), prevSel.end(), tr) != prevSel.end();
        SetMediaTrackInfo_Value(tr, "I_SELECTED", was ? 1.0 : 0.0);
    }
    TrackList_AdjustWindows(false);
    UpdateArrange();
    m_suppressCooldown = 10;
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
    for (int i = 0; i < (int)m_layers.size(); i++)
        RefreshTrackNames(i);
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

void LayersEngine::ReplaceAllLayers(const std::vector<LayerDef>& newLayers, int activeIdx)
{
    // Unregister all existing layer actions
    for (auto& ld : m_layers)
        if (ld.uid > 0)
            UnregisterLayerAction(ld.uid);
    m_layers.clear();
    m_activeLayer = -1;

    // Add new layers (assigns UIDs, registers actions)
    for (const LayerDef& src : newLayers)
    {
        LayerDef ld;
        ld.uid = m_nextUid++;
        strncpy(ld.name, src.name, sizeof(ld.name) - 1);
        ld.name[sizeof(ld.name) - 1] = '\0';
        ld.maxChannels = src.maxChannels;
        ld.tracks      = src.tracks;
        m_layers.push_back(ld);
        RegisterLayerAction((int)m_layers.size() - 1);
    }

    // Persist once (avoids per-layer saves)
    SaveExtState();

    // Activate the requested layer (also calls DoApplyLayer)
    if (activeIdx >= 0 && activeIdx < (int)m_layers.size())
        ActivateLayer(activeIdx);

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
//   LAYER uid=N maxch=N name="..." tracks={guid}|SPACER|{guid2}
//   ...
//   >
void LayersEngine::SaveConfig(ProjectStateContext* ctx)
{
    ctx->AddLine("<LTLAYERS nextuid=%d active=%d mcpvis=%d hidetcp=%d reorder=%d restore=%d globalmaxch=%d trigmcpsel=%d",
                 m_nextUid, m_activeLayer,
                 m_settings.applyMcpVisibility  ? 1 : 0,
                 m_settings.hideTcpToo          ? 1 : 0,
                 m_settings.reorderTracks       ? 1 : 0,
                 m_settings.restoreOnDeactivate ? 1 : 0,
                 m_settings.globalMaxChannels,
                 m_settings.triggerMcpSelect    ? 1 : 0);

    for (const auto& ld : m_layers)
    {
        // Build pipe-separated track list; each entry is GUID:fc=N or SPACER
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

    int nextuid = 1, active = -1, mcpvis = 1, hidetcp = 0, reorder = 0, restore = 1, globalmaxch = 0, trigmcpsel = 0;
    sscanf(line, "<LTLAYERS nextuid=%d active=%d mcpvis=%d hidetcp=%d reorder=%d restore=%d globalmaxch=%d trigmcpsel=%d",
           &nextuid, &active, &mcpvis, &hidetcp, &reorder, &restore, &globalmaxch, &trigmcpsel);

    m_nextUid = (nextuid >= 1) ? nextuid : 1;
    m_settings.applyMcpVisibility  = (mcpvis  != 0);
    m_settings.hideTcpToo          = (hidetcp != 0);
    m_settings.reorderTracks       = (reorder != 0);
    m_settings.restoreOnDeactivate = (restore != 0);
    m_settings.globalMaxChannels   = (globalmaxch >= 0) ? globalmaxch : 0;
    m_settings.triggerMcpSelect    = (trigmcpsel != 0);
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
                        // Token format: GUID or GUID:fc=N
                        char guidBuf[40] = {};
                        int  fc = 0;
                        const char* colon = strchr(p, ':');
                        if (colon)
                        {
                            size_t glen = (size_t)(colon - p);
                            if (glen < sizeof(guidBuf))
                            {
                                memcpy(guidBuf, p, glen);
                                guidBuf[glen] = '\0';
                            }
                            sscanf(colon, ":fc=%d", &fc);
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
