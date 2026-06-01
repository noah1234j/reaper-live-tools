// ---------------------------------------------------------------------------
// MuteGroup.h  –  Mute group data structures and engine
// ---------------------------------------------------------------------------
#pragma once

#ifdef _WIN32
#  include <windows.h>
#endif

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// MuteGroup – one named group of tracks that are muted/unmuted together
// ---------------------------------------------------------------------------
struct MuteGroup
{
    std::string        name;
    std::vector<GUID>  trackGuids;
};

// ---------------------------------------------------------------------------
// MuteGroupsEngine – singleton that manages mute groups
// ---------------------------------------------------------------------------
class MuteGroupsEngine
{
public:
    // Singleton access
    static MuteGroupsEngine& Get();

    // CRUD
    void AddGroup(const char* name);
    void RemoveGroup(int idx);
    void MoveGroup(int fromIdx, int toIdx);
    void SetGroupName(int idx, const char* name);

    // Track membership
    void AddTrackToGroup(int groupIdx, const GUID& guid);
    void RemoveTrackFromGroup(int groupIdx, int trackIdx);

    // Mute state
    void ToggleGroup(int idx);
    void MuteGroup_(int idx, bool muted);
    bool IsGroupMuted(int idx) const;

    // Accessors
    int              GetGroupCount() const;
    const MuteGroup& GetGroup(int idx) const;

    // GUID helper (static utility)
    static std::string GuidToStr(const GUID& g);

    // Project persistence (legacy line-by-line format)
    bool ProcessLine(const char* line);
    void SaveConfig(struct ProjectStateContext* ctx);
    void ResetForProject();

    // Project persistence (new block format used by reaper_transitions.cpp)
    void Clear();           // reset all groups (same as ResetForProject)
    void AddDeserializedGroup(MuteGroup* mg); // takes ownership
    void Serialize(struct ProjectStateContext* ctx);

    // Per-group REAPER command IDs (for keyboard shortcuts / toggle action)
    bool HandleGroupCommand(int cmd);       // returns true if cmd was handled
    bool IsGroupCmdId(int cmd) const;
    bool IsGroupMutedByCmdId(int cmd) const;

private:
    MuteGroupsEngine() = default;

    struct GroupEntry
    {
        MuteGroup group;
        bool      muted  = false;
        int       cmdId  = 0;    // REAPER command ID (0 = not registered)
    };

    std::vector<GroupEntry> m_groups;

    // Registers a new REAPER command ID for the group at the given index.
    // Uses a persistent string pool so the pointer stays valid.
    void RegisterCmd(GroupEntry& e, int groupSlot);
};  // MuteGroupsEngine

// ---------------------------------------------------------------------------
// Free functions (used by reaper_transitions.cpp)
// ---------------------------------------------------------------------------
// Deserialize one mute group from the project state stream.
// Called when REAPER feeds a "<LTMUTEGROUP" line.
// Returns a heap-allocated MuteGroup* (caller passes to AddDeserializedGroup).
MuteGroup* MuteGroup_Deserialize(const char* line, struct ProjectStateContext* ctx);

// Unregister group gaccels from REAPER on plugin unload.
void MuteGroupsEngine_Cleanup();
