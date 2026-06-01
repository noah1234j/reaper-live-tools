#pragma once
// ---------------------------------------------------------------------------
// MeterBridgeWnd.h  –  Live Meter Bridge dockable window
//
// Displays a horizontal row of channel strips (one per REAPER track) showing:
//   • Live VU / peak meter with peak-hold indicator
//   • Fader level (dB readout)
//   • Mute / Solo / Rec-Arm status LEDs
//   • Safe-mask indicator dots (one per active TS_* bit)
//   • Snapshot delta row (highlights tracks that differ from the selected
//     snapshot in the Scenes window)
//
// The window is read-only (no click-to-mute or drag-fader interaction).
// ---------------------------------------------------------------------------

#ifdef _WIN32
#  include <windows.h>
#else
#  include "../sws-master-2.14.0 - Copy/vendor/WDL/WDL/swell/swell.h"
#endif

void MeterBridgeWnd_Init(HINSTANCE hInst);
void MeterBridgeWnd_Cleanup();
void MeterBridgeWnd_ShowHide();
int  MeterBridgeWnd_IsVisible();
