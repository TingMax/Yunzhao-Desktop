//! WeCom reminders from the taskbar flash. Message text is not available.

mod launch;
mod source;

use launch::open_wecom as launch_wecom;
use serde::Serialize;
use source::{ReminderListener, ReminderSignal};
use std::{
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc, Arc, Mutex, OnceLock,
    },
    thread,
    time::{Duration, SystemTime, UNIX_EPOCH},
};
use tauri::{AppHandle, Emitter, Manager};

static SERVICE: OnceLock<Arc<ReminderService>> = OnceLock::new();
const MAIN_WINDOW_LABEL: &str = "main";
const REMINDER_EVENT: &str = "wecom-reminder-changed";
const FLASH_QUIET_MS: u64 = 4_000;

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ReminderSnapshot {
    revision: u64,
    enabled: bool,
    status: String,
    pending: bool,
    last_attention_at: Option<u64>,
    error: Option<String>,
}

impl Default for ReminderSnapshot {
    fn default() -> Self {
        Self {
            revision: 0,
            enabled: false,
            status: "disabled".into(),
            pending: false,
            last_attention_at: None,
            error: None,
        }
    }
}

#[derive(Default)]
struct ReminderState {
    snapshot: ReminderSnapshot,
    generation: u64,
    last_signal_at: Option<u64>,
}

impl ReminderState {
    fn clear(&mut self) {
        self.snapshot.pending = false;
        self.snapshot.revision += 1;
    }

    fn set_enabled(&mut self, enabled: bool) -> bool {
        if self.snapshot.enabled == enabled {
            return false;
        }
        self.generation += 1;
        self.last_signal_at = None;
        self.snapshot = ReminderSnapshot {
            revision: self.snapshot.revision + 1,
            enabled,
            status: if enabled { "waiting" } else { "disabled" }.into(),
            ..Default::default()
        };
        true
    }

    fn attention(&mut self, generation: u64, now: u64) -> bool {
        if !self.snapshot.enabled || self.generation != generation {
            return false;
        }
        if self.snapshot.pending && self.last_signal_at.is_some_and(|at| now.saturating_sub(at) < FLASH_QUIET_MS) {
            self.last_signal_at = Some(now);
            return false;
        }
        self.last_signal_at = Some(now);
        self.snapshot.pending = true;
        self.snapshot.last_attention_at = Some(now);
        self.snapshot.revision += 1;
        true
    }
}

struct ReminderService {
    app: AppHandle,
    state: Mutex<ReminderState>,
    wake: mpsc::SyncSender<()>,
    stopping: AtomicBool,
}

pub fn init(app: &AppHandle) -> Result<(), String> {
    if SERVICE.get().is_some() {
        return Ok(());
    }
    let (wake, receiver) = mpsc::sync_channel(1);
    let service = Arc::new(ReminderService {
        app: app.clone(),
        state: Mutex::new(ReminderState::default()),
        wake,
        stopping: AtomicBool::new(false),
    });
    let worker = service.clone();
    thread::Builder::new()
        .name("wecom-reminders".into())
        .spawn(move || monitor(worker, receiver))
        .map_err(|error| format!("无法启动企业微信提醒监听：{error}"))?;
    SERVICE.set(service).map_err(|_| "企业微信提醒服务已启动。".to_string())
}

pub(crate) fn shutdown() {
    if let Some(service) = SERVICE.get() {
        service.stopping.store(true, Ordering::Release);
        let _ = service.wake.try_send(());
    }
}

fn service() -> Result<&'static Arc<ReminderService>, String> {
    SERVICE.get().ok_or_else(|| "企业微信提醒服务尚未就绪。".into())
}

fn now_ms() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|time| time.as_millis() as u64).unwrap_or(0)
}

fn publish(service: &ReminderService, snapshot: ReminderSnapshot) {
    let visible = service.app.get_webview_window(MAIN_WINDOW_LABEL).is_some_and(|window| window.is_visible().unwrap_or(false));
    let payload = serde_json::json!({"snapshot": snapshot, "windowVisible": visible});
    let _ = service.app.emit_to(crate::settings_window::WINDOW_LABEL, REMINDER_EVENT, payload.clone());
    let _ = service.app.emit_to(MAIN_WINDOW_LABEL, REMINDER_EVENT, payload);
}

fn configure(change: impl FnOnce(&mut ReminderState) -> bool) -> Result<ReminderSnapshot, String> {
    let service = service()?;
    let (snapshot, changed) = {
        let mut state = service.state.lock().map_err(|_| "企业微信提醒状态不可用。")?;
        let changed = change(&mut state);
        (state.snapshot.clone(), changed)
    };
    if changed {
        publish(service, snapshot.clone());
        let _ = service.wake.try_send(());
    }
    Ok(snapshot)
}

fn connection(service: &ReminderService, generation: u64, error: Option<String>) {
    let snapshot = {
        let Ok(mut state) = service.state.lock() else { return };
        if !state.snapshot.enabled || state.generation != generation {
            return;
        }
        let status = if error.is_some() { "error" } else { "listening" };
        if state.snapshot.status == status && state.snapshot.error == error {
            return;
        }
        state.snapshot.status = status.into();
        state.snapshot.error = error;
        state.snapshot.revision += 1;
        state.snapshot.clone()
    };
    publish(service, snapshot);
}

fn monitor(service: Arc<ReminderService>, receiver: mpsc::Receiver<()>) {
    while !service.stopping.load(Ordering::Acquire) {
        let (enabled, generation) = match service.state.lock() {
            Ok(state) => (state.snapshot.enabled, state.generation),
            Err(_) => return,
        };
        if !enabled {
            if receiver.recv().is_err() { return; }
            continue;
        }
        let (send_signal, signals) = mpsc::channel();
        let listener = ReminderListener::new(move |event| {
            if event == ReminderSignal::Attention {
                let _ = send_signal.send(event);
            }
        });
        match listener {
            Ok(listener) => {
                connection(&service, generation, None);
                let still = || {
                    !service.stopping.load(Ordering::Acquire)
                        && service.state.lock().is_ok_and(|state| state.snapshot.enabled && state.generation == generation)
                };
                while still() {
                    listener.poll();
                    for _event in signals.try_iter() {
                        let now = now_ms();
                        let snapshot = {
                            let Ok(mut state) = service.state.lock() else { continue };
                            if !state.attention(generation, now) { continue; }
                            state.snapshot.clone()
                        };
                        publish(&service, snapshot);
                    }
                    if receiver.recv_timeout(Duration::from_millis(50)) == Err(mpsc::RecvTimeoutError::Disconnected) {
                        return;
                    }
                }
            }
            Err(error) => {
                connection(&service, generation, Some(error));
                if receiver.recv_timeout(Duration::from_secs(5)) == Err(mpsc::RecvTimeoutError::Disconnected) {
                    return;
                }
            }
        }
    }
}

#[tauri::command]
pub fn get_wecom_reminder_state() -> Result<ReminderSnapshot, String> {
    service()?.state.lock().map(|state| state.snapshot.clone()).map_err(|_| "企业微信提醒状态不可用。".into())
}

#[tauri::command]
pub fn clear_wecom_reminders() -> Result<ReminderSnapshot, String> {
    configure(|state| { state.clear(); true })
}

#[tauri::command]
pub fn set_wecom_reminders_enabled(enabled: bool) -> Result<ReminderSnapshot, String> {
    configure(|state| state.set_enabled(enabled))
}

#[tauri::command]
pub async fn open_wecom() -> Result<(), String> {
    tauri::async_runtime::spawn_blocking(launch_wecom)
        .await
        .map_err(|error| format!("打开企业微信的线程失败：{error}"))?
}

macro_rules! tauri_commands {
    ($next:ident [$($done:tt)*] $($rest:tt)*) => {
        $next!([$($done)*
            crate::wecom_reminders::get_wecom_reminder_state,
            crate::wecom_reminders::clear_wecom_reminders,
            crate::wecom_reminders::set_wecom_reminders_enabled,
            crate::wecom_reminders::open_wecom,
        ] $($rest)*)
    };
}
pub(crate) use tauri_commands;
