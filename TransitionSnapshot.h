#pragma once

#ifdef _WIN32
#  include <windows.h>
#else
#  include "../sws-master-2.14.0 - Copy/vendor/WDL/WDL/swell/swell.h"
#endif

#include <string>
#include <vector>
#include <ctime>

class ProjectStateContext; // defined in reaper_plugin.h

// ---------------------------------------------------------------------------
// Mask bits – map 1:1 to the UI checkboxes
// ---------------------------------------------------------------------------
static const int TS_VOL         = 0x001;
static const int TS_PAN         = 0x002;
static const int TS_MUTE        = 0x004;
static const int TS_SOLO        = 0x008;
static const int TS_FXPARAMS    = 0x010;  // per-param interpolation  (live-safe)
static const int TS_SENDS       = 0x020;  // reserved – future
static const int TS_VIS         = 0x040;
static const int TS_SELECTION   = 0x080;
static const int TS_PHASE       = 0x100;
static const int TS_PLAY_OFFSET = 0x200;
static const int TS_FXCHAIN     = 0x400;  // offline full-chain swap (not during recording)
// Layout bits (applied instantly, never faded)
static const int TS_TRACKNAME   = 0x0800; // track name
static const int TS_TRACKCOLOR  = 0x1000; // track colour (I_CUSTOMCOLOR)
static const int TS_TRACKHEIGHT = 0x2000; // TCP height override
static const int TS_TRACKORDER  = 0x4000; // track order within the project

// Convenience presets
static const int TS_MIX    = (TS_VOL | TS_PAN | TS_MUTE | TS_SOLO | TS_FXPARAMS | TS_PHASE);
static const int TS_LAYOUT = (TS_VIS | TS_TRACKNAME | TS_TRACKCOLOR | TS_TRACKHEIGHT | TS_TRACKORDER);

// Default mask used when capturing a new snapshot (everything useful, live-safe)
static const int TS_CAPTURE_ALL = (TS_MIX | TS_FXCHAIN | TS_LAYOUT);

// ---------------------------------------------------------------------------
// Taper laws for timed transitions
// ---------------------------------------------------------------------------
enum TaperLaw
{
    TAPER_LINEAR = 0,  // t              – linear fade
    TAPER_SCURVE = 1,  // t²(3-2t)      – smoothstep (default)
    TAPER_LOG    = 2,  // log-scale      – fast start, slow end
    TAPER_EXP    = 3,  // exp-scale      – slow start, fast end
    TAPER_CUSTOM = 4,  // power law t^x  – user-defined exponent
};

// ---------------------------------------------------------------------------
// FXState – per-plugin state
// ---------------------------------------------------------------------------
struct FXState
{
    char   name[256]  = {};  // display name used as identity key (1/2)
    int    slotIndex  = 0;   // captured slot index (fast-path hint)
    int    paramCount = 0;   // identity key (2/2)
    bool   enabled    = true;// FX bypass state

    // Normalized [0..1] parameter values; size == paramCount
    std::vector<double> normVals;
};

// ---------------------------------------------------------------------------
// TrackState – per-track state
// ---------------------------------------------------------------------------
struct TrackState
{
    GUID   guid         = {};

    // Volume / pan
    double vol          = 1.0;
    double pan          = 0.0;
    int    panMode      = 0;
    double width        = 0.0;
    double dualPanL     = 0.0;
    double dualPanR     = 0.0;
    double panLaw       = -1.0;

    // Discrete switches (applied at t=0 even during timed transitions)
    bool   mute         = false;
    int    solo         = 0;
    bool   phase        = false;

    // Visibility  (bit0=TCP, bit1=MCP – stored as int for GetSetMediaTrackInfo)
    int    vis          = 3;
    int    selected     = 0;

    // Play offset
    int    playOffsetFlag = 0;
    double playOffset     = 0.0;

    // Layout (applied instantly)
    std::string trackName;        // P_NAME
    int    color        = 0;      // I_CUSTOMCOLOR (0 = default/unset)
    int    heightOverride = 0;    // I_HEIGHTOVERRIDE (0 = auto)
    bool   heightLocked = false;  // B_HEIGHTLOCK

    // Track order (only meaningful when TS_TRACKORDER is set; stored as
    // the 0-based index in the captured project, used on recall to reorder)
    int    capturedIndex = -1;    // index at capture time

    // FX states (only populated when TS_FXPARAMS was set at capture time)
    std::vector<FXState> fx;

    // Optional offline FX chain chunk (only populated when TS_FXCHAIN was set)
    std::string fxChainChunk;
};

// ---------------------------------------------------------------------------
// TransitionSnapshot – top-level container
// ---------------------------------------------------------------------------
class TransitionSnapshot
{
public:
    TransitionSnapshot(int slot, const char* name);

    // Capture the current REAPER project state into this snapshot
    void Capture(int mask);

    // Project-state persistence (called from SaveExtensionConfig / ProcessExtensionLine)
    void Serialize(ProjectStateContext* ctx) const;
    static TransitionSnapshot* Deserialize(const char* headerLine,
                                           ProjectStateContext* ctx);

    // ---- Data fields -------------------------------------------------------
    int         m_slot     = 0;
    std::string m_name;
    std::string m_notes;
    int         m_mask     = 0;
    int         m_time     = 0;    // Unix timestamp (capture time)

    // Per-snapshot transition settings
    double      m_duration = 2.0;
    int         m_taper    = TAPER_SCURVE;
    double      m_taperExp = 2.0;  // used when m_taper == TAPER_CUSTOM

    std::vector<TrackState> m_tracks;

private:
    // Write a batch of up to 8 normalized param values as one "P ..." line
    static void WriteParamLine(ProjectStateContext* ctx,
                               const std::vector<double>& vals, int start, int end);
};
