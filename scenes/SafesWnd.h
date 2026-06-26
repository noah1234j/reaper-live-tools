#pragma once
#ifdef _WIN32
#  include <windows.h>
#else
#  include "WDL/swell/swell.h"
#endif

class ProjectStateContext;  // from REAPER SDK (reaper_plugin.h)

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
void SafesWnd_Refresh();           // rebuild row list from current REAPER project

// Project persistence (wired into projectconfig callbacks in reaper_transitions.cpp)
void SafesWnd_ResetForProject();   // called from BeginLoadProjectState
bool SafesWnd_ProcessLine(const char* line);          // called from ProcessExtensionLine
void SafesWnd_SaveConfig(ProjectStateContext* ctx);   // called from SaveExtensionConfig
