# 代码地图

读代码从这份文件开始。每个模块一句话说明，然后是入口和关键文件。

## 一、四个程序

| 程序 | 语言 | 源码位置 | 产物 |
|---|---|---|---|
| 桌面核心 | C++20 | `native/core/` | `Yunzhao.exe` |
| 设置中心 | C# / .NET 10 | `settings/Yunzhao.Settings/` | `Settings/Yunzhao.Settings.exe` |
| 日历课表 | JS / Vue (Electron) | `third_party/yunzhao_note2/` | `Calendar/YunzhaoNote.exe` |
| 灵动岛 | Rust (Tauri) | `third_party/fly-island/` | `Island/focusd-island.exe` |

后两个是第三方独立程序，通过命令行和环境变量接入，不共享内存。

## 二、核心（C++）的四个模块

```
native/core/
├── main.cpp            进程入口：消息循环、托盘、模块装配
├── bar.cpp/h           任务栏窗口：输入处理、渲染调度
├── settings.cpp/h      配置读写
├── settings_server.cpp 设置中心的命名管道服务
│
├── render/             画玻璃（20 文件 / 3.4k 行）
├── ui/                 各个弹出面板（25 文件 / 18.6k 行）
└── shell/              和 Windows 系统打交道（49 文件 / 9k 行）
```

### render/ — 渲染管线

任务栏的玻璃效果全部在这里。

| 文件 | 职责 |
|---|---|
| `glass_renderer.cpp/h` | 玻璃材质着色器、交换链、DirectComposition 挂载。**最核心** |
| `bar_painter.cpp/h` | 任务栏内容：图标、文字、放大波浪、启动弹跳 |
| `capture.cpp/h` | 屏幕采集（DXGI Desktop Duplication 或 Windows.Graphics.Capture） |
| `desktop_backdrop.cpp/h` | 桌面壁纸合成，作为玻璃的背景源 |
| `glass_panel.cpp/h` | 弹出面板的玻璃表面参数 |
| `gpu.cpp/h` | D3D11 设备创建 |
| `ui_font.cpp/h` | MiSans 字体加载与文字排版 |
| `wifi_glyph.cpp/h` | 自绘 Wi-Fi 图标 |
| `shaders/*.hlsl` | 着色器源码：`glass` 玻璃、`backdrop_edge` 边缘折射、`compare` 回声检测 |
| `continuous_corners.h` | 连续圆角（squircle）轮廓数学 |
| `corner_profile.h`, `callout_profile.h`, `round_stroke.h` | 形状定义 |

**从 main.cpp 到玻璃绘制的调用链**：
```
main.cpp WM_PAINT/WM_TIMER
  → bar.cpp RenderSession()        ← 渲染线程主循环
      → capture.cpp Acquire()      取屏幕帧
      → desktop_backdrop.cpp       桌面背景
      → glass_renderer.cpp Present()  着色器画玻璃
          → bar_painter.cpp Draw()    画图标文字
      → SwapChain.Present()
```

### ui/ — 弹出面板

每个文件是一种面板，都继承 `surface.h` 的 `Surface`（玻璃表面基类）。

| 文件 | 面板 |
|---|---|
| `surface.cpp/h` | 基类：玻璃表面、输入区域、动画 |
| `start_menu.cpp/h` | 开始菜单（应用列表、搜索、固定项） |
| `desktop_widgets.cpp/h` | 桌面组件（日历、时钟、电池等） |
| `notifications.cpp/h` | 通知卡片 |
| `tray_panel.cpp/h` | 托盘面板（系统托盘图标列表） |
| `control_center.cpp/h` | 控制中心（音量、亮度） |
| `stage_manager.cpp/h` | 台前调度（窗口侧边栏） |
| `calendar_panel.cpp/h` | 日历弹窗 |
| `glass_menu.cpp/h` | 右键菜单 |
| `window_preview.cpp/h` | 悬停预览窗口画面 |
| `window_snapshot.cpp/h` | 抓取窗口缩略图 |
| `widget_faces.cpp/h` | 组件表盘绘制 |

### shell/ — Windows 系统集成

| 文件 | 职责 |
|---|---|
| `task_model.cpp/h` | Windows 任务栏数据模型：窗口列表、图标、分组 |
| `app_catalog.cpp/h` | 应用目录（开始菜单快捷方式、Store 应用） |
| `app_processes.cpp/h` | 进程查询 |
| `app_registration.cpp/h` | 注册为默认桌面、开始菜单项 |
| `system_chrome.cpp/h` | **接管 Windows 自身任务栏**、勿扰模式 |
| `system_status.cpp/h` | 音量、亮度、电池状态读取 |
| `displays.cpp/h` | 多显示器管理 |
| `win_key.cpp/h` | Win 键钩子（当前禁用） |
| `tray_model.cpp/h` | 系统托盘图标枚举 |
| `jump_list.cpp/h` | 跳转列表（任务栏右键菜单） |
| `shell_verbs.cpp/h` | Shell 操作（打开、属性、以管理员运行） |
| `update_checker.cpp/h` | 自动更新检查（12 小时间隔） |
| `notification_listener.cpp/h` | 监听 Windows 通知 |
| `weather.cpp/h`, `lunar.cpp/h` | 天气、农历 |
| `wifi.cpp/h`, `radios.cpp/h` | 网络、蓝牙无线电 |
| `audio_devices.cpp/h` | 音频设备枚举 |
| `bluetooth_batteries.cpp/h` | 蓝牙设备电量 |
| `wechat_source.cpp/h`, `feishu_source.cpp/h` | 微信/飞书消息读取 |
| `logon_task.cpp/h` | 开机启动 |
| `start_layout.cpp/h` | 开始菜单布局持久化 |
| `projection.cpp/h` | 投屏 |
| `uia_client.h` | UI Automation 客户端 |

### 根目录其余文件

| 文件 | 职责 |
|---|---|
| `account.cpp/h` | 账号登录与云端同步 |
| `clock.h` | 单调时钟工具 |
| `log.cpp/h` | 日志写入 |
| `motion.h` | 缓动与弹簧动画数学 |
| `os.h` | Windows 版本判断 |
| `resource.h`, `core.rc` | 图标、版本号资源 |
| `core.manifest` | 应用清单（DPI 感知等） |

## 三、设置中心（C#）

```
settings/Yunzhao.Settings/
├── Program.cs           入口（也是无界面更新工作进程的入口）
├── App.xaml.cs          单实例、激活已有窗口
├── MainWindow.xaml      主窗口布局
├── MainWindow.xaml.cs   主逻辑、页面切换
├── MainWindow.*.cs      各设置页：Island / Widgets / Displays / Pins / Stage …
├── CoreClient.cs        与核心的命名管道客户端
├── BarSettings.cs       配置模型（与 C++ settings.h 对应）
├── ReleaseInfo.cs       读取更新清单（HTTP）
├── ReleaseInstaller.cs  下载、校验、替换安装
├── BitsTransfer.cs      BITS 后台下载封装
└── Controls.cs, SliderTile.xaml …  自定义控件
```

## 四、构建与发布

| 脚本 | 用途 |
|---|---|
| `tools/build.ps1` | 构建核心 + 设置，输出到 `dist/Yunzhao/` |
| `tools/Update.ps1` | 安装更新的辅助脚本（带进度窗口） |
| `tools/Uninstall.ps1` | 卸载 |
| `tools/yunzhao.wxs` | MSI 安装包定义（WiX） |
| `site/` | 官网与更新清单 `latest.json` |

构建命令：
```powershell
tools\build.ps1 -Development   # 开发版，不注册开机启动
tools\build.ps1                # 正式版
```

## 五、运行数据

全部在 `%LOCALAPPDATA%\Yunzhao\`：

| 文件 | 内容 |
|---|---|
| `settings.json` | 主配置 |
| `system.json` | 系统任务栏接管状态（崩溃恢复用） |
| `widgets.json` | 桌面组件布局 |
| `start.json` | 开始菜单布局 |
| `stage.json` | 台前调度状态 |
| `account.json` | 登录信息 |
| `logs/core.log` | 运行日志，含每分钟性能统计 |
| `logs/settings.log` | 设置中心崩溃记录 |

## 六、改代码时的注意点

**渲染相关的坑**（这些踩过）：

1. 交换链的帧延迟等待对象是**自动复位**的。在渲染循环的 `WaitForMultipleObjects` 里等它，信号会被消耗掉，随后 `Ready()` 永远返回 false，画面冻结但点击仍有效。只能在 `Ready()` 里查。
2. `desktop.Update()` 一帧只能决定一次模式。两处调用传入不同模式会让壁纸线程每帧销毁重建。
3. `StopWallpaper()` 不要在渲染线程上 `join()`，壁纸循环轮询 3 秒，会把帧率打到 5Hz。

**跨模块通信**：

- C++ ↔ C#：命名管道，协议见 `docs/SETTINGS-PROTOCOL.md`
- C++ 内部通知：`WM_APP + n` 自定义消息（见 `main.cpp` 顶部常量）
- 修改 `settings.h` 的字段时，同步改 `BarSettings.cs`，否则设置页会丢值
