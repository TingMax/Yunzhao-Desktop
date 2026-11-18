<script setup>
import { onBeforeUnmount, onMounted, ref } from 'vue'

const board = ref(0)
const locked = ref(false)
const hold = ref(0)
let stopState = null
let holdTimer = 0
let holdStart = 0

async function refresh() {
  try {
    const state = await window.api.atriumState?.()
    if (!state) return
    board.value = Number(state.board) === 2 ? 2 : 0
    locked.value = Boolean(state.lock)
  } catch {
    // The desktop still owns the switch if this read fails.
  }
}

function send(payload) {
  void window.api.atriumCommand?.(payload)
}

function choose(next) {
  if (locked.value || next === board.value) return
  board.value = next
  send({ cmd: 'board', board: next })
}

function onLockDown(event) {
  if (event.button != null && event.button !== 0) return
  if (!locked.value) {
    locked.value = true
    send({ cmd: 'lock' })
    return
  }
  holdStart = performance.now()
  hold.value = 0.02
  holdTimer = window.setInterval(() => {
    const t = Math.min(1, (performance.now() - holdStart) / 3000)
    hold.value = t
    if (t >= 1) {
      window.clearInterval(holdTimer)
      holdTimer = 0
      hold.value = 0
      locked.value = false
      send({ cmd: 'unlock' })
    }
  }, 40)
}

function onLockUp() {
  if (!holdTimer) return
  window.clearInterval(holdTimer)
  holdTimer = 0
  hold.value = 0
}

onMounted(async () => {
  await refresh()
  stopState = window.api.onAtriumState?.((state) => {
    board.value = Number(state?.board) === 2 ? 2 : 0
    locked.value = Boolean(state?.lock)
  })
})

onBeforeUnmount(() => {
  onLockUp()
  stopState?.()
})
</script>

<template>
  <div class="atrium-dock" @pointerdown.stop>
    <button
      type="button"
      class="atrium-dock__lock"
      :class="{ 'is-locked': locked }"
      :title="locked ? '按住 3 秒解锁' : '锁定'"
      @pointerdown="onLockDown"
      @pointerup="onLockUp"
      @pointercancel="onLockUp"
      @pointerleave="onLockUp"
    >
      <svg v-if="hold > 0" class="atrium-dock__ring" viewBox="0 0 32 32" aria-hidden="true">
        <circle cx="16" cy="16" r="13" />
        <circle
          cx="16"
          cy="16"
          r="13"
          :stroke-dasharray="`${hold * 81.7} 81.7`"
        />
      </svg>
      <span class="atrium-dock__mdl" aria-hidden="true">{{ locked ? '\uE72E' : '\uE785' }}</span>
    </button>
    <slot />
    <div class="atrium-dock__switch" role="tablist" aria-label="日历和课表">
      <button type="button" role="tab" :aria-selected="board !== 2" :class="{ 'is-on': board !== 2 }" @click="choose(0)">
        日历
      </button>
      <button type="button" role="tab" :aria-selected="board === 2" :class="{ 'is-on': board === 2 }" @click="choose(2)">
        课表
      </button>
    </div>
    <slot name="tools" />
  </div>
</template>

<style scoped>
.atrium-dock {
  display: flex;
  align-items: center;
  gap: 4px;
  height: 30px;
  color: #1d1d1f;
  font-family: 'OPPOSans', 'Segoe UI', 'Microsoft YaHei', sans-serif;
}
.atrium-dock button {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  height: 28px;
  border: 0;
  border-radius: 7px;
  background: transparent;
  color: inherit;
  font-family: inherit;
  font-size: 13px;
  font-weight: 600;
  line-height: 1;
  cursor: pointer;
}
.atrium-dock button:disabled {
  opacity: 0.4;
  cursor: default;
}
.atrium-dock__lock {
  position: relative;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 32px;
  color: #6e6e73;
}
.atrium-dock__mdl {
  font-family: "Segoe MDL2 Assets", sans-serif;
  font-size: 16px;
  line-height: 1;
}
.atrium-dock__lock.is-locked {
  color: #ff9f0a;
}
.atrium-dock__ring {
  position: absolute;
  inset: -3px;
  width: calc(100% + 6px);
  height: calc(100% + 6px);
}
.atrium-dock__ring circle {
  fill: none;
  stroke: rgba(255, 159, 10, 0.28);
  stroke-width: 2;
}
.atrium-dock__ring circle:last-child {
  stroke: #ff9f0a;
  stroke-linecap: round;
  transform: rotate(-90deg);
  transform-origin: 16px 16px;
}
.atrium-dock__switch {
  display: flex;
  margin-left: 4px;
  padding: 2px;
  border-radius: 8px;
  background: #f2f2f7;
}
.atrium-dock__switch button {
  width: 64px;
  color: #6e6e73;
}
.atrium-dock__switch button.is-on {
  background: #0a84ff;
  color: #fff;
}
</style>
