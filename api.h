#pragma once

// ---------------------------------------------------------------------------
// REAPER API – minimal function set for reaper_transitions
//
// REAPERAPI_MINIMAL means REAPERAPI_LoadAPI only checks the functions we
// explicitly declare via REAPERAPI_WANT_*, keeping us compatible with older
// REAPER builds that may be missing rarely-used API entries.
//
// One .cpp file must #define REAPERAPI_IMPLEMENT before including this header.
// All other .cpp files include it as-is.
// ---------------------------------------------------------------------------

#define REAPERAPI_MINIMAL

// Core track enumeration / info
#define REAPERAPI_WANT_GetNumTracks
#define REAPERAPI_WANT_GetTrack
#define REAPERAPI_WANT_GetSetMediaTrackInfo        // GUID, D_VOL, D_PAN, B_MUTE, B_PHASE, ...
#define REAPERAPI_WANT_GetSetMediaTrackInfo_String // P_NAME
#define REAPERAPI_WANT_GetTrackName                // alternate name lookup

// FX chain parameter access (live-safe, no chunk ops)
#define REAPERAPI_WANT_TrackFX_GetCount
#define REAPERAPI_WANT_TrackFX_GetFXName
#define REAPERAPI_WANT_TrackFX_GetNumParams
#define REAPERAPI_WANT_TrackFX_GetParamNormalized
#define REAPERAPI_WANT_TrackFX_SetParamNormalized
#define REAPERAPI_WANT_TrackFX_GetEnabled
#define REAPERAPI_WANT_TrackFX_SetEnabled
#define REAPERAPI_WANT_TrackFX_AddByName   // add plugin by name (safe during recording)
#define REAPERAPI_WANT_TrackFX_Delete      // remove plugin by index (safe during recording)
#define REAPERAPI_WANT_TrackFX_GetParamFromIdent  // resolve ':wet', ':bypass' to param index

// Offline chain swap (only used when NOT recording, when FX Chain mask is set)
#define REAPERAPI_WANT_GetSetObjectState2
#define REAPERAPI_WANT_FreeHeapPtr

// Track reorder (used by Layouts recall – visual-only, zero audio impact)
#define REAPERAPI_WANT_ReorderSelectedTracks

// Transport / timing
#define REAPERAPI_WANT_GetPlayState
#define REAPERAPI_WANT_time_precise
#define REAPERAPI_WANT_GetCursorPosition  // edit cursor position (seconds)
#define REAPERAPI_WANT_GetPlayPosition    // latency-compensated play cursor (seconds)

// Markers
#define REAPERAPI_WANT_AddProjectMarker2  // add a named marker at a position

// UI refresh control
#define REAPERAPI_WANT_PreventUIRefresh
#define REAPERAPI_WANT_TrackList_AdjustWindows   // redraw after vis changes

// Undo / main window
#define REAPERAPI_WANT_Undo_OnStateChangeEx
#define REAPERAPI_WANT_GetMainHwnd

// Plugin registration (timer, projectconfig, command_id, gaccel, hookcommand)
#define REAPERAPI_WANT_plugin_register

// PAFL monitor
#define REAPERAPI_WANT_GetMasterTrack
#define REAPERAPI_WANT_CreateTrackSend
#define REAPERAPI_WANT_GetSetTrackSendInfo
#define REAPERAPI_WANT_GetTrackNumSends
#define REAPERAPI_WANT_InsertTrackAtIndex
#define REAPERAPI_WANT_GetProjExtState
#define REAPERAPI_WANT_SetProjExtState
#define REAPERAPI_WANT_MarkProjectDirty
#define REAPERAPI_WANT_UpdateTimeline
#define REAPERAPI_WANT_GetOutputChannelName
#define REAPERAPI_WANT_GetExtState
#define REAPERAPI_WANT_SetExtState
#define REAPERAPI_WANT_CSurf_SetSurfaceSolo   // push solo LED state to all surfaces without changing I_SOLO

// MIDI I/O (for control surface)
#define REAPERAPI_WANT_CreateMIDIInput
#define REAPERAPI_WANT_CreateMIDIOutput
#define REAPERAPI_WANT_GetNumMIDIInputs
#define REAPERAPI_WANT_GetNumMIDIOutputs
#define REAPERAPI_WANT_GetMIDIInputName
#define REAPERAPI_WANT_GetMIDIOutputName

// Transport / selection helpers (for control surface)
#define REAPERAPI_WANT_Main_OnCommand
#define REAPERAPI_WANT_GetSelectedTrack
#define REAPERAPI_WANT_TrackFX_Show
#define REAPERAPI_WANT_CSurf_NumTracks
#define REAPERAPI_WANT_CSurf_TrackFromID
#define REAPERAPI_WANT_CSurf_TrackToID
#define REAPERAPI_WANT_GetTrackColor
#define REAPERAPI_WANT_SetTrackColor
#define REAPERAPI_WANT_ColorFromNative
#define REAPERAPI_WANT_ColorToNative

// Live Monitor – latency / PDC queries + RT CPU audio hook
#define REAPERAPI_WANT_GetInputOutputLatency
#define REAPERAPI_WANT_GetAudioDeviceInfo
#define REAPERAPI_WANT_TrackFX_GetNamedConfigParm
#define REAPERAPI_WANT_Audio_RegHardwareHook

// Meter Bridge – live peak metering
#define REAPERAPI_WANT_Track_GetPeakInfo

// Live Optimizer – system/REAPER checks
#define REAPERAPI_WANT_Audio_IsRunning
#define REAPERAPI_WANT_Audio_Init
#define REAPERAPI_WANT_get_config_var          // raw in-memory pointer (int/string vars)
#define REAPERAPI_WANT_get_config_var_string
#define REAPERAPI_WANT_get_ini_file
#define REAPERAPI_WANT_GetGlobalAutomationOverride
#define REAPERAPI_WANT_SetGlobalAutomationOverride
#define REAPERAPI_WANT_GetFreeDiskSpaceForRecordPath
#define REAPERAPI_WANT_GetAppVersion

// Docker support
#define REAPERAPI_WANT_DockWindowAdd
#define REAPERAPI_WANT_DockWindowAddEx
#define REAPERAPI_WANT_DockWindowRemove
#define REAPERAPI_WANT_DockWindowActivate
#define REAPERAPI_WANT_DockIsChildOfDock

// Control surface transport / navigation helpers
#define REAPERAPI_WANT_CSurf_OnPlay
#define REAPERAPI_WANT_CSurf_OnStop
#define REAPERAPI_WANT_CSurf_OnRecord
#define REAPERAPI_WANT_CSurf_OnArrow
#define REAPERAPI_WANT_CSurf_OnRewFwd
#define REAPERAPI_WANT_TrackFX_Show
#define REAPERAPI_WANT_GetTrackGUID
#define REAPERAPI_WANT_GetSelectedTrack

// FX offline (TransitionEngine – FX chain offline swap)
#define REAPERAPI_WANT_TrackFX_SetOffline

// Track sends removal (PAFL / Talkback)
#define REAPERAPI_WANT_RemoveTrackSend
// Hardware input enumeration (Talkback)
#define REAPERAPI_WANT_GetInputChannelName

// Audio hardware hook (Live Monitor)
#define REAPERAPI_WANT_Audio_RegHardwareHook

// Config variable read (Live Optimizer)
#define REAPERAPI_WANT_get_config_var

// Track grouping (DCA engine)
#define REAPERAPI_WANT_GetSetTrackGroupMembershipEx
#define REAPERAPI_WANT_DeleteTrack
#define REAPERAPI_WANT_SetMediaTrackInfo_Value   // numeric setter convenience

// Track selection / counting (Layers, MuteGroups)
#define REAPERAPI_WANT_CountTracks
#define REAPERAPI_WANT_CountSelectedTracks
#define REAPERAPI_WANT_SetOnlyTrackSelected
#define REAPERAPI_WANT_UpdateArrange

// User input dialog (MuteGroups)
#define REAPERAPI_WANT_GetUserInputs

// GUID ↔ string (MuteGroups)
#define REAPERAPI_WANT_GuidToStr

#include "reaper_plugin_functions.h"
