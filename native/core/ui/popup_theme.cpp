// The popup placement helpers upstream 3.0 added (Theme itself stays in surface.h).
#include "ui/popup_theme.h"

#include "bar_edge.h"

namespace yunzhao {

POINT PopupAnchor(RECT bar, RECT rect)
{
    switch (EdgeOf(bar)) {
    case BarEdge::Top: return {(rect.left + rect.right) / 2, rect.bottom};
    case BarEdge::Left: return {rect.right, (rect.top + rect.bottom) / 2};
    case BarEdge::Right: return {rect.left, (rect.top + rect.bottom) / 2};
    default: return {(rect.left + rect.right) / 2, rect.top};
    }
}

POINT PopupOrigin(RECT bar, RECT popup, RECT anchor)
{
    const BarEdge edge = EdgeOf(bar);
    const POINT a = PopupAnchor(bar, anchor);
    if (Vertical(edge)) {
        const LONG y = anchor.bottom > anchor.top ? a.y : popup.top + (popup.bottom - popup.top) / 6;
        return {edge == BarEdge::Left ? popup.left : popup.right, y};
    }
    const LONG x = anchor.right > anchor.left ? a.x : popup.left + (popup.right - popup.left) / 6;
    return {x, edge == BarEdge::Top ? popup.top : popup.bottom};
}

} // namespace yunzhao
