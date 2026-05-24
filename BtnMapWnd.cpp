// ---------------------------------------------------------------------------
// BtnMapWnd.cpp  –  Button Mapping editor window
//
// Displays all hardware buttons for the active control surface and lets
// the user override each button with a REAPER command ID.
// ---------------------------------------------------------------------------
#include "BtnMapWnd.h"
#include "ControlSurface.h"
#include "api.h"
#include "resource.h"

#include <commctrl.h>
#include <windowsx.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

// ---------------------------------------------------------------------------
// Button descriptor – one row in the ListView
// ---------------------------------------------------------------------------
struct BtnDesc
{
    const char* name;   // display name
    uint8_t     note;   // MIDI note byte
    int         group;  // filter group index
};

// Group 0 = Rec Arm
// Group 1 = Solo
// Group 2 = Mute
// Group 3 = Select
// Group 4 = Transport
// Group 5 = Global

static const BtnDesc k_fp16Buttons[] =
{
    // ---- Rec Arm (Ch 1-7) ------------------------------------------------
    { "RecArm Ch 1",    0x00, 0 },
    { "RecArm Ch 2",    0x01, 0 },
    { "RecArm Ch 3",    0x02, 0 },
    { "RecArm Ch 4",    0x03, 0 },
    { "RecArm Ch 5",    0x04, 0 },
    { "RecArm Ch 6",    0x05, 0 },
    { "RecArm Ch 7",    0x06, 0 },
    // ---- Solo (Ch 1-16) --------------------------------------------------
    { "Solo Ch 1",      0x08, 1 },
    { "Solo Ch 2",      0x09, 1 },
    { "Solo Ch 3",      0x0A, 1 },
    { "Solo Ch 4",      0x0B, 1 },
    { "Solo Ch 5",      0x0C, 1 },
    { "Solo Ch 6",      0x0D, 1 },
    { "Solo Ch 7",      0x0E, 1 },
    { "Solo Ch 8",      0x0F, 1 },
    { "Solo Ch 9",      0x50, 1 },
    { "Solo Ch 10",     0x51, 1 },
    { "Solo Ch 11",     0x52, 1 },
    { "Solo Ch 12",     0x53, 1 },
    { "Solo Ch 13",     0x54, 1 },
    { "Solo Ch 14",     0x55, 1 },
    { "Solo Ch 15",     0x56, 1 },   // note: same as LOOP (0x56) in transport
    { "Solo Ch 16",     0x57, 1 },
    // ---- Mute (Ch 1-16) -------------------------------------------------
    { "Mute Ch 1",      0x10, 2 },
    { "Mute Ch 2",      0x11, 2 },
    { "Mute Ch 3",      0x12, 2 },
    { "Mute Ch 4",      0x13, 2 },
    { "Mute Ch 5",      0x14, 2 },
    { "Mute Ch 6",      0x15, 2 },
    { "Mute Ch 7",      0x16, 2 },
    { "Mute Ch 8",      0x17, 2 },
    { "Mute Ch 9",      0x78, 2 },
    { "Mute Ch 10",     0x79, 2 },
    { "Mute Ch 11",     0x7A, 2 },
    { "Mute Ch 12",     0x7B, 2 },
    { "Mute Ch 13",     0x7C, 2 },
    { "Mute Ch 14",     0x7D, 2 },
    { "Mute Ch 15",     0x7E, 2 },
    { "Mute Ch 16",     0x7F, 2 },
    // ---- Select (Ch 1-16) -----------------------------------------------
    { "Select Ch 1",    0x18, 3 },
    { "Select Ch 2",    0x19, 3 },
    { "Select Ch 3",    0x1A, 3 },
    { "Select Ch 4",    0x1B, 3 },
    { "Select Ch 5",    0x1C, 3 },
    { "Select Ch 6",    0x1D, 3 },
    { "Select Ch 7",    0x1E, 3 },
    { "Select Ch 8",    0x1F, 3 },
    { "Select Ch 9",    0x07, 3 },   // note 0x07 is shared with RecArm-7 in MCU
    { "Select Ch 10",   0x21, 3 },
    { "Select Ch 11",   0x22, 3 },
    { "Select Ch 12",   0x23, 3 },
    { "Select Ch 13",   0x24, 3 },
    { "Select Ch 14",   0x25, 3 },
    { "Select Ch 15",   0x26, 3 },
    { "Select Ch 16",   0x27, 3 },
    // ---- Transport -------------------------------------------------------
    { "Play",           0x5E, 4 },
    { "Stop",           0x5D, 4 },
    { "Record",         0x5F, 4 },
    { "Rewind",         0x5B, 4 },
    { "Fast Fwd",       0x5C, 4 },
    { "Loop",           0x56, 4 },   // note: same as Solo Ch 15 above
    // ---- Global buttons --------------------------------------------------
    { "Zoom",           0x37, 5 },
    { "Scrub/Master",   0x3A, 5 },
    { "Click/Metro",    0x3B, 5 },
    { "Marker",         0x3D, 5 },
    { "Bank Left",      0x2E, 5 },
    { "Bank Right",     0x2F, 5 },
    { "Chan Left",      0x30, 5 },
    { "Chan Right",     0x31, 5 },
    { "Cursor Up",      0x60, 5 },
    { "Cursor Down",    0x61, 5 },
    { "Cursor Left",    0x62, 5 },
    { "Cursor Right",   0x63, 5 },
    // ---- Mix section buttons (right panel) ---------------------------------
    { "Track",          0x28, 5 },
    { "Edit Plugins",   0x2B, 5 },
    { "Sends",          0x29, 5 },
    { "Pan Mode",       0x2A, 5 },
    { "Audio",          0x3E, 5 },
    { "VI",             0x3F, 5 },
    { "Bus",            0x40, 5 },
    { "VCA",            0x41, 5 },
    { "All",            0x42, 5 },
    { "Shift",          0x46, 5 },
    // ---- Automation section ------------------------------------------------
    { "Read",           0x4A, 5 },
    { "Write",          0x4B, 5 },
    { "Trim",           0x4C, 5 },
    { "Touch",          0x4D, 5 },
    { "Latch",          0x4E, 5 },
    { "Off",            0x4F, 5 },
    // ---- User buttons ------------------------------------------------------
    { "User 1",         0x47, 5 },
    { "User 2",         0x48, 5 },
    { "User 3",         0x49, 5 },
    // ---- Session Navigator mode buttons ------------------------------------
    { "Chan Mode",      0x36, 5 },
    { "Scroll Mode",    0x38, 5 },
    { "Bank Mode",      0x39, 5 },
    { "Section",        0x3C, 5 },
    // ---- Left panel global buttons (estimated notes) ----------------------
    { "Solo Clear",     0x64, 5 },
    { "Mute Clear",     0x65, 5 },
    { "Bypass All",     0x66, 5 },
    { "Macro",          0x67, 5 },
    { "Link",           0x58, 5 },
    { "Pan/Param",      0x20, 5 },
};
static const int k_fp16ButtonCount = (int)(sizeof(k_fp16Buttons) / sizeof(k_fp16Buttons[0]));

static const char* k_groupNames[] =
{
    "All",
    "Rec Arm",
    "Solo",
    "Mute",
    "Select",
    "Transport",
    "Global",
};
static const int k_groupCount = (int)(sizeof(k_groupNames) / sizeof(k_groupNames[0]));

// ---------------------------------------------------------------------------
// MCU button descriptors (standard Mackie Control Universal protocol)
// ---------------------------------------------------------------------------
static const BtnDesc k_mcuButtons[] =
{
    // ---- Rec Arm (Ch 1-8) -----------------------------------------------
    { "RecArm Ch 1",    0x00, 0 },
    { "RecArm Ch 2",    0x01, 0 },
    { "RecArm Ch 3",    0x02, 0 },
    { "RecArm Ch 4",    0x03, 0 },
    { "RecArm Ch 5",    0x04, 0 },
    { "RecArm Ch 6",    0x05, 0 },
    { "RecArm Ch 7",    0x06, 0 },
    { "RecArm Ch 8",    0x07, 0 },
    // ---- Solo (Ch 1-8) --------------------------------------------------
    { "Solo Ch 1",      0x08, 1 },
    { "Solo Ch 2",      0x09, 1 },
    { "Solo Ch 3",      0x0A, 1 },
    { "Solo Ch 4",      0x0B, 1 },
    { "Solo Ch 5",      0x0C, 1 },
    { "Solo Ch 6",      0x0D, 1 },
    { "Solo Ch 7",      0x0E, 1 },
    { "Solo Ch 8",      0x0F, 1 },
    // ---- Mute (Ch 1-8) -------------------------------------------------
    { "Mute Ch 1",      0x10, 2 },
    { "Mute Ch 2",      0x11, 2 },
    { "Mute Ch 3",      0x12, 2 },
    { "Mute Ch 4",      0x13, 2 },
    { "Mute Ch 5",      0x14, 2 },
    { "Mute Ch 6",      0x15, 2 },
    { "Mute Ch 7",      0x16, 2 },
    { "Mute Ch 8",      0x17, 2 },
    // ---- Select (Ch 1-8) -----------------------------------------------
    { "Select Ch 1",    0x18, 3 },
    { "Select Ch 2",    0x19, 3 },
    { "Select Ch 3",    0x1A, 3 },
    { "Select Ch 4",    0x1B, 3 },
    { "Select Ch 5",    0x1C, 3 },
    { "Select Ch 6",    0x1D, 3 },
    { "Select Ch 7",    0x1E, 3 },
    { "Select Ch 8",    0x1F, 3 },
    // ---- Transport ------------------------------------------------------
    { "Play",           0x5E, 4 },
    { "Stop",           0x5D, 4 },
    { "Record",         0x5F, 4 },
    { "Rewind",         0x5B, 4 },
    { "Fast Fwd",       0x5C, 4 },
    { "Loop",           0x56, 4 },
    { "Click/Metro",    0x59, 4 },
    { "Marker",         0x54, 4 },
    // ---- Global ---------------------------------------------------------
    { "Cursor Up",      0x60, 5 },
    { "Cursor Down",    0x61, 5 },
    { "Cursor Left",    0x62, 5 },
    { "Cursor Right",   0x63, 5 },
    { "Zoom",           0x64, 5 },
    { "Scrub",          0x65, 5 },
    { "Bank Left",      0x2E, 5 },
    { "Bank Right",     0x2F, 5 },
    { "Chan Left",      0x30, 5 },
    { "Chan Right",     0x31, 5 },
    { "Jog Wheel",      0x3C, 5 },
    { "Clear Solo",     0x5A, 5 },
    { "Fader Banks",    0x28, 5 },
    { "Sends",          0x29, 5 },
    { "Pan/Surround",   0x2A, 5 },
    { "Plug-in",        0x2B, 5 },
    { "EQ",             0x2C, 5 },
    { "Instrument",     0x2D, 5 },
    { "Auto: Read",     0x4A, 5 },
    { "Auto: Write",    0x4B, 5 },
    { "Auto: Trim",     0x4C, 5 },
    { "Auto: Touch",    0x4D, 5 },
    { "Auto: Latch",    0x4E, 5 },
};
static const int k_mcuButtonCount = (int)(sizeof(k_mcuButtons) / sizeof(k_mcuButtons[0]));

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static HINSTANCE g_hInstance = nullptr;

// Current in-memory copy of the button map shown in the window
static BtnMap    g_btnMap;

// Active button descriptor table (set from proto on ShowModal)
static const BtnDesc* g_activeButtons     = k_fp16Buttons;
static int            g_activeButtonCount = k_fp16ButtonCount;

// Working copy of visible button indices (after group filter)
static std::vector<int> g_visible;  // indices into g_activeButtons[]

// ---------------------------------------------------------------------------
// Surface Layout View globals
// ---------------------------------------------------------------------------
struct SurfLayoutBtn {
    int     x, y, w, h;  // logical coordinates
    uint8_t note;
    char    label[16];    // short label for display (e.g. "S1", "M16", "CHANNEL STRIPS")
    int     group;        // 0=solo 1=mute 2=select 3=recarm 4=transport 5=global
};

static std::vector<SurfLayoutBtn> g_layoutBtns;
static float  g_layoutLogW    = 520.f;
static float  g_layoutLogH    = 148.f;
static int    g_layoutHover   = -1;
static bool   g_useLayoutView = false;
static bool   g_layoutClassReg = false;

static const COLORREF kGroupBgColor[6] = {
    RGB(160,  55,  20),  // 0 rec arm    – orange-red  (matches FP16 arm buttons)
    RGB(160,  35,  35),  // 1 mute       – red         (matches FP16 mute buttons)
    RGB( 20, 140, 140),  // 2 select     – teal/cyan   (matches FP16 select buttons)
    RGB( 30, 130,  50),  // 3 solo       – green       (matches FP16 solo buttons)
    RGB( 60,  40, 120),  // 4 transport  – indigo
    RGB( 35,  70,  90),  // 5 global     – dark blue-gray
};

// Jog wheel visual position for FP16 layout (set by BuildFP16Layout; -1 = none)
static int g_jogX = -1;
static int g_jogY = -1;
static int g_jogW = 0;
static int g_jogH = 0;

// Encoder/V-Pot circle positions for channel strip layout (set by BuildXXXLayout)
struct EncoderPosn { int x, y, w, h; };
static std::vector<EncoderPosn> g_encoderCircles;

// Motorized fader positions for channel strip layout (drawn as vertical slider graphic)
struct FaderPosn { int x, y, w, h; };
static std::vector<FaderPosn> g_faderRects;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static INT_PTR CALLBACK AssignDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static HWND  CreateListView(HWND parent, RECT r);
static void  PopulateFilter(HWND hwnd);
static void  RebuildList(HWND hwnd);
static void  RefreshItem(HWND hList, int listIdx, int descIdx);
static void  GetAssignmentText(uint8_t note, char* buf, int bufSz);
static int   GetSelectedListDesc(HWND hwnd);
static void  OpenAssignDialog(HWND parent, int descIdx);
static int   FindDescByNote(uint8_t note);
static LRESULT CALLBACK LayoutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ---------------------------------------------------------------------------
// Layout builders
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// BuildFP16Part1Layout  –  FaderPort 16, main unit (channels 1-8 + full panel)
// ---------------------------------------------------------------------------
static void BuildFP16Part1Layout()
{
    g_layoutBtns.clear();
    g_encoderCircles.clear();
    g_faderRects.clear();
    g_jogX = -1;

    const int BH  = 18;          // button height
    const int BG  = 2;           // button gap
    const int BS  = BH + BG;     // row stride = 20
    const int HH  = 10;          // section-header height
    const int HS  = HH + 4;      // header + gap = 14

    // ── Left column (global + arm buttons) ──────────────────────────────
    const int LX = 4, LW = 44;

    { SurfLayoutBtn b; b.x=LX; b.y=2; b.w=LW; b.h=HH;
      b.note=0xFF; b.group=7; lstrcpynA(b.label,"LEFT",sizeof(b.label));
      g_layoutBtns.push_back(b); }

    const int lc_y = 2 + HS;  // = 16

    // Global buttons (top of left column)
    {
        struct { uint8_t n; const char* l; } lGlob[] = {
            {0x64,"SoloCl"},{0x65,"MuteCl"},{0x66,"Bypass"},
            {0x67,"Macro"},{0x58,"Link"},
        };
        for (int i = 0; i < 5; ++i) {
            SurfLayoutBtn b; b.x=LX; b.y=lc_y+i*BS; b.w=LW; b.h=BH;
            b.note=lGlob[i].n; b.group=5;
            lstrcpynA(b.label,lGlob[i].l,sizeof(b.label));
            g_layoutBtns.push_back(b);
        }
    }
    // 5 global buttons bottom = lc_y + 4*BS + BH = 16+80+18 = 114

    // Pan/Param encoder-button (between global and arm sections)
    const int panparam_y = lc_y + 5*BS + 2;  // = 118
    { SurfLayoutBtn b; b.x=LX; b.y=panparam_y; b.w=LW; b.h=BH;
      b.note=0x20; b.group=5;
      lstrcpynA(b.label,"Pan/Prm",sizeof(b.label));
      g_layoutBtns.push_back(b); }

    // ARM section header + 7 individual arm buttons (ch1-7)
    const int arm_hdr_y = panparam_y + BH + 4;  // = 140
    { SurfLayoutBtn b; b.x=LX; b.y=arm_hdr_y; b.w=LW; b.h=HH;
      b.note=0xFF; b.group=7; lstrcpynA(b.label,"ARM",sizeof(b.label));
      g_layoutBtns.push_back(b); }

    const int arm_y = arm_hdr_y + HS;  // = 154
    {
        const uint8_t armNotes[7] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06};
        for (int i = 0; i < 7; ++i) {
            SurfLayoutBtn b; b.x=LX; b.y=arm_y+i*BS; b.w=LW; b.h=BH;
            b.note=armNotes[i]; b.group=0;
            char lbl[8]; snprintf(lbl,sizeof(lbl),"Arm%d",i+1);
            lstrcpynA(b.label,lbl,sizeof(b.label));
            g_layoutBtns.push_back(b);
        }
    }
    // Arm7 bottom = arm_y + 6*BS + BH = 154+120+18 = 292

    // ── 8 Channel strips ─────────────────────────────────────────────────
    const int CX = LX + LW + 4;             // = 52
    const int CW = 26, CG = 2, CS = CW+CG; // CS=28

    { SurfLayoutBtn b; b.x=CX; b.y=2; b.w=8*CS-CG; b.h=HH;
      b.note=0xFF; b.group=7; lstrcpynA(b.label,"CHANNEL STRIPS  1-8",sizeof(b.label));
      g_layoutBtns.push_back(b); }

    const int ENC_SZ  = CW;          // 26 – perfect circle
    const int ENC_ROW = ENC_SZ + 6;  // 32 – encoder row height
    const int ch_y = 2 + HS;         // = 16

    const uint8_t selNotes[8]  = {0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F};
    const uint8_t soloNotes[8] = {0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F};
    const uint8_t muteNotes[8] = {0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17};
    const int MW = (CW-1)/2;   // = 12  (solo half)
    const int SW = CW-MW-1;    // = 13  (mute half)
    const int fader_y = ch_y + ENC_ROW + 2*BS + 4;  // = 16+32+40+4 = 92

    for (int i = 0; i < 8; ++i) {
        int bx = CX + i*CS;
        char lbl[8];
        // Encoder circle (drawn as round knob in WM_PAINT)
        g_encoderCircles.push_back({ bx, ch_y+3, ENC_SZ, ENC_SZ });
        // SELECT button (full channel width)
        { SurfLayoutBtn b; b.x=bx; b.y=ch_y+ENC_ROW; b.w=CW; b.h=BH;
          b.note=selNotes[i]; b.group=2;
          snprintf(lbl,sizeof(lbl),"Se%d",i+1);
          lstrcpynA(b.label,lbl,sizeof(b.label)); g_layoutBtns.push_back(b); }
        // SOLO (left half)
        { SurfLayoutBtn b; b.x=bx; b.y=ch_y+ENC_ROW+BS; b.w=MW; b.h=BH;
          b.note=soloNotes[i]; b.group=3;
          snprintf(lbl,sizeof(lbl),"S%d",i+1);
          lstrcpynA(b.label,lbl,sizeof(b.label)); g_layoutBtns.push_back(b); }
        // MUTE (right half)
        { SurfLayoutBtn b; b.x=bx+MW+1; b.y=ch_y+ENC_ROW+BS; b.w=SW; b.h=BH;
          b.note=muteNotes[i]; b.group=1;
          snprintf(lbl,sizeof(lbl),"M%d",i+1);
          lstrcpynA(b.label,lbl,sizeof(b.label)); g_layoutBtns.push_back(b); }
        // Fader placeholder – height filled after right panel is computed
        g_faderRects.push_back({ bx, fader_y, CW, 0 });
    }

    // ── Right panel ───────────────────────────────────────────────────────
    const int RX  = CX + 8*CS + 4;               // = 52+224+4 = 280
    const int RBW = 38, RBG = 3, RBS = RBW+RBG;  // stride=41
    const int RBH = BH;

    // --- MIX section ---
    { SurfLayoutBtn b; b.x=RX; b.y=2; b.w=4*RBS-RBG; b.h=HH;
      b.note=0xFF; b.group=7; lstrcpynA(b.label,"MIX",sizeof(b.label));
      g_layoutBtns.push_back(b); }
    const int mix_y = 2+HS;  // = 16
    {
        struct { uint8_t n; const char* l; } r1[] = {{0x28,"Track"},{0x2B,"EditPl"},{0x29,"Sends"},{0x2A,"Pan M"}};
        struct { uint8_t n; const char* l; } r2[] = {{0x3E,"Audio"},{0x3F,"VI"},{0x40,"Bus"},{0x41,"VCA"}};
        struct { uint8_t n; const char* l; } r3[] = {{0x42,"All"},{0x46,"Shift"}};
        for (int i = 0; i < 4; ++i) {
            SurfLayoutBtn b; b.x=RX+i*RBS; b.y=mix_y;   b.w=RBW; b.h=RBH; b.note=r1[i].n; b.group=5;
            lstrcpynA(b.label,r1[i].l,sizeof(b.label)); g_layoutBtns.push_back(b); }
        for (int i = 0; i < 4; ++i) {
            SurfLayoutBtn b; b.x=RX+i*RBS; b.y=mix_y+BS; b.w=RBW; b.h=RBH; b.note=r2[i].n; b.group=5;
            lstrcpynA(b.label,r2[i].l,sizeof(b.label)); g_layoutBtns.push_back(b); }
        for (int i = 0; i < 2; ++i) {
            SurfLayoutBtn b; b.x=RX+i*RBS; b.y=mix_y+2*BS; b.w=RBW; b.h=RBH; b.note=r3[i].n; b.group=5;
            lstrcpynA(b.label,r3[i].l,sizeof(b.label)); g_layoutBtns.push_back(b); }
    }

    // --- AUTO 3x3 grid ---
    const int auto_hdr_y = mix_y + 3*BS + 2;  // = 78
    { SurfLayoutBtn b; b.x=RX; b.y=auto_hdr_y; b.w=3*RBS-RBG; b.h=HH;
      b.note=0xFF; b.group=7; lstrcpynA(b.label,"AUTO",sizeof(b.label));
      g_layoutBtns.push_back(b); }
    const int auto_y = auto_hdr_y + HS;  // = 92
    {
        struct { uint8_t n; const char* l; } ag[3][3] = {
            {{0x4E,"Latch"},{0x4C,"Trim"},{0x4F,"Off"}},
            {{0x4D,"Touch"},{0x4B,"Write"},{0x4A,"Read"}},
            {{0x47,"User1"},{0x48,"User2"},{0x49,"User3"}},
        };
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                SurfLayoutBtn b; b.x=RX+c*RBS; b.y=auto_y+r*BS; b.w=RBW; b.h=RBH;
                b.note=ag[r][c].n; b.group=5;
                lstrcpynA(b.label,ag[r][c].l,sizeof(b.label)); g_layoutBtns.push_back(b); }
    }
    // AUTO bottom = auto_y + 2*BS + BH = 92+40+18 = 150

    // --- SESSION NAV ---
    const int nav_hdr_y = auto_y + 3*BS + 2;  // = 154
    { SurfLayoutBtn b; b.x=RX; b.y=nav_hdr_y; b.w=4*RBS-RBG; b.h=HH;
      b.note=0xFF; b.group=7; lstrcpynA(b.label,"SESSION NAV",sizeof(b.label));
      g_layoutBtns.push_back(b); }
    const int nav_y = nav_hdr_y + HS;  // = 168

    // Mode rows
    {
        struct { uint8_t n; const char* l; } nr1[] = {{0x36,"Chan"},{0x38,"Scrl"},{0x39,"Bank"},{0x3C,"Sect"}};
        struct { uint8_t n; const char* l; } nr2[] = {{0x3A,"Mstr"},{0x3B,"Clck"},{0x3D,"Mkr"},{0x37,"Zoom"}};
        for (int i = 0; i < 4; ++i) {
            SurfLayoutBtn b; b.x=RX+i*RBS; b.y=nav_y;    b.w=RBW; b.h=RBH; b.note=nr1[i].n; b.group=5;
            lstrcpynA(b.label,nr1[i].l,sizeof(b.label)); g_layoutBtns.push_back(b); }
        for (int i = 0; i < 4; ++i) {
            SurfLayoutBtn b; b.x=RX+i*RBS; b.y=nav_y+BS; b.w=RBW; b.h=RBH; b.note=nr2[i].n; b.group=5;
            lstrcpynA(b.label,nr2[i].l,sizeof(b.label)); g_layoutBtns.push_back(b); }
    }

    // Jog wheel (large round) + Prev / Next
    const int jog_area_y = nav_y + 2*BS + 4;  // = 212
    const int JOG_SZ = 50;  // square → drawn as circle

    { SurfLayoutBtn b; b.x=RX; b.y=jog_area_y; b.w=RBW; b.h=RBH; b.note=0x2E; b.group=5;
      lstrcpynA(b.label,"Prev",sizeof(b.label)); g_layoutBtns.push_back(b); }
    g_jogX = RX + RBS;  g_jogY = jog_area_y;  g_jogW = JOG_SZ;  g_jogH = JOG_SZ;
    { SurfLayoutBtn b; b.x=RX+RBS+JOG_SZ+RBG; b.y=jog_area_y; b.w=RBW; b.h=RBH; b.note=0x2F; b.group=5;
      lstrcpynA(b.label,"Next",sizeof(b.label)); g_layoutBtns.push_back(b); }

    // Cursor arrows (row below jog)
    const int arr_y = jog_area_y + JOG_SZ + 4;  // = 266
    {
        struct { uint8_t n; const char* l; } arrowBtns[] =
            {{0x62,"Left"},{0x60,"Up"},{0x61,"Down"},{0x63,"Right"}};
        for (int i = 0; i < 4; ++i) {
            SurfLayoutBtn b; b.x=RX+i*RBS; b.y=arr_y; b.w=RBW; b.h=RBH; b.note=arrowBtns[i].n; b.group=5;
            lstrcpynA(b.label,arrowBtns[i].l,sizeof(b.label)); g_layoutBtns.push_back(b); }
    }

    // --- TRANSPORT ---
    const int tr_hdr_y = arr_y + RBH + 4;  // = 288
    { SurfLayoutBtn b; b.x=RX; b.y=tr_hdr_y; b.w=3*RBS-RBG; b.h=HH;
      b.note=0xFF; b.group=7; lstrcpynA(b.label,"TRANSPORT",sizeof(b.label));
      g_layoutBtns.push_back(b); }
    const int trans_y = tr_hdr_y + HS;  // = 302
    {
        struct { uint8_t n; const char* l; int g; } tb[] = {
            {0x56,"Loop",4},{0x5B,"<< ",4},{0x5C,">> ",4},
            {0x5D,"Stop",4},{0x5F,"Rec",4},{0x5E,"Play",4},
        };
        for (int i = 0; i < 3; ++i) {
            SurfLayoutBtn b; b.x=RX+i*RBS; b.y=trans_y;    b.w=RBW; b.h=RBH; b.note=tb[i].n; b.group=tb[i].g;
            lstrcpynA(b.label,tb[i].l,sizeof(b.label)); g_layoutBtns.push_back(b); }
        for (int i = 3; i < 6; ++i) {
            SurfLayoutBtn b; b.x=RX+(i-3)*RBS; b.y=trans_y+BS; b.w=RBW; b.h=RBH; b.note=tb[i].n; b.group=tb[i].g;
            lstrcpynA(b.label,tb[i].l,sizeof(b.label)); g_layoutBtns.push_back(b); }
    }
    // transport bottom = trans_y + BS + BH = 302+20+18 = 340

    // ── Phase 2: compute canvas height, then fill in fader heights ────────
    {
        int maxX = 0, maxY = 0;
        for (const auto& b : g_layoutBtns) {
            if (b.x + b.w > maxX) maxX = b.x + b.w;
            if (b.y + b.h > maxY) maxY = b.y + b.h;
        }
        if (g_jogX >= 0) {
            if (g_jogX + g_jogW > maxX) maxX = g_jogX + g_jogW;
            if (g_jogY + g_jogH > maxY) maxY = g_jogY + g_jogH;
        }
        int canvasH = maxY + 4;
        int faderH  = canvasH - fader_y - 4;
        if (faderH < 10) faderH = 10;
        for (auto& f : g_faderRects) f.h = faderH;
        g_layoutLogW = (float)(maxX + 4);
        g_layoutLogH = (float)(canvasH);
    }
}

// ---------------------------------------------------------------------------
// BuildFP16Part2Layout  –  FaderPort 16 extender  (channels 9-16 strips only)
// ---------------------------------------------------------------------------
static void BuildFP16Part2Layout()
{
    g_layoutBtns.clear();
    g_encoderCircles.clear();
    g_faderRects.clear();
    g_jogX = -1;

    const int BH  = 18, BG = 2, BS = BH+BG;
    const int HH  = 10, HS = HH+4;
    const int CW  = 26, CG = 2, CS = CW+CG;

    const int CX = 4;
    { SurfLayoutBtn b; b.x=CX; b.y=2; b.w=8*CS-CG; b.h=HH;
      b.note=0xFF; b.group=7; lstrcpynA(b.label,"CHANNEL STRIPS  9-16",sizeof(b.label));
      g_layoutBtns.push_back(b); }

    const int ENC_SZ = CW, ENC_ROW = ENC_SZ+6;
    const int ch_y = 2+HS;  // = 16

    const uint8_t selNotes[8]  = {0x07,0x21,0x22,0x23,0x24,0x25,0x26,0x27};
    const uint8_t soloNotes[8] = {0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57};
    const uint8_t muteNotes[8] = {0x78,0x79,0x7A,0x7B,0x7C,0x7D,0x7E,0x7F};
    const int MW = (CW-1)/2, SW = CW-MW-1;
    const int fader_y = ch_y + ENC_ROW + 2*BS + 4;  // = 92

    for (int i = 0; i < 8; ++i) {
        int bx = CX + i*CS;
        char lbl[8];
        g_encoderCircles.push_back({ bx, ch_y+3, ENC_SZ, ENC_SZ });
        { SurfLayoutBtn b; b.x=bx; b.y=ch_y+ENC_ROW; b.w=CW; b.h=BH; b.note=selNotes[i]; b.group=2;
          snprintf(lbl,sizeof(lbl),"Se%d",i+9); lstrcpynA(b.label,lbl,sizeof(b.label)); g_layoutBtns.push_back(b); }
        { SurfLayoutBtn b; b.x=bx; b.y=ch_y+ENC_ROW+BS; b.w=MW; b.h=BH; b.note=soloNotes[i]; b.group=3;
          snprintf(lbl,sizeof(lbl),"S%d",i+9); lstrcpynA(b.label,lbl,sizeof(b.label)); g_layoutBtns.push_back(b); }
        { SurfLayoutBtn b; b.x=bx+MW+1; b.y=ch_y+ENC_ROW+BS; b.w=SW; b.h=BH; b.note=muteNotes[i]; b.group=1;
          snprintf(lbl,sizeof(lbl),"M%d",i+9); lstrcpynA(b.label,lbl,sizeof(b.label)); g_layoutBtns.push_back(b); }
        g_faderRects.push_back({ bx, fader_y, CW, 0 });
    }

    // Compute canvas and fill fader heights (match Part1 proportions ~248 units)
    {
        int maxX = 0, maxY = 0;
        for (const auto& b : g_layoutBtns) {
            if (b.x + b.w > maxX) maxX = b.x + b.w;
            if (b.y + b.h > maxY) maxY = b.y + b.h;
        }
        int targetH = fader_y + 248 + 4;
        int canvasH = (maxY + 4 > targetH) ? maxY + 4 : targetH;
        int faderH  = canvasH - fader_y - 4;
        if (faderH < 10) faderH = 10;
        for (auto& f : g_faderRects) f.h = faderH;
        g_layoutLogW = (float)(maxX + 4);
        g_layoutLogH = (float)(canvasH);
    }
}

// ---------------------------------------------------------------------------
// BuildFP8Layout  –  FaderPort 8 in MCU mode (same visual style as Part 1)
// ---------------------------------------------------------------------------
static void BuildFP8Layout()
{
    g_layoutBtns.clear();
    g_encoderCircles.clear();
    g_faderRects.clear();
    g_jogX = -1;

    const int BH  = 18, BG = 2, BS = BH+BG;
    const int HH  = 10, HS = HH+4;
    const int CW  = 26, CG = 2, CS = CW+CG;
    const int LX = 4, LW = 44;

    // Left column
    { SurfLayoutBtn b; b.x=LX; b.y=2; b.w=LW; b.h=HH;
      b.note=0xFF; b.group=7; lstrcpynA(b.label,"LEFT",sizeof(b.label)); g_layoutBtns.push_back(b); }
    const int lc_y = 2+HS;  // = 16

    {
        struct { uint8_t n; const char* l; } lGlob[] = {
            {0x5A,"ClrSol"},{0x64,"Zoom"},{0x65,"Scrub"},
        };
        for (int i = 0; i < 3; ++i) {
            SurfLayoutBtn b; b.x=LX; b.y=lc_y+i*BS; b.w=LW; b.h=BH; b.note=lGlob[i].n; b.group=5;
            lstrcpynA(b.label,lGlob[i].l,sizeof(b.label)); g_layoutBtns.push_back(b);
        }
    }

    const int arm_hdr_y = lc_y + 3*BS + 4;  // = 80
    { SurfLayoutBtn b; b.x=LX; b.y=arm_hdr_y; b.w=LW; b.h=HH;
      b.note=0xFF; b.group=7; lstrcpynA(b.label,"ARM",sizeof(b.label)); g_layoutBtns.push_back(b); }
    const int arm_y = arm_hdr_y + HS;  // = 94
    {
        const uint8_t armNotes[8] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07};
        for (int i = 0; i < 8; ++i) {
            SurfLayoutBtn b; b.x=LX; b.y=arm_y+i*BS; b.w=LW; b.h=BH; b.note=armNotes[i]; b.group=0;
            char lbl[8]; snprintf(lbl,sizeof(lbl),"Arm%d",i+1);
            lstrcpynA(b.label,lbl,sizeof(b.label)); g_layoutBtns.push_back(b);
        }
    }

    // 8 Channel strips (MCU notes, identical to FP16 ch1-8)
    const int CX = LX+LW+4;  // = 52
    { SurfLayoutBtn b; b.x=CX; b.y=2; b.w=8*CS-CG; b.h=HH;
      b.note=0xFF; b.group=7; lstrcpynA(b.label,"CHANNEL STRIPS  1-8",sizeof(b.label)); g_layoutBtns.push_back(b); }

    const int ENC_SZ = CW, ENC_ROW = ENC_SZ+6;
    const int ch_y = 2+HS;
    const uint8_t selNotes[8]  = {0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F};
    const uint8_t soloNotes[8] = {0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F};
    const uint8_t muteNotes[8] = {0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17};
    const int MW = (CW-1)/2, SW = CW-MW-1;
    const int fader_y = ch_y + ENC_ROW + 2*BS + 4;

    for (int i = 0; i < 8; ++i) {
        int bx = CX + i*CS;
        char lbl[8];
        g_encoderCircles.push_back({ bx, ch_y+3, ENC_SZ, ENC_SZ });
        { SurfLayoutBtn b; b.x=bx; b.y=ch_y+ENC_ROW;    b.w=CW; b.h=BH; b.note=selNotes[i]; b.group=2;
          snprintf(lbl,sizeof(lbl),"Se%d",i+1); lstrcpynA(b.label,lbl,sizeof(b.label)); g_layoutBtns.push_back(b); }
        { SurfLayoutBtn b; b.x=bx; b.y=ch_y+ENC_ROW+BS; b.w=MW; b.h=BH; b.note=soloNotes[i]; b.group=3;
          snprintf(lbl,sizeof(lbl),"S%d",i+1);  lstrcpynA(b.label,lbl,sizeof(b.label)); g_layoutBtns.push_back(b); }
        { SurfLayoutBtn b; b.x=bx+MW+1; b.y=ch_y+ENC_ROW+BS; b.w=SW; b.h=BH; b.note=muteNotes[i]; b.group=1;
          snprintf(lbl,sizeof(lbl),"M%d",i+1);  lstrcpynA(b.label,lbl,sizeof(b.label)); g_layoutBtns.push_back(b); }
        g_faderRects.push_back({ bx, fader_y, CW, 0 });
    }

    // Right panel – MCU-style notes
    const int RX  = CX + 8*CS + 4;               // = 280
    const int RBW = 38, RBG = 3, RBS = RBW+RBG;
    const int RBH = BH;

    // MCU Function buttons
    { SurfLayoutBtn b; b.x=RX; b.y=2; b.w=3*RBS-RBG; b.h=HH;
      b.note=0xFF; b.group=7; lstrcpynA(b.label,"FUNC",sizeof(b.label)); g_layoutBtns.push_back(b); }
    const int func_y = 2+HS;
    {
        struct { uint8_t n; const char* l; } fr[]  = {{0x28,"Banks"},{0x29,"Sends"},{0x2A,"Pan"}};
        struct { uint8_t n; const char* l; } fr2[] = {{0x2B,"PlugIn"},{0x2C,"EQ"},{0x2D,"Inst"}};
        for (int i = 0; i < 3; ++i) {
            SurfLayoutBtn b; b.x=RX+i*RBS; b.y=func_y;    b.w=RBW; b.h=RBH; b.note=fr[i].n;  b.group=5;
            lstrcpynA(b.label,fr[i].l,sizeof(b.label));  g_layoutBtns.push_back(b); }
        for (int i = 0; i < 3; ++i) {
            SurfLayoutBtn b; b.x=RX+i*RBS; b.y=func_y+BS; b.w=RBW; b.h=RBH; b.note=fr2[i].n; b.group=5;
            lstrcpynA(b.label,fr2[i].l,sizeof(b.label)); g_layoutBtns.push_back(b); }
    }

    // AUTO (MCU auto notes = same as FP16)
    const int auto_hdr_y = func_y + 2*BS + 2;
    { SurfLayoutBtn b; b.x=RX; b.y=auto_hdr_y; b.w=3*RBS-RBG; b.h=HH;
      b.note=0xFF; b.group=7; lstrcpynA(b.label,"AUTO",sizeof(b.label)); g_layoutBtns.push_back(b); }
    const int auto_y = auto_hdr_y + HS;
    {
        struct { uint8_t n; const char* l; } ag[2][3] = {
            {{0x4A,"Read"},{0x4B,"Write"},{0x4C,"Trim"}},
            {{0x4D,"Touch"},{0x4E,"Latch"},{0x00,"---"}},
        };
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 3; ++c) {
                if (r == 1 && c == 2) continue;
                SurfLayoutBtn b; b.x=RX+c*RBS; b.y=auto_y+r*BS; b.w=RBW; b.h=RBH;
                b.note=ag[r][c].n; b.group=5;
                lstrcpynA(b.label,ag[r][c].l,sizeof(b.label)); g_layoutBtns.push_back(b); }
    }

    // NAVIGATE header + bank/chan
    const int nav_hdr_y = auto_y + 2*BS + 2;
    { SurfLayoutBtn b; b.x=RX; b.y=nav_hdr_y; b.w=4*RBS-RBG; b.h=HH;
      b.note=0xFF; b.group=7; lstrcpynA(b.label,"NAVIGATE",sizeof(b.label)); g_layoutBtns.push_back(b); }
    const int nav_y = nav_hdr_y + HS;
    {
        struct { uint8_t n; const char* l; } nr[] =
            {{0x2E,"B< "},{0x2F,"B> "},{0x30,"C< "},{0x31,"C> "}};
        for (int i = 0; i < 4; ++i) {
            SurfLayoutBtn b; b.x=RX+i*RBS; b.y=nav_y; b.w=RBW; b.h=RBH; b.note=nr[i].n; b.group=5;
            lstrcpynA(b.label,nr[i].l,sizeof(b.label)); g_layoutBtns.push_back(b); }
    }

    // Jog wheel + arrows
    const int jog_area_y = nav_y + BS + 4;
    const int JOG_SZ = 50;
    { SurfLayoutBtn b; b.x=RX; b.y=jog_area_y; b.w=RBW; b.h=RBH; b.note=0x62; b.group=5;
      lstrcpynA(b.label,"Left",sizeof(b.label)); g_layoutBtns.push_back(b); }
    g_jogX = RX+RBS;  g_jogY = jog_area_y;  g_jogW = JOG_SZ;  g_jogH = JOG_SZ;
    { SurfLayoutBtn b; b.x=RX+RBS+JOG_SZ+RBG; b.y=jog_area_y; b.w=RBW; b.h=RBH; b.note=0x63; b.group=5;
      lstrcpynA(b.label,"Right",sizeof(b.label)); g_layoutBtns.push_back(b); }
    { SurfLayoutBtn b; b.x=RX; b.y=jog_area_y+JOG_SZ/2; b.w=RBW; b.h=RBH; b.note=0x60; b.group=5;
      lstrcpynA(b.label,"Up",sizeof(b.label)); g_layoutBtns.push_back(b); }
    { SurfLayoutBtn b; b.x=RX+RBS+JOG_SZ+RBG; b.y=jog_area_y+JOG_SZ/2; b.w=RBW; b.h=RBH; b.note=0x61; b.group=5;
      lstrcpynA(b.label,"Down",sizeof(b.label)); g_layoutBtns.push_back(b); }

    // TRANSPORT
    const int tr_hdr_y = jog_area_y + JOG_SZ + 4;
    { SurfLayoutBtn b; b.x=RX; b.y=tr_hdr_y; b.w=3*RBS-RBG; b.h=HH;
      b.note=0xFF; b.group=7; lstrcpynA(b.label,"TRANSPORT",sizeof(b.label)); g_layoutBtns.push_back(b); }
    const int trans_y = tr_hdr_y + HS;
    {
        struct { uint8_t n; const char* l; int g; } tb[] = {
            {0x5B,"<< ",4},{0x5C,">> ",4},{0x5D,"Stop",4},
            {0x5E,"Play",4},{0x5F,"Rec",4},{0x56,"Loop",4},
        };
        for (int i = 0; i < 3; ++i) {
            SurfLayoutBtn b; b.x=RX+i*RBS; b.y=trans_y;    b.w=RBW; b.h=RBH; b.note=tb[i].n; b.group=tb[i].g;
            lstrcpynA(b.label,tb[i].l,sizeof(b.label)); g_layoutBtns.push_back(b); }
        for (int i = 3; i < 6; ++i) {
            SurfLayoutBtn b; b.x=RX+(i-3)*RBS; b.y=trans_y+BS; b.w=RBW; b.h=RBH; b.note=tb[i].n; b.group=tb[i].g;
            lstrcpynA(b.label,tb[i].l,sizeof(b.label)); g_layoutBtns.push_back(b); }
    }

    // Compute canvas and fill fader heights
    {
        int maxX = 0, maxY = 0;
        for (const auto& b : g_layoutBtns) {
            if (b.x + b.w > maxX) maxX = b.x + b.w;
            if (b.y + b.h > maxY) maxY = b.y + b.h;
        }
        if (g_jogX >= 0) {
            if (g_jogX + g_jogW > maxX) maxX = g_jogX + g_jogW;
            if (g_jogY + g_jogH > maxY) maxY = g_jogY + g_jogH;
        }
        int canvasH = maxY + 4;
        int faderH  = canvasH - fader_y - 4;
        if (faderH < 10) faderH = 10;
        for (auto& f : g_faderRects) f.h = faderH;
        g_layoutLogW = (float)(maxX + 4);
        g_layoutLogH = (float)(canvasH);
    }
}

// ---------------------------------------------------------------------------
// BuildMCULayout  –  Generic MCU surface (compact 8-channel grid)
// ---------------------------------------------------------------------------
static void BuildMCULayout()
{
    g_layoutBtns.clear();
    g_encoderCircles.clear();
    g_faderRects.clear();
    g_jogX = -1;

    const uint8_t armNotes[8]  = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07};
    const uint8_t soloNotes[8] = {0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F};
    const uint8_t muteNotes[8] = {0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17};
    const uint8_t selNotes[8]  = {0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F};

    const int SW = 40, SH = 14, SGAP = 2;
    const int ENC_SZ  = 20;
    const int ENC_ROW = ENC_SZ + 6;

    for (int i = 0; i < 8; ++i)
    {
        int bx = 2 + i * (SW + SGAP);
        char lbl[6];
        SurfLayoutBtn b;
        b.w = SW; b.h = SH; b.x = bx;

        g_encoderCircles.push_back({ bx + (SW - ENC_SZ) / 2, 2 + 2, ENC_SZ, ENC_SZ });

        b.y = 2+ENC_ROW; b.note = armNotes[i]; b.group = 3;
        snprintf(lbl, sizeof(lbl), "R%d", i+1); lstrcpynA(b.label, lbl, sizeof(b.label));
        g_layoutBtns.push_back(b);

        b.y = 2+ENC_ROW+(SH+SGAP); b.note = soloNotes[i]; b.group = 0;
        snprintf(lbl, sizeof(lbl), "S%d", i+1); lstrcpynA(b.label, lbl, sizeof(b.label));
        g_layoutBtns.push_back(b);

        b.y = 2+ENC_ROW+(SH+SGAP)*2; b.note = muteNotes[i]; b.group = 1;
        snprintf(lbl, sizeof(lbl), "M%d", i+1); lstrcpynA(b.label, lbl, sizeof(b.label));
        g_layoutBtns.push_back(b);

        b.y = 2+ENC_ROW+(SH+SGAP)*3; b.note = selNotes[i]; b.group = 2;
        snprintf(lbl, sizeof(lbl), "Se%d", i+1); lstrcpynA(b.label, lbl, sizeof(b.label));
        g_layoutBtns.push_back(b);
    }

    const int RX   = 2 + 8*(SW+SGAP) + 6;
    const int BW   = 26, BH = 14, BGAP = 2;

    struct { uint8_t note; const char* lbl; } func[] = {
        {0x28,"Banks"},{0x29,"Sends"},{0x2A,"Pan"},{0x2B,"PlugIn"},{0x2C,"EQ"},{0x2D,"Inst"},
    };
    for (int i = 0; i < 6; ++i) {
        SurfLayoutBtn b; b.x=RX+i*(BW+BGAP); b.y=2; b.w=BW; b.h=BH; b.note=func[i].note; b.group=5;
        lstrcpynA(b.label, func[i].lbl, sizeof(b.label)); g_layoutBtns.push_back(b);
    }

    struct { uint8_t note; const char* lbl; } autom[] = {
        {0x4A,"A:R"},{0x4B,"A:W"},{0x4C,"A:T"},{0x4D,"A:Tc"},{0x4E,"A:L"},
    };
    for (int i = 0; i < 5; ++i) {
        SurfLayoutBtn b; b.x=RX+i*(BW+BGAP); b.y=2+BH+BGAP; b.w=BW; b.h=BH; b.note=autom[i].note; b.group=5;
        lstrcpynA(b.label, autom[i].lbl, sizeof(b.label)); g_layoutBtns.push_back(b);
    }

    const int Y3 = 2 + (BH+BGAP)*2;

    struct { uint8_t note; const char* lbl; int g; } trans[] = {
        {0x5B,"<< ",4},{0x5C,">> ",4},{0x5D,"Stp",4},{0x5E,"Ply",4},
        {0x5F,"Rec",4},{0x56,"Lop",4},{0x59,"Clk",4},{0x54,"Mkr",4},
    };
    for (int i = 0; i < 8; ++i) {
        SurfLayoutBtn b; b.x=RX+(i%2)*(BW+BGAP); b.y=Y3+(i/2)*(BH+BGAP);
        b.w=BW; b.h=BH; b.note=trans[i].note; b.group=trans[i].g;
        lstrcpynA(b.label, trans[i].lbl, sizeof(b.label)); g_layoutBtns.push_back(b);
    }

    const int NX = RX + 2*(BW+BGAP) + 4;
    struct { uint8_t note; const char* lbl; int g; } nav[] = {
        {0x60,"^ ",5},{0x61,"v ",5},{0x62,"< ",5},{0x63,"> ",5},
        {0x64,"Zm",5},{0x65,"Srb",5},{0x2E,"B< ",5},{0x2F,"B> ",5},
        {0x30,"C< ",5},{0x31,"C> ",5},
    };
    for (int i = 0; i < 10; ++i) {
        SurfLayoutBtn b; b.x=NX+(i%2)*(BW+BGAP); b.y=Y3+(i/2)*(BH+BGAP);
        b.w=BW; b.h=BH; b.note=nav[i].note; b.group=nav[i].g;
        lstrcpynA(b.label, nav[i].lbl, sizeof(b.label)); g_layoutBtns.push_back(b);
    }

    struct { uint8_t note; const char* lbl; } misc[] = {{0x5A,"CSl"},{0x3C,"Jog"}};
    for (int i = 0; i < 2; ++i) {
        SurfLayoutBtn b; b.x=RX+i*(BW+BGAP); b.y=Y3+4*(BH+BGAP);
        b.w=BW; b.h=BH; b.note=misc[i].note; b.group=5;
        lstrcpynA(b.label, misc[i].lbl, sizeof(b.label)); g_layoutBtns.push_back(b);
    }

    {
        int maxX = 0, maxY = 0;
        for (const auto& b : g_layoutBtns) {
            if (b.x + b.w > maxX) maxX = b.x + b.w;
            if (b.y + b.h > maxY) maxY = b.y + b.h;
        }
        g_layoutLogW = (float)(maxX + 4);
        g_layoutLogH = (float)(maxY + 4);
    }
}

// ---------------------------------------------------------------------------
// Layout window (custom child WndProc)
// ---------------------------------------------------------------------------
static LRESULT CALLBACK LayoutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT cr;
        GetClientRect(hwnd, &cr);
        float sx = (float)(cr.right  - cr.left) / g_layoutLogW;
        float sy = (float)(cr.bottom - cr.top)  / g_layoutLogH;

        // Native background (matches REAPER dialog / Win32 appearance)
        HBRUSH hBkBr = CreateSolidBrush(GetSysColor(COLOR_3DFACE));
        FillRect(hdc, &cr, hBkBr);
        DeleteObject(hBkBr);

        int oldBk  = SetBkMode(hdc, TRANSPARENT);
        COLORREF oldTxt = SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));

        for (int i = 0; i < (int)g_layoutBtns.size(); ++i)
        {
            const SurfLayoutBtn& b = g_layoutBtns[i];
            RECT r = {
                (LONG)(b.x * sx),        (LONG)(b.y * sy),
                (LONG)((b.x+b.w) * sx),  (LONG)((b.y+b.h) * sy)
            };
            // Section header (group 7): dark label band, not interactive
            if (b.group == 7) {
                HBRUSH hBr = CreateSolidBrush(GetSysColor(COLOR_3DSHADOW));
                FillRect(hdc, &r, hBr);
                DeleteObject(hBr);
                SetTextColor(hdc, GetSysColor(COLOR_HIGHLIGHTTEXT));
                DrawTextA(hdc, b.label, -1, &r, DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
                continue;
            }
            auto it = g_btnMap.find(b.note);
            bool hasOvr    = (it != g_btnMap.end() && it->second.type != BtnActionType::Default);
            bool isDisable = (it != g_btnMap.end() && it->second.type == BtnActionType::None);
            bool hover     = (i == g_layoutHover);

            // Choose fill and text colors
            COLORREF fillColor, textColor;
            if (hover) {
                fillColor = GetSysColor(COLOR_HIGHLIGHT);
                textColor = GetSysColor(COLOR_HIGHLIGHTTEXT);
            } else if (isDisable) {
                fillColor = RGB(240, 210, 210);   // light red – disabled
                textColor = RGB(140, 0, 0);
            } else if (hasOvr) {
                fillColor = RGB(210, 240, 210);   // light green – custom command
                textColor = RGB(0, 100, 0);
            } else {
                fillColor = GetSysColor(COLOR_BTNFACE);
                textColor = GetSysColor(COLOR_BTNTEXT);
            }

            HBRUSH hBr = CreateSolidBrush(fillColor);
            FillRect(hdc, &r, hBr);
            DeleteObject(hBr);

            // 3-D raised border (native button look)
            DrawEdge(hdc, &r, EDGE_RAISED, BF_RECT | BF_SOFT);

            SetTextColor(hdc, textColor);
            DrawTextA(hdc, b.label, -1, &r,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        // Draw encoder / V-Pot circles (round knob indicators)
        for (const auto& enc : g_encoderCircles)
        {
            RECT er = {
                (LONG)(enc.x * sx),          (LONG)(enc.y * sy),
                (LONG)((enc.x+enc.w) * sx),  (LONG)((enc.y+enc.h) * sy)
            };
            HBRUSH eb  = CreateSolidBrush(GetSysColor(COLOR_3DSHADOW));
            HPEN   ep  = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DHILIGHT));
            HBRUSH oeb = (HBRUSH)SelectObject(hdc, eb);
            HPEN   oep = (HPEN)SelectObject(hdc, ep);
            Ellipse(hdc, er.left, er.top, er.right, er.bottom);
            SelectObject(hdc, oeb); SelectObject(hdc, oep);
            DeleteObject(eb); DeleteObject(ep);
        }

        // Draw motorized fader tracks + caps
        for (const auto& f : g_faderRects)
        {
            int trackW = (int)(3.0f * ((sx + sy) * 0.5f) + 0.5f);
            if (trackW < 2) trackW = 2;
            int cxTrack = (int)((f.x + f.w * 0.5f) * sx);
            RECT track = {
                cxTrack - trackW/2, (LONG)(f.y * sy),
                cxTrack + trackW/2 + 1, (LONG)((f.y + f.h) * sy)
            };
            HBRUSH trBr = CreateSolidBrush(GetSysColor(COLOR_3DSHADOW));
            FillRect(hdc, &track, trBr);
            DeleteObject(trBr);
            DrawEdge(hdc, &track, EDGE_SUNKEN, BF_RECT);

            int capH = (int)(10.0f * sy + 0.5f);
            if (capH < 5) capH = 5;
            int capMidY = (int)((f.y + f.h * 0.5f) * sy);
            RECT cap = {
                (LONG)(f.x * sx),       capMidY - capH/2,
                (LONG)((f.x + f.w) * sx), capMidY + capH/2 + 1
            };
            HBRUSH capBr = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
            FillRect(hdc, &cap, capBr);
            DeleteObject(capBr);
            DrawEdge(hdc, &cap, EDGE_RAISED, BF_RECT | BF_SOFT);
        }

        SetBkMode(hdc, oldBk);
        SetTextColor(hdc, oldTxt);

        // Draw jog wheel indicator for FP16 layout
        if (g_jogX >= 0)
        {
            RECT jr = {
                (LONG)(g_jogX * sx),         (LONG)(g_jogY * sy),
                (LONG)((g_jogX+g_jogW) * sx), (LONG)((g_jogY+g_jogH) * sy)
            };
            HBRUSH jb  = CreateSolidBrush(GetSysColor(COLOR_3DSHADOW));
            HPEN   jp  = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DHILIGHT));
            HBRUSH ojb = (HBRUSH)SelectObject(hdc, jb);
            HPEN   ojp = (HPEN)SelectObject(hdc, jp);
            Ellipse(hdc, jr.left, jr.top, jr.right, jr.bottom);
            SelectObject(hdc, ojb); SelectObject(hdc, ojp);
            DeleteObject(jb); DeleteObject(jp);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, GetSysColor(COLOR_HIGHLIGHTTEXT));
            DrawTextA(hdc, "JOG", -1, &jr, DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        RECT cr; GetClientRect(hwnd, &cr);
        float sx = (float)(cr.right-cr.left)/g_layoutLogW;
        float sy = (float)(cr.bottom-cr.top)/g_layoutLogH;
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        for (int i = 0; i < (int)g_layoutBtns.size(); ++i) {
            const SurfLayoutBtn& b = g_layoutBtns[i];
            RECT r = {(LONG)(b.x*sx),(LONG)(b.y*sy),(LONG)((b.x+b.w)*sx),(LONG)((b.y+b.h)*sy)};
            if (mx>=r.left && mx<r.right && my>=r.top && my<r.bottom) {
                if (b.note != 0xFF) {  // 0xFF = display-only (jog wheel)
                    int di = FindDescByNote(b.note);
                    if (di >= 0) OpenAssignDialog(GetParent(hwnd), di);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
        }
        return 0;
    }

    case WM_RBUTTONDOWN:
    {
        RECT cr; GetClientRect(hwnd, &cr);
        float sx = (float)(cr.right-cr.left)/g_layoutLogW;
        float sy = (float)(cr.bottom-cr.top)/g_layoutLogH;
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        for (int i = 0; i < (int)g_layoutBtns.size(); ++i) {
            const SurfLayoutBtn& b = g_layoutBtns[i];
            RECT r = {(LONG)(b.x*sx),(LONG)(b.y*sy),(LONG)((b.x+b.w)*sx),(LONG)((b.y+b.h)*sy)};
            if (mx>=r.left && mx<r.right && my>=r.top && my<r.bottom) {
                if (b.note == 0xFF) break;  // 0xFF = display-only (jog wheel)
                POINT pt = {mx, my}; ClientToScreen(hwnd, &pt);
                HMENU hM = CreatePopupMenu();
                AppendMenuA(hM, MF_STRING, 1, "Assign...");
                AppendMenuA(hM, MF_STRING, 2, "Set to Default");
                AppendMenuA(hM, MF_STRING, 3, "Disable");
                int cmd = (int)TrackPopupMenu(hM, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                    pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(hM);
                if (cmd == 1) {
                    int di = FindDescByNote(b.note);
                    if (di >= 0) OpenAssignDialog(GetParent(hwnd), di);
                } else if (cmd == 2) {
                    g_btnMap.erase(b.note);
                } else if (cmd == 3) {
                    BtnAction a; a.type = BtnActionType::None; a.cmdId = 0;
                    g_btnMap[b.note] = a;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
        }
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        RECT cr; GetClientRect(hwnd, &cr);
        float sx = (float)(cr.right-cr.left)/g_layoutLogW;
        float sy = (float)(cr.bottom-cr.top)/g_layoutLogH;
        int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
        int newHov = -1;
        for (int i = 0; i < (int)g_layoutBtns.size(); ++i) {
            const SurfLayoutBtn& b = g_layoutBtns[i];
            RECT r = {(LONG)(b.x*sx),(LONG)(b.y*sy),(LONG)((b.x+b.w)*sx),(LONG)((b.y+b.h)*sy)};
            if (mx>=r.left && mx<r.right && my>=r.top && my<r.bottom) { newHov = i; break; }
        }
        if (newHov != g_layoutHover) {
            g_layoutHover = newHov;
            InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            // Update surface label with hovered button info
            HWND hParent = GetParent(hwnd);
            if (hParent && newHov >= 0) {
                const SurfLayoutBtn& b = g_layoutBtns[newHov];
                int di = FindDescByNote(b.note);
                char info[160];
                if (di >= 0) {
                    char assign[64];
                    GetAssignmentText(b.note, assign, sizeof(assign));
                    snprintf(info, sizeof(info), "%s  (0x%02X)  --  %s",
                        g_activeButtons[di].name, (unsigned)b.note, assign);
                } else {
                    snprintf(info, sizeof(info), "Note 0x%02X", (unsigned)b.note);
                }
                SetDlgItemTextA(hParent, IDC_BM_SURFACE_LABEL, info);
            }
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_layoutHover = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void BtnMapWnd_ShowModal(HWND hParent, HINSTANCE hInst, BtnMap& map,
                         CSurfProtocol proto, const char* portLabel,
                         int channelOffset, int templateIdx)
{
    g_hInstance = hInst;
    g_btnMap    = map;

    // Select button descriptor table based on protocol
    if (proto == CSurfProtocol::FP16)
    {
        g_activeButtons     = k_fp16Buttons;
        g_activeButtonCount = k_fp16ButtonCount;
    }
    else
    {
        // MCU, HUI, RAW all use MCU-style note numbers
        g_activeButtons     = k_mcuButtons;
        g_activeButtonCount = k_mcuButtonCount;
    }

    // Set up layout view for FP16 and MCU
    g_useLayoutView = (proto == CSurfProtocol::FP16 || proto == CSurfProtocol::MCU);
    g_layoutHover   = -1;
    if (proto == CSurfProtocol::FP16 && channelOffset == 0)
        BuildFP16Part1Layout();
    else if (proto == CSurfProtocol::FP16 && channelOffset > 0)
        BuildFP16Part2Layout();
    else if (proto == CSurfProtocol::MCU && templateIdx == 3)
        BuildFP8Layout();
    else if (proto == CSurfProtocol::MCU)
        BuildMCULayout();
    else
        g_layoutBtns.clear();

    DialogBoxParam(hInst, MAKEINTRESOURCE(IDD_BTN_MAP), hParent, DialogProc, (LPARAM)portLabel);
    map = g_btnMap;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static HWND CreateListView(HWND parent, RECT r)
{
    HWND hList = CreateWindowEx(
        0, WC_LISTVIEWA, "",
        WS_CHILD | WS_VISIBLE | WS_BORDER |
        LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        r.left, r.top, r.right - r.left, r.bottom - r.top,
        parent, (HMENU)IDC_BM_LIST, g_hInstance, nullptr);

    ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    LVCOLUMNA col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;

    col.pszText = (LPSTR)"Button";      col.cx = 160; ListView_InsertColumn(hList, 0, &col);
    col.pszText = (LPSTR)"MIDI Note";   col.cx =  70; ListView_InsertColumn(hList, 1, &col);
    col.pszText = (LPSTR)"Assignment";  col.cx = 200; ListView_InsertColumn(hList, 2, &col);

    return hList;
}

static void PopulateFilter(HWND hwnd)
{
    HWND hCombo = GetDlgItem(hwnd, IDC_BM_FILTER);
    for (int i = 0; i < k_groupCount; ++i)
        ComboBox_AddString(hCombo, k_groupNames[i]);
    ComboBox_SetCurSel(hCombo, 0);
}

static int FindDescByNote(uint8_t note)
{
    for (int i = 0; i < g_activeButtonCount; ++i)
        if (g_activeButtons[i].note == note) return i;
    return -1;
}

static const char* DefaultActionDesc(uint8_t note)
{
    // Transport
    if (note == 0x5E) return "Play/Pause";
    if (note == 0x5D) return "Stop";
    if (note == 0x5F) return "Record";
    if (note == 0x5B) return "Rewind";
    if (note == 0x5C) return "Fast Fwd";
    if (note == 0x56) return "Loop";
    // Navigation/global
    if (note == 0x37) return "Zoom toggle";
    if (note == 0x3A) return "Scrub/Master";
    if (note == 0x3B) return "Click/Metro";
    if (note == 0x3D) return "Set Marker";
    if (note == 0x2E) return "Bank Left";
    if (note == 0x2F) return "Bank Right";
    if (note == 0x30) return "Channel Left";
    if (note == 0x31) return "Channel Right";
    if (note == 0x60) return "Cursor Up";
    if (note == 0x61) return "Cursor Down";
    if (note == 0x62) return "Cursor Left";
    if (note == 0x63) return "Cursor Right";
    // Channel strip ranges
    if (note >= 0x00 && note <= 0x06) { static char s[24]; snprintf(s,sizeof(s),"Rec Arm Ch %d",note+1); return s; }
    if (note >= 0x08 && note <= 0x0F) { static char s[24]; snprintf(s,sizeof(s),"Solo Ch %d",note-0x08+1); return s; }
    if (note >= 0x10 && note <= 0x17) { static char s[24]; snprintf(s,sizeof(s),"Mute Ch %d",note-0x10+1); return s; }
    if (note >= 0x18 && note <= 0x1F) { static char s[24]; snprintf(s,sizeof(s),"Select Ch %d",note-0x18+1); return s; }
    if (note == 0x07) return "Select Ch 9";
    if (note >= 0x21 && note <= 0x27) { static char s[24]; snprintf(s,sizeof(s),"Select Ch %d",note-0x21+10); return s; }
    if (note >= 0x50 && note <= 0x57) { static char s[24]; snprintf(s,sizeof(s),"Solo Ch %d",note-0x50+9); return s; }
    if (note >= 0x78 && note <= 0x7F) { static char s[24]; snprintf(s,sizeof(s),"Mute Ch %d",note-0x78+9); return s; }
    if (note >= 0x68 && note <= 0x77) { static char s[24]; snprintf(s,sizeof(s),"Fader Touch %d",note-0x68+1); return s; }
    return "(unmapped)";
}

static void GetAssignmentText(uint8_t note, char* buf, int bufSz)
{
    auto it = g_btnMap.find(note);
    if (it == g_btnMap.end() || it->second.type == BtnActionType::Default)
    {
        lstrcpynA(buf, DefaultActionDesc(note), bufSz);
        return;
    }
    if (it->second.type == BtnActionType::None)
    {
        lstrcpynA(buf, "(disabled)", bufSz);
        return;
    }
    // Command
    snprintf(buf, bufSz, "Cmd: %d", it->second.cmdId);
}

static void RefreshItem(HWND hList, int listIdx, int descIdx)
{
    const BtnDesc& d = g_activeButtons[descIdx];

    char noteBuf[16], assignBuf[64];
    snprintf(noteBuf, sizeof(noteBuf), "0x%02X", (unsigned)d.note);
    GetAssignmentText(d.note, assignBuf, sizeof(assignBuf));

    LVITEMA item = {};
    item.mask    = LVIF_TEXT;
    item.iItem   = listIdx;

    item.iSubItem = 0; item.pszText = (LPSTR)d.name;    ListView_SetItem(hList, &item);
    item.iSubItem = 1; item.pszText = noteBuf;           ListView_SetItem(hList, &item);
    item.iSubItem = 2; item.pszText = assignBuf;         ListView_SetItem(hList, &item);
}

static void RebuildList(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_BM_LIST);
    HWND hFilter = GetDlgItem(hwnd, IDC_BM_FILTER);
    int sel = ComboBox_GetCurSel(hFilter);  // 0 = All, 1-6 = group 0-5

    g_visible.clear();
    for (int i = 0; i < g_activeButtonCount; ++i)
    {
        if (sel <= 0 || g_activeButtons[i].group == sel - 1)
            g_visible.push_back(i);
    }

    ListView_DeleteAllItems(hList);
    for (int i = 0; i < (int)g_visible.size(); ++i)
    {
        LVITEMA item = {};
        item.mask    = LVIF_TEXT | LVIF_PARAM;
        item.iItem   = i;
        item.iSubItem = 0;
        item.pszText  = (LPSTR)g_activeButtons[g_visible[i]].name;
        item.lParam   = (LPARAM)g_visible[i];
        ListView_InsertItem(hList, &item);

        RefreshItem(hList, i, g_visible[i]);
    }
}

static int GetSelectedListDesc(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_BM_LIST);
    int idx = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (idx < 0) return -1;

    LVITEMA item = {};
    item.mask  = LVIF_PARAM;
    item.iItem = idx;
    if (!ListView_GetItem(hList, &item)) return -1;
    return (int)item.lParam;
}

// ---------------------------------------------------------------------------
// Assign sub-dialog
// ---------------------------------------------------------------------------
struct AssignDlgData
{
    const char* btnName;
    BtnAction   result;
    bool        ok;
};

static INT_PTR CALLBACK AssignDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        AssignDlgData* data = (AssignDlgData*)lParam;
        SetWindowLongPtr(hwnd, DWLP_USER, (LONG_PTR)data);

        // Show button name in label
        SetDlgItemTextA(hwnd, IDC_BA_LABEL, data->btnName);

        // Set radio buttons from current assignment
        int radioId;
        switch (data->result.type)
        {
        case BtnActionType::None:    radioId = IDC_BA_NONE; break;
        case BtnActionType::Command: radioId = IDC_BA_CMD;  break;
        default:                     radioId = IDC_BA_DEF;  break;
        }
        CheckRadioButton(hwnd, IDC_BA_DEF, IDC_BA_CMD, radioId);

        if (data->result.type == BtnActionType::Command && data->result.cmdId != 0)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", data->result.cmdId);
            SetDlgItemTextA(hwnd, IDC_BA_CMDID, buf);
        }

        // Enable/disable command ID edit
        EnableWindow(GetDlgItem(hwnd, IDC_BA_CMDID),
                     data->result.type == BtnActionType::Command);
        return TRUE;
    }

    case WM_COMMAND:
    {
        AssignDlgData* data = (AssignDlgData*)GetWindowLongPtr(hwnd, DWLP_USER);
        int ctrl = LOWORD(wParam);
        int note = HIWORD(wParam);

        if (ctrl == IDC_BA_DEF || ctrl == IDC_BA_NONE || ctrl == IDC_BA_CMD)
        {
            // Enable/disable command ID edit based on radio selection
            bool cmdEnabled = (IsDlgButtonChecked(hwnd, IDC_BA_CMD) == BST_CHECKED);
            EnableWindow(GetDlgItem(hwnd, IDC_BA_CMDID), cmdEnabled);
        }

        if (ctrl == IDOK)
        {
            if (IsDlgButtonChecked(hwnd, IDC_BA_DEF) == BST_CHECKED)
            {
                data->result.type  = BtnActionType::Default;
                data->result.cmdId = 0;
            }
            else if (IsDlgButtonChecked(hwnd, IDC_BA_NONE) == BST_CHECKED)
            {
                data->result.type  = BtnActionType::None;
                data->result.cmdId = 0;
            }
            else
            {
                // Command
                char buf[32] = {};
                GetDlgItemTextA(hwnd, IDC_BA_CMDID, buf, sizeof(buf));
                int cmdId = atoi(buf);
                if (cmdId <= 0)
                {
                    MessageBoxA(hwnd, "Please enter a valid REAPER Command ID (positive integer).",
                                "Button Map", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                data->result.type  = BtnActionType::Command;
                data->result.cmdId = cmdId;
            }
            data->ok = true;
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (ctrl == IDCANCEL)
        {
            data->ok = false;
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        // suppress unreferenced warning
        (void)note;
        break;
    }
    }
    return FALSE;
}

static void OpenAssignDialog(HWND parent, int descIdx)
{
    if (descIdx < 0 || descIdx >= g_activeButtonCount) return;
    const BtnDesc& d = g_activeButtons[descIdx];

    // Build label: "Button: <name>  [note 0x??]"
    char label[64];
    snprintf(label, sizeof(label), "Button: %s  [note 0x%02X]", d.name, (unsigned)d.note);

    // Seed with current assignment
    BtnAction cur;
    auto it = g_btnMap.find(d.note);
    if (it != g_btnMap.end()) cur = it->second;

    AssignDlgData data;
    data.btnName = label;
    data.result  = cur;
    data.ok      = false;

    DialogBoxParam(g_hInstance, MAKEINTRESOURCE(IDD_BTN_ASSIGN),
                   parent, AssignDialogProc, (LPARAM)&data);

    if (!data.ok) return;

    if (data.result.type == BtnActionType::Default)
        g_btnMap.erase(d.note);       // remove override; fall through to default
    else
        g_btnMap[d.note] = data.result;

    // Refresh the list row (or layout view)
    if (g_useLayoutView)
    {
        HWND hLayout = GetDlgItem(parent, IDC_BM_LIST);
        InvalidateRect(hLayout, nullptr, FALSE);
    }
    else
    {
        HWND hList = GetDlgItem(parent, IDC_BM_LIST);
        int listIdx = -1;
        for (int i = 0; i < (int)g_visible.size(); ++i)
        {
            if (g_visible[i] == descIdx) { listIdx = i; break; }
        }
        if (listIdx >= 0)
            RefreshItem(hList, listIdx, descIdx);
    }
}

// ---------------------------------------------------------------------------
// Main dialog proc
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        // portLabel passed via lParam (may be null)
        const char* portLabel = (const char*)lParam;

        // Get placeholder rect, then destroy it
        HWND hPlaceholder = GetDlgItem(hwnd, IDC_BM_LIST);
        RECT r;
        GetWindowRect(hPlaceholder, &r);
        MapWindowPoints(nullptr, hwnd, (POINT*)&r, 2);
        DestroyWindow(hPlaceholder);

        if (g_useLayoutView)
        {
            // Register custom window class once
            if (!g_layoutClassReg)
            {
                WNDCLASSEXA wc = {};
                wc.cbSize        = sizeof(wc);
                wc.lpfnWndProc   = LayoutWndProc;
                wc.hInstance     = g_hInstance;
                wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
                wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
                wc.lpszClassName = "LTSurfLayout";
                RegisterClassExA(&wc);
                g_layoutClassReg = true;
            }

            // Size dialog to fit the layout canvas; clamp to 90% of monitor
            HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfo(hMon, &mi);
            int monW = mi.rcWork.right  - mi.rcWork.left;
            int monH = mi.rcWork.bottom - mi.rcWork.top;
            // canvas + dialog chrome (~16px H border) + label row + button row + title
            int wantW = (int)g_layoutLogW + 30;
            int wantH = (int)g_layoutLogH + 110;
            int capW  = monW * 9 / 10;
            int capH  = monH * 9 / 10;
            int cx    = wantW > capW ? capW : (wantW < 640 ? 640 : wantW);
            int cy    = wantH > capH ? capH : (wantH < 300 ? 300 : wantH);
            // Center on monitor, always fully on-screen
            int wx = mi.rcWork.left + (monW - cx) / 2;
            int wy = mi.rcWork.top  + (monH - cy) / 2;
            if (wx < mi.rcWork.left)             wx = mi.rcWork.left;
            if (wy < mi.rcWork.top)              wy = mi.rcWork.top;
            if (wx + cx > mi.rcWork.right)  wx = mi.rcWork.right  - cx;
            if (wy + cy > mi.rcWork.bottom) wy = mi.rcWork.bottom - cy;
            SetWindowPos(hwnd, nullptr, wx, wy, cx, cy, SWP_NOZORDER);

            // Update placeholder rect after resize
            RECT cr2; GetClientRect(hwnd, &cr2);
            r.left = 5; r.right = cr2.right - 5;
            // (top/bottom will be adjusted in WM_SIZE)

            // Create layout window in the placeholder area
            CreateWindowExA(0, "LTSurfLayout", "",
                WS_CHILD | WS_VISIBLE | WS_BORDER,
                r.left, r.top, r.right - r.left, r.bottom - r.top,
                hwnd, (HMENU)(INT_PTR)IDC_BM_LIST, g_hInstance, nullptr);

            // Hide filter+assign+default+disable UI (not needed for layout mode)
            if (HWND h = GetDlgItem(hwnd, IDC_BM_FILTER))  ShowWindow(h, SW_HIDE);
            if (HWND h = GetDlgItem(hwnd, IDC_BM_ASSIGN))  ShowWindow(h, SW_HIDE);
            if (HWND h = GetDlgItem(hwnd, IDC_BM_DEFAULT)) ShowWindow(h, SW_HIDE);
            if (HWND h = GetDlgItem(hwnd, IDC_BM_DISABLE)) ShowWindow(h, SW_HIDE);
        }
        else
        {
            CreateListView(hwnd, r);
        }

        // Surface label
        char labelBuf[128];
        if (portLabel && *portLabel)
            snprintf(labelBuf, sizeof(labelBuf), "Button overrides for: %s", portLabel);
        else if (g_activeButtons == k_fp16Buttons)
            lstrcpynA(labelBuf, "FaderPort 16 (Native)  \x96  FP16 mode button overrides", sizeof(labelBuf));
        else
            lstrcpynA(labelBuf, "MCU/HUI  \x96  button overrides (all MCU-protocol devices)", sizeof(labelBuf));
        SetDlgItemTextA(hwnd, IDC_BM_SURFACE_LABEL, labelBuf);

        if (!g_useLayoutView)
        {
            PopulateFilter(hwnd);
            RebuildList(hwnd);
        }
        return TRUE;
    }

    case WM_COMMAND:
    {
        int ctrl = LOWORD(wParam);
        int notif = HIWORD(wParam);

        if (ctrl == IDC_BM_FILTER && notif == CBN_SELCHANGE)
        {
            RebuildList(hwnd);
            return TRUE;
        }

        if (ctrl == IDC_BM_ASSIGN)
        {
            int di = GetSelectedListDesc(hwnd);
            if (di >= 0) OpenAssignDialog(hwnd, di);
            return TRUE;
        }

        if (ctrl == IDC_BM_DEFAULT)
        {
            int di = GetSelectedListDesc(hwnd);
            if (di >= 0)
            {
                g_btnMap.erase(g_activeButtons[di].note);
                // Refresh that row
                HWND hList = GetDlgItem(hwnd, IDC_BM_LIST);
                int listIdx = -1;
                for (int i = 0; i < (int)g_visible.size(); ++i)
                    if (g_visible[i] == di) { listIdx = i; break; }
                if (listIdx >= 0) RefreshItem(hList, listIdx, di);
            }
            return TRUE;
        }

        if (ctrl == IDC_BM_DISABLE)
        {
            int di = GetSelectedListDesc(hwnd);
            if (di >= 0)
            {
                BtnAction a;
                a.type = BtnActionType::None;
                g_btnMap[g_activeButtons[di].note] = a;
                HWND hList = GetDlgItem(hwnd, IDC_BM_LIST);
                int listIdx = -1;
                for (int i = 0; i < (int)g_visible.size(); ++i)
                    if (g_visible[i] == di) { listIdx = i; break; }
                if (listIdx >= 0) RefreshItem(hList, listIdx, di);
            }
            return TRUE;
        }

        if (ctrl == IDC_BM_RESETALL)
        {
            if (MessageBoxA(hwnd,
                    "Reset all button assignments to their defaults?",
                    "Button Map", MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
                g_btnMap.clear();
                if (g_useLayoutView)
                    InvalidateRect(GetDlgItem(hwnd, IDC_BM_LIST), nullptr, FALSE);
                else
                    RebuildList(hwnd);
            }
            return TRUE;
        }

        // ---- Import button map from text file ----------------------------
        if (ctrl == IDC_BM_IMPORT)
        {
            char path[MAX_PATH] = {};
            OPENFILENAMEA ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = hwnd;
            ofn.lpstrFilter = "Button Map Files\0*.btnmap\0Text Files\0*.txt\0All Files\0*.*\0";
            ofn.lpstrFile   = path;
            ofn.nMaxFile    = MAX_PATH;
            ofn.lpstrTitle  = "Import Button Map";
            ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameA(&ofn))
            {
                FILE* f = fopen(path, "r");
                if (f)
                {
                    g_btnMap.clear();
                    char line[64];
                    while (fgets(line, sizeof(line), f))
                    {
                        unsigned note = 0; int type = 0, cmd = 0;
                        if (sscanf(line, "%02X:%d:%d", &note, &type, &cmd) >= 2)
                        {
                            BtnAction a;
                            a.type  = (BtnActionType)type;
                            a.cmdId = (a.type == BtnActionType::Command) ? cmd : 0;
                            g_btnMap[(uint8_t)note] = a;
                        }
                    }
                    fclose(f);
                    RebuildList(hwnd);
                }
                else
                    MessageBoxA(hwnd, "Could not open the selected file.", "Import Error", MB_OK | MB_ICONERROR);
            }
            return TRUE;
        }

        // ---- Export button map to text file --------------------------------
        if (ctrl == IDC_BM_EXPORT)
        {
            char path[MAX_PATH] = {};
            lstrcpynA(path, "ButtonMap.btnmap", MAX_PATH);
            OPENFILENAMEA ofn = {};
            ofn.lStructSize  = sizeof(ofn);
            ofn.hwndOwner    = hwnd;
            ofn.lpstrFilter  = "Button Map Files\0*.btnmap\0Text Files\0*.txt\0All Files\0*.*\0";
            ofn.lpstrFile    = path;
            ofn.nMaxFile     = MAX_PATH;
            ofn.lpstrTitle   = "Export Button Map";
            ofn.lpstrDefExt  = "btnmap";
            ofn.Flags        = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameA(&ofn))
            {
                FILE* f = fopen(path, "w");
                if (f)
                {
                    // Write header comment
                    fprintf(f, "# Live Tools Button Map Export\n");
                    fprintf(f, "# Format: NoteHex:Type:CmdId  (Type: 0=Default 1=None 2=Command)\n");
                    for (const auto& kv : g_btnMap)
                    {
                        fprintf(f, "%02X:%d:%d\n",
                            (unsigned)kv.first,
                            (int)kv.second.type,
                            kv.second.cmdId);
                    }
                    fclose(f);
                }
                else
                    MessageBoxA(hwnd, "Could not write to the selected file.", "Export Error", MB_OK | MB_ICONERROR);
            }
            return TRUE;
        }

        if (ctrl == IDCANCEL)
        {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }

    case WM_NOTIFY:
    {
        if (!g_useLayoutView)
        {
            NMHDR* hdr = (NMHDR*)lParam;
            if (hdr->idFrom == IDC_BM_LIST && hdr->code == NM_DBLCLK)
            {
                int di = GetSelectedListDesc(hwnd);
                if (di >= 0) OpenAssignDialog(hwnd, di);
            }
        }
        break;
    }

    case WM_SIZE:
    {
        // Resize the ListView to fill the available area above the button row
        RECT cr;
        GetClientRect(hwnd, &cr);
        int bottom = cr.bottom;

        // Button row height ~20px from bottom; keep some margin
        HWND hAssign = GetDlgItem(hwnd, IDC_BM_ASSIGN);
        RECT br;
        GetWindowRect(hAssign, &br);
        MapWindowPoints(nullptr, hwnd, (POINT*)&br, 2);
        int btnTop = br.top - 4;

        // Filter + label row height
        int listTop;
        if (g_useLayoutView)
        {
            HWND hLabel = GetDlgItem(hwnd, IDC_BM_SURFACE_LABEL);
            RECT lr;
            GetWindowRect(hLabel, &lr);
            MapWindowPoints(nullptr, hwnd, (POINT*)&lr, 2);
            listTop = lr.bottom + 4;
        }
        else
        {
            HWND hFilter = GetDlgItem(hwnd, IDC_BM_FILTER);
            RECT fr;
            GetWindowRect(hFilter, &fr);
            MapWindowPoints(nullptr, hwnd, (POINT*)&fr, 2);
            listTop = fr.bottom + 4;
        }

        HWND hList = GetDlgItem(hwnd, IDC_BM_LIST);
        SetWindowPos(hList, nullptr, 5, listTop,
                     cr.right - 10, btnTop - listTop, SWP_NOZORDER);

        // Stretch the Reset All button to the right edge
        HWND hReset = GetDlgItem(hwnd, IDC_BM_RESETALL);
        RECT rr;
        GetWindowRect(hReset, &rr);
        MapWindowPoints(nullptr, hwnd, (POINT*)&rr, 2);
        SetWindowPos(hReset, nullptr, cr.right - (rr.right - rr.left) - 5, rr.top,
                     0, 0, SWP_NOZORDER | SWP_NOSIZE);

        // Move Close button to right edge of bottom row
        HWND hClose = GetDlgItem(hwnd, IDCANCEL);
        RECT clr;
        GetWindowRect(hClose, &clr);
        MapWindowPoints(nullptr, hwnd, (POINT*)&clr, 2);
        SetWindowPos(hClose, nullptr, cr.right - (clr.right - clr.left) - 5, clr.top,
                     0, 0, SWP_NOZORDER | SWP_NOSIZE);

        (void)bottom;
        InvalidateRect(hwnd, nullptr, TRUE);
        break;
    }

    case WM_CLOSE:
        EndDialog(hwnd, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}
