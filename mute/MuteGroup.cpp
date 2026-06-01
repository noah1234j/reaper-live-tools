// ---------------------------------------------------------------------------
// MuteGroup.cpp  –  MuteGroupsEngine implementation
// ---------------------------------------------------------------------------
#include "MuteGroup.h"
#include "api.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <list>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Persistent string pool for REAPER command-name pointers
// (REAPER keeps the pointer; std::list nodes are stable on insert)
// ---------------------------------------------------------------------------
static std::list<std::string> s_cmdNamePool;

// Registered gaccel_register_t objects (for cleanup on unload)
static std::vector<gaccel_register_t*> s_gaccels;

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
MuteGroupsEngine& MuteGroupsEngine::Get()
{
    static MuteGroupsEngine instance;
    return instance;
}

// ---------------------------------------------------------------------------
// GUID helper
// ---------------------------------------------------------------------------
std::string MuteGroupsEngine::GuidToStr(const GUID& g)
{
    WCHAR wbuf[64] = {};
    StringFromGUID2(g, wbuf, 64);
    char buf[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, 64, nullptr, nullptr);
    return buf;
}

static GUID StrToGuid(const char* s)
{
    GUID g = {};
    if (!s || !s[0]) return g;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (wlen <= 0) return g;
    std::vector<WCHAR> wbuf(wlen);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, wbuf.data(), wlen);
    CLSIDFromString(wbuf.data(), &g);
    return g;
}

// ---------------------------------------------------------------------------
// CRUD
// ---------------------------------------------------------------------------
void MuteGroupsEngine::AddGroup(const char* name)
{
    GroupEntry e;
    e.group.name = name ? name : "Group";
    e.muted = false;
    m_groups.push_back(std::move(e));
}

void MuteGroupsEngine::RemoveGroup(int idx)
{
    if (idx < 0 || idx >= (int)m_groups.size()) return;
    m_groups.erase(m_groups.begin() + idx);
}

void MuteGroupsEngine::MoveGroup(int fromIdx, int toIdx)
{
    if (fromIdx < 0 || fromIdx >= (int)m_groups.size()) return;
    if (toIdx   < 0 || toIdx   >= (int)m_groups.size()) return;
    if (fromIdx == toIdx) return;

    GroupEntry tmp = std::move(m_groups[fromIdx]);
    m_groups.erase(m_groups.begin() + fromIdx);
    // Insert at toIdx in the now-shrunk array.
    // Both "move backward" and "move forward" cases work without adjustment:
    // the desired final index is exactly toIdx in the n-element result.
    m_groups.insert(m_groups.begin() + toIdx, std::move(tmp));
}

void MuteGroupsEngine::SetGroupName(int idx, const char* name)
{
    if (idx < 0 || idx >= (int)m_groups.size()) return;
    m_groups[idx].group.name = name ? name : "";
}

// ---------------------------------------------------------------------------
// Track membership
// ---------------------------------------------------------------------------
void MuteGroupsEngine::AddTrackToGroup(int groupIdx, const GUID& guid)
{
    if (groupIdx < 0 || groupIdx >= (int)m_groups.size()) return;
    auto& tracks = m_groups[groupIdx].group.trackGuids;

    // Don't add duplicates
    for (const GUID& g : tracks)
        if (IsEqualGUID(g, guid)) return;

    tracks.push_back(guid);
}

void MuteGroupsEngine::RemoveTrackFromGroup(int groupIdx, int trackIdx)
{
    if (groupIdx < 0 || groupIdx >= (int)m_groups.size()) return;
    auto& tracks = m_groups[groupIdx].group.trackGuids;
    if (trackIdx < 0 || trackIdx >= (int)tracks.size()) return;
    tracks.erase(tracks.begin() + trackIdx);
}

// ---------------------------------------------------------------------------
// Mute state
// ---------------------------------------------------------------------------
void MuteGroupsEngine::ToggleGroup(int idx)
{
    if (idx < 0 || idx >= (int)m_groups.size()) return;
    MuteGroup_(idx, !m_groups[idx].muted);
}

void MuteGroupsEngine::MuteGroup_(int idx, bool muted)
{
    if (idx < 0 || idx >= (int)m_groups.size()) return;
    m_groups[idx].muted = muted;

    // Apply mute to all tracks in the group
    for (const GUID& guid : m_groups[idx].group.trackGuids)
    {
        const int n = GetNumTracks();
        for (int i = 0; i < n; ++i)
        {
            MediaTrack* tr = GetTrack(nullptr, i);
            if (!tr) continue;
            GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
            if (pg && IsEqualGUID(*pg, guid))
            {
                bool muteVal = muted;
                GetSetMediaTrackInfo(tr, "B_MUTE", &muteVal);
                break;
            }
        }
    }
}

bool MuteGroupsEngine::IsGroupMuted(int idx) const
{
    if (idx < 0 || idx >= (int)m_groups.size()) return false;
    return m_groups[idx].muted;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------
int MuteGroupsEngine::GetGroupCount() const
{
    return (int)m_groups.size();
}

const MuteGroup& MuteGroupsEngine::GetGroup(int idx) const
{
    static MuteGroup empty;
    if (idx < 0 || idx >= (int)m_groups.size()) return empty;
    return m_groups[idx].group;
}

// ---------------------------------------------------------------------------
// Project persistence
// ---------------------------------------------------------------------------
void MuteGroupsEngine::ResetForProject()
{
    m_groups.clear();
}

// Lines produced by SaveConfig:
//   LTMGGROUP <name>
//   LTMGTRACK <guidStr>
//   LTMGTRACK ...
//   (repeat per group)
bool MuteGroupsEngine::ProcessLine(const char* line)
{
    while (*line == ' ' || *line == '\t') ++line;

    if (strncmp(line, "LTMGGROUP ", 10) == 0)
    {
        const char* name = line + 10;
        // Trim trailing whitespace
        std::string n = name;
        while (!n.empty() && (n.back() == ' ' || n.back() == '\r' || n.back() == '\n'))
            n.pop_back();
        AddGroup(n.c_str());
        return true;
    }
    if (strncmp(line, "LTMGTRACK ", 10) == 0)
    {
        if (m_groups.empty()) return true;
        const char* gs = line + 10;
        std::string gstr = gs;
        while (!gstr.empty() && (gstr.back() == ' ' || gstr.back() == '\r' || gstr.back() == '\n'))
            gstr.pop_back();
        GUID g = StrToGuid(gstr.c_str());
        m_groups.back().group.trackGuids.push_back(g);
        return true;
    }
    return false;
}

void MuteGroupsEngine::SaveConfig(ProjectStateContext* ctx)
{
    for (const auto& e : m_groups)
    {
        char line[256];
        snprintf(line, sizeof(line), "LTMGGROUP %s", e.group.name.c_str());
        ctx->AddLine("%s", line);
        for (const GUID& g : e.group.trackGuids)
        {
            std::string gs = GuidToStr(g);
            snprintf(line, sizeof(line), "LTMGTRACK %s", gs.c_str());
            ctx->AddLine("%s", line);
        }
    }
}

// ---------------------------------------------------------------------------
// Helper: register a REAPER command ID for a GroupEntry
// ---------------------------------------------------------------------------
void MuteGroupsEngine::RegisterCmd(GroupEntry& e, int groupSlot)
{
    // Build a stable name in the pool
    s_cmdNamePool.push_back("LT_MUTEGROUP_" + std::to_string(groupSlot));
    const char* name = s_cmdNamePool.back().c_str();

    e.cmdId = plugin_register("command_id", (void*)name);

    if (e.cmdId)
    {
        // Register a named action so it appears in REAPER's action list
        auto* acc    = new gaccel_register_t{};
        acc->desc    = name;
        acc->accel   = {};
        acc->accel.cmd = (WORD)e.cmdId;
        plugin_register("gaccel", acc);
        s_gaccels.push_back(acc);
    }
}

// ---------------------------------------------------------------------------
// New block-format persistence
// ---------------------------------------------------------------------------
void MuteGroupsEngine::Clear()
{
    // Command IDs / gaccels can't be un-registered per REAPER API,
    // so we simply stop tracking the groups; the cmd IDs become orphaned
    // until the next plugin load.
    m_groups.clear();
}

void MuteGroupsEngine::AddDeserializedGroup(MuteGroup* mg)
{
    if (!mg) return;

    GroupEntry e;
    e.group = std::move(*mg);
    delete mg;
    e.muted  = false;

    // Register a REAPER action for this group
    RegisterCmd(e, (int)m_groups.size());

    m_groups.push_back(std::move(e));
}

void MuteGroupsEngine::Serialize(ProjectStateContext* ctx)
{
    for (const auto& e : m_groups)
    {
        // Block header: <LTMUTEGROUP "Name"
        ctx->AddLine("<LTMUTEGROUP \"%s\"", e.group.name.c_str());
        for (const GUID& g : e.group.trackGuids)
        {
            std::string gs = GuidToStr(g);
            ctx->AddLine("LTMGTRACK %s", gs.c_str());
        }
        ctx->AddLine(">");
    }
}

// ---------------------------------------------------------------------------
// Command-ID helpers
// ---------------------------------------------------------------------------
bool MuteGroupsEngine::HandleGroupCommand(int cmd)
{
    for (int i = 0; i < (int)m_groups.size(); ++i)
    {
        if (m_groups[i].cmdId == cmd)
        {
            ToggleGroup(i);
            return true;
        }
    }
    return false;
}

bool MuteGroupsEngine::IsGroupCmdId(int cmd) const
{
    if (cmd <= 0) return false;
    for (const auto& e : m_groups)
        if (e.cmdId == cmd) return true;
    return false;
}

bool MuteGroupsEngine::IsGroupMutedByCmdId(int cmd) const
{
    for (const auto& e : m_groups)
        if (e.cmdId == cmd) return e.muted;
    return false;
}

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

// Parses a "<LTMUTEGROUP "Name"" header and reads subsequent child lines
// from the ProjectStateContext until ">" is encountered.
// Returns a heap-allocated MuteGroup* (caller must pass to AddDeserializedGroup).
MuteGroup* MuteGroup_Deserialize(const char* line, ProjectStateContext* ctx)
{
    auto* mg = new MuteGroup();

    // Parse the group name from the opening line:
    //   <LTMUTEGROUP "My Group"
    const char* p = line;
    while (*p == ' ' || *p == '\t' || *p == '<') ++p;
    // skip "LTMUTEGROUP"
    if (strncmp(p, "LTMUTEGROUP", 11) == 0) p += 11;
    while (*p == ' ' || *p == '\t') ++p;
    // strip optional quotes
    if (*p == '"') ++p;
    std::string name = p;
    // trim trailing quote and whitespace
    while (!name.empty() && (name.back() == '"' || name.back() == ' ' ||
                              name.back() == '\r' || name.back() == '\n'))
        name.pop_back();
    mg->name = name;

    // Read child lines
    char buf[512];
    while (ctx->GetLine(buf, sizeof(buf)) == 0)
    {
        // Trim leading whitespace
        const char* bl = buf;
        while (*bl == ' ' || *bl == '\t') ++bl;

        if (*bl == '>') break;  // end of block

        if (strncmp(bl, "LTMGTRACK ", 10) == 0)
        {
            std::string gs = bl + 10;
            while (!gs.empty() && (gs.back() == ' ' || gs.back() == '\r' || gs.back() == '\n'))
                gs.pop_back();

            // Convert GUID string to GUID
            int wlen = MultiByteToWideChar(CP_UTF8, 0, gs.c_str(), -1, nullptr, 0);
            if (wlen > 0)
            {
                std::vector<WCHAR> wbuf(wlen);
                MultiByteToWideChar(CP_UTF8, 0, gs.c_str(), -1, wbuf.data(), wlen);
                GUID g = {};
                CLSIDFromString(wbuf.data(), &g);
                mg->trackGuids.push_back(g);
            }
        }
    }

    return mg;
}

// Unregister gaccel entries on plugin unload.
void MuteGroupsEngine_Cleanup()
{
    for (gaccel_register_t* acc : s_gaccels)
    {
        plugin_register("-gaccel", acc);
        delete acc;
    }
    s_gaccels.clear();
    MuteGroupsEngine::Get().Clear();
}
