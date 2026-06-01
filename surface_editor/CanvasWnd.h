#pragma once

#include <windows.h>
#include <string>
#include <functional>

// ---------------------------------------------------------------------------
// CanvasWnd.h  —  owner-drawn child window used by both editors
//
// Modes:
//   SurfaceEdit  — Shows all widgets from a Surface.  Widgets can be dragged
//                  to new grid cells.  Clicking a blank cell with the palette
//                  selection active creates a new widget there.
//
//   ZoneAssign   — Shows widgets from a Surface rendered with their current
//                  zone-action label.  Single-click fires OnWidgetClicked so
//                  the caller can open an action-picker dialog.
// ---------------------------------------------------------------------------

namespace SurfaceEditor {

struct Surface;
struct ZoneFile;

enum class CanvasMode {
    SurfaceEdit,
    ZoneAssign,
};

// Callbacks the canvas fires on user interaction
struct CanvasCallbacks {
    // SurfaceEdit: user dragged widget from (srcCol,srcRow) to canvas pixel (dstPixX,dstPixY)
    std::function<void(int srcCol, int srcRow, int dstPixX, int dstPixY)> onWidgetMoved;

    // SurfaceEdit: user clicked a blank cell (col, row) to place a new widget
    std::function<void(int col, int row)> onBlankCellClicked;

    // SurfaceEdit / ZoneAssign: user selected (clicked) a widget
    std::function<void(int col, int row)> onWidgetSelected;

    // ZoneAssign: user double-clicked a widget → open action picker
    std::function<void(int col, int row)> onWidgetActivated;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Create the canvas child window inside hParent.
// Returns the new HWND (or NULL on failure).
HWND CanvasWnd_Create(HWND hParent, HINSTANCE hInst, int x, int y, int w, int h);

// Attach data models.  Pass nullptr to clear.
void CanvasWnd_SetSurface(HWND hCanvas, Surface* surf);
void CanvasWnd_SetZoneFile(HWND hCanvas, ZoneFile* zone);

// Switch mode – repaints immediately
void CanvasWnd_SetMode(HWND hCanvas, CanvasMode mode);

// Set which palette type is "active" for new-widget placement in SurfaceEdit mode.
// type is one of "Button", "Fader", "Encoder", "Display", "VUMeter".
void CanvasWnd_SetPaletteSelection(HWND hCanvas, const char* type);

// Highlight a specific widget (selectedCol, selectedRow) – use (-1,-1) to clear.
void CanvasWnd_SetSelection(HWND hCanvas, int col, int row);

// Register callbacks
void CanvasWnd_SetCallbacks(HWND hCanvas, const CanvasCallbacks& cb);

// Zoom factor (1.0 = default cell size of 80×50 px)
void CanvasWnd_SetZoom(HWND hCanvas, float zoom);

// Force a repaint
void CanvasWnd_Refresh(HWND hCanvas);

// Destroy and clean up (called before the parent is destroyed)
void CanvasWnd_Destroy(HWND hCanvas);

// Window class name — register once at startup
bool CanvasWnd_RegisterClass(HINSTANCE hInst);

} // namespace SurfaceEditor
