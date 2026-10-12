#pragma once

#ifdef _WIN32
#  include <windows.h>
#  include <shellapi.h>   // HDROP
#else
#  include "WDL/swell/swell.h"
#endif

class TransitionSnapshot;

// ---------------------------------------------------------------------------
// MusicSheetWnd - the window that shows a scene's music sheet, and the
// pencil, text and eraser that mark it up.
//
// It shows one scene's sheet at a time: the "owner", which is always a scene
// rather than a subscene (a subscene reads its parent's - see
// TransitionSnapshot::m_sheet). Callers hand it the owner; working out which
// scene that is stays with the Scenes window, which knows the list.
//
// It follows recalls: once open, recalling another song swaps the sheet in,
// and recalling a subscene of the song already showing leaves the page alone.
// Clicking the sidebar button points it at the selected scene instead.
//
// Like the Scenes window it can float or dock, and remembers which.
// ---------------------------------------------------------------------------

void MusicSheetWnd_Init(HINSTANCE hInstance);
void MusicSheetWnd_Cleanup();

// The action: open on the last-recalled scene's sheet, or close.
void MusicSheetWnd_ShowHide();
int  MusicSheetWnd_IsVisible();

// Open (or bring forward) on this scene's sheet.
void MusicSheetWnd_ShowScene(TransitionSnapshot* owner);

// A scene was recalled; `owner` holds its sheet (null for none). An open
// window follows it. A closed one opens only when `openIfAttached` and the
// owner actually has a sheet.
void MusicSheetWnd_OnSceneRecalled(TransitionSnapshot* owner, bool openIfAttached);

// The project was switched or reloaded: every scene pointer is stale.
void MusicSheetWnd_OnProjectReset();

// Page turns for foot pedals and other bindings, wherever the focus is.
void MusicSheetWnd_NextPage();
void MusicSheetWnd_PrevPage();

// Full-page mode: hide or show the toolbar (remembered between sessions).
void MusicSheetWnd_ToggleToolbar();

// ---------------------------------------------------------------------------
// Attaching - shared with the Scenes sidebar so both say the same things.
// Each returns true when it changed the scene; both create their own undo
// point and refresh the sidebar and the viewer.
// ---------------------------------------------------------------------------

// Pick a PDF or image and attach it. On a scene whose file has gone missing,
// this is "locate it": the annotations are kept.
//
// Either way the file is copied into a "Music Sheets" folder beside the .rpp
// and linked from there (unless it is already inside the project folder, or
// the project has never been saved), so the project carries its charts.
bool MusicSheetWnd_Attach(HWND parent, TransitionSnapshot* owner);

// A file dropped from Explorer (WM_DROPFILES): attach its first file to
// `owner` the same way, asking first if that replaces another sheet, then
// show it. Finishes the drop whatever happens.
bool MusicSheetWnd_AttachDrop(HWND parent, TransitionSnapshot* owner, HDROP drop);

// Remove the sheet, after confirming. Its annotations go with it.
bool MusicSheetWnd_Detach(HWND parent, TransitionSnapshot* owner);

// ---------------------------------------------------------------------------
// The sidebar button (IDC_SHEET_BTN), drawn here so its icons match the
// viewer's. `owner` may be null (nothing, or several scenes, selected).
// ---------------------------------------------------------------------------
void MusicSheetWnd_DrawSheetButton(const DRAWITEMSTRUCT* dis, const TransitionSnapshot* owner);

// True when a click at `ptClient` (button client coordinates) lands on the
// trash zone at the right end of an attached sheet's button.
bool MusicSheetWnd_SheetButtonTrashHit(HWND hBtn, POINT ptClient, const TransitionSnapshot* owner);
