#pragma once
#include "api.h"
#ifdef _WIN32
#  include <windows.h>
#else
#  include "WDL/swell/swell.h"
#endif
#include <map>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// LayerTrack  –  one track entry within a layer
// ---------------------------------------------------------------------------
struct LayerTrack
{
    GUID guid           = {};
    char name[128]      = {};  // cached display name
    bool isSpacer       = false; // true = visual spacer slot, no real track
    int  folderCompact  = 0;   // I_FOLDERCOMPACT value (0=open, 1=compact, 2=closed)

    // Where this channel shows up when the layer is recalled.
    //
    // Layers used to drive one panel picked globally ("Layers control:
    // MCP/TCP"), so every track in every layer went to the same place and a
    // layer could not, say, keep a submix on the mixer while leaving it out of
    // the track panel. The choice is per track now and both panels are written
    // on every recall, which is what the TCP/MCP columns in the Layers window
    // edit. Both default to true: that is what every track captured before
    // these flags existed meant, and what an unqualified "add this channel"
    // still means.
    bool showTcp        = true;
    bool showMcp        = true;
};

// ---------------------------------------------------------------------------
// LayerDef  –  one named layer containing an ordered list of tracks
// ---------------------------------------------------------------------------
struct LayerDef
{
    char                  name[64];
    std::vector<LayerTrack> tracks;
    int                   maxChannels;  // 0 = show all tracks in layer
    int                   uid;          // stable unique ID

    LayerDef();
};

// ---------------------------------------------------------------------------
// LayersSettings  –  global behavior settings
// ---------------------------------------------------------------------------
struct LayersSettings
{
    // Master switch for the whole visibility pass. Off means a layer recall
    // touches neither panel (reordering and spacers still run).
    //
    // There used to be two more settings beside it — a "Layers control:
    // MCP/TCP" radio pair and "Also hide in the other panel" — which between
    // them decided which panel a layer drove. Both are gone: a layer now
    // always writes the TCP and the MCP together, and which of the two a given
    // channel appears in is stored per track on LayerTrack::showTcp/showMcp.
    bool applyMcpVisibility  = true;
    bool reorderTracks       = false;
    bool restoreOnDeactivate = true;
    int  globalMaxChannels   = 0;   // 0 = unlimited; applies to all layers
    bool triggerMcpSelect    = false; // briefly select/deselect first MCP track to refresh surfaces

    // Whether activating a layer applies the layer's visual spacers.
    //
    // REAPER has a single I_SPACER flag per track and draws it in both the TCP
    // and the MCP, so a layer cannot give the two panels different spacers —
    // managing them for the target panel necessarily rewrites them in the
    // other one. Off by default, so layers leave every spacer alone and a
    // project's own spacers are never collateral damage of a layer change.
    // Layers still record spacers when captured; turning this on applies them.
    bool manageSpacers       = false;

    void Load();
    void Save() const;
};

// ---------------------------------------------------------------------------
// LayersEngine  –  singleton
// ---------------------------------------------------------------------------
class LayersEngine
{
public:
    static LayersEngine& Get();

    void Init();
    void Cleanup();

    // --- Layer data access ---
    LayerDef&       GetLayer(int i)       { return m_layers[i]; }
    const LayerDef& GetLayer(int i) const { return m_layers[i]; }
    int             GetLayerCount() const { return (int)m_layers.size(); }
    int             GetActiveLayer() const { return m_activeLayer; }

    // --- Stable UID access ---
    // Layer UIDs survive renames, reordering and scene recall. Prefer them
    // over indices whenever a layer reference has to outlive the current
    // in-memory layer list (scene recall, registered actions, persistence).
    int  GetLayerUid(int idx) const
    { return (idx >= 0 && idx < (int)m_layers.size()) ? m_layers[idx].uid : 0; }
    int  FindLayerByUid(int uid) const;   // returns index, or -1
    bool ActivateLayerByUid(int uid);     // returns false if uid is unknown
    int  AllocUid() { return m_nextUid++; }  // mint a uid for an unnumbered layer

    // --- Activation ---
    void ActivateLayer(int idx);
    void Deactivate();
    void NextLayer();
    void PrevLayer();

    // --- Layer management ---
    int  AddLayer(const char* name);        // appends a new layer, returns its index
    int  AddSpacerTrack(int layerIdx);      // appends a spacer slot to a layer's track list
    void RemoveLayer(int idx);
    void MoveLayer(int from, int to);

    // Bulk-replace all layers (for scene recall). Rebuilds layer list from
    // newLayers, activates the layer whose uid is activeUid, saves ext state
    // once. Each entry's uid is preserved when non-zero so action bindings
    // and scene layer references survive the replace; entries with uid <= 0
    // get a freshly minted one. Pass activeUid <= 0 for "activate nothing".
    void ReplaceAllLayers(const std::vector<LayerDef>& newLayers, int activeUid);

    // --- Dynamic action dispatch ---
    bool HandleLayerCommand(int cmdId);  // returns true if handled
    void UpdateLayerActionDesc(int idx); // refresh desc after a rename

    // --- Active-layer helpers ---
    // Note that a layer's track layout — its track order and its spacer rows —
    // was edited in the Layers window rather than in REAPER. A window edit is
    // stored only, never pushed into the project until the layer is recalled,
    // so until that recall the layer and the project disagree on purpose and
    // the timer's sync-from-REAPER pass must not "fix" the layer back to what
    // the project looks like. Cleared on the next recall, which is the thing
    // that makes the edit live.
    void MarkLayoutEdited(int idx);

    // --- Timer (registered with plugin_register("timer",...)) ---
    static void TimerCallback();

    // --- Settings ---
    LayersSettings&       GetSettings()       { return m_settings; }
    const LayersSettings& GetSettings() const { return m_settings; }
    void SetSettings(const LayersSettings& s);

    // --- Persistence ---
    void SaveExtState();
    void LoadExtState();

    // Project-specific persistence (project_config_extension_t hooks)
    void SaveConfig(ProjectStateContext* ctx);
    bool ProcessLine(const char* line, ProjectStateContext* ctx);
    void ResetForProject();

    // --- Track name refresh ---
    void RefreshTrackNames(int layerIdx);
    void RefreshAllTrackNames();

    // --- GUID helpers ---
    static void GuidToStr(const GUID& g, char out[40]);
    static bool StrToGuid(const char* s, GUID& out);

private:
    LayersEngine();
    LayersEngine(const LayersEngine&) = delete;

    void DoApplyLayer(int idx);
    void RestoreAllVisible();
    void SyncLayerOrderFromReaper(int idx);  // update layer order to match REAPER track positions

    // Action registration helpers
    void RegisterLayerAction(int idx);
    void UnregisterLayerAction(int uid);
    void RegisterAllActions();
    void UnregisterAllActions();

    std::vector<LayerDef>            m_layers;
    int                              m_activeLayer    = -1;
    int                              m_pendingEditUid = 0;   // uid of a layer edited in the window, not yet recalled
    int                              m_nextUid        = 1;
    int                              m_lastStateCount = -1;  // for timer-based sync
    int                              m_suppressCooldown = 0; // ticks to skip after apply
    LayersSettings                   m_settings;

    // Stable storage for REAPER action pointers
    // (std::map nodes don't move on insert/erase, so c_str() stays valid)
    std::map<int, std::string>        m_cmdStrs;   // uid → "LT_LAYER_UID_NNNN"
    std::map<int, std::string>        m_cmdDescs;  // uid → display description
    std::map<int, gaccel_register_t>  m_accels;    // uid → registered gaccel
    std::map<int, int>                m_cmdIds;    // uid → REAPER command ID
    std::map<int, int>                m_cmdToUid;  // cmdId → uid
};

// Called from ReaperPluginEntry
void LayersEngine_Init();
void LayersEngine_Cleanup();
