#pragma once

#ifdef _WIN32
#  include <windows.h>
#else
#  include "WDL/swell/swell.h"
#endif

#include <string>
#include <vector>
#include <ctime>
#include <unordered_map>

class ProjectStateContext; // defined in reaper_plugin.h

// ---------------------------------------------------------------------------
// GUID hash/equality for unordered containers
// (Identical definitions also live in LayersEngine.cpp as local types.)
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

// ---------------------------------------------------------------------------
// Mask bits – map 1:1 to the UI checkboxes
// ---------------------------------------------------------------------------
static const int TS_VOL         = 0x001;
static const int TS_PAN         = 0x002;
static const int TS_MUTE        = 0x004;
static const int TS_SOLO        = 0x008;
static const int TS_FXPARAMS    = 0x010;  // per-param interpolation  (live-safe)
static const int TS_SENDS       = 0x020;  // track-to-track sends + hardware outputs
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
static const int TS_LAYERS      = 0x8000; // active layer (LayersEngine)
static const int TS_FXSLOTS     = 0x10000;// TCP/MCP empty-slot positions for FX and sends
                                          // (REAPER v7.75+; purely visual, applied instantly)

// Convenience presets
static const int TS_MIX    = (TS_VOL | TS_PAN | TS_MUTE | TS_SOLO | TS_FXPARAMS | TS_PHASE);
static const int TS_LAYOUT = (TS_VIS | TS_TRACKNAME | TS_TRACKCOLOR | TS_TRACKHEIGHT | TS_TRACKORDER | TS_FXSLOTS);

// Default mask used when capturing a new snapshot (everything useful, live-safe)
static const int TS_CAPTURE_ALL = (TS_MIX | TS_FXCHAIN | TS_LAYOUT | TS_SENDS);

// ---------------------------------------------------------------------------
// Slot-hint capability probe (REAPER v7.75+)
//
// REAPER v7.75 added "allow empty slots in TCP/MCP FX/send lists": FX and sends
// keep a *slot* position independent of their (still dense) chain/send index.
// GetTrackNumSends(tr, 0x10000001) returns 0x10000000 on builds that support
// UI-ordered lists and 0 on older ones, which is a cheaper and more reliable
// probe than parsing GetAppVersion(). Result is cached after the first call.
//
// Writing slot_hint / I_SLOT_HINT on an older build is harmless (the parameter
// is simply not recognised), so this gates UI affordances and pointless work
// rather than protecting against a crash.
// ---------------------------------------------------------------------------
bool LT_SlotHintsSupported();

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
// SendState – one send or hardware output captured per track
// ---------------------------------------------------------------------------
struct SendState
{
    bool   isHW      = false;  // true = hardware output (category 1)
    GUID   destGuid  = {};     // destination track GUID (track sends only)
    int    hwDstChan = 0;      // hardware send: I_DSTCHAN (identity key)
    int    hwSrcChan = 0;      // hardware send: I_SRCCHAN (source channel routing)
    int    srcChan   = 0;      // track send: I_SRCCHAN (source channel routing)
    int    dstChan   = 0;      // track send: I_DSTCHAN (dest channel routing, e.g. sidechain 3&4)
    double vol       = 1.0;    // D_VOL
    double pan       = 0.0;    // D_PAN
    bool   mute      = false;  // B_MUTE
    int    sendMode  = 0;      // I_SENDMODE (0=post-fader, 1=pre-fx, 3=pre-fader)

    // TCP/MCP slot position (I_SLOT_HINT, REAPER v7.75+). -1 = unhinted, which is
    // also what pre-7.75 REAPER and pre-slot-support snapshots yield. Track sends
    // and hardware outputs share ONE slot space in the panel list.
    int    slotHint  = -1;
};

// ---------------------------------------------------------------------------
// FXState – per-plugin state
// ---------------------------------------------------------------------------
struct FXState
{
    char   name[256]  = {};  // display name (fallback identity for old snapshots)
    char   fxIdent[512] = {}; // precise plugin identity e.g. "VST3:{GUID}:Name"
    int    slotIndex  = 0;   // captured chain index (fast-path identity hint)

    // TCP/MCP slot position ("slot_hint", REAPER v7.75+). Distinct from slotIndex:
    // the chain index stays dense, the slot is where the plugin sits in the panel
    // grid when "allow empty slots" is on. -1 = unhinted (also the value for
    // pre-7.75 REAPER and snapshots saved before slot support).
    int    slotHint   = -1;
    int    paramCount = 0;   // identity key (fallback, with name)
    bool   enabled    = true;// FX bypass state

    // Normalized [0..1] parameter values; size == paramCount.
    // Empty only for ChunkRecallList plugins (chunk is the sole source of state).
    std::vector<double> normVals;

    // Wet/dry mix (REAPER per-FX wet control, accessed via ":wet" ident)
    double wetVal = 1.0;

    // Full VST state blob (base64 vst_chunk). Non-empty for:
    //   (a) ChunkRecallList plugins — normVals is always empty here;
    //       recall uses vst_chunk only.
    //   (b) All other plugins — vst_chunk captured alongside normVals
    //       so g_chunkAllInstant is a pure recall-time switch (no re-save needed).
    // Instant path uses vst_chunk when g_chunkAllInstant is on (or normVals empty),
    // and falls back to normVals if the chunk write fails.
    // Timed path ignores fxChunk when normVals is populated (lerps via params).
    // Persisted as a FXCHUNKSTART/FXCHUNKEND multi-line block (see Serialize);
    // a blob whose reassembled length mismatches the header is discarded on
    // load rather than kept corrupt.
    std::string fxChunk;
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

    // Whole-chain FX bypass (I_FXEN) – distinct from per-plugin FXState::enabled.
    // Only meaningful when TS_FXPARAMS was set at capture time.
    bool   fxChainEnabled = true;

    // FX states (only populated when TS_FXPARAMS was set at capture time)
    std::vector<FXState> fx;

    // Optional offline FX chain chunk (only populated when TS_FXCHAIN was set)
    std::string fxChainChunk;

    // Sends (only populated when TS_SENDS was set at capture time)
    std::vector<SendState> sends;
};

// ---------------------------------------------------------------------------
// GUIDLess – strict-weak-ordering comparator for GUID keys in std::map
// ---------------------------------------------------------------------------
#include <map>
struct GUIDLess {
    bool operator()(const GUID& a, const GUID& b) const {
        return memcmp(&a, &b, sizeof(GUID)) < 0;
    }
};

// Convenience alias: GUID → MediaTrack* map used by TransitionEngine and
// TransitionWnd (DoPrewarm) so both can build the same track-lookup table.
class MediaTrack;
using TrackGUIDMap = std::map<GUID, MediaTrack*, GUIDLess>;

// ---------------------------------------------------------------------------
// CapturedLayerTrack / CapturedLayer – full layer snapshot stored per scene
// ---------------------------------------------------------------------------
struct CapturedLayerTrack
{
    GUID guid     = {};
    bool isSpacer = false;
};

struct CapturedLayer
{
    std::string                     name;
    std::vector<CapturedLayerTrack> tracks;
    int                             maxChannels = 0;
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
    bool        m_isSpacer = false;  // if true, this row is a visual separator with no track data
    int         m_slot     = 0;
    std::string m_name;
    std::string m_notes;
    int         m_mask     = 0;
    int         m_time     = 0;    // Unix timestamp (capture time)

    // Per-snapshot transition settings
    double      m_duration = 2.0;
    int         m_taper    = TAPER_SCURVE;
    double      m_taperExp = 2.0;  // used when m_taper == TAPER_CUSTOM

    // Layer assignment – active layer index at capture time (-1 = none)
    int         m_layerIdx = -1;

    // Full layer state snapshot (all layers at capture time). When non-empty
    // this is used on recall instead of the bare m_layerIdx above.
    std::vector<CapturedLayer> m_layers;

    std::vector<TrackState> m_tracks;

private:
    // Write a batch of up to 8 normalized param values as one "P ..." line
    static void WriteParamLine(ProjectStateContext* ctx,
                               const std::vector<double>& vals, int start, int end);
};
