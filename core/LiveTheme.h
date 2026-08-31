#pragma once

// ---------------------------------------------------------------------------
// LiveTheme – shared light/dark theming for all live_tools windows.
//
// The active REAPER color theme drives everything: LiveTheme reads REAPER's
// themed system colors (GSC_mainwnd) and classifies the theme as light or
// dark by background luminance. Windows with hand-tuned palettes pick their
// dark or light variant off LiveTheme_IsLight(); standard dialogs use the
// themed colors directly so they match any REAPER theme.
//
// A central REAPER timer polls for theme switches (~1 Hz) and fires the
// registered per-window callbacks so open windows restyle live.
// ---------------------------------------------------------------------------

#ifdef _WIN32
#  include <windows.h>
#else
#  include "WDL/swell/swell.h"
#endif

// Call once from ReaperPluginEntry after REAPERAPI_LoadAPI succeeds.
void LiveTheme_Init();

// True when the active REAPER theme has a light background.
bool LiveTheme_IsLight();

// Bumped every time the theme flips light<->dark or its colors change.
// Windows can compare against a stored value instead of using a callback.
int LiveTheme_Generation();

// Pick a color by current mode.
inline COLORREF LiveTheme_Pick(COLORREF dark, COLORREF light)
{
    return LiveTheme_IsLight() ? light : dark;
}

// Themed system colors (from the REAPER theme, with GetSysColor fallback).
struct LiveThemeColors
{
    COLORREF dlgBg;      // dialog / button-face background
    COLORREF dlgText;    // dialog label text
    COLORREF editBg;     // edit / combo field background
    COLORREF editText;   // edit / combo field text
    COLORREF listBg;     // list view background
    COLORREF listText;   // list view text
    COLORREF listGrid;   // list grid / separator lines
    COLORREF hlBg;       // selection background
    COLORREF hlText;     // selection text
};
const LiveThemeColors& LiveTheme_Colors();

// Register a callback invoked (from the main thread) after the theme changes.
// Callbacks stay registered for the life of the plugin.
typedef void (*LiveThemeCallback)();
void LiveTheme_RegisterCallback(LiveThemeCallback cb);

// ---------------------------------------------------------------------------
// Dialog helpers
// ---------------------------------------------------------------------------

// Handle WM_CTLCOLORDLG / STATIC / BTN / EDIT / LISTBOX for a standard
// dialog so it uses the REAPER theme colors. Returns a brush (as INT_PTR)
// when handled, 0 when the message should fall through to the default proc.
//
//   case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
//   case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
//       if (INT_PTR r = LiveTheme_CtlColor(msg, wParam, lParam)) return r;
//       break;
INT_PTR LiveTheme_CtlColor(UINT msg, WPARAM wParam, LPARAM lParam);

// Apply themed background/text/header colors to a SysListView32.
void LiveTheme_ApplyListView(HWND hList);

// Apply the theme to a dialog and all its children: dark title bar,
// dark-mode control themes (buttons, scrollbars, combos) and list view
// colors. Call on WM_INITDIALOG and from the theme-change callback.
void LiveTheme_ApplyDialog(HWND hDlg);
