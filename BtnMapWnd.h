#pragma once
#include <windows.h>
#include "ControlSurface.h"

// Open the Button Map editor as a modal dialog.
// map is read on entry and written back when the dialog closes.
void BtnMapWnd_ShowModal(HWND hParent, HINSTANCE hInst, BtnMap& map);
