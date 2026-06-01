// ---------------------------------------------------------------------------
// MeterBridgeWnd.cpp  –  Live Meter Bridge dockable window
//
// One horizontal channel strip per REAPER track, entirely custom-painted.
// Strips are 52 px wide; the window scrolls horizontally if more tracks
// exist than fit in the client area.
// ---------------------------------------------------------------------------

#include "MeterBridgeWnd.h"
#include "resource.h"
#include "api.h"
#include "TransitionEngine.h"
#include "TransitionSnapshot.h"
#include "TransitionWnd.h"

#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>
#include <windowsx.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const UINT k_TimerID        = 44;       // WM_TIMER id
static const double k_MeterMin     = -60.0;    // dBFS floor
static const double k_MeterMax     =  +6.0;    // dBFS ceiling
static const char* k_ExtSection    = "reaper_transitions";
static const char* k_DockKey       = "meterbridge_docked";

// ---------------------------------------------------------------------------
// User-configurable settings (persisted via GetExtState / SetExtState)
// ---------------------------------------------------------------------------
static int g_mb_StripW        = 14;   // strip width in pixels
static int g_mb_FontSize      =  8;   // positive px; used as -g_mb_FontSize in LOGFONT
static int g_mb_NameH         = 56;   // name zone height in pixels
static int g_mb_PeakHoldTicks = 33;   // ~2 s at 60 ms/tick
static int g_mb_Fps           = 16;   // repaint/poll rate (16 ≈ 60 ms)

static void MB_LoadSettings()
{
    auto readInt = [](const char* key, int def, int lo, int hi) -> int {
        const char* v = GetExtState(k_ExtSection, key);
        if (!v || !*v) return def;
        int val = atoi(v);
        if (val < lo) val = lo;
        if (val > hi) val = hi;
        return val;
    };
    g_mb_StripW        = readInt("mb_strip_w",         14,  6, 80);
    g_mb_FontSize      = readInt("mb_font_size",         8,  4, 32);
    g_mb_NameH         = readInt("mb_name_h",           56, 10,200);
    g_mb_PeakHoldTicks = readInt("mb_peak_hold_ticks",  33,  0,300);
    g_mb_Fps           = readInt("mb_fps",              16,  1, 60);
}

static void MB_SaveSettings()
{
    char buf[32];
    auto saveInt = [&](const char* key, int val) {
        _itoa_s(val, buf, 10);
        SetExtState(k_ExtSection, key, buf, true);
    };
    saveInt("mb_strip_w",         g_mb_StripW);
    saveInt("mb_font_size",       g_mb_FontSize);
    saveInt("mb_name_h",          g_mb_NameH);
    saveInt("mb_peak_hold_ticks", g_mb_PeakHoldTicks);
    saveInt("mb_fps",             g_mb_Fps);
}

// ---------------------------------------------------------------------------
// Per-strip state (one entry per visible track)
// ---------------------------------------------------------------------------
struct StripState
{
    double   peakDb       = -144.0;
    double   peakHoldDb   = -144.0;
    int      peakHoldTick = 0;
    double   faderDb      = 0.0;       // current fader level
    bool     mute         = false;
    int      solo         = 0;         // 0=off, 1=on, 2=solo-defeat
    bool     recArm       = false;
    COLORREF trackColor   = RGB(80, 80, 80);
    char     name[64]     = {};
    int      safeMask     = 0;         // TS_* bits
    bool     hasDelta     = false;     // differs from selected snapshot
    int      deltaFlags   = 0;         // which TS_* bits differ
};

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------
static HINSTANCE    g_hInst    = nullptr;
static HWND         g_wnd      = nullptr;
static HWND         g_hScroll  = nullptr;

static int          s_bankOffset   = 0;   // index of leftmost visible strip
static int          s_visibleCount = 0;   // strips that fit in client area

// Guard: prevents WM_DESTROY from overwriting the dock-state pref during ToggleDocking
static bool g_suppressDockStateSave = false;

static std::vector<StripState> s_strips;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static double AmpToDb(double amp)
{
    if (amp <= 0.0) return -144.0;
    double db = 20.0 * std::log10(amp);
    if (db < -144.0) db = -144.0;
    return db;
}

// Map a dB value to a 0..1 fraction over [k_MeterMin, k_MeterMax]
static double DbToFrac(double db)
{
    if (db <= k_MeterMin) return 0.0;
    if (db >= k_MeterMax) return 1.0;
    return (db - k_MeterMin) / (k_MeterMax - k_MeterMin);
}

// Color of the meter bar segment at a given dB level
static COLORREF MeterColor(double db)
{
    if (db >= -6.0) return RGB(220,  20,  20);  // red
    if (db >= -18.0) return RGB(220, 180,   0); // yellow
    return                  RGB( 20, 160,  20); // green
}

// ---------------------------------------------------------------------------
// Poll all track states (called from WM_TIMER, main thread)
// ---------------------------------------------------------------------------
static void PollStrips()
{
    const int nTracks = GetNumTracks();

    s_strips.resize((size_t)nTracks);

    // Query selected snapshot for delta comparison
    const int selIdx = TransitionWnd_GetSelectedIndex();
    const TransitionSnapshot* snap =
        (selIdx >= 0 && selIdx < (int)g_snapshots.size())
        ? g_snapshots[(size_t)selIdx].get()
        : nullptr;

    for (int i = 0; i < nTracks; ++i)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        StripState& s  = s_strips[(size_t)i];

        if (!tr)
        {
            s = StripState{};
            continue;
        }

        // ---- Peak metering -------------------------------------------------
        // Track_GetPeakInfo returns post-fader peak amplitude.
        // We take the max of left (ch 0) and right (ch 1) channels.
        double ampL = Track_GetPeakInfo(tr, 0);
        double ampR = Track_GetPeakInfo(tr, 1);
        double amp  = (ampL > ampR) ? ampL : ampR;
        double db   = AmpToDb(amp);

        s.peakDb = db;

        // Peak hold
        if (db >= s.peakHoldDb || s.peakHoldTick <= 0)
        {
            s.peakHoldDb   = db;
            s.peakHoldTick = g_mb_PeakHoldTicks;
        }
        else
        {
            s.peakHoldTick--;
            if (s.peakHoldTick <= 0)
                s.peakHoldDb = db; // let it fall after hold expires
        }

        // ---- Fader / mix info ---------------------------------------------
        double* pVol = (double*)GetSetMediaTrackInfo(tr, "D_VOL", nullptr);
        s.faderDb = pVol ? AmpToDb(*pVol * (*pVol >= 0.0 ? 1.0 : -1.0)) : 0.0;
        // D_VOL can be negative for phase-inverted tracks; take abs for dB
        if (pVol) s.faderDb = AmpToDb(std::abs(*pVol));

        bool* pMute = (bool*)GetSetMediaTrackInfo(tr, "B_MUTE", nullptr);
        s.mute = pMute && *pMute;

        int* pSolo = (int*)GetSetMediaTrackInfo(tr, "I_SOLO", nullptr);
        s.solo = pSolo ? *pSolo : 0;

        int* pArm = (int*)GetSetMediaTrackInfo(tr, "I_RECARM", nullptr);
        s.recArm = pArm && (*pArm != 0);

        // ---- Track color ---------------------------------------------------
        // I_CUSTOMCOLOR returns int* — dereference to get the stored COLORREF.
        // Bit 24 = custom color present; low 24 bits = COLORREF (0x00BBGGRR).
        int* pColor = (int*)GetSetMediaTrackInfo(tr, "I_CUSTOMCOLOR", nullptr);
        int col = pColor ? *pColor : 0;
        if (col & 0x1000000)
            s.trackColor = col & 0x00FFFFFF;  // already a valid COLORREF
        else
            s.trackColor = RGB(60, 60, 60);

        // ---- Track name ----------------------------------------------------
        GetTrackName(tr, s.name, (int)sizeof(s.name) - 1);

        // ---- Safe mask -----------------------------------------------------
        GUID* pGuid = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
        s.safeMask = pGuid ? GetEffectiveSafeMask(*pGuid) : 0;

        // ---- Delta vs selected snapshot ------------------------------------
        s.hasDelta   = false;
        s.deltaFlags = 0;

        if (snap && pGuid)
        {
            for (const auto& ts : snap->m_tracks)
            {
                if (memcmp(&ts.guid, pGuid, sizeof(GUID)) == 0)
                {
                    int diff = 0;
                    // Compare volume (with 0.01 dB tolerance)
                    if (pVol && std::abs(ts.vol - *pVol) > 0.001)
                        diff |= TS_VOL;
                    // Compare mute
                    if (pMute && ts.mute != *pMute)
                        diff |= TS_MUTE;
                    // Compare solo
                    if (pSolo && ts.solo != *pSolo)
                        diff |= TS_SOLO;
                    // Pan comparison omitted for brevity (add TS_PAN if desired)
                    if (diff)
                    {
                        s.hasDelta   = true;
                        s.deltaFlags = diff;
                    }
                    break;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Draw one strip
// ---------------------------------------------------------------------------
static void DrawStrip(HDC hdc, int x, int clientH, int stripIdx)
{
    if (stripIdx < 0 || stripIdx >= (int)s_strips.size()) return;
    const StripState& s = s_strips[(size_t)stripIdx];

    const int W = g_mb_StripW;
    int y = 0;

    // Row heights
    const int kColorBandH = 2;
    const int kNameH      = g_mb_NameH;
    const int kStatusH    = 24;   // M/S/R: 3 × 8 px
    const int kSafeH      = 8;
    const int _mh         = clientH - kColorBandH - kNameH - kStatusH - kSafeH;
    const int kMeterH     = _mh > 4 ? _mh : 4;

    // ── REAPER Default 7.0 palette (values decoded from .ReaperTheme file) ──
    // col_mixerbg=RGB(51,51,51)  col_tr1_bg=RGB(66,66,66)  col_tr2_bg=RGB(69,69,69)
    // col_vuintcol=RGB(32,32,32) col_vutop=RGB(0,254,149)   col_vubot=RGB(0,191,191)
    // col_vuclip=RGB(187,37,0)
    const COLORREF kStripBg = (stripIdx & 1) ? RGB(66, 66, 66) : RGB(60, 60, 60);

    // ── 0. Full strip background ─────────────────────────────────────────────
    {
        RECT rc = { x, 0, x + W, clientH };
        HBRUSH hbr = CreateSolidBrush(kStripBg);
        FillRect(hdc, &rc, hbr);
        DeleteObject(hbr);
    }

    // ── 1. Track color band (2 px) ───────────────────────────────────────────
    {
        RECT rc = { x, y, x + W - 1, y + kColorBandH };
        HBRUSH hbr = CreateSolidBrush(s.trackColor);
        FillRect(hdc, &rc, hbr);
        DeleteObject(hbr);
    }
    y += kColorBandH;

    // ── 2. Rotated track name (Tahoma -8, 90° CCW) ───────────────────────────
    {
        // Name bg: 20% track color blended into strip bg — subtle tinting
        const int rs = (int)GetRValue(kStripBg), gs = (int)GetGValue(kStripBg), bs2 = (int)GetBValue(kStripBg);
        const int rt = (int)GetRValue(s.trackColor), gt = (int)GetGValue(s.trackColor), bt = (int)GetBValue(s.trackColor);
        COLORREF nameBg = RGB(rs * 80 / 100 + rt * 20 / 100,
                              gs * 80 / 100 + gt * 20 / 100,
                              bs2 * 80 / 100 + bt * 20 / 100);
        RECT rcBg = { x, y, x + W - 1, y + kNameH };
        HBRUSH hbg = CreateSolidBrush(nameBg);
        FillRect(hdc, &rcBg, hbg);
        DeleteObject(hbg);

        // Clip to name zone
        HRGN hOldClip = CreateRectRgn(0, 0, 1, 1);
        int  hadClip  = GetClipRgn(hdc, hOldClip);
        HRGN hClip    = CreateRectRgn(x, y, x + W - 1, y + kNameH);
        SelectClipRgn(hdc, hClip);
        DeleteObject(hClip);

        // Tahoma is REAPER's compact UI font; configurable px at lfEscapement=900
        LOGFONTA lf         = {};
        lf.lfHeight         = -g_mb_FontSize;
        lf.lfEscapement     = 900;
        lf.lfOrientation    = 900;
        lf.lfWeight         = FW_NORMAL;
        lf.lfCharSet        = DEFAULT_CHARSET;
        lf.lfQuality        = CLEARTYPE_QUALITY;
        lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
        lstrcpyA(lf.lfFaceName, "Tahoma");
        HFONT hRot    = CreateFontIndirectA(&lf);
        HFONT hOldFnt = (HFONT)SelectObject(hdc, hRot);

        // col_tcp_text ≈ RGB(185,185,185) for mixer context; dim when muted
        SetTextColor(hdc, s.mute ? RGB(80, 80, 80) : RGB(185, 185, 185));
        SetBkMode(hdc, TRANSPARENT);
        TextOutA(hdc, x + W / 2 - 4, y + kNameH - 3,
                 s.name, (int)strlen(s.name));

        SelectObject(hdc, hOldFnt);
        DeleteObject(hRot);
        if (hadClip == 1) SelectClipRgn(hdc, hOldClip);
        else              SelectClipRgn(hdc, nullptr);
        DeleteObject(hOldClip);
    }
    y += kNameH;

    // ── 3. VU Meter (REAPER Default 7.0 teal-green palette) ─────────────────
    // REAPER's authentic VU colors are NOT green-yellow-red.
    // The meter uses a teal (col_vubot=RGB(0,191,191)) → mint-green
    // (col_vutop=RGB(0,254,149)) gradient, with orange-red ONLY at clip
    // (col_vuclip=RGB(187,37,0)).  Unlit area: col_vuintcol=RGB(32,32,32).
    {
        RECT rcBg = { x + 1, y, x + W - 1, y + kMeterH };
        HBRUSH hbg = CreateSolidBrush(RGB(32, 32, 32));   // col_vuintcol
        FillRect(hdc, &rcBg, hbg);
        DeleteObject(hbg);

        const int barW    = W - 2;
        const int totalPx = kMeterH - 2;
        // 2 px lit + 1 px gap (tight, REAPER-style)
        const int kSeg = 2, kStep = 3;
        const int nSegs = (totalPx >= kStep * 2) ? totalPx / kStep : 0;

        for (int si = 0; si < nSegs; ++si)
        {
            const double segDb = k_MeterMax
                - (double)si / (double)(nSegs - 1) * (k_MeterMax - k_MeterMin);
            const bool lit = !s.mute && (s.peakDb >= segDb);

            COLORREF col;
            if (lit)
            {
                if (segDb >= 0.0)
                {
                    col = RGB(187, 37, 0);   // col_vuclip — exact REAPER value
                }
                else
                {
                    // Linear gradient: vubot RGB(0,191,191) at bottom → vutop RGB(0,254,149) at top
                    // frac=0 at k_MeterMin (-60), frac=1 at 0 dBFS
                    const double frac = (segDb - k_MeterMin) / (0.0 - k_MeterMin);
                    const int gv = (int)(191.0 + 63.0 * frac + 0.5);   // 191→254
                    const int bv = (int)(191.0 - 42.0 * frac + 0.5);   // 191→149
                    col = RGB(0, gv, bv);
                }
            }
            else
            {
                // Unlit: very dark tint matching the lit zone color family
                col = (segDb >= 0.0) ? RGB(24, 5, 0)    // dark orange-red
                                     : RGB(0, 25, 20);   // dark teal
            }

            const int sy = y + 1 + si * kStep;
            RECT sr = { x + 1, sy, x + 1 + barW, sy + kSeg };
            HBRUSH hbr = CreateSolidBrush(col);
            FillRect(hdc, &sr, hbr);
            DeleteObject(hbr);
        }

        // Peak hold: light gray normally; col_vuclip orange-red at clip
        if (!s.mute && s.peakHoldDb > k_MeterMin && nSegs > 1)
        {
            const double hf = DbToFrac(s.peakHoldDb);
            const int    hy = y + 1 + (int)((1.0 - hf) * (totalPx - 1) + 0.5);
            if (hy >= y + 1 && hy < y + totalPx)
            {
                COLORREF hcol = (s.peakHoldDb >= 0.0) ? RGB(187, 37, 0)
                                                       : RGB(210, 210, 210);
                RECT hl = { x + 1, hy, x + 1 + barW, hy + 1 };
                HBRUSH hbr = CreateSolidBrush(hcol);
                FillRect(hdc, &hl, hbr);
                DeleteObject(hbr);
            }
        }
    }
    y += kMeterH;

    // ── 4. M / S / R buttons (REAPER characteristic colors) ─────────────────
    {
        // REAPER orange mute / amber-yellow solo / red rec-arm
        struct Led { const char* lbl; bool on; COLORREF onCol; };
        const Led leds[3] = {
            { "M", s.mute,       RGB(200,  80,  20) },
            { "S", s.solo != 0,  RGB(190, 155,  15) },
            { "R", s.recArm,     RGB(185,  35,  30) },
        };

        // Explicit small normal font — undo the rotated font from the name zone
        LOGFONTA lf         = {};
        lf.lfHeight         = -7;
        lf.lfWeight         = FW_BOLD;
        lf.lfCharSet        = DEFAULT_CHARSET;
        lf.lfQuality        = DEFAULT_QUALITY;
        lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
        lstrcpyA(lf.lfFaceName, "Tahoma");
        HFONT hBtn    = CreateFontIndirectA(&lf);
        HFONT hOldBtn = (HFONT)SelectObject(hdc, hBtn);

        const int kRowH = kStatusH / 3;
        for (int li = 0; li < 3; ++li)
        {
            const int ry = y + li * kRowH;
            RECT lr = { x, ry, x + W - 1, ry + kRowH };

            // Active: colored fill; inactive: strip bg (flat, REAPER-style)
            HBRUSH hbr = CreateSolidBrush(leds[li].on ? leds[li].onCol : kStripBg);
            FillRect(hdc, &lr, hbr);
            DeleteObject(hbr);

            // 1-px separator between rows (REAPER-style divider)
            if (li < 2)
            {
                RECT sp = { x, ry + kRowH - 1, x + W - 1, ry + kRowH };
                HBRUSH hs = CreateSolidBrush(RGB(28, 28, 28));
                FillRect(hdc, &sp, hs);
                DeleteObject(hs);
            }

            SetTextColor(hdc, leds[li].on ? RGB(255, 255, 255) : RGB(85, 85, 85));
            SetBkMode(hdc, TRANSPARENT);
            DrawTextA(hdc, leds[li].lbl, -1, &lr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(hdc, hOldBtn);
        DeleteObject(hBtn);
    }
    y += kStatusH;

    // ── 5. Safe indicator dots ────────────────────────────────────────────────
    {
        RECT rcBg = { x, y, x + W - 1, y + kSafeH };
        HBRUSH hbg = CreateSolidBrush(RGB(32, 32, 32));
        FillRect(hdc, &rcBg, hbg);
        DeleteObject(hbg);

        static const struct { int bit; COLORREF col; } kDots[] = {
            { TS_VOL,      RGB(  0, 190, 190) },
            { TS_PAN,      RGB( 30,  75, 210) },
            { TS_MUTE,     RGB(210, 110,   0) },
            { TS_SOLO,     RGB(200, 200,   0) },
            { TS_FXPARAMS, RGB(185,   0, 185) },
        };
        const int nD = (int)(sizeof(kDots) / sizeof(kDots[0]));
        const int dW = (W - 2) / nD;
        int dx = x + 1;
        for (int di = 0; di < nD; ++di)
        {
            if (s.safeMask & kDots[di].bit)
            {
                RECT dr = { dx + 1, y + 2, dx + dW - 1, y + kSafeH - 2 };
                HBRUSH hbr = CreateSolidBrush(kDots[di].col);
                FillRect(hdc, &dr, hbr);
                DeleteObject(hbr);
            }
            dx += dW;
        }
    }

    // ── Right separator: 1 px, near-black (col_tr1_divline ≈ RGB(38,38,38)) ──
    {
        RECT rl = { x + W - 1, 0, x + W, clientH };
        HBRUSH hbr = CreateSolidBrush(RGB(30, 30, 30));
        FillRect(hdc, &rl, hbr);
        DeleteObject(hbr);
    }
}

// ---------------------------------------------------------------------------
// Update scroll bar parameters
// ---------------------------------------------------------------------------
static void UpdateScrollBar(HWND hwnd)
{
    const int nTracks = (int)s_strips.size();
    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin   = 0;
    si.nMax   = nTracks > 0 ? nTracks - 1 : 0;
    si.nPage  = (UINT)(s_visibleCount > 0 ? s_visibleCount : 1);
    si.nPos   = s_bankOffset;
    SetScrollInfo(g_hScroll, SB_CTL, &si, TRUE);
}

// ---------------------------------------------------------------------------
// Settings dialog proc
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK MeterBridgeSettingsDlgProc(HWND hwnd, UINT msg,
                                                    WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        InitCommonControls();

        // Helper: set edit box text + configure spin control
        auto setupSpin = [&](int editId, int spinId, int val, int lo, int hi) {
            SetDlgItemInt(hwnd, editId, (UINT)val, FALSE);
            HWND hSpin = GetDlgItem(hwnd, spinId);
            if (hSpin)
            {
                SendMessage(hSpin, UDM_SETRANGE32, (WPARAM)lo, (LPARAM)hi);
                SendMessage(hSpin, UDM_SETPOS32, 0, (LPARAM)val);
                SendMessage(hSpin, UDM_SETBUDDY, (WPARAM)GetDlgItem(hwnd, editId), 0);
            }
        };

        setupSpin(IDC_MB_STRIP_W,   IDC_MB_STRIP_W_SPIN,   g_mb_StripW,         6, 80);
        setupSpin(IDC_MB_FONT_SIZE,  IDC_MB_FONT_SIZE_SPIN, g_mb_FontSize,       4, 32);
        setupSpin(IDC_MB_NAME_H,     IDC_MB_NAME_H_SPIN,    g_mb_NameH,         10,200);
        setupSpin(IDC_MB_PEAKHOLD,   IDC_MB_PEAKHOLD_SPIN,  g_mb_PeakHoldTicks,  0,300);
        setupSpin(IDC_MB_FPS,        IDC_MB_FPS_SPIN,       g_mb_Fps,            1, 60);
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            BOOL ok = TRUE;
            auto getInt = [&](int id, int lo, int hi) -> int {
                BOOL translated = FALSE;
                int v = (int)GetDlgItemInt(hwnd, id, &translated, FALSE);
                if (!translated) { ok = FALSE; return lo; }
                if (v < lo) v = lo;
                if (v > hi) v = hi;
                return v;
            };

            int sw   = getInt(IDC_MB_STRIP_W,   6,  80);
            int fs   = getInt(IDC_MB_FONT_SIZE,  4,  32);
            int nh   = getInt(IDC_MB_NAME_H,    10, 200);
            int ph   = getInt(IDC_MB_PEAKHOLD,   0, 300);
            int fps  = getInt(IDC_MB_FPS,        1,  60);

            if (!ok)
            {
                MessageBoxA(hwnd, "Please enter valid numbers in all fields.",
                            "Meter Bridge Settings", MB_OK | MB_ICONWARNING);
                return TRUE;
            }

            g_mb_StripW        = sw;
            g_mb_FontSize      = fs;
            g_mb_NameH         = nh;
            g_mb_PeakHoldTicks = ph;
            g_mb_Fps           = fps;

            MB_SaveSettings();

            // Apply changes live: restart timer, recalculate layout, redraw
            if (g_wnd && IsWindow(g_wnd))
            {
                KillTimer(g_wnd, k_TimerID);
                SetTimer(g_wnd, k_TimerID,
                         (UINT)(1000 / (g_mb_Fps > 0 ? g_mb_Fps : 1)), nullptr);

                RECT rc;
                GetClientRect(g_wnd, &rc);
                const int scrollH = 16;
                const int clientW = rc.right;
                s_visibleCount = (g_mb_StripW > 0) ? (clientW / g_mb_StripW) : 1;
                if (s_visibleCount < 1) s_visibleCount = 1;

                InvalidateRect(g_wnd, nullptr, FALSE);
            }

            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Dialog proc
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK MeterBridgeDlgProc(HWND hwnd, UINT msg,
                                            WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        g_wnd = hwnd;
        MB_LoadSettings();

        // Create a horizontal scrollbar along the bottom edge
        RECT rc;
        GetClientRect(hwnd, &rc);
        const int scrollH = 16;
        g_hScroll = CreateWindowExA(0, "SCROLLBAR", nullptr,
                                    WS_CHILD | WS_VISIBLE | SBS_HORZ,
                                    0, rc.bottom - scrollH,
                                    rc.right, scrollH,
                                    hwnd, (HMENU)(UINT_PTR)IDC_MB_SCROLL,
                                    g_hInst, nullptr);

        SetTimer(hwnd, k_TimerID, (UINT)(1000 / (g_mb_Fps > 0 ? g_mb_Fps : 1)), nullptr);
        return TRUE;
    }

    case WM_DESTROY:
    {
        KillTimer(hwnd, k_TimerID);
        if (!g_suppressDockStateSave)
        {
            bool isFloat = false;
            bool isDocked = (DockIsChildOfDock(hwnd, &isFloat) >= 0);
            SetExtState(k_ExtSection, k_DockKey, isDocked ? "1" : "0", true);
        }
        g_hScroll = nullptr;
        g_wnd     = nullptr;
        return 0;
    }

    case WM_TIMER:
        if (wParam == k_TimerID)
        {
            PollStrips();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;  // we paint everything ourselves — skip default erase

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);
        const int scrollH = g_hScroll ? 16 : 0;
        const int clientH = rc.bottom - scrollH;
        const int clientW = rc.right;

        // Ensure s_visibleCount is valid (WM_SIZE may not have fired yet
        // when the window is first added to the docker).
        if (s_visibleCount <= 0 && clientW > 0)
        {
            s_visibleCount = clientW / g_mb_StripW;
            if (s_visibleCount < 1) s_visibleCount = 1;
        }

        if (clientW <= 0 || clientH <= 0)
        {
            EndPaint(hwnd, &ps);
            return 0;
        }

        // Double-buffer
        HDC hdcMem  = CreateCompatibleDC(hdc);
        HBITMAP hbm = CreateCompatibleBitmap(hdc, clientW, clientH);
        HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hbm);

        // Background
        RECT rcAll = { 0, 0, clientW, clientH };
        HBRUSH hbgBrush = CreateSolidBrush(RGB(51, 51, 51));  // col_mixerbg
        FillRect(hdcMem, &rcAll, hbgBrush);
        DeleteObject(hbgBrush);

        // Font – small proportional
        HFONT hFont = CreateFontA(
            -9, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        if (!hFont)
            hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT hOldFont = (HFONT)SelectObject(hdcMem, hFont);

        SetBkMode(hdcMem, TRANSPARENT);

        // Draw visible strips
        for (int i = 0; i < s_visibleCount; ++i)
        {
            int trackIdx = s_bankOffset + i;
            int sx = i * g_mb_StripW;
            DrawStrip(hdcMem, sx, clientH, trackIdx);
        }

        // "No tracks" message
        if (s_strips.empty())
        {
            SetTextColor(hdcMem, RGB(80, 80, 80));
            DrawTextA(hdcMem, "No tracks in project", -1, &rcAll,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        BitBlt(hdc, 0, 0, clientW, clientH, hdcMem, 0, 0, SRCCOPY);

        SelectObject(hdcMem, hOldFont);
        DeleteObject(hFont);
        SelectObject(hdcMem, hOld);
        DeleteObject(hbm);
        DeleteDC(hdcMem);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
    {
        const int clientW = LOWORD(lParam);
        const int clientH = HIWORD(lParam);
        const int scrollH = 16;

        s_visibleCount = (g_mb_StripW > 0) ? (clientW / g_mb_StripW) : 1;
        if (s_visibleCount < 1) s_visibleCount = 1;

        // Clamp bank offset
        const int nTracks = (int)s_strips.size();
        int maxOff = nTracks - s_visibleCount;
        if (maxOff < 0) maxOff = 0;
        if (s_bankOffset > maxOff) s_bankOffset = maxOff;

        // Reposition scrollbar
        if (g_hScroll && IsWindow(g_hScroll))
        {
            SetWindowPos(g_hScroll, nullptr,
                         0, clientH - scrollH, clientW, scrollH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }

        UpdateScrollBar(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_HSCROLL:
    {
        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask  = SIF_ALL;
        GetScrollInfo(g_hScroll, SB_CTL, &si);

        int newPos = si.nPos;
        switch (LOWORD(wParam))
        {
        case SB_LINELEFT:       newPos--;              break;
        case SB_LINERIGHT:      newPos++;              break;
        case SB_PAGELEFT:       newPos -= s_visibleCount; break;
        case SB_PAGERIGHT:      newPos += s_visibleCount; break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK:     newPos = si.nTrackPos; break;
        default: break;
        }

        const int nTracks = (int)s_strips.size();
        int maxOff = nTracks - s_visibleCount;
        if (maxOff < 0) maxOff = 0;
        if (newPos < 0)       newPos = 0;
        if (newPos > maxOff)  newPos = maxOff;

        s_bankOffset = newPos;
        UpdateScrollBar(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int step  = (delta > 0) ? -3 : 3;
        const int nTracks = (int)s_strips.size();
        int maxOff = nTracks - s_visibleCount;
        if (maxOff < 0) maxOff = 0;
        s_bankOffset += step;
        if (s_bankOffset < 0)       s_bankOffset = 0;
        if (s_bankOffset > maxOff)  s_bankOffset = maxOff;
        UpdateScrollBar(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_CONTEXTMENU:
    {
        HMENU hMenu = CreatePopupMenu();
        bool isFloat = false;
        bool isDocked = (DockIsChildOfDock(hwnd, &isFloat) >= 0);
        AppendMenuA(hMenu, MF_STRING, 201,
                    isDocked ? "Undock" : "Dock to Docker");
        AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(hMenu, MF_STRING, 203, "Settings...");
        AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(hMenu, MF_STRING, 202, "Close");
        int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                 GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam),
                                 0, hwnd, nullptr);
        DestroyMenu(hMenu);
        if (cmd == 201)
        {
            bool newDocked = !isDocked;
            SetExtState(k_ExtSection, k_DockKey, newDocked ? "1" : "0", true);
            g_suppressDockStateSave = true;
            DestroyWindow(hwnd);
            g_suppressDockStateSave = false;
            MeterBridgeWnd_ShowHide();
        }
        else if (cmd == 202)
        {
            if (isDocked) DockWindowRemove(hwnd);
            DestroyWindow(hwnd);
        }
        else if (cmd == 203)
        {
            DialogBoxA(g_hInst, MAKEINTRESOURCEA(IDD_METERBRIDGE_SETTINGS),
                       hwnd, MeterBridgeSettingsDlgProc);
        }
        return 0;
    }

    default:
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void MeterBridgeWnd_Init(HINSTANCE hInst)
{
    g_hInst = hInst;
    // Window is created lazily on first ShowHide call
}

void MeterBridgeWnd_Cleanup()
{
    if (g_wnd && IsWindow(g_wnd))
    {
        bool isFloat = false;
        if (DockIsChildOfDock(g_wnd, &isFloat) >= 0)
            DockWindowRemove(g_wnd);
        DestroyWindow(g_wnd);
    }
    g_wnd = nullptr;
}

void MeterBridgeWnd_ShowHide()
{
    if (!g_wnd || !IsWindow(g_wnd))
    {
        g_wnd = CreateDialogParam(g_hInst,
                                  MAKEINTRESOURCE(IDD_METERBRIDGE),
                                  GetMainHwnd(),
                                  MeterBridgeDlgProc,
                                  0);
        if (!g_wnd) return;

        const char* dockVal = GetExtState(k_ExtSection, k_DockKey);
        bool wantDocked = (dockVal && dockVal[0] == '1');
        if (wantDocked)
        {
            DockWindowAddEx(g_wnd, "Meter Bridge", "reaper_trans_meterbridge", true);
            DockWindowActivate(g_wnd);
        }
        else
        {
            ShowWindow(g_wnd, SW_SHOW);
        }
        return;
    }

    bool isFloat = false;
    if (DockIsChildOfDock(g_wnd, &isFloat) >= 0)
    {
        DockWindowActivate(g_wnd);
        return;
    }

    if (IsWindowVisible(g_wnd))
        ShowWindow(g_wnd, SW_HIDE);
    else
        ShowWindow(g_wnd, SW_SHOW);
}

int MeterBridgeWnd_IsVisible()
{
    if (!g_wnd || !IsWindow(g_wnd)) return 0;
    bool isFloat = false;
    if (DockIsChildOfDock(g_wnd, &isFloat) >= 0) return 1;
    return IsWindowVisible(g_wnd) ? 1 : 0;
}
