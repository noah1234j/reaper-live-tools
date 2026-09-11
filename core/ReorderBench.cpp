// ---------------------------------------------------------------------------
// ReorderBench.cpp  –  temporary diagnostic
//
// Settles one question: is ReorderSelectedTracks expensive per *call* or per
// *track moved*?
//
// A scene recall measured 45,804 ms across 86 calls that each moved a single
// track — 532 ms apiece. Relocating one item in a list of 114 cannot cost that
// much, so the time is almost certainly fixed overhead REAPER pays per call
// (relinking routing, rebuilding panel order, producing undo state). If that
// is right, the fix is to make fewer calls: the API moves every selected track
// at once, and the recall path uses it one track at a time.
//
// The test moves the last N tracks up by one and back down again, so the
// project ends in the order it started:
//
//   Pass A : 2N calls, one track each
//   Pass B : 2 calls, N tracks each
//
// Same number of track-relocations either way. If A and B cost about the same
// per call, the overhead is per call and batching is worth building. If B
// costs about N times one of A's calls, the cost follows the tracks and
// batching buys nothing.
//
// Delete this file once the question is answered.
// ---------------------------------------------------------------------------
#include "api.h"

#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace {

double BenchQpcMs()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return f.QuadPart ? (double)c.QuadPart * 1000.0 / (double)f.QuadPart : 0.0;
}

void SelectOnly(const std::vector<MediaTrack*>& sel)
{
    const int n = CountTracks(0);
    for (int i = 0; i < n; i++)
    {
        MediaTrack* tr = GetTrack(0, i);
        if (!tr) continue;
        int v = 0;
        GetSetMediaTrackInfo(tr, "I_SELECTED", &v);
    }
    for (MediaTrack* tr : sel)
    {
        if (!tr) continue;
        int v = 1;
        GetSetMediaTrackInfo(tr, "I_SELECTED", &v);
    }
}

int IndexOf(MediaTrack* tr)
{
    return (int)(intptr_t)GetSetMediaTrackInfo(tr, "IP_TRACKNUMBER", nullptr) - 1;
}

} // namespace

void LT_RunReorderBenchmark()
{
    const int total = CountTracks(0);
    if (total < 6)
    {
        ShowConsoleMsg("[Live Tools] Reorder benchmark needs at least 6 tracks.\n");
        return;
    }

    // Work on the tail of the project and keep N small: every relocation here
    // is a real project edit, and a smaller N keeps the worst case short if
    // the per-call cost turns out to be as bad as the recall suggested.
    const int N = (total - 2 < 12) ? (total - 2) : 12;

    // The block being moved: the last N tracks.
    std::vector<MediaTrack*> block;
    for (int i = total - N; i < total; i++)
        if (MediaTrack* tr = GetTrack(0, i)) block.push_back(tr);
    if ((int)block.size() != N)
    {
        ShowConsoleMsg("[Live Tools] Reorder benchmark: could not collect tracks.\n");
        return;
    }

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "[Live Tools] Reorder benchmark  (%d tracks in project, N=%d)\n"
        "  Moving the last %d tracks up one and back down, two ways.\n",
        total, N, N);
    ShowConsoleMsg(buf);

    PreventUIRefresh(1);

    // ---- Pass A: one track per call ------------------------------------
    // Each track is moved up one, then back down: 2N calls, 2N relocations.
    double aMs = 0.0;
    int    aCalls = 0;
    {
        const double t0 = BenchQpcMs();
        for (MediaTrack* tr : block)
        {
            int idx = IndexOf(tr);
            if (idx <= 0) continue;
            SelectOnly({ tr });
            ReorderSelectedTracks(idx - 1, 0);   // up one
            aCalls++;

            idx = IndexOf(tr);
            SelectOnly({ tr });
            ReorderSelectedTracks(idx + 2, 0);   // back down (insert-before semantics)
            aCalls++;
        }
        aMs = BenchQpcMs() - t0;
    }

    // ---- Pass B: the whole block per call -------------------------------
    // Same tracks, same kind of relocation, but 2 calls instead of 2N.
    double bMs = 0.0;
    int    bCalls = 0;
    {
        const int blockTop = IndexOf(block.front());
        const double t0 = BenchQpcMs();
        if (blockTop > 0)
        {
            SelectOnly(block);
            ReorderSelectedTracks(blockTop - 1, 0);         // whole block up one
            bCalls++;

            const int nowTop = IndexOf(block.front());
            SelectOnly(block);
            ReorderSelectedTracks(nowTop + N + 1, 0);       // whole block back down
            bCalls++;
        }
        bMs = BenchQpcMs() - t0;
    }

    PreventUIRefresh(-1);
    TrackList_AdjustWindows(false);
    UpdateArrange();

    const double aPerCall  = aCalls ? aMs / aCalls : 0.0;
    const double bPerCall  = bCalls ? bMs / bCalls : 0.0;
    const double aPerTrack = aCalls ? aMs / aCalls : 0.0;              // 1 track per call
    const double bPerTrack = (bCalls && N) ? bMs / (bCalls * N) : 0.0; // N tracks per call

    snprintf(buf, sizeof(buf),
        "  Pass A  one track per call : %9.2f ms over %d calls  = %8.2f ms/call  (%8.2f ms/track)\n"
        "  Pass B  whole block per call: %9.2f ms over %d calls  = %8.2f ms/call  (%8.2f ms/track)\n"
        "\n"
        "  If the two ms/call figures are close, the cost is per CALL and\n"
        "  batching the recall's reorder is worth building.\n"
        "  If Pass B's ms/call is roughly %d x Pass A's, the cost follows the\n"
        "  tracks and batching will not help.\n"
        "\n"
        "  Track order should be unchanged. Undo once if anything looks off.\n",
        aMs, aCalls, aPerCall, aPerTrack,
        bMs, bCalls, bPerCall, bPerTrack,
        N);
    ShowConsoleMsg(buf);
}
