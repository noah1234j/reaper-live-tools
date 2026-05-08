#include "TransitionEngine.h"
#include "api.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cassert>

// Convenience wrapper (mirrors REAPER SDK GetMediaTrackInfo_Value)
static inline double GetMediaTrackInfo_Value(MediaTrack* tr, const char* parm)
{
    double* pd = (double*)GetSetMediaTrackInfo(tr, parm, nullptr);
    return pd ? *pd : 0.0;
}

// ---------------------------------------------------------------------------
// Safes globals
// ---------------------------------------------------------------------------
int  g_globalSafeMask     = 0;
bool g_trackSafesEnabled = true;
std::vector<TrackSafeEntry> g_trackSafes;

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
// FindFX – check hint slot first; fall back to O(N) name+paramCount scan
// ---------------------------------------------------------------------------
int TransitionEngine::FindFX(MediaTrack* tr,
                              const char* name, int paramCount, int hintSlot)
{
    const int nfx = TrackFX_GetCount(tr);
    if (nfx <= 0) return -1;

    // Fast path: hint slot still matches
    if (hintSlot >= 0 && hintSlot < nfx)
    {
        char hname[256] = {};
        TrackFX_GetFXName(tr, hintSlot, hname, (int)sizeof(hname));
        if (strcmp(hname, name) == 0 && TrackFX_GetNumParams(tr, hintSlot) == paramCount)
            return hintSlot;
    }

    // Slow path: scan all FX
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
                                    bool timed, std::vector<WetLerp>& wetLerps)
{
    // Helper: does this FX (by name+paramCount) appear in the snapshot?
    auto inSnapshot = [&](const char* name, int nparams) -> const FXState* {
        for (const auto& fxs : ts.fx)
            if (strcmp(fxs.name, name) == 0 && fxs.paramCount == nparams)
                return &fxs;
        return nullptr;
    };

    if (!timed)
    {
        // ---- Instant path: remove extras, add missing, set params ---------
        for (int i = TrackFX_GetCount(tr) - 1; i >= 0; --i)
        {
            char name[256] = {};
            TrackFX_GetFXName(tr, i, name, sizeof(name));
            if (!inSnapshot(name, TrackFX_GetNumParams(tr, i)))
                TrackFX_Delete(tr, i);
        }
        for (const auto& fxs : ts.fx)
        {
            int slot = FindFX(tr, fxs.name, fxs.paramCount, fxs.slotIndex);
            if (slot < 0)
            {
                slot = TrackFX_AddByName(tr, fxs.name, false, -1000);
                if (slot < 0) continue;
            }
            TrackFX_SetEnabled(tr, slot, fxs.enabled);
            for (int p = 0; p < (int)fxs.normVals.size(); ++p)
                TrackFX_SetParamNormalized(tr, slot, p, fxs.normVals[p]);
        }
        return;
    }

    // ---- Timed path -------------------------------------------------------
    // Step 1: for each FX currently on the track, decide fate
    const int nLive = TrackFX_GetCount(tr);
    for (int i = 0; i < nLive; ++i)
    {
        char name[256] = {};
        TrackFX_GetFXName(tr, i, name, sizeof(name));
        const int nparams = TrackFX_GetNumParams(tr, i);
        const FXState* target = inSnapshot(name, nparams);

        if (!target)
        {
            // Plugin not in snapshot → fade out then delete
            int wetIdx = TrackFX_GetParamFromIdent(tr, i, ":wet");
            double curWet = (wetIdx >= 0) ?
                TrackFX_GetParamNormalized(tr, i, wetIdx) : 1.0;
            wetLerps.push_back({ tr, i, wetIdx, curWet, 0.0, true, false });
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
                    wetLerps.push_back({ tr, i, wetIdx, 0.0, 1.0, false, false });
                }
            }
            // No change in enabled state → BuildLerpLists handles params
        }
    }

    // Step 2: add FX missing from track
    for (const auto& fxs : ts.fx)
    {
        int slot = FindFX(tr, fxs.name, fxs.paramCount, fxs.slotIndex);
        if (slot >= 0) continue; // already on track, handled above

        slot = TrackFX_AddByName(tr, fxs.name, false, -1000);
        if (slot < 0) continue; // plugin not installed

        // Set all params to target immediately (BuildLerpLists skips these
        // because start==end; they sound correct right away)
        TrackFX_SetEnabled(tr, slot, fxs.enabled);
        for (int p = 0; p < (int)fxs.normVals.size(); ++p)
            TrackFX_SetParamNormalized(tr, slot, p, fxs.normVals[p]);

        // Start at wet=0 and fade in so the plugin enters gracefully
        int wetIdx = TrackFX_GetParamFromIdent(tr, slot, ":wet");
        if (wetIdx >= 0 && fxs.enabled)
        {
            TrackFX_SetParamNormalized(tr, slot, wetIdx, 0.0);
            wetLerps.push_back({ tr, slot, wetIdx, 0.0, 1.0, false, false });
        }
    }
}

// ---------------------------------------------------------------------------
// ApplyImmediate – instant recall (duration == 0)
// ---------------------------------------------------------------------------
void TransitionEngine::ApplyImmediate(const TransitionSnapshot* snap, int mask)
{
    int skippedTracks = 0;

    PreventUIRefresh(1);

    for (const auto& ts : snap->m_tracks)
    {
        MediaTrack* tr = FindTrack(ts.guid);
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
            SyncFXChain(tr, ts, false /*instant*/, dummy);
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
            const GUID& g = order[pass].guid;
            MediaTrack* tr = FindTrack(g);
            if (!tr) continue;
            // Current index of this track
            // IP_TRACKNUMBER returns the value directly as void* (1-based), not a pointer
            int curIdx = (int)(intptr_t)GetSetMediaTrackInfo(tr, "IP_TRACKNUMBER", nullptr) - 1;
            if (curIdx == pass) continue; // already in place
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

    TrackList_AdjustWindows(false);
    PreventUIRefresh(-1);

    if (skippedTracks > 0)
        snprintf(m_statusBuf, sizeof(m_statusBuf),
                 "Done (%d track%s not found)", skippedTracks,
                 skippedTracks != 1 ? "s" : "");
    else
        snprintf(m_statusBuf, sizeof(m_statusBuf), "Done");
}

// ---------------------------------------------------------------------------
// BuildLerpLists – populate m_paramLerps and m_volPanLerps from live state
// ---------------------------------------------------------------------------
void TransitionEngine::BuildLerpLists(const TransitionSnapshot* snap, int mask)
{
    m_paramLerps.clear();
    m_volPanLerps.clear();

    for (const auto& ts : snap->m_tracks)
    {
        MediaTrack* tr = FindTrack(ts.guid);
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
            for (const auto& fxs : ts.fx)
            {
                int slot = FindFX(tr, fxs.name, fxs.paramCount, fxs.slotIndex);
                if (slot < 0) continue;

                for (int p = 0; p < (int)fxs.normVals.size(); p++)
                {
                    double cur = TrackFX_GetParamNormalized(tr, slot, p);
                    double tgt = fxs.normVals[p];
                    if (fabs(cur - tgt) < 1e-7) continue;
                    m_paramLerps.push_back({tr, slot, p, cur, tgt});
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
        double ev = vpl.endVol;
        double ep = vpl.endPan;
        GetSetMediaTrackInfo(vpl.tr, "D_VOL", &ev);
        GetSetMediaTrackInfo(vpl.tr, "D_PAN", &ep);
    }
    for (const auto& pl : m_paramLerps)
        TrackFX_SetParamNormalized(pl.tr, pl.fxIdx, pl.paramIdx, pl.endNorm);

    // Collect plugins to delete (must delete in descending slot order per track)
    struct DelEntry { MediaTrack* tr; int slot; };
    std::vector<DelEntry> toDelete;

    for (const auto& wl : m_wetLerps)
    {
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

    // Delete in descending slot order (so earlier indices aren't invalidated)
    std::sort(toDelete.begin(), toDelete.end(),
              [](const DelEntry& a, const DelEntry& b) {
                  return a.tr == b.tr ? a.slot > b.slot : a.tr > b.tr;
              });
    for (const auto& de : toDelete)
        TrackFX_Delete(de.tr, de.slot);

    m_volPanLerps.clear();
    m_paramLerps.clear();
    m_wetLerps.clear();
}

// ---------------------------------------------------------------------------
// Recall (main entry point)
// ---------------------------------------------------------------------------
void TransitionEngine::Recall(const TransitionSnapshot* snap,
                               int    mask,
                               double duration)
{
    if (!snap) return;

    m_lastRecalledSlot = snap->m_slot;

    // Store taper settings from the snapshot
    m_taper    = (TaperLaw)snap->m_taper;
    m_taperExp = snap->m_taperExp;

    // If a transition is already active, snap it to end first
    if (m_active)
    {
        SnapToEnd();
        plugin_register("-timer", (void*)&TransitionEngine::TimerCallback);
        m_active = false;
    }

    // -----------------------------------------------------------------------
    // Instant path (duration == 0): single tight loop, no timer overhead
    // -----------------------------------------------------------------------
    if (duration <= 0.0)
    {
        ApplyImmediate(snap, mask);
        m_active = false;
        if (onTransitionComplete) onTransitionComplete();
        return;
    }

    // -----------------------------------------------------------------------
    // Timed path: apply discrete params immediately, then build lerp lists
    // -----------------------------------------------------------------------
    m_wetLerps.clear();
    int skippedTracks = 0;

    PreventUIRefresh(1);

    for (const auto& ts : snap->m_tracks)
    {
        MediaTrack* tr = FindTrack(ts.guid);
        if (!tr) { ++skippedTracks; continue; }

        const int safe    = GetEffectiveSafeMask(ts.guid);
        const int effMask = mask & ~safe;

        // Apply discrete params immediately (no lerp for these)
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

        // FX chain sync: add/remove plugins with wet-fade (timed=true)
        if (effMask & (TS_FXPARAMS | TS_FXCHAIN))
            SyncFXChain(tr, ts, true /*timed*/, m_wetLerps);
    }

    // Track reordering – instant, happens before lerp timer starts
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
            MediaTrack* tr = FindTrack(order[pass].guid);
            if (!tr) continue;
            int curIdx = (int)(intptr_t)GetSetMediaTrackInfo(tr, "IP_TRACKNUMBER", nullptr) - 1;
            if (curIdx == pass) continue;
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

    PreventUIRefresh(-1);

    // Build lerp lists for vol/pan/FX params
    BuildLerpLists(snap, mask);

    if (m_paramLerps.empty() && m_volPanLerps.empty() && m_wetLerps.empty())
    {
        snprintf(m_statusBuf, sizeof(m_statusBuf), "Done (no interpolatable params)");
        if (onTransitionComplete) onTransitionComplete();
        return;
    }

    m_duration  = duration;
    m_startTime = time_precise();
    m_active    = true;

    if (skippedTracks > 0)
        snprintf(m_statusBuf, sizeof(m_statusBuf),
                 "Transitioning... (%d track%s not found)",
                 skippedTracks, skippedTracks != 1 ? "s" : "");
    else
        snprintf(m_statusBuf, sizeof(m_statusBuf), "Transitioning...");

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
        double v = lerp(vpl.startVol, vpl.endVol, t);
        double p = lerp(vpl.startPan, vpl.endPan, t);
        GetSetMediaTrackInfo(vpl.tr, "D_VOL", &v);
        GetSetMediaTrackInfo(vpl.tr, "D_PAN", &p);
    }

    // Interpolate FX params
    for (const auto& pl : eng.m_paramLerps)
        TrackFX_SetParamNormalized(pl.tr, pl.fxIdx, pl.paramIdx,
                                   lerp(pl.startNorm, pl.endNorm, t));

    // Interpolate wet/dry for plugin fades (add/remove/disable)
    for (const auto& wl : eng.m_wetLerps)
    {
        if (wl.wetParamIdx >= 0)
            TrackFX_SetParamNormalized(wl.tr, wl.fxSlot, wl.wetParamIdx,
                                       lerp(wl.startWet, wl.endWet, t));
    }

    if (t_raw >= 1.0)
    {
        // Write exact end values and finalise wet lerps (delete/disable)
        eng.SnapToEnd();
        TrackList_AdjustWindows(false);

        plugin_register("-timer", (void*)&TransitionEngine::TimerCallback);
        eng.m_active = false;
        snprintf(eng.m_statusBuf, sizeof(eng.m_statusBuf), "Done");
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
