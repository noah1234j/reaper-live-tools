#pragma once

#ifdef _WIN32
#  include <windows.h>
#else
#  include "../sws-master-2.14.0 - Copy/vendor/WDL/WDL/swell/swell.h"
#endif

#include "DcaGroup.h"
#include "api.h"

#include <vector>
#include <memory>

// ---------------------------------------------------------------------------
// Global DCA group list (project-scoped, persisted to RPP)
// ---------------------------------------------------------------------------
extern std::vector<std::unique_ptr<DcaGroup>> g_dcaGroups;

// ---------------------------------------------------------------------------
// DCA engine functions
// ---------------------------------------------------------------------------

// Create a new DCA group: inserts a DCA control track, claims the next free
// REAPER group number starting at startGroup, and sets the given flags.
// Returns the new group (owned by g_dcaGroups), or nullptr if no free slot.
DcaGroup* DcaEngine_Create(int startGroup, uint32_t flags);

// Delete the DCA group at index idx: removes all group memberships.
// If deleteTrack==true the DCA control track is also removed from the project.
void DcaEngine_Delete(int idx, bool deleteTrack);

// Make every currently-selected REAPER track a follower of dca.
// Applies all flags that are set in dca->flags.
void DcaEngine_AssignSelectedTracks(DcaGroup* dca);

// Remove one track from all group memberships for this DCA.
void DcaEngine_RemoveTrackFromDca(DcaGroup* dca, MediaTrack* tr);

// Enable or disable one flag for the DCA group.
// Updates the lead track's lead bits and every follower track's follow bits.
void DcaEngine_SetFlag(DcaGroup* dca, uint32_t flag, bool enable);

// Spill: show only member tracks in TCP (active=true) or restore all (false).
void DcaEngine_Spill(DcaGroup* dca, bool active);

// Return all tracks currently following this DCA (any active flag is enough).
std::vector<MediaTrack*> DcaEngine_GetMemberTracks(const DcaGroup* dca);

// Return the DCA control (lead) track by GUID, or nullptr if not found.
MediaTrack* DcaEngine_GetControlTrack(const DcaGroup* dca);

// Scan from startGroup upward for the first group number (1-128) that has no
// lead or follow assignment on any track in the project.  Returns 0 if full.
int DcaEngine_GetNextFreeGroup(int startGroup);

// Re-apply lead group bits from stored state after a project load.
// (REAPER persists follow bits in the .RPP itself; we just fix up lead bits.)
void DcaEngine_ReapplyAll();
