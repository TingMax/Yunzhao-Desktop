// The strip at the screen's own edge that summons a hidden bar.
#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace yunzhao {

// In DIP and in screen px. The pointer has to be inside it: resting near the bottom of the screen
// is not a summon, or the bar jumps out whenever the pointer crosses the bottom of a window in use.
constexpr float kRevealEdgeDip = 3.f;
inline int RevealEdgePx(float scale)
{
    const float sc = scale > 0.25f && scale < 8.f ? scale : 1.f;
    return std::clamp(int(std::lround(kRevealEdgeDip * sc)), 2, 8);
}

// A pointer on `monitor`'s own bottom edge, strictly inside its width. Only the last few pixels
// count: the work area's edge, a window's bottom border and a hover near the bottom all sit above
// it, and treating those as "the edge" made the bar jump out over a window in use.
inline bool TouchingBottomEdge(POINT pointer, const RECT& monitor, float scale)
{
    return pointer.x >= monitor.left && pointer.x < monitor.right && pointer.y >= monitor.bottom - RevealEdgePx(scale) &&
           pointer.y >= monitor.top;
}

// The same test for a monitor handle, which is what the bar's reveal loop has at hand.
inline bool TouchingBottomEdge(POINT pointer, HMONITOR monitor, float scale)
{
    MONITORINFO mi{sizeof(mi)};
    if (!monitor || !GetMonitorInfoW(monitor, &mi))
        return false;
    return TouchingBottomEdge(pointer, mi.rcMonitor, scale);
}

} // namespace yunzhao
