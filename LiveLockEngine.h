#pragma once
#include "api.h"
#include <windows.h>
#include <vector>

// ---------------------------------------------------------------------------
// LiveLockSettings  -  which categories to protect and how
// ---------------------------------------------------------------------------
struct LiveLockSettings
{
    bool lockRouting      = true;   // protect track-send vol/pan/mute (cat 0)
    bool lockSelectedOnly = false;  // only tracks selected at engage time
    bool lockHardwareOut  = true;   // protect hardware output sends (cat 1)
    bool lockMasterSend   = true;   // protect B_MAINSEND toggle per track
    bool lockFxBypass     = true;   // protect FX enabled/bypassed states
    bool lockRecArm       = true;   // protect I_RECARM + I_RECMON
    bool requireConfirm   = false;  // ask before accepting a blocked change
    int  intervalMs       = 250;    // enforcement poll rate  50–2000 ms

    void Load();
    void Save() const;
};

// ---------------------------------------------------------------------------
// Per-send snapshot
// ---------------------------------------------------------------------------
struct SendLockState
{
    double vol;
    double pan;
    bool   muted;
};

// ---------------------------------------------------------------------------
// Per-track snapshot
// ---------------------------------------------------------------------------
struct TrackLockState
{
    GUID                       guid;
    std::vector<SendLockState> sends;      // cat 0
    std::vector<SendLockState> hwSends;    // cat 1
    bool                       masterSend; // B_MAINSEND
    std::vector<bool>          fxEnabled;  // one per FX slot
    int                        recArm;     // I_RECARM (int 0/1)
    int                        recMon;     // I_RECMON (int 0/1/2)
};

// ---------------------------------------------------------------------------
// LiveLockEngine  -  singleton enforcement engine
// ---------------------------------------------------------------------------
class LiveLockEngine
{
public:
    static LiveLockEngine& Get();

    // Snapshot current state and start enforcement
    void Engage();
    // Stop enforcement and discard snapshot
    void Disengage();
    // Toggle between Engage and Disengage
    void ToggleLock();

    bool IsLocked()       const { return m_locked; }
    int  GetRevertCount() const { return m_revertCount; }

    const LiveLockSettings& GetSettings() const { return m_settings; }

    // Apply new settings; saves them; re-engages if currently locked
    void SetSettings(const LiveLockSettings& s);

    // Load settings from ExtState (called at plugin init)
    void LoadSettings();

    // Project-specific persistence (project_config_extension_t hooks)
    void SaveConfig(ProjectStateContext* ctx);
    bool ProcessLine(const char* line);
    void ResetSettingsToDefaults();

    // Registered with plugin_register("timer", ...)
    static void TimerCallback();

private:
    LiveLockEngine() = default;
    LiveLockEngine(const LiveLockEngine&) = delete;

    static MediaTrack* FindTrack(const GUID& guid);
    void EnforceNow();

    bool                        m_locked         = false;
    bool                        m_confirmPending = false; // guard against dialog cascade
    int                         m_revertCount    = 0;
    double                      m_lastPollTime   = 0.0;
    std::vector<TrackLockState> m_state;
    LiveLockSettings            m_settings;
};

// Called from ReaperPluginEntry
void LiveLockEngine_Init();
void LiveLockEngine_Cleanup();
