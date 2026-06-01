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
#define IDC_GSET_PRELOAD_OFFLINE 2245   // "Preload new plugins offline" checkbox

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

// ---- CSI (Control Surface Integrator) resource IDs -----------------------
#define ID_RemoveListener               3
#define ID_RemoveBroadcaster            4
#define ID_BUTTON_AddBroadcaster        5
#define ID_BUTTON_AddListener           6
#define IDD_DIALOG_Page                 103
#define IDD_DIALOG_LearnFX              117
#define IDD_DIALOG_LearnFXLevel2        118
#define IDD_DIALOG_LearnFXLevel3        119
#define IDD_DIALOG_AdvancedSetup        123
#define IDD_DIALOG_EditAdvanced         126
#define IDD_DIALOG_EditFXAlias          129
#define IDD_DIALOG_AdvancedSharing      130
#define IDD_SURFACEEDIT_CSI             265
#define IDD_DIALOG_MidiSurface          268
#define IDD_DIALOG_PageSurface          270
#define IDC_EDIT_PageName               1003
#define IDC_COMBO_MidiIn                1020
#define IDC_COMBO_MidiOut               1021
#define IDC_EDIT_MidiSurfaceName        1022
#define IDC_EDIT_MidiSurfaceRefreshRate 1023
#define IDC_LIST_Pages                  1024
#define IDC_EDIT_MidiSurfaceMaxSysExMessagesPerRun 1024
#define IDC_BUTTON_AddPage              1025
#define IDC_BUTTON_EditPage             1026
#define IDC_BUTTON_RemovePage           1027
#define IDC_LIST_PageSurfaces           1028
#define IDC_BUTTON_AddPageSurface       1029
#define IDC_BUTTON_EditPageSurface      1030
#define IDC_BUTTON_RemovePageSurface    1031
#define IDC_LIST_Surfaces               1032
#define IDC_BUTTON_AddMidiSurface       1033
#define IDC_BUTTON_EditSurface          1034
#define IDC_BUTTON_RemoveSurface        1035
#define IDC_BUTTON_Advanced             1037
#define IDC_EDIT_NumChannels            1085
#define IDC_EDIT_ChannelOffset          1088
#define IDC_COMBO_PageSurface           1089
#define IDC_COMBO_PageSurfaceFolder     1091
#define IDC_COMBO_Surface               1092
#define IDC_CHECK_SynchPages            1093
#define IDC_CHECK_ScrollLink            1094
#define IDC_CHECK_ScrollSynch           1096
#define IDC_RADIO_MCP                   1097
#define IDC_RADIO_TCP                   1098
#define IDC_FXAlias                     1102
#define IDC_DeepEdit                    1104
#define IDC_Params                      1105
#define IDC_Advanced                    1106
#define IDC_EditSteps                   1107
#define IDC_PickSteps                   1108
#define IDC_PickRingStyle               1143
#define IDC_FXParamNameEdit             1149
#define IDC_Edit_FixedTextDisplayTop    1150
#define IDC_Edit_FixedTextDisplayBottom 1151
#define IDC_Edit_ParamValueDisplayTop   1152
#define IDC_Edit_ParamValueDisplayBottom 1153
#define IDC_AssignWidgetDisplay         1154
#define IDC_AssignFXParamDisplay        1155
#define IDC_FXParamValueDisplayPickFont 1163
#define IDC_FixedTextDisplayPickFont    1166
#define IDC_GroupFXParamValues          1170
#define IDC_FXParamRingColor            1173
#define IDC_FXParamIndicatorColor       1174
#define IDC_FixedTextDisplayForegroundColor 1179
#define IDC_FixedTextDisplayBackgroundColor 1182
#define IDC_FXParamDisplayForegroundColor 1185
#define IDC_FXParamDisplayBackgroundColor 1188
#define IDC_Save                        1190
#define IDC_Unassign                    1191
#define IDC_Alias                       1192
#define IDC_Assign                      1193
#define IDC_Done                        1194
#define IDC_FXParamRingColorBox         1205
#define IDC_FXParamIndicatorColorBox    1208
#define IDC_FXFixedTextDisplayForegroundColorBox 1211
#define IDC_FXFixedTextDisplayBackgroundColorBox 1214
#define IDC_FXParamValueDisplayBackgroundColorBox 1218
#define IDC_FXParamValueDisplayForegroundColorBox 1221
#define IDC_StepsPromptGroup            1231
#define IDC_LIST_Listeners              1246
#define IDC_LIST_Broadcasters           1247
#define IDC_EDIT_Delta                  1248
#define IDC_EDIT_RangeMin               1249
#define IDC_EDIT_RangeMax               1250
#define IDC_EDIT_DeltaValues            1251
#define IDC_EDIT_TickValues             1252
#define IDC_ListenCheckboxes            1253
#define IDC_CHECK_GoHome                1254
#define IDC_CHECK_SelectedTrackFX       1255
#define IDC_CHECK_Sends                 1256
#define IDC_CHECK_Receives              1257
#define IDC_CHECK_FXMenu                1260
#define IDC_AddBroadcaster              1261
#define IDC_AddListener                 1262
#define IDC_CHECK_Modifiers             1267
#define IDC_CHECK_ShowRawInput          1268
#define IDC_CHECK_ShowInput             1269
#define IDC_CHECK_ShowOutput            1270
#define IDC_CHECK_WriteFXParams         1271
#define IDC_EDIT_DebugLevel             1272
#define IDC_LABEL_DebugLevel            1273
#define IDC_COMBO_Type                  1275
#define IDC_AcceleratedTickValuesLabel  1277
#define IDC_AcceleratedDeltaValuesLabel 1278
#define IDC_RangeMaximumLabel           1279
#define IDC_RangeMinimumLabel           1280
#define IDC_DeltaValueLabel             1281
#define IDC_Steps                       1283
#define IDC_COMBO_PickNameDisplay       1292
#define IDC_COMBO_PickValueDisplay      1293
#define IDC_COMBO_PickAutoResetWidget   1294
#define IDC_ApplyColorsToAll            1295
#define IDC_EDIT_FXAlias                1300
#define IDC_SurfaceName                 1304
#define IDC_ApplyFontsAndMarginsToAll   1307
#define IDC_GroupApplyToAll             1308
#define IDC_EDIT_FREE_FORM              1309
#define IDC_EraseControl                5
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

// ---- CSI Surface & Zone Editor (3400+) ------------------------------------
// Main docked window (IDD not used – created programmatically)
#define IDC_SE_TREE              3400   // TreeView: CSI file browser (left pane)
#define IDC_SE_SPLITTER          3401   // splitter divider placeholder
// Surface editor sub-panel
#define IDC_SE_CANVAS            3402   // CanvasWnd placeholder (owner-draw panel)
#define IDC_SE_PALETTE_LIST      3403   // ListBox: widget type palette
#define IDC_SE_PROP_NAME         3404   // Edit: selected widget name
#define IDC_SE_PROP_GENTYPE      3405   // ComboBox: generator type
#define IDC_SE_PROP_BYTE0        3406   // Edit: generator byte 0 (status)
#define IDC_SE_PROP_BYTE1        3407   // Edit: generator byte 1 (note/CC)
#define IDC_SE_PROP_BYTE2        3408   // Edit: generator byte 2 (value)
#define IDC_SE_PROP_TOUCH        3409   // CheckBox: has Touch sub-message
#define IDC_SE_PROP_TOUCH_B0     3410   // Edit: Touch byte 0
#define IDC_SE_PROP_TOUCH_B1     3411   // Edit: Touch byte 1
#define IDC_SE_PROP_TOUCH_B2     3412   // Edit: Touch byte 2
#define IDC_SE_PROP_TOGGLE       3413   // CheckBox: has Toggle sub-message
#define IDC_SE_PROP_TOG_B0       3414   // Edit: Toggle byte 0
#define IDC_SE_PROP_TOG_B1       3415   // Edit: Toggle byte 1
#define IDC_SE_PROP_TOG_B2       3416   // Edit: Toggle byte 2
#define IDC_SE_PROP_FBTYPE       3417   // ComboBox: feedback processor type
#define IDC_SE_PROP_FB_B0        3418   // Edit: FB byte 0
#define IDC_SE_PROP_FB_B1        3419   // Edit: FB byte 1
#define IDC_SE_PROP_FB_B2        3420   // Edit: FB byte 2
#define IDC_SE_PROP_DELETE       3421   // Button: delete selected widget
#define IDC_SE_NEW_BTN           3422   // Button: new surface
#define IDC_SE_OPEN_BTN          3423   // Button: open Surface.txt
#define IDC_SE_SAVE_BTN          3424   // Button: save Surface.txt
#define IDC_SE_CHANNELS_EDIT     3425   // Edit: channel count
#define IDC_SE_CHANNELS_SPIN     3426   // Spin: channel count
#define IDC_SE_STATUS            3427   // Static: status/filename label
// Zone editor sub-panel
#define IDC_ZE_ZONE_NAME         3430   // Edit: zone name
#define IDC_ZE_ZONE_TYPE         3431   // ComboBox: zone type (Home/Track/etc.)
#define IDC_ZE_INC_LIST          3432   // ListBox: IncludedZones
#define IDC_ZE_INC_ADD           3433   // Button: add to IncludedZones
#define IDC_ZE_INC_REMOVE        3434   // Button: remove from IncludedZones
#define IDC_ZE_SUB_LIST          3435   // ListBox: SubZones
#define IDC_ZE_SUB_ADD           3436   // Button: add to SubZones
#define IDC_ZE_SUB_REMOVE        3437   // Button: remove from SubZones
#define IDC_ZE_MOD_SHIFT         3438   // CheckBox: filter – Shift modifier
#define IDC_ZE_MOD_OPTION        3439   // CheckBox: filter – Option modifier
#define IDC_ZE_MOD_CONTROL       3440   // CheckBox: filter – Control modifier
#define IDC_ZE_MOD_ALT           3441   // CheckBox: filter – Alt modifier
#define IDC_ZE_MOD_FLIP          3442   // CheckBox: filter – Flip modifier
#define IDC_ZE_MOD_HOLD          3443   // CheckBox: filter – Hold modifier
#define IDC_ZE_CANVAS            3444   // CanvasWnd: zone assign visual view
#define IDC_ZE_TABLE             3445   // ListView: zone assign table view
#define IDC_ZE_TAB               3446   // TabControl: Visual / Table
#define IDC_ZE_ADD_ROW           3447   // Button: add assignment row (table tab)
#define IDC_ZE_DEL_ROW           3448   // Button: delete selected row (table tab)
#define IDC_ZE_NEW_BTN           3449   // Button: new zone file
#define IDC_ZE_OPEN_BTN          3450   // Button: open .zon file
#define IDC_ZE_SAVE_BTN          3451   // Button: save .zon file
#define IDC_ZE_STATUS            3452   // Static: status/filename label
// Action search dialog (modal)
#define IDD_ACTION_SEARCH        240
#define IDC_AS_TAB               3460   // TabControl: REAPER Actions / CSI Actions
#define IDC_AS_SEARCH            3461   // Edit: filter text box
#define IDC_AS_LIST              3462   // ListView: action list
#define IDC_AS_CUSTOM            3463   // Edit: free-form custom action entry
#define IDC_AS_OK                3464   // Button: OK / assign
#define IDC_AS_CANCEL            3465   // Button: Cancel
// New Surface wizard dialog (modal)
#define IDD_NEW_SURFACE          241
#define IDC_NS_NAME              3470   // Edit: surface name
#define IDC_NS_CHANNELS          3471   // Edit: channel count
#define IDC_NS_CHANNELS_SPIN     3472   // Spin: channel count
#define IDC_NS_ROWS              3473   // Edit: grid rows
#define IDC_NS_ROWS_SPIN         3474   // Spin: grid rows
#define IDC_NS_OK                3475   // Button: OK
#define IDC_NS_CANCEL            3476   // Button: Cancel

// ---- ZoneEditorWnd aliases (alternate names used in ZoneEditorWnd.cpp) ----
#define IDC_ZE_INC_DEL           IDC_ZE_INC_REMOVE   // = 3434
#define IDC_ZE_SUB_DEL           IDC_ZE_SUB_REMOVE   // = 3437
#define IDC_ZE_SAVE              IDC_ZE_NEW_BTN       // = 3449
#define IDC_ZE_ROW_ADD           IDC_ZE_ADD_ROW       // = 3447
#define IDC_ZE_ROW_DEL           IDC_ZE_DEL_ROW       // = 3448

// ---- SurfaceEditorWnd redesign — toolbar dropdowns + zone bar (3480+) ----
#define IDC_SE_REFRESH_BTN       3480   // Button: refresh / rescan surfaces
#define IDC_SE_SURFACE_LABEL     3481   // Static: "Surface:" toolbar label
#define IDC_SE_SURFACE_COMBO     3482   // ComboBox: surface selector
#define IDC_SE_ZONE_LABEL        3483   // Static: "Zone:" toolbar label
#define IDC_SE_ZONE_COMBO        3484   // ComboBox: zone selector (+ "Surface Layout" sentinel)
#define IDC_SE_ZONE_SAVE_BTN     3485   // Button: save current zone file (toolbar)
// Zone bottom bar controls
#define IDC_SE_ZONE_NAME_LBL     3486   // Static: "Zone:" label in zone bar
#define IDC_SE_ZONE_NAME         3487   // Edit: zone name
#define IDC_SE_ZONE_TYPE_LBL     3488   // Static: "Type:" label in zone bar
#define IDC_SE_ZONE_TYPE         3489   // ComboBox: zone type (Home/Track/etc.)
#define IDC_SE_ZONE_MOD_SHIFT    3490   // CheckBox: Shift modifier filter
#define IDC_SE_ZONE_MOD_OPTION   3491   // CheckBox: Option modifier filter
#define IDC_SE_ZONE_MOD_CONTROL  3492   // CheckBox: Control modifier filter
#define IDC_SE_ZONE_MOD_ALT      3493   // CheckBox: Alt modifier filter
#define IDC_SE_ZONE_MOD_FLIP     3494   // CheckBox: Flip modifier filter
#define IDC_SE_ZONE_MOD_HOLD     3495   // CheckBox: Hold modifier filter
#define IDC_SE_ZONE_INC_LBL      3496   // Static: "Included:" label
#define IDC_SE_ZONE_INC          3497   // Edit: included zones (comma-separated)
#define IDC_SE_ZONE_SUB_LBL      3498   // Static: "Sub zones:" label
#define IDC_SE_ZONE_SUB          3499   // Edit: sub zones (comma-separated)
// Surface bottom bar labels
#define IDC_SE_PROP_NAME_LBL     3500   // Static: "Name:" label in prop bar

// ---- CSI newer surface config dialog (live_tools/csurf) -------------------
// IDD_SURFACEEDIT_CSI is updated to a single-ListView layout
#define IDC_SURF_LIST            1310   // ListView: surfaces in main config
#define IDC_SURF_ADD             1311   // Button: add surface
#define IDC_SURF_ADVANCED        1312   // Button: advanced settings
// IDD_DIALOG_SurfaceEdit – combined add/edit surface popup
#define IDD_DIALOG_SurfaceEdit   272
#define IDC_SE_NAME              1313   // Edit: surface name
#define IDC_SE_CHANNELS          1314   // Edit: channel count
#define IDC_SE_MIDI_IN           1315   // ComboBox: MIDI In port
#define IDC_SE_MIDI_OUT          1316   // ComboBox: MIDI Out port
#define IDC_SE_ZONE_FOLDER       1317   // ComboBox: zone folder
#define IDC_SE_FX_ZONE_FOLDER    1318   // ComboBox: FX zone folder
#define IDC_SE_REFRESH_RATE      1319   // Edit: refresh rate (Hz)
#define IDC_SE_MAX_SYSEX         1320   // Edit: max sysex messages per run
#define IDC_SE_MOTORIZED         1321   // CheckBox: motorized faders
#define IDC_SE_INFER_TOUCH       1322   // CheckBox: infer touch from fader activity

// ---- Surface Monitor window ----------------------------------------------
#define IDD_SURFACE_MONITOR      229
#define IDC_SURF_MON_EDIT        3500   // Read-only multiline edit (log output)
#define IDC_SURF_MON_CLEAR       3501   // "Clear" button
#define IDC_SURF_MON_DIAG        3502   // "Run Diagnostics" button
