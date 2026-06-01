#include "LayoutSnapshot.h"
#include "api.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
LayoutSnapshot::LayoutSnapshot(int slot, const char* name)
    : m_slot(slot)
    , m_name(name ? name : "")
    , m_mask(0)
    , m_time((int)std::time(nullptr))
{
}

// ---------------------------------------------------------------------------
// GUID helpers (Windows-only; no WDL required)
// ---------------------------------------------------------------------------
static std::string GuidToStr(const GUID& g)
{
    WCHAR wbuf[64];
    StringFromGUID2(g, wbuf, 64);
    char buf[64];
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
// FindTrackIndexByGuid
// Linear scan – only called during Recall; negligible for live track counts.
// ---------------------------------------------------------------------------
int LayoutSnapshot::FindTrackIndexByGuid(const GUID& guid)
{
    const int n = GetNumTracks();
    for (int i = 0; i < n; i++)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;
        GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
        if (pg && memcmp(pg, &guid, sizeof(GUID)) == 0)
            return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Capture
// Single pass over all tracks, O(N). No audio-graph reads.
// ---------------------------------------------------------------------------
void LayoutSnapshot::Capture(int mask)
{
    m_mask = mask;
    m_tracks.clear();

    const int n = GetNumTracks();
    m_tracks.reserve(n);

    for (int i = 0; i < n; i++)
    {
        MediaTrack* tr = GetTrack(nullptr, i);
        if (!tr) continue;

        LayoutTrackState lts;
        lts.position = i;

        // GUID always captured (needed for identity on recall)
        GUID* pg = (GUID*)GetSetMediaTrackInfo(tr, "GUID", nullptr);
        if (pg) lts.guid = *pg;

        if (mask & LY_HEIGHT)
        {
            int* ph = (int*)GetSetMediaTrackInfo(tr, "I_HEIGHTOVERRIDE", nullptr);
            lts.height = ph ? *ph : 0;
        }

        if (mask & LY_VIS)
        {
            int* pt = (int*)GetSetMediaTrackInfo(tr, "I_SHOWINTCP",   nullptr);
            int* pm = (int*)GetSetMediaTrackInfo(tr, "I_SHOWINMIXER", nullptr);
            int tv = pt ? (*pt & 1) : 1;
            int mv = pm ? (*pm & 1) : 1;
            lts.vis = tv | (mv << 1);
        }

        if (mask & LY_NAME)
        {
            char namebuf[512] = "";
            GetSetMediaTrackInfo_String(tr, "P_NAME", namebuf, false);
            lts.name = namebuf;
        }

        m_tracks.push_back(std::move(lts));
    }
}

// ---------------------------------------------------------------------------
// Recall
// All visual-only operations; no audio graph changes.
// ---------------------------------------------------------------------------
void LayoutSnapshot::Recall(int mask) const
{
    if (m_tracks.empty()) return;

    PreventUIRefresh(1);

    // 1) Names – no ordering dependency
    if (mask & LY_NAME)
    {
        for (const auto& lts : m_tracks)
        {
            int idx = FindTrackIndexByGuid(lts.guid);
            if (idx < 0) continue;
            MediaTrack* tr = GetTrack(nullptr, idx);
            if (!tr) continue;
            char buf[512];
            snprintf(buf, sizeof(buf), "%s", lts.name.c_str());
            GetSetMediaTrackInfo_String(tr, "P_NAME", buf, true);
        }
    }

    // 2) Visibility
    if (mask & LY_VIS)
    {
        for (const auto& lts : m_tracks)
        {
            int idx = FindTrackIndexByGuid(lts.guid);
            if (idx < 0) continue;
            MediaTrack* tr = GetTrack(nullptr, idx);
            if (!tr) continue;
            int tv = (lts.vis & 1);
            int mv = (lts.vis >> 1) & 1;
            GetSetMediaTrackInfo(tr, "I_SHOWINTCP",   &tv);
            GetSetMediaTrackInfo(tr, "I_SHOWINMIXER", &mv);
        }
    }

    // 3) Heights / spacer sizes
    if (mask & LY_HEIGHT)
    {
        for (const auto& lts : m_tracks)
        {
            int idx = FindTrackIndexByGuid(lts.guid);
            if (idx < 0) continue;
            MediaTrack* tr = GetTrack(nullptr, idx);
            if (!tr) continue;
            int h = lts.height;
            GetSetMediaTrackInfo(tr, "I_HEIGHTOVERRIDE", &h);
        }
    }

    // 4) Track order
    // Strategy: iterate target positions 0..N-1.
    // For each, find where the desired track currently sits and move it there
    // using ReorderSelectedTracks, which moves selected tracks to just before
    // a given index.  We deselect all first, select only the one track, move it.
    if (mask & LY_ORDER)
    {
        // Deselect all tracks once up front
        {
            const int n = GetNumTracks();
            int zero = 0;
            for (int i = 0; i < n; i++)
            {
                MediaTrack* tr = GetTrack(nullptr, i);
                if (tr) GetSetMediaTrackInfo(tr, "I_SELECTED", &zero);
            }
        }

        for (int targetPos = 0; targetPos < (int)m_tracks.size(); targetPos++)
        {
            const LayoutTrackState& lts = m_tracks[targetPos];
            int curPos = FindTrackIndexByGuid(lts.guid);
            if (curPos < 0 || curPos == targetPos) continue;

            // Deselect all, then select only this track
            {
                const int n = GetNumTracks();
                int zero = 0, one = 1;
                for (int i = 0; i < n; i++)
                {
                    MediaTrack* tr = GetTrack(nullptr, i);
                    if (!tr) continue;
                    GetSetMediaTrackInfo(tr, "I_SELECTED",
                        (i == curPos) ? &one : &zero);
                }
            }

            // ReorderSelectedTracks(beforeIdx, makePrevFolder):
            //   moves selected tracks to immediately before beforeIdx.
            // If targetPos < curPos the insert position is targetPos;
            // if targetPos > curPos the insert is after the shift so use targetPos+1.
            int insertBefore = (targetPos < curPos) ? targetPos : targetPos + 1;
            ReorderSelectedTracks(insertBefore, 0);
        }

        // Restore all tracks to deselected
        {
            const int n = GetNumTracks();
            int zero = 0;
            for (int i = 0; i < n; i++)
            {
                MediaTrack* tr = GetTrack(nullptr, i);
                if (tr) GetSetMediaTrackInfo(tr, "I_SELECTED", &zero);
            }
        }
    }

    TrackList_AdjustWindows(false);
    PreventUIRefresh(-1);

    Undo_OnStateChangeEx("Recall Layout", -1, -1);
}

// ---------------------------------------------------------------------------
// Serialize
// Format:
//   <LTLAYOUT slot mask time "name"
//     TRACK {guid} position height vis "trackname"
//     ...
//   >
// ---------------------------------------------------------------------------
void LayoutSnapshot::Serialize(ProjectStateContext* ctx) const
{
    // Escape double-quotes in the layout name
    std::string safeName = m_name;
    for (char& c : safeName) if (c == '"') c = '\'';

    ctx->AddLine("<LTLAYOUT %d %d %d \"%s\"",
                 m_slot, m_mask, m_time, safeName.c_str());

    for (const auto& lts : m_tracks)
    {
        std::string sguid = GuidToStr(lts.guid);

        // Escape double-quotes in track name
        std::string safeTName = lts.name;
        for (char& c : safeTName) if (c == '"') c = '\'';

        ctx->AddLine("TRACK %s %d %d %d \"%s\"",
                     sguid.c_str(),
                     lts.position,
                     lts.height,
                     lts.vis,
                     safeTName.c_str());
    }

    ctx->AddLine(">");
}

// ---------------------------------------------------------------------------
// Deserialize
// headerLine = "LTLAYOUT slot mask time \"name\""
// ctx->GetLine reads child lines; ">" ends the block.
// ---------------------------------------------------------------------------
LayoutSnapshot* LayoutSnapshot::Deserialize(const char* headerLine,
                                             ProjectStateContext* ctx)
{
    int slot = 0, mask = 0, ts_time = 0;
    // headerLine arrives with the leading '<' (REAPER does not strip it)
    if (sscanf(headerLine, "<LTLAYOUT %d %d %d", &slot, &mask, &ts_time) < 3)
        return nullptr;

    // Extract quoted layout name
    char lname[512] = "";
    const char* q = strchr(headerLine, '"');
    if (q)
    {
        q++;
        int i = 0;
        while (*q && *q != '"' && i < (int)sizeof(lname) - 1)
            lname[i++] = *q++;
        lname[i] = '\0';
    }

    auto* ly   = new LayoutSnapshot(slot, lname);
    ly->m_mask = mask;
    ly->m_time = ts_time;

    char line[4096];
    while (ctx->GetLine(line, sizeof(line)) == 0)
    {
        char* t = line;
        while (*t == ' ' || *t == '\t') t++;

        if (strcmp(t, ">") == 0) break;

        if (strncmp(t, "TRACK ", 6) == 0)
        {
            LayoutTrackState lts;

            char sguid[64] = "";
            int pos = 0, height = 0, vis = 3;
            sscanf(t, "TRACK %63s %d %d %d", sguid, &pos, &height, &vis);
            lts.guid     = StrToGuid(sguid);
            lts.position = pos;
            lts.height   = height;
            lts.vis      = vis;

            // Extract quoted track name (optional)
            const char* nq = strchr(t + 6, '"');
            if (nq)
            {
                nq++;
                int i = 0;
                char nbuf[512] = "";
                while (*nq && *nq != '"' && i < (int)sizeof(nbuf) - 1)
                    nbuf[i++] = *nq++;
                nbuf[i] = '\0';
                lts.name = nbuf;
            }

            ly->m_tracks.push_back(std::move(lts));
        }
    }

    return ly;
}
