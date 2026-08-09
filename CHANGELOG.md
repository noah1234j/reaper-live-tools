# Changelog

## [v0.0.24-beta] — 2026-08-09

### Bug Fixes

- **Scenes: Large plugin states (SSL Native, VMR, …) no longer lost across project reload**: Plugin state blobs (`vst_chunk`) were serialized into the project file as a single line of up to 200KB, but the scene parser read lines into a 4KB buffer — silently truncating and corrupting every blob over ~4KB when the project was reopened. On recall, the corrupt blob was applied, rejected by the plugin, and the plugin kept its previous settings (the "SSL/VMR settings not recalled" report — an in-session re-save appeared to fix it only because the in-memory copy was still intact). Blobs are now written as a length-prefixed multi-line `FXCHUNKSTART`/`FXCHUNKEND` block (256 chars per line) and verified against the declared length on load; a blob that fails verification is discarded (recall then falls back to param values) rather than kept corrupt. Legacy single-line `FXCHUNK` scenes still load (now via a 1MB read buffer); a legacy blob detected as truncated is discarded. **Existing projects: re-save each scene once with this build to migrate it to the safe format.**

- **Scenes: "Recall by chunk on instant path" setting now actually honored**: Since v0.0.19 the instant recall path applied the chunk for *every* plugin whenever one was captured, ignoring the checkbox and never touching the correctly-captured per-param values. The documented contract is restored: chunk recall on the instant path only when the setting is ON or the plugin is in the Chunk Recall list; param-based recall otherwise. All chunk writes (instant and timed paths) now also check the API return value and fall back to per-param recall on failure instead of leaving the plugin untouched.

- **Scenes: Warning when a Chunk Recall plugin's state cannot be captured**: Saving a scene now retries the `vst_chunk` read with escalating buffers (512KB → 2MB → 8MB) and, if a Chunk-Recall-list plugin still yields no state, shows a warning naming the plugin — previously the scene saved silently with nothing captured, and recall would silently do nothing for that plugin.

- **Scenes: Shadow VST3 param map no longer trusts stale values**: The shadow map (used to skip redundant param writes on instant recall) was only updated by control-surface notifications and only cleared on project load, so external state changes (e.g. an SWS Snapshot recall) could cause subsequent scene recalls to skip param writes that were actually needed. The map is now invalidated per-plugin after every chunk write, and cleared entirely when the project state change count shows something external modified the project since the last recall.

### Changes

- **Chunk Recall defaults**: "SSL Native" added to the default Chunk Recall plugin list (covers SSL Native Channel Strip 2, Bus Compressor 2, etc., whose state is not fully parameter-exposed). Note: projects that have already saved Live Tools settings keep their own persisted list — add "SSL Native" via Global Settings → Chunk Recall Plugins, then re-save affected scenes.

---

## [v0.0.23-beta] — 2026-07-27

### Bug Fixes

- **Scenes: Side-chain / channel-routed sends now recall correctly**: A send routed to non-default ports (e.g. 1&2 → 3&4 for a side-chain input) reverted to the default 1&2 → 1&2 routing on recall. Root cause: `SendState` never captured `I_SRCCHAN`/`I_DSTCHAN`, only volume/pan/mute/mode, so routing info was silently dropped at capture time. Fixed by capturing and restoring channel routing for both track sends and hardware sends, on both the instant and timed recall paths. Older scenes without the new fields default to standard stereo routing (the same behavior they had before).

- **Scenes: Whole-channel FX bypass now saved and recalled**: The per-plugin bypass state was already captured, but the master "bypass all FX on this channel" toggle (`I_FXEN`) was not, so it never persisted across scene recall. Added `TrackState::fxChainEnabled`, captured/restored alongside FX params.

- **Scenes: Docked window close/toggle now behaves correctly**: The small "x" close button on a docked window and the right-click "Close window" menu item only hid the window (`ShowWindow(SW_HIDE)`) instead of actually closing/undocking it, and the "Toggle UI Visible" action only worked when floating — while docked it just kept re-activating the tab instead of toggling it off. All three now properly destroy/undock the window, matching REAPER's native dockable-window behavior.

- **Scenes: Stale scene list after switching/opening projects**: `BeginLoadProjectState` cleared the in-memory scene list but never refreshed the ListView, so opening a project with fewer (or zero) scenes left the previous project's rows on screen. The list is now refreshed immediately after the clear.

### New Features

- **New actions**: Safes — Show/Hide toggle, Add Selected Track(s) to Safes. Scenes — Create New Scene, Recall Selected Scene, Update Selected Scene, Update Last Touched Scene, Advance to Scene After Last Recalled.

---

## [v0.0.21-beta] — 2026-06-28

### Bug Fixes

- **Scenes: Send enable/disable state now applies to the correct send**: When recalling a scene that adds a new send alongside an existing send to the same destination, the new send could receive the mute/enable state intended for the existing send. Root cause: `CreateTrackSend` inserts new sends at index 0, shifting all existing send indices and invalidating the lookup map used to apply discrete params. Fixed by splitting the send update loop into two passes — pass 1 updates all existing sends (map lookups valid), pass 2 creates all new sends (no map lookups). Affects both instant and timed recall paths.

- **Scenes: Extra sends now removed when scene has fewer sends to a destination**: When toggling from a scene with N sends to a destination to a scene with fewer than N sends to that same destination, the extra sends were not removed. Root cause: the removal check was a simple "is this destination in the snapshot?" boolean, which matched all N sends even when the snapshot only had M < N. Fixed by using count-based matching — each snapshot entry can absorb only one live send, so excess sends are correctly identified and removed. Applies to both track sends and HW sends on both instant and timed recall paths.

---

## [v0.0.20-beta] — 2026-06-22

### New Features

- **Scenes: Export / Import (.lts files)**: Right-click any scene to export it as a `.lts` file, or import a `.lts` file to append it to the current scene list. Scene files use the same serialization format as the project file, so all captured state (mix, FX, layers, notes, transition settings) is fully preserved.

- **Scenes: Copy / Paste**: Right-click a scene and choose **Copy**, then right-click anywhere in the list and choose **Paste** to insert a duplicate of the scene at that position. The pasted scene is named with ` (copy)` appended.

### Bug Fixes

- **Scenes: Selected layer now recalled correctly**: Scenes were not activating their designated layer on recall. The `RestoreLayerState` path called `ReplaceAllLayers` even when `m_layerIdx == -1` ("no layer recall"), which reset `m_activeLayer` to `-1` inside the engine and then skipped `ActivateLayer`/`DoApplyLayer` — leaving track visibility unchanged. Fixed by adding an early return when `m_layerIdx < 0` or when the scene has no captured layer data, so only scenes with an explicitly assigned layer trigger a layer switch.

- **Scenes: Old-format scenes no longer wipe layer definitions**: Previously, recalling a scene saved without `LAYERDEF` blocks (old format, `m_layers` empty) would call `ReplaceAllLayers({}, idx)` and destroy all layer definitions in the engine. The same guard above prevents this.

### Changes

- **PAFL Monitor**: Temporarily disabled. The PAFL Monitor action and menu entry are hidden pending further development. All PAFL code is preserved in the codebase.

---

## [v0.0.19-beta] — 2026-06-14

### New Features

- **Settings: Shadow VST3 params**: New "Shadow VST3 params" checkbox in Global Settings. When enabled, a hidden REAPER control surface listens to all VST3 parameter changes via `CSURF_EXT_SETFXPARAM` and maintains an in-memory shadow map of current values. On instant scene recall, each param write is skipped if the shadow value already matches the target — eliminating redundant `SetParamNormalized` calls that each trigger a DSP recalc. This is the primary tool for reducing FX chain recall latency when many params are already at their target values.

- **Settings: Recall all plugins by chunk on instant path**: New "Recall by chunk on instant path" checkbox in Global Settings. When enabled, the instant recall path restores VST3 and VST2 plugins via `SetNamedConfigParm("vst_chunk")` (a single atomic state dump) instead of looping over individual parameters. Both `vst_chunk` and per-param values are now always captured at snapshot time, so this flag is a pure recall-time switch — no re-saving of scenes is required after enabling it.

- **Settings: Per-category instant recall timing**: When "Duration debug" is enabled, the console output for instant-path recalls now shows a six-row breakdown — VolPan, Mute/Solo/Phase, Vis/Sel/Offset, Layout, FX chains, and Sends — making it easy to identify which category dominates recall time.

- **Settings: Active-flags header in timing output**: Every timing report (both instant and timed path) now opens with a settings header line showing which of ShadowParams / ChunkInstant / SkipUnchanged / PreloadOffline were active at the moment of recall. Useful for comparing measurements across different configuration combinations.

### Technical Notes

- Shadow map key: `(track GUID, fx_ident string, paramIdx)` → normalized double. Map is cleared on project load and the surface is registered/unregistered with the plugin lifecycle.
- Chunk capture: `vst_chunk` is now always written into every `FXState` at snapshot time regardless of settings, keeping scenes self-contained. The `g_chunkAllInstant` flag only controls which code path is used at recall time.

---

## [v0.0.18-beta] — 2026-06-12

### New Features

- **Settings: Detailed recall timing breakdown**: When "Duration debug" is enabled, the REAPER console now shows a full per-phase breakdown after each scene recall — SnapToEnd, BuildTrackMap (with matched/skipped track counts), DiscreteParams, FXChainSync, SendsSetup, TrackReorder, BuildLerpLists (with vol/pan, FX param, wet, and send lerp counts), RestoreLayerState, and grand total. Instant-path recalls show a simplified version. Makes it easy to pinpoint which phase is taking the most time.

### Bug Fixes

- **Layers: Track selection preserved on layer switch**: Switching layers no longer selects all tracks. The previous code passed a `bool*` to `GetSetMediaTrackInfo(tr, "I_SELECTED", ...)` — REAPER reads 4 bytes for `I_SELECTED`, so a 1-byte `bool` caused garbage reads that resulted in all tracks appearing selected. Fixed by using `int` throughout and saving/restoring the selection vector explicitly.

- **Layers: REAPER native spacers auto-sync into active layer**: Visual spacers inserted via the REAPER track list (Edit → Insert visual spacer) are now automatically detected and written into the active layer on the next timer tick. Previously, `SyncLayerOrderFromReaper` only synced track order — it now also reads `I_SPACER` per track and injects or removes spacer slots in the layer to match.

- **Scenes: Spacer recall fixed**: Visual spacers in scenes were silently failing to apply on recall. `Main_OnCommand(42665)` ("Insert visual spacer before selected tracks") does not work inside `PreventUIRefresh(1)` — moved the spacer insertion step to after `PreventUIRefresh(-1)` so REAPER processes the action correctly.

---

## [v0.0.17-beta] — 2026-06-12

### New Features

- **Scenes: Scene title edit box**: A new text edit field above the Notes area shows the selected scene's name. Editing it immediately renames the scene in the list, replacing the previous need to double-click inline in the ListView.

- **Scenes: Layers button in main UI**: "Layers..." button added to the bottom of the Scenes window right panel (below Settings). Clicking it opens the Layers window. The button is no longer inside the Global Settings dialog.

- **Scenes: Per-scene layer dropdown**: A dropdown (combobox) in the right panel lets you choose which layer activates when a scene is recalled. It shows the layers stored inside that scene's snapshot. "no layer recall" (index 0) means no layer change on recall. Default is the layer that was active when the scene was saved.

- **Scenes: Layer status indicator**: A "Layer: [name]" readout in the right panel updates in real time (every 100ms) to show which layer is currently active.

- **Scenes/Layers: Mutual exclusivity**: When a scene with an assigned layer is recalled, the scene engine no longer applies track visibility (TS_VIS) — the Layers engine manages visibility instead. This prevents the two systems from conflicting.

- **Settings: Duration debug**: New "Duration debug" checkbox in Global Settings. When enabled, a timing breakdown is printed to the REAPER console after each scene recall, showing time spent in engine recall and RestoreLayerState so performance bottlenecks can be identified.

### Bug Fixes

- **Layers: Fader levels no longer jump on layer switch**: `DoApplyLayer` now wraps all REAPER state changes inside `PreventUIRefresh(1)` / `PreventUIRefresh(-1)`, preventing automation modes or control surfaces from triggering spurious fader updates during the layer apply.

- **Layers: Spacer check in reorder loop**: Added an explicit `isSpacer` guard to the optional track-reorder loop in `DoApplyLayer` to prevent zero-GUID spacer slots from being processed as real tracks.

---

## [v0.0.16-beta] — 2026-06-11

### New Features

- **Settings: Layers button**: Global Settings dialog now has a "Layers..." button that opens the Layers window directly. The button is disabled (greyed out) when the Global Safes "Layers" safe is active.

- **Safes: Per-track column cleanup**: The per-track safes ListView no longer shows Visibility, Selected, Height, or Order columns — those parameters are only relevant in Global Safes and have been removed from the per-track section. Remaining columns: Vol, Pan, Mute, Solo, Phase, FX, Name, Color, All.

- **Safes: Rotated column headers**: Per-track safes column headers (Vol, Pan, Mute, etc.) now render rotated 90° so narrow checkbox columns display their full label without truncation.

- **Safes: Drag-to-check**: Click and drag across multiple checkboxes in the per-track safes list to check/uncheck them all in one gesture. The drag applies the same action (checking or unchecking) determined by the first cell clicked.

- **Scenes: Add Spacer restored**: "Add Spacer" is back in the scene list right-click context menu, allowing visual divider rows to be added between scenes. Adding a spacer now correctly marks the project dirty.

### Bug Fixes

- **Track Order: folder boundary protection**: When recalling a scene with track reordering enabled, tracks inside a folder can no longer be moved outside of that folder. Any reorder move that would cross a folder boundary is silently skipped for that track.

- **Layers: left-panel "Capture Visible Tracks" now adds spacers**: The "Capture Visible Tracks" option on the left-panel (layer list) context menu now reads REAPER's native `I_SPACER` value for each track and inserts a layer spacer entry before tracks that have one — matching the behaviour of the right-panel version and "Add Layer (Current MCP Visibility)". Also captures `I_FOLDERCOMPACT` (folder open/close state) like those functions already did.

- **Layers: removed "Add Layer" and "Add Sel" buttons**: The two bottom buttons that had no `WM_COMMAND` handler were removed from the Layers dialog. The freed space was given back to the layer and track ListViews. All layer/track management remains available via right-click context menus.

---

## [v0.0.15-beta] — 2026-06-10

### New Features

- **Scenes: inline rename on New**: The "Name" text field has been removed. Clicking **New** auto-generates a name (`Scene N`) and immediately drops into an inline rename on the list row. Press Enter to accept or Escape to keep the auto-name.

- **PAFL: active state no longer persists across sessions**: PAFL now starts inactive every time REAPER opens. The user must explicitly click "Active" to enable it. On project switch, PAFL deactivates automatically and re-enables via "Active on project startup" if that setting is on.

- **Layers: spacer changes apply immediately to active layer**: Adding a spacer via the track context menu ("Add Spacer Before/After") now instantly re-applies the layer if it is currently active, so the spacer appears in REAPER without needing to re-activate.

- **Layers: folder open/closed state captured and restored**: `I_FOLDERCOMPACT` (folder expand/collapse state) is now captured when using "Capture Visible Tracks" or "Add Layer (Current MCP Visibility)" and is restored when the layer is activated. Stored as `GUID:fc=N` in the project file; existing projects default to open (`fc=0`).

- **Layers: Trigger MCP Select setting**: New checkbox in Layers Settings — *"Trigger MCP select (refresh control surface channels)"*. When enabled, activating a layer briefly selects then deselects the first visible MCP track so hardware control surfaces re-scan their channel strip assignments.

### Changes

- **Layers: removed duplicate "Delete All Tracks" from track context menu**: "Delete All Tracks" and "Clear All Tracks" were identical. "Delete All Tracks" has been removed; "Clear All Tracks" remains.

- **Temporarily disabled**: Layouts, Surface & Zone Editor, Surface Monitor, and Control Surface (CSI) have been removed from the Extensions menu and action list while under development. All code is intact and re-enabling is a one-line uncomment per feature.

---

## [v0.0.14-beta] — 2026-06-09

### New Features

- **PAFL: full rewrite (poll-based, solo bus-backed)**: PAFL now uses REAPER's native
  dedicated solo bus (`soloip` bit 16) instead of manual track manipulation. On activate, all
  six solo-preference checkboxes are saved and set (`soloip |= 1|2|16|32|64`); on deactivate
  they are restored. A persistent post-fader *program source* send is kept live at all times —
  unmuted when nothing is PAFL'd, muted the moment any track is soloed. PAFL sends are created
  and removed each poll cycle (~30fps) rather than on button press, so the state always matches
  REAPER's actual solo state.

- **PAFL: program source track**: A configurable "Program source" track can be assigned in the
  PAFL window. It receives a permanent post-fader send to the PAFL bus, making the bus carry
  full-mix audio by default and switching to the soloed track(s) when PAFL is active.

- **PAFL: bus and program source excluded from PAFL scan**: The PAFL bus track and the program
  source track are both excluded from the solo poll, so accidentally soloing either one does not
  corrupt the routing or mute the program feed.

- **Scenes: per-project window state persistence**: The Scenes window now saves its
  dock/float/position/size per project (`LTSCENESWND` line in the extension block). Loading a
  project restores exactly the window state that was saved — open and floating, open and docked,
  or closed.

- **Layers: global max channels**: A new "Global Max Ch" setting caps MCP-visibility channel
  count across all layers, independent of the per-layer max. Persisted as `globalmaxch=` inside
  the `<LTLAYERS>` project block.

- **Layers: recall-by-index actions**: 20 REAPER actions registered for recalling layers by
  position (`LT_RECALL_LAYER_01` … `LT_RECALL_LAYER_20`, shown as "Live Tools: Layers - Recall
  Layer N" in the Actions list). Tied to the layer's index in the list rather than its name, so
  renaming or reordering layers does not break action bindings or controller mappings. Each
  action also reports a toggle state (lit when that layer is active).

### Bug Fixes

- **Scenes: layer recall with empty scene now clears layers**: Scene recall previously skipped
  `ReplaceAllLayers` when the captured layer list was empty, so a scene saved with no layers
  active would leave the current layer set unchanged. Now an empty layer list is applied
  literally (all layers cleared).

- **PAFL: nometers direction corrected**: The "show metering on unsoloed tracks" bits
  (`nometers |= 1|4096`) were previously being cleared instead of set, hiding meters when PAFL
  was activated. Direction is now correct.

- **PAFL: all five solo checkboxes now toggled**: Previously only `soloip` bit 16 ("Solo via
  dedicated solo bus") was set on activate; all five relevant bits (1|2|16|32|64 = 115) are now
  set, matching the recommended solo bus configuration.

---

## [v0.0.13-beta] — 2026-06-08

### New Features

- **Safes persistent save per project**: Safes configuration (global safe mask, per-track safe
  entries, and the "Enable Per-Track Safes" toggle) is now saved and loaded with the REAPER
  project file. State is written as `LTSAFEGLOBAL`, `LTSAFETRACKSEN`, and `LTSAFETRACK {guid} mask`
  lines inside the project's extension block — the same pattern used by Layers, MuteGroups, and
  DCA. Safes are now correctly reset when switching projects (no bleed-over). Any change to the
  safes grid marks the project dirty so Ctrl+S captures the update.

- **Safes: "All Tracks" checkbox**: A new "All Tracks" checkbox in the Global Safes groupbox sets
  every per-track row to all parameters at once. Unchecking it clears all per-track safe entries.
  The checkbox reflects the current aggregate state when the window is opened or refreshed.

- **Safes UI layout improvement**: The Global Safes groupbox has been expanded from two cramped
  rows of 7/5 into three evenly-spaced rows of 5, roughly doubling the horizontal space available
  per checkbox label.

### Bug Fixes

- **PAFL: folder parent tracks now PAFL correctly**: Pressing Solo on a folder-parent / group bus
  track now creates a PAFL send and mutes the program feed, the same as any regular track. Previously
  the handler detected `I_FOLDERDEPTH > 0` and immediately returned early, so folder parents could
  never be PAFL'd. `Run()` and the safety sweep now exempt folder parents that have an active
  unmuted PAFL send, so their `I_SOLO=2` is preserved while the send is live (keeping surface LEDs
  lit) while still clearing the spurious `I_SOLO` that REAPER auto-derives on non-PAFL'd parents
  from child solos.

---

## [v0.0.12-beta] — 2026-06-04

### New Features

- **Chunk Recall Plugins**: A new mechanism for plugins whose per-parameter values are not a
  reliable representation of state (e.g. Waves Virtual Mix Rack, where a module swap changes
  the meaning of fixed parameter indices while the count stays constant). Plugins whose names
  contain a built-in or user-defined keyword are now saved and restored using the full
  `vst_chunk` blob via `TrackFX_GetNamedConfigParm("vst_chunk")` /
  `TrackFX_SetNamedConfigParm("vst_chunk", ...)`, wrapped in a per-slot offline sandwich for
  safety. The per-FX wet/dry value continues to be captured and lerped separately.
  Built-in keywords: `Virtual Mix Rack`, `StudioRack`, `Scheps Omni`, `ML4000`.

- **Chunk Recall Plugins settings dialog**: Accessible from Global Settings → Live Performance →
  "Chunk Recall Plugins…". Lists all built-in keywords (read-only, marked `[built-in]`) and
  user-defined keywords. Add or remove user keywords via the Add/Remove buttons. User keywords
  are persisted per-project as `LTCHUNKPLUGIN <keyword>` lines in the settings block.

---

## [v0.0.8-beta] — 2026-06-02

### Performance Improvements

- **Faster scene recall on large projects (Opt C — always on)**: `SyncFXChain` and `BuildLerpLists`
  now build O(1) lookup maps (by `fx_ident` and by name+paramCount) from a single upfront pass
  over the live FX chain, replacing repeated `TrackFX_GetNamedConfigParm` / `TrackFX_GetFXName` /
  `TrackFX_GetNumParams` calls that previously ran for every slot on every iteration. This
  eliminates O(N²) API call patterns on tracks with large FX chains.

- **Skip unchanged params on recall (Opt B — toggleable)**: A new "Skip writing unchanged params
  on recall" option in Global Settings causes the instant-recall path to read each parameter's
  current live value and skip the `TrackFX_SetParamNormalized` call if it already matches the
  saved value (within 1e-7). This is most effective when recalling to a scene whose FX chain
  state is close to the current live state. Newly-added plugins always write all params
  regardless of this setting. Enable via Global Settings → Live Performance.

---

## [v0.0.7-beta] — 2026-06-02

### New Features

- **Send routing & level recall** (`TS_SENDS`): Scenes now capture and restore track-to-track sends and hardware output sends. On instant recall, sends are added/removed and levels (vol, pan, mute, send mode) are applied atomically. On timed/crossfade recall, new sends fade in from 0 and removed sends fade out to 0 before being deleted — all vol/pan changes lerp smoothly over the transition duration. Routing changes (add/remove) are recording-safe and blocked while REAPER is recording.

---

## [v0.0.6-beta] — 2026-06-02

### Bug Fixes

- **FX windows opening on scene recall**: When recalling a scene that adds new plugins via
  `TrackFX_AddByName`, REAPER's "auto-open FX window on insert" preference would immediately
  open a floating FX window for each plugin. The offline sandwich that followed would read
  `TrackFX_GetOpen` as `true` and re-open the window after the sandwich completed, leaving all
  newly-added plugin windows permanently visible. The window is now unconditionally closed
  before the offline sandwich for newly-added plugins, and never re-opened afterward. This
  affects both the instant recall path (with "Preload new plugins offline" enabled) and the
  timed/crossfade recall path.

---

## [v0.0.5-beta] — 2026-06-01

### Bug Fixes

- **Crash on project switch during active transition** (CRASH-1): If a transition was in progress
  when the user switched or loaded a new project, the plugin would crash because the timer
  callback was still accessing the old scene list while it was being cleared.
  `TransitionEngine::StopAndReset()` is now called at the very start of `BeginLoadProjectState`,
  before any snapshot state is touched.

- **Crash when a track is deleted mid-transition** (CRASH-2): Track pointers are now validated
  via `ValidatePtr2` before use in the timer callback and all FX chain manipulation paths.

- **Safes window settings not persisting across projects**: Safes configuration is now saved and
  restored via the project state context (`LTSAFES` lines), matching the persistence model used
  by all other windows.

### New Features

- **"Preload new plugins offline" toggle** (Global Settings): When enabled, newly added plugins
  are loaded offline ahead of the recall, then brought online during the transition. This
  substantially reduces the live audio stutter caused by plugin initialisation on the audio thread.
  The setting is persisted per-project as `LTPRELOADOFFLINE`.

- **O(n log n) track lookup**: Track resolution during snapshot capture and recall now uses a
  `std::map<GUID, MediaTrack*>` built once per operation instead of a linear scan, removing
  the O(n²) worst case on sessions with many tracks.

### Removed

- **"Leave FX windows open during recall" setting**: This option has been removed from Global
  Settings. FX windows are now always closed on scene recall. The underlying conditional sweep
  is gone — all open FX windows are unconditionally swept and closed at the start of every
  recall (instant and timed).

### Internal

- Source reorganised into feature subdirectories: `core/`, `csurf/`, `dca/`, `layers/`,
  `layouts/`, `livelock/`, `monitors/`, `mute/`, `scenes/`, `talkback/`.

---

## [v0.0.4-beta] — 2026-05-24

### Bug Fixes

- **Right-click "Remove from Cue" requires double-click** (#8): The context menu in the cue list
  now appears on the first right-click. Previously, when the cue dialog did not have focus,
  the first click was consumed by window activation and the menu appeared only on the second click.
  Fixed by handling `WM_RBUTTONDOWN` instead of `WM_RBUTTONUP` in the cue list subclass proc.

- **Single-click recall not firing** (#9): Single-click recall now fires reliably even when the
  mouse moves by a pixel between press and release. Previously, `ListView_HitTest` was used on
  `WM_LBUTTONUP` to confirm the item, but it returns -1 if the pointer has moved at all. Fixed by
  tracking whether a drag threshold was crossed and using that flag instead of a hit-test.

- **"Leave FX windows open" unchecked does not close all plugin UIs** (#10): After a recall
  (both instant and timed), all open FX windows are now swept and closed when this setting is
  unchecked. Previously only windows involved in the offline-sandwich path were closed.

- **Wet/dry mix not captured in snapshots** (#11): The per-FX wet/dry mix knob is now saved and
  restored as part of each snapshot. `FXState` gains a `wetVal` field (default `1.0`) that is
  captured via `TrackFX_GetParamFromIdent(":wet")`, written as an `FXWET` line in the project
  file, and loaded back transparently (old snapshots without the line default to `1.0`).

- **Wet/dry lerp direction wrong during timed recall**: Previously, plugins whose enabled state
  did not change had their wet mix forced to `0` at the start of a timed recall and then lerped
  to `1.0` (always, due to an out-of-bounds normVals lookup). The wet mix is now lerped smoothly
  from its current live value to the saved target value, matching all other parameter behaviour.

- **Wet/dry target value wrong for disabled→enabled and newly-added plugins**: The target wet
  value now correctly reads from `FXState::wetVal` instead of an out-of-bounds `normVals` slot
  that silently fell back to `1.0`.

### New Features

- **Per-track safes "All" column**: A new **All** checkbox column in the Safes window allows
  all safe parameters to be toggled for a track in a single click. Clicking when all parameters
  are already safe clears them all; clicking otherwise marks all parameters safe.

---

## [v0.0.3-beta] — (prior release)

See GitHub releases page for history.
