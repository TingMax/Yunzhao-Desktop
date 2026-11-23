// Pure geometry for the hosted calendar. The window the user drags is the
// calendar. A saved place on a display that is no longer connected is brought
// back once. Touching a screen edge does not hide it; collapse is a separate
// tab that stays inside that one display.

const MIN_OVERLAP = 48
const EDGE_MARGIN = 24
export const DOCK_SLACK = 8

export function overlapSize(bounds, area) {
  const left = Math.max(bounds.x, area.x)
  const top = Math.max(bounds.y, area.y)
  const right = Math.min(bounds.x + bounds.width, area.x + area.width)
  const bottom = Math.min(bounds.y + bounds.height, area.y + area.height)
  return { width: right - left, height: bottom - top }
}

export function intersectsArea(bounds, area, min = MIN_OVERLAP) {
  const size = overlapSize(bounds, area)
  return size.width >= min && size.height >= min
}

export function intersectsAny(bounds, areas, min = MIN_OVERLAP) {
  return areas.some((area) => intersectsArea(bounds, area, min))
}

export function fitOntoArea(bounds, area, margin = EDGE_MARGIN) {
  const width = Math.max(240, Math.min(bounds.width, Math.max(240, area.width - margin * 2)))
  const height = Math.max(180, Math.min(bounds.height, Math.max(180, area.height - margin * 2)))
  const x = Math.min(Math.max(bounds.x, area.x + margin), area.x + area.width - margin - width)
  const y = Math.min(Math.max(bounds.y, area.y + margin), area.y + area.height - margin - height)
  return { x: Math.round(x), y: Math.round(y), width: Math.round(width), height: Math.round(height) }
}

// live is the electron window. It wins whenever it is actually on a display,
// including when it is flush with an edge. saved is only used when the window
// itself is off every display.
export function calendarHome({ live, saved, areas, rest, holding }) {
  if (rest && holding) return rest
  if (live && intersectsAny(live, areas)) return live
  if (saved && !intersectsAny(saved, areas)) {
    const target = areas[0]
    return target ? fitOntoArea(saved, target) : saved
  }
  return saved || live || null
}

export function dockEdge(home, area, slack = DOCK_SLACK) {
  if (!home || !area) return ''
  const gapTop = home.y - area.y
  const gapLeft = home.x - area.x
  const gapRight = area.x + area.width - (home.x + home.width)
  const gapBottom = area.y + area.height - (home.y + home.height)
  const flush = (gap) => gap >= -slack && gap <= slack
  let edge = ''
  let gap = slack + 1
  const take = (name, value) => {
    if (!flush(value) || Math.abs(value) >= gap) return
    edge = name
    gap = Math.abs(value)
  }
  take('top', gapTop)
  take('left', gapLeft)
  take('right', gapRight)
  take('bottom', gapBottom)
  return edge
}

// Moving onto a display that came back, or onto the only display left.
// A window already on a display, including one parked on the top edge, stays.
export function relocationTarget(bounds, areas) {
  if (!bounds || intersectsAny(bounds, areas)) return null
  const target = areas[0]
  return target ? fitOntoArea(bounds, target) : null
}

// The calendar stays inside one display. A window that crosses two screens
// is pulled onto the display that was passed in.
export function containInArea(bounds, area) {
  const width = Math.max(1, Math.min(bounds.width, area.width))
  const height = Math.max(1, Math.min(bounds.height, area.height))
  const x = Math.min(Math.max(bounds.x, area.x), area.x + area.width - width)
  const y = Math.min(Math.max(bounds.y, area.y), area.y + area.height - height)
  return { x: Math.round(x), y: Math.round(y), width: Math.round(width), height: Math.round(height) }
}

export function nearestOpenEdge(bounds, area) {
  const gaps = [
    ['top', bounds.y - area.y],
    ['left', bounds.x - area.x],
    ['right', area.x + area.width - (bounds.x + bounds.width)]
  ]
  gaps.sort((a, b) => a[1] - b[1])
  return gaps[0][0]
}

// The island's further tuck: the bar stays the same size and slides until only
// `rim` pixels remain on the display. The rest hangs off that edge.
export function peekRim(home, area, edge, thick, length, rim) {
  const bar = peekTab(home, area, edge, thick, length)
  const visible = Math.max(1, Math.min(rim, edge === 'top' ? bar.height : bar.width))
  if (edge === 'top') return { ...bar, y: area.y - (bar.height - visible) }
  if (edge === 'left') return { ...bar, x: area.x - (bar.width - visible) }
  return { ...bar, x: area.x + area.width - visible }
}

export function peekTab(home, area, edge, thick, length) {
  const long = Math.min(length, edge === 'top' ? area.width : area.height)
  if (edge === 'top') {
    const x = Math.min(Math.max(home.x + home.width / 2 - long / 2, area.x), area.x + area.width - long)
    return { x: Math.round(x), y: area.y, width: Math.round(long), height: thick }
  }
  const y = Math.min(Math.max(home.y + home.height / 2 - long / 2, area.y), area.y + area.height - long)
  const x = edge === 'left' ? area.x : area.x + area.width - thick
  return { x: Math.round(x), y: Math.round(y), width: thick, height: Math.round(long) }
}

function islandEase(t) {
  const x1 = 0.16
  const y1 = 1
  const x2 = 0.3
  const y2 = 1
  const cx = 3 * x1
  const bx = 3 * (x2 - x1) - cx
  const ax = 1 - cx - bx
  const cy = 3 * y1
  const by = 3 * (y2 - y1) - cy
  const ay = 1 - cy - by
  const sampleX = (u) => ((ax * u + bx) * u + cx) * u
  const sampleY = (u) => ((ay * u + by) * u + cy) * u
  const slope = (u) => (3 * ax * u + 2 * bx) * u + cx
  let u = Math.min(1, Math.max(0, t))
  for (let i = 0; i < 6; i += 1) {
    const dx = slope(u)
    if (Math.abs(dx) < 1e-4) break
    u = Math.min(1, Math.max(0, u - (sampleX(u) - t) / dx))
  }
  return sampleY(u)
}

export function lerpBounds(from, to, t) {
  const k = islandEase(t)
  return {
    x: Math.round(from.x + (to.x - from.x) * k),
    y: Math.round(from.y + (to.y - from.y) * k),
    width: Math.max(1, Math.round(from.width + (to.width - from.width) * k)),
    height: Math.max(1, Math.round(from.height + (to.height - from.height) * k))
  }
}
