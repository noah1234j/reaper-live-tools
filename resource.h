#pragma once

// ---- Dialogs ---------------------------------------------------------------
#define IDD_TSNAPS               201

// ---- Controls (range 2100+, no overlap with SWS IDs) ----------------------
#define IDC_LIST                 2100
#define IDC_SAVE                 2101   // "New" button
#define IDC_RECALL               2102   // "Recall" button
#define IDC_DURATION             2103   // duration edit box
#define IDC_PROGRESS             2104   // progress bar
#define IDC_STATUS               2105   // status static label

// Filter radio buttons
#define IDC_MIX                  2106
#define IDC_CUSTOM               2107

// Filter checkboxes
#define IDC_VOL                  2108
#define IDC_PAN                  2109
#define IDC_MUTE                 2110
#define IDC_SOLO                 2111
#define IDC_FXCHAIN              2112   // offline FX chain swap
#define IDC_SENDS                2113
#define IDC_VISIBILITY           2114
#define IDC_SELECTION            2115
#define IDC_PHASE                2116
#define IDC_PLAY_OFFSET          2117

// Options checkboxes
#define IDC_APPLYRECALL          2118
#define IDC_SELECTEDONLY_SAVE    2119
#define IDC_SELECTEDONLY_RECALL  2120
#define IDC_SHOWSELONLY          2121
#define IDC_NAMEPROMPT           2122
#define IDC_HIDENEW              2123

// Navigation buttons
#define IDC_PREVIOUS             2124
#define IDC_NEXT                 2125
#define IDC_SWAP_UP              2126
#define IDC_SWAP_DOWN            2127

// Group boxes
#define IDC_FILTERGROUP          2128
#define IDC_TRANSITION_GROUP     2129

// Safes button (in main dialog)
#define IDC_SAFES_BTN            2130

// ---- New snapshot editor / transition controls -----------------------------
#define IDC_INSTANT              2131   // "Instant" checkbox
#define IDC_TAPER                2132   // taper law combobox
#define IDC_TAPER_CUSTOM         2133   // custom taper exponent edit
#define IDC_SNAPNAME             2134   // snapshot name edit
#define IDC_SNAPNOTES            2135   // snapshot notes multiline edit
#define IDC_COPY_SNAP            2136   // copy snapshot button
#define IDC_PASTE_SNAP           2137   // paste snapshot button

// ---- Safes dialog ----------------------------------------------------------
#define IDD_SAFES                202
#define IDC_SAFESLIST            2200
#define IDC_REFRESH_SAFES        2201
#define IDC_CLEAR_SAFES          2202

// ---- Layouts dialog --------------------------------------------------------
#define IDD_LAYOUTS              203

#define IDC_LAY_LIST             2300   // ListView placeholder
#define IDC_LAY_CAPTURE          2301   // "Capture" button
#define IDC_LAY_RECALL           2302   // "Recall" button
#define IDC_LAY_PREV             2303   // Previous layout
#define IDC_LAY_NEXT             2304   // Next layout
#define IDC_LAY_UP               2305   // Move up in list
#define IDC_LAY_DOWN             2306   // Move down in list

// Settings checkboxes
#define IDC_LAY_ORDER            2310   // Track order
#define IDC_LAY_HEIGHT           2311   // Track heights / spacer sizes
#define IDC_LAY_VIS              2312   // TCP/MCP visibility

// ---- PAFL monitor window --------------------------------------------------
#define IDD_PAFL                 204

// ---- Live Monitor window --------------------------------------------------
#define IDD_MONITOR              207
#define IDC_PAFL_SRCTRACK        2400
#define IDC_PAFL_HWOUT           2401
#define IDC_PAFL_SENDTYPE        2402
#define IDC_PAFL_INTERCEPT       2403
#define IDC_PAFL_STATUS          2404
#define IDC_PAFL_INIT            2405
#define IDC_PAFL_CLEAR           2406
#define IDC_PAFL_AUTOSETUP       2407
#define IDC_PAFL_HIDEFADER       2408
#define IDC_LAY_NAMES            2313   // Track names

#define IDC_LAY_STATUS           2314   // Status label
#define IDC_LAY_SETTGRP          2315   // "Settings" group box
#define IDC_GLOBAL_SAFES_EN      2203   // (unused – kept for compat)
#define IDC_TRACK_SAFES_EN       2204   // enable per-track safes checkbox

// Global Safes groupbox + per-parameter toggles
#define IDC_GSAFES_GROUP         2209
#define IDC_GSAFE_VOL            2210
#define IDC_GSAFE_PAN            2211
#define IDC_GSAFE_MUTE           2212
#define IDC_GSAFE_SOLO           2213
#define IDC_GSAFE_PHASE          2214
#define IDC_GSAFE_FX             2215
#define IDC_GSAFE_VIS            2216
#define IDC_GSAFE_NAME           2217
#define IDC_GSAFE_COLOR          2218
#define IDC_GSAFE_HEIGHT         2219
#define IDC_GSAFE_ORDER          2220

// ---- Scenes window dock toggle + marker option ----------------------------
#define IDC_DOCK_BTN             2221
#define IDC_MARKER_BTN           2222

// ---- Live Optimizer window ------------------------------------------------
#define IDD_LIVE_OPTIMIZE        208
#define IDC_LO_SCORE_BAR         2500   // owner-draw static – colored score bar
#define IDC_LO_SCORE_TEXT        2501   // score label text
#define IDC_LO_LIST              2502   // SysListView32 placeholder
#define IDC_LO_INFO              2503   // multi-line info / tooltip text
#define IDC_LO_REFRESH           2504   // "Refresh" button
#define IDC_LO_APPLY_FIX         2505   // "Apply Fix" button
#define IDC_LO_STATUS            2506   // status label at bottom

// ---- Control Surface config dialog ----------------------------------------
#define IDD_CSURF_CONFIG         205
#define IDC_CSURF_TEMPLATE       2500
#define IDC_CSURF_PROTO_MCU      2501
#define IDC_CSURF_PROTO_HUI      2502
#define IDC_CSURF_MIDI_IN        2503
#define IDC_CSURF_MIDI_OUT       2504
#define IDC_CSURF_FOLLOW_SEL     2505
#define IDC_CSURF_SHOW_VU        2506
#define IDC_CSURF_SHOW_NAMES     2507
#define IDC_CSURF_FADER_VOL      2508
#define IDC_CSURF_FADER_PAN      2509
#define IDC_CSURF_FADER_SEND     2510
#define IDC_CSURF_BANK_OFFSET    2511
#define IDC_CSURF_CHAN_COUNT     2512
#define IDC_CSURF_DESC           2513
#define IDC_CSURF_SEND_COLORS   2514
#define IDC_CSURF_FOLLOW_MCP    2515

// ---- Control Surface standalone settings dialog ---------------------------
#define IDD_CSURF_STANDALONE     206
