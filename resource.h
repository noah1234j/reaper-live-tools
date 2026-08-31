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

// MIDI Surface dialog
#define IDC_CHECK_MotorizedFaders 2131
#define IDC_CHECK_InferTouch      2132

// ---- New snapshot editor / transition controls -----------------------------
#define IDC_INSTANT              2131   // "Instant" checkbox (in settings popup)
#define IDC_TAPER                2132   // taper law combobox (in settings popup)
#define IDC_TAPER_CUSTOM         2133   // custom taper exponent edit (in settings popup)
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
#define IDC_SNAPNAME             2145   // scene title edit box (main Scenes window right panel)
#define IDC_LAYERS_BTN           2146   // "Layers..." button (main Scenes window right panel)
#define IDC_LAYER_STATUS         2147   // current layer indicator label (main Scenes window)

// ---- Scene settings popup dialog -----------------------------------------
#define IDD_SNAP_SETTINGS        213

// ---- Safes dialog ----------------------------------------------------------
#define IDD_SAFES                202
#define IDC_SAFESLIST            2200
#define IDC_REFRESH_SAFES        2201
#define IDC_CLEAR_SAFES          2202

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
#define IDC_GSAFE_ALL            2222

// ---- Duration debug checkbox (Global Settings) ---------------------------
#define IDC_GSET_DURATION_DEBUG  2254   // "Duration debug" checkbox
#define IDC_GSET_SHADOW_PARAMS      2255   // "Shadow VST3 params" checkbox
#define IDC_GSET_CHUNK_ALL_INSTANT  2256   // "Chunk all on instant path" checkbox

// ---- Scenes window dock toggle + marker option ----------------------------
// (2145-2147 now used – see IDC_SNAPNAME etc. above)
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
#define IDC_GSET_PRELOAD_OFFLINE 2245   // "Preload new plugins offline" checkbox
#define IDC_GSET_SKIP_UNCHANGED  2246   // "Skip writing unchanged params on recall" checkbox
#define IDC_GSET_CHUNK_BTN       2247   // "Chunk Recall Plugins..." button in global settings
#define IDC_GSET_LAYERS_BTN      2253   // "Layers..." button in global settings
#define IDC_GSET_STOP_REC_BEFORE 2257   // "Stop recording before recall" checkbox
#define IDC_GSET_START_REC_AFTER 2258   // "Start recording after recall" checkbox

// ---- Chunk Recall Plugins dialog ------------------------------------------
#define IDD_CHUNK_RECALL_PLUGINS 273
#define IDC_CRP_LIST             2248   // ListBox of keywords
#define IDC_CRP_ADD              2249   // "Add..." button
#define IDC_CRP_REMOVE           2250   // "Remove" button
#define IDC_CRP_NOTIFY           2252   // "Notify on save" debug checkbox

// ---- Add Keyword dialog (text input prompt) --------------------------------
#define IDD_ADD_KEYWORD          274
#define IDC_AK_EDIT              2251   // edit control for keyword input

// ---- Live Optimizer window ------------------------------------------------
#define IDD_LIVE_OPTIMIZE        208
#define IDC_LO_SCORE_BAR         2500   // owner-draw static – colored score bar
#define IDC_LO_SCORE_TEXT        2501   // score label text
#define IDC_LO_LIST              2502   // SysListView32 placeholder
#define IDC_LO_INFO              2503   // multi-line info / tooltip text
#define IDC_LO_REFRESH           2504   // "Refresh" button
#define IDC_LO_APPLY_FIX         2505   // "Apply Fix" button
#define IDC_LO_STATUS            2506   // status label at bottom

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

// ---- Monitor RT-CPU thresholds (added after initial IO/PDC/RT rows) ------
#define IDC_MON_RC_YEL           2513   // RT CPU yellow threshold
#define IDC_MON_RC_ORA           2514   // RT CPU orange threshold
#define IDC_MON_RC_RED           2515   // RT CPU red threshold

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
#define IDC_LYR_SET_TRIGGERMCP   2828
#define IDC_LYR_SET_TARGET_MCP   2829
#define IDC_LYR_SET_TARGET_TCP   2830

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

#define IDC_SE_REFRESH_RATE      1319   // Edit: refresh rate (Hz)
#define IDC_SE_MAX_SYSEX         1320   // Edit: max sysex messages per run
#define IDC_SE_MOTORIZED         1321   // CheckBox: motorized faders
#define IDC_SE_INFER_TOUCH       1322   // CheckBox: infer touch from fader activity

// ---- Surface Monitor window ----------------------------------------------
#define IDD_SURFACE_MONITOR      229
#define IDC_SURF_MON_EDIT        3500   // Read-only multiline edit (log output)
#define IDC_SURF_MON_CLEAR       3501   // "Clear" button
#define IDC_SURF_MON_DIAG        3502   // "Run Diagnostics" button
