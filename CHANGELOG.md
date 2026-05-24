# Changelog

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
