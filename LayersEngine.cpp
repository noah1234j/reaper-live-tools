// ---------------------------------------------------------------------------
// LayersEngine.cpp  –  Channel-strip layer management engine
//
// Manages up to 10 named "layers", each containing an ordered list of tracks.
// Activating a layer sets MCP (and optionally TCP) visibility so only those
// tracks appear in the mixer, up to a configurable max-channel count.
// ---------------------------------------------------------------------------
#include "LayersEngine.h"
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
}

void LayersSettings::Save() const
{
    SetExtState(k_Sec, "lyr_mcpvis",  applyMcpVisibility  ? "1" : "0", true);
    SetExtState(k_Sec, "lyr_hidetcp", hideTcpToo          ? "1" : "0", true);
    SetExtState(k_Sec, "lyr_reorder", reorderTracks       ? "1" : "0", true);
    SetExtState(k_Sec, "lyr_restore", restoreOnDeactivate ? "1" : "0", true);
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
    m_settings.Load();
    LoadExtState();
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
    const LayerDef&       layer = m_layers[idx];
    const LayersSettings& cfg   = m_settings;

    if (!cfg.applyMcpVisibility) return;

    int numTracks = CountTracks(0);

    // Determine slot limit (spacers count as slots)
    int limit = (int)layer.tracks.size();
    if (layer.maxChannels > 0 && layer.maxChannels < limit)
        limit = layer.maxChannels;

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
    }

    // Optional: reorder tracks to match layer ordering
    if (cfg.reorderTracks && limit > 0)
    {
        for (int li = 0; li < limit; li++)
        {
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

    // ---- Set REAPER visual spacers -----------------------------------------
    // Drive spacers through REAPER's own built-in actions so the MCP redraws
    // reliably. Command 42670 removes all track spacers; command 42665 inserts
    // one spacer-unit above the currently-selected track.
    {
        auto findTrack = [&](const GUID& guid) -> MediaTrack*
        {
            int n = CountTracks(0);
            for (int t = 0; t < n; t++)
            {
                MediaTrack* tr = GetTrack(0, t);
                if (!tr) continue;
                GUID* tg = GetTrackGUID(tr);
                if (tg && memcmp(tg, &guid, sizeof(GUID)) == 0)
                    return tr;
            }
            return nullptr;
        };

        // Save current selection so we can restore it afterwards.
        std::vector<MediaTrack*> prevSel;
        {
            int nSel = CountSelectedTracks(0);
            for (int i = 0; i < nSel; i++)
                prevSel.push_back(GetSelectedTrack(0, i));
        }

        // Select all tracks, then remove every spacer in the project.
        int cur = CountTracks(0);
        for (int t = 0; t < cur; t++)
        {
            MediaTrack* tr = GetTrack(0, t);
            if (tr) SetMediaTrackInfo_Value(tr, "I_SELECTED", 1.0);
        }
        Main_OnCommand(42670, 0);  // Track: Remove track spacers

        // For each real track in the layer, count spacer entries that
        // immediately precede it and insert that many spacer-units above it.
        for (int li = 0; li < limit; li++)
        {
            if (layer.tracks[li].isSpacer) continue;

            int spacerCount = 0;
            for (int k = li - 1; k >= 0; k--)
            {
                if (layer.tracks[k].isSpacer) spacerCount++;
                else break;
            }
            if (spacerCount == 0) continue;

            MediaTrack* tr = findTrack(layer.tracks[li].guid);
            if (!tr) continue;

            SetOnlyTrackSelected(tr);
            for (int s = 0; s < spacerCount; s++)
                Main_OnCommand(42665, 0);  // Track: Insert visual spacer before tracks
        }

        // Restore original selection.
        for (int t = 0; t < cur; t++)
        {
            MediaTrack* tr = GetTrack(0, t);
            if (!tr) continue;
            bool was = std::find(prevSel.begin(), prevSel.end(), tr) != prevSel.end();
            SetMediaTrackInfo_Value(tr, "I_SELECTED", was ? 1.0 : 0.0);
        }
    }

    TrackList_AdjustWindows(false);
    UpdateArrange();
}

void LayersEngine::ActivateLayer(int idx)
{
    int n = (int)m_layers.size();
    if (idx < 0 || idx >= n) return;
    m_activeLayer = idx;
    DoApplyLayer(idx);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", m_activeLayer);
    SetExtState(k_Sec, "lyr_active", buf, true);
}

void LayersEngine::Deactivate()
{
    m_activeLayer = -1;
    if (m_settings.restoreOnDeactivate)
        RestoreAllVisible();
    SetExtState(k_Sec, "lyr_active", "-1", true);
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
    for (int t = 0; t < numTracks; t++)
    {
        MediaTrack* track = GetTrack(0, t);
        if (!track) continue;
        bool show = true;
        GetSetMediaTrackInfo(track, "B_SHOWINMIXER", &show);
        if (m_settings.hideTcpToo)
            GetSetMediaTrackInfo(track, "B_SHOWINTCP", &show);
        SetMediaTrackInfo_Value(track, "I_SELECTED", 1.0);
    }
    Main_OnCommand(42670, 0);  // Track: Remove track spacers
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
    m_settings.Save();

    int count = (int)m_layers.size();
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", count);
    SetExtState(k_Sec, "lyr_count", buf, true);

    snprintf(buf, sizeof(buf), "%d", m_nextUid);
    SetExtState(k_Sec, "lyr_nextuid", buf, true);

    for (int i = 0; i < count; i++)
    {
        char key[64];
        const LayerDef& ld = m_layers[i];

        snprintf(key, sizeof(key), "lyr_%d_name", i);
        SetExtState(k_Sec, key, ld.name, true);

        char uidBuf[16];
        snprintf(uidBuf, sizeof(uidBuf), "%d", ld.uid);
        snprintf(key, sizeof(key), "lyr_%d_uid", i);
        SetExtState(k_Sec, key, uidBuf, true);

        char numBuf[16];
        snprintf(numBuf, sizeof(numBuf), "%d", ld.maxChannels);
        snprintf(key, sizeof(key), "lyr_%d_maxch", i);
        SetExtState(k_Sec, key, numBuf, true);

        std::string trackData;
        trackData.reserve(ld.tracks.size() * 42);
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
            }
        }
        snprintf(key, sizeof(key), "lyr_%d_tracks", i);
        SetExtState(k_Sec, key, trackData.c_str(), true);
    }

    snprintf(buf, sizeof(buf), "%d", m_activeLayer);
    SetExtState(k_Sec, "lyr_active", buf, true);
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
