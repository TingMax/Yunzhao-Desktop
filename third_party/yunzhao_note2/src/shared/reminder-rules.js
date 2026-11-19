export const REMINDER_CHANNELS = Object.freeze({ system: 1, popup: 2, tray: 4, force: 8 })
export const SNOOZE_MINUTES = Object.freeze([5, 10, 30, 60])

export function normalizeReminderChannels(value, legacyEnabled = false) {
  if (value === undefined || value === null) return legacyEnabled ? 1 : 0
  const channels = Number(value)
  if (!Number.isInteger(channels) || channels < 0 || channels > 15) {
    throw new Error('提醒方式无效')
  }
  return channels
}

export function assertSnoozeTime(value, now = Date.now()) {
  const dueAt = Number(value)
  if (!Number.isSafeInteger(dueAt) || dueAt <= now || dueAt > now + 366 * 86400000) {
    throw new Error('请选择未来一年内的提醒时间')
  }
  return dueAt
}

/** Force popup follows the daily clock. Other channels still require a future effective time. */
export function storedReminderChannels(
  value,
  { completed = false, canScheduleAtEffectiveTime = false } = {}
) {
  if (completed) return 0
  const channels = normalizeReminderChannels(value)
  return canScheduleAtEffectiveTime ? channels : channels & REMINDER_CHANNELS.force
}

export function reminderChannelLabels(value) {
  const channels = normalizeReminderChannels(value)
  return [
    [1, '系统提醒'],
    [2, '软件弹窗'],
    [4, '托盘闪烁'],
    [8, '桌面强制弹窗']
  ]
    .filter(([bit]) => channels & bit)
    .map(([, label]) => label)
    .join('、')
}
