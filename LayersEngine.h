#pragma once
#include "api.h"
#include <windows.h>
#include <map>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// LayerTrack  –  one track entry within a layer
// ---------------------------------------------------------------------------
struct LayerTrack
{
    GUID guid;
    char name[128];   // cached display name
    bool isSpacer;    // true = visual spacer slot, no real track
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
    bool applyMcpVisibility  = true;
    bool hideTcpToo          = false;
    bool reorderTracks       = false;
    bool restoreOnDeactivate = true;

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
    // newLayers, activates activeIdx, saves ext state once.
    void ReplaceAllLayers(const std::vector<LayerDef>& newLayers, int activeIdx);

    // --- Dynamic action dispatch ---
    bool HandleLayerCommand(int cmdId);  // returns true if handled
    void UpdateLayerActionDesc(int idx); // refresh desc after a rename

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

    // Action registration helpers
    void RegisterLayerAction(int idx);
    void UnregisterLayerAction(int uid);
    void RegisterAllActions();
    void UnregisterAllActions();

    std::vector<LayerDef>            m_layers;
    int                              m_activeLayer = -1;
    int                              m_nextUid     = 1;
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
