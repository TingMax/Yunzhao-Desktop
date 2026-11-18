import { app, BrowserWindow, Notification, screen, ipcMain } from 'electron'
import { existsSync, readFileSync, unlinkSync, watch, writeFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
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

function todayTodoText() {
  try {
    const db = getDb()
    if (!db) return '今日待办'
    const start = new Date()
    start.setHours(0, 0, 0, 0)
    const rows = db
      .prepare(
        'SELECT content FROM notes WHERE is_deleted = 0 AND effective_at >= ? AND effective_at < ? ORDER BY effective_at'
      )
      .all(start.getTime(), start.getTime() + 86400000)
    const lines = rows.map((row) => String(row.content || '').replace(/<[^>]+>/g, '').trim()).filter(Boolean)
    return lines.length ? lines.join('\n') : '今天没有待办'
  } catch {
    return '今日待办'
  }
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
      date: typeof data.date === 'string' ? data.date : ''
    }
  } catch {
    return { board: 0, lock: false }
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

export function atriumInitialBounds() {
  const place = readPlace()
  if (!place || place.left == null) return null
  return dipBounds(place)
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

const DOCK_SLACK = 8
const DOCK_STRIP = 39
const DOCK_LEAVE_MS = 180

export function setAtriumSizing(active) {
  userSizing = active === true
  if (!active) holdPlaceUntil = Date.now() + 1200
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

function currentHome(place) {
  const saved = place && place.left != null ? dipBounds(place) : null
  if (restHome) {
    if (saved && nearBounds(saved, restHome)) restHome = null
    else if (userSizing || Date.now() < holdPlaceUntil) return restHome
  }
  return saved
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

function visualFor(place) {
  const home = currentHome(place)
  if (!home) return null
  dockHome = home
  if (userSizing || Date.now() < holdPlaceUntil) {
    dockOpen = true
    return { home, open: true, edge: '' }
  }
  const spec = dockSpec(home)
  if (!spec.edge) {
    dockEdge = ''
    dockOpen = true
    dockArmed = false
    dockHideAt = 0
    return { home, open: true, edge: '' }
  }
  if (spec.edge !== dockEdge) {
    dockEdge = spec.edge
    dockOpen = false
    dockArmed = false
    dockHideAt = 0
  }
  const onEdge = pointerOpens(home, spec.edge, dockOpen)
  if (!onEdge) dockArmed = true
  if (dockOpen) {
    if (!onEdge) {
      if (!dockHideAt) dockHideAt = Date.now()
      if (Date.now() - dockHideAt >= DOCK_LEAVE_MS) dockOpen = false
    } else dockHideAt = 0
  } else if (dockArmed && onEdge) {
    dockOpen = true
    dockHideAt = 0
  }
  return { home, open: dockOpen, edge: spec.edge }
}

export function expandAtriumForDrag(win) {
  if (!win || win.isDestroyed() || !dockHome) return
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

function dayPageUrl(dateKey) {
  const query = `?surface=day&date=${encodeURIComponent(dateKey || '')}`
  const dev = process.env.ELECTRON_RENDERER_URL
  if (dev) return `${dev.replace(/\/$/, '')}/month.html${query}`
  return pathToFileURL(join(process.cwd(), 'out', 'renderer', 'month.html')).href + query
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
      if (dayWin === created) dayWin = null
    })
    dayWin.on('page-title-updated', (event) => {
      event.preventDefault()
      if (dayWin && !dayWin.isDestroyed()) dayWin.setTitle('')
    })
    dayWin.loadURL(dayPageUrl(body.dateKey))
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
      frame: true,
      autoHideMenuBar: true,
      title: '课堂讲义',
      backgroundColor: '#FFFFFF',
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

export function startAtriumHost({ getWindow, getMode, switchView, suspendDock }) {
  ipcMain.on('atrium:sizing', (_event, active) => {
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
    watch(dataDir, (_event, filename) => {
      if (filename === 'todos.json') pushTodos()
      if (filename === 'yunzhao-state.json') pushState()
      if (filename === 'yunzhao-table.json') pushTable()
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
    if (userSizing) {
      restHome = win.getBounds()
      return
    }
    if (switching) return
    const place = readPlace()
    if (!place) return
    const visual = visualFor(place)
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
  setInterval(place, 250)
  setInterval(() => {
    const plan = readDailyPlan()
    if (!plan.channels || !plan.time) return
    const now = new Date()
    const stamp = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`
    const day = `${now.getFullYear()}-${now.getMonth() + 1}-${now.getDate()}`
    if (stamp !== plan.time || plan.fired === day) return
    writeFileSync(dailyPlanPath, JSON.stringify({ ...plan, fired: day }))
    const text = todayTodoText()
    if (plan.channels & 1 && Notification.isSupported()) {
      new Notification({ title: '今日待办', body: text.slice(0, 180) }).show()
    }
    if (plan.channels & 8 || plan.channels & 2) {
      const host = getWindow()
      const area = host && !host.isDestroyed()
        ? screen.getDisplayMatching(host.getBounds()).bounds
        : screen.getPrimaryDisplay().bounds
      const box = { width: 460, height: 260 }
      const popup = new BrowserWindow({
        width: box.width,
        height: box.height,
        x: Math.round(area.x + (area.width - box.width) / 2),
        y: Math.round(area.y + (area.height - box.height) / 2),
        frame: false,
        alwaysOnTop: Boolean(plan.channels & 8),
        resizable: false,
        title: 'Yunzhao 待办',
        webPreferences: { sandbox: true }
      })
      const safe = text.replace(/[&<>]/g, (ch) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' })[ch])
      popup.loadURL(
        'data:text/html;charset=utf-8,' +
          encodeURIComponent(
            `<!doctype html><meta charset="utf-8"><body style="margin:0;font-family:Segoe UI,Microsoft YaHei,sans-serif;background:#fff;color:#1d1d1f;display:flex;flex-direction:column;height:100vh"><div style="padding:22px 24px 8px;font-size:18px;font-weight:650">今日待办</div><div style="padding:8px 24px;flex:1;font-size:15px;line-height:1.6;white-space:pre-wrap;overflow:auto">${safe}</div><button onclick="window.close()" style="margin:12px 24px 22px;height:36px;border:0;border-radius:8px;background:#0a84ff;color:#fff;font-size:14px">知道了</button></body>`
          )
      )
    }
    const host = getWindow()
    if (plan.channels & 4 && host && !host.isDestroyed()) host.flashFrame(true)
  }, 20000)
}
