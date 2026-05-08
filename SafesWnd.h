#pragma once
#include <windows.h>

// ---------------------------------------------------------------------------
// SafesWnd – per-channel safes grid window
//
// Displays a ListView grid:
//   Row 0   = "Global" (sets g_globalSafeMask in TransitionEngine)
//   Row 1-N = individual REAPER tracks (sets g_trackSafes entries)
//
// Columns (one checkbox cell each):
//   Track | Vol | Pan | Mute | Solo | Phase | FX | Vis | Sel
//
// Clicking any non-Track cell toggles the corresponding TS_* bit.
// ---------------------------------------------------------------------------

void SafesWnd_Init(HINSTANCE hInstance);
void SafesWnd_Cleanup();
void SafesWnd_ShowHide();
bool SafesWnd_IsVisible();
void SafesWnd_Refresh();   // rebuild row list from current REAPER project
