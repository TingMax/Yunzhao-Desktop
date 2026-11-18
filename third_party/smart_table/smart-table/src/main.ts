import { createApp } from "vue";
import { createPinia } from "pinia";
import ElementPlus from "element-plus";
import * as ElementPlusIconsVue from "@element-plus/icons-vue";
import "element-plus/dist/index.css";
import "./styles/yunzhao-theme.css";
import VxeTable from "vxe-table";
import "vxe-table/lib/style.css";

import App from "./App.vue";
import router from "./router";
import i18n from "./i18n";
import { db } from "./db";
import { initDayjsPlugins } from "./utils/timezone";
import { getTokenRefreshService } from "./services/tokenRefreshService";
import { checkAndPromptUpdate } from "./utils/versionCheck";

// 初始化时区插件
initDayjsPlugins();

// 将 db 暴露到全局，方便调试
if (typeof window !== "undefined") {
  (window as any).db = db;
}

const app = createApp(App);

app.use(createPinia());
app.use(router);
app.use(i18n);
app.use(ElementPlus);
app.use(VxeTable);

for (const [key, component] of Object.entries(ElementPlusIconsVue)) {
  app.component(key, component);
}

app.mount("#app");

// 初始化Token自动续期服务
const tokenRefreshService = getTokenRefreshService();
tokenRefreshService.start();

// 版本检测（app 挂载后调用，非阻塞，后台请求 /version.json）
// 版本不一致时弹出 ElMessageBox 确认对话框，用户确认后强制刷新
void checkAndPromptUpdate();

// 首屏由页面自己的底色撑住，不再盖一层加载动画。
router.isReady().catch(() => undefined);

// 应用销毁时清理资源
window.addEventListener('beforeunload', () => {
  tokenRefreshService.destroy();
});
