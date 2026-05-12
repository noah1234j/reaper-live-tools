#pragma once

#ifdef _WIN32
#  include <windows.h>
#else
#  include "../sws-master-2.14.0 - Copy/vendor/WDL/WDL/swell/swell.h"
#endif

#include <string>

class ProjectStateContext;

// ---------------------------------------------------------------------------
// DcaFlag  –  bitmask of which parameters the DCA controls
// ---------------------------------------------------------------------------
enum DcaFlag : uint32_t
{
    DCA_VOL    = 1u << 0,   // VOLUME_VCA_LEAD/FOLLOW
    DCA_PAN    = 1u << 1,   // PAN_LEAD/FOLLOW
    DCA_WIDTH  = 1u << 2,   // WIDTH_LEAD/FOLLOW
    DCA_MUTE   = 1u << 3,   // MUTE_LEAD/FOLLOW
    DCA_SOLO   = 1u << 4,   // SOLO_LEAD/FOLLOW
    DCA_RECARM = 1u << 5,   // RECARM_LEAD/FOLLOW
    DCA_ALL    = 0x3Fu
};

// ---------------------------------------------------------------------------
// DcaGroup  –  one DCA group (control track + follow relationships)
// ---------------------------------------------------------------------------
struct DcaGroup
{
    int         groupNum   = 0;     // 1–128 (REAPER internal group number)
    std::string name;               // display label, e.g. "DCA 1"
    GUID        trackGuid  = {};    // GUID of the DCA control (lead) track
    uint32_t    flags      = 0;     // bitmask of DcaFlag values
    bool        spilled    = false; // whether spill is currently active (not persisted)

    // RPP persistence
    void Serialize(ProjectStateContext* ctx) const;
    static DcaGroup* Deserialize(const char* headerLine,
                                 ProjectStateContext* ctx);
};
