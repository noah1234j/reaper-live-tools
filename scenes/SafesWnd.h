#pragma once
#ifdef _WIN32
#  include <windows.h>
#else
#  include "WDL/swell/swell.h"
#endif

#include "TransitionSnapshot.h"   // SafeSet

class ProjectStateContext;  // from REAPER SDK (reaper_plugin.h)

// ---------------------------------------------------------------------------
// SafesWnd – per-channel safes grid window
//
// The window is a tabbed editor over two safes sets:
//   "Project"   – g_globalSafeMask / g_trackSafes, applied to every recall
//   "Subscenes" – g_subsceneSafes, OR'd in for subscene recalls only
//
// Each tab shows:
//   a row of global checkboxes (one per TS_* bit), then a ListView grid with
//   one row per REAPER track and one checkbox column per parameter type.
//   The Project tab additionally shows the layer recall-safe table, which has
//   no meaning for a subscene.
//
// The same grid is reused by the Recall Filters popup (see
// SafesWnd_EditSceneSafes), which edits a snapshot's own SafeSet: a scene's
// recall filters are safes that apply to that scene's recalls only.
// ---------------------------------------------------------------------------

void SafesWnd_Init(HINSTANCE hInstance);
void SafesWnd_Cleanup();
void SafesWnd_ShowHide();
bool SafesWnd_IsVisible();
void SafesWnd_Refresh();           // rebuild row list from current REAPER project

// Open the Safes window with the Subscenes tab already selected.
void SafesWnd_ShowSubsceneTab();

// ---------------------------------------------------------------------------
// Modal Recall Filters editor. Edits `set` in place; `title` is what the
// banner across the top says, e.g. "Recall Filters for 2  Verse 1". Returns true
// when the user changed anything, so the caller can mark the project dirty.
// ---------------------------------------------------------------------------
bool SafesWnd_EditSceneSafes(HWND parent, SafeSet& set, const char* title);

// Mark all currently-selected REAPER tracks fully safe (all per-track columns:
// Vol/Pan/Mute/Solo/Phase/FX/Name/Color) — the headless equivalent of checking
// "All" for that track's row in the Safes grid. No-op if nothing is selected.
// Always acts on the project set.
void SafesWnd_AddSelectedTracksToSafes();

// Project persistence (wired into projectconfig callbacks in reaper_transitions.cpp)
void SafesWnd_ResetForProject();   // called from BeginLoadProjectState
bool SafesWnd_ProcessLine(const char* line);          // called from ProcessExtensionLine
void SafesWnd_SaveConfig(ProjectStateContext* ctx);   // called from SaveExtensionConfig
