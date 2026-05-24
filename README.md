> **⚠ BETA — v0.0.4-beta**
> This plugin is functional and actively used, but it is **not yet production-hardened**.
> Expect rough edges, missing polish, and the occasional crash or unexpected behaviour.
> **Back up your REAPER projects before using scene recall on anything critical.**
> Bug reports and feedback are welcome — please [open an issue](../../issues) on GitHub.

# Live Tools — Transition Snapshots for REAPER

A native REAPER extension plugin (`reaper_transitions.dll`) built for live sound engineers who need fast, reliable scene recall — without modal dialogs, project saves, or pauses in audio.

---

## The Problem

REAPER is a powerful DAW, but it was designed for recording and post-production. Using it as a live mixing platform exposes some real friction:

- **Snapshot recall is all-or-nothing.** Recalling a scene can glitch volume, mute states, and FX in unpredictable order with no grace period.
- **There's no timed transition.** Moving 40 faders instantly is a hard cut. Live consoles fade gracefully between scenes.
- **No per-parameter safes.** You can't protect a single track's reverb send while still recalling everything else.
- **Layouts and mix state are tangled.** Changing track visibility shouldn't re-fire audio parameters.
- **No dedicated PAFL bus.** REAPER's solo system doesn't behave like a console's Pre-Fader Listen when you're on stage.
- **No live health dashboard.** CPU load, PDC, and I/O latency are buried in separate menus during a show.

---

## What This Plugin Does

Live Tools adds a suite of purpose-built dockable windows to REAPER, all accessible from the **Actions** list and assignable to any key, MIDI, or OSC message.

---

### Scenes (`Live Tools: Scenes`)

The core feature. Scenes are full-project mix snapshots — every track's volume, pan, mute, solo, phase, FX parameters, FX chains, visibility, and more, captured at once and recalled on demand.

| Feature | Detail |
|---|---|
| **Timed transitions** | Fade smoothly between scenes over a configurable time (0 = instant). Runs on REAPER's main-thread timer — no audio interruptions or dropouts. |
| **Taper laws** | Linear, S-curve (default), Log, Exp, or Custom power-law exponent configured per scene. |
| **Per-parameter mask** | Choose exactly which parameter types a scene contains: Vol, Pan, Mute, Solo, Phase, FX Params, FX Chain, Visibility, Selection, Track Order, Track Name, Track Color, Height. |
| **30 assignable action slots** | Recall or overwrite any scene slot directly from a key, MIDI, or OSC event without touching the UI. |
| **Safes** | Global or per-track protection masks prevent specific parameters from being touched during recall, even mid-transition. |
| **Persisted in the project** | Scenes are written inside the `.RPP` file — no sidecar files to lose. |

---

### Safes (`Safes` window)

A track × parameter grid. Each cell is a checkbox:

- **Row 0 — Global:** protects that parameter type on every track.
- **Per-track rows:** per-track overrides, OR'd with the global mask.

Parameters covered: **Vol · Pan · Mute · Solo · Phase · FX · Visibility · Selection**

---

### Layouts (`Live Tools: Layouts`)

Visual-only snapshots — track order, height, TCP/MCP visibility, and track names — completely separate from the mix. Recall a layout without touching a single fader. Useful for switching between "FOH view" and "monitor view" on the same project.

---

### PAFL Monitor (`Live Tools: PAFL Monitor`)

A Pre/After Fader Listen system that mimics hardware console behaviour:

- Designates a dedicated PAFL bus track.
- When PAFL intercept is active, soloing a track mutes the program feed and routes only that track to the PAFL bus.
- Bus and source track assignments are saved per-project inside the `.RPP` file.

---

### Live Monitor (`Live Tools: Live Monitor`)

A compact real-time health dashboard showing:

- REAPER CPU load
- Audio I/O latency
- Maximum FX chain PDC (Plugin Delay Compensation)
- Round-trip latency

Color-coded **green → yellow → orange → red** as values approach danger levels.

---

### Live Optimize (`Live Tools: Live Optimize`)

Scans REAPER and Windows system settings for anything that could cause audio dropouts during a live show and produces a scored report with actionable fixes — ASIO buffer size, Windows power plan, background processes, REAPER preferences, and more.

---

### Control Surface (`Live Tools: Control Surface Settings`)

A built-in MCU/HUI control surface driver:

- **MCU and HUI** protocol support
- **8 or 16 channel** banks
- Motorized fader mode: **Volume, Pan, or Sends**
- VU metering, scribble strip names, track colors (X-Touch)
- Follow selection / follow MCP track order
- Configurable at runtime via the standalone settings dialog — no REAPER restart required

Pre-baked templates included for common surfaces (Behringer X-Touch, iCON Platform, etc.).

---

## Installation

### Pre-built (Windows)

1. Download `reaper_transitions.dll` from the [Releases](../../releases) page.
2. Copy it to your REAPER `UserPlugins` folder:
   - **Windows:** `%APPDATA%\REAPER\UserPlugins\`
3. Start or restart REAPER.
4. Open **Actions → Show action list** and search **`Live Tools`** to assign shortcuts to any scene slot or window.

### Build from Source

**Requirements:**
- Windows 10/11
- [Visual Studio 2022](https://visualstudio.microsoft.com/) (Desktop C++ workload)
- [CMake 3.15+](https://cmake.org/download/)

**Steps:**

```powershell
# 1. Configure
cmake -S reaper-transition-snapshots `
      -B reaper-transition-snapshots/build `
      -G "Visual Studio 17 2022" -A x64

# 2. Build
cmake --build reaper-transition-snapshots/build --config Release --parallel

# 3. Install directly to REAPER UserPlugins
cmake --install reaper-transition-snapshots/build --config Release
```

Or use the VS Code task **🔨 Build & Install (Release)** to do steps 2 and 3 in one shot.

---

## Inspiration & Prior Art

This plugin stands on the shoulders of:

- **[SWS Snapshots](https://www.sws-extension.org/)** — the original REAPER snapshot system. Provided the project-state serialization pattern, the per-track mask concept, and the snapshot model that this plugin extends with timed transitions and live-safe guarantees.
- **[Snapshooter](https://forum.cockos.com/showthread.php?t=184401)** — demonstrated that scene recall in REAPER could be fast enough for live use and popularized the per-slot action paradigm that lets hardware buttons trigger specific scenes.
- **[Reasolotus](https://forum.cockos.com/showthread.php?t=157824)** — proved the PAFL/solo intercept concept was viable inside REAPER and showed how to manage a dedicated monitor bus entirely in software.
- **MCU/HUI integrators** — the many community fader-controller scripts and extensions (SWS CSurf, various Lua MCU scripts, the Cockos MIDI surface SDK examples) that established the conventions for motorized surface control in REAPER and informed the control surface driver here.

---

## License

MIT — see individual source file headers.
