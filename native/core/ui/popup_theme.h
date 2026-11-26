// Popup theme and placement helpers.
//
// Upstream 3.0 moved Theme out of surface.h into this file, together with the edge-aware
// placement helpers (PopupAnchor, PopupOrigin) that the arc-shaped popups need. Yunzhao keeps
// Theme where it has always lived (surface.h) so the eighteen files that use it are untouched;
// this header only forwards to it and adds what was missing.
#pragma once

#include "surface.h"

#include <windows.h>
#include <d2d1_3.h>

namespace yunzhao {

// The anchor of a popup over the bar (screen px): the point on the bar's glass the popup grows
// from, on whichever edge the bar sits. Upstream 3.0; the bar sits at the bottom here, so this
// is the bar's top edge at the popup's centre, kept inside the bar's span.
POINT PopupAnchor(RECT bar, RECT rect);
// Where a popup of `popup` size goes when it belongs to `anchor`: centred on the anchor and kept
// inside the work area, above the bar (the bar is at the bottom), the popup's gap clear of it.
POINT PopupOrigin(RECT bar, RECT popup, RECT anchor);

} // namespace yunzhao
