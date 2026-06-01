#pragma once
// ---------------------------------------------------------------------------
// MonitorWnd.h  –  Live Monitor dockable window
//
// Displays real-time audio health metrics (CPU, I/O latency, max FX chain
// PDC, round-trip latency) in a compact custom-painted panel.  Color-coded
// green → yellow → orange → red as values approach danger zones.
// ---------------------------------------------------------------------------
#include <windows.h>

void MonitorWnd_Init(HINSTANCE hInst);
void MonitorWnd_Cleanup();
void MonitorWnd_ShowHide();
int  MonitorWnd_IsVisible();
