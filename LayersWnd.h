#pragma once

#ifdef _WIN32
#  include <windows.h>
#endif

// Called from ReaperPluginEntry
void LayersWnd_Init(HINSTANCE hInst);
void LayersWnd_Cleanup();

// Show / hide the main Layers window
void LayersWnd_ShowHide();
int  LayersWnd_IsVisible();

// Refresh the layer list display (call after engine state changes)
void LayersWnd_Refresh();
