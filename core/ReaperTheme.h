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

// Window background: the mixer's (col_mixerbg) in Dark mode, the Windows
// dialog colour otherwise. Custom-painted parts of a window (headers,
// splitters, grips) fill with this so they sit on the same background.
COLORREF ReaperTheme_DialogBg();

// "Dark mode" (Global Settings; ExtState key match_theme_ui). On, windows are
// painted in the REAPER theme's mixer colours. Off, they keep the plain
// Windows look of REAPER's Preferences: system colours and native controls.
// Saved to REAPER's ExtState; setting it re-reads the colours and restyles the
// open windows (ReaperTheme_ReapplyAll).
bool ReaperTheme_MatchTheme();
void ReaperTheme_SetMatchTheme(bool on);

// Text colour for dialog labels, painted buttons and tabs: the list text
// in Dark mode, the Windows dialog text otherwise.
COLORREF ReaperTheme_Text();

// List column header face: a step off the window background. Headers draw
// dividers in ReaperTheme_Line, like the grid.
COLORREF ReaperTheme_HeaderBg();

// Frames, grid lines and header dividers: black in Dark mode, the Windows
// shadow colour otherwise.
COLORREF ReaperTheme_Line();

// Call first thing in a dialog proc. Non-zero means handled: return it.
INT_PTR ReaperTheme_CtlColor(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);

// Once from WM_INITDIALOG. In Dark mode, drops the visual style from buttons,
// checkboxes, group boxes, tabs, edits and dropdowns and paints them in theme
// colours; otherwise leaves (or puts back) the native controls. Windows only.
void ReaperTheme_ApplyDialog(HWND hDlg);

// Restyles every open dialog that went through ReaperTheme_ApplyDialog (and
// the list views in it) for the current Dark mode setting. Called by
// ReaperTheme_SetMatchTheme.
void ReaperTheme_ReapplyAll();

// Sets a list view's background and text colours from the theme. On Windows,
// a list with LVS_EX_GRIDLINES gets them drawn in ReaperTheme_Line instead of the
// system's light grey (the native style is stripped, now and if set later).
void ReaperTheme_ApplyListView(HWND hList);

// From CDDS_ITEMPREPAINT: paints the row in theme colours, including the
// selection, and clears the selected/focus state so Windows doesn't draw its
// own highlight over it. `muted` uses the secondary text colour.
void ReaperTheme_ListItemPrePaint(NMLVCUSTOMDRAW* cd, HWND hList, bool muted);

// Background a custom-drawn cell should use for the given row.
COLORREF ReaperTheme_ListCellBg(HWND hList, int row);

// Text colour to go with ReaperTheme_ListCellBg.
COLORREF ReaperTheme_ListCellFg(HWND hList, int row);
