export type WecomText = {
  desktopOnly: string;
  receive: string;
  open: string;
  clickToOpen: string;
  noReminders: string;
  openErrors: Record<"not-installed" | "no-main-window" | "restore-failed" | "launch-failed", string>;
  status: {
    disabled: string;
    waiting: string;
    listening: string;
    error: string;
  };
};

export const zhCN: WecomText = {
  desktopOnly: "请在 Windows 桌面版中使用企业微信提醒。",
  receive: "接收企业微信提醒",
  open: "打开企业微信",
  clickToOpen: "点击打开企业微信",
  noReminders: "暂无提醒",
  openErrors: {
    "not-installed": "没有找到企业微信。请先安装并登录。",
    "no-main-window": "企业微信已在运行，但没有找到主窗口。请从系统托盘打开。",
    "restore-failed": "没能把企业微信放到前面。请从任务栏打开。",
    "launch-failed": "没能启动企业微信。",
  },
  status: {
    disabled: "未开启",
    waiting: "正在连接",
    listening: "正在监听",
    error: "连接失败",
  },
};

export const en: WecomText = {
  desktopOnly: "WeCom reminders need the Windows desktop app.",
  receive: "Receive WeCom reminders",
  open: "Open WeCom",
  clickToOpen: "Click to open WeCom",
  noReminders: "No reminders",
  openErrors: {
    "not-installed": "WeCom was not found. Install it and sign in.",
    "no-main-window": "WeCom is running, but its main window was not found. Open it from the tray.",
    "restore-failed": "WeCom could not be brought forward. Open it from the taskbar.",
    "launch-failed": "WeCom could not be started.",
  },
  status: {
    disabled: "Off",
    waiting: "Connecting",
    listening: "Listening",
    error: "Connection failed",
  },
};
