#include "TransitionEngine.h"
#include "ChunkRecallList.h"
#include "api.h"

#include <cmath>

extern bool g_durationDebug;  // defined in scenes/TransitionWnd.cpp
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cassert>
#include <string>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Tiny QPC timing helper used by Recall() instrumentation
// ---------------------------------------------------------------------------
static inline double QpcMs()
{
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
}

// ---------------------------------------------------------------------------
// Helper: returns true if moving 'tr' to 0-based position targetIdx would
// take it outside its parent folder, which we must not allow.
// ---------------------------------------------------------------------------
static bool WouldLeaveFolder(MediaTrack* tr, int targetIdx)
{
    if (!tr) return false;
    // P_PARTRACK returns the parent folder track, or null if at root
    MediaTrack* parent = (MediaTrack*)GetSetMediaTrackInfo(tr, "P_PARTRACK", nullptr);
    if (!parent) return false;  // track is at root – no folder constraint

    // Find parent's 0-based index
    int numTracks = GetNumTracks();
    int parentIdx = -1;
    for (int t = 0; t < numTracks; t++)
    {
        if (GetTrack(nullptr, t) == parent) { parentIdx = t; break; }
    }
    if (parentIdx < 0) return false;

    // Walk forward from parentIdx+1 to find the last track inside this folder.
    // I_FOLDERDEPTH: +1 = folder start (the parent itself), -1 = decrements depth,
    // 0 = normal. We track net depth; when it goes below 1 the folder has ended.
    int depth = 1;
    int folderLastIdx = parentIdx;
    for (int t = parentIdx + 1; t < numTracks; t++)
    {
        int* pfd = (int*)GetSetMediaTrackInfo(GetTrack(nullptr, t), "I_FOLDERDEPTH", nullptr);
        if (pfd)
        {
            if (*pfd > 0) depth += *pfd;  // nested sub-folder start
            else if (*pfd < 0) depth += *pfd;  // folder end
        }
        if (depth <= 0) break;
        folderLastIdx = t;
    }

    // targetIdx must stay within [parentIdx+1, folderLastIdx]
    return (targetIdx < parentIdx + 1 || targetIdx > folderLastIdx);
}

// ---------------------------------------------------------------------------
// Safes globals
// ---------------------------------------------------------------------------
int  g_globalSafeMask     = 0;
bool g_trackSafesEnabled = true;
std::vector<TrackSafeEntry> g_trackSafes;

// Shared UI preferences (defined non-static in TransitionWnd.cpp)
extern bool g_preloadOffline;
extern bool g_skipUnchangedParams;

int GetEffectiveSafeMask(const GUID& guid)
{
    int safe = g_globalSafeMask;
    if (g_trackSafesEnabled)
    {
        for (const auto& e : g_trackSafes)
            if (IsEqualGUID(e.guid, guid)) { safe |= e.mask; break; }
    }
    return safe;
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
TransitionEngine& TransitionEngine::Get()
{
    static TransitionEngine s_inst;
    return s_inst;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
static inline double lerp(double a, double b, double t) { return a + (b - a) * t; }

// Apply a taper curve to normalised time t∈[0,1]
static double ApplyTaper(double t, TaperLaw law, double customExp)
{
    t = clamp01(t);
    switch (law)
    {
    case TAPER_LINEAR:  return t;
    case TAPER_SCURVE:  return t * t * (3.0 - 2.0 * t);       // smoothstep
    case TAPER_LOG:     return log1p(t * 1.7182818284590452);  // fast start, slow end
    case TAPER_EXP:     return (exp(t * 2.3978952727983707) - 1.0) / 10.0; // slow start, fast end
    case TAPER_CUSTOM:  return pow(t, customExp > 0.0 ? customExp : 1.0);
    default:            return t * t * (3.0 - 2.0 * t);
    }
}

// ---------------------------------------------------------------------------
// StopAndReset – safely abort an in-progress transition (main thread only)
// ---------------------------------------------------------------------------
void TransitionEngine::StopAndReset()
{
    if (m_active)
        plugin_register("-timer", (void*)&TransitionEngine::TimerCallback);
    m_active = false;
    m_volPanLerps.clear();
    m_paramLerps.clear();
    m_wetLerps.clear();
    m_sendLerps.clear();
}

// ---------------------------------------------------------------------------
// BuildTrackMap – build GUID→MediaTrack* map once per recall (O(n log n))
// ---------------------------------------------------------------------------
TransitionEngine::TrackMap TransitionEngine::BuildTrackMap()
{
    TrackMap m;
    const int n = GetNumTracks();
    for (int i = 0; i < n; i++)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;
        GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
        if (pg) m.emplace(*pg, tr);
    }
    return m;
}

// ---------------------------------------------------------------------------
// FindTrack – resolve GUID to live MediaTrack* (main thread only)
// ---------------------------------------------------------------------------
MediaTrack* TransitionEngine::FindTrack(const GUID& guid)
{
    const int n = GetNumTracks();
    for (int i = 0; i < n; i++)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;
        GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
        if (pg && IsEqualGUID(*pg, guid))
            return tr;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// FindFX – locate an FX slot on a track.
// Priority:
//   1. fxIdent match (precise plugin identity, ignores display name variants)
//   2. hint slot name+paramCount match (fast path for unchanged chains)
//   3. O(N) name+paramCount scan (fallback for old snapshots without fxIdent)
// ---------------------------------------------------------------------------
int TransitionEngine::FindFX(MediaTrack* tr,
                              const char* name, int paramCount, int hintSlot,
                              const char* fxIdent)
{
    const int nfx = TrackFX_GetCount(tr);
    if (nfx <= 0) return -1;

    // Ident-based scan (most precise — handles multiple versions of same plugin)
    if (fxIdent && fxIdent[0])
    {
        for (int fx = 0; fx < nfx; fx++)
        {
            char ident[512] = {};
            TrackFX_GetNamedConfigParm(tr, fx, "fx_ident", ident, (int)sizeof(ident));
            if (strcmp(ident, fxIdent) == 0)
                return fx;
        }
        // fxIdent present but not found — plugin not on track
        return -1;
    }

    // Fast path: hint slot still matches (name+paramCount)
    if (hintSlot >= 0 && hintSlot < nfx)
    {
        char hname[256] = {};
        TrackFX_GetFXName(tr, hintSlot, hname, (int)sizeof(hname));
        if (strcmp(hname, name) == 0 && TrackFX_GetNumParams(tr, hintSlot) == paramCount)
            return hintSlot;
    }

    // Slow path: scan all FX by name+paramCount
    for (int fx = 0; fx < nfx; fx++)
    {
        char fname[256] = {};
        TrackFX_GetFXName(tr, fx, fname, (int)sizeof(fname));
        if (strcmp(fname, name) == 0 && TrackFX_GetNumParams(tr, fx) == paramCount)
            return fx;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// EnforceFXOrder – reorder the live chain to match snapshot slot order.
//
// After add/remove operations, surviving and newly-added plugins may be
// in the wrong order. This restores the captured order using
// TrackFX_CopyToTrack with move=true (in-place swap within the same track).
//
// Parked-offline plugins (primed for other scenes) are skipped during the
// search pass and naturally end up at the tail of the chain.
// ---------------------------------------------------------------------------
static void EnforceFXOrder(MediaTrack* tr,
                           const std::vector<FXState>& targetFX)
{
    const int nSnap = (int)targetFX.size();
    for (int i = 0; i < nSnap; ++i)
    {
        const FXState& want = targetFX[i];
        // Check if it's already in the right slot
        {
            char cur[512] = {};
            if (want.fxIdent[0])
                TrackFX_GetNamedConfigParm(tr, i, "fx_ident", cur, (int)sizeof(cur));
            else
                TrackFX_GetFXName(tr, i, cur, (int)sizeof(cur));
            const bool matches = want.fxIdent[0]
                ? strcmp(cur, want.fxIdent) == 0
                : (strcmp(cur, want.name) == 0 &&
                   TrackFX_GetNumParams(tr, i) == want.paramCount);
            if (matches) continue;
        }
        // Search from i+1 onward, skipping parked-offline slots
        const int nFX = TrackFX_GetCount(tr);
        for (int j = i + 1; j < nFX; ++j)
        {
            if (TrackFX_GetOffline(tr, j)) continue; // parked for another scene
            char cand[512] = {};
            bool hit = false;
            if (want.fxIdent[0])
            {
                TrackFX_GetNamedConfigParm(tr, j, "fx_ident", cand, (int)sizeof(cand));
                hit = strcmp(cand, want.fxIdent) == 0;
            }
            else
            {
                TrackFX_GetFXName(tr, j, cand, (int)sizeof(cand));
                hit = strcmp(cand, want.name) == 0 &&
                      TrackFX_GetNumParams(tr, j) == want.paramCount;
            }
            if (hit)
            {
                TrackFX_CopyToTrack(tr, j, tr, i, true /*move*/);
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// SyncFXChain – align the track's FX chain to match the snapshot.
//
// timed=false (instant): add/remove/enable immediately.
// timed=true:  newly-added plugins start at wet=0 (fade in);
//              plugins being removed fade out then delete;
//              plugins going enabled→disabled fade out then disable.
//
// Uses TrackFX_AddByName/Delete (surgical ops, safe during recording).
// ---------------------------------------------------------------------------
void TransitionEngine::SyncFXChain(MediaTrack* tr, const TrackState& ts,
                                    bool timed, std::vector<WetLerp>& wetLerps,
                                    std::vector<RecallTimings::FXOpTiming>* out_fxOps)
{
    // --- Opt C: build O(1) lookup caches from a single pass over live chain ---
    struct LiveFXInfo {
        std::string ident;
        std::string name;
        int         paramCount;
    };
    const int nLive = TrackFX_GetCount(tr);
    std::vector<LiveFXInfo> liveCache(nLive);
    std::unordered_map<std::string, int> liveSlotByIdent;
    std::unordered_map<std::string, int> liveSlotByNameCount;
    for (int i = 0; i < nLive; ++i)
    {
        char ident[512] = {}, name[256] = {};
        TrackFX_GetNamedConfigParm(tr, i, "fx_ident", ident, (int)sizeof(ident));
        TrackFX_GetFXName(tr, i, name, (int)sizeof(name));
        int pc = TrackFX_GetNumParams(tr, i);
        liveCache[i] = { ident, name, pc };
        if (ident[0]) liveSlotByIdent[ident] = i;
        char key[768]; snprintf(key, sizeof(key), "%s\x01%d", name, pc);
        liveSlotByNameCount[key] = i;
    }
    // Snapshot reverse maps (no API calls needed)
    std::unordered_map<std::string, const FXState*> snapByIdent;
    std::unordered_map<std::string, const FXState*> snapByNameCount;
    for (const auto& fxs : ts.fx)
    {
        if (fxs.fxIdent[0]) snapByIdent[fxs.fxIdent] = &fxs;
        char key[768]; snprintf(key, sizeof(key), "%s\x01%d", fxs.name, fxs.paramCount);
        snapByNameCount[key] = &fxs;
    }

    // Helper: does this live FX slot appear in the snapshot? (O(1) via maps)
    auto inSnapshot = [&](int fxIdx) -> const FXState* {
        if (fxIdx < 0 || fxIdx >= nLive) return nullptr;
        const LiveFXInfo& li = liveCache[fxIdx];
        if (li.ident[0])
        {
            auto it = snapByIdent.find(li.ident);
            return (it != snapByIdent.end()) ? it->second : nullptr;
        }
        char key[768]; snprintf(key, sizeof(key), "%s\x01%d", li.name.c_str(), li.paramCount);
        auto it = snapByNameCount.find(key);
        return (it != snapByNameCount.end()) ? it->second : nullptr;
    };

    // Helper: find live slot for a snapshot FX entry using the cached maps.
    // Falls back through hint slot, ident map, then name+count map.
    auto findFXCached = [&](const FXState& fxs) -> int {
        if (fxs.fxIdent[0])
        {
            auto it = liveSlotByIdent.find(fxs.fxIdent);
            return (it != liveSlotByIdent.end()) ? it->second : -1;
        }
        // Hint slot fast-path
        if (fxs.slotIndex >= 0 && fxs.slotIndex < nLive)
        {
            const LiveFXInfo& li = liveCache[fxs.slotIndex];
            if (li.name == fxs.name && li.paramCount == fxs.paramCount)
                return fxs.slotIndex;
        }
        char key[768]; snprintf(key, sizeof(key), "%s\x01%d", fxs.name, fxs.paramCount);
        auto it = liveSlotByNameCount.find(key);
        return (it != liveSlotByNameCount.end()) ? it->second : -1;
    };

    if (!timed)
    {
        // ---- Instant path: remove extras, add missing, set params ---------
        // Remove in reverse order so indices remain valid; set offline first
        // to avoid a chain-reconstruction click on each delete.
        for (int i = nLive - 1; i >= 0; --i)
        {
            if (!inSnapshot(i))
            {
                // If already offline, it's a primed plugin for another scene
                // — leave it parked, don't delete.
                if (TrackFX_GetOffline(tr, i))
                    continue;
                // Close floating window before going offline to prevent flash
                if (TrackFX_GetOpen(tr, i))
                    TrackFX_Show(tr, i, 0);
                TrackFX_SetOffline(tr, i, true);
                TrackFX_Delete(tr, i);
            }
        }
        for (const auto& fxs : ts.fx)
        {
            int slot = findFXCached(fxs);
            const bool isNewPlugin = (slot < 0);

            // Per-FX timing (only collected when caller wants it)
            RecallTimings::FXOpTiming opT;
            if (out_fxOps) opT.fxName = fxs.name;

            if (isNewPlugin)
            {
                const char* addName = fxs.fxIdent[0] ? fxs.fxIdent : fxs.name;
                if (out_fxOps)
                {
                    double t0 = QpcMs();
                    slot = TrackFX_AddByName(tr, addName, false, -1000);
                    opT.addByName_ms = QpcMs() - t0;
                    opT.wasNew = true;
                }
                else
                {
                    slot = TrackFX_AddByName(tr, addName, false, -1000);
                }
                if (slot < 0) continue;
                if (g_preloadOffline)
                {
                    // Offline sandwich: briefly take offline so resume() fires
                    // cleanly, then bring back online before writing params.
                    // Always close the FX window first — REAPER may have auto-opened
                    // it when AddByName fired. Don't re-open: this is a new add, not
                    // a user-opened window being preserved.
                    TrackFX_Show(tr, slot, 0);
                    if (out_fxOps)
                    {
                        double t0 = QpcMs();
                        TrackFX_SetOffline(tr, slot, true);
                        TrackFX_SetOffline(tr, slot, false);
                        opT.offlineSandwich_ms = QpcMs() - t0;
                    }
                    else
                    {
                        TrackFX_SetOffline(tr, slot, true);
                        TrackFX_SetOffline(tr, slot, false);
                    }
                }
            }
            else if (TrackFX_GetOffline(tr, slot))
            {
                // Primed plugin — bring online with offline sandwich so params stick
                bool wasOpen = TrackFX_GetOpen(tr, slot);
                if (wasOpen) TrackFX_Show(tr, slot, 0);
                if (out_fxOps)
                {
                    opT.wasPrimed = true;
                    double t0 = QpcMs();
                    TrackFX_SetOffline(tr, slot, true);
                    TrackFX_SetOffline(tr, slot, false);
                    opT.offlineSandwich_ms = QpcMs() - t0;
                }
                else
                {
                    TrackFX_SetOffline(tr, slot, true);
                    TrackFX_SetOffline(tr, slot, false);
                }
                if (wasOpen) TrackFX_Show(tr, slot, 1);
            }
            // Set state on the live plugin (new or existing).
            TrackFX_SetEnabled(tr, slot, fxs.enabled);
            if (!fxs.fxChunk.empty())
            {
                // Chunk recall: SetNamedConfigParm("vst_chunk") MUST be called while the
                // plugin is online. REAPER routes it directly to VST setChunk(), the same
                // path as preset recall — thread-safe without any extra help from us.
                // Do NOT wrap in an offline sandwich: SetOffline(true) causes REAPER to
                // snapshot the plugin's current state; SetOffline(false) restores that
                // snapshot, silently overwriting anything written while offline.
                if (out_fxOps)
                {
                    double t0 = QpcMs();
                    TrackFX_SetNamedConfigParm(tr, slot, "vst_chunk", fxs.fxChunk.c_str());
                    opT.setChunk_ms = QpcMs() - t0;
                }
                else
                {
                    TrackFX_SetNamedConfigParm(tr, slot, "vst_chunk", fxs.fxChunk.c_str());
                }
                int wi = TrackFX_GetParamFromIdent(tr, slot, ":wet");
                if (wi >= 0) TrackFX_SetParamNormalized(tr, slot, wi, fxs.wetVal);
            }
            else
            {
                // Opt B: for existing (not newly added) plugins, skip params that
                // already match the saved value to avoid redundant API calls.
                if (out_fxOps)
                {
                    double t0 = QpcMs();
                    for (int p = 0; p < (int)fxs.normVals.size(); ++p)
                    {
                        if (!isNewPlugin && g_skipUnchangedParams)
                        {
                            double cur = TrackFX_GetParamNormalized(tr, slot, p);
                            if (fabs(cur - fxs.normVals[p]) < 1e-7) continue;
                        }
                        TrackFX_SetParamNormalized(tr, slot, p, fxs.normVals[p]);
                    }
                    opT.paramLoop_ms = QpcMs() - t0;
                }
                else
                {
                    for (int p = 0; p < (int)fxs.normVals.size(); ++p)
                    {
                        if (!isNewPlugin && g_skipUnchangedParams)
                        {
                            double cur = TrackFX_GetParamNormalized(tr, slot, p);
                            if (fabs(cur - fxs.normVals[p]) < 1e-7) continue;
                        }
                        TrackFX_SetParamNormalized(tr, slot, p, fxs.normVals[p]);
                    }
                }
                // REAPER's :wet index is typically above paramCount and therefore not
                // covered by the normVals loop — apply it explicitly.
                int wi = TrackFX_GetParamFromIdent(tr, slot, ":wet");
                if (wi >= 0) TrackFX_SetParamNormalized(tr, slot, wi, fxs.wetVal);
            }
            TrackFX_SetNamedConfigParm(tr, slot, "chain_bypass_delta", "0");

            // Record op timing if any phase was non-trivial
            if (out_fxOps)
            {
                const double opTotal = opT.addByName_ms + opT.offlineSandwich_ms
                                     + opT.setChunk_ms  + opT.paramLoop_ms;
                if (opTotal > 0.01)
                    out_fxOps->push_back(std::move(opT));
            }
        }
        EnforceFXOrder(tr, ts.fx);
        return;
    }

    // ---- Timed path -------------------------------------------------------
    // Step 1: for each FX currently on the track, decide fate
    for (int i = 0; i < nLive; ++i)
    {
        const FXState* target = inSnapshot(i);

        const bool liveOffline = TrackFX_GetOffline(tr, i);

        if (!target)
        {
            // Plugin not in snapshot
            if (liveOffline)
                continue;  // Primed for another scene — leave it parked offline
            // Plugin is online and not needed → fade out then delete
            int wetIdx = TrackFX_GetParamFromIdent(tr, i, ":wet");
            double curWet = (wetIdx >= 0) ?
                TrackFX_GetParamNormalized(tr, i, wetIdx) : 1.0;
            wetLerps.push_back({ tr, i, wetIdx, curWet, 0.0, true, false });
        }
        else if (liveOffline)
        {
            // Primed plugin entering this scene — bring online with offline sandwich
            bool fxWasOpen = TrackFX_GetOpen(tr, i);
            if (fxWasOpen) TrackFX_Show(tr, i, 0);
            TrackFX_SetOffline(tr, i, true);
            TrackFX_SetOffline(tr, i, false);  // resume() fires
            if (fxWasOpen) TrackFX_Show(tr, i, 1);

            TrackFX_SetEnabled(tr, i, target->enabled);

            if (!target->fxChunk.empty())
            {
                // Plugin is now online (post-resume) — apply vst_chunk directly.
                // See site-1 comment: must NOT be wrapped in an offline sandwich.
                TrackFX_SetNamedConfigParm(tr, i, "vst_chunk", target->fxChunk.c_str());
                int wetIdx = TrackFX_GetParamFromIdent(tr, i, ":wet");
                if (wetIdx >= 0) TrackFX_SetParamNormalized(tr, i, wetIdx, 0.0);
                if (target->enabled && wetIdx >= 0)
                    wetLerps.push_back({ tr, i, wetIdx, 0.0, target->wetVal, false, false });
            }
            else
            {
                int wetIdx = TrackFX_GetParamFromIdent(tr, i, ":wet");
                // Zero wet before writing params so the plugin is silent during init
                if (wetIdx >= 0) TrackFX_SetParamNormalized(tr, i, wetIdx, 0.0);
                for (int p = 0; p < (int)target->normVals.size(); ++p)
                    TrackFX_SetParamNormalized(tr, i, p, target->normVals[p]);
                // Re-enforce wet=0 (the normVals loop may have restored it)
                if (wetIdx >= 0) TrackFX_SetParamNormalized(tr, i, wetIdx, 0.0);
                // Fade in from silence
                if (target->enabled && wetIdx >= 0)
                    wetLerps.push_back({ tr, i, wetIdx, 0.0, target->wetVal, false, false });
            }
        }
        else
        {
            const bool liveEnabled = TrackFX_GetEnabled(tr, i);
            if (liveEnabled && !target->enabled)
            {
                // Enabled → disabled: fade out then disable
                int wetIdx = TrackFX_GetParamFromIdent(tr, i, ":wet");
                double curWet = (wetIdx >= 0) ?
                    TrackFX_GetParamNormalized(tr, i, wetIdx) : 1.0;
                wetLerps.push_back({ tr, i, wetIdx, curWet, 0.0, false, true });
            }
            else if (!liveEnabled && target->enabled)
            {
                // Disabled → enabled: enable immediately, start wet at 0, fade in
                TrackFX_SetEnabled(tr, i, true);
                int wetIdx = TrackFX_GetParamFromIdent(tr, i, ":wet");
                if (wetIdx >= 0)
                {
                    TrackFX_SetParamNormalized(tr, i, wetIdx, 0.0);
                    wetLerps.push_back({ tr, i, wetIdx, 0.0, target->wetVal, false, false });
                }
            }
            else
            {
                // No change in enabled state.
                if (!target->fxChunk.empty())
                {
                    // Chunk plugin: apply vst_chunk directly on the online plugin.
                    // Wet lerp handled normally by BuildLerpLists (current→target).
                    TrackFX_SetNamedConfigParm(tr, i, "vst_chunk", target->fxChunk.c_str());
                }
                // BuildLerpLists will add a normal ParamLerp from current wet → target.wetVal.
            }
        }
    }

    // Step 2: add FX missing from track
    for (const auto& fxs : ts.fx)
    {
        int slot = findFXCached(fxs);
        if (slot >= 0) continue; // already on track, handled above

        const char* addName = fxs.fxIdent[0] ? fxs.fxIdent : fxs.name;
        slot = TrackFX_AddByName(tr, addName, false, -1000);
        if (slot < 0) continue; // plugin not installed

        // ---- Offline sandwich (empty) ----
        // Briefly take offline so the audio thread cannot process the plugin
        // between AddByName and our param writes. ALL writes happen AFTER
        // SetOffline(false) for three reasons:
        //   (a) TrackFX_GetParamFromIdent(":wet") must query a live plugin;
        //       calling it while offline can return a wrong or stale index.
        //   (b) TrackFX_SetEnabled must apply to a live plugin to stick.
        //   (c) Param writes must land after the plugin's resume()/activate()
        //       to avoid being wiped by plugin-internal initialization.
        // Always close before the sandwich — REAPER may have auto-opened the
        // window when AddByName fired. Don't re-open: this is a new add, not
        // a user-opened window being preserved.
        TrackFX_Show(tr, slot, 0);
        TrackFX_SetOffline(tr, slot, true);
        TrackFX_SetOffline(tr, slot, false);  // plugin resume() fires here; nothing written yet

        // All writes below are on a fully-live, post-resume plugin.
        TrackFX_SetEnabled(tr, slot, fxs.enabled);

        if (!fxs.fxChunk.empty())
        {
            // Plugin is online post-resume — apply vst_chunk directly.
            TrackFX_SetNamedConfigParm(tr, slot, "vst_chunk", fxs.fxChunk.c_str());
            int wetIdx = TrackFX_GetParamFromIdent(tr, slot, ":wet");
            if (wetIdx >= 0) TrackFX_SetParamNormalized(tr, slot, wetIdx, 0.0);
            if (fxs.enabled && wetIdx >= 0)
                wetLerps.push_back({ tr, slot, wetIdx, 0.0, fxs.wetVal, false, false });
        }
        else
        {
            // Resolve the wet-param index on the live plugin (correct after resume).
            int wetIdx = TrackFX_GetParamFromIdent(tr, slot, ":wet");

            // Zero wet so the plugin is silent while we load params.
            if (wetIdx >= 0)
                TrackFX_SetParamNormalized(tr, slot, wetIdx, 0.0);

            // Write all saved params to the live plugin — they will stick.
            for (int p = 0; p < (int)fxs.normVals.size(); ++p)
                TrackFX_SetParamNormalized(tr, slot, p, fxs.normVals[p]);

            // Re-enforce wet=0: if wetIdx is within normVals range the loop above
            // just restored it to the saved (non-zero) value — put it back to 0.
            if (wetIdx >= 0)
                TrackFX_SetParamNormalized(tr, slot, wetIdx, 0.0);

            // Fade wet in from 0 to the saved value.
            // BuildLerpLists will see cur==tgt for every other param → no ParamLerps
            // are added, so only the wet knob animates during the transition.
            if (fxs.enabled && wetIdx >= 0)
                wetLerps.push_back({ tr, slot, wetIdx, 0.0, fxs.wetVal, false, false });
        }
    }
    EnforceFXOrder(tr, ts.fx);
}

// ---------------------------------------------------------------------------
// ApplyImmediate – instant recall (duration == 0)
// ---------------------------------------------------------------------------
void TransitionEngine::ApplyImmediate(const TransitionSnapshot* snap, int mask,
                                      const TrackMap& tmap)
{
    int skippedTracks = 0;

    PreventUIRefresh(1);

    for (const auto& ts : snap->m_tracks)
    {
        auto it = tmap.find(ts.guid);
        MediaTrack* tr = (it != tmap.end()) ? it->second : nullptr;
        if (!tr) { ++skippedTracks; continue; }

        const int safe    = GetEffectiveSafeMask(ts.guid);
        const int effMask = mask & ~safe;

        if (effMask & TS_VOL)
        {
            double v = ts.vol;
            GetSetMediaTrackInfo(tr, "D_VOL", &v);
        }
        if (effMask & TS_PAN)
        {
            double pan = ts.pan;  int pm = ts.panMode;
            double w = ts.width;  double dpl = ts.dualPanL;
            double dpr = ts.dualPanR;  double pl = ts.panLaw;
            GetSetMediaTrackInfo(tr, "D_PAN",      &pan);
            GetSetMediaTrackInfo(tr, "I_PANMODE",  &pm);
            GetSetMediaTrackInfo(tr, "D_WIDTH",    &w);
            GetSetMediaTrackInfo(tr, "D_DUALPANL", &dpl);
            GetSetMediaTrackInfo(tr, "D_DUALPANR", &dpr);
            GetSetMediaTrackInfo(tr, "D_PANLAW",   &pl);
        }
        if (effMask & TS_MUTE)  { bool m = ts.mute;  GetSetMediaTrackInfo(tr, "B_MUTE",  &m); }
        if (effMask & TS_SOLO)  { int s = ts.solo;   GetSetMediaTrackInfo(tr, "I_SOLO",  &s); }
        if (effMask & TS_PHASE) { bool p = ts.phase; GetSetMediaTrackInfo(tr, "B_PHASE", &p); }
        if (effMask & TS_VIS)
        {
            int mixer = ts.vis & 1;  int tcp = (ts.vis >> 1) & 1;
            GetSetMediaTrackInfo(tr, "I_SHOWINMIXER", &mixer);
            GetSetMediaTrackInfo(tr, "I_SHOWINTCP",   &tcp);
        }
        if (effMask & TS_SELECTION) { int sel = ts.selected; GetSetMediaTrackInfo(tr, "I_SELECTED", &sel); }
        if (effMask & TS_PLAY_OFFSET)
        {
            int pof = ts.playOffsetFlag;  double pov = ts.playOffset;
            GetSetMediaTrackInfo(tr, "I_PLAY_OFFSET_FLAG", &pof);
            GetSetMediaTrackInfo(tr, "D_PLAY_OFFSET",      &pov);
        }

        // Layout – applied instantly, no lerp
        if ((effMask & TS_TRACKNAME) && !ts.trackName.empty())
            GetSetMediaTrackInfo_String(tr, "P_NAME", (char*)ts.trackName.c_str(), true);
        if (effMask & TS_TRACKCOLOR)
        {
            int c = ts.color;
            GetSetMediaTrackInfo(tr, "I_CUSTOMCOLOR", &c);
        }
        if (effMask & TS_TRACKHEIGHT)
        {
            int  h = ts.heightOverride;
            bool l = ts.heightLocked;
            GetSetMediaTrackInfo(tr, "I_HEIGHTOVERRIDE", &h);
            GetSetMediaTrackInfo(tr, "B_HEIGHTLOCK",     &l);
        }

        if (effMask & (TS_FXPARAMS | TS_FXCHAIN))
        {
            std::vector<WetLerp> dummy; // instant path: no wet lerps needed
            if (g_durationDebug)
            {
                double tFX0 = QpcMs();
                std::vector<RecallTimings::FXOpTiming> trackFXOps;
                SyncFXChain(tr, ts, false /*instant*/, dummy, &trackFXOps);
                double trackFXMs = QpcMs() - tFX0;
                if (trackFXMs > 1.0 || !trackFXOps.empty())
                {
                    char trkNameBuf[256] = {};
                    GetTrackName(tr, trkNameBuf, (int)sizeof(trkNameBuf));
                    RecallTimings::TrackFXTiming tft;
                    tft.trackName = trkNameBuf;
                    tft.total_ms  = trackFXMs;
                    tft.fxOps     = std::move(trackFXOps);
                    lastTimings.fxDetail.push_back(std::move(tft));
                }
            }
            else
            {
                SyncFXChain(tr, ts, false /*instant*/, dummy);
            }
            // Clear any delta-solo state that may linger from the chain sync.
            // TrackFX_SetNamedConfigParm silently fails if the param is unsupported.
            const int nfxPost = TrackFX_GetCount(tr);
            for (int fx = 0; fx < nfxPost; ++fx)
                TrackFX_SetNamedConfigParm(tr, fx, "chain_bypass_delta", "0");
        }

        // Sends – routing changes guarded by recording check (same as TS_FXCHAIN)
        if (effMask & TS_SENDS)
        {
            const bool canRoute = !(GetPlayState() & 4); // no routing changes while recording

            // --- Build maps of current live sends ---
            // Track sends: destGuid -> send index
            std::map<GUID, int, GUIDLess> liveSends;
            {
                const int n = GetTrackNumSends(tr, 0);
                for (int si = 0; si < n; si++)
                {
                    MediaTrack* dest = (MediaTrack*)GetSetTrackSendInfo(tr, 0, si, "P_DESTTRACK", nullptr);
                    if (!dest) continue;
                    GUID* pg = (GUID*)GetSetMediaTrackInfo(dest, "GUID", nullptr);
                    if (pg) liveSends.emplace(*pg, si);
                }
            }
            // HW sends: dstChan -> send index
            std::map<int, int> liveHWSends;
            {
                const int n = GetTrackNumSends(tr, 1);
                for (int si = 0; si < n; si++)
                {
                    int* pc = (int*)GetSetTrackSendInfo(tr, 1, si, "I_DSTCHAN", nullptr);
                    if (pc) liveHWSends.emplace(*pc, si);
                }
            }

            // --- Update or add sends from snapshot ---
            for (const auto& ss : ts.sends)
            {
                if (ss.isHW)
                {
                    auto it2 = liveHWSends.find(ss.hwDstChan);
                    if (it2 != liveHWSends.end())
                    {
                        // Exists: update level params
                        int si = it2->second;
                        double v = ss.vol;  double p = ss.pan;  bool m = ss.mute;
                        GetSetTrackSendInfo(tr, 1, si, "D_VOL",  &v);
                        GetSetTrackSendInfo(tr, 1, si, "D_PAN",  &p);
                        GetSetTrackSendInfo(tr, 1, si, "B_MUTE", &m);
                    }
                    else if (canRoute)
                    {
                        // Add new HW send
                        int newIdx = CreateTrackSend(tr, nullptr);
                        if (newIdx >= 0)
                        {
                            // Set channel and rebuild map entry
                            int ch = ss.hwDstChan;
                            GetSetTrackSendInfo(tr, 1, newIdx, "I_DSTCHAN", &ch);
                            double v = ss.vol;  double p = ss.pan;  bool m = ss.mute;
                            GetSetTrackSendInfo(tr, 1, newIdx, "D_VOL",  &v);
                            GetSetTrackSendInfo(tr, 1, newIdx, "D_PAN",  &p);
                            GetSetTrackSendInfo(tr, 1, newIdx, "B_MUTE", &m);
                        }
                    }
                }
                else
                {
                    auto it2 = liveSends.find(ss.destGuid);
                    if (it2 != liveSends.end())
                    {
                        // Exists: update level params
                        int si = it2->second;
                        double v = ss.vol;  double p = ss.pan;  bool m = ss.mute;  int sm = ss.sendMode;
                        GetSetTrackSendInfo(tr, 0, si, "D_VOL",      &v);
                        GetSetTrackSendInfo(tr, 0, si, "D_PAN",      &p);
                        GetSetTrackSendInfo(tr, 0, si, "B_MUTE",     &m);
                        GetSetTrackSendInfo(tr, 0, si, "I_SENDMODE", &sm);
                    }
                    else if (canRoute)
                    {
                        // Resolve destination track from GUID
                        auto destIt = tmap.find(ss.destGuid);
                        if (destIt != tmap.end())
                        {
                            int newIdx = CreateTrackSend(tr, destIt->second);
                            if (newIdx >= 0)
                            {
                                double v = ss.vol;  double p = ss.pan;  bool m = ss.mute;  int sm = ss.sendMode;
                                GetSetTrackSendInfo(tr, 0, newIdx, "D_VOL",      &v);
                                GetSetTrackSendInfo(tr, 0, newIdx, "D_PAN",      &p);
                                GetSetTrackSendInfo(tr, 0, newIdx, "B_MUTE",     &m);
                                GetSetTrackSendInfo(tr, 0, newIdx, "I_SENDMODE", &sm);
                            }
                        }
                    }
                }
            }

            // --- Remove live sends absent from snapshot (routing only, not while recording) ---
            if (canRoute)
            {
                // Track sends: build set of GUIDs in snapshot
                std::vector<GUID> snapGuids;
                std::vector<int>  snapHWChans;
                for (const auto& ss : ts.sends)
                {
                    if (ss.isHW) snapHWChans.push_back(ss.hwDstChan);
                    else         snapGuids.push_back(ss.destGuid);
                }

                // Remove track sends in reverse index order
                {
                    const int n = GetTrackNumSends(tr, 0);
                    for (int si = n - 1; si >= 0; si--)
                    {
                        MediaTrack* dest = (MediaTrack*)GetSetTrackSendInfo(tr, 0, si, "P_DESTTRACK", nullptr);
                        if (!dest) continue;
                        GUID* pg = (GUID*)GetSetMediaTrackInfo(dest, "GUID", nullptr);
                        if (!pg) continue;
                        bool found = false;
                        for (const auto& g : snapGuids)
                            if (IsEqualGUID(g, *pg)) { found = true; break; }
                        if (!found)
                            RemoveTrackSend(tr, 0, si);
                    }
                }
                // Remove HW sends in reverse index order
                {
                    const int n = GetTrackNumSends(tr, 1);
                    for (int si = n - 1; si >= 0; si--)
                    {
                        int* pc = (int*)GetSetTrackSendInfo(tr, 1, si, "I_DSTCHAN", nullptr);
                        if (!pc) continue;
                        bool found = false;
                        for (int ch : snapHWChans)
                            if (ch == *pc) { found = true; break; }
                        if (!found)
                            RemoveTrackSend(tr, 1, si);
                    }
                }
            }
        }
    }

    // Track reordering – must happen after all per-track property updates
    // because FindTrack() scans by GUID at current positions.
    if (mask & TS_TRACKORDER)
    {
        // Build a target order from tracks that have a valid capturedIndex.
        // We only reorder tracks that exist in the snapshot; unknown tracks stay put.
        struct OrderEntry { int targetIdx; GUID guid; };
        std::vector<OrderEntry> order;
        for (const auto& ts : snap->m_tracks)
        {
            if (ts.capturedIndex < 0) continue;
            const int safe = GetEffectiveSafeMask(ts.guid);
            if (safe & TS_TRACKORDER) continue; // safed
            order.push_back({ ts.capturedIndex, ts.guid });
        }
        // Sort by target index
        std::sort(order.begin(), order.end(),
                  [](const OrderEntry& a, const OrderEntry& b){ return a.targetIdx < b.targetIdx; });
        // Move each track into position iteratively
        for (int pass = 0; pass < (int)order.size(); ++pass)
        {
            auto it2 = tmap.find(order[pass].guid);
            MediaTrack* tr = (it2 != tmap.end()) ? it2->second : nullptr;
            if (!tr) continue;
            // Current index of this track
            // IP_TRACKNUMBER returns the value directly as void* (1-based), not a pointer
            int curIdx = (int)(intptr_t)GetSetMediaTrackInfo(tr, "IP_TRACKNUMBER", nullptr) - 1;
            if (curIdx == pass) continue; // already in place
            // Don't move a track outside its parent folder
            if (WouldLeaveFolder(tr, pass)) continue;
            // Deselect all, select only this track
            int n = GetNumTracks();
            for (int i = 0; i < n; ++i)
            {
                MediaTrack* t2 = GetTrack(nullptr, i);
                if (!t2) continue;
                int zero = 0;
                GetSetMediaTrackInfo(t2, "I_SELECTED", &zero);
            }
            int one = 1;
            GetSetMediaTrackInfo(tr, "I_SELECTED", &one);
            // Insert before track at target position
            ReorderSelectedTracks(pass, 0);
        }
    }

    // Close all open FX windows on recall
    {
        int nTr = CountTracks(nullptr);
        for (int tIdx = 0; tIdx < nTr; tIdx++)
        {
            MediaTrack* trSweep = GetTrack(nullptr, tIdx);
            if (!trSweep) continue;
            int nfx = TrackFX_GetCount(trSweep);
            for (int fx = 0; fx < nfx; fx++)
                if (TrackFX_GetOpen(trSweep, fx))
                    TrackFX_Show(trSweep, fx, 0);
        }
    }

    TrackList_AdjustWindows(false);
    PreventUIRefresh(-1);

    if (skippedTracks > 0)
        snprintf(m_statusBuf, sizeof(m_statusBuf),
                 "Done %s (%d track%s not found)", m_targetSceneName.c_str(),
                 skippedTracks, skippedTracks != 1 ? "s" : "");
    else
        snprintf(m_statusBuf, sizeof(m_statusBuf), "Done %s", m_targetSceneName.c_str());
}

// ---------------------------------------------------------------------------
// BuildLerpLists – populate m_paramLerps and m_volPanLerps from live state
// ---------------------------------------------------------------------------
void TransitionEngine::BuildLerpLists(const TransitionSnapshot* snap, int mask,
                                       const TrackMap& tmap)
{
    m_paramLerps.clear();
    m_volPanLerps.clear();
    m_sendLerps.clear();

    for (const auto& ts : snap->m_tracks)
    {
        auto it = tmap.find(ts.guid);
        MediaTrack* tr = (it != tmap.end()) ? it->second : nullptr;
        if (!tr) continue;

        const int safe    = GetEffectiveSafeMask(ts.guid);
        const int effMask = mask & ~safe;

        // Vol / pan lerp entry
        if ((effMask & (TS_VOL | TS_PAN)) != 0)
        {
            VolPanLerp vpl{};
            vpl.tr = tr;

            if (effMask & TS_VOL)
            {
                double* pv = (double*)GetSetMediaTrackInfo(tr, "D_VOL", nullptr);
                vpl.startVol = pv ? *pv : 1.0;
                vpl.endVol   = ts.vol;
            }
            if (effMask & TS_PAN)
            {
                double* pp = (double*)GetSetMediaTrackInfo(tr, "D_PAN", nullptr);
                vpl.startPan = pp ? *pp : 0.0;
                vpl.endPan   = ts.pan;
            }

            const bool volMoves = (effMask & TS_VOL) && fabs(vpl.endVol - vpl.startVol) >= 1e-9;
            const bool panMoves = (effMask & TS_PAN) && fabs(vpl.endPan - vpl.startPan) >= 1e-9;
            if (volMoves || panMoves)
                m_volPanLerps.push_back(vpl);
        }

        // FX param lerp entries (SyncFXChain has already synced the chain)
        if (effMask & TS_FXPARAMS)
        {
            // Opt C: build O(1) live-slot lookup for this track
            const int nLiveFX = TrackFX_GetCount(tr);
            std::unordered_map<std::string, int> blSlotByIdent;
            std::unordered_map<std::string, int> blSlotByNameCount;
            for (int fi = 0; fi < nLiveFX; ++fi)
            {
                char ident[512] = {}, name[256] = {};
                TrackFX_GetNamedConfigParm(tr, fi, "fx_ident", ident, (int)sizeof(ident));
                TrackFX_GetFXName(tr, fi, name, (int)sizeof(name));
                int pc = TrackFX_GetNumParams(tr, fi);
                if (ident[0]) blSlotByIdent[ident] = fi;
                char key[768]; snprintf(key, sizeof(key), "%s\x01%d", name, pc);
                blSlotByNameCount[key] = fi;
            }

            for (const auto& fxs : ts.fx)
            {
                int slot = -1;
                if (fxs.fxIdent[0])
                {
                    auto it = blSlotByIdent.find(fxs.fxIdent);
                    if (it != blSlotByIdent.end()) slot = it->second;
                }
                else
                {
                    // Hint slot fast-path
                    if (fxs.slotIndex >= 0 && fxs.slotIndex < nLiveFX)
                    {
                        char hname[256] = {};
                        TrackFX_GetFXName(tr, fxs.slotIndex, hname, (int)sizeof(hname));
                        if (strcmp(hname, fxs.name) == 0 && TrackFX_GetNumParams(tr, fxs.slotIndex) == fxs.paramCount)
                            slot = fxs.slotIndex;
                    }
                    if (slot < 0)
                    {
                        char key[768]; snprintf(key, sizeof(key), "%s\x01%d", fxs.name, fxs.paramCount);
                        auto it = blSlotByNameCount.find(key);
                        if (it != blSlotByNameCount.end()) slot = it->second;
                    }
                }
                if (slot < 0) continue;

                // Find the wet param index for this FX slot if SyncFXChain
                // already added a WetLerp for it (timed path). Skip that param
                // here to avoid a conflicting duplicate lerp entry.
                int wetParamAlreadyLerped = -1;
                for (const auto& wl : m_wetLerps)
                    if (wl.tr == tr && wl.fxSlot == slot)
                        { wetParamAlreadyLerped = wl.wetParamIdx; break; }

                for (int p = 0; p < (int)fxs.normVals.size(); p++)
                {
                    if (p == wetParamAlreadyLerped) continue; // handled by WetLerp
                    double cur = TrackFX_GetParamNormalized(tr, slot, p);
                    double tgt = fxs.normVals[p];
                    if (fabs(cur - tgt) < 1e-7) continue;
                    m_paramLerps.push_back({tr, slot, p, cur, tgt});
                }

                // Wet/dry lerp: only if no WetLerp already owns this slot
                if (wetParamAlreadyLerped < 0)
                {
                    int wetIdx = TrackFX_GetParamFromIdent(tr, slot, ":wet");
                    if (wetIdx >= 0)
                    {
                        double curWet = TrackFX_GetParamNormalized(tr, slot, wetIdx);
                        if (fabs(curWet - fxs.wetVal) > 1e-7)
                            m_paramLerps.push_back({tr, slot, wetIdx, curWet, fxs.wetVal});
                    }
                }
            }
        }

        // Send lerp entries (vol/pan fade for each send present after routing reconcile)
        if (effMask & TS_SENDS)
        {
            // Build live send maps after routing was already reconciled in the timed
            // pre-pass (Recall() loop runs SyncSends equivalent via ApplyImmediate-style
            // routing-only pass).  We read live state now as the authoritative source.
            for (const auto& ss : ts.sends)
            {
                // Find the live send index
                int liveIdx = -1;
                if (ss.isHW)
                {
                    const int n = GetTrackNumSends(tr, 1);
                    for (int si = 0; si < n; si++)
                    {
                        int* pc = (int*)GetSetTrackSendInfo(tr, 1, si, "I_DSTCHAN", nullptr);
                        if (pc && *pc == ss.hwDstChan) { liveIdx = si; break; }
                    }
                }
                else
                {
                    const int n = GetTrackNumSends(tr, 0);
                    for (int si = 0; si < n; si++)
                    {
                        MediaTrack* dest = (MediaTrack*)GetSetTrackSendInfo(tr, 0, si, "P_DESTTRACK", nullptr);
                        if (!dest) continue;
                        GUID* pg = (GUID*)GetSetMediaTrackInfo(dest, "GUID", nullptr);
                        if (pg && IsEqualGUID(*pg, ss.destGuid)) { liveIdx = si; break; }
                    }
                }
                if (liveIdx < 0) continue; // send not present (routing guard blocked add)

                int cat = ss.isHW ? 1 : 0;
                double* pv = (double*)GetSetTrackSendInfo(tr, cat, liveIdx, "D_VOL", nullptr);
                double* pp = (double*)GetSetTrackSendInfo(tr, cat, liveIdx, "D_PAN", nullptr);
                double curVol = pv ? *pv : 1.0;
                double curPan = pp ? *pp : 0.0;

                const bool volMoves = fabs(ss.vol - curVol) >= 1e-9;
                const bool panMoves = fabs(ss.pan - curPan) >= 1e-9;
                if (volMoves || panMoves)
                {
                    SendLerp sl{};
                    sl.tr           = tr;
                    sl.sendIdx      = liveIdx;
                    sl.destGuid     = ss.destGuid;
                    sl.isHW         = ss.isHW;
                    sl.hwDstChan    = ss.hwDstChan;
                    sl.startVol     = curVol;
                    sl.endVol       = ss.vol;
                    sl.startPan     = curPan;
                    sl.endPan       = ss.pan;
                    sl.pendingRemove = false;
                    m_sendLerps.push_back(sl);
                }
            }

            // Also push fade-out lerps for live sends that are being removed
            // (routing guard must have allowed the removal – if not recording).
            // We compare live sends against snapshot to find orphaned ones.
            if (!(GetPlayState() & 4))
            {
                // Track sends: find live sends not in snapshot → fade to 0
                const int nTr = GetTrackNumSends(tr, 0);
                for (int si = 0; si < nTr; si++)
                {
                    MediaTrack* dest = (MediaTrack*)GetSetTrackSendInfo(tr, 0, si, "P_DESTTRACK", nullptr);
                    if (!dest) continue;
                    GUID* pg = (GUID*)GetSetMediaTrackInfo(dest, "GUID", nullptr);
                    if (!pg) continue;
                    bool inSnap = false;
                    for (const auto& ss : ts.sends)
                        if (!ss.isHW && IsEqualGUID(ss.destGuid, *pg)) { inSnap = true; break; }
                    if (!inSnap)
                    {
                        double* pv = (double*)GetSetTrackSendInfo(tr, 0, si, "D_VOL", nullptr);
                        double* pp = (double*)GetSetTrackSendInfo(tr, 0, si, "D_PAN", nullptr);
                        SendLerp sl{};
                        sl.tr            = tr;
                        sl.sendIdx       = si;
                        sl.destGuid      = *pg;
                        sl.isHW          = false;
                        sl.hwDstChan     = 0;
                        sl.startVol      = pv ? *pv : 1.0;
                        sl.endVol        = 0.0;
                        sl.startPan      = pp ? *pp : 0.0;
                        sl.endPan        = pp ? *pp : 0.0;
                        sl.pendingRemove = true;
                        m_sendLerps.push_back(sl);
                    }
                }
                // HW sends: same
                const int nHW = GetTrackNumSends(tr, 1);
                for (int si = 0; si < nHW; si++)
                {
                    int* pc = (int*)GetSetTrackSendInfo(tr, 1, si, "I_DSTCHAN", nullptr);
                    if (!pc) continue;
                    bool inSnap = false;
                    for (const auto& ss : ts.sends)
                        if (ss.isHW && ss.hwDstChan == *pc) { inSnap = true; break; }
                    if (!inSnap)
                    {
                        double* pv = (double*)GetSetTrackSendInfo(tr, 1, si, "D_VOL", nullptr);
                        double* pp = (double*)GetSetTrackSendInfo(tr, 1, si, "D_PAN", nullptr);
                        SendLerp sl{};
                        sl.tr            = tr;
                        sl.sendIdx       = si;
                        sl.hwDstChan     = *pc;
                        sl.isHW          = true;
                        sl.startVol      = pv ? *pv : 1.0;
                        sl.endVol        = 0.0;
                        sl.startPan      = pp ? *pp : 0.0;
                        sl.endPan        = pp ? *pp : 0.0;
                        sl.pendingRemove = true;
                        m_sendLerps.push_back(sl);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// SnapToEnd – write exact final values for all lerp entries, then clear
// ---------------------------------------------------------------------------
void TransitionEngine::SnapToEnd()
{
    for (const auto& vpl : m_volPanLerps)
    {
        if (!ValidatePtr2(nullptr, vpl.tr, "MediaTrack*")) continue;
        double ev = vpl.endVol;
        double ep = vpl.endPan;
        GetSetMediaTrackInfo(vpl.tr, "D_VOL", &ev);
        GetSetMediaTrackInfo(vpl.tr, "D_PAN", &ep);
    }
    for (const auto& pl : m_paramLerps)
    {
        if (!ValidatePtr2(nullptr, pl.tr, "MediaTrack*")) continue;
        TrackFX_SetParamNormalized(pl.tr, pl.fxIdx, pl.paramIdx, pl.endNorm);
    }

    // Collect plugins to delete (must delete in descending slot order per track)
    struct DelEntry { MediaTrack* tr; int slot; };
    std::vector<DelEntry> toDelete;

    for (const auto& wl : m_wetLerps)
    {
        if (!ValidatePtr2(nullptr, wl.tr, "MediaTrack*")) continue;
        if (wl.deleteOnComplete)
        {
            if (wl.wetParamIdx >= 0)
                TrackFX_SetParamNormalized(wl.tr, wl.fxSlot, wl.wetParamIdx, wl.endWet);
            toDelete.push_back({ wl.tr, wl.fxSlot });
        }
        else if (wl.disableOnComplete)
        {
            TrackFX_SetEnabled(wl.tr, wl.fxSlot, false);
            // Restore wet so the FX chain is clean when re-enabled later
            if (wl.wetParamIdx >= 0)
                TrackFX_SetParamNormalized(wl.tr, wl.fxSlot, wl.wetParamIdx, wl.startWet);
        }
        else
        {
            if (wl.wetParamIdx >= 0)
                TrackFX_SetParamNormalized(wl.tr, wl.fxSlot, wl.wetParamIdx, wl.endWet);
        }
    }

    // Delete in descending slot order (so earlier indices aren't invalidated).
    // Set offline first so REAPER removes each plugin from the signal path
    // cleanly before destroying it — avoids the chain-reconstruction click.
    std::sort(toDelete.begin(), toDelete.end(),
              [](const DelEntry& a, const DelEntry& b) {
                  return a.tr == b.tr ? a.slot > b.slot : a.tr > b.tr;
              });
    for (const auto& de : toDelete)
    {
        TrackFX_SetOffline(de.tr, de.slot, true);
        TrackFX_Delete(de.tr, de.slot);
    }

    m_volPanLerps.clear();
    m_paramLerps.clear();
    m_wetLerps.clear();

    // Finalize send lerps: write exact end values, then remove pending sends
    // in descending index order per track to avoid index invalidation.
    struct SendDelEntry { MediaTrack* tr; int cat; int idx; };
    std::vector<SendDelEntry> sendsToRemove;

    for (const auto& sl : m_sendLerps)
    {
        if (!ValidatePtr2(nullptr, sl.tr, "MediaTrack*")) continue;
        int cat = sl.isHW ? 1 : 0;
        // Revalidate index (sends can shift if prior removals happened this frame)
        int liveIdx = sl.sendIdx;
        if (sl.isHW)
        {
            const int n = GetTrackNumSends(sl.tr, 1);
            liveIdx = -1;
            for (int si = 0; si < n; si++)
            {
                int* pc = (int*)GetSetTrackSendInfo(sl.tr, 1, si, "I_DSTCHAN", nullptr);
                if (pc && *pc == sl.hwDstChan) { liveIdx = si; break; }
            }
        }
        else
        {
            const int n = GetTrackNumSends(sl.tr, 0);
            liveIdx = -1;
            for (int si = 0; si < n; si++)
            {
                MediaTrack* dest = (MediaTrack*)GetSetTrackSendInfo(sl.tr, 0, si, "P_DESTTRACK", nullptr);
                if (!dest) continue;
                GUID* pg = (GUID*)GetSetMediaTrackInfo(dest, "GUID", nullptr);
                if (pg && IsEqualGUID(*pg, sl.destGuid)) { liveIdx = si; break; }
            }
        }
        if (liveIdx < 0) continue;

        double ev = sl.endVol;
        double ep = sl.endPan;
        GetSetTrackSendInfo(sl.tr, cat, liveIdx, "D_VOL", &ev);
        GetSetTrackSendInfo(sl.tr, cat, liveIdx, "D_PAN", &ep);
        if (sl.pendingRemove && !(GetPlayState() & 4))
            sendsToRemove.push_back({ sl.tr, cat, liveIdx });
    }
    // Remove in descending index order per track so indices stay valid
    std::sort(sendsToRemove.begin(), sendsToRemove.end(),
              [](const SendDelEntry& a, const SendDelEntry& b) {
                  if (a.tr  != b.tr)  return a.tr  > b.tr;
                  if (a.cat != b.cat) return a.cat > b.cat;
                  return a.idx > b.idx;
              });
    for (const auto& sd : sendsToRemove)
        RemoveTrackSend(sd.tr, sd.cat, sd.idx);

    m_sendLerps.clear();
}

// ---------------------------------------------------------------------------
// Recall (main entry point)
// ---------------------------------------------------------------------------
void TransitionEngine::Recall(const TransitionSnapshot* snap,
                               int    mask,
                               double duration)
{
    if (!snap) return;

    lastTimings = RecallTimings{};  // reset
    double tRecallStart = QpcMs();

    m_lastRecalledSlot = snap->m_slot;
    m_targetSceneName  = snap->m_name;

    // Store taper settings from the snapshot
    m_taper    = (TaperLaw)snap->m_taper;
    m_taperExp = snap->m_taperExp;

    // If a transition is already active, snap it to end first
    if (m_active)
    {
        double t0 = QpcMs();
        SnapToEnd();
        plugin_register("-timer", (void*)&TransitionEngine::TimerCallback);
        m_active = false;
        lastTimings.snapToEnd = QpcMs() - t0;
    }

    // -----------------------------------------------------------------------
    // Instant path (duration == 0): single tight loop, no timer overhead
    // -----------------------------------------------------------------------
    if (duration <= 0.0)
    {
        lastTimings.instantPath = true;

        double t0 = QpcMs();
        TrackMap tmap = BuildTrackMap();
        lastTimings.buildTrackMap = QpcMs() - t0;

        t0 = QpcMs();
        ApplyImmediate(snap, mask, tmap);
        lastTimings.discreteParams = QpcMs() - t0;  // ApplyImmediate covers all instant work

        // Sort fxDetail slowest-first for the console report
        std::sort(lastTimings.fxDetail.begin(), lastTimings.fxDetail.end(),
                  [](const RecallTimings::TrackFXTiming& a,
                     const RecallTimings::TrackFXTiming& b)
                  { return a.total_ms > b.total_ms; });

        lastTimings.tracksMatched = (int)tmap.size();
        lastTimings.total = QpcMs() - tRecallStart;

        m_active = false;
        if (onTransitionComplete) onTransitionComplete();
        return;
    }

    // -----------------------------------------------------------------------
    // Timed path: apply discrete params immediately, then build lerp lists
    // -----------------------------------------------------------------------
    lastTimings.instantPath = false;

    double t0 = QpcMs();
    TrackMap tmap = BuildTrackMap();
    lastTimings.buildTrackMap = QpcMs() - t0;
    lastTimings.tracksMatched = (int)tmap.size();

    m_wetLerps.clear();
    int skippedTracks = 0;

    PreventUIRefresh(1);

    double tDiscrete = 0.0, tFXChain = 0.0, tSends = 0.0;

    for (const auto& ts : snap->m_tracks)
    {
        auto it = tmap.find(ts.guid);
        MediaTrack* tr = (it != tmap.end()) ? it->second : nullptr;
        if (!tr) { ++skippedTracks; continue; }

        const int safe    = GetEffectiveSafeMask(ts.guid);
        const int effMask = mask & ~safe;

        // Apply discrete params immediately (no lerp for these)
        double td0 = QpcMs();
        if (effMask & TS_MUTE)  { bool m = ts.mute;  GetSetMediaTrackInfo(tr, "B_MUTE",  &m); }
        if (effMask & TS_SOLO)  { int  s = ts.solo;  GetSetMediaTrackInfo(tr, "I_SOLO",  &s); }
        if (effMask & TS_PHASE) { bool p = ts.phase; GetSetMediaTrackInfo(tr, "B_PHASE", &p); }
        if (effMask & TS_VIS)
        {
            int mixer = ts.vis & 1;  int tcp = (ts.vis >> 1) & 1;
            GetSetMediaTrackInfo(tr, "I_SHOWINMIXER", &mixer);
            GetSetMediaTrackInfo(tr, "I_SHOWINTCP",   &tcp);
        }
        if (effMask & TS_SELECTION) { int sel = ts.selected; GetSetMediaTrackInfo(tr, "I_SELECTED", &sel); }
        if (effMask & TS_PLAY_OFFSET)
        {
            int pof = ts.playOffsetFlag;  double pov = ts.playOffset;
            GetSetMediaTrackInfo(tr, "I_PLAY_OFFSET_FLAG", &pof);
            GetSetMediaTrackInfo(tr, "D_PLAY_OFFSET",      &pov);
        }

        // Layout bits – always instant even in timed transitions
        if ((effMask & TS_TRACKNAME) && !ts.trackName.empty())
            GetSetMediaTrackInfo_String(tr, "P_NAME", (char*)ts.trackName.c_str(), true);
        if (effMask & TS_TRACKCOLOR)
        {
            int c = ts.color;
            GetSetMediaTrackInfo(tr, "I_CUSTOMCOLOR", &c);
        }
        if (effMask & TS_TRACKHEIGHT)
        {
            int  h = ts.heightOverride;
            bool l = ts.heightLocked;
            GetSetMediaTrackInfo(tr, "I_HEIGHTOVERRIDE", &h);
            GetSetMediaTrackInfo(tr, "B_HEIGHTLOCK",     &l);
        }
        tDiscrete += QpcMs() - td0;

        // FX chain sync: add/remove plugins with wet-fade (timed=true)
        double tfx0 = QpcMs();
        if (effMask & (TS_FXPARAMS | TS_FXCHAIN))
            SyncFXChain(tr, ts, true /*timed*/, m_wetLerps);
        tFXChain += QpcMs() - tfx0;

        // Sends routing – apply at t=0 (same guard as TS_FXCHAIN)
        // Level changes are handled by BuildLerpLists / SendLerp.
        double ts0 = QpcMs();
        if ((effMask & TS_SENDS) && !ts.sends.empty())
        {
            const bool canRoute = !(GetPlayState() & 4);

            // Build live send maps
            std::map<GUID, int, GUIDLess> liveSends;
            {
                const int n = GetTrackNumSends(tr, 0);
                for (int si = 0; si < n; si++)
                {
                    MediaTrack* dest = (MediaTrack*)GetSetTrackSendInfo(tr, 0, si, "P_DESTTRACK", nullptr);
                    if (!dest) continue;
                    GUID* pg = (GUID*)GetSetMediaTrackInfo(dest, "GUID", nullptr);
                    if (pg) liveSends.emplace(*pg, si);
                }
            }
            std::map<int, int> liveHWSends;
            {
                const int n = GetTrackNumSends(tr, 1);
                for (int si = 0; si < n; si++)
                {
                    int* pc = (int*)GetSetTrackSendInfo(tr, 1, si, "I_DSTCHAN", nullptr);
                    if (pc) liveHWSends.emplace(*pc, si);
                }
            }

            // Add new sends (not yet live) with initial vol=0 so they fade in via SendLerp
            if (canRoute)
            {
                for (const auto& ss : ts.sends)
                {
                    if (ss.isHW)
                    {
                        if (liveHWSends.find(ss.hwDstChan) == liveHWSends.end())
                        {
                            int newIdx = CreateTrackSend(tr, nullptr);
                            if (newIdx >= 0)
                            {
                                int ch = ss.hwDstChan;  double v = 0.0;
                                GetSetTrackSendInfo(tr, 1, newIdx, "I_DSTCHAN", &ch);
                                GetSetTrackSendInfo(tr, 1, newIdx, "D_VOL",     &v);
                                bool m = ss.mute;
                                GetSetTrackSendInfo(tr, 1, newIdx, "B_MUTE",    &m);
                            }
                        }
                    }
                    else
                    {
                        if (liveSends.find(ss.destGuid) == liveSends.end())
                        {
                            auto destIt = tmap.find(ss.destGuid);
                            if (destIt != tmap.end())
                            {
                                int newIdx = CreateTrackSend(tr, destIt->second);
                                if (newIdx >= 0)
                                {
                                    double v = 0.0;  int sm = ss.sendMode;
                                    GetSetTrackSendInfo(tr, 0, newIdx, "D_VOL",      &v);
                                    GetSetTrackSendInfo(tr, 0, newIdx, "I_SENDMODE", &sm);
                                    bool m = ss.mute;
                                    GetSetTrackSendInfo(tr, 0, newIdx, "B_MUTE",     &m);
                                }
                            }
                        }
                    }
                }

                // Remove sends absent from snapshot (in reverse order; they'll fade out via SendLerp before removal)
                // We just leave them in place here — BuildLerpLists will push pendingRemove SendLerps.
                // Nothing to do now for the removal side.
            }
        }
        tSends += QpcMs() - ts0;
    } // end per-track timed loop

    lastTimings.discreteParams = tDiscrete;
    lastTimings.fxChainSync    = tFXChain;
    lastTimings.sendsSetup     = tSends;
    lastTimings.tracksSkipped  = skippedTracks;
    lastTimings.tracksMatched  = (int)tmap.size() - skippedTracks;

    // Track reordering – instant, happens before lerp timer starts
    {
        double tr0 = QpcMs();
        if (mask & TS_TRACKORDER)
        {
        struct OrderEntry { int targetIdx; GUID guid; };
        std::vector<OrderEntry> order;
        for (const auto& ts : snap->m_tracks)
        {
            if (ts.capturedIndex < 0) continue;
            const int safe = GetEffectiveSafeMask(ts.guid);
            if (safe & TS_TRACKORDER) continue;
            order.push_back({ ts.capturedIndex, ts.guid });
        }
        std::sort(order.begin(), order.end(),
                  [](const OrderEntry& a, const OrderEntry& b){ return a.targetIdx < b.targetIdx; });
        for (int pass = 0; pass < (int)order.size(); ++pass)
        {
            auto it2 = tmap.find(order[pass].guid);
            MediaTrack* tr = (it2 != tmap.end()) ? it2->second : nullptr;
            if (!tr) continue;
            int curIdx = (int)(intptr_t)GetSetMediaTrackInfo(tr, "IP_TRACKNUMBER", nullptr) - 1;
            if (curIdx == pass) continue;
            // Don't move a track outside its parent folder
            if (WouldLeaveFolder(tr, pass)) continue;
            int n = GetNumTracks();
            for (int i = 0; i < n; ++i)
            {
                MediaTrack* t2 = GetTrack(nullptr, i);
                if (!t2) continue;
                int zero = 0; GetSetMediaTrackInfo(t2, "I_SELECTED", &zero);
            }
            int one = 1; GetSetMediaTrackInfo(tr, "I_SELECTED", &one);
            ReorderSelectedTracks(pass, 0);
        }
        }
        lastTimings.trackReorder = QpcMs() - tr0;
    }

    PreventUIRefresh(-1);

    // Build lerp lists for vol/pan/FX params
    {
        double tbl0 = QpcMs();
        BuildLerpLists(snap, mask, tmap);
        lastTimings.buildLerpLists = QpcMs() - tbl0;
    }

    lastTimings.paramLerps  = (int)m_paramLerps.size();
    lastTimings.volPanLerps = (int)m_volPanLerps.size();
    lastTimings.wetLerps    = (int)m_wetLerps.size();
    lastTimings.sendLerps   = (int)m_sendLerps.size();
    lastTimings.total       = QpcMs() - tRecallStart;

    if (m_paramLerps.empty() && m_volPanLerps.empty() && m_wetLerps.empty() && m_sendLerps.empty())
    {
        snprintf(m_statusBuf, sizeof(m_statusBuf), "Done %s (no interpolatable params)", m_targetSceneName.c_str());
        if (onTransitionComplete) onTransitionComplete();
        return;
    }

    m_duration  = duration;
    m_startTime = time_precise();
    m_active    = true;

    if (skippedTracks > 0)
        snprintf(m_statusBuf, sizeof(m_statusBuf),
                 "Transitioning... %s (%d track%s not found)",
                 m_targetSceneName.c_str(), skippedTracks, skippedTracks != 1 ? "s" : "");
    else
        snprintf(m_statusBuf, sizeof(m_statusBuf), "Transitioning... %s", m_targetSceneName.c_str());

    plugin_register("timer", (void*)&TransitionEngine::TimerCallback);
}

// ---------------------------------------------------------------------------
// TimerCallback – runs on REAPER main thread at ~30 fps
// ---------------------------------------------------------------------------
void TransitionEngine::TimerCallback()
{
    TransitionEngine& eng = TransitionEngine::Get();

    if (!eng.m_active) return;

    const double elapsed = time_precise() - eng.m_startTime;
    const double t_raw   = clamp01(elapsed / eng.m_duration);
    const double t       = ApplyTaper(t_raw, eng.m_taper, eng.m_taperExp);

    // Interpolate vol/pan
    for (const auto& vpl : eng.m_volPanLerps)
    {
        if (!ValidatePtr2(nullptr, vpl.tr, "MediaTrack*")) continue;
        double v = lerp(vpl.startVol, vpl.endVol, t);
        double p = lerp(vpl.startPan, vpl.endPan, t);
        GetSetMediaTrackInfo(vpl.tr, "D_VOL", &v);
        GetSetMediaTrackInfo(vpl.tr, "D_PAN", &p);
    }

    // Interpolate FX params
    for (const auto& pl : eng.m_paramLerps)
    {
        if (!ValidatePtr2(nullptr, pl.tr, "MediaTrack*")) continue;
        TrackFX_SetParamNormalized(pl.tr, pl.fxIdx, pl.paramIdx,
                                   lerp(pl.startNorm, pl.endNorm, t));
    }

    // Interpolate wet/dry for plugin fades (add/remove/disable)
    for (const auto& wl : eng.m_wetLerps)
    {
        if (!ValidatePtr2(nullptr, wl.tr, "MediaTrack*")) continue;
        if (wl.wetParamIdx >= 0)
            TrackFX_SetParamNormalized(wl.tr, wl.fxSlot, wl.wetParamIdx,
                                       lerp(wl.startWet, wl.endWet, t));
    }

    // Interpolate send vol/pan
    for (const auto& sl : eng.m_sendLerps)
    {
        if (!ValidatePtr2(nullptr, sl.tr, "MediaTrack*")) continue;
        int cat = sl.isHW ? 1 : 0;
        double v = lerp(sl.startVol, sl.endVol, t);
        double p = lerp(sl.startPan, sl.endPan, t);
        GetSetTrackSendInfo(sl.tr, cat, sl.sendIdx, "D_VOL", &v);
        GetSetTrackSendInfo(sl.tr, cat, sl.sendIdx, "D_PAN", &p);
    }

    if (t_raw >= 1.0)
    {
        // Write exact end values and finalise wet lerps (delete/disable)
        eng.SnapToEnd();

        // Close all open FX windows on recall
        {
            int nTr = CountTracks(nullptr);
            for (int tIdx = 0; tIdx < nTr; tIdx++)
            {
                MediaTrack* trSweep = GetTrack(nullptr, tIdx);
                if (!trSweep) continue;
                int nfx = TrackFX_GetCount(trSweep);
                for (int fx = 0; fx < nfx; fx++)
                    if (TrackFX_GetOpen(trSweep, fx))
                        TrackFX_Show(trSweep, fx, 0);
            }
        }

        TrackList_AdjustWindows(false);

        plugin_register("-timer", (void*)&TransitionEngine::TimerCallback);
        eng.m_active = false;
        snprintf(eng.m_statusBuf, sizeof(eng.m_statusBuf), "Done %s", eng.m_targetSceneName.c_str());
        if (eng.onTransitionComplete) eng.onTransitionComplete();
    }
}

// ---------------------------------------------------------------------------
// GetProgress
// ---------------------------------------------------------------------------
double TransitionEngine::GetProgress() const
{
    if (!m_active || m_duration <= 0.0) return 1.0;
    return clamp01((time_precise() - m_startTime) / m_duration);
}
