#pragma once

#ifdef _WIN32
#  include <windows.h>
#else
#  include "../sws-master-2.14.0 - Copy/vendor/WDL/WDL/swell/swell.h"
#endif

#include "LayoutSnapshot.h"
#include <vector>
#include <memory>

// Global layout list – owned here, serialized by reaper_transitions.cpp
extern std::vector<std::unique_ptr<LayoutSnapshot>> g_layouts;

// Called from ReaperPluginEntry
void LayoutsWnd_Init(HINSTANCE hInstance);
void LayoutsWnd_Cleanup();

// Called from action callback / MenuHook
void LayoutsWnd_ShowHide();
int  LayoutsWnd_IsVisible();

// Called after g_layouts changes (e.g. project load)
void LayoutsWnd_RefreshList();
