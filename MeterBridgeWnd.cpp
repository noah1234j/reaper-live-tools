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
static const int  k_StripW      = 14;       // strip width in pixels
static const UINT k_TimerID     = 44;       // WM_TIMER id
static const UINT k_TimerMs     = 60;       // ~16 fps
static const int  k_PeakHoldTicks = 33;     // ~2 s at 60 ms/tick
static const double k_MeterMin  = -60.0;    // dBFS floor
static const double k_MeterMax  =  +6.0;    // dBFS ceiling
static const char* k_ExtSection = "reaper_transitions";
static const char* k_DockKey    = "meterbridge_docked";

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
            s.peakHoldTick = k_PeakHoldTicks;
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

    const int W = k_StripW;
    int y = 0;

    // Row heights
    const int kColorBandH = 3;
    const int kNameH      = 56;   // rotated name zone
    const int kStatusH    = 24;   // M/S/R: 3 × 8 px
    const int kSafeH      = 8;
    const int _mh         = clientH - kColorBandH - kNameH - kStatusH - kSafeH;
    const int kMeterH     = _mh > 4 ? _mh : 4;

    // ── 1. Track color band ──────────────────────────────────────────────────
    {
        RECT rc = { x, y, x + W - 1, y + kColorBandH };
        HBRUSH hbr = CreateSolidBrush(s.trackColor);
        FillRect(hdc, &rc, hbr);
        DeleteObject(hbr);
    }
    y += kColorBandH;

    // ── 2. Rotated track name (90°, reads bottom→top) ────────────────────────
    {
        RECT rcBg = { x, y, x + W - 1, y + kNameH };
        HBRUSH hbg = CreateSolidBrush(RGB(20, 20, 20));
        FillRect(hdc, &rcBg, hbg);
        DeleteObject(hbg);

        // Clip text to this zone
        HRGN hOldClip = CreateRectRgn(0, 0, 1, 1);
        int  hadClip  = GetClipRgn(hdc, hOldClip);
        HRGN hClip    = CreateRectRgn(x, y, x + W - 1, y + kNameH);
        SelectClipRgn(hdc, hClip);
        DeleteObject(hClip);

        // 90° CCW rotated font (lfEscapement in tenths of degree, 900 = 90°)
        LOGFONTA lf        = {};
        lf.lfHeight        = -11;
        lf.lfEscapement    = 900;
        lf.lfOrientation   = 900;
        lf.lfWeight        = FW_NORMAL;
        lf.lfCharSet       = DEFAULT_CHARSET;
        lf.lfQuality       = CLEARTYPE_QUALITY;
        lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
        lstrcpyA(lf.lfFaceName, "Segoe UI");
        HFONT hRot    = CreateFontIndirectA(&lf);
        HFONT hOldFnt = (HFONT)SelectObject(hdc, hRot);

        // With lfEscapement=900 the baseline runs upward; character height (~8px)
        // is now the horizontal extent. Centre in strip: x + W/2 - ascent/2.
        SetTextColor(hdc, s.mute ? RGB(75, 75, 75) : RGB(185, 185, 185));
        SetBkMode(hdc, TRANSPARENT);
        TextOutA(hdc, x + W / 2 - 5, y + kNameH - 3,
                 s.name, (int)strlen(s.name));

        SelectObject(hdc, hOldFnt);
        DeleteObject(hRot);
        if (hadClip == 1) SelectClipRgn(hdc, hOldClip);
        else              SelectClipRgn(hdc, nullptr);
        DeleteObject(hOldClip);
    }
    y += kNameH;

    // ── 3. Segmented VU meter ────────────────────────────────────────────────
    {
        RECT rcBg = { x + 2, y, x + W - 2, y + kMeterH };
        HBRUSH hbg = CreateSolidBrush(RGB(12, 12, 12));
        FillRect(hdc, &rcBg, hbg);
        DeleteObject(hbg);

        const int barW    = W - 4;
        const int totalPx = kMeterH - 2;
        // 3 px lit block + 1 px gap = 4 px per segment
        const int kSeg = 3, kStep = 4;
        const int nSegs = (totalPx >= kStep * 2) ? totalPx / kStep : 0;

        for (int si = 0; si < nSegs; ++si)
        {
            // si=0 = top (k_MeterMax), si=nSegs-1 = bottom (k_MeterMin)
            const double segDb = k_MeterMax
                - (double)si / (double)(nSegs - 1) * (k_MeterMax - k_MeterMin);
            const bool lit = !s.mute && (s.peakDb >= segDb);

            COLORREF col;
            if (lit)
            {
                if      (segDb >= -6.0)  col = RGB(220,  40,  40);
                else if (segDb >= -18.0) col = RGB(210, 170,   0);
                else                     col = RGB(  0, 180,   0);
            }
            else
            {
                if      (segDb >= -6.0)  col = RGB( 42,  14,  14);
                else if (segDb >= -18.0) col = RGB( 36,  33,   8);
                else                     col = RGB(  7,  30,   7);
            }

            const int sy = y + 1 + si * kStep;
            RECT sr = { x + 2, sy, x + 2 + barW, sy + kSeg };
            HBRUSH hbr = CreateSolidBrush(col);
            FillRect(hdc, &sr, hbr);
            DeleteObject(hbr);
        }

        // Peak hold line (white normally, red when ≥ 0 dBFS)
        if (!s.mute && s.peakHoldDb > k_MeterMin && nSegs > 1)
        {
            const double hf = DbToFrac(s.peakHoldDb);
            const int    hy = y + 1 + (int)((1.0 - hf) * (totalPx - 1) + 0.5);
            if (hy >= y + 1 && hy < y + totalPx)
            {
                COLORREF hcol = (s.peakHoldDb >= 0.0) ? RGB(255, 80, 80)
                                                       : RGB(255, 255, 255);
                RECT hl = { x + 2, hy, x + 2 + barW, hy + 2 };
                HBRUSH hbr = CreateSolidBrush(hcol);
                FillRect(hdc, &hl, hbr);
                DeleteObject(hbr);
            }
        }
    }
    y += kMeterH;

    // ── 4. M / S / R rows ────────────────────────────────────────────────────
    {
        struct Led { const char* lbl; bool on; COLORREF col; };
        const Led leds[3] = {
            { "M", s.mute,      RGB(200,  35,  35) },
            { "S", s.solo != 0, RGB(210, 165,   0) },
            { "R", s.recArm,    RGB(200,  35,  35) },
        };
        const int kRowH = kStatusH / 3;
        for (int li = 0; li < 3; ++li)
        {
            const int ry = y + li * kRowH;
            RECT lr = { x + 1, ry, x + W - 1, ry + kRowH };
            HBRUSH hbr = CreateSolidBrush(leds[li].on ? leds[li].col : RGB(24, 24, 24));
            FillRect(hdc, &lr, hbr);
            DeleteObject(hbr);
            SetTextColor(hdc, leds[li].on ? RGB(255, 255, 255) : RGB(65, 65, 65));
            DrawTextA(hdc, leds[li].lbl, -1, &lr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
    y += kStatusH;

    // ── 5. Safe mask dots ────────────────────────────────────────────────────
    {
        RECT rcBg = { x, y, x + W - 1, y + kSafeH };
        HBRUSH hbg = CreateSolidBrush(RGB(13, 13, 13));
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
        const int dW = (W - 4) / nD;
        int dx = x + 2;
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

    // ── Separator (right edge) ───────────────────────────────────────────────
    {
        RECT rl = { x + W - 1, 0, x + W, clientH };
        HBRUSH hbr = CreateSolidBrush(RGB(40, 40, 40));
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

        SetTimer(hwnd, k_TimerID, k_TimerMs, nullptr);
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
            s_visibleCount = clientW / k_StripW;
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
        HBRUSH hbgBrush = CreateSolidBrush(RGB(15, 15, 15));
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
            int sx = i * k_StripW;
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

        s_visibleCount = (k_StripW > 0) ? (clientW / k_StripW) : 1;
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
        AppendMenuA(hMenu, MF_STRING, 202, "Close");
        int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                 GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam),
                                 0, hwnd, nullptr);
        DestroyMenu(hMenu);
        if (cmd == 201)
        {
            // Destroy and recreate with opposite dock state (same pattern as Scenes)
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
