#pragma once
#include <windows.h>
#include "ControlSurface.h"

// ---------------------------------------------------------------------------
// Debug / MIDI event log window
// ---------------------------------------------------------------------------

// Open the modeless log window (no-op if already open)
void CSurfDebug_Open(HWND parent, HINSTANCE hInst);

// Close the log window (no-op if not open)
void CSurfDebug_Close();

// Returns true if the window is currently open
bool CSurfDebug_IsOpen();

// Append a timestamped message to the log (safe to call even when window is closed)
void CSurfDebug_Log(const char* msg);

// Dump a full settings summary to the log
void CSurfDebug_DumpSettings(const CSurfSettings& s);
