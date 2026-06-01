// ---------------------------------------------------------------------------
// DcaEngine.cpp  –  DCA group management logic
//
// Wraps REAPER's native track grouping API to provide VCA-style DCA groups.
// Each DCA group owns one "control track" (the lead) and any number of
// "member tracks" (the follows).  No audio passes through the control track.
// ---------------------------------------------------------------------------
#include "DcaEngine.h"
#include "api.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

// ---------------------------------------------------------------------------
// Global DCA group list
// ---------------------------------------------------------------------------
std::vector<std::unique_ptr<DcaGroup>> g_dcaGroups;

// ---------------------------------------------------------------------------
// Per-flag group-type name pairs
// Index matches the DcaFlag bit position (bit 0 = DCA_VOL, etc.)
// ---------------------------------------------------------------------------
struct FlagGroupNames { const char* lead; const char* follow; };
static const FlagGroupNames k_flagGroups[] =
{
    { "VOLUME_VCA_LEAD",  "VOLUME_VCA_FOLLOW"  },  // bit 0: DCA_VOL
    { "PAN_LEAD",         "PAN_FOLLOW"          },  // bit 1: DCA_PAN
    { "WIDTH_LEAD",       "WIDTH_FOLLOW"        },  // bit 2: DCA_WIDTH
    { "MUTE_LEAD",        "MUTE_FOLLOW"         },  // bit 3: DCA_MUTE
    { "SOLO_LEAD",        "SOLO_FOLLOW"         },  // bit 4: DCA_SOLO
    { "RECARM_LEAD",      "RECARM_FOLLOW"       },  // bit 5: DCA_RECARM
};
static const int k_flagCount = (int)(sizeof(k_flagGroups) / sizeof(k_flagGroups[0]));

// ---------------------------------------------------------------------------
// GUID helpers
// ---------------------------------------------------------------------------
static std::string GuidToStr(const GUID& g)
{
    WCHAR wbuf[64];
    StringFromGUID2(g, wbuf, 64);
    char buf[64];
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, 64, nullptr, nullptr);
    return buf;
}

// ---------------------------------------------------------------------------
// SetGroupBit: set or clear one bit in a group membership word
// ---------------------------------------------------------------------------
static void SetGroupBit(MediaTrack* tr, const char* groupName,
                        int groupNum, bool enable)
{
    int          offset = (groupNum - 1) / 32;
    unsigned int bit    = 1u << ((groupNum - 1) % 32);
    GetSetTrackGroupMembershipEx(tr, groupName, offset, bit, enable ? bit : 0u);
}

// ---------------------------------------------------------------------------
// GroupNumIsUsed: true if any track already has a lead or follow bit for groupNum
// ---------------------------------------------------------------------------
static bool GroupNumIsUsed(int groupNum)
{
    const int    offset = (groupNum - 1) / 32;
    const unsigned int bit    = 1u << ((groupNum - 1) % 32);
    const int    n      = GetNumTracks();

    for (int i = 0; i < n; i++)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;
        for (int f = 0; f < k_flagCount; f++)
        {
            if (GetSetTrackGroupMembershipEx(tr, k_flagGroups[f].lead,   offset, 0, 0) & bit) return true;
            if (GetSetTrackGroupMembershipEx(tr, k_flagGroups[f].follow, offset, 0, 0) & bit) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// DcaEngine_GetNextFreeGroup
// ---------------------------------------------------------------------------
int DcaEngine_GetNextFreeGroup(int startGroup)
{
    for (int g = startGroup; g <= 128; g++)
        if (!GroupNumIsUsed(g)) return g;
    return 0;
}

// ---------------------------------------------------------------------------
// DcaEngine_GetControlTrack
// ---------------------------------------------------------------------------
MediaTrack* DcaEngine_GetControlTrack(const DcaGroup* dca)
{
    if (!dca) return nullptr;
    const int n = GetNumTracks();
    for (int i = 0; i < n; i++)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;
        GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
        if (pg && memcmp(pg, &dca->trackGuid, sizeof(GUID)) == 0)
            return tr;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// DcaEngine_GetMemberTracks
// Returns all tracks that have at least one enabled follow bit for this group.
// ---------------------------------------------------------------------------
std::vector<MediaTrack*> DcaEngine_GetMemberTracks(const DcaGroup* dca)
{
    std::vector<MediaTrack*> result;
    if (!dca || dca->groupNum < 1 || dca->groupNum > 128) return result;

    const int          offset = (dca->groupNum - 1) / 32;
    const unsigned int bit    = 1u << ((dca->groupNum - 1) % 32);
    const int          n      = GetNumTracks();

    for (int i = 0; i < n; i++)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;
        for (int f = 0; f < k_flagCount; f++)
        {
            if ((dca->flags & (1u << f)) == 0) continue;
            if (GetSetTrackGroupMembershipEx(tr, k_flagGroups[f].follow,
                                              offset, 0, 0) & bit)
            {
                result.push_back(tr);
                break; // found — don't add the same track twice
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// DcaEngine_AssignSelectedTracks
// ---------------------------------------------------------------------------
void DcaEngine_AssignSelectedTracks(DcaGroup* dca)
{
    if (!dca || dca->groupNum < 1 || dca->groupNum > 128) return;

    MediaTrack* lead = DcaEngine_GetControlTrack(dca);
    const int   n    = GetNumTracks();

    for (int i = 0; i < n; i++)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;
        int* sel = (int*)GetSetMediaTrackInfo(tr, "I_SELECTED", nullptr);
        if (!sel || !*sel) continue;
        if (tr == lead) continue; // don't make the DCA track follow itself

        for (int f = 0; f < k_flagCount; f++)
            if (dca->flags & (1u << f))
                SetGroupBit(tr, k_flagGroups[f].follow, dca->groupNum, true);
    }
    Undo_OnStateChangeEx("Assign tracks to DCA", -1, -1);
}

// ---------------------------------------------------------------------------
// DcaEngine_RemoveTrackFromDca
// ---------------------------------------------------------------------------
void DcaEngine_RemoveTrackFromDca(DcaGroup* dca, MediaTrack* tr)
{
    if (!dca || !tr) return;
    for (int f = 0; f < k_flagCount; f++)
        SetGroupBit(tr, k_flagGroups[f].follow, dca->groupNum, false);
    Undo_OnStateChangeEx("Remove track from DCA", -1, -1);
}

// ---------------------------------------------------------------------------
// DcaEngine_SetFlag
// ---------------------------------------------------------------------------
void DcaEngine_SetFlag(DcaGroup* dca, uint32_t flag, bool enable)
{
    if (!dca) return;

    // Find which array slot this flag maps to
    int bitIdx = -1;
    for (int f = 0; f < k_flagCount; f++)
        if (flag == (1u << f)) { bitIdx = f; break; }
    if (bitIdx < 0) return;

    MediaTrack* lead = DcaEngine_GetControlTrack(dca);

    if (enable)
    {
        if (lead)
            SetGroupBit(lead, k_flagGroups[bitIdx].lead, dca->groupNum, true);

        // Find member tracks by scanning ALL follow-bit types for this group.
        // This is necessary because the new flag may not yet be in dca->flags,
        // so DcaEngine_GetMemberTracks (which filters by dca->flags) would miss
        // tracks that are already members via other follow bits.
        const int          offset = (dca->groupNum - 1) / 32;
        const unsigned int bit    = 1u << ((dca->groupNum - 1) % 32);
        const int          n      = GetNumTracks();
        for (int i = 0; i < n; i++)
        {
            MediaTrack* tr = GetTrack(nullptr, i);
            if (!tr || tr == lead) continue;
            // Check if this track is a member via any follow type
            for (int f = 0; f < k_flagCount; f++)
            {
                if (GetSetTrackGroupMembershipEx(tr, k_flagGroups[f].follow,
                                                 offset, 0, 0) & bit)
                {
                    SetGroupBit(tr, k_flagGroups[bitIdx].follow,
                                dca->groupNum, true);
                    break;
                }
            }
        }

        dca->flags |= flag;
    }
    else
    {
        // Remove follow bit from every track in project
        const int n = GetNumTracks();
        for (int i = 0; i < n; i++)
        {
            MediaTrack* tr = GetTrack(nullptr, i);
            if (tr) SetGroupBit(tr, k_flagGroups[bitIdx].follow, dca->groupNum, false);
        }
        if (lead)
            SetGroupBit(lead, k_flagGroups[bitIdx].lead, dca->groupNum, false);

        dca->flags &= ~flag;
    }
}

// ---------------------------------------------------------------------------
// DcaEngine_Create
// ---------------------------------------------------------------------------
DcaGroup* DcaEngine_Create(int startGroup, uint32_t flags)
{
    int groupNum = DcaEngine_GetNextFreeGroup(startGroup);
    if (groupNum == 0) return nullptr; // all 128 group slots used

    // Insert a new control track at the end of the project
    InsertTrackAtIndex(GetNumTracks(), false);
    MediaTrack* lead = GetTrack(nullptr, GetNumTracks() - 1);
    if (!lead) return nullptr;

    // Name: "DCA N" where N is the 1-based index in our list
    char tname[64];
    snprintf(tname, sizeof(tname), "DCA %d", (int)g_dcaGroups.size() + 1);
    GetSetMediaTrackInfo_String(lead, "P_NAME", tname, true);

    // Disable master send so no audio passes through the DCA control track
    SetMediaTrackInfo_Value(lead, "B_MAINSEND", 0.0);

    // Set lead group bits for all requested flags
    for (int f = 0; f < k_flagCount; f++)
        if (flags & (1u << f))
            SetGroupBit(lead, k_flagGroups[f].lead, groupNum, true);

    // Capture the new track's GUID
    GUID* pg = (GUID*)GetSetMediaTrackInfo(lead, "GUID", nullptr);
    GUID  guid = pg ? *pg : GUID{};

    auto* dca       = new DcaGroup();
    dca->groupNum   = groupNum;
    dca->name       = tname;
    dca->trackGuid  = guid;
    dca->flags      = flags;

    g_dcaGroups.push_back(std::unique_ptr<DcaGroup>(dca));

    Undo_OnStateChangeEx("Create DCA group", -1, -1);
    TrackList_AdjustWindows(false);

    return dca;
}

// ---------------------------------------------------------------------------
// DcaEngine_Delete
// ---------------------------------------------------------------------------
void DcaEngine_Delete(int idx, bool deleteTrack)
{
    if (idx < 0 || idx >= (int)g_dcaGroups.size()) return;
    DcaGroup* dca = g_dcaGroups[idx].get();

    // Clear all group bits (lead and follow) for this group number
    const int n = GetNumTracks();
    for (int i = 0; i < n; i++)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;
        for (int f = 0; f < k_flagCount; f++)
        {
            SetGroupBit(tr, k_flagGroups[f].lead,   dca->groupNum, false);
            SetGroupBit(tr, k_flagGroups[f].follow, dca->groupNum, false);
        }
    }

    if (deleteTrack)
    {
        MediaTrack* lead = DcaEngine_GetControlTrack(dca);
        if (lead) DeleteTrack(lead);
    }

    g_dcaGroups.erase(g_dcaGroups.begin() + idx);

    Undo_OnStateChangeEx("Delete DCA group", -1, -1);
}

// ---------------------------------------------------------------------------
// DcaEngine_Spill
// ---------------------------------------------------------------------------
void DcaEngine_Spill(DcaGroup* dca, bool active)
{
    if (!dca) return;
    dca->spilled = active;

    std::vector<MediaTrack*> members = DcaEngine_GetMemberTracks(dca);
    const int n = GetNumTracks();

    for (int i = 0; i < n; i++)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;

        bool isMember = std::find(members.begin(), members.end(), tr) != members.end();

        GUID* pg    = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
        bool  isLead = pg && (memcmp(pg, &dca->trackGuid, sizeof(GUID)) == 0);

        if (active)
        {
            double vis = (isMember || isLead) ? 1.0 : 0.0;
            SetMediaTrackInfo_Value(tr, "B_SHOWINTCP",   vis);
            SetMediaTrackInfo_Value(tr, "B_SHOWINMIXER", vis);
        }
        else
        {
            SetMediaTrackInfo_Value(tr, "B_SHOWINTCP",   1.0);
            SetMediaTrackInfo_Value(tr, "B_SHOWINMIXER", 1.0);
        }
    }

    TrackList_AdjustWindows(false);
}

// ---------------------------------------------------------------------------
// DcaEngine_ReapplyAll
// Re-applies lead group bits and master-send disable after project load.
// Follow bits are persisted by REAPER natively in the .RPP track data.
// ---------------------------------------------------------------------------
void DcaEngine_ReapplyAll()
{
    for (const auto& dca : g_dcaGroups)
    {
        MediaTrack* lead = DcaEngine_GetControlTrack(dca.get());
        if (!lead) continue;

        for (int f = 0; f < k_flagCount; f++)
            if (dca->flags & (1u << f))
                SetGroupBit(lead, k_flagGroups[f].lead, dca->groupNum, true);

        SetMediaTrackInfo_Value(lead, "B_MAINSEND", 0.0);
    }
}
