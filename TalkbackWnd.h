#pragma once
#ifdef _WIN32
#  include <windows.h>
#else
#  include "../sws-master-2.14.0 - Copy/vendor/WDL/WDL/swell/swell.h"
#endif

class ProjectStateContext;

// One-time init / cleanup (called from ReaperPluginEntry).
void TalkbackWnd_Init(HINSTANCE hInstance);
void TalkbackWnd_Cleanup();

// Show / hide the Talkback settings window.
void TalkbackWnd_ShowHide();
int  TalkbackWnd_IsVisible();

// Project state hooks (called from the projectconfig callbacks).
void TalkbackWnd_OnProjectLoad();
bool TalkbackWnd_ProcessLine(const char* line);
void TalkbackWnd_SaveConfig(ProjectStateContext* ctx);

// Talkback on/off actions (registered as separate REAPER command IDs so
// the user can bind them to individual keys for momentary operation).
void TalkbackWnd_TbOn();
void TalkbackWnd_TbOff();
