// ---------------------------------------------------------------------------
// LiveLockEngine.cpp  -  Live Safe Locking enforcement engine
//
// Captures a reference state snapshot of track routing, FX bypass states,
// and record-arm settings when engaged, then on a configurable timer checks
// for deviations and reverts them.  If requireConfirm is set, shows a
// MessageBox before accepting a deviation; otherwise silently reverts.
//
// Threading: all calls must be on the REAPER main thread (timer fires there).
// ---------------------------------------------------------------------------
#include "LiveLockEngine.h"
#include "api.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const char* k_Sect = "reaper_transitions";

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------
void LiveLockSettings::Load()
{
    auto getB = [](const char* key, bool def) -> bool {
        const char* v = GetExtState(k_Sect, key);
        return (v && v[0]) ? (atoi(v) != 0) : def;
    };
    auto getI = [](const char* key, int def, int lo, int hi) -> int {
        const char* v = GetExtState(k_Sect, key);
        if (!v || !v[0]) return def;
        int n = atoi(v);
        return (n < lo) ? lo : (n > hi) ? hi : n;
    };

    lockRouting      = getB("ll_routing",    true);
    lockSelectedOnly = getB("ll_selonly",     false);
    lockHardwareOut  = getB("ll_hwout",       true);
    lockMasterSend   = getB("ll_mastersend",  true);
    lockFxBypass     = getB("ll_fxbypass",    true);
    lockRecArm       = getB("ll_recarm",      true);
    requireConfirm   = getB("ll_confirm",     false);
    intervalMs       = getI("ll_intervalms",  250, 50, 2000);
}

void LiveLockSettings::Save() const
{
    // Settings are project-specific; persisted via SaveExtensionConfig -> SaveConfig.
    MarkProjectDirty(nullptr);
}

// ---------------------------------------------------------------------------
// Low-level helpers
// ---------------------------------------------------------------------------
static inline double getSendD(MediaTrack* tr, int cat, int idx, const char* parm)
{
    void* p = GetSetTrackSendInfo(tr, cat, idx, parm, nullptr);
    return p ? *(double*)p : 0.0;
}
static inline bool getSendB(MediaTrack* tr, int cat, int idx, const char* parm)
{
    void* p = GetSetTrackSendInfo(tr, cat, idx, parm, nullptr);
    return p ? *(bool*)p : false;
}
static inline void setSendD(MediaTrack* tr, int cat, int idx, const char* parm, double v)
{
    GetSetTrackSendInfo(tr, cat, idx, parm, &v);
}
static inline void setSendB(MediaTrack* tr, int cat, int idx, const char* parm, bool v)
{
    GetSetTrackSendInfo(tr, cat, idx, parm, &v);
}
static inline bool getTrackBool(MediaTrack* tr, const char* parm)
{
    void* p = GetSetMediaTrackInfo(tr, parm, nullptr);
    return p ? *(bool*)p : false;
}
static inline void setTrackBool(MediaTrack* tr, const char* parm, bool v)
{
    GetSetMediaTrackInfo(tr, parm, &v);
}
static inline int getTrackInt(MediaTrack* tr, const char* parm)
{
    void* p = GetSetMediaTrackInfo(tr, parm, nullptr);
    return p ? *(int*)p : 0;
}
static inline void setTrackInt(MediaTrack* tr, const char* parm, int v)
{
    GetSetMediaTrackInfo(tr, parm, &v);
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
LiveLockEngine& LiveLockEngine::Get()
{
    static LiveLockEngine s_inst;
    return s_inst;
}

// ---------------------------------------------------------------------------
// FindTrack  -  resolve a GUID to a live MediaTrack*
// ---------------------------------------------------------------------------
MediaTrack* LiveLockEngine::FindTrack(const GUID& guid)
{
    const int n = GetNumTracks();
    for (int i = 0; i < n; ++i)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;
        GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
        if (pg && IsEqualGUID(*pg, guid)) return tr;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// CaptureTrack  -  snapshot one track's lockable state
// ---------------------------------------------------------------------------
static TrackLockState CaptureTrack(MediaTrack* tr, const LiveLockSettings& s)
{
    TrackLockState ts;
    GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
    ts.guid = pg ? *pg : GUID{};
    ts.masterSend = false;
    ts.recArm     = 0;
    ts.recMon     = 0;

    if (s.lockRouting)
    {
        const int ns = GetTrackNumSends(tr, 0);
        ts.sends.resize(ns);
        for (int i = 0; i < ns; ++i)
        {
            ts.sends[i].vol   = getSendD(tr, 0, i, "D_VOL");
            ts.sends[i].pan   = getSendD(tr, 0, i, "D_PAN");
            ts.sends[i].muted = getSendB(tr, 0, i, "B_MUTE");
        }
    }

    if (s.lockHardwareOut)
    {
        const int nh = GetTrackNumSends(tr, 1);
        ts.hwSends.resize(nh);
        for (int i = 0; i < nh; ++i)
        {
            ts.hwSends[i].vol   = getSendD(tr, 1, i, "D_VOL");
            ts.hwSends[i].pan   = getSendD(tr, 1, i, "D_PAN");
            ts.hwSends[i].muted = getSendB(tr, 1, i, "B_MUTE");
        }
    }

    if (s.lockMasterSend)
        ts.masterSend = getTrackBool(tr, "B_MAINSEND");

    if (s.lockFxBypass)
    {
        const int nfx = TrackFX_GetCount(tr);
        ts.fxEnabled.resize(nfx);
        for (int i = 0; i < nfx; ++i)
            ts.fxEnabled[i] = TrackFX_GetEnabled(tr, i);
    }

    if (s.lockRecArm)
    {
        ts.recArm = getTrackInt(tr, "I_RECARM");
        ts.recMon = getTrackInt(tr, "I_RECMON");
    }

    return ts;
}

// ---------------------------------------------------------------------------
// Engage
// ---------------------------------------------------------------------------
void LiveLockEngine::Engage()
{
    m_state.clear();
    m_revertCount = 0;

    const int n = GetNumTracks();
    for (int i = 0; i < n; ++i)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;
        if (m_settings.lockSelectedOnly && getTrackInt(tr, "I_SELECTED") == 0)
            continue;
        m_state.push_back(CaptureTrack(tr, m_settings));
    }

    m_locked = true;
}

// ---------------------------------------------------------------------------
// Disengage
// ---------------------------------------------------------------------------
void LiveLockEngine::Disengage()
{
    m_locked = false;
    m_state.clear();
    m_revertCount = 0;
}

// ---------------------------------------------------------------------------
// ToggleLock
// ---------------------------------------------------------------------------
void LiveLockEngine::ToggleLock()
{
    if (m_locked) Disengage();
    else          Engage();
}

// ---------------------------------------------------------------------------
// SetSettings
// ---------------------------------------------------------------------------
void LiveLockEngine::SetSettings(const LiveLockSettings& s)
{
    m_settings = s;
    m_settings.Save();
    if (m_locked) Engage(); // re-capture under new settings
}

// ---------------------------------------------------------------------------
// LoadSettings
// ---------------------------------------------------------------------------
void LiveLockEngine::LoadSettings()
{
    m_settings.Load();
}

// ---------------------------------------------------------------------------
// EnforceNow  -  compare live state vs snapshot, revert deviations
// ---------------------------------------------------------------------------
void LiveLockEngine::EnforceNow()
{
    if (m_confirmPending) return;

    char firstDesc[256] = {};
    int  violations     = 0;

    for (auto& ts : m_state)
    {
        MediaTrack* tr = FindTrack(ts.guid);
        if (!tr) continue;

        // ---- Routing (track sends, cat 0) ---------------------------------
        if (m_settings.lockRouting)
        {
            const int ns = (std::min)((int)ts.sends.size(), GetTrackNumSends(tr, 0));
            for (int i = 0; i < ns; ++i)
            {
                double vol   = getSendD(tr, 0, i, "D_VOL");
                double pan   = getSendD(tr, 0, i, "D_PAN");
                bool   muted = getSendB(tr, 0, i, "B_MUTE");
                if (vol != ts.sends[i].vol || pan != ts.sends[i].pan || muted != ts.sends[i].muted)
                {
                    setSendD(tr, 0, i, "D_VOL",  ts.sends[i].vol);
                    setSendD(tr, 0, i, "D_PAN",  ts.sends[i].pan);
                    setSendB(tr, 0, i, "B_MUTE", ts.sends[i].muted);
                    if (!firstDesc[0])
                        snprintf(firstDesc, sizeof(firstDesc), "Send %d routing was modified.", i + 1);
                    ++violations;
                }
            }
        }

        // ---- Hardware outputs (cat 1) -------------------------------------
        if (m_settings.lockHardwareOut)
        {
            const int nh = (std::min)((int)ts.hwSends.size(), GetTrackNumSends(tr, 1));
            for (int i = 0; i < nh; ++i)
            {
                double vol   = getSendD(tr, 1, i, "D_VOL");
                double pan   = getSendD(tr, 1, i, "D_PAN");
                bool   muted = getSendB(tr, 1, i, "B_MUTE");
                if (vol != ts.hwSends[i].vol || pan != ts.hwSends[i].pan || muted != ts.hwSends[i].muted)
                {
                    setSendD(tr, 1, i, "D_VOL",  ts.hwSends[i].vol);
                    setSendD(tr, 1, i, "D_PAN",  ts.hwSends[i].pan);
                    setSendB(tr, 1, i, "B_MUTE", ts.hwSends[i].muted);
                    if (!firstDesc[0])
                        snprintf(firstDesc, sizeof(firstDesc), "HW output %d was modified.", i + 1);
                    ++violations;
                }
            }
        }

        // ---- Master send --------------------------------------------------
        if (m_settings.lockMasterSend)
        {
            bool cur = getTrackBool(tr, "B_MAINSEND");
            if (cur != ts.masterSend)
            {
                setTrackBool(tr, "B_MAINSEND", ts.masterSend);
                if (!firstDesc[0])
                    snprintf(firstDesc, sizeof(firstDesc), "Master send was toggled.");
                ++violations;
            }
        }

        // ---- FX bypass states --------------------------------------------
        if (m_settings.lockFxBypass)
        {
            const int nfx = (std::min)((int)ts.fxEnabled.size(), TrackFX_GetCount(tr));
            for (int i = 0; i < nfx; ++i)
            {
                bool cur = TrackFX_GetEnabled(tr, i);
                if (cur != ts.fxEnabled[i])
                {
                    TrackFX_SetEnabled(tr, i, ts.fxEnabled[i]);
                    if (!firstDesc[0])
                        snprintf(firstDesc, sizeof(firstDesc), "FX %d bypass was modified.", i + 1);
                    ++violations;
                }
            }
        }

        // ---- Record arm / input monitoring --------------------------------
        if (m_settings.lockRecArm)
        {
            int curArm = getTrackInt(tr, "I_RECARM");
            int curMon = getTrackInt(tr, "I_RECMON");
            if (curArm != ts.recArm)
            {
                setTrackInt(tr, "I_RECARM", ts.recArm);
                if (!firstDesc[0])
                    snprintf(firstDesc, sizeof(firstDesc), "Record arm was modified.");
                ++violations;
            }
            if (curMon != ts.recMon)
            {
                setTrackInt(tr, "I_RECMON", ts.recMon);
                ++violations;
            }
        }
    }

    if (violations > 0)
    {
        m_revertCount += violations;

        if (m_settings.requireConfirm && firstDesc[0])
        {
            m_confirmPending = true;
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "Live Lock blocked a change:\n\n%s\n\n"
                     "Yes = Accept change (update locked reference)\n"
                     "No  = Keep reverted",
                     firstDesc);
            HWND parent = GetMainHwnd ? GetMainHwnd() : nullptr;
            int res = MessageBoxA(parent, msg, "Live Lock",
                                  MB_YESNO | MB_ICONWARNING | MB_TOPMOST);
            m_confirmPending = false;
            if (res == IDYES)
                Engage(); // re-capture accepting the change
        }
    }
}

// ---------------------------------------------------------------------------
// TimerCallback  -  registered with plugin_register("timer", ...)
// ---------------------------------------------------------------------------
void LiveLockEngine::TimerCallback()
{
    LiveLockEngine& eng = Get();
    if (!eng.m_locked) return;

    const double now      = time_precise();
    const double interval = eng.m_settings.intervalMs / 1000.0;
    if (now - eng.m_lastPollTime < interval) return;
    eng.m_lastPollTime = now;

    eng.EnforceNow();
}

// ---------------------------------------------------------------------------
// Project-specific persistence (project_config_extension_t hooks)
// ---------------------------------------------------------------------------
void LiveLockEngine::ResetSettingsToDefaults()
{
    m_settings = LiveLockSettings{};
}

void LiveLockEngine::SaveConfig(ProjectStateContext* ctx)
{
    ctx->AddLine("LTLOCKSET routing=%d selonly=%d hwout=%d mastersend=%d fxbypass=%d recarm=%d confirm=%d intervalms=%d",
                 m_settings.lockRouting      ? 1 : 0,
                 m_settings.lockSelectedOnly ? 1 : 0,
                 m_settings.lockHardwareOut  ? 1 : 0,
                 m_settings.lockMasterSend   ? 1 : 0,
                 m_settings.lockFxBypass     ? 1 : 0,
                 m_settings.lockRecArm       ? 1 : 0,
                 m_settings.requireConfirm   ? 1 : 0,
                 m_settings.intervalMs);
}

bool LiveLockEngine::ProcessLine(const char* line)
{
    if (!line || strncmp(line, "LTLOCKSET ", 10) != 0) return false;

    int r = 1, so = 0, hw = 1, ms = 1, fx = 1, ra = 1, cf = 0, ims = 250;
    sscanf(line + 10,
           "routing=%d selonly=%d hwout=%d mastersend=%d fxbypass=%d recarm=%d confirm=%d intervalms=%d",
           &r, &so, &hw, &ms, &fx, &ra, &cf, &ims);

    m_settings.lockRouting      = (r  != 0);
    m_settings.lockSelectedOnly = (so != 0);
    m_settings.lockHardwareOut  = (hw != 0);
    m_settings.lockMasterSend   = (ms != 0);
    m_settings.lockFxBypass     = (fx != 0);
    m_settings.lockRecArm       = (ra != 0);
    m_settings.requireConfirm   = (cf != 0);
    m_settings.intervalMs       = (ims >= 50 && ims <= 2000) ? ims : 250;
    return true;
}

// ---------------------------------------------------------------------------
// Init / Cleanup
// ---------------------------------------------------------------------------
void LiveLockEngine_Init()
{
    LiveLockEngine::Get().ResetSettingsToDefaults();
}

void LiveLockEngine_Cleanup()
{
    LiveLockEngine::Get().Disengage();
}
