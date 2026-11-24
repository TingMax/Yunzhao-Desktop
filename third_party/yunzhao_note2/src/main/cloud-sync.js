import { execFile } from 'node:child_process'
import { watch } from 'node:fs'
import { existsSync, mkdirSync, readFileSync, unlinkSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'
import { app } from 'electron'

import { getDb } from './db/db.js'

const HOST = 'https://yunzhao-desktop.com'
const STAND_IN = 'a1000000-0000-4000-8000-000000000001'

function powershell(script) {
  return new Promise((resolve, reject) => {
    execFile(
      'powershell.exe',
      ['-NoProfile', '-NonInteractive', '-Command', script],
      { windowsHide: true, timeout: 20000 },
      (error, stdout) => {
        if (error) reject(error)
        else resolve(String(stdout || '').trim())
      }
    )
  })
}

async function unprotect(bytes) {
  const script = `
Add-Type -AssemblyName System.Security
$blob = [Convert]::FromBase64String('${bytes.toString('base64')}')
$plain = [Security.Cryptography.ProtectedData]::Unprotect($blob, $null, 'CurrentUser')
[Convert]::ToBase64String($plain)
`
  const out = await powershell(script)
  return Buffer.from(out, 'base64').toString('utf8')
}

async function protect(text) {
  const script = `
Add-Type -AssemblyName System.Security
$plain = [Convert]::FromBase64String('${Buffer.from(text, 'utf8').toString('base64')}')
$blob = [Security.Cryptography.ProtectedData]::Protect($plain, $null, 'CurrentUser')
[Convert]::ToBase64String($blob)
`
  const out = await powershell(script)
  return Buffer.from(out, 'base64')
}

function sessionPath() {
  return join(process.env.LOCALAPPDATA || '', 'Yunzhao', 'session.bin')
}

async function readSession() {
  try {
    const plain = await unprotect(readFileSync(sessionPath()))
    const data = JSON.parse(plain)
    if (!data || data.access_token === 'local-admin' || data.id === STAND_IN || !data.access_token || !data.refresh_token)
      return null
    return data
  } catch {
    return null
  }
}

async function writeSession(data) {
  const sealed = await protect(JSON.stringify(data))
  writeFileSync(sessionPath(), sealed)
}

// Every request carries a deadline: without one a blocked network left the calendar
// spinning on its loading overlay forever, which is what happened on a machine whose
// route to the server was interrupted.
const REQUEST_TIMEOUT_MS = 15000

async function request(url, options = {}) {
  const controller = new AbortController()
  const timer = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS)
  try {
    return await fetch(url, { ...options, signal: controller.signal })
  } finally {
    clearTimeout(timer)
  }
}

async function refresh(session) {
  const response = await request(`${HOST}/api/auth/refresh`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ refresh_token: session.refresh_token })
  })
  if (!response.ok) return null
  const body = await response.json()
  const tokens = body?.data?.tokens
  if (!tokens?.access_token) return null
  session.access_token = tokens.access_token
  if (tokens.refresh_token) session.refresh_token = tokens.refresh_token
  session.expires_at = Math.floor(Date.now() / 1000) + Number(tokens.expires_in || 86400)
  await writeSession(session)
  return session
}

async function send(session, method, path, payload) {
  const once = () =>
    request(HOST + path, {
      method,
      headers: {
        'Content-Type': 'application/json',
        Authorization: `Bearer ${session.access_token}`
      },
      body: payload === undefined ? undefined : JSON.stringify(payload)
    })
  let response = await once()
  if (response.status === 401) {
    const next = await refresh(session)
    if (!next) return response
    response = await once()
  }
  return response
}

function accountDir(id) {
  const dir = join(process.env.LOCALAPPDATA || '', 'Yunzhao', 'users', id)
  mkdirSync(dir, { recursive: true })
  return dir
}

function mapPath(id) {
  return join(accountDir(id), 'cloud-notes.json')
}

function handoutPath(id) {
  return join(accountDir(id), 'handouts.json')
}

function readHandouts(id) {
  try {
    const data = JSON.parse(readFileSync(handoutPath(id), 'utf8'))
    return data && typeof data === 'object' && !Array.isArray(data) ? data : {}
  } catch {
    return null
  }
}

function readMap(id) {
  try {
    return JSON.parse(readFileSync(mapPath(id), 'utf8'))
  } catch {
    return {}
  }
}

function welcomeOnly(notes) {
  return notes.length <= 1 && notes.every((note) => String(note.content || '').includes('欢迎使用 Yunzhao_note'))
}

function replaceRows(db, table, rows) {
  const columns = db.prepare(`PRAGMA table_info(${table})`).all().map((column) => column.name)
  const insert = (row) => {
    const keys = columns.filter((key) => Object.prototype.hasOwnProperty.call(row, key))
    if (!keys.length) return
    db.prepare(`INSERT OR REPLACE INTO ${table} (${keys.join(',')}) VALUES (${keys.map(() => '?').join(',')})`).run(
      keys.map((key) => row[key])
    )
  }
  const apply = db.transaction((items) => {
    for (const row of items) insert(row)
  })
  apply(rows)
}

let syncing = false
let timer = null

async function syncNotes() {
  if (syncing) return
  const db = getDb()
  if (!db) return
  const session = await readSession()
  if (!session) return
  syncing = true
  try {
    const notes = db.prepare('SELECT * FROM notes').all()
    const templates = db.prepare('SELECT * FROM note_templates').all()
    const map = readMap(session.id)
    let handouts = readHandouts(session.id)
    if (handouts == null || welcomeOnly(notes)) {
      const listed = await send(session, 'GET', '/items?kind=notes&limit=1')
      if (listed.ok) {
        const page = await listed.json()
        const row = Array.isArray(page?.items) ? page.items[0] : null
        const remote = Array.isArray(row?.payload?.notes) ? row.payload.notes : []
        const remoteHandouts = row?.payload?.handouts
        if (handouts == null && remoteHandouts && typeof remoteHandouts === 'object') {
          writeFileSync(handoutPath(session.id), JSON.stringify(remoteHandouts))
          handouts = remoteHandouts
        }
        if (welcomeOnly(notes) && remote.length > notes.length) {
          replaceRows(db, 'notes', remote)
          if (Array.isArray(row.payload.templates)) replaceRows(db, 'note_templates', row.payload.templates)
          if (row.id) writeFileSync(mapPath(session.id), JSON.stringify({ id: row.id }))
          return
        }
      }
    }
    if (!handouts) handouts = {}
    const payload = { _src: 'yunzhao_note2:notes', notes, templates, handouts }
    const response = map.id
      ? await send(session, 'PUT', `/items/${map.id}`, { payload })
      : await send(session, 'POST', '/items', { kind: 'notes', payload })
    if (response.status !== 200 && response.status !== 201) return
    const saved = await response.json()
    const id = Number(saved?.id || map.id || 0)
    if (id > 0) writeFileSync(mapPath(session.id), JSON.stringify({ id }))
  } catch {
    // The calendar keeps working from the local database when the server is unreachable.
  } finally {
    syncing = false
  }
}

export async function uploadCloudNow() {
  const db = getDb()
  const session = await readSession()
  if (!db || !session) return false
  const notes = db.prepare('SELECT * FROM notes').all()
  const templates = db.prepare('SELECT * FROM note_templates').all()
  const map = readMap(session.id)
  const handouts = readHandouts(session.id) || {}
  const payload = { _src: 'yunzhao_note2:notes', notes, templates, handouts }
  const response = map.id
    ? await send(session, 'PUT', `/items/${map.id}`, { payload })
    : await send(session, 'POST', '/items', { kind: 'notes', payload })
  if (response.status !== 200 && response.status !== 201) return false
  const saved = await response.json()
  const id = Number(saved?.id || map.id || 0)
  if (id > 0) writeFileSync(mapPath(session.id), JSON.stringify({ id }))
  return true
}

export async function pullPending() {
  const session = await readSession()
  if (!session) return false
  return existsSync(join(accountDir(session.id), 'pull-notes.once'))
}

// `force` ignores the once-per-start flag: the titlebar's refresh asks for the
// server's copy again on demand, which is the only way to recover data that a
// machine missed while it was offline.
export async function pullCloudNow({ force = false } = {}) {
  const db = getDb()
  const session = await readSession()
  if (!db || !session) return false
  const flag = join(accountDir(session.id), 'pull-notes.once')
  if (!force && !existsSync(flag)) return false
  const listed = await send(session, 'GET', '/items?kind=notes&limit=1')
  if (!listed.ok) return false
  const page = await listed.json()
  const row = Array.isArray(page?.items) ? page.items[0] : null
  const remote = Array.isArray(row?.payload?.notes) ? row.payload.notes : []
  let replaced = false
  if (remote.length) {
    replaceRows(db, 'notes', remote)
    if (Array.isArray(row.payload.templates)) replaceRows(db, 'note_templates', row.payload.templates)
    replaced = true
  }
  if (row?.payload?.handouts && typeof row.payload.handouts === 'object')
    writeFileSync(handoutPath(session.id), JSON.stringify(row.payload.handouts))
  if (row?.id) writeFileSync(mapPath(session.id), JSON.stringify({ id: row.id }))
  try {
    unlinkSync(flag)
  } catch {
    // The next start can try the pull again.
  }
  return replaced ? 'reload' : true
}

export function scheduleCloudSync() {
  clearTimeout(timer)
  timer = setTimeout(() => {
    syncNotes().catch(() => {})
  }, 3000)
}

export function startCloudSync() {
  const run = () => {
    syncNotes().catch(() => {})
  }
  setTimeout(() => {
    pullCloudNow().catch(() => {}).finally(run)
  }, 1500)
  try {
    watch(app.getPath('userData'), (_event, name) => {
      if (name !== 'app.db' && name !== 'app.db-wal') return
      clearTimeout(timer)
      timer = setTimeout(run, 3000)
    })
  } catch {
    // Startup still uploads once.
  }
}
