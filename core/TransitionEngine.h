#pragma once

// reaper_plugin.h (included via api.h) forward-declares MediaTrack,
// but headers that include us before api.h need the forward-decl here.
class MediaTrack;

#include "TransitionSnapshot.h"
#include <vector>
#include <map>
#include <functional>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Safes – prevent specific parameter types from being touched during recall.
//
// g_globalSafeMask  : bits that are ALWAYS safe regardless of track.
// g_trackSafes      : per-track overrides; OR'd with globalSafeMask.
//
// g_trackSafesEnabled : master on/off switch for per-track safes.
// g_globalSafeMask bits ARE the on/off for each parameter type globally.
// Bit flags reuse the TS_* constants from TransitionSnapshot.h.
// ---------------------------------------------------------------------------
extern int  g_globalSafeMask;
extern bool g_trackSafesEnabled;

struct TrackSafeEntry {
    GUID guid;
    int  mask; // TS_* bits that are safe on this track
};
extern std::vector<TrackSafeEntry> g_trackSafes;

// Returns per-track effective safe mask (global OR track-specific),
// honouring the enable flags.
int GetEffectiveSafeMask(const GUID& guid);

// ---------------------------------------------------------------------------
// TransitionEngine
//
// Manages timed and instant snapshot recall.
//
// Threading model:
//   All REAPER API calls (including TrackFX_SetParamNormalized) are NOT
//   thread-safe and MUST stay on the REAPER main thread.  The timer
//   callback fires on the main thread at ~30 fps, so the computation
//   (taper + lerp) and writes all happen there.
//
// Live-safety guarantees:
//   • No GetSetObjectState / SetFXChain calls in the hot path.
//   • Offline FX-chain swaps are guarded by GetPlayState() & 4 (recording).
//   • No modal dialogs – errors set m_statusBuf only.
//   • Instant path (duration==0) is a tight single-pass loop with no timer.
// ---------------------------------------------------------------------------
class TransitionEngine
{
public:
    static TransitionEngine& Get();

    // -----------------------------------------------------------------------
    // Main entry point called by the UI.
    //   snap     – target snapshot (caller keeps ownership)
    //   mask     – which parameters to recall (usually snap->m_mask)
    //   duration – transition time in seconds (0 = instant)
    //
    // Taper law and custom exponent are read from snap->m_taper / m_taperExp.
    // -----------------------------------------------------------------------
    void Recall(const TransitionSnapshot* snap, int mask, double duration);

    // Per-recall sub-step timing, populated by the last Recall() call.
    // All values in milliseconds; zero if that phase was skipped.
    struct RecallTimings
    {
        double snapToEnd      = 0.0;  // snap active transition to end
        double buildTrackMap  = 0.0;  // GUID→MediaTrack* map build
        double discreteParams = 0.0;  // mute/solo/vis/name/height/color per track
        double fxChainSync    = 0.0;  // SyncFXChain calls across all tracks
        double sendsSetup     = 0.0;  // send routing add/remove pass
        double trackReorder   = 0.0;  // TS_TRACKORDER reorder pass
        double buildLerpLists = 0.0;  // BuildLerpLists (vol/pan/FX lerp setup)
        double total          = 0.0;  // wall time for entire Recall()

        // Instant-path sub-breakdowns (only collected when g_durationDebug)
        double i_volPan   = 0.0;  // vol/pan/panMode/panLaw across all tracks
        double i_muteSolo = 0.0;  // mute/solo/phase across all tracks
        double i_vis      = 0.0;  // visibility + selection + play-offset
        double i_layout   = 0.0;  // name/color/height across all tracks
        double i_fx       = 0.0;  // total SyncFXChain time (all tracks)
        double i_sends    = 0.0;  // send add/update/remove time (all tracks)
        int    tracksMatched  = 0;    // tracks found in project
        int    tracksSkipped  = 0;    // tracks not found
        int    paramLerps     = 0;    // FX param lerp entries
        int    volPanLerps    = 0;    // vol/pan lerp entries
        int    wetLerps       = 0;    // wet/dry lerp entries
        int    sendLerps      = 0;    // send lerp entries
        bool   instantPath    = false; // true = duration==0

        // Settings that were active at recall time (logged when g_durationDebug)
        bool   s_skipUnchanged    = false;
        bool   s_shadowParams     = false;
        bool   s_chunkAllInstant  = false;
        bool   s_preloadOffline   = false;

        // Per-FX operation timing for the instant path.
        // Only populated when g_durationDebug is true.
        struct FXOpTiming
        {
            std::string fxName;
            double addByName_ms      = 0.0;
            double offlineSandwich_ms = 0.0;
            double setChunk_ms       = 0.0;
            double paramLoop_ms      = 0.0;
            bool   wasNew    = false;
            bool   wasPrimed = false;
        };
        struct TrackFXTiming
        {
            std::string          trackName;
            double               total_ms = 0.0;
            std::vector<FXOpTiming> fxOps;
        };
        std::vector<TrackFXTiming> fxDetail;  // sorted slowest-first
    };
    RecallTimings lastTimings;

    // Progress [0.0 .. 1.0] for the progress bar; 1.0 when idle
    double      GetProgress()  const;
    bool        IsActive()     const { return m_active; }
    const char* GetStatus()    const { return m_statusBuf; }

    // Called by TransitionWnd when a new "next" slot navigation is requested
    int         GetCurrentSlot() const { return m_lastRecalledSlot; }
    void        SetCurrentSlot(int s)  { m_lastRecalledSlot = s; }

    // Immediately stop any in-progress transition: deregisters the timer,
    // clears lerp lists, sets m_active = false.  Call on REAPER main thread.
    void        StopAndReset();

    // Callback registered with plugin_register("timer", ...)
    // – must be a plain static function (no captures)
    static void TimerCallback();

    // -----------------------------------------------------------------------
    // FX Parameter Shadow Map
    // -----------------------------------------------------------------------
    // Tracks the last-written normalized value for each (track GUID, fxIdent,
    // paramIdx) triple.  Populated two ways:
    //   (a) Write-through: after each TrackFX_SetParamNormalized in SyncFXChain.
    //   (b) CSURF_EXT_SETFXPARAM notifications via the FXShadowSurface class.
    // Used during instant recall of VST3 plugins: if the shadow value already
    // matches the target, the SetParamNormalized call (and the plugin DSP
    // recalc it triggers) is skipped.
    //
    // Shadow is per-plugin identified by fxIdent ("fx_ident" named config parm).
    // It is cleared on project load to prevent stale data from a previous project.
    // -----------------------------------------------------------------------
    void ShadowWrite(const GUID& guid, const char* fxIdent, int paramIdx, double val);
    bool ShadowGet  (const GUID& guid, const char* fxIdent, int paramIdx, double& outVal) const;
    void ShadowClear();
    // Drop all entries for one plugin instance. Needed after a vst_chunk
    // write: it changes params without CSURF notifications, so the plugin's
    // shadow entries are provably stale.
    void ShadowInvalidate(const GUID& guid, const char* fxIdent);

    // Register / unregister the internal CSURF surface that feeds the shadow map.
    // Called from ReaperPluginEntry on load and unload.
    static void RegisterShadowSurface();
    static void UnregisterShadowSurface();

    // Optional notify: set by TransitionWnd so the engine can poke UI on finish
    std::function<void()> onTransitionComplete;

private:
    TransitionEngine() = default;
    TransitionEngine(const TransitionEngine&) = delete;

    // -----------------------------------------------------------------------
    // Internal structures
    // -----------------------------------------------------------------------

    // One interpolated FX parameter
    struct ParamLerp {
        MediaTrack* tr;
        int   fxIdx;
        int   paramIdx;
        double startNorm;
        double endNorm;
    };

    // One interpolated vol/pan pair per track
    struct VolPanLerp {
        MediaTrack* tr;
        double startVol, endVol;
        double startPan, endPan;
    };

    // Wet/dry fade for an FX slot (plugin add/remove/disable during transition)
    struct WetLerp {
        MediaTrack* tr;
        int    fxSlot;
        int    wetParamIdx;      // -1 if plugin has no wet control
        double startWet;
        double endWet;
        bool   deleteOnComplete; // TrackFX_Delete when transition ends
        bool   disableOnComplete;// TrackFX_SetEnabled(false) when transition ends
    };

    // Send vol/pan fade (track-to-track or hardware send)
    struct SendLerp {
        MediaTrack* tr;          // source track
        int    sendIdx;          // live send index at lerp build time
        GUID   destGuid;         // used to revalidate sendIdx for track sends
        bool   isHW;             // true = hardware send (category 1)
        int    hwDstChan;        // I_DSTCHAN for HW send revalidation
        double startVol;
        double endVol;
        double startPan;
        double endPan;
        bool   pendingRemove;    // fade to 0 then RemoveTrackSend at t=1
    };

    // -----------------------------------------------------------------------
    // GUID → MediaTrack* lookup map (built once per Recall to avoid O(n²))
    // GUIDLess and TrackGUIDMap are defined in TransitionSnapshot.h
    // -----------------------------------------------------------------------
    using TrackMap = TrackGUIDMap;

    // Build a TrackMap for all tracks in the current project
    static TrackMap BuildTrackMap();

    // -----------------------------------------------------------------------
    // Instant path (duration == 0)
    // -----------------------------------------------------------------------
    void ApplyImmediate(const TransitionSnapshot* snap, int mask, const TrackMap& tmap);

    // -----------------------------------------------------------------------
    // Timer path helpers
    // -----------------------------------------------------------------------

    // Build the lerp lists from current live state → snapshot target
    void BuildLerpLists(const TransitionSnapshot* snap, int mask, const TrackMap& tmap);

    // Apply exact end values to all lerp entries and finalize wet lerps
    void SnapToEnd();

    // -----------------------------------------------------------------------
    // Static helpers
    // -----------------------------------------------------------------------

    // Find the FX slot on a track matching fxIdent (preferred) or name+paramCount;
    // hintSlot checked first as a fast path
    static int FindFX(MediaTrack* tr,
                      const char* name, int paramCount, int hintSlot,
                      const char* fxIdent = "");

    // Resolve a GUID to a live MediaTrack*
    static MediaTrack* FindTrack(const GUID& guid);

    // Sync FX chain for timed or instant transitions.
    // In timed mode: collects WetLerp entries instead of deleting/disabling
    // immediately, so plugins can be faded in/out via the wet control.
    static void SyncFXChain(MediaTrack* tr, const TrackState& ts,
                             bool timed, std::vector<WetLerp>& wetLerps,
                             std::vector<RecallTimings::FXOpTiming>* out_fxOps = nullptr);

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    bool   m_active           = false;
    double m_startTime        = 0.0;
    double m_duration         = 0.0;
    int    m_lastRecalledSlot = -1;
    std::string m_targetSceneName;

    TaperLaw m_taper       = TAPER_SCURVE;
    double   m_taperExp    = 2.0;

    std::vector<ParamLerp>   m_paramLerps;
    std::vector<VolPanLerp>  m_volPanLerps;
    std::vector<WetLerp>     m_wetLerps;
    std::vector<SendLerp>    m_sendLerps;

    char   m_statusBuf[256]   = "Idle";

    // -----------------------------------------------------------------------
    // Shadow map storage
    // Key: track GUID → fxIdent string → per-param values (indexed by param idx)
    // A value of -1e308 (kShadowEmpty) means "not yet observed".
    // -----------------------------------------------------------------------
    using ShadowParamVec = std::vector<double>;
    using ShadowFXMap    = std::unordered_map<std::string, ShadowParamVec>;
    using ShadowTrackMap = std::unordered_map<GUID, ShadowFXMap, GUIDHash, GUIDEqual>;
    ShadowTrackMap m_shadow;
    static constexpr double kShadowEmpty = -1e308;

    // Project state change count observed when the last recall finished.
    // If it differs at the next recall's entry, something external (SWS
    // snapshot, preset load, manual edit REAPER didn't notify us about)
    // touched the project — the shadow map can no longer be trusted and is
    // cleared. Trade-off: any external edit costs one unoptimized recall;
    // back-to-back scene recalls (the optimization's target) keep the map.
    int m_shadowStateCount = -1;
};
