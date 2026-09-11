# Changelog

## [v0.0.36-beta] — 2026-09-11

### Bug Fixes

- **Layers: activating a layer could move tracks into or out of folders**: REAPER stores no per-track parent — folder membership is purely positional, decided by which folder's `I_FOLDERDEPTH` span a track happens to sit inside. Reordering tracks to match a layer moves them to absolute indices, so a track whose destination landed inside a folder was silently adopted into it, and one moved out of a folder's span was evicted. Nothing in the move said "change the folder"; the new position simply meant a different one, which is why it only went wrong for some tracks in some layers. Both reorder paths now check before moving and skip any move that would change which folder a track belongs to. A track that opens a folder or closes one is never relocated at all, since moving a parent away from its children rewrites the tree. Skipping is deliberate rather than moving and repairing afterwards: once REAPER has re-parented a track there is no reliable way to put it back. The cost is that some tracks no longer reorder — folder parents, last children, and any track whose layer position falls inside a different folder — which is the direct price of never restructuring the project.

### New Features

- **Scenes: the layer recall dropdown is greyed out while the Layers global safe is set**: With that safe on, recall skips layer state entirely, so offering a layer to recall promised something that would not happen. The safe is toggled from the Safes window, which knows nothing about the dropdown, so the Scenes window notices the change on its UI tick and greys the control immediately instead of waiting for the scene selection to change.

- **Layers: the capture menu items name the panel they will actually read**: Both entries hardcoded "MCP" regardless of the Target setting, so with layers following the TCP the menu told the user the wrong thing while capturing the right one. They now read "Add Layer (Current TCP Visibility)" and "Update Layer (Capture Visible TCP Tracks)" when the target is the track panel, and the Update confirmation names the panel too — which also makes the current target visible at the point of use rather than only inside the settings dialog.

---

## [v0.0.35-beta] — 2026-09-10

### New Features

- **Layers: the window is now two plain labelled columns**: The right-hand side was wrapped in a "Layer Properties" frame containing a `Name:` box above the track list. Both are gone — the frame and the name box, which was a dead control with nothing behind it; layers have always been renamed in the list itself (F2, or right-click > Rename). Both lists now start at the same height and run taller for it.

- **Layers: the bottom bar is just Settings**: Activate, Prev, Next and Show All have been removed from it. Activate was already in the layer list's right-click menu, Prev and Next had no code behind them at all, and **Show All Tracks** has moved into that same menu — it was the only way to deactivate a layer, so leaving it on a removed button would have cost the feature.

- **Scenes: Rename is in the right-click menu**: The handler already existed and F2 already worked; the menu entry was simply missing.

- **Scenes: sidebar rearranged**: The status readout ("Done. Layer 4…") moved to the very bottom, directly above the version line — both are pinned to the bottom of the window now, so they stay last however tall it gets. `Layer: -`, the **Layers…** button and the per-scene "layer to recall" selector are grouped together in that order, and Safes / Cue Setup / Settings moved up to take the freed space. The notes box starts at double its previous height.

### Bug Fixes

- **Scenes: renaming a scene inline edited the "#" column**: The edit box opened over the row-number column, pre-filled with the index rather than the scene name, because a list view edits the item *label* — which is column 0 — and the name lives in column 1. The box is now seeded with the name and moved onto the Name column; the move is deferred by a posted message because the list view sizes the edit control after the notification that starts the edit returns. The edit is also no longer allowed to write back into the label, which used to flash the typed name into the "#" column before the row was rebuilt.

---

## [v0.0.34-beta] — 2026-09-10

### New Features

- **Scenes: the divider between the scene list and the sidebar can be dragged**: The split between the two columns was fixed — widening the window only ever widened the list. A divider now sits in the gutter between them; drag it to give either column more room. The sidebar's controls are re-laid out proportionally into whatever width is left rather than keeping a fixed width, the Scenes/Cue List toggles follow the divider with the list they belong to, and both columns are clamped to a usable minimum so the divider cannot be dragged far enough to swallow one. The position is saved with the window state.

- **Scenes: the sidebar name box is gone; rename in the list instead**: Scenes are renamed in place in the left column — right-click > Rename, or F2 — which is where the name is actually read. The duplicate name box in the sidebar has been removed and everything below it moved up. Inline renaming already existed; it now also marks the project dirty, which the sidebar box used to be what did.

- **Scenes: the notes box can now be dragged at the default window size**: Its limit was measured against the gap that happened to exist at the design size, which left nothing to claim until the window was made taller. It now measures against the version footer, so the space freed by removing the name box is available to it immediately.

### Bug Fixes

- **Scenes: every layer in an older scene showed as "(not in Layers)" in the recall dropdown**: Scenes saved before layer uids existed are migrated on first use, and that migration minted a *fresh* uid for each captured layer. A freshly minted uid matches nothing in the live layer list by construction, so the dropdown classified every one of them as a layer that no longer exists and listed them all as dangling entries. The migration now adopts the uid of the live layer the capture refers to — matching on name first, then on the position it was captured at — and only mints one for a layer with no counterpart at all. The dropdown no longer lists captured-but-missing layers either: it shows exactly the layers that exist right now.

- **Scenes: a scene pointing at a deleted layer recalled no layer at all**: Both the dropdown and recall now fall back to the first layer when the assigned one is gone, rather than leaving a dangling reference that activated nothing. A scene set to "(no layer recall)" is a deliberate choice and is left alone, and a layer list that is simply empty — which is what the world looks like before a project's layers have loaded — is no longer mistaken for a deletion.

- **Scenes: the layer UI did not update after a scene recall**: A recall can replace the entire layer set, but neither the sidebar's "Layer:" readout nor the Layers window was repainted afterwards, so both kept showing the previous state until something else happened to touch a layer. Both now refresh at the end of a recall.

---

## [v0.0.33-beta] — 2026-09-10

### New Features

- **Scenes: the plugin version is shown at the bottom of the sidebar**: There was no way to tell which build was loaded without checking the DLL's timestamp, which matters when a beta is being replaced often. The version now sits as a footer at the very bottom of the right-hand column. It is pinned to the bottom of the window rather than parked at a fixed height, so it stays the last thing in the column at any size, and the notes box's drag limit accounts for it — dragging the box down stops above the footer instead of covering it. The window is 9 units taller to make room; the scene list grew with it rather than losing space.

---

## [v0.0.32-beta] — 2026-09-10

### New Features

- **Scenes: Notes moved to the bottom of the sidebar, and it can be dragged taller**: The notes box sat in the middle of the right-hand panel, between the layer selector and the layer/status readouts, which pushed every button down and left it a fixed 34 units tall no matter how large the window was. Notes is now the last thing in the stack and everything above it has moved up to close the gap. A grab handle sits on the bottom edge of the box: drag it down to grow the box into the empty space below the sidebar, which is exactly the space that opens up as you make the window taller. The height is clamped to the space actually available, so shrinking the window pulls the box back in rather than letting it run off the bottom, and it is saved with the rest of the window state in the project.

- **Scenes: the notes box accepts Return for a new line**: It was a multi-line field that could never be given a second line — Return went to the dialog rather than the control. The field now takes Return, and because REAPER sits ahead of the window in the keyboard queue and would otherwise run whatever action Return is bound to, a keyboard hook claims the key while (and only while) focus is inside the notes box.

### Bug Fixes

- **Scenes: line breaks in notes were replaced with spaces when the project was saved**: Notes are stored on a single line in the project file, and newlines were flattened to spaces to keep them there. That was invisible while the field could not produce a newline in the first place; now that it can, newlines are escaped instead, so multi-line notes survive a save and reload. Backslashes are escaped alongside them so the two cannot be confused. Notes written by older builds read back unchanged.

---

## [v0.0.31-beta] — 2026-09-10

### New Features

- **Layers: "Update Layer" in the layer right-click menu**: The layer list's context menu item that re-captures the currently visible tracks is now named **Update Layer (Capture Visible Tracks)**, and it asks for confirmation naming the layer it is about to change. The action always replaced the layer's entire track list, but nothing said so and nothing said *which* layer — right-clicking one row while a different one was selected was enough to quietly discard a track list with no undo. The status line now reports the layer name along with the new track count.

### Bug Fixes

- **Scenes: the "select a layer to recall" list did not show layers created after the scene was saved**: The dropdown was populated from `snap->m_layers` — the copy of the layer set frozen into the scene at capture time — so a layer added or renamed afterwards never appeared in it, and the only way to refresh the list was a full **Overwrite** of the scene, which also discards its captured track and plugin state. The list is now built from the live layer set, so it always reflects the layers that exist right now. Choosing a layer the scene has never captured refreshes that scene's stored layer definitions from the current ones — layer definitions are not performance state, so this leaves everything else in the scene untouched. Layers the scene captured that no longer exist are still listed, marked *(not in Layers)*, so an existing assignment is never silently dropped.

- **Layers: layer identity was rebuilt from scratch on every scene recall, breaking action bindings**: Each layer carries a stable uid, and its REAPER action is registered against that uid as `LT_LAYER_UID_NNNN`. `ReplaceAllLayers` — which runs on every scene recall that restores layer state — discarded the incoming uids and minted fresh ones for the whole set, so every recall unregistered each layer's action and registered a *differently named* one in its place. Any keyboard shortcut or control-surface binding pointing at a layer's Activate action therefore came loose the first time a scene was recalled, and pointed at nothing afterwards. Uids are now carried through the replace intact, and are only allocated for layers that genuinely arrive without one.

- **Scenes: a scene's layer assignment broke when layers were reordered, renamed, added or deleted**: Scenes referenced their layer by position in the captured list (`LAYER n`). Any change to the layer set shifted those positions, so the scene silently recalled whichever layer had moved into that slot. Scenes now store the layer's stable uid (`LAYERUID`) and each captured layer records its own uid, so the reference survives renames and reordering. Scenes saved by older builds are migrated on first use — their existing index is resolved to a uid once and kept — and the old `LAYER` line is still written, so a scene saved by this build still loads on older builds.

---

## [v0.0.30-beta] — 2026-09-09

### Bug Fixes

- **Scenes: changing the order or number of plugins in a chain corrupted a few parameters on the plugins that moved**: `EnforceFXOrder` ran at the very end of `SyncFXChain`, *after* every chunk and parameter had been written, and it reorders the chain with `TrackFX_CopyToTrack` — which can make REAPER tear down and rebuild a plugin. v0.0.28-beta stopped that function from making *spurious* moves, but did nothing about the case where the order genuinely differs, which is precisely when a real move fires: change a scene's FX order or add or remove a plugin, and every plugin after the change point is moved, immediately after its state was written. The move serializes each plugin with `getChunk` and restores it at the destination, so parameters REAPER had already folded into the plugin's state survived and ones written moments earlier did not — per-parameter writes reach the plugin asynchronously, so the `getChunk` could capture the pre-write state. That is why it hit a handful of parameters rather than all of them, why it varied from recall to recall, and why it only affected plugins *not* on the Chunk Recall list: `SetNamedConfigParm("vst_chunk")` sets exactly the state the move round-trips, so chunk-recalled plugins were structurally immune. Chain membership and order are now settled first — missing plugins are added, then the chain is ordered, then the slot maps are rebuilt, and only then is any plugin state written — so a plugin rebuilt by a move receives its parameters afterwards instead of before.

- **Scenes: wet/dry fades could be applied to the wrong plugin after a reorder**: On the timed path the wet lerp list is built during `SyncFXChain` and stores raw chain indices, but `EnforceFXOrder` ran afterwards and renumbers every slot between the source and destination of each move. Any fade recorded before the move was then applied to whichever plugin had landed on that index — so on a scene that reordered the chain, a plugin could be faded out while an unrelated one was left at the wrong wet level. Each pending fade's plugin identity is now captured before the move and its slot re-resolved after.

- **Scenes: the live FX slot maps were not refreshed after plugins were deleted**: `SyncFXChain` built its identity→slot lookup once at entry, then removed plugins that the incoming scene does not contain. Every subsequent lookup used indices from before those deletions. The maps are now rebuilt after any operation that renumbers the chain — delete, add, or reorder.

---

## [v0.0.29-beta] — 2026-09-09

### Bug Fixes

- **Scenes: a few plugin parameters were recalled wrong, at random, on plugins not covered by the Chunk Recall list**: Most parameters came back correct and a handful did not, differing from one recall to the next. On the timed path a plugin that is not on the Chunk Recall list has no chunk written for it — every parameter is ramped individually by the transition timer instead — and `BuildLerpLists` only added a parameter to that ramp if its live value already differed from the target. A parameter that matched was dropped, and because `SnapToEnd` writes exact final values *only* for entries in the ramp list, a dropped parameter got no final write at all. That is fine in isolation, but during the ramp a plugin whose parameters are interdependent (FabFilter Pro-Q, and rack-style plugins generally) recomputes them from the intermediate states the ramp feeds it — a band's frequency sliding while its shape parameter sweeps through enum values is a state the plugin was never meant to be in — and that recompute moves parameters the build pass had discarded as "already correct". The damage therefore landed precisely on the parameters the skip had dropped, permanently, varying with ramp timing. Every non-chunk-recalled plugin now gets its complete stored parameter set written once when the transition completes, whether or not each parameter was ramped, so the end state is authoritative regardless of what the plugin did to itself mid-ramp. Chunk-recalled plugins are deliberately excluded — their state was restored atomically by the `vst_chunk` write and writing parameters over it would fight that restore, which is why plugins on the list (SSL Native and similar) never showed this. The ramp-list skip itself is now gated on the existing **"Skip unchanged parameters"** setting rather than always being on.

- **Scenes: per-parameter values were silently dropped from the project file, so scenes behaved differently after a reload**: v0.0.28-beta made capture store per-parameter values **and** the chunk for every plugin, so that the Chunk Recall list would be a pure recall-time switch. Serialization was never updated to match: it wrote the chunk *or* the parameters, never both. Since REAPER hands back a `vst_chunk` for essentially every VST, the parameter branch almost never ran and `normVals` was lost on every save — a project with 26 saved plugin states contained 26 chunks and zero parameter lines. Nothing was wrong in the session the scene was captured in, because the values were still in memory; after closing and reopening the project they were gone. That inverted the recall path behind the user's back: `normVals.empty()` is what makes `SyncFXChain` and `BuildLerpLists` choose chunk recall, so after a reload *every* plugin was chunk-recalled regardless of the Chunk Recall list, the list stopped having any effect, the timed path built no parameter ramps at all, and the per-param fallback for a failed chunk write had nothing left to fall back to. Scenes now write both, and existing scenes pick the new format up on the next save. The file format is compatible in both directions — the reader already accepted parameter lines alongside a chunk, and the chunk block is consumed by its declared length — so a scene saved by this build still loads on older builds.

- **Scenes: a failed chunk write on an already-online plugin left it untouched**: Of the three places the timed path writes a `vst_chunk`, two checked whether the write succeeded and fell back to per-parameter recall; the third — the one covering a plugin that is already online with its enabled state unchanged, which is the common case on a scene change — ignored the result entirely. A failure there silently left the plugin holding whatever the previous scene had set, with nothing logged. It now falls back to per-parameter recall like the other two sites and records the failure in the recall log.

---

## [v0.0.28-beta] — 2026-09-05

### New Features

- **Recall log**: New **"Write recall log to file"** option in Global Settings (off by default) writes a per-recall trace to `live_tools_recall.log` in the REAPER resource folder — the same folder as `reaper.ini`. For each track it records the live FX chain (including which plugins are parked offline), and for each plugin the slot it resolved to, whether it was newly added, which recall path was taken (chunk or per-param), the chunk size and whether the write succeeded, how many params were written versus skipped, and every reorder move. Nothing is written to disk while the recall is running — the trace is buffered in memory and flushed once the recall finishes — so leaving it on cannot add latency mid-transition. The file appends across sessions and rotates at 4 MB.

### Bug Fixes

- **Scenes: recalling one song scene directly after another could reset a plugin to its defaults**: `EnforceFXOrder` runs at the very end of the instant path, after params and chunks have been written, and compared snapshot position *i* against raw chain slot *i*. Those only line up when the chain contains nothing but the scene's own plugins — a plugin parked offline for another scene still occupies a chain index, so one sitting earlier in the chain shifted every index after it and the comparison failed spuriously. The function then "corrected" the order with `TrackFX_CopyToTrack`, which can make REAPER tear down and rebuild a heavyweight VST3 (SSL Native and similar) so it comes back at its defaults — silently discarding the correct state that had just been written. Ordering now maps over online slots only, so parked plugins can no longer shift the mapping and a move happens only when the order is genuinely wrong. Going to a scene from a base/"LIVE" scene tended to avoid this because the chain was already in the target order; going song-to-song did not, which is why it looked order-dependent.

- **Scenes: the Chunk Recall list is now honored at recall, not only at save**: Recall decided between chunk and per-param by testing whether the scene happened to store no params, which was only ever a proxy for "this plugin was on the Chunk Recall list when the scene was saved". Adding or removing a plugin from the list therefore had no effect on any existing scene — in *either* direction — until every scene was re-saved. Scenes now capture per-param values **and** the chunk for every plugin, and recall consults the list itself, so toggling it takes effect immediately on scenes you already have. Chunk-recall plugins also gain a working per-param fallback if the chunk write fails, and can be lerped on the timed path. (Scenes written by older builds that stored only one of the two still recall exactly as before.)

- **Scenes: plugins that were offline when a scene was saved no longer zero themselves on recall**: Capture walked every FX slot on the track, including plugins parked offline for another scene. REAPER has unloaded those, so `GetNumParams`/`GetParamNormalized` report zeros and `vst_chunk` comes back empty — the scene silently stored an all-zero parameter set, and recalling it drove every knob to the bottom. Such plugins are now recorded by identity only and flagged (`FXOFFLINE`), and recall leaves them untouched rather than writing meaningless state over a live plugin. The "could not capture plugin state" save warning no longer fires for them either.

---

## [v0.0.27-beta] — 2026-09-05

### Bug Fixes

- **Reverted the REAPER-theme light/dark UI theming (v0.0.25-beta)**: The shared LiveTheme module recolored every window from the active REAPER theme, and in the Scenes window it left the scene list wrong — rows, text and spacer separators no longer read correctly against the list background. Rather than patch around it, the whole theming change is backed out: LiveTheme is removed and every window (Scenes and its child dialogs, Safes, Layers, Mute Groups, Live Optimizer, DCA Groups, LiveLock, Live Monitor, Meter Bridge) renders exactly as it did in v0.0.24-beta, with its previous fixed palette. Nothing else from v0.0.25 or v0.0.26 is affected — the Layers MCP/TCP option, the multi-project-tab state handling, and the v7.75 FX/send slot support all remain.

---

## [v0.0.26-beta] — 2026-09-03

### New Features

- **Scenes: honor REAPER's empty FX/send slots (v7.75+)**: REAPER v7.75 added "allow empty slots in TCP/MCP FX lists" and the equivalent for sends, giving every FX and send a *slot* position in the panel grid that is independent of its (still dense) chain or send index. Scenes previously knew nothing about it: any recall that added, swapped or recreated a plugin or send appended it, silently collapsing a deliberately laid-out grid — the limiter parked in slot 10 would jump up behind whatever was added. Scenes now capture each FX's `slot_hint` and each send's `I_SLOT_HINT` and restore them after the chain and routing have settled, under a new **Slots** mask bit (included in the default capture mask, and safe-able from Global Safes).

  Support is probed once per session via `GetTrackNumSends(tr, 0x10000001)`; on REAPER older than v7.75 nothing is captured, the Slots safe is disabled, and behavior is unchanged. Scenes saved before this build, and scenes saved on a pre-7.75 build, carry no slot data and are left alone on recall rather than clearing slots you placed by hand — **re-save a scene once on v7.75+ to start capturing its slot layout.** Duplicate slot claims within one scene resolve first-come-first-served. Slot positions are purely visual: they are applied instantly on both the instant and timed paths, and no audio parameter is touched.

  Not covered: Layouts snapshots (they intentionally never enumerate FX or sends), and the master track's MIDI hardware-output slot (`I_MIDIHWOUT_SLOT`), which Live Tools does not capture.

### Bug Fixes

- **macOS: dialogs were missing controls added since v0.0.23-beta**: The SWELL dialog resources for the Apple build are generated from `reaper_transitions.rc` into a checked-in `.rc_mac_dlg` file, and that file had not been regenerated since the Layers MCP/TCP option was added. The macOS build was therefore missing the "Layers control" radio group and the resized Layers Settings dialog. Regenerated, which also picks up the new Slots safe checkbox. Windows was unaffected (it compiles the `.rc` directly).

---

## [v0.0.25-beta] — 2026-08-31

### New Features

- **Layers: choose whether layers control the MCP or TCP**: New "Layers control" option in Layers Settings — layer visibility can now target either the mixer (MCP, the previous behavior) or the track panel (TCP), with "Also hide in the other panel" applying to whichever panel is not the target.

- **All windows now follow the REAPER theme (light/dark)**: A shared LiveTheme module reads the active REAPER theme's colors and classifies it as light or dark by background luminance, polling ~1×/second so open windows restyle live when the theme is switched. The custom-painted windows (DCA Groups, LiveLock, Live Monitor, Meter Bridge) previously hardcoded dark palettes; each now carries a hand-tuned light variant (dark rendering is unchanged). The standard dialogs (Scenes and all its child dialogs, Safes, Layers, Mute Groups, Live Optimizer) previously used fixed system colors; they now draw with the REAPER theme's colors — including list views, headers, and (on Windows 10 20H1+) dark title bars and dark-mode button/scrollbar/combo styling. Status and accent colors (VU meters, clip, M/S/R LEDs, traffic-light grades, status dots, per-track colors) are intentionally identical in both modes.

---

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
