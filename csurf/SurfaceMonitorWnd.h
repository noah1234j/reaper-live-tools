#pragma once
#include <windows.h>

void SurfaceMonitorWnd_Init(HINSTANCE hInst);
void SurfaceMonitorWnd_Cleanup();
void SurfaceMonitorWnd_ShowHide();
int  SurfaceMonitorWnd_IsVisible();
void SurfaceMonitorWnd_Append(const char* msg);
