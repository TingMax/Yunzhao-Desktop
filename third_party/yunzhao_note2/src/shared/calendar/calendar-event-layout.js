import { dateKeyFromOrdinal, dateOrdinal, noteDateRange } from './calendar-date-rules.js'

function compareNotes(left, right, referenceDate) {
  const previewDifference =
    Number(left.preview_kind === 'recurrence') - Number(right.preview_kind === 'recurrence')
  if (previewDifference) return previewDifference
  const completionDifference =
    Number(left.status === 'completed') - Number(right.status === 'completed')
  if (completionDifference) return completionDifference
  const leftRange = noteDateRange(left, referenceDate)
  const rightRange = noteDateRange(right, referenceDate)
  if (leftRange.durationDays !== rightRange.durationDays) {
    return rightRange.durationDays - leftRange.durationDays
  }
  const pinDifference = Number(Boolean(right.is_pinned)) - Number(Boolean(left.is_pinned))
  if (pinDifference) return pinDifference
  const timeDifference = Number(left.effective_at) - Number(right.effective_at)
  if (timeDifference) return timeDifference
  return String(left.id).localeCompare(String(right.id), 'zh-CN', { numeric: true })
}

function laneIsFree(weekLanes, lane, columnStart, columnSpan) {
  const mask = ((1 << columnSpan) - 1) << (columnStart - 1)
  return ((weekLanes[lane] || 0) & mask) === 0
}

function occupyLane(weekLanes, lane, columnStart, columnSpan) {
  const mask = ((1 << columnSpan) - 1) << (columnStart - 1)
  weekLanes[lane] = (weekLanes[lane] || 0) | mask
}

/**
 * 把真实待办区间裁剪到当前日历网格，并按自然周拆为横条。横条只保存布局信息，
 * 不复制或改写待办数据。lane 在各周之间尽量连续，且分配顺序稳定可测试。
 */
export function buildCalendarEventSegments(
  days,
  notes,
  { activeStartKey, activeEndKey, referenceDate = Date.now() } = {}
) {
  if (!days?.length || !notes?.length) return []
  const gridStartOrdinal = dateOrdinal(days[0].key)
  const gridEndOrdinal = dateOrdinal(days[days.length - 1].key)
  const visibleStartOrdinal = Math.max(
    gridStartOrdinal,
    activeStartKey ? dateOrdinal(activeStartKey) : gridStartOrdinal
  )
  const visibleEndOrdinal = Math.min(
    gridEndOrdinal,
    activeEndKey ? dateOrdinal(activeEndKey) : gridEndOrdinal
  )
  const weekLanes = Array.from({ length: Math.ceil(days.length / 7) }, () => [])
  const previousLaneByNote = new Map()
  const segments = []

  for (const note of [...notes].sort((left, right) => compareNotes(left, right, referenceDate))) {
    const itemId = note.id
    const range = noteDateRange(note, referenceDate)
    const clippedStart = Math.max(range.startOrdinal, visibleStartOrdinal)
    const clippedEnd = Math.min(range.endOrdinal, visibleEndOrdinal)
    if (clippedStart > clippedEnd) continue

    const firstWeek = Math.floor((clippedStart - gridStartOrdinal) / 7)
    const lastWeek = Math.floor((clippedEnd - gridStartOrdinal) / 7)
    for (let weekIndex = firstWeek; weekIndex <= lastWeek; weekIndex += 1) {
      const weekStart = gridStartOrdinal + weekIndex * 7
      const segmentStart = Math.max(clippedStart, weekStart)
      const segmentEnd = Math.min(clippedEnd, weekStart + 6)
      const columnStart = segmentStart - weekStart + 1
      const columnSpan = segmentEnd - segmentStart + 1
      const preferredLane = previousLaneByNote.get(itemId)
      let lane = 0
      if (
        Number.isInteger(preferredLane) &&
        laneIsFree(weekLanes[weekIndex], preferredLane, columnStart, columnSpan)
      ) {
        lane = preferredLane
      } else {
        while (!laneIsFree(weekLanes[weekIndex], lane, columnStart, columnSpan)) lane += 1
      }
      occupyLane(weekLanes[weekIndex], lane, columnStart, columnSpan)
      previousLaneByNote.set(itemId, lane)
      segments.push({
        noteId: itemId,
        weekIndex,
        columnStart,
        columnSpan,
        startKey: dateKeyFromOrdinal(segmentStart),
        endKey: dateKeyFromOrdinal(segmentEnd),
        continuesBefore: segmentStart > range.startOrdinal,
        continuesAfter: segmentEnd < range.endOrdinal,
        lane
      })
    }
  }
  return segments.sort(
    (left, right) =>
      left.weekIndex - right.weekIndex ||
      left.lane - right.lane ||
      left.columnStart - right.columnStart ||
      String(left.noteId).localeCompare(String(right.noteId), 'zh-CN', { numeric: true })
  )
}

export function notesCoveringDate(notes, dateKey, referenceDate = Date.now()) {
  const target = dateOrdinal(dateKey)
  return (notes || [])
    .filter((note) => {
      const range = noteDateRange(note, referenceDate)
      return range.startOrdinal <= target && range.endOrdinal >= target
    })
    .sort((left, right) => compareNotes(left, right, referenceDate))
}

export function noteCountsByDate(days, notes, referenceDate = Date.now()) {
  return new Map(
    (days || [])
      .map((day) => [day.key, notesCoveringDate(notes, day.key, referenceDate).length])
      .filter(([, count]) => count > 0)
  )
}

// A week lane stays put so a long note does not jump rows on the days it shares.
// A date that only carries the tail of that note would otherwise draw it in an
// empty row and then clip it. Each date packs the notes it actually has.
export function calendarDayChips(segments) {
  const byDay = new Map()
  for (const segment of segments || []) {
    for (let offset = 0; offset < segment.columnSpan; offset += 1) {
      const column = segment.columnStart + offset
      const key = `${segment.weekIndex}:${column}`
      if (!byDay.has(key)) byDay.set(key, [])
      byDay.get(key).push({ segment, offset })
    }
  }
  const chips = []
  for (const [key, items] of byDay) {
    const [weekIndex, column] = key.split(':').map(Number)
    items.sort(
      (left, right) =>
        left.segment.lane - right.segment.lane ||
        String(left.segment.noteId).localeCompare(String(right.segment.noteId), 'zh-CN', { numeric: true })
    )
    items.forEach(({ segment, offset }, lane) => {
      const first = offset === 0 && !segment.continuesBefore
      chips.push({
        noteId: segment.noteId,
        weekIndex,
        columnStart: column,
        columnSpan: 1,
        startKey: segment.startKey,
        endKey: segment.endKey,
        continuesBefore: !first,
        continuesAfter: offset === segment.columnSpan - 1 && segment.continuesAfter,
        lane
      })
    })
  }
  return chips.sort(
    (left, right) =>
      left.weekIndex - right.weekIndex ||
      left.columnStart - right.columnStart ||
      left.lane - right.lane ||
      String(left.noteId).localeCompare(String(right.noteId), 'zh-CN', { numeric: true })
  )
}

export function hasHiddenCalendarNotes(totalCount, visibleCount) {
  const total = Math.max(0, Math.trunc(Number(totalCount) || 0))
  const visible = Math.max(0, Math.trunc(Number(visibleCount) || 0))
  return total > visible
}
