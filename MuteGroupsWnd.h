#pragma once

#ifdef _WIN32
#  include <windows.h>
#endif

// Called from ReaperPluginEntry
void MuteGroupsWnd_Init(HINSTANCE hInst);
void MuteGroupsWnd_Cleanup();

// Show / hide the Mute Groups window
void MuteGroupsWnd_ShowHide();
int  MuteGroupsWnd_IsVisible();

// Refresh the display (call after engine state changes)
void MuteGroupsWnd_Refresh();
