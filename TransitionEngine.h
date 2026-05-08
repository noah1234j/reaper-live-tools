#pragma once

// reaper_plugin.h (included via api.h) forward-declares MediaTrack,
// but headers that include us before api.h need the forward-decl here.
class MediaTrack;

#include "TransitionSnapshot.h"
#include <vector>
#include <functional>

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

    // Progress [0.0 .. 1.0] for the progress bar; 1.0 when idle
    double      GetProgress()  const;
    bool        IsActive()     const { return m_active; }
    const char* GetStatus()    const { return m_statusBuf; }

    // Called by TransitionWnd when a new "next" slot navigation is requested
    int         GetCurrentSlot() const { return m_lastRecalledSlot; }
    void        SetCurrentSlot(int s)  { m_lastRecalledSlot = s; }

    // Callback registered with plugin_register("timer", ...)
    // – must be a plain static function (no captures)
    static void TimerCallback();

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

    // -----------------------------------------------------------------------
    // Instant path (duration == 0)
    // -----------------------------------------------------------------------
    void ApplyImmediate(const TransitionSnapshot* snap, int mask);

    // -----------------------------------------------------------------------
    // Timer path helpers
    // -----------------------------------------------------------------------

    // Build the lerp lists from current live state → snapshot target
    void BuildLerpLists(const TransitionSnapshot* snap, int mask);

    // Apply exact end values to all lerp entries and finalize wet lerps
    void SnapToEnd();

    // -----------------------------------------------------------------------
    // Static helpers
    // -----------------------------------------------------------------------

    // Find the FX slot on a track matching name+paramCount; hintSlot first
    static int FindFX(MediaTrack* tr,
                      const char* name, int paramCount, int hintSlot);

    // Resolve a GUID to a live MediaTrack*
    static MediaTrack* FindTrack(const GUID& guid);

    // Sync FX chain for timed or instant transitions.
    // In timed mode: collects WetLerp entries instead of deleting/disabling
    // immediately, so plugins can be faded in/out via the wet control.
    static void SyncFXChain(MediaTrack* tr, const TrackState& ts,
                             bool timed, std::vector<WetLerp>& wetLerps);

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    bool   m_active           = false;
    double m_startTime        = 0.0;
    double m_duration         = 0.0;
    int    m_lastRecalledSlot = -1;

    TaperLaw m_taper       = TAPER_SCURVE;
    double   m_taperExp    = 2.0;

    std::vector<ParamLerp>   m_paramLerps;
    std::vector<VolPanLerp>  m_volPanLerps;
    std::vector<WetLerp>     m_wetLerps;

    char   m_statusBuf[256]   = "Idle";
};
