#pragma once

// ---------------------------------------------------------------------------
// ReaperTheme - colours from the active REAPER theme for the Scenes windows.
//
// Dialog backgrounds, labels and edit boxes are not coloured here at all:
// ReaperTheme_CtlColor hands WM_CTLCOLOR* to REAPER's main window and returns
// REAPER's own brush, the same way SWS themes its windows.
//
// List views take the theme's "genlist_*" colours. Selected rows are painted
// by ReaperTheme_ListItemPrePaint from NM_CUSTOMDRAW rather than by Windows,
// so the selection colour comes from the theme too. No uxtheme dark-mode
// styles are applied to list views or headers.
//
// Colours are cached; ReaperTheme_Refresh re-reads them (cheap) and bumps
// ReaperTheme_Generation when anything changed, so an open window can
// re-apply after the user switches theme.
// ---------------------------------------------------------------------------

#ifdef _WIN32
#  include <windows.h>
#  include <commctrl.h>
#else
#  include "WDL/swell/swell.h"
#endif

struct ReaperListColors
{
    COLORREF bg;
    COLORREF fg;
    COLORREF selBg;       // selected, list focused
    COLORREF selFg;
    COLORREF selInBg;     // selected, list not focused
    COLORREF selInFg;
    COLORREF muted;       // secondary text (spacer rows), readable on bg
};

// Re-read the theme. Returns true (and bumps the generation) on a change.
bool ReaperTheme_Refresh();
int  ReaperTheme_Generation();

// A Win32 system colour index (COLOR_BTNFACE, COLOR_BTNTEXT, ...) as the
// REAPER theme defines it. Falls back to GetSysColor.
COLORREF ReaperTheme_Sys(int sysIdx);

const ReaperListColors& ReaperTheme_List();

// Call first thing in a dialog proc. Non-zero means handled: return it.
INT_PTR ReaperTheme_CtlColor(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);

// Once from WM_INITDIALOG: drops the visual style from checkboxes, radio
// buttons and group boxes so their text follows the theme (Windows only).
void ReaperTheme_ApplyDialog(HWND hDlg);

// Sets a list view's background and text colours from the theme.
void ReaperTheme_ApplyListView(HWND hList);

// From CDDS_ITEMPREPAINT: paints the row in theme colours, including the
// selection, and clears the selected/focus state so Windows doesn't draw its
// own highlight over it. `muted` uses the secondary text colour.
void ReaperTheme_ListItemPrePaint(NMLVCUSTOMDRAW* cd, HWND hList, bool muted);

// Background a custom-drawn cell should use for the given row.
COLORREF ReaperTheme_ListCellBg(HWND hList, int row);
