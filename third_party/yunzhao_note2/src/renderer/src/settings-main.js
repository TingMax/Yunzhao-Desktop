import './assets/tokens.css'
import './utils/smoothScroll.js'
import { createApp, h } from 'vue'
import { createMessageProvider } from './composables/useMessage.js'
import SettingsPanel from './components/system/SettingsPanel.vue'
import { createDefaultSettings, VIEW_MODES } from '../../shared/settings-schema.js'
import { applySettingsSnapshot } from './utils/applySettingsSnapshot.js'

document.documentElement.classList.add('month-view')
applySettingsSnapshot({ values: createDefaultSettings(VIEW_MODES.MONTH) })

window.addEventListener('error', (event) => {
  const text = event.error?.stack || event.message
  document.body.insertAdjacentHTML('afterbegin', `<pre id="settings-boot-error">${text}</pre>`)
})

const Root = {
  setup() {
    createMessageProvider()
    return () => h(SettingsPanel, { visible: true, hosted: true, viewMode: VIEW_MODES.MONTH })
  }
}
const app = createApp(Root)
app.config.errorHandler = (error) => {
  const text = error?.stack || String(error)
  document.body.insertAdjacentHTML('afterbegin', `<pre id="settings-boot-error">${text}</pre>`)
}
app.mount('#app')
