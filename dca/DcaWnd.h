#pragma once

#ifdef _WIN32
#  include <windows.h>
#else
#  include "../sws-master-2.14.0 - Copy/vendor/WDL/WDL/swell/swell.h"
#endif

// ---------------------------------------------------------------------------
// DcaWnd public API  (mirrors the pattern used by other Live Tools windows)
// ---------------------------------------------------------------------------

class ProjectStateContext; // defined in reaper_plugin.h

void DcaWnd_Init(HINSTANCE hInstance);
void DcaWnd_Cleanup();
void DcaWnd_ShowHide();
int  DcaWnd_IsVisible();    // returns 1 if visible, 0 otherwise
void DcaWnd_Refresh();      // rebuild strip controls and repaint
void DcaWnd_OnProjectLoad();

// Project-specific persistence (project_config_extension_t hooks)
void DcaWnd_SaveSettings(ProjectStateContext* ctx);
bool DcaWnd_ProcessSettingsLine(const char* line);
void DcaWnd_ResetSettings();
