#pragma once
#include <windows.h>

// ---------------------------------------------------------------------------
// LiveLockWnd  -  Live Safe Locking dockable panel
//
// A compact dockable window with:
//   - A large LOCKED/UNLOCKED owner-drawn toggle button (green/red)
//   - A status label showing which categories are being protected
//   - A revert counter
//   - A "Settings..." button that opens a modal settings dialog
//   - A CPU warning note
//
// Settings dialog lets the user toggle each protection category and set
// the enforcement poll interval (50-2000 ms).
// ---------------------------------------------------------------------------

void LiveLockWnd_Init(HINSTANCE hInst);
void LiveLockWnd_Cleanup();
void LiveLockWnd_ShowHide();
int  LiveLockWnd_IsVisible();
