#pragma once
// ---------------------------------------------------------------------------
// MonitorWnd.h  –  Live Monitor dockable window
//
// Displays real-time audio health metrics (CPU, I/O latency, max FX chain
// PDC, round-trip latency) in a compact custom-painted panel.  Color-coded
// green → yellow → orange → red as values approach danger zones.
// ---------------------------------------------------------------------------
#ifdef _WIN32
#  include <windows.h>
#else
#  include "WDL/swell/swell.h"
#endif

void MonitorWnd_Init(HINSTANCE hInst);
void MonitorWnd_Cleanup();
void MonitorWnd_ShowHide();
int  MonitorWnd_IsVisible();
