#pragma once

#include <windows.h>
#include <string>

// ---------------------------------------------------------------------------
// ZoneEditorWnd.h  —  zone assignment editor panel
//
// Shows the contents of a single .zon file and lets the user:
//   • Edit the zone name and type
//   • Add/remove IncludedZones and SubZones
//   • Toggle modifier filters (Shift, Option, Control, Alt, Flip, Hold)
//   • Assign actions to widgets via a Visual (canvas) or Table (ListView) view
//   • Save back to disk
//
// Call ZoneEditorWnd_Create() once during parent-window creation, then
// call ZoneEditorWnd_LoadZone() whenever the user picks a .zon file in the
// surface-tree.  The panel auto-resizes to fill its parent.
// ---------------------------------------------------------------------------

namespace SurfaceEditor { struct Surface; }

// Create the zone-editor panel as a child of hParent.
// Returns the panel HWND (or NULL on failure).
HWND ZoneEditorWnd_Create(HWND hParent, HINSTANCE hInst);

// Load (or reload) a zone file.  surf is the Surface the zone belongs to.
// Passing filePath="" clears the editor.
void ZoneEditorWnd_LoadZone(HWND hPanel, const std::string& filePath,
                             SurfaceEditor::Surface* surf);

// Move/resize to fill a RECT inside the parent (called on WM_SIZE)
void ZoneEditorWnd_Resize(HWND hPanel, const RECT& r);

// Refresh display from in-memory model (call after external changes)
void ZoneEditorWnd_Refresh(HWND hPanel);
