#pragma once

#ifdef _WIN32
#  include <windows.h>
#else
#  include "../sws-master-2.14.0 - Copy/vendor/WDL/WDL/swell/swell.h"
#endif

#include <string>
#include <vector>
#include <ctime>

class ProjectStateContext;

// ---------------------------------------------------------------------------
// Mask bits – map 1:1 to the UI checkboxes in LayoutsWnd
// ---------------------------------------------------------------------------
static const int LY_ORDER  = 0x01;  // Track order (position in TCP)
static const int LY_HEIGHT = 0x02;  // Track height / spacer sizes (I_HEIGHTOVERRIDE)
static const int LY_VIS    = 0x04;  // TCP/MCP visibility
static const int LY_NAME   = 0x08;  // Track name

static const int LY_ALL    = (LY_ORDER | LY_HEIGHT | LY_VIS | LY_NAME);

// ---------------------------------------------------------------------------
// LayoutTrackState – per-track data stored in a Layout
// ---------------------------------------------------------------------------
struct LayoutTrackState
{
    GUID        guid     = {};
    int         position = 0;   // 0-based index at capture time (also restore target)
    int         height   = 0;   // I_HEIGHTOVERRIDE value (0 = default height)
    int         vis      = 3;   // bit0 = show in TCP, bit1 = show in MCP
    std::string name;
};

// ---------------------------------------------------------------------------
// LayoutSnapshot – top-level container
// ---------------------------------------------------------------------------
class LayoutSnapshot
{
public:
    LayoutSnapshot(int slot, const char* name);

    // Capture the current REAPER track layout into this snapshot.
    void Capture(int mask);

    // Apply this snapshot to the current project.
    // All operations are visual-only (no audio graph changes).
    void Recall(int mask) const;

    // Project-state persistence (same pattern as TransitionSnapshot)
    void Serialize(ProjectStateContext* ctx) const;
    static LayoutSnapshot* Deserialize(const char* headerLine,
                                       ProjectStateContext* ctx);

    // ---- Public data -------------------------------------------------------
    int         m_slot = 0;
    std::string m_name;
    std::string m_notes;
    int         m_mask = 0;
    int         m_time = 0;   // Unix timestamp (capture time)

    std::vector<LayoutTrackState> m_tracks;

private:
    // Find the current position of a track by GUID. Returns -1 if not found.
    static int FindTrackIndexByGuid(const GUID& guid);
};
