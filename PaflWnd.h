#pragma once

#ifdef _WIN32
#  include <windows.h>
#else
#  include "../sws-master-2.14.0 - Copy/vendor/WDL/WDL/swell/swell.h"
#endif

// Forward-declare REAPER types so this header compiles standalone
class MediaTrack;
class ProjectStateContext;

// ---------------------------------------------------------------------------
// PaflWnd – PAFL (Pre/After Fader Listen) monitor window
//
// Provides a dedicated monitor bus that behaves like a live mixing console's
// PFL/AFL solo system. The "program" feed runs by default. Pressing solo on
// a track (when intercept is active) mutes the program feed and routes only
// that track to the PAFL bus.
// ---------------------------------------------------------------------------

// Called from ReaperPluginEntry (init / uninit)
void PaflWnd_Init(HINSTANCE hInstance);
void PaflWnd_Cleanup();

// Called from BeginLoadProjectState to trigger auto-setup after project loads
void PaflWnd_OnProjectLoad();

// ---------------------------------------------------------------------------
// Project serialization callbacks (called from reaper_transitions.cpp)
// ---------------------------------------------------------------------------

// Reset per-project GUID state at start of each project load
void PaflWnd_ResetProjectState();

// Parse a line from the .RPP file; returns true if consumed
bool PaflWnd_ProcessLine(const char* line);

// Write bus/src GUIDs into the .RPP file
void PaflWnd_SaveConfig(ProjectStateContext* ctx);

// Show / hide the PAFL window (called from action callback)
void PaflWnd_ShowHide();

// Returns 1 if visible, 0 if hidden / not yet created
int  PaflWnd_IsVisible();

// Called by the plugin timer so PAFL can poll solo intercept
void PaflWnd_TimerTick();

// Toggle a specific track's PAFL solo (also called externally if needed)
void PaflToggleTrack(MediaTrack* tr);
