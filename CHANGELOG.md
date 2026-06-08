# Changelog

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
