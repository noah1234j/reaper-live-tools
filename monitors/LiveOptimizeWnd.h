#pragma once

#ifdef _WIN32
#  include <windows.h>
#else
#  include "../sws-master-2.14.0 - Copy/vendor/WDL/WDL/swell/swell.h"
#endif

// ---------------------------------------------------------------------------
// LiveOptimizeWnd – modeless dialog that scans REAPER + Windows system
// settings for live-performance optimization and displays a scored report.
//
// Public API (called from reaper_transitions.cpp):
//   LiveOptimizeWnd_Init      – create dialog (call once at plugin load)
//   LiveOptimizeWnd_ShowHide  – toggle visibility (action callback)
//   LiveOptimizeWnd_IsVisible – return 1 if visible, 0 otherwise
//   LiveOptimizeWnd_Cleanup   – destroy dialog (call on plugin unload)
// ---------------------------------------------------------------------------

void LiveOptimizeWnd_Init(HINSTANCE hInstance);
void LiveOptimizeWnd_ShowHide();
int  LiveOptimizeWnd_IsVisible();
void LiveOptimizeWnd_Cleanup();
