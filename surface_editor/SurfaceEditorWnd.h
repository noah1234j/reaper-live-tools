#pragma once

#include <windows.h>

// ---------------------------------------------------------------------------
// SurfaceEditorWnd.h  —  top-level Surface & Zone Editor docked window
//
// Follows the standard Live Tools window module pattern:
//   Init → ShowHide (lazy create) → Cleanup
// ---------------------------------------------------------------------------

void SurfaceEditorWnd_Init(HINSTANCE hInstance);
void SurfaceEditorWnd_Cleanup();
void SurfaceEditorWnd_ShowHide();
int  SurfaceEditorWnd_IsVisible();
void SurfaceEditorWnd_Refresh();
