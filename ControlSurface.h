#pragma once
#include "api.h"
#include <string>
#include <cstdint>

// ---------------------------------------------------------------------------
// Protocols
// ---------------------------------------------------------------------------
enum class CSurfProtocol : int { MCU = 0, HUI = 1 };

// ---------------------------------------------------------------------------
// Pre-baked surface templates
// ---------------------------------------------------------------------------
struct CSurfTemplate
{
    const char*    name;
    CSurfProtocol  proto;
    int            channelCount;  // 8 or 16
    const char*    desc;
};

extern const CSurfTemplate k_csurfTemplates[];
extern const int           k_csurfTemplateCount;

// ---------------------------------------------------------------------------
// Settings – serialized to/from a pipe-delimited config string
// ---------------------------------------------------------------------------
struct CSurfSettings
{
    CSurfProtocol proto         = CSurfProtocol::MCU;
    int           midiInDev     = -1;   // -1 = not configured
    int           midiOutDev    = -1;
    int           templateIdx   = 0;
    int           channelCount  = 8;    // 8 or 16
    bool          followSel     = true; // bank scrolls to selected track
    bool          showVU        = true;
    bool          showNames     = true;
    int           faderMode     = 0;    // 0=Volume, 1=Pan, 2=Sends
    int           bankOffset    = 0;    // initial bank starting track index
    bool          sendColors    = false; // send X-Touch scribble strip colors
    bool          followMCP     = false; // use MCP (mixer) track order

    std::string Serialize()   const;
    static CSurfSettings Deserialize(const char* cfg);
};

// ---------------------------------------------------------------------------
// TransCSurf : IReaperControlSurface
// ---------------------------------------------------------------------------
class TransCSurf : public IReaperControlSurface
{
public:
    explicit TransCSurf(const CSurfSettings& s, int* errStats);
    ~TransCSurf() override;

    // IReaperControlSurface interface
    const char* GetTypeString() override { return "LTCSURF"; }
    const char* GetDescString() override;
    const char* GetConfigString() override;
    void        CloseNoReset() override;

    void Run() override;

    void SetTrackListChange() override;
    void SetSurfaceVolume(MediaTrack* tr, double vol) override;
    void SetSurfacePan(MediaTrack* tr, double pan) override;
    void SetSurfaceMute(MediaTrack* tr, bool mute) override;
    void SetSurfaceSelected(MediaTrack* tr, bool selected) override;
    void SetSurfaceSolo(MediaTrack* tr, bool solo) override;
    void SetSurfaceRecArm(MediaTrack* tr, bool recarm) override;
    void SetPlayState(bool play, bool pause, bool rec) override;
    void SetRepeatState(bool rep) override;
    void SetTrackTitle(MediaTrack* tr, const char* title) override;
    bool GetTouchState(MediaTrack* tr, int isPan) override;
    void SetAutoMode(int mode) override;
    void ResetCachedVolPanStates() override;
    void OnTrackSelection(MediaTrack* tr) override;
    int  Extended(int call, void* p1, void* p2, void* p3) override;

    // Apply new settings at runtime (called from standalone config dialog)
    void ApplyNewSettings(const CSurfSettings& s);

private:
    CSurfSettings m_s;
    mutable std::string m_descStr;
    mutable std::string m_cfgStr;

    midi_Input*  m_midiIn  = nullptr;
    midi_Output* m_midiOut = nullptr;

    int  m_bankOffset = 0;  // first visible track index (1-based REAPER index)

    // Per-strip state (up to 16)
    bool   m_touchState[16]      = {};
    double m_lastFaderMove[16]   = {}; // time_precise() debounce timer
    double m_lastSentVol[16]     = {}; // last fader value sent to surface
    bool   m_lastMute[16]        = {};
    bool   m_lastSolo[16]        = {};
    bool   m_lastRecArm[16]      = {};
    bool   m_lastSelect[16]      = {};

    // Transport LED cache
    bool m_lastPlay   = false;
    bool m_lastPause  = false;
    bool m_lastRec    = false;
    bool m_lastRepeat = false;
    int  m_lastAutoMode = -1;

    // HUI coarse fader bytes
    uint8_t m_huiFaderCoarse[16] = {};
    // HUI zone select state (for button decoding)
    int  m_huiLastZone = -1;

    // VU meter send throttle (only update every ~100ms = every 3 Run() calls)
    int  m_vuThrottle = 0;

    // ---- Private helpers ---------------------------------------------------
    int         GetStripForTrack(MediaTrack* tr) const;
    MediaTrack* GetTrackForStrip(int strip) const;
    int         GetTotalTracks() const;

    void BankLeft();
    void BankRight();
    void ChannelLeft();
    void ChannelRight();
    void ScrollToTrack(MediaTrack* tr);
    void ClampBankOffset();

    void RefreshAll();
    void RefreshTransport();
    void RefreshAutoLEDs(int mode);

    // MCU encode/decode
    void MCU_ProcessMIDI(const MIDI_event_t* ev);
    void MCU_SendFader(int strip, double vol);
    void MCU_SendPanEncoder(int strip, double pan);
    void MCU_SendLED(uint8_t note, bool on);
    void MCU_SendLCD(int charOffset, const char* text, int len);
    void MCU_SendScribble(int strip, const char* topRow, const char* botRow);
    void MCU_SendStripColors(const uint8_t* colors8); // X-Touch scribble color SysEx
    void MCU_SendVU(int strip, int level_0_12);
    void MCU_SendReset();
    void MCU_SetButtonAction(uint8_t note, bool down);

    // HUI encode/decode
    void HUI_ProcessMIDI(const MIDI_event_t* ev);
    void HUI_SendFader(int strip, double vol);
    void HUI_SendLED(int zone, int port, bool on);
    void HUI_SendKeepAlive();

    // Low-level MIDI send
    void SendMIDI(uint8_t b0, uint8_t b1, uint8_t b2);
    void SendSysEx(const uint8_t* data, int len);

    // Volume/dB helpers (matching MCU 14-bit scale)
    static int    VolToFader14(double vol);
    static double Fader14ToVol(int fader14);
};

// ---------------------------------------------------------------------------
// Factory registration / unregistration (called from ReaperPluginEntry)
// ---------------------------------------------------------------------------
void CSurf_Register(reaper_plugin_info_t* rec);
void CSurf_Unregister(reaper_plugin_info_t* rec);

// Called once from ReaperPluginEntry so dialogs can load resource from the DLL
void CSurfConfigDlg_SetInstance(HINSTANCE hInst);

// Apply settings to the currently running surface instance (no-op if none active)
void CSurf_ApplySettings(const CSurfSettings& s);

// Get the current config string from the running instance (empty string if none)
std::string CSurf_GetCurrentConfig();

// Open a standalone (modal) settings dialog from the Extensions menu
void CSurf_ShowStandaloneConfig(HWND parent);
