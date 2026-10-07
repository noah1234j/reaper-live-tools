#pragma once
// ---------------------------------------------------------------------------
// TrackOrder.h  –  put a set of tracks into a wanted order with the fewest moves
//
// Shared by layer recall and scene recall. Both used to walk the wanted order
// and drop track k into slot k, one move per step. That is a selection sort:
// it is correct, but taking one channel from the top of a group to the bottom
// shifted every other channel up one at a time — n-1 moves (each one a full
// ReorderSelectedTracks, hundreds of ms on a large project) where one would do.
//
// The fewest moves is the group size minus the longest run of tracks that are
// already in the right order relative to each other (the longest increasing
// subsequence of their current positions, taken in the wanted order). Those
// tracks stay where they are; every other track is lifted out and dropped
// directly after its predecessor in the wanted order (or directly in front of
// the first track that stays, if it has no predecessor). So moving one
// channel costs exactly one move, and an order that already matches costs none.
//
// Folder safety is the caller's job and is kept by construction as long as the
// group shares one folder parent and no member opens or closes a folder
// (I_FOLDERDEPTH == 0): the slot directly after or before such a track lies in
// the same folder, so a moved track keeps its parent.
// ---------------------------------------------------------------------------
#include "api.h"
#include <vector>
#include <cstdint>

// 0-based project position of a track right now.
inline int TrackOrder_LiveIndex(MediaTrack* tr)
{
    if (!tr) return -1;
    return (int)(intptr_t)GetSetMediaTrackInfo(tr, "IP_TRACKNUMBER", nullptr) - 1;
}

// keep[i] is true for the members of one longest strictly increasing
// subsequence of pos. O(n log n).
inline std::vector<bool> TrackOrder_KeepSet(const std::vector<int>& pos)
{
    const int n = (int)pos.size();
    std::vector<int> tailIdx;          // tailIdx[len-1] = index ending the best run of that length
    std::vector<int> prev(n, -1);
    for (int i = 0; i < n; ++i)
    {
        int lo = 0, hi = (int)tailIdx.size();
        while (lo < hi)
        {
            const int mid = (lo + hi) / 2;
            if (pos[tailIdx[mid]] < pos[i]) lo = mid + 1; else hi = mid;
        }
        if (lo > 0) prev[i] = tailIdx[lo - 1];
        if (lo == (int)tailIdx.size()) tailIdx.push_back(i); else tailIdx[lo] = i;
    }
    std::vector<bool> keep(n, false);
    for (int i = tailIdx.empty() ? -1 : tailIdx.back(); i >= 0; i = prev[i]) keep[i] = true;
    return keep;
}

// Reorder group (listed in the wanted order) with the fewest moves.
// selectOnly(tr) must leave tr as the only selected track; it is called right
// before each move. Returns the number of moves made.
template <typename SelectOnly>
int TrackOrder_ApplyMinimal(const std::vector<MediaTrack*>& group, SelectOnly selectOnly)
{
    const int n = (int)group.size();
    if (n < 2) return 0;

    std::vector<int> pos(n);
    for (int i = 0; i < n; ++i)
    {
        pos[i] = TrackOrder_LiveIndex(group[i]);
        if (pos[i] < 0) return 0;      // not in the project any more
    }
    const std::vector<bool> keep = TrackOrder_KeepSet(pos);

    int firstKept = -1;
    for (int i = 0; i < n && firstKept < 0; ++i) if (keep[i]) firstKept = i;
    if (firstKept < 0) return 0;

    int moves = 0;
    for (int k = 0; k < n; ++k)
    {
        if (keep[k]) continue;

        // Positions are read live: every move renumbers the tracks between
        // its source and its destination.
        const int cur = TrackOrder_LiveIndex(group[k]);
        if (cur < 0) continue;

        // ReorderSelectedTracks inserts before an index in the current list.
        int beforeIdx;
        if (k == 0)
            beforeIdx = TrackOrder_LiveIndex(group[firstKept]);
        else
            beforeIdx = TrackOrder_LiveIndex(group[k - 1]) + 1;
        if (beforeIdx < 0) continue;
        if (beforeIdx == cur || beforeIdx == cur + 1) continue;   // already there

        selectOnly(group[k]);
        ReorderSelectedTracks(beforeIdx, 0);
        ++moves;
    }
    return moves;
}
