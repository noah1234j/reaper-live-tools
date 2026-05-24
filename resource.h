#pragma once

// ---- Dialogs ---------------------------------------------------------------
#define IDD_TSNAPS               201

// ---- Controls (range 2100+, no overlap with SWS IDs) ----------------------
#define IDC_LIST                 2100
#define IDC_SAVE                 2101   // "New" button
#define IDC_TESTBTN              2199   // build-time stamp (temporary)
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
#define IDC_INSTANT              2131   // "Instant" checkbox (in settings popup)
#define IDC_TAPER                2132   // taper law combobox (in settings popup)
#define IDC_TAPER_CUSTOM         2133   // custom taper exponent edit (in settings popup)
#define IDC_SNAPNAME             2134   // snapshot name edit
#define IDC_SNAPNOTES            2135   // snapshot notes multiline edit (in settings popup)
#define IDC_COPY_SNAP            2136   // copy snapshot button (context menu only)
#define IDC_PASTE_SNAP           2137   // paste snapshot button (context menu only)
#define IDC_OVERWRITE_BTN        2138   // overwrite selected scene button (context menu only)
#define IDC_DELETE_BTN           2139   // delete selected scene button (context menu only)
#define IDC_MODE_SCENES          2140   // "Scenes" mode toggle button
#define IDC_MODE_CUE             2141   // "Cue List" mode toggle button
#define IDC_SNAP_LAYER           2142   // layer assignment combobox
#define IDC_RECALL_LAYERS        2143   // "Recall layer with scene" checkbox
#define IDC_SETTINGS_BTN         2144   // "Settings..." button (opens scene settings popup)

// ---- Scene settings popup dialog -----------------------------------------
#define IDD_SNAP_SETTINGS        213

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
#define IDD_MONITOR_SETTINGS     212

// Live Monitor threshold-settings controls
#define IDC_MON_CPU_YEL          2500
#define IDC_MON_CPU_ORA          2501
#define IDC_MON_CPU_RED          2502
#define IDC_MON_IO_YEL           2503
#define IDC_MON_IO_ORA           2504
#define IDC_MON_IO_RED           2505
#define IDC_MON_PDC_YEL          2506
#define IDC_MON_PDC_ORA          2507
#define IDC_MON_PDC_RED          2508
#define IDC_MON_RT_YEL           2509
#define IDC_MON_RT_ORA           2510
#define IDC_MON_RT_RED           2511
#define IDC_MON_RESET            2512
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
#define IDC_GSAFE_LAYERS         2221

// ---- Scenes window dock toggle + marker option ----------------------------
#define IDC_DOCK_BTN             2145   // (unused, reserved)
#define IDC_MARKER_BTN           2222
#define IDC_CUE_SETUP_BTN        2223   // "Cue Setup..." button

// ---- Global settings dialog -----------------------------------------------
#define IDD_GLOBAL_SETTINGS      227
#define IDC_GSET_INSTANT         2230
#define IDC_GSET_DURATION        2231
#define IDC_GSET_TAPER           2232
#define IDC_GSET_TAPER_CUSTOM    2233

// ---- Cue list setup dialog -------------------------------------------------
#define IDD_CUE_SETUP            228
#define IDC_CUE_LEFT_LIST        2234
#define IDC_CUE_RIGHT_LIST       2235
#define IDC_CUE_ADD              2236
#define IDC_CUE_REMOVE           2237
#define IDC_CUE_MOVE_UP          2238
#define IDC_CUE_MOVE_DOWN        2239
#define IDC_GSET_MARKER          2240   // "Place marker on recall" checkbox in global settings
#define IDC_GSET_SINGLE_CLICK    2241   // "Single click to recall" checkbox in global settings
#define IDC_GSET_ALT_DELETE      2242   // "Alt+click to delete" checkbox in global settings
#define IDC_GSET_CTRL_OVERWRITE  2243   // "Ctrl+click to overwrite" checkbox in global settings
#define IDC_GSET_KEEP_FX_WINDOWS 2244   // "Leave FX windows open during recall" checkbox

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

// ---- Meter Bridge window --------------------------------------------------
#define IDD_METERBRIDGE          209
#define IDC_MB_SCROLL            2600

// ---- Live Lock window -----------------------------------------------------
#define IDD_LIVELOCK             214
#define IDD_LIVELOCK_SETTINGS    215

// Main panel controls
#define IDC_LL_LOCK              2700   // owner-drawn toggle button (green/red)
#define IDC_LL_STATUS            2701   // status label (active categories)
#define IDC_LL_SETTINGS          2702   // "Settings..." button
#define IDC_LL_REVERTS           2703   // revert counter label
#define IDC_LL_WARN              2704   // CPU warning note

// Settings dialog controls
#define IDC_LL_CHK_ROUTING       2710   // protect track sends
#define IDC_LL_CHK_SELONLY       2711   // selected tracks only
#define IDC_LL_CHK_HWOUT         2712   // hardware outputs
#define IDC_LL_CHK_MASTERSEND    2713   // master send toggle
#define IDC_LL_CHK_FXBYPASS      2714   // FX bypass states
#define IDC_LL_CHK_RECARM        2715   // record arm / input monitoring
#define IDC_LL_CHK_CONFIRM       2716   // require confirmation on violation
#define IDC_LL_INTERVAL_EDIT     2717   // check interval edit box
#define IDC_LL_INTERVAL_SPIN     2718   // check interval spin control

// ---- Meter Bridge settings dialog ----------------------------------------
#define IDD_METERBRIDGE_SETTINGS 210
#define IDC_MB_STRIP_W           2601   // strip width edit
#define IDC_MB_STRIP_W_SPIN      2602   // strip width spin
#define IDC_MB_FONT_SIZE         2603   // font size edit
#define IDC_MB_FONT_SIZE_SPIN    2604   // font size spin
#define IDC_MB_NAME_H            2605   // name height edit
#define IDC_MB_NAME_H_SPIN       2606   // name height spin
#define IDC_MB_PEAKHOLD          2607   // peak hold edit
#define IDC_MB_PEAKHOLD_SPIN     2608   // peak hold spin
#define IDC_MB_FPS               2609   // refresh rate edit
#define IDC_MB_FPS_SPIN          2610   // refresh rate spin

// ---- Control Surface extender sub-dialog ---------------------------------
#define IDD_CSURF_EXTENDER_EDIT  211
#define IDC_CSURF_FOLLOW_LAYERS  2516   // follow Layers track order checkbox
#define IDC_CSURF_BTN_MAP_BTN    2517   // "Button Map..." button
#define IDC_CSURF_EXT_LIST       2518   // extenders list box
#define IDC_CSURF_EXT_ADD        2519   // Add extender button
#define IDC_CSURF_EXT_EDIT       2520   // Edit extender button
#define IDC_CSURF_EXT_REMOVE     2521   // Remove extender button
#define IDC_EXT_MIDI_IN          2522   // extender MIDI input combo
#define IDC_EXT_MIDI_OUT         2523   // extender MIDI output combo
#define IDC_EXT_CHAN_OFFSET      2524   // channel offset edit
#define IDC_EXT_CHAN_OFFSET_SPIN 2525   // channel offset spin
#define IDC_CSURF_SENDS_SPILL   2526   // "Sends spill receives" checkbox
#define IDC_CSURF_SENDS_ACTIVE  2527   // radio: shows active sends only
#define IDC_CSURF_SENDS_CREATE  2528   // radio: shows all channels, creates send on fader
#define IDC_CSURF_TOUCH_CHAN    2529   // Show Touched Channels in MCP checkbox
#define IDC_CSURF_DEBUG_LOG     2530   // Enable Debug Log checkbox
#define IDC_CSURF_EXPORT_TPL    2531   // Export Template button
#define IDC_CSURF_IMPORT_TPL    2532   // Import Template button
#define IDC_CSURF_DBG_OPEN      2533   // Open Debug Log button
#define IDC_CSURF_WIZARD_BTN    2534   // Setup Wizard button
// --- Per-port controls in extender edit dialog ---------------------------
#define IDC_EXT_PROTO_MCU       2535   // per-port MCU radio button
#define IDC_EXT_PROTO_HUI       2536   // per-port HUI radio button
#define IDC_EXT_PROTO_RAW       2537   // per-port Raw MIDI radio button
#define IDC_EXT_DEVICE_PRESET   2538   // per-port device preset combo
#define IDC_EXT_BTN_MAP         2539   // per-port Button Map button
// --- Debug log window ---------------------------------------------------
#define IDD_CSURF_DEBUG         230
#define IDC_CSURF_DBG_LIST      2540   // log list box (multiline edit)
#define IDC_CSURF_DBG_CLEAR     2541   // clear button
#define IDC_CSURF_DBG_COPY      2542   // copy-all button
// --- Setup Wizard dialog ------------------------------------------------
#define IDD_CSURF_WIZARD        231
#define IDC_WIZ_CONTENT         2543   // scrollable instructions (multiline edit)
#define IDC_WIZ_TMPL_LABEL      2544
#define IDC_WIZ_TMPL            2545   // template combo (step 3)
#define IDC_WIZ_IN_LABEL        2546
#define IDC_WIZ_MIDI_IN         2547   // MIDI in combo  (step 3)
#define IDC_WIZ_OUT_LABEL       2548
#define IDC_WIZ_MIDI_OUT        2549   // MIDI out combo (step 3)
#define IDC_WIZ_STEP            2550   // "Step N / 5" label
#define IDC_WIZ_BACK            2551
#define IDC_WIZ_NEXT            2552
// --- Button map import / export -----------------------------------------
#define IDC_BM_IMPORT           2553
#define IDC_BM_EXPORT           2554

// ---- Monitor RT-CPU thresholds (added after initial IO/PDC/RT rows) ------
#define IDC_MON_RC_YEL           2513   // RT CPU yellow threshold
#define IDC_MON_RC_ORA           2514   // RT CPU orange threshold
#define IDC_MON_RC_RED           2515   // RT CPU red threshold

// ---- PAFL (additional controls added after initial set) ------------------
#define IDC_PAFL_BUSTRACK        2409   // PAFL bus track combobox
#define IDC_PAFL_NEWBUS          2410   // New PAFL bus button
#define IDC_PAFL_ACTIVE          2411   // PAFL active toggle (push-like checkbox)

// ---- Layers window -------------------------------------------------------
#define IDD_LAYERS               216
#define IDD_LAYERS_SETTINGS      217
// Layers main dialog
#define IDC_LYR_LAYER_LIST       2800   // layer ListView placeholder
#define IDC_LYR_ADD_LAYER        2803
#define IDC_LYR_ADD_SPACER       2804   // add spacer track to selected layer
#define IDC_LYR_DELETE_LAYER     2805
#define IDC_LYR_PROP_GROUP       2806   // "Layer Properties" groupbox
#define IDC_LYR_NAME_EDIT        2807
#define IDC_LYR_MAXCH_EDIT       2809
#define IDC_LYR_MAXCH_SPIN       2810
#define IDC_LYR_TRACK_LIST       2811   // track ListView placeholder
#define IDC_LYR_ADD_TRACK        2812
#define IDC_LYR_REM_TRACK        2813
#define IDC_LYR_CAPTURE          2816
#define IDC_LYR_CLEAR_LAYER      2817
#define IDC_LYR_ACTIVATE         2818
#define IDC_LYR_PREV             2819
#define IDC_LYR_NEXT             2820
#define IDC_LYR_DEACTIVATE       2821
#define IDC_LYR_SETTINGS_BTN     2822
#define IDC_LYR_STATUS           2823
// Layers settings dialog
#define IDC_LYR_SET_MCPVIS       2824
#define IDC_LYR_SET_HIDETCP      2825
#define IDC_LYR_SET_REORDER      2826
#define IDC_LYR_SET_RESTORE      2827

// ---- Button Map window ---------------------------------------------------
#define IDD_BTN_MAP              218
#define IDD_BTN_ASSIGN           219
// BtnMap dialog
#define IDC_BM_SURFACE_LABEL     2900
#define IDC_BM_FILTER            2901
#define IDC_BM_RESETALL          2902
#define IDC_BM_LIST              2903
#define IDC_BM_ASSIGN            2904
#define IDC_BM_DEFAULT           2905
#define IDC_BM_DISABLE           2906
// BtnAssign dialog
#define IDC_BA_LABEL             2910
#define IDC_BA_DEF               2911
#define IDC_BA_NONE              2912
#define IDC_BA_CMD               2913
#define IDC_BA_CMDID             2914

// ---- Mute Groups window --------------------------------------------------
#define IDD_MUTEGROUPS           222
#define IDC_MG_GROUP_LIST        3000   // groups ListView placeholder
#define IDC_MG_TRACK_LIST        3001   // tracks ListView placeholder
#define IDC_MG_ADD_GROUP         3002
#define IDC_MG_RENAME            3003
#define IDC_MG_DELETE            3004
#define IDC_MG_MOVE_UP           3005
#define IDC_MG_MOVE_DOWN         3006
#define IDC_MG_ADD_TRACKS        3007
#define IDC_MG_REM_TRACK         3008
#define IDC_MG_TOGGLE            3009   // "Toggle Mute" button
#define IDC_MG_STATUS            3010   // status label

// ---- Plugin Safety window ------------------------------------------------
#define IDD_PLUGIN_SAFETY        223
#define IDC_PS_SCAN              3100   // "Scan" button
#define IDC_PS_FILTER            3101   // Filter combobox (All/Green/Yellow/Red/In Use)
#define IDC_PS_SEARCH            3102   // Search edit
#define IDC_PS_LIST              3103   // ListView placeholder
#define IDC_PS_DETAIL            3104   // Detail static label at bottom
#define IDC_PS_SHOW_WARNINGS     3105   // "Show warning icons in Scenes/Layouts" checkbox

// ---- DCA Groups window ---------------------------------------------------
#define IDD_DCA                  224
#define IDC_DCA_ADDSTRIP         3200   // "+ Add DCA" button
#define IDC_DCA_STARTGROUP       3201   // start group number edit box
#define IDC_DCA_DEF_VOL          3202   // default flag: Volume
#define IDC_DCA_DEF_PAN          3203   // default flag: Pan
#define IDC_DCA_DEF_WIDTH        3204   // default flag: Width
#define IDC_DCA_DEF_MUTE         3205   // default flag: Mute
#define IDC_DCA_DEF_SOLO         3206   // default flag: Solo
#define IDC_DCA_DEF_RECARM       3207   // default flag: Record Arm
#define IDC_DCA_SCROLL           3208   // strip area placeholder (legacy)
#define IDC_DCA_LIST             3209   // DCA Groups SysListView32

// ---- Talkback window (3300–3315) -----------------------------------------
#define IDD_TALKBACK             225
#define IDC_TB_TRACK             3300   // ComboBox: select existing TB track
#define IDC_TB_NEWTRACK          3301   // "New" button – create TB track
#define IDC_TB_INPUT             3302   // ComboBox: hardware input channel
#define IDC_TB_STEREO            3303   // Checkbox: stereo pair
#define IDC_TB_GAIN_SLIDER       3304   // Trackbar: mic gain
#define IDC_TB_GAIN_LABEL        3305   // Static: gain value text
#define IDC_TB_DESTLIST          3306   // SysListView32: destination tracks
#define IDC_TB_TALK              3307   // Button: TALKBACK toggle (pushlike)
#define IDC_TB_HIDEFADER         3308   // Checkbox: hide TB track fader
#define IDC_TB_AUTOSETUP         3309   // Checkbox: active on project startup
#define IDC_TB_AUTODIM           3310   // Checkbox: auto-dim master when active
#define IDC_TB_DIM_LABEL         3311   // Static: dim value text
#define IDC_TB_DIM_SLIDER        3312   // Trackbar: dim amount
#define IDC_TB_MODE_TOGGLE       3313   // Radio: toggle mode
#define IDC_TB_MODE_MOMENTARY    3314   // Radio: momentary mode
#define IDC_TB_ROUTE_BTN         3315   // Button: open routing popup
// Routing popup window
#define IDD_TALKBACK_ROUTING     226
#define IDC_TBR_LIST             3316   // SysListView32: destination tracks (routing popup)
