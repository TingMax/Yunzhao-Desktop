<script setup>
import { useWorkspaceRuntime } from './composables/useWorkspaceRuntime.js'
import { useWorkspaceWallpaper } from './composables/useWorkspaceWallpaper.js'
import { useWorkspaceNotices } from './composables/useWorkspaceNotices.js'
import { useUpdateCheck } from './composables/useUpdateCheck.js'
import { useModalRequest } from './composables/useQueuedModal.js'
import EditingDraftDialog from './components/system/EditingDraftDialog.vue'
import CaptureDelivery from './components/capture/CaptureDelivery.vue'
import { editingDataGeneration } from './composables/useDraftProtection.js'
import { defineAsyncComponent } from 'vue'
import { computed, ref, onMounted } from 'vue'
import AlmanacLauncher from './components/almanac/AlmanacLauncher.vue'
import AppTitlebar from './components/system/AppTitlebar.vue'
import TitlebarActions from './components/system/TitlebarActions.vue'
import ViewSwitcher from './components/system/ViewSwitcher.vue'
import ResizeHandles from './components/system/ResizeHandles.vue'
import WindowPresentation from './components/system/WindowPresentation.vue'
const SettingsPanel = defineAsyncComponent(() => import('./components/system/SettingsPanel.vue'))
import MessageToast from './components/system/MessageToast.vue'
import MonthWorkspace from './components/month/MonthWorkspace.vue'
import UpdateDialog from './components/system/UpdateDialog.vue'
import RemoteNoticeDialog from './components/system/RemoteNoticeDialog.vue'
import FirstUseNoticeDialog from './components/system/FirstUseNoticeDialog.vue'
import HolidayDataNoticeDialog from './components/system/HolidayDataNoticeDialog.vue'
import ShortcutConflictDialog from './components/system/ShortcutConflictDialog.vue'
import DailyReportDialog from './components/report/DailyReportDialog.vue'
import DailyReportButton from './components/report/DailyReportButton.vue'
import AppIcon from './components/ui/AppIcon.vue'
import TemplatePage from './components/template/TemplatePage.vue'
import HelpPage from './components/help/HelpPage.vue'
import { useSlidingWorkspace } from './composables/useSlidingWorkspace.js'
import { createDefaultSettings, VIEW_MODES } from '../../shared/settings-schema.js'

const props = defineProps({
  viewMode: {
    type: String,
    default: VIEW_MODES.MONTH,
    validator: (value) => value === VIEW_MODES.MONTH || value === VIEW_MODES.WEEK
  }
})
const isWeekView = computed(() => props.viewMode === VIEW_MODES.WEEK)
const yunzhaoShell = ref(false)
const yunzhaoLocked = ref(false)
const peekEdge = ref('')
const peekBox = ref({ thick: 17, long: 151, radius: 6 })
const peekStyle = computed(() => {
  if (!peekEdge.value) return null
  const thick = `${peekBox.value.thick}px`
  const radius = `${peekBox.value.radius}px`
  if (peekEdge.value === 'left') {
    return { inset: '0 0 0 auto', width: thick, borderRadius: `0 ${radius} ${radius} 0` }
  }
  if (peekEdge.value === 'right') {
    return { inset: '0 auto 0 0', width: thick, borderRadius: `${radius} 0 0 ${radius}` }
  }
  if (peekEdge.value === 'bottom') {
    return { inset: '0 0 auto 0', height: thick, borderRadius: `${radius} ${radius} 0 0` }
  }
  return { inset: 'auto 0 0 0', height: thick, borderRadius: `0 0 ${radius} ${radius}` }
})
function expandPeek() {
  void window.api?.yunzhaoCommand?.({ cmd: 'expand' })
}
function hoverPeek(on) {
  void window.api?.yunzhaoCommand?.({ cmd: 'peek-hover', on })
}
const daySurface = new URLSearchParams(window.location.search).get('surface') === 'day'
const dayKey = new URLSearchParams(window.location.search).get('date') || ''
onMounted(() => {
  if (daySurface) return
  void window.api?.yunzhaoHosted?.().then((hosted) => {
    yunzhaoShell.value = Boolean(hosted)
    if (!hosted) return
    document.documentElement.style.background = 'transparent'
    document.body.style.background = 'transparent'
  })
  void window.api?.yunzhaoState?.().then((state) => {
    yunzhaoLocked.value = state?.lock === true
  })
  window.api?.onYunzhaoState?.((state) => {
    yunzhaoLocked.value = state?.lock === true
  })
  window.api?.onYunzhaoPeek?.((payload) => {
    peekEdge.value = payload?.edge || ''
    if (payload?.thick) {
      peekBox.value = {
        thick: payload.thick,
        long: payload.long,
        radius: payload.radius
      }
    }
  })
})
const viewLabel = computed(() => (isWeekView.value ? '周视图' : '月视图'))
const defaults = createDefaultSettings(props.viewMode)
const workspaceRuntime = useWorkspaceRuntime({
  defaults: defaults,
  getViewMode: () => props.viewMode,
  label: 'MonthApp',
  hoverPhase: 'mounted',
  windowFallback: true,
  waitForContent: true,
  onSnapshot: (snapshot) => {
    void wallpaper.sync(snapshot)
  },
  onInitialSnapshot: (snapshot) => notices.applyInitialSnapshot(snapshot),
  onSettingsChanged: (snapshot) => notices.revealFirstUseNoticeFromSnapshot(snapshot),

  onRemoteNoticesChanged: () => {
    void notices.loadPendingRemoteNotices({ show: true })
  },
  beforeReady: async () => {
    await notices.loadPendingRemoteNotices({ show: true })
    await notices.loadHolidayDataNotice()
  }
})
const { showMessage, presentationMode, locked, zOrderMode } = workspaceRuntime
const wallpaper = useWorkspaceWallpaper({
  defaults: defaults,
  cacheLoads: false,
  label: 'MonthApp'
})
const { wallpaperUrl, wallpaperVisible, wallpaperBlurRadius, wallpaperRenderKey } = wallpaper
const updateCheck = useUpdateCheck({ policy: 'calendar', label: 'MonthApp' })
const { updateQueue, showUpdateDialog, updateChecking, updateResult, checkForUpdates } = updateCheck
const notices = useWorkspaceNotices({ showMessage, openSettings, label: 'MonthApp' })
const {
  noticeQueue,
  firstUseQueue,
  holidayQueue,
  showRemoteNoticeDialog,
  showFirstUseNotice,
  showHolidayDataNoticeDialog,
  pendingRemoteNotices,
  pendingHolidayDataNotice,
  dismissHolidayDataNotice,
  closeRemoteNoticeDialog,
  onRemoteNoticeAcknowledged,
  onFirstUseCompleted
} = notices

const showSettings = ref(false)
const reportQueue = useModalRequest()
const showDailyReportDialog = reportQueue.requested
const almanacQueue = useModalRequest()
const showAlmanac = almanacQueue.requested
const templatePanelRef = ref(null)
const helpPanelRef = ref(null)
const templateWorkspace = useSlidingWorkspace({ getElement: () => templatePanelRef.value })
const helpWorkspace = useSlidingWorkspace({ getElement: () => helpPanelRef.value })
const {
  rendered: templatesRendered,
  active: templatePanelActive,
  phase: templatePhase,
  interactive: templateInteractive,
  close: closeTemplateWorkspace,
  onTransitionEnd: onTemplateTransitionEnd,
  onTransitionCancel: onTemplateTransitionCancel
} = templateWorkspace
const {
  rendered: helpRendered,
  active: helpPanelActive,
  phase: helpPhase,
  interactive: helpInteractive,
  close: closeHelpWorkspace,
  onTransitionEnd: onHelpTransitionEnd,
  onTransitionCancel: onHelpTransitionCancel
} = helpWorkspace
const showShortcutNotice = ref(false)
const calendarBusinessModalOpen = ref(false)
const compactBlocked = computed(
  () =>
    showSettings.value ||
    calendarBusinessModalOpen.value ||
    showFirstUseNotice.value ||
    showUpdateDialog.value ||
    showRemoteNoticeDialog.value ||
    showHolidayDataNoticeDialog.value ||
    showShortcutNotice.value ||
    showDailyReportDialog.value ||
    showAlmanac.value ||
    templateInteractive.value ||
    helpInteractive.value
)

async function requestCompactPresentation(anchor) {
  if (locked.value) {
    showMessage('warning', '窗口已锁定：切换为灵动岛会改变窗口位置和尺寸，请先解锁')
    return
  }
  if (compactBlocked.value) {
    showMessage('warning', '请先完成或关闭当前操作，再收起为灵动岛')
    return
  }
  try {
    await presentationMode.enter(anchor)
  } catch (error) {
    showMessage('error', `收起为灵动岛失败：${error.message}`)
  }
}

const onCalendarWorkspaceReady = workspaceRuntime.markContentReady

function openSettings() {
  closeTemplates()
  closeHelp()
  workspaceRuntime.retainBlur('settings')
  showSettings.value = true
}

function openDailyReport() {
  closeTemplates()
  closeHelp()
  showDailyReportDialog.value = true
}

function openTemplates() {
  closeHelp()
  void templateWorkspace.open()
}

function closeTemplates() {
  closeTemplateWorkspace()
}

function toggleTemplates() {
  if (templatePhase.value === 'closed' || templatePhase.value === 'closing') openTemplates()
  else closeTemplates()
}

function openHelp() {
  closeTemplates()
  void helpWorkspace.open()
}

function closeHelp() {
  closeHelpWorkspace()
}

function toggleHelp() {
  if (helpPhase.value === 'closed' || helpPhase.value === 'closing') void openHelp()
  else closeHelpWorkspace()
}

function releaseSettingsBlur() {
  workspaceRuntime.releaseBlur('settings')
}
</script>

<template>
  <div v-if="daySurface" class="month-root app-bg day-surface">
    <MonthWorkspace day-surface :day-key="dayKey" @ready="onCalendarWorkspaceReady" />
  </div>
  <div v-else class="month-root app-bg" :class="{ 'is-week-view': isWeekView, 'yunzhao-shell': yunzhaoShell, 'is-peek': peekEdge }">
    <Transition name="month-wallpaper" v-bind="wallpaper.transitionHooks()">
      <div v-if="wallpaperVisible && !yunzhaoShell" :key="wallpaperRenderKey" class="month-wallpaper">
        <div
          class="month-wallpaper-image"
          :style="{
            backgroundImage: `url(${wallpaperUrl})`,
            '--wallpaper-blur': `${wallpaperBlurRadius}px`
          }"
        />
      </div>
    </Transition>
    <button
      v-if="peekEdge"
      type="button"
      class="calendar-peek"
      :class="`is-${peekEdge}`"
      :style="peekStyle"
      title="展开"
      @click="expandPeek"
      @pointerenter="hoverPeek(true)"
      @pointerleave="hoverPeek(false)"
    >
      <span />
    </button>
    <ResizeHandles v-show="!peekEdge" :locked="yunzhaoShell ? yunzhaoLocked : locked" :mode="presentationMode.displayMode.value" />
    <WindowPresentation
      v-show="!peekEdge"
      :mode="presentationMode.mode.value"
      :operation="presentationMode.operation.value"
      :locked="locked"
    >
      <div
        class="month-scene"
        :class="{
          'is-ui-background-blurred': showSettings || calendarBusinessModalOpen
        }"
        :inert="
          showSettings ||
          calendarBusinessModalOpen ||
          showFirstUseNotice ||
          showUpdateDialog ||
          showRemoteNoticeDialog ||
          showHolidayDataNoticeDialog ||
          showShortcutNotice ||
          showDailyReportDialog ||
          showAlmanac
        "
      >
        <AppTitlebar
          v-model:locked="locked"
          v-model:z-order-mode="zOrderMode"
          @request:compact="requestCompactPresentation"
        >
          <TitlebarActions>
            <ViewSwitcher :active-view="viewMode" />
            <AlmanacLauncher v-model:visible="showAlmanac" :queue="almanacQueue" />
            <DailyReportButton month-view @open="openDailyReport" />
            <button
              class="titlebar-btn titlebar-btn-template month-titlebar-btn"
              :class="{ 'is-active': templatePanelActive }"
              :title="templatePanelActive ? '关闭循环模板' : '打开循环模板'"
              aria-controls="template-workspace"
              :aria-expanded="templatePanelActive"
              @click="toggleTemplates"
            >
              <AppIcon class="btn-icon" name="recurrence" alt="循环模板" />
            </button>
            <button
              class="titlebar-btn titlebar-btn-settings month-titlebar-btn"
              title="设置"
              @click="openSettings"
            >
              <AppIcon class="btn-icon" name="settings" alt="设置" />
            </button>
            <button
              class="titlebar-btn titlebar-btn-help month-titlebar-btn"
              :class="{ 'is-active': helpPanelActive }"
              :title="helpPanelActive ? '关闭帮助' : '帮助'"
              aria-controls="help-workspace"
              :aria-expanded="helpPanelActive"
              @click="toggleHelp"
            >
              <AppIcon class="btn-icon" name="help" alt="帮助" />
            </button>
          </TitlebarActions>
        </AppTitlebar>
        <div class="month-content-stage">
          <main
            class="month-content"
            :class="{ 'is-ui-background-blurred': templateInteractive || helpInteractive }"
            :inert="templateInteractive || helpInteractive"
            :aria-label="`${viewLabel}内容区域`"
          >
            <MonthWorkspace
              :key="editingDataGeneration"
              :view-mode="viewMode"
              @modal-state-change="calendarBusinessModalOpen = $event"
              @ready="onCalendarWorkspaceReady"
            />
          </main>

          <div
            v-if="templatesRendered"
            class="month-template-wrapper"
            :class="{ 'is-interactive': templateInteractive }"
          >
            <div
              id="template-workspace"
              ref="templatePanelRef"
              class="month-template-panel"
              :class="{ active: templatePanelActive }"
              role="region"
              aria-label="循环待办模板设置"
              @transitionend="onTemplateTransitionEnd"
              @transitioncancel="onTemplateTransitionCancel"
            >
              <TemplatePage :key="editingDataGeneration" />
            </div>
          </div>

          <div
            v-if="helpRendered"
            class="month-help-wrapper"
            :class="{ 'is-interactive': helpInteractive }"
          >
            <div
              id="help-workspace"
              ref="helpPanelRef"
              class="month-help-panel"
              :class="{ active: helpPanelActive }"
              role="region"
              :aria-label="`${viewLabel}帮助中心`"
              @transitionend="onHelpTransitionEnd"
              @transitioncancel="onHelpTransitionCancel"
            >
              <HelpPage :view-mode="viewMode" @close="closeHelp" />
            </div>
          </div>
        </div>
      </div>
    </WindowPresentation>
    <CaptureDelivery />
    <ShortcutConflictDialog
      v-model:visible="showShortcutNotice"
      @open-settings="showSettings = true"
    />
    <SettingsPanel
      v-if="showSettings"
      v-model:visible="showSettings"
      :view-mode="viewMode"
      @blur-release="releaseSettingsBlur"
      @check-update="checkForUpdates"
    />
    <FirstUseNoticeDialog
      v-if="showFirstUseNotice"
      :queue="firstUseQueue"
      :visible="showFirstUseNotice"
      @completed="onFirstUseCompleted"
    />
    <UpdateDialog
      v-model:visible="showUpdateDialog"
      :queue="updateQueue"
      :checking="updateChecking"
      :result="updateResult"
      @retry="checkForUpdates"
    />
    <RemoteNoticeDialog
      v-if="showRemoteNoticeDialog && pendingRemoteNotices.length"
      :queue="noticeQueue"
      :notices="pendingRemoteNotices"
      @close="closeRemoteNoticeDialog"
      @acknowledged="onRemoteNoticeAcknowledged"
    />
    <HolidayDataNoticeDialog
      v-if="pendingHolidayDataNotice"
      :queue="holidayQueue"
      :visible="showHolidayDataNoticeDialog"
      :year="pendingHolidayDataNotice.year"
      @dismiss="dismissHolidayDataNotice()"
      @open-settings="dismissHolidayDataNotice({ openSettingsAfter: true })"
    />
    <DailyReportDialog v-model:visible="showDailyReportDialog" :queue="reportQueue" />
    <EditingDraftDialog />
    <MessageToast />
  </div>
</template>

<style scoped>
.month-root,
.month-scene {
  width: 100vw;
  height: 100vh;
  overflow: hidden;
  border-radius: var(--window-radius);
}

.month-root.day-surface {
  background: #f6f6f8;
}
.month-root.yunzhao-shell,
.month-root.yunzhao-shell .month-scene {
  border-radius: 8px;
  overflow: hidden;
  background: #f6f6f8;
}
.month-root.yunzhao-shell.is-peek {
  background: transparent;
}
.calendar-peek {
  position: absolute;
  inset: 0;
  z-index: var(--z-local-top);
  display: flex;
  align-items: center;
  justify-content: center;
  border: 0;
  border-radius: 0 0 6px 6px;
  background: #f6f6f8;
  cursor: pointer;
}
.calendar-peek.is-left {
  border-radius: 0 6px 6px 0;
}
.calendar-peek.is-right {
  border-radius: 6px 0 0 6px;
}
.calendar-peek:hover {
  background: color-mix(in srgb, #f6f6f8 92%, #1d1d1f 8%);
}
.calendar-peek span {
  width: 14px;
  height: 2px;
  border-radius: 999px;
  background: #1d1d1f;
  opacity: 0.28;
}
.calendar-peek.is-left span,
.calendar-peek.is-right span {
  width: 2px;
  height: 14px;
}

.month-root {
  position: relative;
  background-color: rgb(var(--bg-color) / var(--window-opacity));
}

.month-scene {
  position: relative;
  z-index: var(--z-local-content);
  display: flex;
  flex-direction: column;
  transition: filter 180ms ease;
}

.month-content {
  flex: 1;
  min-width: 0;
  min-height: 0;
  transition: filter 180ms ease;
}

.month-content-stage {
  position: relative;
  display: flex;
  flex: 1;
  min-height: 0;
  overflow: hidden;
}

.month-template-wrapper,
.month-help-wrapper {
  position: absolute;
  z-index: var(--z-global-workspace);
  inset: 0;
  overflow: hidden;
  border-radius: var(--window-radius);
  pointer-events: none;
}

.month-template-wrapper.is-interactive,
.month-help-wrapper.is-interactive {
  pointer-events: auto;
}

.month-template-panel,
.month-help-panel {
  position: absolute;
  inset: 0;
  display: flex;
  min-width: 0;
  min-height: 0;
  overflow: hidden;
  border-radius: inherit;
  background-color: var(--surface-panel);
  box-shadow: -12px 0 36px rgba(0, 0, 0, 0.16);
  transform: translateX(100%);
  transition: transform 360ms var(--ease-standard);
  will-change: transform;
}

.month-template-panel.active,
.month-help-panel.active {
  transform: translateX(0);
}

.month-wallpaper {
  position: absolute;
  z-index: var(--z-local-base);
  inset: 0;
  overflow: hidden;
  border-radius: inherit;
  pointer-events: none;
}

.month-wallpaper-image {
  position: absolute;
  inset: calc(var(--wallpaper-blur) * -2);
  background-position: center;
  background-repeat: no-repeat;
  background-size: cover;
  filter: blur(var(--wallpaper-blur));
}

.month-wallpaper-enter-active,
.month-wallpaper-leave-active {
  transition: opacity 180ms ease;
}

.month-wallpaper-enter-from,
.month-wallpaper-leave-to {
  opacity: 0;
}
</style>

<style>
html[data-yunzhao-dock],
html[data-yunzhao-dock] body {
  overflow: hidden;
  background: #f6f6f8;
}
html[data-yunzhao-dock] .month-root.yunzhao-shell,
html[data-yunzhao-dock] .month-root.yunzhao-shell .month-scene {
  width: var(--yunzhao-home-w);
  height: var(--yunzhao-home-h);
  min-width: 0;
  min-height: 0;
  max-width: none;
  transform: translate3d(var(--yunzhao-shift-x, 0px), var(--yunzhao-shift-y, 0px), 0);
  border-radius: 0;
}
.month-root.yunzhao-shell .month-workspace__calendar-body {
  flex-direction: column;
  overflow: hidden;
}
.month-root.yunzhao-shell .month-grid {
  width: 100%;
  min-width: 0;
  min-height: 0;
  height: 100%;
  --calendar-row-min-height: 0px;
}
.month-root.yunzhao-shell .month-grid__weeks {
  grid-template-rows: repeat(var(--calendar-rows), minmax(0, 1fr));
}
.month-root.yunzhao-shell .month-toolbar {
  min-height: 36rem;
  grid-template-areas: none;
  grid-template-columns: minmax(0, 1fr) auto minmax(0, 1fr);
  grid-template-rows: auto;
  row-gap: 0;
}
.month-root.yunzhao-shell .month-toolbar__leading,
.month-root.yunzhao-shell .month-toolbar__navigation,
.month-root.yunzhao-shell .month-toolbar__trailing {
  grid-area: auto;
}
.month-root.yunzhao-shell .month-week__events {
  top: 30rem;
  bottom: 16rem;
}
.month-root.yunzhao-shell .month-event-bar {
  height: 16rem;
  font-size: calc(var(--fs-month-event) * 0.85);
  transform: translateY(calc(var(--event-lane) * 17rem));
}
.month-root.yunzhao-shell .month-day-cell,
.month-root.yunzhao-shell .month-week {
  min-height: 0;
  overflow: hidden;
}
</style>
