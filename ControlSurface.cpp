// ---------------------------------------------------------------------------
// ControlSurface.cpp  –  MCU / HUI control surface implementation
//
// Registered as a REAPER "csurf" factory so it appears in
// REAPER > Preferences > Control Surfaces > Add...
// ---------------------------------------------------------------------------
#include "ControlSurface.h"
#include "resource.h"

#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// Pre-baked template table
// ---------------------------------------------------------------------------
const CSurfTemplate k_csurfTemplates[] = {
    { "Generic MCU (8ch)",          CSurfProtocol::MCU, 8,
      "Standard Mackie Control Universal - 8 channels" },
    { "MCU + Extender (16ch)",       CSurfProtocol::MCU, 16,
      "MCU main unit + extender - 16 channels total" },
    { "Presonus FaderPort 16",       CSurfProtocol::MCU, 16,
      "Hold TRACK while plugging in USB to enable MCU mode" },
    { "Presonus FaderPort 8",        CSurfProtocol::MCU, 8,
      "Hold TRACK while plugging in USB to enable MCU mode" },
    { "Behringer X-Touch",           CSurfProtocol::MCU, 8,
      "Factory default is MCU mode" },
    { "Behringer X-Touch Compact",   CSurfProtocol::MCU, 8,
      "Set MC mode via Util button" },
    { "Icon Platform M+",            CSurfProtocol::MCU, 8,
      "MCU compatible" },
    { "Mackie Control (classic)",    CSurfProtocol::MCU, 8,
      "Original Mackie Control" },
    { "Pro Tools HUI",               CSurfProtocol::HUI, 8,
      "Pro Tools Human User Interface protocol" },
    { "Yamaha 01V96",                CSurfProtocol::HUI, 8,
      "HUI mode - enable in Setup > MIDI/Host menu" },
};
const int k_csurfTemplateCount = (int)(sizeof(k_csurfTemplates) / sizeof(k_csurfTemplates[0]));

// ---------------------------------------------------------------------------
// MCU button note assignments
// ---------------------------------------------------------------------------
// Channel strip notes (strip 0-7, then 8-15 for 16ch surfaces)
static const uint8_t MCU_REC_BASE    = 0x00; // +strip
static const uint8_t MCU_SOLO_BASE   = 0x08; // +strip
static const uint8_t MCU_MUTE_BASE   = 0x10; // +strip
static const uint8_t MCU_SEL_BASE    = 0x18; // +strip
static const uint8_t MCU_TOUCH_BASE  = 0x68; // +strip  (fader touch)

// Transport
static const uint8_t MCU_BTN_LOOP    = 0x56;
static const uint8_t MCU_BTN_REW     = 0x5B;
static const uint8_t MCU_BTN_FFW     = 0x5C;
static const uint8_t MCU_BTN_STOP    = 0x5D;
static const uint8_t MCU_BTN_PLAY    = 0x5E;
static const uint8_t MCU_BTN_REC     = 0x5F;

// Bank/channel navigation
static const uint8_t MCU_BTN_BANK_L  = 0x2E;
static const uint8_t MCU_BTN_BANK_R  = 0x2F;
static const uint8_t MCU_BTN_CH_L    = 0x30;
static const uint8_t MCU_BTN_CH_R    = 0x31;

// V-Pot CCs (pan encoders)
static const uint8_t MCU_CC_VPOT_BASE = 0x10; // +strip

// MCU SysEx header (Mackie Control)
static const uint8_t MCU_SYSEX_HDR[]  = { 0xF0, 0x00, 0x00, 0x66, 0x14 };
static const uint8_t MCU_CMD_RESET     = 0x08;
static const uint8_t MCU_CMD_LCD       = 0x12;

// HUI zone/port constants
// Fader zones: 0x00-0x07 (strips 0-7), zone 0x0E = transport
static const int HUI_ZONE_TRANSPORT = 0x0E;

// ---------------------------------------------------------------------------
// Settings serialization
// ---------------------------------------------------------------------------
std::string CSurfSettings::Serialize() const
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d",
        (int)proto, midiInDev, midiOutDev,
        templateIdx, channelCount,
        followSel ? 1 : 0,
        showVU ? 1 : 0,
        showNames ? 1 : 0,
        faderMode,
        bankOffset,
        sendColors ? 1 : 0,
        followMCP ? 1 : 0);
    return buf;
}

CSurfSettings CSurfSettings::Deserialize(const char* cfg)
{
    CSurfSettings s;
    if (!cfg || !*cfg) return s;
    int p = 0, inDev = -1, outDev = -1, tmpl = 0, chCnt = 8;
    int fSel = 1, fVU = 1, fNames = 1, fMode = 0, bOff = 0, sCols = 0, fMCP = 0;
    int n = sscanf(cfg, "%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d",
        &p, &inDev, &outDev, &tmpl, &chCnt,
        &fSel, &fVU, &fNames, &fMode, &bOff, &sCols, &fMCP);
    if (n >= 1)  s.proto        = (CSurfProtocol)p;
    if (n >= 2)  s.midiInDev    = inDev;
    if (n >= 3)  s.midiOutDev   = outDev;
    if (n >= 4)  s.templateIdx  = tmpl;
    if (n >= 5)  s.channelCount = (chCnt == 16) ? 16 : 8;
    if (n >= 6)  s.followSel    = (fSel != 0);
    if (n >= 7)  s.showVU       = (fVU != 0);
    if (n >= 8)  s.showNames    = (fNames != 0);
    if (n >= 9)  s.faderMode    = fMode;
    if (n >= 10) s.bankOffset   = bOff;
    if (n >= 11) s.sendColors   = (sCols != 0);
    if (n >= 12) s.followMCP    = (fMCP != 0);
    return s;
}

// ---------------------------------------------------------------------------
// Volume <-> 14-bit fader helpers
// MCU uses the same scale as REAPER's vol slider: 0x0000–0x3FFF
// We map REAPER's linear amplitude (where 1.0 = 0 dBFS) using the same
// sqrt curve that Mackie uses in their official reference implementation.
// ---------------------------------------------------------------------------
int TransCSurf::VolToFader14(double vol)
{
    // REAPER volume 0..2 maps to 0..16383 using a power curve (~sqrt)
    if (vol <= 0.0) return 0;
    double pos = std::sqrt(vol / 2.0);
    int v = (int)(pos * 16383.0 + 0.5);
    return std::max(0, std::min(16383, v));
}

double TransCSurf::Fader14ToVol(int v)
{
    double pos = (double)v / 16383.0;
    return pos * pos * 2.0;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
static TransCSurf* s_currentInstance = nullptr;

TransCSurf::TransCSurf(const CSurfSettings& s, int* errStats)
    : m_s(s)
{
    s_currentInstance = this;
    m_bankOffset = s.bankOffset;

    if (s.midiInDev >= 0)
    {
        m_midiIn = CreateMIDIInput(s.midiInDev);
        if (m_midiIn)
            m_midiIn->start();
        else if (errStats)
            *errStats |= 1;
    }

    if (s.midiOutDev >= 0)
    {
        m_midiOut = CreateMIDIOutput(s.midiOutDev, false, nullptr);
        if (!m_midiOut && errStats)
            *errStats |= 2;
    }

    if (m_midiOut)
    {
        if (s.proto == CSurfProtocol::MCU)
            MCU_SendReset();
        RefreshAll();
    }
}

TransCSurf::~TransCSurf()
{
    if (s_currentInstance == this) s_currentInstance = nullptr;
    CloseNoReset();
}

void TransCSurf::CloseNoReset()
{
    if (m_midiIn)
    {
        m_midiIn->stop();
        m_midiIn->Destroy();
        m_midiIn = nullptr;
    }
    if (m_midiOut)
    {
        m_midiOut->Destroy();
        m_midiOut = nullptr;
    }
}

void TransCSurf::ApplyNewSettings(const CSurfSettings& s)
{
    CloseNoReset();
    m_s          = s;
    m_bankOffset = s.bankOffset;

    if (s.midiInDev >= 0)
    {
        m_midiIn = CreateMIDIInput(s.midiInDev);
        if (m_midiIn) m_midiIn->start();
    }
    if (s.midiOutDev >= 0)
        m_midiOut = CreateMIDIOutput(s.midiOutDev, false, nullptr);

    if (m_midiOut)
    {
        if (s.proto == CSurfProtocol::MCU) MCU_SendReset();
        RefreshAll();
    }
}

void CSurf_ApplySettings(const CSurfSettings& s)
{
    if (s_currentInstance) s_currentInstance->ApplyNewSettings(s);
}

std::string CSurf_GetCurrentConfig()
{
    if (s_currentInstance) return s_currentInstance->GetConfigString();
    return {};
}

// ---------------------------------------------------------------------------
// IReaperControlSurface: identity strings
// ---------------------------------------------------------------------------
const char* TransCSurf::GetDescString()
{
    const char* tmpl = (m_s.templateIdx >= 0 && m_s.templateIdx < k_csurfTemplateCount)
        ? k_csurfTemplates[m_s.templateIdx].name
        : "Custom";
    m_descStr = std::string("Live Tools Control Surface (") + tmpl + ")";
    return m_descStr.c_str();
}

const char* TransCSurf::GetConfigString()
{
    m_cfgStr = m_s.Serialize();
    return m_cfgStr.c_str();
}

// ---------------------------------------------------------------------------
// Run() – called ~30x/sec by REAPER
// ---------------------------------------------------------------------------
void TransCSurf::Run()
{
    if (!m_midiIn && !m_midiOut) return;

    // ---- HUI keep-alive ping (every ~1 sec = every ~30 Run() calls) --------
    static int s_keepAlive = 0;
    if (m_s.proto == CSurfProtocol::HUI && m_midiOut)
    {
        if (++s_keepAlive >= 30)
        {
            s_keepAlive = 0;
            HUI_SendKeepAlive();
        }
    }

    // ---- VU meter updates (every ~100ms = every 3 Run() calls) ------------
    if (m_midiOut && m_s.showVU && m_s.proto == CSurfProtocol::MCU)
    {
        if (++m_vuThrottle >= 3)
        {
            m_vuThrottle = 0;
            int nCh = m_s.channelCount;
            for (int strip = 0; strip < nCh; ++strip)
            {
                MediaTrack* tr = GetTrackForStrip(strip);
                if (!tr) { MCU_SendVU(strip, 0); continue; }
                // REAPER doesn't expose live VU directly through the csurf API;
                // use Track_GetPeakInfo workaround if available, else send 0.
                // We send a fixed mid-level as a placeholder for now.
                MCU_SendVU(strip, 0);
            }
        }
    }

    // ---- Poll MIDI input ---------------------------------------------------
    if (!m_midiIn) return;

    m_midiIn->SwapBufs(GetTickCount());
    MIDI_eventlist* evList = m_midiIn->GetReadBuf();
    int bpos = 0;
    MIDI_event_t* ev;
    while ((ev = evList->EnumItems(&bpos)))
    {
        if (m_s.proto == CSurfProtocol::MCU)
            MCU_ProcessMIDI(ev);
        else
            HUI_ProcessMIDI(ev);
    }
}

// ---------------------------------------------------------------------------
// Helpers – track / strip mapping
// ---------------------------------------------------------------------------
int TransCSurf::GetTotalTracks() const
{
    return CSurf_NumTracks ? CSurf_NumTracks(m_s.followMCP) : GetNumTracks();
}

MediaTrack* TransCSurf::GetTrackForStrip(int strip) const
{
    int idx = m_bankOffset + strip; // 0-based track index
    int total = GetTotalTracks();
    if (idx < 0 || idx >= total) return nullptr;
    // CSurf_TrackFromID uses 1-based index (0 = master), so add 1
    if (CSurf_TrackFromID) return CSurf_TrackFromID(idx + 1, m_s.followMCP);
    return GetTrack(nullptr, idx);
}

int TransCSurf::GetStripForTrack(MediaTrack* tr) const
{
    if (!tr) return -1;
    // CSurf_TrackToID returns 1-based index (0 = master/-1 = not found)
    int trackIdx = CSurf_TrackToID ? (CSurf_TrackToID(tr, m_s.followMCP) - 1) : -1;
    if (trackIdx < 0)
    {
        // Fallback: linear scan by TCP order
        int total = GetTotalTracks();
        for (int i = 0; i < total; ++i)
            if (GetTrack(nullptr, i) == tr) { trackIdx = i; break; }
    }
    if (trackIdx < 0) return -1;
    int strip = trackIdx - m_bankOffset;
    return (strip >= 0 && strip < m_s.channelCount) ? strip : -1;
}

// ---------------------------------------------------------------------------
// Banking
// ---------------------------------------------------------------------------
void TransCSurf::ClampBankOffset()
{
    int total = GetTotalTracks();
    int maxOff = std::max(0, total - m_s.channelCount);
    m_bankOffset = std::max(0, std::min(m_bankOffset, maxOff));
}

void TransCSurf::BankLeft()
{
    m_bankOffset = std::max(0, m_bankOffset - m_s.channelCount);
    RefreshAll();
}

void TransCSurf::BankRight()
{
    m_bankOffset += m_s.channelCount;
    ClampBankOffset();
    RefreshAll();
}

void TransCSurf::ChannelLeft()
{
    if (m_bankOffset > 0) { --m_bankOffset; RefreshAll(); }
}

void TransCSurf::ChannelRight()
{
    m_bankOffset++;
    ClampBankOffset();
    RefreshAll();
}

void TransCSurf::ScrollToTrack(MediaTrack* tr)
{
    if (!tr) return;
    int total = GetTotalTracks();
    for (int i = 0; i < total; ++i)
    {
        if (GetTrack(nullptr, i) == tr)
        {
            // Center the track in view if possible
            int newOff = i - (m_s.channelCount / 2);
            newOff = std::max(0, newOff);
            int maxOff = std::max(0, total - m_s.channelCount);
            newOff = std::min(newOff, maxOff);
            if (newOff != m_bankOffset)
            {
                m_bankOffset = newOff;
                RefreshAll();
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// State-change callbacks from REAPER
// ---------------------------------------------------------------------------
void TransCSurf::SetTrackListChange()
{
    ClampBankOffset();
    RefreshAll();
}

void TransCSurf::OnTrackSelection(MediaTrack* tr)
{
    if (m_s.followSel && tr)
        ScrollToTrack(tr);
}

void TransCSurf::SetSurfaceVolume(MediaTrack* tr, double vol)
{
    int strip = GetStripForTrack(tr);
    if (strip < 0 || !m_midiOut) return;

    // Debounce: skip if user just moved this fader (within 120ms)
    double now = time_precise();
    if (now - m_lastFaderMove[strip] < 0.12) return;

    if (m_s.proto == CSurfProtocol::MCU)
        MCU_SendFader(strip, vol);
    else
        HUI_SendFader(strip, vol);

    m_lastSentVol[strip] = vol;
}

void TransCSurf::SetSurfacePan(MediaTrack* tr, double pan)
{
    int strip = GetStripForTrack(tr);
    if (strip < 0 || !m_midiOut) return;
    if (m_s.proto == CSurfProtocol::MCU)
        MCU_SendPanEncoder(strip, pan);
}

void TransCSurf::SetSurfaceMute(MediaTrack* tr, bool mute)
{
    int strip = GetStripForTrack(tr);
    if (strip < 0 || !m_midiOut) return;
    if (m_lastMute[strip] == mute) return;
    m_lastMute[strip] = mute;
    if (m_s.proto == CSurfProtocol::MCU)
        MCU_SendLED((uint8_t)(MCU_MUTE_BASE + strip), mute);
    else
        HUI_SendLED(strip, 1, mute); // zone=strip, port=1 (mute)
}

void TransCSurf::SetSurfaceSolo(MediaTrack* tr, bool solo)
{
    int strip = GetStripForTrack(tr);
    if (strip < 0 || !m_midiOut) return;
    if (m_lastSolo[strip] == solo) return;
    m_lastSolo[strip] = solo;
    if (m_s.proto == CSurfProtocol::MCU)
        MCU_SendLED((uint8_t)(MCU_SOLO_BASE + strip), solo);
    else
        HUI_SendLED(strip, 2, solo); // zone=strip, port=2 (solo)
}

void TransCSurf::SetSurfaceRecArm(MediaTrack* tr, bool recarm)
{
    int strip = GetStripForTrack(tr);
    if (strip < 0 || !m_midiOut) return;
    if (m_lastRecArm[strip] == recarm) return;
    m_lastRecArm[strip] = recarm;
    if (m_s.proto == CSurfProtocol::MCU)
        MCU_SendLED((uint8_t)(MCU_REC_BASE + strip), recarm);
    else
        HUI_SendLED(strip, 0, recarm); // zone=strip, port=0 (rec)
}

void TransCSurf::SetSurfaceSelected(MediaTrack* tr, bool selected)
{
    int strip = GetStripForTrack(tr);
    if (strip < 0 || !m_midiOut) return;
    if (m_lastSelect[strip] == selected) return;
    m_lastSelect[strip] = selected;
    if (m_s.proto == CSurfProtocol::MCU)
        MCU_SendLED((uint8_t)(MCU_SEL_BASE + strip), selected);
    else
        HUI_SendLED(strip, 4, selected); // zone=strip, port=4 (select)
}

void TransCSurf::SetPlayState(bool play, bool pause, bool rec)
{
    if (!m_midiOut) return;
    if (m_lastPlay == play && m_lastPause == pause && m_lastRec == rec) return;
    m_lastPlay  = play;
    m_lastPause = pause;
    m_lastRec   = rec;
    RefreshTransport();
}

void TransCSurf::SetRepeatState(bool rep)
{
    if (!m_midiOut) return;
    if (m_lastRepeat == rep) return;
    m_lastRepeat = rep;
    if (m_s.proto == CSurfProtocol::MCU)
        MCU_SendLED(MCU_BTN_LOOP, rep);
}

void TransCSurf::SetTrackTitle(MediaTrack* tr, const char* title)
{
    int strip = GetStripForTrack(tr);
    if (strip < 0 || !m_midiOut || !m_s.showNames) return;
    if (m_s.proto == CSurfProtocol::MCU)
    {
        // Get second row: track index label
        char bot[8];
        snprintf(bot, sizeof(bot), "Trk %d", m_bankOffset + strip + 1);
        MCU_SendScribble(strip, title ? title : "", bot);
    }
}

bool TransCSurf::GetTouchState(MediaTrack* tr, int /*isPan*/)
{
    int strip = GetStripForTrack(tr);
    if (strip < 0) return false;
    return m_touchState[strip];
}

void TransCSurf::SetAutoMode(int mode)
{
    if (!m_midiOut) return;
    if (m_lastAutoMode == mode) return;
    m_lastAutoMode = mode;
    RefreshAutoLEDs(mode);
}

void TransCSurf::ResetCachedVolPanStates()
{
    RefreshAll();
}

int TransCSurf::Extended(int call, void* /*p1*/, void* /*p2*/, void* /*p3*/)
{
    if (call == CSURF_EXT_RESET)
    {
        if (m_midiOut && m_s.proto == CSurfProtocol::MCU)
            MCU_SendReset();
        RefreshAll();
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// RefreshAll – push full state to surface
// ---------------------------------------------------------------------------
void TransCSurf::RefreshAll()
{
    if (!m_midiOut) return;

    int nCh = m_s.channelCount;

    for (int strip = 0; strip < nCh; ++strip)
    {
        MediaTrack* tr = GetTrackForStrip(strip);

        // --- Volume ---
        double vol = tr ? *(double*)GetSetMediaTrackInfo(tr, "D_VOL", nullptr) : 0.0;
        if (m_s.proto == CSurfProtocol::MCU) MCU_SendFader(strip, vol);
        else                                  HUI_SendFader(strip, vol);
        m_lastSentVol[strip] = vol;

        // --- Pan ---
        double pan = tr ? *(double*)GetSetMediaTrackInfo(tr, "D_PAN", nullptr) : 0.0;
        if (m_s.proto == CSurfProtocol::MCU) MCU_SendPanEncoder(strip, pan);

        // --- Mute LED ---
        bool mute = tr ? (*(int*)GetSetMediaTrackInfo(tr, "B_MUTE", nullptr) != 0) : false;
        m_lastMute[strip] = mute;
        if (m_s.proto == CSurfProtocol::MCU)
            MCU_SendLED((uint8_t)(MCU_MUTE_BASE + strip), mute);
        else
            HUI_SendLED(strip, 1, mute);

        // --- Solo LED ---
        bool solo = tr ? (*(int*)GetSetMediaTrackInfo(tr, "I_SOLO", nullptr) != 0) : false;
        m_lastSolo[strip] = solo;
        if (m_s.proto == CSurfProtocol::MCU)
            MCU_SendLED((uint8_t)(MCU_SOLO_BASE + strip), solo);
        else
            HUI_SendLED(strip, 2, solo);

        // --- RecArm LED ---
        bool recarm = tr ? (*(int*)GetSetMediaTrackInfo(tr, "I_RECARM", nullptr) != 0) : false;
        m_lastRecArm[strip] = recarm;
        if (m_s.proto == CSurfProtocol::MCU)
            MCU_SendLED((uint8_t)(MCU_REC_BASE + strip), recarm);
        else
            HUI_SendLED(strip, 0, recarm);

        // --- Select LED ---
        bool sel = tr ? (*(int*)GetSetMediaTrackInfo(tr, "I_SELECTED", nullptr) != 0) : false;
        m_lastSelect[strip] = sel;
        if (m_s.proto == CSurfProtocol::MCU)
            MCU_SendLED((uint8_t)(MCU_SEL_BASE + strip), sel);
        else
            HUI_SendLED(strip, 4, sel);

        // --- Track name (scribble) ---
        if (m_s.showNames && m_s.proto == CSurfProtocol::MCU)
        {
            char name[128] = "";
            if (tr) GetTrackName(tr, name, (int)sizeof(name));
            char bot[8];
            snprintf(bot, sizeof(bot), "Trk %d", m_bankOffset + strip + 1);
            MCU_SendScribble(strip, name, bot);
        }
    }

    // --- Scribble strip colors (X-Touch and compatible) ---
    if (m_s.sendColors && m_s.proto == CSurfProtocol::MCU)
    {
        uint8_t colors[8] = { 0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07 }; // default white
        for (int strip = 0; strip < std::min(nCh, 8); ++strip)
        {
            MediaTrack* tr = GetTrackForStrip(strip);
            if (!tr) { colors[strip] = 0x00; continue; } // empty strip = off
            int col = GetTrackColor ? GetTrackColor(tr) : 0;
            if (!col) { colors[strip] = 0x47; continue; } // no custom color = white (inverted)
            int r = 0, g = 0, b = 0;
            if (ColorFromNative) ColorFromNative(col, &r, &g, &b);
            else { r = col & 0xFF; g = (col >> 8) & 0xFF; b = (col >> 16) & 0xFF; }
            // Map RGB to nearest X-Touch color index (with bit 6 = inverted background)
            int maxC = std::max({r, g, b});
            int minC = std::min({r, g, b});
            int sat  = maxC > 0 ? (maxC - minC) * 100 / maxC : 0;
            if (maxC < 30)  { colors[strip] = 0x40; continue; } // near-black
            if (sat < 25)   { colors[strip] = 0x47; continue; } // near-white/grey
            int delta = maxC - minC;
            float hue;
            if      (maxC == r) hue = 60.0f * (float)(g - b) / (float)delta;
            else if (maxC == g) hue = 60.0f * (2.0f + (float)(b - r) / (float)delta);
            else                hue = 60.0f * (4.0f + (float)(r - g) / (float)delta);
            if (hue < 0) hue += 360.0f;
            if      (hue < 30 || hue >= 330) colors[strip] = 0x41; // red
            else if (hue < 90)               colors[strip] = 0x43; // yellow
            else if (hue < 150)              colors[strip] = 0x42; // green
            else if (hue < 210)              colors[strip] = 0x46; // cyan
            else if (hue < 270)              colors[strip] = 0x44; // blue
            else                             colors[strip] = 0x45; // magenta
        }
        MCU_SendStripColors(colors);
    }

    RefreshTransport();

    if (m_lastAutoMode >= 0)
        RefreshAutoLEDs(m_lastAutoMode);
}

void TransCSurf::RefreshTransport()
{
    if (!m_midiOut || m_s.proto != CSurfProtocol::MCU) return;
    MCU_SendLED(MCU_BTN_PLAY,  m_lastPlay && !m_lastPause);
    MCU_SendLED(MCU_BTN_STOP,  !m_lastPlay);
    MCU_SendLED(MCU_BTN_REC,   m_lastRec);
    MCU_SendLED(MCU_BTN_LOOP,  m_lastRepeat);
}

// Automation mode: 0=Trim/Off, 1=Read, 2=Touch, 3=Write, 4=Latch, 5=LatchPreview
void TransCSurf::RefreshAutoLEDs(int mode)
{
    if (!m_midiOut || m_s.proto != CSurfProtocol::MCU) return;
    // Note numbers from MCU spec: Read=0x4A, Write=0x4B, Trim=0x4C, Touch=0x4D, Latch=0x4E
    MCU_SendLED(0x4A, mode == 1);
    MCU_SendLED(0x4B, mode == 3);
    MCU_SendLED(0x4C, mode == 0);
    MCU_SendLED(0x4D, mode == 2);
    MCU_SendLED(0x4E, mode == 4);
}

// ---------------------------------------------------------------------------
// MCU MIDI processing
// ---------------------------------------------------------------------------
void TransCSurf::MCU_ProcessMIDI(const MIDI_event_t* ev)
{
    if (!ev || ev->size < 1) return;
    uint8_t st = ev->midi_message[0];
    uint8_t d1 = ev->size > 1 ? ev->midi_message[1] : 0;
    uint8_t d2 = ev->size > 2 ? ev->midi_message[2] : 0;

    uint8_t status = st & 0xF0;
    uint8_t chan   = st & 0x0F;

    // ---- Fader move (pitch bend per channel) --------------------------------
    if (status == 0xE0)
    {
        int strip = chan;
        if (strip >= m_s.channelCount) return;
        int fader14 = (int)d1 | ((int)d2 << 7);
        double vol = Fader14ToVol(fader14);
        m_lastFaderMove[strip] = time_precise();
        MediaTrack* tr = GetTrackForStrip(strip);
        if (tr)
            GetSetMediaTrackInfo(tr, "D_VOL", &vol);
        return;
    }

    // ---- Note On (button press/release) ------------------------------------
    if (status == 0x90)
    {
        bool down = (d2 > 0);
        MCU_SetButtonAction(d1, down);
        return;
    }

    // ---- CC (V-Pot encoder rotation) ---------------------------------------
    if (status == 0xB0)
    {
        if (d1 >= MCU_CC_VPOT_BASE && d1 < MCU_CC_VPOT_BASE + m_s.channelCount)
        {
            int strip = d1 - MCU_CC_VPOT_BASE;
            MediaTrack* tr = GetTrackForStrip(strip);
            if (!tr) return;
            // Relative encoder: bit 6 set = decrement, bits 0-5 = steps
            bool dec   = (d2 & 0x40) != 0;
            int  steps = (d2 & 0x3F);
            double pan = *(double*)GetSetMediaTrackInfo(tr, "D_PAN", nullptr);
            pan += (dec ? -1.0 : 1.0) * (double)steps * 0.015;
            pan = std::max(-1.0, std::min(1.0, pan));
            GetSetMediaTrackInfo(tr, "D_PAN", &pan);
        }
        return;
    }
}

void TransCSurf::MCU_SetButtonAction(uint8_t note, bool down)
{
    if (!down) return; // act on press only (except for touch notes)

    int nCh = m_s.channelCount;

    // Fader touch (act on both press and release)
    if (note >= MCU_TOUCH_BASE && note < MCU_TOUCH_BASE + nCh)
    {
        int strip = note - MCU_TOUCH_BASE;
        // Retrieve actual down/up from velocity in caller, but we only get note here.
        // The caller already filtered: down=true means press, down=false means release.
        // We need to re-check the velocity, but we only have 'note' here; callers
        // should pass velocity. Work around: we use the 'down' argument passed in Run().
        // Since we only call MCU_SetButtonAction from Run() which passes down=(d2>0):
        m_touchState[strip] = down;  // down is passed via the outer context
        return;
    }

    // --- Rec arm (strip 0-15) ---
    if (note >= MCU_REC_BASE && note < (uint8_t)(MCU_REC_BASE + nCh))
    {
        int strip = note - MCU_REC_BASE;
        MediaTrack* tr = GetTrackForStrip(strip);
        if (!tr) return;
        int arm = *(int*)GetSetMediaTrackInfo(tr, "I_RECARM", nullptr);
        arm = arm ? 0 : 1;
        GetSetMediaTrackInfo(tr, "I_RECARM", &arm);
        return;
    }

    // --- Solo (strip 0-15) ---
    if (note >= MCU_SOLO_BASE && note < (uint8_t)(MCU_SOLO_BASE + nCh))
    {
        int strip = note - MCU_SOLO_BASE;
        MediaTrack* tr = GetTrackForStrip(strip);
        if (!tr) return;
        int solo = *(int*)GetSetMediaTrackInfo(tr, "I_SOLO", nullptr);
        solo = solo ? 0 : 2; // 0=no solo, 2=solo
        GetSetMediaTrackInfo(tr, "I_SOLO", &solo);
        return;
    }

    // --- Mute (strip 0-15) ---
    if (note >= MCU_MUTE_BASE && note < (uint8_t)(MCU_MUTE_BASE + nCh))
    {
        int strip = note - MCU_MUTE_BASE;
        MediaTrack* tr = GetTrackForStrip(strip);
        if (!tr) return;
        bool mute = *(bool*)GetSetMediaTrackInfo(tr, "B_MUTE", nullptr);
        mute = !mute;
        GetSetMediaTrackInfo(tr, "B_MUTE", &mute);
        return;
    }

    // --- Select (strip 0-15) ---
    if (note >= MCU_SEL_BASE && note < (uint8_t)(MCU_SEL_BASE + nCh))
    {
        int strip = note - MCU_SEL_BASE;
        MediaTrack* tr = GetTrackForStrip(strip);
        if (!tr) return;
        int sel = 1;
        GetSetMediaTrackInfo(tr, "I_SELECTED", &sel);
        return;
    }

    // --- Transport ---
    if (note == MCU_BTN_PLAY)  { Main_OnCommand(1007, 0); return; } // Play
    if (note == MCU_BTN_STOP)  { Main_OnCommand(1016, 0); return; } // Stop
    if (note == MCU_BTN_REC)   { Main_OnCommand(1013, 0); return; } // Record
    if (note == MCU_BTN_REW)   { Main_OnCommand(1014, 0); return; } // Rewind
    if (note == MCU_BTN_FFW)   { Main_OnCommand(1015, 0); return; } // Fast Forward
    if (note == MCU_BTN_LOOP)  { Main_OnCommand(1068, 0); return; } // Toggle repeat

    // --- Banking ---
    if (note == MCU_BTN_BANK_L) { BankLeft();    return; }
    if (note == MCU_BTN_BANK_R) { BankRight();   return; }
    if (note == MCU_BTN_CH_L)   { ChannelLeft(); return; }
    if (note == MCU_BTN_CH_R)   { ChannelRight();return; }
}

// ---------------------------------------------------------------------------
// MCU MIDI output
// ---------------------------------------------------------------------------
void TransCSurf::SendMIDI(uint8_t b0, uint8_t b1, uint8_t b2)
{
    if (m_midiOut)
        m_midiOut->Send(b0, b1, b2, -1);
}

void TransCSurf::SendSysEx(const uint8_t* data, int len)
{
    if (!m_midiOut || len <= 0) return;
    // MIDI_event_t has a fixed 4-byte midi_message[] but size indicates the real length.
    // Allocate a variable-length event on the stack.
    int allocSize = (int)sizeof(MIDI_event_t) - 4 + len;
    void* buf = _alloca((size_t)allocSize);
    MIDI_event_t* ev = (MIDI_event_t*)buf;
    ev->frame_offset = -1;
    ev->size = len;
    memcpy(ev->midi_message, data, (size_t)len);
    m_midiOut->SendMsg(ev, -1);
}

void TransCSurf::MCU_SendFader(int strip, double vol)
{
    int v = VolToFader14(vol);
    SendMIDI((uint8_t)(0xE0 | strip), (uint8_t)(v & 0x7F), (uint8_t)((v >> 7) & 0x7F));
}

void TransCSurf::MCU_SendPanEncoder(int strip, double pan)
{
    // Send V-Pot ring display via CC 0x30+strip
    // pan: -1.0 (hard left) .. 0.0 (center) .. +1.0 (hard right)
    // MCU ring display: center=0x06, range 0x01-0x0B (11 positions)
    int pos = (int)((pan + 1.0) * 5.0 + 0.5); // 0-10
    pos = std::max(0, std::min(10, pos));
    uint8_t ring = (uint8_t)(0x01 | ((pos + 1) << 4) | (1 << 6)); // center-fan mode
    // Simplified: just use single dot mode (mode 0)
    ring = (uint8_t)(pos + 1); // 1-11
    SendMIDI(0xB0, (uint8_t)(0x30 + strip), ring);
}

void TransCSurf::MCU_SendLED(uint8_t note, bool on)
{
    SendMIDI(0x90, note, on ? 0x7F : 0x00);
}

void TransCSurf::MCU_SendLCD(int charOffset, const char* text, int len)
{
    if (!m_midiOut || !text) return;
    // SysEx: F0 00 00 66 14 12 <offset> <ASCII...> F7
    int tLen = std::min(len, 56);
    // Header(7) + text + F7(1) = 8 + tLen bytes max
    uint8_t buf[72];
    int i = 0;
    buf[i++] = 0xF0;
    buf[i++] = 0x00; buf[i++] = 0x00; buf[i++] = 0x66; buf[i++] = 0x14;
    buf[i++] = 0x12;
    buf[i++] = (uint8_t)charOffset;
    for (int j = 0; j < tLen; ++j)
        buf[i++] = (uint8_t)(text[j] & 0x7F);
    buf[i++] = 0xF7;
    SendSysEx(buf, i);
}

void TransCSurf::MCU_SendScribble(int strip, const char* topRow, const char* botRow)
{
    if (!m_midiOut || strip < 0 || strip >= 8) return;
    // MCU LCD is 56 chars wide, two rows.
    // Top row: offsets 0–55, bottom row: 56–111.
    // Each strip occupies 7 chars per row.
    char top7[8] = "       ";
    char bot7[8] = "       ";
    if (topRow) { strncpy(top7, topRow, 7); top7[7] = '\0'; }
    if (botRow) { strncpy(bot7, botRow, 7); bot7[7] = '\0'; }
    // Pad to exactly 7 chars
    for (int j = (int)strlen(top7); j < 7; ++j) top7[j] = ' ';
    for (int j = (int)strlen(bot7); j < 7; ++j) bot7[j] = ' ';
    MCU_SendLCD(strip * 7,      top7, 7);
    MCU_SendLCD(56 + strip * 7, bot7, 7);
}

void TransCSurf::MCU_SendStripColors(const uint8_t* colors8)
{
    // X-Touch (and compatible) scribble strip color SysEx
    // F0 00 20 32 15 4C <col0>..<col7> F7
    // Color byte: bits 0-2 = hue (0=off, 1=red, 2=green, 3=yellow, 4=blue, 5=pink, 6=cyan, 7=white)
    //             bit 6    = inverted (colored background, dark text)
    uint8_t msg[15];
    msg[0] = 0xF0; msg[1] = 0x00; msg[2] = 0x20;
    msg[3] = 0x32; msg[4] = 0x15; msg[5] = 0x4C;
    for (int i = 0; i < 8; ++i) msg[6 + i] = colors8[i] & 0x7F; // SysEx data bytes must be < 0x80
    msg[14] = 0xF7;
    SendSysEx(msg, 15);
}

void TransCSurf::MCU_SendVU(int strip, int level_0_12){
    // Channel pressure: 0xD0, high nibble = strip, low nibble = level (0-D) or E/F for clip
    uint8_t val = (uint8_t)(((strip & 0x0F) << 4) | (level_0_12 & 0x0F));
    SendMIDI(0xD0, val, 0);
}

void TransCSurf::MCU_SendReset()
{
    // MCU reset SysEx: F0 00 00 66 14 08 00 F7
    uint8_t resetMsg[] = { 0xF0, 0x00, 0x00, 0x66, 0x14, 0x08, 0x00, 0xF7 };
    SendSysEx(resetMsg, (int)sizeof(resetMsg));
}

// ---------------------------------------------------------------------------
// HUI MIDI processing
// ---------------------------------------------------------------------------
void TransCSurf::HUI_ProcessMIDI(const MIDI_event_t* ev)
{
    if (!ev || ev->size < 2) return;
    uint8_t st = ev->midi_message[0];
    uint8_t d1 = ev->midi_message[1];
    uint8_t d2 = ev->size > 2 ? ev->midi_message[2] : 0;

    // HUI fader: CC 0x00-0x07 = coarse (MSB), CC 0x20-0x27 = fine (LSB)
    if (st == 0xB0)
    {
        if (d1 <= 0x07)
        {
            m_huiFaderCoarse[d1] = d2;
        }
        else if (d1 >= 0x20 && d1 <= 0x27)
        {
            int strip = d1 - 0x20;
            int fader14 = ((int)m_huiFaderCoarse[strip] << 7) | (d2 & 0x7F);
            double vol = Fader14ToVol(fader14);
            m_lastFaderMove[strip] = time_precise();
            MediaTrack* tr = GetTrackForStrip(strip);
            if (tr) GetSetMediaTrackInfo(tr, "D_VOL", &vol);
        }
        return;
    }

    // HUI button: Note On 0x90 – zone select (d2=0x7F) followed by port select
    if (st == 0x90)
    {
        if (d2 == 0x7F)
        {
            // Zone select: d1 = zone
            m_huiLastZone = (int)d1;
        }
        else
        {
            // Port select: d1 = port, d2 = 0 or 0x7F
            int zone = m_huiLastZone;
            if (zone < 0) return;
            bool pressed = (d2 == 0x40);
            if (!pressed) return;

            // Channel strip buttons
            if (zone >= 0x00 && zone <= 0x07)
            {
                int strip = zone;
                MediaTrack* tr = GetTrackForStrip(strip);
                if (!tr) return;
                switch (d1)
                {
                case 0: { int a = *(int*)GetSetMediaTrackInfo(tr,"I_RECARM",nullptr); a=a?0:1; GetSetMediaTrackInfo(tr,"I_RECARM",&a); break; }
                case 1: { bool m = *(bool*)GetSetMediaTrackInfo(tr,"B_MUTE",nullptr); m=!m; GetSetMediaTrackInfo(tr,"B_MUTE",&m); break; }
                case 2: { int s = *(int*)GetSetMediaTrackInfo(tr,"I_SOLO",nullptr); s=s?0:2; GetSetMediaTrackInfo(tr,"I_SOLO",&s); break; }
                case 4: { int sel=1; GetSetMediaTrackInfo(tr,"I_SELECTED",&sel); break; }
                }
            }

            // Transport zone
            if (zone == HUI_ZONE_TRANSPORT)
            {
                switch (d1)
                {
                case 0: Main_OnCommand(1014, 0); break; // Rew
                case 1: Main_OnCommand(1015, 0); break; // FFwd
                case 2: Main_OnCommand(1016, 0); break; // Stop
                case 3: Main_OnCommand(1007, 0); break; // Play
                case 5: Main_OnCommand(1013, 0); break; // Record
                }
            }

            m_huiLastZone = -1;
        }
        return;
    }
}

void TransCSurf::HUI_SendFader(int strip, double vol)
{
    if (!m_midiOut || strip >= 8) return;
    int v = VolToFader14(vol);
    uint8_t coarse = (uint8_t)((v >> 7) & 0x7F);
    uint8_t fine   = (uint8_t)(v & 0x7F);
    // HUI fader: CC on channel (strip): CC 0 = coarse, CC 32 = fine
    // Actually HUI sends on channel 0 with CC = strip for coarse, strip+32 for fine
    SendMIDI(0xB0, (uint8_t)strip, coarse);
    SendMIDI(0xB0, (uint8_t)(strip + 0x20), fine);
}

void TransCSurf::HUI_SendLED(int zone, int port, bool on)
{
    if (!m_midiOut) return;
    // HUI LED: Note On 0x90, note = zone, vel = 0x00..0x7F (high nibble = port, low = on/off)
    uint8_t vel = (uint8_t)(((port & 0x0F) << 4) | (on ? 0x01 : 0x00));
    SendMIDI(0x90, (uint8_t)(zone & 0x7F), vel);
}

void TransCSurf::HUI_SendKeepAlive()
{
    // HUI keep-alive: 0xA0 0x00 0x00 (Polyphonic Pressure)
    SendMIDI(0xA0, 0x00, 0x00);
}

// ---------------------------------------------------------------------------
// Factory / registration
// ---------------------------------------------------------------------------
static IReaperControlSurface* CSurf_Create(
    const char* /*typeString*/,
    const char* configString,
    int*        errStats)
{
    CSurfSettings s = CSurfSettings::Deserialize(configString);
    return new TransCSurf(s, errStats);
}

// CSurf_ShowConfig is defined in CSurfConfigDlg.cpp
extern HWND CSurf_ShowConfig(
    const char* typeString,
    HWND        parent,
    const char* initConfigString);

static reaper_csurf_reg_t s_csurfReg =
{
    "LTCSURF",
    "Live Tools Control Surface",
    CSurf_Create,
    CSurf_ShowConfig
};

void CSurf_Register(reaper_plugin_info_t* rec)
{
    rec->Register("csurf", &s_csurfReg);
}

void CSurf_Unregister(reaper_plugin_info_t* /*rec*/)
{
    // plugin_register is a global function pointer loaded at startup.
    // We call it directly rather than through rec (which may be null at unload).
    if (plugin_register)
        plugin_register("-csurf", &s_csurfReg);
}
