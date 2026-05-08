#pragma once

#ifdef _WIN32
#  include <windows.h>
#else
#  include "../sws-master-2.14.0 - Copy/vendor/WDL/WDL/swell/swell.h"
#endif

// ---------------------------------------------------------------------------
// TransitionWnd – plain Win32 modeless dialog (no SWS dependency)
//
// All g_snapshots management goes through this interface so that other
// modules (reaper_transitions.cpp) can operate on the snapshot list without
// pulling in dialog internals.
// ---------------------------------------------------------------------------

#include "TransitionSnapshot.h"
#include <vector>
#include <memory>

// Global snapshot list (owned here, serialized by reaper_transitions.cpp)
extern std::vector<std::unique_ptr<TransitionSnapshot>> g_snapshots;

// Plugin entry helpers (called from reaper_transitions.cpp)
void TransitionWnd_Init(HINSTANCE hInstance);
void TransitionWnd_Cleanup();

// Show/hide the window (called from action callback)
void TransitionWnd_ShowHide();

// Return 1 if visible, 0 if hidden/not yet created
int  TransitionWnd_IsVisible();

// Refresh the list view (call after snapshot list changes via project load)
void TransitionWnd_RefreshList();

// Headless action helpers (called from registered REAPER actions, index is 0-based)
void TransitionWnd_RecallScene(int index);   // recall scene at slot index
void TransitionWnd_OverwriteScene(int index); // re-capture scene at slot index
