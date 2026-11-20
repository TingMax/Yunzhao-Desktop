import { createRequire } from 'node:module'
import { app, BrowserWindow, Notification, screen, ipcMain } from 'electron'
import { existsSync, readFileSync, unlinkSync, watch, writeFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { spawn } from 'node:child_process'
import { calendarHome, containInArea, fitOntoArea, intersectsAny, lerpBounds, nearestOpenEdge, peekRim, peekTab } from './atrium-display.js'
import { scheduleCloudSync } from './cloud-sync.js'
import { getDb } from './db/db.js'
import { addCompanionWindow, removeCompanionWindow } from './ipc/companion-windows.js'

function atriumRoot() {
  return join(process.env.LOCALAPPDATA || '', 'Yunzhao')
}

function signedInDir() {
  const root = atriumRoot()
  const safe = (id) => typeof id === 'string' && /^[0-9a-fA-F-]{32,40}$/.test(id) && !/[\\/:. ]/.test(id)
  const fromEnv = process.env.YUNZHAO_ACCOUNT_ID || ''
  if (safe(fromEnv)) return join(root, 'users', fromEnv)
  try {
    const id = JSON.parse(readFileSync(join(root, 'account.json'), 'utf8')).id
    if (safe(id)) return join(root, 'users', id)
  } catch {
    // Signed-out desktop still shares the Atrium folder.
  }
  return root
}

const require = createRequire(import.meta.url)

// The desktop shell can still paint its own timetable until it is rebuilt.
// That face is a second calendar. Keep its window region empty so one page
// remains, on one monitor or two and on either desktop.
let nativeCalendarClip
function ensureClipApi() {
  if (nativeCalendarClip) return nativeCalendarClip
  const koffi = require('koffi')
  const user32 = koffi.load('user32.dll')
  const gdi32 = koffi.load('gdi32.dll')
  const rectType = koffi.struct('AtriumClipRect', {
    left: 'int32',
    top: 'int32',
    right: 'int32',
    bottom: 'int32'
  })
  nativeCalendarClip = {
    find: user32.func('uintptr_t FindWindowExW(uintptr_t, uintptr_t, str16, str16)'),
    visible: user32.func('int IsWindowVisible(uintptr_t)'),
    region: user32.func('int GetWindowRgn(uintptr_t, void *)'),
    setRegion: user32.func('int SetWindowRgn(uintptr_t, void *, int)'),
    create: gdi32.func('void * CreateRectRgn(int, int, int, int)'),
    round: gdi32.func('void * CreateRoundRectRgn(int, int, int, int, int, int)'),
    offset: gdi32.func('int OffsetRgn(void *, int, int)'),
    combine: gdi32.func('int CombineRgn(void *, void *, void *, int)'),
    drop: gdi32.func('int DeleteObject(void *)'),
    box: gdi32.func('GetRgnBox', 'int', ['void *', koffi.out(koffi.pointer(rectType))])
  }
  return nativeCalendarClip
}
function hideNativeCalendarFace() {
  try {
    const api = ensureClipApi()
    let after = 0n
    for (let i = 0; i < 32; i += 1) {
      const hwnd = api.find(0n, after, 'AtriumSurface', 'Yunzhao Widget')
      if (!hwnd) break
      after = hwnd
      if (!api.visible(hwnd)) continue
      const probe = api.create(0, 0, 1, 1)
      const kind = api.region(hwnd, probe)
      const box = {}
      api.box(probe, box)
      api.drop(probe)
      const width = Number(box.right) - Number(box.left)
      const height = Number(box.bottom) - Number(box.top)
      if ((kind !== 2 && kind !== 3) || width < 640 || height < 400) continue
      const empty = api.create(0, 0, 1, 1)
      const other = api.create(0, 0, 1, 1)
      api.combine(empty, empty, other, 3)
      api.drop(other)
      if (!api.setRegion(hwnd, empty, 1)) api.drop(empty)
    }
  } catch {
    // The hosted page is still the only calendar this app opens.
  }
}

const placeDir = atriumRoot()
const dataDir = signedInDir()
const placePath = join(placeDir, 'yunzhao-place.json')
const hwndPath = join(placeDir, 'yunzhao-hwnd.txt')
const todosPath = join(dataDir, 'todos.json')
const statePath = join(dataDir, 'yunzhao-state.json')
const tablePath = join(dataDir, 'yunzhao-table.json')
const cmdPath = join(dataDir, 'yunzhao-cmd.json')
const handoutsPath = join(dataDir, 'handouts.json')
const dailyPlanPath = join(dataDir, 'daily-reminder.json')

function readDailyPlan() {
  try {
    const data = JSON.parse(readFileSync(dailyPlanPath, 'utf8'))
    const time = typeof data.time === 'string' && /^\d{2}:\d{2}$/.test(data.time) ? data.time : '08:00'
    return { time, channels: Math.max(0, Math.min(15, Number(data.channels) || 0)), fired: String(data.fired || '') }
  } catch {
    return { time: '08:00', channels: 8, fired: '' }
  }
}

function escapeHtml(value) {
  return String(value).replace(/[&<>]/g, (ch) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' })[ch])
}

function incompleteTodosThroughToday() {
  try {
    const db = getDb()
    if (!db) return []
    const start = new Date()
    start.setHours(0, 0, 0, 0)
    const startMs = start.getTime()
    const rows = db
      .prepare(
        `SELECT content, effective_at FROM notes
         WHERE is_deleted = 0 AND status != 'completed' AND effective_at IS NOT NULL AND effective_at < ?
         ORDER BY effective_at ASC, id ASC`
      )
      .all(startMs + 86400000)
    return rows.map((row) => {
      const when = new Date(Number(row.effective_at))
      const text = String(row.content || '').replace(/<[^>]+>/g, '').replace(/\s+/g, ' ').trim() || '未命名待办'
      return {
        past: Number(row.effective_at) < startMs,
        label: `${when.getMonth() + 1}月${when.getDate()}日`,
        text
      }
    })
  } catch {
    return []
  }
}

function openBlockingTodoPopup(items) {
  const displays = screen.getAllDisplays()
  let left = Infinity
  let top = Infinity
  let right = -Infinity
  let bottom = -Infinity
  for (const display of displays) {
    const bounds = display.bounds
    left = Math.min(left, bounds.x)
    top = Math.min(top, bounds.y)
    right = Math.max(right, bounds.x + bounds.width)
    bottom = Math.max(bottom, bounds.y + bounds.height)
  }
  const area = screen.getDisplayNearestPoint(screen.getCursorScreenPoint()).bounds
  const width = Math.min(460, Math.max(320, area.width - 48))
  const height = Math.min(520, Math.max(280, area.height - 80))
  const dialog = new BrowserWindow({
    x: Math.round(area.x + (area.width - width) / 2),
    y: Math.round(area.y + (area.height - height) / 2),
    width,
    height,
    frame: false,
    transparent: false,
    thickFrame: false,
    resizable: false,
    movable: false,
    minimizable: false,
    maximizable: false,
    fullscreenable: false,
    skipTaskbar: true,
    focusable: true,
    alwaysOnTop: true,
    backgroundColor: '#ffffff',
    title: '未完成待办',
    autoHideMenuBar: true,
    webPreferences: { sandbox: true, contextIsolation: true }
  })
  const dimmer = new BrowserWindow({
    x: left,
    y: top,
    width: Math.max(1, right - left),
    height: Math.max(1, bottom - top),
    frame: false,
    transparent: true,
    thickFrame: false,
    resizable: false,
    movable: false,
    focusable: false,
    skipTaskbar: true,
    hasShadow: false,
    show: false,
    alwaysOnTop: true,
    backgroundColor: '#00000000',
    webPreferences: { sandbox: true }
  })
  dimmer.setAlwaysOnTop(true, 'pop-up-menu')
  dialog.setAlwaysOnTop(true, 'screen-saver')
  let allowClose = false
  let finished = false
  const blocked = BrowserWindow.getAllWindows().filter(
    (win) => win !== dialog && win !== dimmer && !win.isDestroyed() && win.isEnabled()
  )
  const finish = () => {
    if (finished) return
    finished = true
    allowClose = true
    if (!dialog.isDestroyed()) dialog.close()
  }
  dialog.on('close', (event) => {
    if (!allowClose) event.preventDefault()
  })
  const timer = setInterval(() => {
    if (dialog.isDestroyed() || dialog.isFocused()) return
    dialog.setAlwaysOnTop(true, 'screen-saver')
    dialog.moveTop()
  }, 800)
  const release = () => {
    clearInterval(timer)
    for (const win of blocked) {
      if (!win.isDestroyed()) win.setEnabled(true)
    }
    if (!dimmer.isDestroyed()) dimmer.close()
  }
  dialog.on('closed', release)
  dialog.webContents.on('before-input-event', (event, input) => {
    if (input.type !== 'keyDown' || (input.key !== 'Enter' && input.key !== 'Escape')) return
    event.preventDefault()
    finish()
  })
  const rows = items
    .map((item) => {
      const color = item.past ? '#ff453a' : '#1d1d1f'
      const dateColor = item.past ? '#ff453a' : '#6e6e73'
      return `<div style="display:flex;gap:12px;padding:8px 0;border-top:1px solid #ececf1"><span style="flex:0 0 64px;color:${dateColor}">${escapeHtml(item.label)}</span><span style="min-width:0;color:${color}">${escapeHtml(item.text)}</span></div>`
    })
    .join('')
  const html = `<!doctype html><meta charset="utf-8"><title>未完成待办</title><body style="margin:0;height:100vh;display:flex;flex-direction:column;background:#fff;color:#1d1d1f;font-family:Segoe UI,Microsoft YaHei,sans-serif"><div style="padding:22px 24px 8px;font-size:18px;font-weight:650">未完成待办</div><div style="padding:0 24px;flex:1;overflow:auto;font-size:15px;line-height:1.5">${rows}</div><button id="ok" type="button" style="margin:12px 24px 22px;height:40px;border:0;border-radius:8px;background:#0a84ff;color:#fff;font-size:14px;cursor:pointer">确认</button></body>`
  dimmer.loadURL(
    'data:text/html;charset=utf-8,' +
      encodeURIComponent('<!doctype html><body style="margin:0;height:100vh;background:rgba(0,0,0,.28)"></body>')
  )
  dialog.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(html))
  dialog.webContents.once('did-finish-load', () => {
    if (dialog.isDestroyed()) return
    if (!dimmer.isDestroyed()) dimmer.showInactive()
    dialog.show()
    dialog.moveTop()
    dialog.focus()
    for (const win of blocked) {
      if (!win.isDestroyed()) win.setEnabled(false)
    }
    dialog.webContents
      .executeJavaScript(
        `new Promise((resolve) => {
          const done = () => resolve(true)
          document.getElementById('ok').addEventListener('click', done)
          window.addEventListener('keydown', (event) => {
            if (event.key === 'Enter' || event.key === 'Escape') done()
          })
        })`
      )
      .then(() => finish())
      .catch(() => {})
  })
  return dialog
}

export function readAtriumState() {
  try {
    const data = JSON.parse(readFileSync(statePath, 'utf8'))
    const layer = data.layer === 'top' || data.layer === 'bottom' || data.layer === 'normal' ? data.layer : ''
    return {
      board: Number(data.board) === 2 ? 2 : 0,
      lock: data.lock === true,
      layer,
      ask: Number(data.ask) || 0,
      date: typeof data.date === 'string' ? data.date : '',
      badgeScale: Number.isFinite(Number(data.badgeScale)) ? Number(data.badgeScale) : readBadgeScale(),
      dateSize: Number(data.dateSize) || 0,
      tableSize: Number(data.tableSize) || 0
    }
  } catch {
    return { board: 0, lock: false, badgeScale: readBadgeScale() }
  }
}
export function readAtriumTable() {
  try {
    const data = JSON.parse(readFileSync(tablePath, 'utf8'))
    return {
      lessons: Array.isArray(data?.lessons) ? data.lessons : [],
      changes: Array.isArray(data?.changes) ? data.changes : [],
      periods: Array.isArray(data?.periods) ? data.periods : [],
      marks: Array.isArray(data?.marks) ? data.marks : []
    }
  } catch {
    return { lessons: [], changes: [], periods: [], marks: [] }
  }
}

export function readAtriumTodos() {
  try {
    const data = JSON.parse(readFileSync(todosPath, 'utf8'))
    return Array.isArray(data?.items) ? data.items : []
  } catch {
    return []
  }
}

function readPlace() {
  try {
    const data = JSON.parse(readFileSync(placePath, 'utf8'))
    const view = data.view === 'list' ? 'list' : data.view === 'week' ? 'week' : data.view === 'keep' ? 'keep' : 'month'
    const left = Number(data.left)
    const top = Number(data.top)
    const right = Number(data.right)
    const bottom = Number(data.bottom)
    const sized =
      [left, top, right, bottom].every(Number.isFinite) && right - left >= 240 && bottom - top >= 180
    return {
      show: data.show !== false,
      view,
      left: sized ? left : null,
      top: sized ? top : null,
      right: sized ? right : null,
      bottom: sized ? bottom : null
    }
  } catch {
    return null
  }
}

export function readAtriumLaunch() {
  const place = readPlace()
  if (place?.view === 'keep') place.view = 'month'
  return place
}

export function atriumHosted() {
  return readPlace() != null
}

export function atriumWindowStaysVisible() {
  return atriumHosted()
}

export function atriumPeeking() {
  return peek != null
}

export function atriumInitialBounds() {
  const place = readPlace()
  if (!place || place.left == null) return null
  return fitVisibleDisplay(dipBounds(place))
}

function rememberHwnd(win) {
  try {
    const buf = win.getNativeWindowHandle()
    const hwnd = buf.length >= 8 ? buf.readBigUInt64LE(0) : BigInt(buf.readUInt32LE(0))
    const text = hwnd.toString()
    if (text === lastHwnd) return
    lastHwnd = text
    writeFileSync(hwndPath, text)
  } catch {
    // The desktop can still show the page without the z-order handle.
  }
}

let userSizing = false
let holdPlaceUntil = 0
let restHome = null
let lastHwnd = ''
let dockPaint = ''
let dockShown = 1
let taskbarCleared = false
let minSized = false
let sideExtra = null
let forceWin = null
let dockHome = null
let dockEdge = ''
let dockOpen = true
let dockArmed = false
let dockHideAt = 0
let dockStyle = ''
let peek = null
let motion = null

// Half of the island's collapsed bar. The bar is 340 by 38 at scale 0.89, and
// the tucked edge keeps 10px. Both stages are half of that. Windows will not
// make the window itself that short, so the extra pixels are clipped away.
const PEEK_THICK = Math.round(38 * 0.89 / 2)
const PEEK_LONG = Math.round(340 * 0.89 / 2)
const PEEK_RIM = 5
const PEEK_RADIUS = 6

function readDockDuration() {
  try {
    const data = JSON.parse(readFileSync(join(atriumRoot(), 'settings.json'), 'utf8'))
    const ms = Number(data.calendarDockMs)
    if (Number.isFinite(ms)) return Math.min(1200, Math.max(80, ms))
  } catch {
    // A missing file keeps the middle speed.
  }
  return 380
}

export function clampAtriumBounds(bounds) {
  if (!bounds || !Number.isFinite(bounds.x) || !Number.isFinite(bounds.width)) return bounds
  let display = screen.getDisplayMatching(bounds)
  if (!intersectsAny(bounds, [display.bounds])) {
    display = screen.getDisplayNearestPoint({
      x: Math.round(bounds.x + bounds.width / 2),
      y: Math.round(bounds.y + bounds.height / 2)
    })
  }
  return containInArea(bounds, display.bounds)
}

function tellPeek(win, edge) {
  if (!win || win.isDestroyed() || win.webContents.isDestroyed()) return
  win.webContents.send('atrium:peek', edge
    ? { edge, thick: PEEK_THICK, long: PEEK_LONG, radius: PEEK_RADIUS }
    : { edge: '' })
}

function windowHwnd(win) {
  const handle = win.getNativeWindowHandle()
  return handle.length >= 8 ? handle.readBigUInt64LE(0) : BigInt(handle.readUInt32LE(0))
}

let peekShapeKey = ''

function clearPeekShape(win) {
  peekShapeKey = ''
  if (!win || win.isDestroyed()) return
  try {
    ensureClipApi().setRegion(windowHwnd(win), null, 1)
  } catch {
    // The expanded window stays rectangular.
  }
}

function shapePeek(win) {
  if (!peek || !win || win.isDestroyed()) return
  const actual = win.getBounds()
  const across = peek.edge === 'top' || peek.edge === 'bottom'
  const width = Math.max(1, Math.min(across ? PEEK_LONG : PEEK_THICK, actual.width))
  const height = Math.max(1, Math.min(across ? PEEK_THICK : PEEK_LONG, actual.height))
  let ox = 0
  let oy = 0
  if (peek.edge === 'top') oy = Math.max(0, actual.height - height)
  if (peek.edge === 'left') ox = Math.max(0, actual.width - width)
  const key = `${peek.edge}:${ox}:${oy}:${width}:${height}`
  if (key === peekShapeKey) return
  try {
    const api = ensureClipApi()
    const hwnd = windowHwnd(win)
    const radius = Math.min(PEEK_RADIUS, Math.floor(Math.min(width, height) / 2))
    const region = api.round(0, 0, width + 1, height + 1, radius * 2, radius * 2)
    if (radius > 0) {
      const cap = peek.edge === 'top'
        ? api.create(0, 0, width, radius)
        : peek.edge === 'left'
          ? api.create(0, 0, radius, height)
          : peek.edge === 'right'
            ? api.create(width - radius, 0, width, height)
            : api.create(0, height - radius, width, height)
      api.combine(region, region, cap, 2)
      api.drop(cap)
    }
    if (ox || oy) api.offset(region, ox, oy)
    if (!api.setRegion(hwnd, region, 1)) {
      api.drop(region)
      return
    }
    peekShapeKey = key
  } catch {
    peekShapeKey = ''
  }
}

function animateBounds(win, to, duration, done, kind) {
  motion = { from: win.getBounds(), to, start: Date.now(), duration: Math.max(80, duration), done, kind }
  win.setMinimumSize(1, 1)
}

function tickMotion(win) {
  if (!motion || !win || win.isDestroyed()) return false
  const t = Math.min(1, (Date.now() - motion.start) / motion.duration)
  win.setBounds(lerpBounds(motion.from, motion.to, t))
  if (t < 1) return true
  win.setBounds(motion.to)
  const done = motion.done
  motion = null
  done?.()
  return true
}

let peekHot = false
let rimTimer = 0
let peekQuietUntil = 0

function peekArea() {
  return screen.getDisplayMatching(peek.home).bounds
}

function barBounds() {
  return peekTab(peek.home, peekArea(), peek.edge, PEEK_THICK, PEEK_LONG)
}

function rimBounds() {
  return peekRim(peek.home, peekArea(), peek.edge, PEEK_THICK, PEEK_LONG, PEEK_RIM)
}

function peekHostBounds(win, visual) {
  const actual = win && !win.isDestroyed() ? win.getBounds() : visual
  const across = peek.edge === 'top' || peek.edge === 'bottom'
  let width = actual.width
  let height = actual.height
  if (across) {
    if (Math.abs(width - visual.width) > 8) width = visual.width
    if (height > visual.height + 30) height = visual.height
  } else {
    if (Math.abs(height - visual.height) > 8) height = visual.height
    if (width > visual.width + 30) width = visual.width
  }
  let x = visual.x
  let y = visual.y
  if (peek.edge === 'top') y = visual.y + visual.height - height
  if (peek.edge === 'left') x = visual.x + visual.width - width
  return {
    x: Math.round(x),
    y: Math.round(y),
    width: Math.round(width),
    height: Math.round(height)
  }
}

function cursorOnPill() {
  if (!peek) return false
  const cursor = screen.getCursorScreenPoint()
  const visual = peek.placed === 'bar' ? barBounds() : rimBounds()
  const pad = 8
  let x = visual.x
  let y = visual.y
  let width = visual.width
  let height = visual.height
  if (peek.placed !== 'bar') {
    if (peek.edge === 'top') {
      y = visual.y + visual.height - PEEK_RIM
      height = PEEK_RIM
    } else if (peek.edge === 'left') {
      x = visual.x + visual.width - PEEK_RIM
      width = PEEK_RIM
    } else if (peek.edge === 'right') {
      width = PEEK_RIM
    } else {
      height = PEEK_RIM
    }
  }
  return cursor.x >= x - pad && cursor.x < x + width + pad && cursor.y >= y - pad && cursor.y < y + height + pad
}

function scheduleTuck(win) {
  clearTimeout(rimTimer)
  rimTimer = setTimeout(() => {
    rimTimer = 0
    if (!peek || !win || win.isDestroyed()) return
    if (motion) {
      scheduleTuck(win)
      return
    }
    if (peekHot || cursorOnPill()) return
    if (peek.placed === 'rim') return
    peek.placed = 'rim'
    animateBounds(win, peekHostBounds(win, rimBounds()), Math.min(readDockDuration(), 320), () => alignPeek(win), 'tuck')
  }, 180)
}

function revealPeek(win) {
  if (!peek || !win || win.isDestroyed() || !peekHot) return
  if (motion?.kind === 'hover' || (peek.placed === 'bar' && !motion)) return
  if (motion || Date.now() < peekQuietUntil) {
    clearTimeout(rimTimer)
    rimTimer = setTimeout(() => {
      rimTimer = 0
      if (peekHot) revealPeek(win)
    }, 40)
    return
  }
  peek.placed = 'bar'
  animateBounds(win, peekHostBounds(win, barBounds()), Math.min(readDockDuration(), 320), () => alignPeek(win), 'hover')
}

let peekResizeGuard = null
let parkingPeek = false

function holdPeekSize(win) {
  if (!win || win.isDestroyed()) return
  if (win.isResizable()) win.setResizable(false)
  if (peekResizeGuard === win) return
  peekResizeGuard = win
  win.on('will-resize', (event) => {
    if (peek) event.preventDefault()
  })
  win.on('resize', () => {
    if (!peek || motion || parkingPeek) return
    const visual = peek.placed === 'bar' ? barBounds() : rimBounds()
    const parked = peekHostBounds(win, visual)
    const actual = win.getBounds()
    if (
      Math.abs(parked.x - actual.x) > 2 ||
      Math.abs(parked.y - actual.y) > 2 ||
      Math.abs(parked.width - actual.width) > 2 ||
      Math.abs(parked.height - actual.height) > 2
    ) {
      parkingPeek = true
      peekShapeKey = ''
      win.setBounds(parked)
      parkingPeek = false
    }
    shapePeek(win)
  })
}

function alignPeek(win) {
  if (!peek || !win || win.isDestroyed()) return
  const visual = peek.placed === 'bar' ? barBounds() : rimBounds()
  const next = peekHostBounds(win, visual)
  const actual = win.getBounds()
  peekQuietUntil = Date.now() + 280
  if (
    Math.abs(next.x - actual.x) > 2 ||
    Math.abs(next.y - actual.y) > 2 ||
    Math.abs(next.width - actual.width) > 2 ||
    Math.abs(next.height - actual.height) > 2
  ) {
    peekShapeKey = ''
    win.setBounds(next)
  }
  const parked = peekHostBounds(win, visual)
  const settled = win.getBounds()
  if (Math.abs(parked.x - settled.x) > 2 || Math.abs(parked.y - settled.y) > 2) {
    peekShapeKey = ''
    win.setBounds(parked)
  }
  const finalBounds = win.getBounds()
  const across = peek.edge === 'top' || peek.edge === 'bottom'
  const lockW = across ? PEEK_LONG : finalBounds.width
  const lockH = across ? finalBounds.height : PEEK_LONG
  win.setMinimumSize(lockW, lockH)
  win.setMaximumSize(lockW, lockH)
  holdPeekSize(win)
  shapePeek(win)
  setTimeout(() => {
    if (!peek || motion || !win || win.isDestroyed()) return
    peekShapeKey = ''
    shapePeek(win)
  }, 60)
  if (peekHot) revealPeek(win)
  else if (peek.placed !== 'rim') scheduleTuck(win)
}

function collapseCalendar(win) {
  if (!win || win.isDestroyed() || peek || !edgeHideEnabled()) return
  if (motion?.kind === 'collapse') return
  motion = null
  peekHot = false
  clearTimeout(rimTimer)
  const home = clampAtriumBounds(win.getBounds())
  const area = screen.getDisplayMatching(home).bounds
  const edge = nearestOpenEdge(home, area)
  peek = { edge, home, placed: 'rim' }
  dockShown = 1
  holdPeekSize(win)
  win.setOpacity(1)
  win.setIgnoreMouseEvents(false)
  win.webContents.executeJavaScript("document.documentElement.style.transform='none'").catch(() => {})
  tellPeek(win, edge)
  animateBounds(win, peekHostBounds(win, rimBounds()), readDockDuration(), () => alignPeek(win), 'collapse')
}

function expandCalendar(win) {
  if (!win || win.isDestroyed() || !peek) return
  motion = null
  peekHot = false
  clearTimeout(rimTimer)
  clearPeekShape(win)
  win.setMaximumSize(16384, 16384)
  win.setMinimumSize(1, 1)
  const home = clampAtriumBounds(peek.home)
  win.setOpacity(1)
  win.setIgnoreMouseEvents(false)
  animateBounds(win, home, readDockDuration(), () => {
    peek = null
    minSized = true
    if (!win.isDestroyed()) {
      win.setMinimumSize(240, 180)
      tellPeek(win, '')
    }
  }, 'expand')
}

function hoverPeek(win, on) {
  if (!win || win.isDestroyed() || !peek) return
  peekHot = on
  if (on) {
    clearTimeout(rimTimer)
    rimTimer = 0
    revealPeek(win)
    return
  }
  scheduleTuck(win)
}

const DOCK_SLACK = 8
const DOCK_STRIP = 39
const DOCK_LEAVE_MS = 180

export function setAtriumSizing(active) {
  userSizing = active === true
  if (!active) holdPlaceUntil = Date.now() + 1200
}

function displayAreas() {
  return screen.getAllDisplays().map((display) => display.bounds)
}

function intersectsDisplay(bounds) {
  return intersectsAny(bounds, displayAreas())
}

function nearestArea(bounds) {
  return screen.getDisplayNearestPoint({
    x: Math.round(bounds.x + bounds.width / 2),
    y: Math.round(bounds.y + bounds.height / 2)
  }).workArea
}

function fitVisibleDisplay(bounds) {
  if (intersectsDisplay(bounds)) return bounds
  return fitOntoArea(bounds, nearestArea(bounds))
}

function clampDisplay(bounds) {
  const area = screen.getDisplayNearestPoint({
    x: bounds.x + bounds.width / 2,
    y: bounds.y + bounds.height / 2
  }).bounds
  const width = Math.min(bounds.width, area.width)
  const height = Math.min(bounds.height, area.height)
  let x = bounds.x
  let y = bounds.y
  if (x < area.x) x = area.x
  if (y < area.y) y = area.y
  if (x + width > area.x + area.width) x = area.x + area.width - width
  if (y + height > area.y + area.height) y = area.y + area.height - height
  return { x: Math.round(x), y: Math.round(y), width: Math.round(width), height: Math.round(height) }
}

export function noteAtriumBounds(bounds) {
  holdPlaceUntil = Date.now() + 800
  if (bounds && Number.isFinite(bounds.x)) restHome = { ...bounds }
}

export function publishAtriumFrame(win) {
  if (!win || win.isDestroyed()) return
  const bounds = clampDisplay(win.getBounds())
  if (!nearBounds(win.getBounds(), bounds)) win.setBounds(bounds)
  restHome = bounds
  const origin = screen.dipToScreenPoint({ x: bounds.x, y: bounds.y })
  const far = screen.dipToScreenPoint({ x: bounds.x + bounds.width, y: bounds.y + bounds.height })
  writeFileSync(
    cmdPath,
    JSON.stringify({
      cmd: 'place',
      left: Math.round(origin.x),
      top: Math.round(origin.y),
      right: Math.round(far.x),
      bottom: Math.round(far.y)
    })
  )
}

function placeWindow(win, place) {
  if (!win || win.isDestroyed() || place.left == null) return
  const bounds = dipBounds(place)
  const current = win.getBounds()
  const dx = Math.abs(current.x - bounds.x)
  const dy = Math.abs(current.y - bounds.y)
  const dw = Math.abs(current.width - bounds.width)
  const dh = Math.abs(current.height - bounds.height)
  win.setMinimumSize(1, 1)
  if (dx > 2 || dy > 2 || dw > 2 || dh > 2) win.setBounds(bounds)
  rememberHwnd(win)
  if (place.show) {
    if (!win.isVisible()) win.showInactive()
    win.setSkipTaskbar(true)
  } else if (win.isVisible()) {
    win.hide()
  }
}

function dipBounds(place) {
  const origin = screen.screenToDipPoint({ x: place.left, y: place.top })
  const far = screen.screenToDipPoint({ x: place.right, y: place.bottom })
  const bounds = {
    x: Math.round(origin.x),
    y: Math.round(origin.y),
    width: Math.max(1, Math.round(far.x - origin.x)),
    height: Math.max(1, Math.round(far.y - origin.y))
  }
  return bounds
}

function nearBounds(a, b) {
  return (
    !!a &&
    !!b &&
    Math.abs(a.x - b.x) <= 2 &&
    Math.abs(a.y - b.y) <= 2 &&
    Math.abs(a.width - b.width) <= 2 &&
    Math.abs(a.height - b.height) <= 2
  )
}

function containsPoint(bounds, point, pad = 0) {
  return (
    point.x >= bounds.x - pad &&
    point.y >= bounds.y - pad &&
    point.x < bounds.x + bounds.width + pad &&
    point.y < bounds.y + bounds.height + pad
  )
}

function currentHome(place, live) {
  const saved = place && place.left != null ? dipBounds(place) : null
  const holding = userSizing || Date.now() < holdPlaceUntil
  if (restHome && saved && nearBounds(saved, restHome) && !holding) restHome = null
  return calendarHome({
    live,
    saved,
    areas: displayAreas(),
    rest: restHome,
    holding: Boolean(restHome) && holding
  })
}

function readBadgeScale() {
  try {
    const data = JSON.parse(readFileSync(join(atriumRoot(), 'settings.json'), 'utf8'))
    const scale = Number(data.timetableBadgeScale)
    if (Number.isFinite(scale)) return Math.min(1.6, Math.max(0.4, scale))
  } catch {
    // Missing settings keep the timetable badge at seven tenths.
  }
  return 0.7
}

function edgeHideEnabled() {
  try {
    const data = JSON.parse(readFileSync(join(atriumRoot(), 'settings.json'), 'utf8'))
    return data.calendarEdgeHide !== false
  } catch {
    return true
  }
}

// Any monitor edge can hide the page. The window stays at its full size and
// turns fully transparent, so nothing is left on the bezel and the next monitor stays clear.
function dockSpec(home) {
  if (!edgeHideEnabled()) return { edge: '' }
  const area = screen.getDisplayMatching(home).bounds
  const gapTop = home.y - area.y
  const gapLeft = home.x - area.x
  const gapRight = area.x + area.width - (home.x + home.width)
  const gapBottom = area.y + area.height - (home.y + home.height)
  const flush = (gap) => gap >= -DOCK_SLACK && gap <= DOCK_SLACK
  let edge = ''
  let gap = DOCK_SLACK + 1
  const take = (name, value) => {
    if (!flush(value) || Math.abs(value) >= gap) return
    edge = name
    gap = Math.abs(value)
  }
  take('top', gapTop)
  take('left', gapLeft)
  take('right', gapRight)
  take('bottom', gapBottom)
  return { edge }
}

function pointerOpens(home, edge, open) {
  if (!edge) return true
  const cursor = screen.getCursorScreenPoint()
  if (open && containsPoint(home, cursor, 0)) return true
  const area = screen.getDisplayMatching(home).bounds
  const alongX = cursor.x >= home.x - 1 && cursor.x <= home.x + home.width + 1
  const alongY = cursor.y >= home.y - 1 && cursor.y <= home.y + home.height + 1
  if (edge === 'top') return alongX && cursor.y >= area.y && cursor.y <= area.y + 1
  if (edge === 'bottom') return alongX && cursor.y <= area.y + area.height && cursor.y >= area.y + area.height - 1
  if (edge === 'left') return alongY && cursor.x >= area.x && cursor.x <= area.x + 1
  if (edge === 'right') return alongY && cursor.x <= area.x + area.width && cursor.x >= area.x + area.width - 1
  return false
}

const clearDockJs =
  "(() => { const h = document.documentElement; delete h.dataset.atriumDock; ['--atrium-home-w','--atrium-home-h','--atrium-shift-x','--atrium-shift-y'].forEach((k) => h.style.removeProperty(k)); })()"

function dockStyleJs(visual) {
  return `(() => { const h = document.documentElement; h.dataset.atriumDock = ${JSON.stringify(visual.edge)}; h.style.setProperty('--atrium-home-w', ${JSON.stringify(`${visual.home.width}px`)}); h.style.setProperty('--atrium-home-h', ${JSON.stringify(`${visual.home.height}px`)}); h.style.setProperty('--atrium-shift-x', ${JSON.stringify(`${visual.shiftX}px`)}); h.style.setProperty('--atrium-shift-y', ${JSON.stringify(`${visual.shiftY}px`)}); })()`
}

function writeDockStyle(win, visual) {
  const open = !visual.edge || visual.open
  const next = open ? '' : `${visual.edge}|${visual.home.width}|${visual.home.height}|${visual.shiftX}|${visual.shiftY}`
  if (next === dockStyle) return false
  dockStyle = next
  const bounds = open ? visual.home : visual.bounds
  if (!open) {
    win.webContents
      .executeJavaScript(dockStyleJs(visual))
      .then(() => {
        if (!win.isDestroyed() && dockStyle === next) {
          win.setMinimumSize(1, 1)
          win.setBounds(bounds)
        }
      })
      .catch(() => {
        if (!win.isDestroyed()) win.setBounds(bounds)
      })
    return true
  }
  win.setMinimumSize(1, 1)
  win.setBounds(bounds)
  win.webContents.executeJavaScript(clearDockJs).catch(() => {})
  return true
}

function placeVisual(win, visual) {
  if (peek || motion) {
    rememberHwnd(win)
    return
  }
  if (!minSized) {
    minSized = true
    const size = win.getBounds()
    if (size.width >= 240 && size.height >= 180) win.setMinimumSize(240, 180)
  }
  if (userSizing || Date.now() < holdPlaceUntil) {
    if (dockPaint !== 'steady') {
      dockPaint = 'steady'
      dockShown = 1
      win.setOpacity(1)
      win.setIgnoreMouseEvents(false)
      win.webContents
        .executeJavaScript(
          "document.documentElement.style.transition='none';document.documentElement.style.transform='none'"
        )
        .catch(() => {})
    }
    rememberHwnd(win)
    return
  }
  const target = visual.edge && !visual.open ? 0 : 1
  const step = 32 / 240
  if (dockShown < target) dockShown = Math.min(target, dockShown + step)
  else if (dockShown > target) dockShown = Math.max(target, dockShown - step)
  const hidden = dockShown < 0.04
  const paint = `${visual.edge}|${dockShown.toFixed(2)}`
  if (paint !== dockPaint) {
    dockPaint = paint
    win.setOpacity(dockShown)
    win.setIgnoreMouseEvents(hidden)
    const distance = visual.edge === 'left' || visual.edge === 'right' ? visual.home.width : visual.home.height
    const shift = Math.round((1 - dockShown) * distance)
    const tx = visual.edge === 'left' ? -shift : visual.edge === 'right' ? shift : 0
    const ty = visual.edge === 'top' ? -shift : visual.edge === 'bottom' ? shift : 0
    win.webContents
      .executeJavaScript(
        `document.documentElement.style.transform=${JSON.stringify(`translate(${tx}px, ${ty}px)`)};document.documentElement.style.transition='transform 40ms linear'`
      )
      .catch(() => {})
    if (!hidden && !win.isVisible()) win.showInactive()
  }
  if (!win.isVisible() && !hidden) win.showInactive()
  rememberHwnd(win)
  if (!taskbarCleared) {
    taskbarCleared = true
    win.setSkipTaskbar(true)
  }
}

function visualFor(place, live) {
  const home = currentHome(place, live)
  if (!home) return null
  dockHome = home
  dockEdge = ''
  dockOpen = true
  return { home, open: true, edge: '' }
}

export function expandAtriumForDrag(win) {
  if (!win || win.isDestroyed() || !dockHome || peek || motion) return
  dockOpen = true
  dockEdge = ''
  dockArmed = false
  dockHideAt = 0
  dockPaint = 'show'
  win.setOpacity(1)
  win.setIgnoreMouseEvents(false)
  win.setMinimumSize(240, 180)
  if (!nearBounds(win.getBounds(), dockHome)) win.setBounds(dockHome)
  restHome = { ...dockHome }
}

function surfacePreload() {
  const fromCwd = join(process.cwd(), 'out', 'preload', 'index.js')
  if (existsSync(fromCwd)) return fromCwd
  let dir = dirname(fileURLToPath(import.meta.url))
  for (let i = 0; i < 5; i += 1) {
    const candidate = join(dir, 'preload', 'index.js')
    if (existsSync(candidate)) return candidate
    const parent = dirname(dir)
    if (parent === dir) break
    dir = parent
  }
  return fromCwd
}

function surfaceFile(name) {
  return join(process.cwd(), 'side', name)
}

function placeBeside(host, width, height) {
  if (!host || host.isDestroyed()) {
    const area = screen.getPrimaryDisplay().workArea
    return {
      x: Math.round(area.x + (area.width - width) / 2),
      y: Math.round(area.y + (area.height - height) / 2),
      width,
      height
    }
  }
  const bounds = host.getBounds()
  const area = screen.getDisplayMatching(bounds).bounds
  const roomLeft = bounds.x - area.x
  const roomRight = area.x + area.width - (bounds.x + bounds.width)
  let x = roomRight >= roomLeft ? bounds.x + bounds.width + 12 : bounds.x - width - 12
  x = Math.max(area.x, Math.min(x, area.x + area.width - width))
  const y = Math.max(area.y, Math.min(bounds.y, area.y + area.height - height))
  return { x: Math.round(x), y: Math.round(y), width, height }
}

let dayWin = null
let daySizeTimer = null
let dayHeld = false

function dayBoundsPath() {
  return join(app.getPath('userData'), 'day-window-bounds.json')
}

function readDaySize() {
  try {
    const data = JSON.parse(readFileSync(dayBoundsPath(), 'utf8'))
    const width = Math.round(Number(data.width))
    const height = Math.round(Number(data.height))
    if (width >= 280 && width <= 1600 && height >= 360 && height <= 1400) return { width, height }
  } catch {
    // 第一次打开使用默认大小。
  }
  return { width: 420, height: 760 }
}

function rememberDaySize() {
  if (!dayWin || dayWin.isDestroyed()) return
  const [width, height] = dayWin.getSize()
  if (width < 280 || height < 360) return
  try {
    writeFileSync(dayBoundsPath(), JSON.stringify({ width, height }))
  } catch {
    // 尺寸写失败时下次仍用上一次成功保存的值。
  }
}

function queueDaySize() {
  clearTimeout(daySizeTimer)
  daySizeTimer = setTimeout(rememberDaySize, 180)
}

function surfacePage(name) {
  const dev = process.env.ELECTRON_RENDERER_URL
  if (dev) return { kind: 'url', target: `${dev.replace(/\/$/, '')}/${name}` }
  return { kind: 'file', target: join(app.getAppPath(), 'out', 'renderer', name) }
}

function loadSurface(win, name, query) {
  const page = surfacePage(name)
  if (page.kind === 'url') {
    const url = new URL(page.target)
    for (const [key, value] of Object.entries(query || {})) url.searchParams.set(key, String(value ?? ''))
    return win.loadURL(url.href)
  }
  return win.loadFile(page.target, { query })
}

function dayPayload(payload) {
  const dateKey = typeof payload?.dateKey === 'string' ? payload.dateKey : ''
  return { dateKey }
}

function deliverDay(payload) {
  if (!dayWin || dayWin.isDestroyed()) return
  const send = () => {
    if (!dayWin || dayWin.isDestroyed() || dayWin.webContents.isDestroyed()) return
    dayWin.webContents.send('atrium:day-show', payload)
  }
  if (dayWin.webContents.isLoadingMainFrame()) dayWin.webContents.once('did-finish-load', send)
  else send()
}

function openDayWindow(getWindow, payload) {
  const host = getWindow()
  const body = dayPayload(payload)
  if (!dayWin || dayWin.isDestroyed()) {
    const size = readDaySize()
    const box = placeBeside(host, size.width, size.height)
    dayWin = new BrowserWindow({
      ...box,
      frame: false,
      thickFrame: true,
      resizable: true,
      minimizable: false,
      maximizable: false,
      skipTaskbar: true,
      roundedCorners: true,
      minWidth: 280,
      minHeight: 360,
      title: '',
      backgroundColor: '#F6F6F8',
      parent: host && !host.isDestroyed() ? host : undefined,
      webPreferences: {
        preload: surfacePreload(),
        contextIsolation: true,
        sandbox: true,
        nodeIntegration: false
      }
    })
    const created = dayWin
    addCompanionWindow(created)
    created.setMenuBarVisibility(false)
    created.on('resize', queueDaySize)
    created.on('close', rememberDaySize)
    created.on('closed', () => {
      removeCompanionWindow(created)
      if (dayWin === created) {
        dayWin = null
        dayHeld = false
      }
    })
    dayWin.on('page-title-updated', (event) => {
      event.preventDefault()
      if (dayWin && !dayWin.isDestroyed()) dayWin.setTitle('')
    })
    loadSurface(dayWin, 'month.html', { surface: 'day', date: body.dateKey })
    deliverDay(body)
    return true
  }
  deliverDay(body)
  if (!dayWin.isVisible()) dayWin.show()
  return true
}

function syncOpenDayWindow(payload) {
  if (!dayWin || dayWin.isDestroyed()) return false
  deliverDay(dayPayload(payload))
  if (!dayWin.isVisible()) {
    try {
      dayWin.showInactive()
    } catch {
      dayWin.show()
    }
  }
  return true
}

let handoutWin = null
let handoutFullWait = null

function openHandoutWindow(getWindow, payload) {
  const host = getWindow()
  const box = placeBeside(host, 860, 640)
  if (!handoutWin || handoutWin.isDestroyed()) {
    handoutWin = new BrowserWindow({
      ...box,
      frame: false,
      thickFrame: true,
      resizable: true,
      minimizable: false,
      maximizable: false,
      title: '课堂讲义',
      backgroundColor: '#FFFFFF',
      roundedCorners: true,
      webPreferences: {
        preload: surfacePreload(),
        contextIsolation: true,
        sandbox: true,
        nodeIntegration: false
      }
    })
    handoutWin.on('closed', () => {
      handoutWin = null
    })
    handoutWin.webContents.once('did-finish-load', () => {
      if (handoutWin && !handoutWin.isDestroyed()) handoutWin.webContents.send('atrium:handout-data', payload)
    })
    handoutWin.loadFile(surfaceFile('handout.html'))
    return true
  }
  if (!handoutWin.isFullScreen()) handoutWin.setBounds(box)
  handoutWin.webContents.send('atrium:handout-data', payload)
  if (!handoutWin.isVisible()) handoutWin.show()
  handoutWin.focus()
  return true
}

function toggleHandoutFull(event) {
  const win = BrowserWindow.fromWebContents(event.sender)
  if (!win || win.isDestroyed()) return false
  const next = !win.isFullScreen()
  return new Promise((resolve) => {
    if (handoutFullWait) clearTimeout(handoutFullWait)
    const finish = () => {
      if (handoutFullWait) clearTimeout(handoutFullWait)
      handoutFullWait = null
      resolve(!win.isDestroyed() && win.isFullScreen())
    }
    win.once(next ? 'enter-full-screen' : 'leave-full-screen', finish)
    handoutFullWait = setTimeout(finish, 800)
    win.setFullScreen(next)
  })
}

function saveHandoutPiece(key, html) {
  if (typeof key !== 'string' || !key || typeof html !== 'string' || html.length > 8_000_000) return false
  let all = {}
  try {
    all = JSON.parse(readFileSync(handoutsPath, 'utf8'))
  } catch {
    all = {}
  }
  if (!all || typeof all !== 'object' || Array.isArray(all)) all = {}
  all[key] = html
  writeFileSync(handoutsPath, JSON.stringify(all))
  scheduleCloudSync()
  return true
}

const TEACHER_PAGES = {
  '/teacher/exams': '试卷管理',
  '/teacher/homework': '作业管理',
  '/teacher/ai-grading': 'AI批改',
  '/teacher/recitations': '背书情况',
  '/advisor/students': '学生档案',
  '/advisor/scores': '成绩分析',
  '/advisor/leaves': '请假管理',
  '/advisor/evaluations': '期末评价',
  '/advisor/communications': '家校沟通',
  '/advisor/seats': '座位表'
}
const teacherWindows = new Map()

function teacherServerFile() {
  const starts = [dirname(fileURLToPath(import.meta.url)), process.cwd()]
  for (const start of starts) {
    let dir = start
    for (let i = 0; i < 8; i += 1) {
      const candidate = join(dir, 'third_party', 'teacher_works', 'backend', 'server.js')
      if (existsSync(candidate)) return candidate
      const parent = dirname(dir)
      if (parent === dir) break
      dir = parent
    }
  }
  return ''
}

async function ensureTeacherServer() {
  const health = 'http://127.0.0.1:3090/api/v1/health'
  try {
    if ((await fetch(health)).ok) return true
  } catch {
    // The desk is started on the first page that needs it.
  }
  const server = teacherServerFile()
  if (!server) return false
  const child = spawn('node', ['server.js'], {
    cwd: dirname(server),
    env: { ...process.env, PORT: '3090' },
    detached: true,
    stdio: 'ignore',
    windowsHide: true
  })
  child.unref()
  for (let i = 0; i < 40; i += 1) {
    await new Promise((resolve) => setTimeout(resolve, 250))
    try {
      if ((await fetch(health)).ok) return true
    } catch {
      // Keep waiting while node starts.
    }
  }
  return false
}

async function openTeacherPage(host, pagePath) {
  const title = TEACHER_PAGES[pagePath]
  if (!title) return false
  const existing = teacherWindows.get(pagePath)
  if (existing && !existing.isDestroyed()) {
    if (existing.isMinimized()) existing.restore()
    existing.show()
    existing.focus()
    return true
  }
  if (!(await ensureTeacherServer())) return false
  const area = host && !host.isDestroyed()
    ? screen.getDisplayMatching(host.getBounds()).workArea
    : screen.getPrimaryDisplay().workArea
  const width = Math.min(1100, area.width)
  const height = Math.min(760, area.height)
  const win = new BrowserWindow({
    x: Math.round(area.x + (area.width - width) / 2),
    y: Math.round(area.y + (area.height - height) / 2),
    width,
    height,
    minWidth: Math.min(800, area.width),
    minHeight: Math.min(560, area.height),
    title,
    autoHideMenuBar: true,
    backgroundColor: '#f5f5f7',
    webPreferences: { sandbox: true }
  })
  teacherWindows.set(pagePath, win)
  win.on('closed', () => {
    if (teacherWindows.get(pagePath) === win) teacherWindows.delete(pagePath)
  })
  await win.loadURL(`http://127.0.0.1:3090${pagePath}?solo=1`)
  return true
}

export function startAtriumHost({ getWindow, getMode, switchView, suspendDock, applyUiSetting, getSettings }) {
  ipcMain.on('atrium:sizing', (_event, active) => {
    if (peek && active === true) return
    if (active === true) expandAtriumForDrag(getWindow())
    setAtriumSizing(active === true)
    if (active === true) {
      const win = getWindow()
      if (win && !win.isDestroyed()) restHome = win.getBounds()
    }
  })
  ipcMain.handle('atrium:hosted', () => atriumHosted())
  ipcMain.handle('atrium:todos', () => readAtriumTodos())
  ipcMain.handle('atrium:state', () => readAtriumState())
  ipcMain.handle('atrium:command', (_event, payload) => {
    const cmd = payload && typeof payload.cmd === 'string' ? payload.cmd : ''
    const win = getWindow()
    if (cmd === 'collapse') {
      collapseCalendar(win)
      return true
    }
    if (cmd === 'expand') {
      expandCalendar(win)
      return true
    }
    if (cmd === 'peek-hover') {
      hoverPeek(win, payload?.on === true)
      return true
    }
    if (cmd === 'teacher-page') {
      return openTeacherPage(win, payload?.path)
    }
    const body = { cmd }
    if (cmd === 'board') body.board = Number(payload?.board) === 2 ? 2 : 0
    if (cmd === 'layer' && typeof payload?.layer === 'string') body.layer = payload.layer
    const numeric = {
      cell: ['wd', 'period', 'y', 'm', 'd'],
      period: ['row', 's', 'e'],
      tool: ['kind'],
      handout: ['wd', 'period'],
      place: ['left', 'top', 'right', 'bottom'],
      week: ['delta'],
      focus: ['y', 'm', 'd'],
      check: ['index']
    }
    for (const key of numeric[cmd] || []) body[key] = Number(payload?.[key])
    if (cmd === 'cell') {
      body.name = typeof payload?.name === 'string' ? payload.name : ''
      body.teacher = typeof payload?.teacher === 'string' ? payload.teacher : ''
      body.scope = payload?.scope === 'week' ? 'week' : 'fixed'
    }
    writeFileSync(cmdPath, JSON.stringify(body))
    return true
  })
  const send = (channel, payload) => {
    const win = getWindow()
    if (!win || win.isDestroyed() || win.webContents.isDestroyed()) return
    win.webContents.send(channel, payload)
  }
  ipcMain.handle('atrium:handouts', () => {
    try {
      const data = JSON.parse(readFileSync(handoutsPath, 'utf8'))
      return data && typeof data === 'object' ? data : {}
    } catch {
      return {}
    }
  })
  ipcMain.handle('atrium:handouts-save', (_event, body) => {
    if (!body || typeof body !== 'object' || Array.isArray(body)) return false
    const text = JSON.stringify(body)
    if (text.length > 8_000_000) return false
    writeFileSync(handoutsPath, text)
    scheduleCloudSync()
    return true
  })
  ipcMain.handle('atrium:day-window', (_event, payload) => openDayWindow(getWindow, payload))
  ipcMain.handle('atrium:day-sync', (_event, payload) => syncOpenDayWindow(payload))
  ipcMain.handle('atrium:open-handout', (_event, payload) => openHandoutWindow(getWindow, payload))
  ipcMain.handle('atrium:handout-full', (event) => toggleHandoutFull(event))
  ipcMain.handle('atrium:handout-piece', (_event, key, html) => saveHandoutPiece(key, html))
  const holdDayWindow = () => {
    if (!dayWin || dayWin.isDestroyed() || !dayWin.isVisible()) return false
    dayHeld = true
    dayWin.hide()
    const host = getWindow()
    if (host && !host.isDestroyed()) host.focus()
    return true
  }
  const revealDayWindow = () => {
    if (!dayHeld) return false
    dayHeld = false
    if (!dayWin || dayWin.isDestroyed()) return false
    try {
      dayWin.showInactive()
    } catch {
      dayWin.show()
    }
    return true
  }
  ipcMain.handle('atrium:day-hold', () => holdDayWindow())
  ipcMain.handle('atrium:day-reveal', () => revealDayWindow())
  ipcMain.on('atrium:day-create', (event, dateKey) => {
    const host = getWindow()
    if (host && !host.isDestroyed() && event.sender !== host.webContents)
      host.webContents.send('atrium:day-create', dateKey)
  })
  ipcMain.on('atrium:day-edit', (event, id) => {
    const host = getWindow()
    if (host && !host.isDestroyed() && event.sender !== host.webContents)
      host.webContents.send('atrium:day-edit', id)
  })
  ipcMain.handle('atrium:force-popup', (_event, text) => {
    const message = String(text || '待办提醒').slice(0, 4000)
    if (forceWin && !forceWin.isDestroyed()) forceWin.close()
    const win = getWindow()
    const area = win ? screen.getDisplayMatching(win.getBounds()).bounds : screen.getPrimaryDisplay().bounds
    const box = { width: 460, height: 260 }
    forceWin = new BrowserWindow({
      width: box.width,
      height: box.height,
      x: Math.round(area.x + (area.width - box.width) / 2),
      y: Math.round(area.y + (area.height - box.height) / 2),
      frame: false,
      alwaysOnTop: true,
      resizable: false,
      title: 'Yunzhao 待办',
      webPreferences: { sandbox: true }
    })
    const safe = message.replace(/[&<>]/g, (ch) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' })[ch])
    const html = `<!doctype html><meta charset="utf-8"><body style="margin:0;font-family:Segoe UI,Microsoft YaHei,sans-serif;background:#fff;color:#1d1d1f;display:flex;flex-direction:column;height:100vh"><div style="padding:22px 24px 8px;font-size:18px;font-weight:650">待办提醒</div><div style="padding:8px 24px;flex:1;font-size:15px;line-height:1.6;overflow:auto">${safe}</div><button onclick="window.close()" style="margin:12px 24px 22px;height:36px;border:0;border-radius:8px;background:#0a84ff;color:#fff;font-size:14px">知道了</button></body>`
    forceWin.loadURL('data:text/html;charset=utf-8,' + encodeURIComponent(html))
    return true
  })
  ipcMain.handle('atrium:sign-out', async () => {
    const { uploadCloudNow } = await import('./cloud-sync.js')
    await uploadCloudNow()
    try {
      await fetch('http://127.0.0.1:5000/api/auth/desktop', { method: 'POST' })
    } catch {
      // The table window uploads on its own when it is open.
    }
    await new Promise((resolve) => setTimeout(resolve, 3500))
    writeFileSync(cmdPath, JSON.stringify({ cmd: 'sign-out' }))
    setTimeout(() => app.quit(), 400)
    return true
  })
  ipcMain.handle('atrium:pull-pending', async () => {
    const { pullPending } = await import('./cloud-sync.js')
    return pullPending()
  })
  ipcMain.handle('atrium:pull', async () => {
    const { pullCloudNow } = await import('./cloud-sync.js')
    return pullCloudNow()
  })
  ipcMain.handle('atrium:daily-plan', () => readDailyPlan())
  ipcMain.handle('atrium:daily-plan-save', (_event, plan) => {
    const time = typeof plan?.time === 'string' && /^\d{2}:\d{2}$/.test(plan.time) ? plan.time : '08:00'
    const channels = Math.max(0, Math.min(15, Number(plan?.channels) || 0))
    const current = readDailyPlan()
    writeFileSync(dailyPlanPath, JSON.stringify({ time, channels, fired: current.fired || '' }))
    return readDailyPlan()
  })
  ipcMain.handle('atrium:table', () => readAtriumTable())
  const pushTodos = () => send('atrium:todos', readAtriumTodos())
  const pushState = () => send('atrium:state', readAtriumState())
  const pushTable = () => send('atrium:table', readAtriumTable())
  setTimeout(() => {
    pushTodos()
    pushState()
    pushTable()
  }, 600)
  try {
    watch(join(atriumRoot(), 'settings.json'), () => pushState())
  } catch {
    // The state push still reads the badge size when the timetable asks.
  }
  try {
    watch(dataDir, (_event, filename) => {
      if (filename === 'todos.json') pushTodos()
      if (filename === 'yunzhao-state.json') pushState()
      if (filename === 'yunzhao-table.json') pushTable()
      if (filename === 'settings.json') pushState()
      if (filename === 'sign-out.request') {
        try {
          unlinkSync(join(dataDir, 'sign-out.request'))
        } catch {
          // The request can already have been consumed.
        }
        void (async () => {
          const { uploadCloudNow } = await import('./cloud-sync.js')
          await uploadCloudNow().catch(() => false)
          try {
            await fetch('http://127.0.0.1:5000/api/auth/desktop', { method: 'POST' })
          } catch {
            // The table uploads on its own when that window is open.
          }
          await new Promise((resolve) => setTimeout(resolve, 3500))
          writeFileSync(cmdPath, JSON.stringify({ cmd: 'sign-out' }))
          setTimeout(() => app.quit(), 400)
        })()
      }
    })
  } catch {
    // The renderer also asks for the list when it opens.
  }
  let last = ''
  let suspended = false
  let switching = false
  setInterval(() => {
    let raw = ''
    try {
      raw = readFileSync(placePath, 'utf8')
    } catch {
      return
    }
    const win = getWindow()
    if (!win || win.isDestroyed()) return
    if (tickMotion(win)) {
      rememberHwnd(win)
      return
    }
    if (userSizing) {
      if (!peek) restHome = win.getBounds()
      return
    }
    if (!peek) {
      const contained = clampAtriumBounds(win.getBounds())
      if (!nearBounds(win.getBounds(), contained)) {
        const small = contained.width < 240 || contained.height < 180
        win.setMinimumSize(small ? 1 : 240, small ? 1 : 180)
        win.setBounds(contained)
        restHome = contained
        holdPlaceUntil = Date.now() + 400
      }
    }
    if (switching) return
    const place = readPlace()
    if (!place) return
    const live = win.getBounds()
    if (!userSizing && !peek && !motion && !intersectsDisplay(live)) {
      const fitted = fitVisibleDisplay(live)
      if (!nearBounds(live, fitted)) {
        win.setMinimumSize(240, 180)
        win.setBounds(fitted)
        restHome = fitted
        holdPlaceUntil = Date.now() + 1600
        if (!win.isVisible()) win.showInactive()
      }
    }
    const visual = visualFor(place, win.getBounds())
    if (!visual) return
    const changed = raw !== last
    last = raw
    if (!suspended) {
      try {
        suspendDock()
      } catch {
        // The window can still be moved without the edge-dock.
      }
      suspended = true
    }
    placeVisual(win, visual)
    if (!changed || !place.show) return
    const apply = async () => {
      if (place.view && place.view !== 'keep' && getMode() !== place.view) {
        switching = true
        try {
          await switchView(place.view)
        } catch {
          // Keep the window where it is if the view switch is busy.
        } finally {
          switching = false
        }
      }
      const current = getWindow()
      if (current && !current.isDestroyed() && !current.isVisible()) current.showInactive()
    }
    void apply()
  }, 32)
  try {
    watch(placePath, () => {
      last = ''
    })
  } catch {
    // The interval still applies the latest place file.
  }
  startIsland(getWindow)
  watchHostedUiSettings(applyUiSetting, getSettings, getWindow)
}

const HOSTED_UI_KEYS = {
  'appearance.titlebarIconScale': 'titlebarIconScale',
  'appearance.iconColor': 'iconColor',
  'appearance.uiScale': 'uiScale',
  'css.bgColor': 'bgColor',
  'css.textColor': 'textColor',
  'css.windowBorder': 'windowBorder',
  'css.fontSizeBase': 'fontSizeBase',
  'css.noteRemarkFontSize': 'noteRemarkFontSize',
  'window.compactFontSize': 'compactFontSize',
  'blur.enabled': 'blurEnabled',
  'blur.radius': 'blurRadius',
  'blur.saturation': 'blurSaturation',
  'blur.cornerRadius': 'cornerRadius',
  'css.popupOpacity': 'popupOpacity',
  'css.bgBlur': 'bgBlur',
  'css.windowOpacity': 'windowOpacity',
  'notes.tagColorEnabled': 'tagColorEnabled',
  'interaction.doubleClickQuickEdit': 'doubleClickQuickEdit',
  'interaction.hideMainViewDuringScreenshot': 'hideMainViewDuringScreenshot',
  'listAppearance.minimalMode': 'minimalMode',
  'dock.revealHandleMode': 'revealHandleMode',
  'dock.enabledEdges': 'enabledEdges',
  'shortcuts.viewVisibility': 'viewVisibility',
  'shortcuts.newNote': 'newNote',
  'shortcuts.screenshot': 'screenshot',
  'shortcuts.screenshotCopy': 'screenshotCopy',
  'weather.enabled': 'weatherEnabled',
  'remote.receiveNotices': 'receiveNotices',
  'remote.uploadDeviceInfo': 'uploadDeviceInfo',
  'window.zOrderMode': 'zOrderMode'
}

function watchHostedUiSettings(applyUiSetting, getSettings, getWindow) {
  const dir = atriumRoot()
  const commandPath = join(dir, 'calendar-ui.json')
  const statePath = join(dir, 'calendar-ui-state.json')
  const state = {
    titlebarIconScale: 100,
    iconColor: 'black',
    uiScale: 1,
    bgColor: '255 255 255',
    textColor: '#1d1d1f',
    windowBorder: false,
    fontSizeBase: 20,
    noteRemarkFontSize: 19,
    compactFontSize: 17,
    blurEnabled: true,
    blurRadius: 12,
    blurSaturation: 1.8,
    cornerRadius: 12,
    popupOpacity: 0.2,
    bgBlur: 10,
    windowOpacity: 0.3,
    tagColorEnabled: true,
    doubleClickQuickEdit: true,
    hideMainViewDuringScreenshot: false,
    minimalMode: false,
    revealHandleMode: 'direct',
    enabledEdges: [],
    viewVisibility: '',
    newNote: '',
    screenshot: '',
    screenshotCopy: '',
    weatherEnabled: false,
    receiveNotices: true,
    uploadDeviceInfo: true,
    zOrderMode: 'top'
  }
  const publish = () => {
    try {
      writeFileSync(statePath, JSON.stringify(state))
    } catch {
      // The settings page keeps its own defaults until the calendar can be read.
    }
  }
  try {
    const values = typeof getSettings === 'function' ? getSettings() || {} : {}
    Object.assign(state, {
      titlebarIconScale: values.appearance?.titlebarIconScale ?? state.titlebarIconScale,
      iconColor: values.appearance?.iconColor ?? state.iconColor,
      uiScale: values.appearance?.uiScale ?? state.uiScale,
      bgColor: values.css?.bgColor ?? state.bgColor,
      textColor: values.css?.textColor ?? state.textColor,
      windowBorder: values.css?.windowBorder === true,
      fontSizeBase: values.css?.fontSizeBase ?? state.fontSizeBase,
      noteRemarkFontSize: values.css?.noteRemarkFontSize ?? state.noteRemarkFontSize,
      compactFontSize: values.window?.compactFontSize ?? state.compactFontSize,
      blurEnabled: values.blur?.enabled !== false,
      blurRadius: values.blur?.radius ?? state.blurRadius,
      blurSaturation: values.blur?.saturation ?? state.blurSaturation,
      cornerRadius: values.blur?.cornerRadius ?? state.cornerRadius,
      popupOpacity: values.css?.popupOpacity ?? state.popupOpacity,
      bgBlur: values.css?.bgBlur ?? state.bgBlur,
      windowOpacity: values.css?.windowOpacity ?? state.windowOpacity,
      tagColorEnabled: values.notes?.tagColorEnabled !== false,
      doubleClickQuickEdit: values.interaction?.doubleClickQuickEdit ?? true,
      hideMainViewDuringScreenshot: values.interaction?.hideMainViewDuringScreenshot === true,
      minimalMode: values.listAppearance?.minimalMode === true,
      revealHandleMode: values.dock?.revealHandleMode ?? state.revealHandleMode,
      enabledEdges: Array.isArray(values.dock?.enabledEdges) ? values.dock.enabledEdges : state.enabledEdges,
      viewVisibility: values.shortcuts?.viewVisibility ?? state.viewVisibility,
      newNote: values.shortcuts?.newNote ?? state.newNote,
      screenshot: values.shortcuts?.screenshot ?? state.screenshot,
      screenshotCopy: values.shortcuts?.screenshotCopy ?? state.screenshotCopy,
      weatherEnabled: values.weather?.enabled === true,
      receiveNotices: values.remote?.receiveNotices !== false,
      uploadDeviceInfo: values.remote?.uploadDeviceInfo !== false,
      zOrderMode: values.window?.zOrderMode ?? state.zOrderMode
    })
  } catch {
    // Defaults stay until the calendar process can read its own settings.
  }
  publish()
  let last = 0
  const read = () => {
    try {
      const data = JSON.parse(readFileSync(commandPath, 'utf8'))
      if (!data || data.at === last || typeof data.id !== 'string') return
      last = data.at
      const key = HOSTED_UI_KEYS[data.id]
      if (key) state[key] = data.value
      publish()
      try {
        const pending = applyUiSetting?.(data.id, data.value)
        if (pending && typeof pending.catch === 'function') {
          pending.catch((error) => console.warn('[Yunzhao] 日历设置没有写上:', error))
        }
      } catch (error) {
        console.warn('[Yunzhao] 日历设置没有写上:', error)
      }
      const win = typeof getWindow === 'function' ? getWindow() : null
      if (win && !win.isDestroyed()) win.webContents.send('atrium:ui-setting', { id: data.id, value: data.value })
    } catch {
      // A partial write is ignored until the file is complete.
    }
  }
  setInterval(read, 200)
}

function calendarSettingsUrl() {
  const dev = process.env.ELECTRON_RENDERER_URL
  if (dev) return `${dev.replace(/\/$/, '')}/calendar-settings.html`
  return pathToFileURL(join(process.cwd(), 'out', 'renderer', 'calendar-settings.html')).href
}

function publishCalendarSettingsHwnd(win) {
  if (!win || win.isDestroyed()) return
  const handle = win.getNativeWindowHandle()
  const hwnd = handle.length >= 8 ? handle.readBigUInt64LE(0) : handle.readUInt32LE(0)
  writeFileSync(join(atriumRoot(), 'yunzhao-calendar-settings-hwnd.txt'), String(hwnd))
}

function openCalendarSettingsWindow() {
  const win = new BrowserWindow({
    width: 880,
    height: 720,
    show: false,
    frame: false,
    skipTaskbar: true,
    resizable: false,
    title: 'Yunzhao 日历设置',
    backgroundColor: '#F6F6F8',
    webPreferences: {
      preload: surfacePreload(),
      contextIsolation: true,
      sandbox: true,
      nodeIntegration: false,
      backgroundThrottling: false
    }
  })
  addCompanionWindow(win)
  win.on('closed', () => removeCompanionWindow(win))
  const logPath = join(atriumRoot(), 'calendar-settings-log.txt')
  win.webContents.on('console-message', (_event, _level, message) => {
    try { writeFileSync(logPath, message + '\n', { flag: 'a' }) } catch { /* the page can still open */ }
  })
  win.webContents.on('did-fail-load', (_event, code, description, url) => {
    try { writeFileSync(logPath, `load failed ${code} ${description} ${url}\n`, { flag: 'a' }) } catch { /* same */ }
  })
  const publish = () => publishCalendarSettingsHwnd(win)
  win.once('ready-to-show', publish)
  win.webContents.on('did-finish-load', publish)
  win.loadURL(calendarSettingsUrl()).catch(() => {})
}

function readIsland() {
  try {
    const data = JSON.parse(readFileSync(join(atriumRoot(), 'settings.json'), 'utf8'))
    const text = typeof data.islandText === 'string' ? data.islandText.slice(0, 80) : ''
    return {
      on: data.islandOn === true,
      top: Math.max(0, Math.min(240, Number(data.islandTop) || 0)),
      width: Math.max(80, Math.min(560, Number(data.islandWidth) || 180)),
      height: Math.max(28, Math.min(140, Number(data.islandHeight) || 40)),
      text
    }
  } catch {
    return { on: false, top: 12, width: 180, height: 40, text: '' }
  }
}

function islandPage(text) {
  const body = text
    ? `<b id="label"></b>`
    : `<b id="clock"></b><script>const tick=()=>{const n=new Date();document.getElementById('clock').textContent=String(n.getHours()).padStart(2,'0')+':'+String(n.getMinutes()).padStart(2,'0')}</script>`
  const boot = text
    ? `<script>document.getElementById('label').textContent=${JSON.stringify(text)}</script>`
    : `<script>tick();setInterval(tick,1000)</script>`
  return `data:text/html;charset=utf-8,${encodeURIComponent(`<!doctype html><html><head><meta charset="utf-8"><style>html,body{margin:0;height:100%;background:transparent}body{display:flex;align-items:center;justify-content:center;color:#fff;font:600 14px "Segoe UI","Microsoft YaHei",sans-serif}b{max-width:100%;padding:0 16px;overflow:hidden;white-space:nowrap;text-overflow:ellipsis}.pill{width:100%;height:100%;display:flex;align-items:center;justify-content:center;background:#1c1c1e;border-radius:999px;box-shadow:0 8px 24px rgba(0,0,0,.28)}</style></head><body><div class="pill">${body}</div>${boot}</body></html>`)}`
}

function startIsland(getWindow) {
  let island = null
  let signature = ''
  const place = () => {
    const spec = readIsland()
    const mark = JSON.stringify(spec)
    if (!spec.on) {
      signature = mark
      if (island && !island.isDestroyed()) island.hide()
      return
    }
    const host = getWindow()
    const area = host && !host.isDestroyed() ? screen.getDisplayMatching(host.getBounds()).bounds : screen.getPrimaryDisplay().bounds
    const bounds = {
      x: Math.round(area.x + (area.width - spec.width) / 2),
      y: Math.round(area.y + spec.top),
      width: Math.round(spec.width),
      height: Math.round(spec.height)
    }
    if (!island || island.isDestroyed()) {
      island = new BrowserWindow({
        ...bounds,
        title: 'Yunzhao 灵动岛',
        frame: false,
        transparent: true,
        resizable: false,
        movable: false,
        focusable: false,
        skipTaskbar: true,
        alwaysOnTop: true,
        hasShadow: false,
        show: false,
        webPreferences: { contextIsolation: true, nodeIntegration: false }
      })
      island.setAlwaysOnTop(true, 'floating')
      signature = ''
    }
    if (!nearBounds(island.getBounds(), bounds)) island.setBounds(bounds)
    if (mark !== signature) {
      signature = mark
      island.loadURL(islandPage(spec.text)).catch(() => {})
    }
    if (!island.isVisible()) island.showInactive()
  }
  const bringBack = () => {
    const win = getWindow()
    if (!win || win.isDestroyed() || userSizing || peek || motion) return
    const current = win.getBounds()
    if (intersectsDisplay(current)) return
    const fitted = fitVisibleDisplay(current)
    if (nearBounds(current, fitted)) return
    win.setMinimumSize(1, 1)
    win.setBounds(fitted)
    restHome = fitted
    holdPlaceUntil = Date.now() + 1600
    if (!win.isVisible()) win.showInactive()
  }
  bringBack()
  hideNativeCalendarFace()
  setInterval(hideNativeCalendarFace, 1000)
  for (const eventName of ['display-added', 'display-removed', 'display-metrics-changed']) {
    screen.on(eventName, () => setTimeout(bringBack, 400))
  }
  setInterval(place, 250)
  setInterval(() => {
    const plan = readDailyPlan()
    if (!plan.channels || !plan.time) return
    const now = new Date()
    const stamp = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`
    const day = `${now.getFullYear()}-${now.getMonth() + 1}-${now.getDate()}`
    if (stamp !== plan.time || plan.fired === day) return
    writeFileSync(dailyPlanPath, JSON.stringify({ ...plan, fired: day }))
    const items = incompleteTodosThroughToday()
    if (!items.length) return
    const text = items.map((item) => `${item.past ? '逾期 ' : ''}${item.label} ${item.text}`).join('\n')
    if (plan.channels & 1 && Notification.isSupported()) {
      new Notification({ title: '未完成待办', body: text.slice(0, 180) }).show()
    }
    if (plan.channels & 8 || plan.channels & 2) openBlockingTodoPopup(items)
    const host = getWindow()
    if (plan.channels & 4 && host && !host.isDestroyed()) host.flashFrame(true)
  }, 20000)
}
