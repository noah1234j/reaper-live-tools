#pragma once

#include <windows.h>
#include <string>

// ---------------------------------------------------------------------------
// ActionSearchDlg.h  —  modal action picker dialog
//
// Shows two tabs:
//   "REAPER Actions"  — live list from kbd_enumerateActions (main section)
//   "CSI Actions"     — hard-coded CSI virtual action list
//
// A search/filter edit box narrows both lists in real time.
// A free-form text field at the bottom lets the user type any custom action.
//
// Returns the selected action string, or "" if cancelled.
// ---------------------------------------------------------------------------

// Initialize the dialog (call once at plugin load to register the dialog).
void ActionSearchDlg_Init(HINSTANCE hInst);

// Show the dialog modally.
// parent  — parent HWND
// initial — pre-fill the custom field with this string (pass "" for blank)
// Returns the chosen action string, or "" if the user cancelled.
std::string ActionSearchDlg_Show(HWND parent, const std::string& initial = "");
