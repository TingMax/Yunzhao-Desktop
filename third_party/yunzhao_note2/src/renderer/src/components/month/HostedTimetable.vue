<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { dateKeyFromParts, noteDateRange } from '../../../../shared/calendar/calendar-date-rules.js'

const names = ['周一', '周二', '周三', '周四', '周五', '周六', '周日']
const table = ref({ lessons: [], changes: [], periods: [], marks: [] })
const notes = ref([])
const focus = ref('')
const editing = ref(false)
const clock = ref(null)
const picked = ref(null)
const form = ref(null)
const paneH = ref(Number(localStorage.getItem('yunzhao-todo-pane')) || 160)

function mondayOf(iso) {
  const parts = String(iso || '').split('-').map(Number)
  const now = new Date()
  const year = parts[0] || now.getFullYear()
  const month = parts[1] || now.getMonth() + 1
  const day = parts[2] || now.getDate()
  const date = new Date(year, month - 1, day)
  const js = date.getDay()
  date.setDate(date.getDate() + (js === 0 ? -6 : 1 - js))
  return date
}

const week = computed(() => {
  const start = mondayOf(focus.value)
  return Array.from({ length: 7 }, (_, index) => {
    const date = new Date(start)
    date.setDate(start.getDate() + index)
    return {
      wd: index,
      y: date.getFullYear(),
      m: date.getMonth() + 1,
      d: date.getDate()
    }
  })
})

const monday = computed(() => week.value[0])

const focusDay = computed(() => {
  const parts = String(focus.value || '').split('-').map(Number)
  const now = new Date()
  return {
    y: parts[0] || now.getFullYear(),
    m: parts[1] || now.getMonth() + 1,
    d: parts[2] || now.getDate()
  }
})

function dayKey(day) {
  return dateKeyFromParts(day.y, day.m, day.d)
}

function noteCovers(note, day) {
  const range = noteDateRange(note)
  const key = dayKey(day)
  return range.startKey <= key && range.endKey >= key
}

const dayNotes = computed(() => notes.value.filter((note) => noteCovers(note, focusDay.value)))

function noteLabel(note) {
  const text = String(note?.content || note?.remark || '').trim()
  return text || '未命名待办'
}

async function loadNotes() {
  const data = await window.api.getWeekCalendarData?.(dayKey(focusDay.value))
  notes.value = Array.isArray(data?.notes) ? data.notes : []
}

async function toggleNote(note) {
  if (note.status === 'completed') await window.api.reopenNote?.(note.id)
  else await window.api.completeNote?.(note.id)
  await loadNotes()
}

function same(day, iso) {
  const parts = String(iso || '').split('-').map(Number)
  return day.y === parts[0] && day.m === parts[1] && day.d === parts[2]
}

function todayOf(day) {
  const now = new Date()
  return day.y === now.getFullYear() && day.m === now.getMonth() + 1 && day.d === now.getDate()
}

function marked(day) {
  return notes.value.some((note) => noteCovers(note, day))
}

function courseAt(wd, period) {
  const mon = monday.value
  const change = table.value.changes.find(
    (row) =>
      Number(row.y) === mon.y &&
      Number(row.m) === mon.m &&
      Number(row.d) === mon.d &&
      Number(row.wd) === wd &&
      Number(row.period) === period
  )
  if (change) {
    if (change.off || !change.name) return null
    return { name: change.name, teacher: change.teacher || '', kind: 'week' }
  }
  const lesson = table.value.lessons.find((row) => Number(row.wd) === wd && Number(row.period) === period)
  if (!lesson?.name) return null
  return { name: lesson.name, teacher: lesson.teacher || '', kind: 'fixed' }
}

function periodAt(row) {
  const item = table.value.periods[row]
  const start = Number(item?.s)
  const end = Number(item?.e)
  return {
    s: Number.isFinite(start) ? start : -1,
    e: Number.isFinite(end) ? end : -1
  }
}

function clockText(minutes) {
  if (!(minutes >= 0)) return ''
  const hour = Math.floor(minutes / 60)
  const minute = minutes % 60
  return `${String(hour).padStart(2, '0')}:${String(minute).padStart(2, '0')}`
}

function applyTable(next) {
  table.value = {
    lessons: Array.isArray(next?.lessons) ? next.lessons : [],
    changes: Array.isArray(next?.changes) ? next.changes : [],
    periods: Array.isArray(next?.periods) ? next.periods : [],
    marks: Array.isArray(next?.marks) ? next.marks : []
  }
}

function onEdit(event) {
  editing.value = Boolean(event.detail)
  form.value = null
  picked.value = null
  clock.value = null
}

function onWindowKey(event) {
  if (!editing.value || !picked.value) return
  if (event.key !== 'Delete' && event.key !== 'Backspace') return
  const tag = event.target?.tagName
  if (tag === 'INPUT' || tag === 'TEXTAREA' || event.target?.isContentEditable) return
  event.preventDefault()
  clearPicked()
}

let stopTable = null
let stopState = null
let stopNotes = null

onMounted(() => {
  window.addEventListener('atrium-table-edit', onEdit)
  window.addEventListener('keydown', onWindowKey)
  void window.api.atriumTable?.().then(applyTable)
  void window.api.atriumState?.().then((state) => {
    focus.value = state?.date || ''
    return loadNotes()
  })
  stopTable = window.api.onAtriumTable?.(applyTable)
  stopNotes = window.api.onNotesChanged?.(() => {
    void loadNotes()
  })
  stopState = window.api.onAtriumState?.((state) => {
    focus.value = state?.date || focus.value
  })
})

watch(focus, (key) => {
  void loadNotes()
  if (/^\d{4}-\d{2}-\d{2}$/.test(String(key || ''))) void window.api.atriumDaySync?.({ dateKey: key })
})

onBeforeUnmount(() => {
  window.removeEventListener('atrium-table-edit', onEdit)
  window.removeEventListener('keydown', onWindowKey)
  stopTable?.()
  stopNotes?.()
  stopState?.()
})

function pickDay(day) {
  focus.value = dayKey(day)
  window.api.atriumCommand?.({ cmd: 'focus', y: day.y, m: day.m, d: day.d })
}

function shiftWeek(delta) {
  window.api.atriumCommand?.({ cmd: 'week', delta })
}

function chosen(wd, period) {
  return picked.value && picked.value.wd === wd && picked.value.period === period
}

function openCell(day, period) {
  if (!editing.value) {
    pickDay(day)
    return
  }
  const course = courseAt(day.wd, period)
  const again = chosen(day.wd, period)
  picked.value = { wd: day.wd, period, kind: course?.kind || 'fixed' }
  clock.value = null
  if (!course || again) {
    form.value = {
      wd: day.wd,
      period,
      name: course?.name || '',
      teacher: course?.teacher || '',
      scope: course?.kind || 'fixed'
    }
  } else form.value = null
}

function clearPicked() {
  if (!picked.value) return
  const mon = monday.value
  window.api.atriumCommand?.({
    cmd: 'cell',
    wd: picked.value.wd,
    period: picked.value.period,
    name: '',
    teacher: '',
    scope: picked.value.kind,
    y: mon.y,
    m: mon.m,
    d: mon.d
  })
  picked.value = null
  form.value = null
}

function saveForm() {
  if (!form.value) return
  const mon = monday.value
  window.api.atriumCommand?.({
    cmd: 'cell',
    wd: form.value.wd,
    period: form.value.period,
    name: form.value.name.trim(),
    teacher: form.value.teacher.trim(),
    scope: form.value.scope === 'week' ? 'week' : 'fixed',
    y: mon.y,
    m: mon.m,
    d: mon.d
  })
  form.value = null
  picked.value = null
}

function handoutKey(day, period) {
  return `${day.y}-${day.m}-${day.d}-${period}`
}

async function openHandout(day, period) {
  if (editing.value) return
  if (!courseAt(day.wd, period)) return
  const key = handoutKey(day, period)
  const all = (await window.api.atriumHandouts?.()) || {}
  await window.api.atriumOpenHandout?.({
    key,
    title: `${day.m}月${day.d}日 ${names[day.wd] || ''}`,
    html: typeof all[key] === 'string' ? all[key] : ''
  })
}

function startSplit(event) {
  const startY = event.clientY
  const start = paneH.value
  const move = (ev) => {
    paneH.value = Math.max(72, Math.min(window.innerHeight * 0.7, start - (ev.clientY - startY)))
  }
  const up = () => {
    window.removeEventListener('pointermove', move)
    localStorage.setItem('yunzhao-todo-pane', String(Math.round(paneH.value)))
  }
  window.addEventListener('pointermove', move)
  window.addEventListener('pointerup', up, { once: true })
}

function openClock(row) {
  if (!editing.value) return
  const period = periodAt(row)
  clock.value = { row, s: period.s >= 0 ? period.s : 8 * 60, e: period.e >= 0 ? period.e : 8 * 60 + 45 }
  form.value = null
}

function stepClock(field, delta) {
  if (!clock.value) return
  const next = Math.max(0, Math.min(24 * 60 - 5, clock.value[field] + delta))
  clock.value = { ...clock.value, [field]: next }
  if (clock.value.e <= clock.value.s) clock.value.e = Math.min(24 * 60, clock.value.s + 5)
}

function saveClock() {
  if (!clock.value) return
  window.api.atriumCommand?.({ cmd: 'period', row: clock.value.row, s: clock.value.s, e: clock.value.e })
  clock.value = null
}
</script>

<template>
  <div class="timetable">
    <div class="sheet">
      <div class="week-nav">
        <button type="button" title="上一周" @click="shiftWeek(-1)">‹</button>
        <button type="button" title="下一周" @click="shiftWeek(1)">›</button>
      </div>
      <button
        v-for="day in week"
        :key="day.wd"
        type="button"
        class="day"
        :class="{ picked: same(day, focus), today: todayOf(day) }"
        :style="{ gridColumn: 4 + day.wd, gridRow: 1 }"
        @click="pickDay(day)"
      >
        <span>{{ names[day.wd] }}</span>
        <strong>{{ day.m }}/{{ day.d }}</strong>
        <i v-if="marked(day)" />
      </button>

      <div class="ampm am">上<br />午</div>
      <div class="ampm pm">下<br />午</div>

      <template v-for="row in 8" :key="row">
        <div class="num" :class="{ split: row === 5 }" :style="{ gridColumn: 2, gridRow: row + 1 }">{{ row <= 5 ? row : row - 5 }}</div>
        <button type="button" class="time" :class="{ split: row === 5 }" :style="{ gridColumn: 3, gridRow: row + 1 }" @click="openClock(row - 1)">
          <template v-if="clock && clock.row === row - 1">
            <span class="stepper" @click.stop>
              <button type="button" @click="stepClock('s', -5)">−</button>
              {{ clockText(clock.s) }}
              <button type="button" @click="stepClock('s', 5)">+</button>
            </span>
            <span class="stepper" @click.stop>
              <button type="button" @click="stepClock('e', -5)">−</button>
              {{ clockText(clock.e) }}
              <button type="button" @click="stepClock('e', 5)">+</button>
            </span>
            <button type="button" class="ok" @click.stop="saveClock">确定</button>
          </template>
          <template v-else>
            {{ clockText(periodAt(row - 1).s) || (editing ? '设置' : '') }}
            <small v-if="periodAt(row - 1).e >= 0">{{ clockText(periodAt(row - 1).e) }}</small>
          </template>
        </button>
        <button
          v-for="day in week"
          :key="`${day.wd}-${row}`"
          type="button"
          class="cell"
          :class="{ picked: same(day, focus), split: row === 5, 'is-editing': editing, chosen: chosen(day.wd, row - 1) }"
          :style="{ gridColumn: 4 + day.wd, gridRow: row + 1 }"
          @click="openCell(day, row - 1)"
          @dblclick.stop="openHandout(day, row - 1)"
        >
          <template v-if="courseAt(day.wd, row - 1)">
            <b>{{ courseAt(day.wd, row - 1).name }}</b>
            <small v-if="courseAt(day.wd, row - 1).teacher">{{ courseAt(day.wd, row - 1).teacher }}</small>
          </template>
        </button>
      </template>
    </div>
    <div class="split-line" @pointerdown.prevent="startSplit" />
    <section class="todo-pane" :style="{ height: paneH + 'px' }">
      <h2>{{ focusDay.m }}月{{ focusDay.d }}日</h2>
      <button v-for="note in dayNotes" :key="note.id" type="button" class="todo" @click="toggleNote(note)">
        <i class="ring" :class="{ done: note.status === 'completed' }" :style="{ color: note.status === 'completed' ? '#07c160' : '#8e8e93' }" />
        <span :class="{ done: note.status === 'completed' }">{{ noteLabel(note) }}</span>
      </button>
      <p v-if="!dayNotes.length" class="todo-empty">这一天没有待办</p>
    </section>
    <div v-if="form" class="mask" @click.self="form = null">
      <form class="dialog" @submit.prevent="saveForm">
        <h3>课程</h3>
        <input v-model="form.name" placeholder="课程名称" />
        <input v-model="form.teacher" placeholder="任课老师" />
        <div class="scopes">
          <label><input v-model="form.scope" type="radio" value="fixed" />固定课</label>
          <label><input v-model="form.scope" type="radio" value="week" />调课</label>
        </div>
        <p class="hint">调课只改这一周，下周仍按固定课显示。</p>
        <div class="actions">
          <button type="button" @click="form = null">取消</button>
          <button type="submit" class="ok">确定</button>
        </div>
      </form>
    </div>
  </div>
</template>

<style scoped>
.timetable {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  container-type: size;
  background: #f5f5f7;
  color: #1d1d1f;
  border-radius: 24px;
  font-family: 'OPPOSans', 'Segoe UI', 'Microsoft YaHei', sans-serif;
}
.sheet {
  display: grid;
  grid-template-columns: clamp(12px, 2cqw, 18px) clamp(14px, 2.2cqw, 20px) clamp(40px, 9cqw, 92px) repeat(7, minmax(0, 1fr));
  grid-template-rows: clamp(26px, 8cqh, 56px) repeat(8, minmax(0, 1fr));
  flex: 1;
  min-height: 0;
  padding: clamp(4px, 1cqh, 12px) clamp(6px, 1.2cqw, 14px);
  box-sizing: border-box;
  gap: clamp(2px, 0.5cqh, 6px);
}
.split-line {
  flex: 0 0 10px;
  cursor: row-resize;
  position: relative;
  z-index: 3;
}
.split-line::after {
  content: '';
  position: absolute;
  left: 18px;
  right: 18px;
  top: 3px;
  height: 3px;
  border-radius: 2px;
  background: transparent;
}
.split-line:hover::after {
  background: #0a84ff;
}
.todo-pane {
  flex: 0 0 auto;
  min-height: 0;
  box-sizing: border-box;
  overflow: auto;
  padding: 8px 14px 12px;
  border-top: 1px solid #e5e5ea;
}
.cell.chosen {
  outline: 2px solid #0a84ff;
  outline-offset: -2px;
}
.mask {
  position: absolute;
  inset: 0;
  z-index: 8;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgba(0, 0, 0, 0.18);
}
.dialog {
  width: min(360px, calc(100% - 32px));
  display: flex;
  flex-direction: column;
  gap: 10px;
  padding: 16px;
  border-radius: 16px;
  background: #fff;
  box-shadow: 0 16px 40px rgba(0, 0, 0, 0.16);
}
.dialog h3 {
  margin: 0;
  font-size: 16px;
}
.dialog input[type='text'],
.dialog input:not([type]) {
  height: 36px;
  border: 1px solid #d1d1d6;
  border-radius: 8px;
  padding: 0 10px;
  font: inherit;
}
.scopes {
  display: flex;
  gap: 16px;
}
.hint {
  margin: 0;
  color: #6e6e73;
  font-size: 12px;
}
.actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
}
.handout {
  position: fixed;
  inset: 8px;
  z-index: 30;
  display: flex;
  flex-direction: column;
  background: #fff;
  border-radius: 16px;
  box-shadow: 0 16px 40px rgba(0, 0, 0, 0.18);
}
.handout.is-full {
  inset: 0;
  border-radius: 0;
}
.handout header {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 10px 12px;
}
.handout header b {
  margin-right: auto;
}
.handout header button,
.dialog button {
  height: 32px;
  border: 0;
  border-radius: 8px;
  padding: 0 12px;
  background: #f2f2f7;
  font: inherit;
  cursor: pointer;
}
.handout header button:hover,
.dialog button:hover {
  background: #e5e5ea;
}
.handout header button:active,
.dialog button:active {
  background: #d1d1d6;
  transform: scale(0.96);
}
.handout header button.icon {
  width: 32px;
  padding: 0;
  font-family: 'Segoe MDL2 Assets', sans-serif;
  font-size: 16px;
}
.handout-body {
  flex: 1;
  min-height: 0;
  overflow: auto;
  padding: 12px 16px 20px;
  outline: none;
  font-size: 15px;
  line-height: 1.6;
}
.handout-body img {
  max-width: 100%;
  height: auto;
}
.todo-pane h2 {
  margin: 0 0 6px;
  color: #0a84ff;
  font-size: 14px;
  font-weight: 700;
}
.todo {
  display: flex;
  align-items: center;
  gap: 8px;
  width: 100%;
  height: 26px;
  margin-top: 4px;
  padding: 0 8px;
  border: 0;
  border-radius: 8px;
  background: rgba(29, 29, 31, 0.07);
  color: #1d1d1f;
  font: inherit;
  font-size: 13px;
  text-align: left;
  cursor: pointer;
}
.todo span {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.todo span.done {
  color: #aeaeb2;
  text-decoration: line-through;
}
.ring {
  flex: 0 0 auto;
  width: 13px;
  height: 13px;
  border: 2px solid currentColor;
  border-radius: 50%;
  box-sizing: border-box;
}
.ring.done::after {
  content: '';
  display: block;
  width: 3px;
  height: 6px;
  margin: 0 0 0 3px;
  border: solid currentColor;
  border-width: 0 1.5px 1.5px 0;
  transform: rotate(45deg);
}
.todo-empty {
  margin: 8px 0 0;
  color: #8e8e93;
  font-size: 13px;
}
.week-nav {
  grid-column: 1 / 4;
  grid-row: 1;
  display: flex;
  align-items: center;
  gap: 4px;
}
.week-nav button,
.day,
.time,
.cell {
  border: 0;
  background: transparent;
  color: inherit;
  font: inherit;
  cursor: pointer;
}
.week-nav button {
  width: 44px;
  height: 44px;
  border-radius: 10px;
  color: #6e6e73;
  font-size: 36px;
}
.week-nav button:hover {
  background: rgba(29, 29, 31, 0.06);
}
.day {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 1px;
  border-radius: 8px;
  position: relative;
}
.day span {
  font-size: 14px;
  font-weight: 700;
}
.day strong {
  font-size: 12px;
  font-weight: 500;
  color: #6e6e73;
}
.day.today strong,
.day.picked {
  color: #0a84ff;
}
.day.picked {
  background: rgba(10, 132, 255, 0.1);
}
.day i {
  position: absolute;
  top: 6px;
  right: 8px;
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: #ff3b30;
}
.ampm {
  display: flex;
  align-items: center;
  justify-content: center;
  color: #6e6e73;
  font-size: 12px;
  font-weight: 600;
  line-height: 1.15;
  writing-mode: horizontal-tb;
}
.ampm.am {
  grid-column: 1;
  grid-row: 2 / 7;
}
.ampm.pm {
  grid-column: 1;
  grid-row: 7 / 10;
}
.num,
.time,
.cell {
  min-width: 0;
  min-height: 0;
}
.num {
  display: flex;
  align-items: center;
  justify-content: center;
  color: #6e6e73;
  font-size: 12px;
  font-weight: 600;
}
.time {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 1px;
  color: #6e6e73;
  font-size: 11px;
  line-height: 1.2;
}
.time small,
.cell small {
  color: #6e6e73;
  font-size: 10px;
  font-weight: 500;
}
.cell {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 2px;
  border: 1px solid rgba(29, 29, 31, 0.1);
  border-radius: 4px;
  background: #fff;
}
.cell.picked {
  background: rgba(10, 132, 255, 0.06);
}
.cell.split,
.time.split,
.num.split {
  box-shadow: inset 0 -1px 0 rgba(29, 29, 31, 0.16);
}
.cell b {
  max-width: 100%;
  overflow: hidden;
  font-size: 15px;
  font-weight: 700;
  line-height: 1.15;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.cell.is-editing:hover {
  border-color: rgba(10, 132, 255, 0.45);
}
.name-input,
.teacher-input {
  width: calc(100% - 8px);
  border: 0;
  border-radius: 4px;
  background: #f2f2f7;
  color: #1d1d1f;
  text-align: center;
  font-family: inherit;
}
.name-input {
  height: 22px;
  font-size: 13px;
  font-weight: 700;
}
.teacher-input {
  height: 16px;
  font-size: 10px;
}
.stepper {
  display: inline-flex;
  align-items: center;
  gap: 2px;
}
.stepper button,
.ok {
  border: 0;
  border-radius: 4px;
  background: rgba(10, 132, 255, 0.14);
  color: #0a84ff;
  font: inherit;
  cursor: pointer;
}
.stepper button {
  width: 16px;
  height: 16px;
  padding: 0;
}
.ok {
  margin-top: 2px;
  padding: 0 6px;
  font-size: 10px;
}
</style>
