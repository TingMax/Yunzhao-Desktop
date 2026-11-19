use crate::win32::{
    message_window::{self, WindowMessages},
    process, shell_hook, window,
};
use std::{cell::RefCell, path::PathBuf};
use windows::Win32::{
    Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, WPARAM},
    UI::WindowsAndMessaging::{HSHELL_REDRAW, HSHELL_WINDOWACTIVATED, HSHELL_WINDOWCREATED},
};

const CLASS_NAME: &str = "YunzhaoWeComReminderSource";
const HSHELL_FLASH: u32 = HSHELL_REDRAW | 0x8000;
const HSHELL_RUDEAPPACTIVATED: u32 = HSHELL_WINDOWACTIVATED | 0x8000;
const EXECUTABLE: &str = "WXWork.exe";

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum ReminderSignal {
    Attention,
    Activated,
    ActivatedElsewhere,
}

struct ListenerContext {
    message: u32,
    callback: Box<dyn FnMut(ReminderSignal)>,
}

thread_local! {
    static CONTEXT: RefCell<Option<ListenerContext>> = const { RefCell::new(None) };
}

pub fn is_wecom_executable(path: &str) -> bool {
    path.rsplit(['\\', '/'])
        .next()
        .is_some_and(|name| name.eq_ignore_ascii_case(EXECUTABLE))
}

pub fn running_executable() -> Option<PathBuf> {
    process::running_executable(&[EXECUTABLE])
        .filter(|path| is_wecom_executable(path))
        .map(PathBuf::from)
}

fn matches_window(target: HWND) -> Result<bool, ()> {
    if target.is_invalid() {
        return Ok(false);
    }
    let path = process::image_path(process::window_process_id(target)).ok_or(())?;
    Ok(is_wecom_executable(&path))
}

#[derive(Clone, Copy)]
pub(crate) struct WeComWindow {
    pub(crate) handle: HWND,
    pub(crate) pid: u32,
    pub(crate) visible: bool,
}

pub(crate) fn existing_windows() -> Result<Vec<WeComWindow>, String> {
    let mut found = Vec::new();
    window::for_each_top_level_window(|target| {
        let process_id = process::window_process_id(target);
        let Some(path) = process::image_path(process_id) else {
            return;
        };
        if !is_wecom_executable(&path) {
            return;
        }
        let title = window::window_text(target, 64);
        if !title.contains("企业微信") && !title.eq_ignore_ascii_case("WeCom") && !title.eq_ignore_ascii_case("WXWork") {
            return;
        }
        found.push(WeComWindow {
            handle: target,
            pid: process_id,
            visible: window::is_visible(target),
        });
    })
    .map_err(|error| format!("无法查找企业微信窗口：{error}"))?;
    Ok(found)
}

pub(crate) fn preferred_window(windows: &[WeComWindow]) -> Option<WeComWindow> {
    windows.iter().copied().find(|item| item.visible).or_else(|| windows.first().copied())
}

struct ReminderWindow;

impl WindowMessages for ReminderWindow {
    fn handle(_: HWND, message: u32, kind: WPARAM, target: LPARAM) -> Option<LRESULT> {
        CONTEXT.with(|slot| {
            let Ok(mut context) = slot.try_borrow_mut() else {
                return;
            };
            let Some(context) = context.as_mut() else {
                return;
            };
            if message != context.message {
                return;
            }
            let signal = match kind.0 as u32 {
                HSHELL_FLASH => ReminderSignal::Attention,
                HSHELL_WINDOWACTIVATED | HSHELL_RUDEAPPACTIVATED => ReminderSignal::Activated,
                HSHELL_REDRAW | HSHELL_WINDOWCREATED => return,
                _ => return,
            };
            match matches_window(HWND(target.0 as *mut _)) {
                Ok(true) => {
                    if signal == ReminderSignal::Attention
                        && matches_window(window::foreground_window()).unwrap_or(false)
                    {
                        return;
                    }
                    (context.callback)(signal);
                }
                Ok(false) if signal == ReminderSignal::Activated => {
                    (context.callback)(ReminderSignal::ActivatedElsewhere);
                }
                Ok(false) | Err(()) => {}
            }
        });
        None
    }
}

pub struct ReminderListener {
    window: HWND,
    instance: HINSTANCE,
}

impl ReminderListener {
    pub fn new(callback: impl FnMut(ReminderSignal) + 'static) -> Result<Self, String> {
        if CONTEXT.with(|slot| slot.borrow().is_some()) {
            return Err("企业微信提醒监听器已在此线程运行。".into());
        }
        if running_executable().is_none() && existing_windows().ok().is_none_or(|windows| windows.is_empty()) {
            return Err("未找到企业微信。打开企业微信后会自动连接。".into());
        }
        let message = shell_hook::hook_message().map_err(|error| format!("无法注册企业微信提醒事件：{error}"))?;
        let instance = message_window::module_instance().map_err(|error| error.to_string())?;
        message_window::register_class::<ReminderWindow>(instance, CLASS_NAME).map_err(|error| error.to_string())?;
        let window = match message_window::create_tool_popup_window(instance, CLASS_NAME, "") {
            Ok(window) => window,
            Err(error) => {
                let _ = message_window::unregister_class(instance, CLASS_NAME);
                return Err(format!("无法创建企业微信提醒接收器：{error}"));
            }
        };
        CONTEXT.with(|slot| {
            *slot.borrow_mut() = Some(ListenerContext { message, callback: Box::new(callback) });
        });
        let listener = Self { window, instance };
        shell_hook::register_window(window).map_err(|error| format!("无法监听企业微信任务栏提醒：{error}"))?;
        Ok(listener)
    }

    pub fn poll(&self) {
        message_window::dispatch_pending_messages();
    }
}

impl Drop for ReminderListener {
    fn drop(&mut self) {
        let _ = shell_hook::deregister_window(self.window);
        let _ = message_window::destroy_window(self.window);
        let _ = message_window::unregister_class(self.instance, CLASS_NAME);
        CONTEXT.with(|slot| *slot.borrow_mut() = None);
    }
}
