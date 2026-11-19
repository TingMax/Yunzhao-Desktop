//! The add-account window, centered on the monitor the island is using.

pub(crate) mod lock;

use tauri::{
    window::Color, AppHandle, Emitter, LogicalSize, Manager, Monitor, PhysicalPosition, Size, WebviewWindow,
    WebviewWindowBuilder,
};

pub const WINDOW_LABEL: &str = "vault-editor";
const DRAFT_EVENT: &str = "vault-entry-draft";
const RESET_EVENT: &str = "vault-editor-reset";
const WIDTH: f64 = 420.0;
const HEIGHT: f64 = 500.0;
const DARK_BACKGROUND: Color = Color(0x1c, 0x1c, 0x1e, 0xff);
const LIGHT_BACKGROUND: Color = Color(0xf5, 0xf5, 0xf7, 0xff);

#[derive(Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
struct VaultDraft {
    purpose: String,
    account: String,
    secret: String,
}

pub fn init(app: &tauri::AppHandle) {
    lock::start(app.clone());
}

pub fn open(app: &AppHandle) -> Result<(), String> {
    let window = if let Some(window) = app.get_webview_window(WINDOW_LABEL) {
        let _ = app.emit_to(WINDOW_LABEL, RESET_EVENT, ());
        window
    } else {
        let mut config = app
            .config()
            .app
            .windows
            .iter()
            .find(|window| window.label == WINDOW_LABEL)
            .ok_or_else(|| "Account window config was not found.".to_string())?
            .clone();
        config.width = WIDTH;
        config.height = HEIGHT;
        WebviewWindowBuilder::from_config(app, &config)
            .map_err(|error| error.to_string())?
            .visible(true)
            .build()
            .map_err(|error| error.to_string())?
    };
    place(&window, app);
    let _ = window.unminimize();
    window.show().map_err(|error| error.to_string())?;
    let _ = window.set_always_on_top(true);
    if let Ok(hwnd) = window.hwnd() {
        crate::win32::window::raise_topmost(hwnd);
    }
    let _ = crate::system::window_focus::focus_window(&window);
    Ok(())
}

fn island_monitor(app: &AppHandle) -> Option<Monitor> {
    app.get_webview_window(crate::island::WINDOW_LABEL)
        .and_then(|window| window.current_monitor().ok().flatten())
        .or_else(|| app.primary_monitor().ok().flatten())
}

fn place(window: &WebviewWindow, app: &AppHandle) {
    let Some(monitor) = island_monitor(app) else { return };
    let area = monitor.work_area();
    let scale = monitor.scale_factor();
    let x = area.position.x + ((f64::from(area.size.width) - WIDTH * scale) / 2.0).max(0.0) as i32;
    let y = area.position.y + ((f64::from(area.size.height) - HEIGHT * scale) / 2.0).max(0.0) as i32;
    let _ = window.set_size(Size::Logical(LogicalSize::new(WIDTH, HEIGHT)));
    let _ = window.set_position(PhysicalPosition::new(x, y));
}

#[tauri::command]
pub async fn open_vault_editor(app: AppHandle) -> Result<(), String> {
    open(&app)
}

#[tauri::command]
pub fn show_vault_editor(window: WebviewWindow, dark: bool, title: String) -> Result<(), String> {
    window
        .set_background_color(Some(if dark { DARK_BACKGROUND } else { LIGHT_BACKGROUND }))
        .map_err(|error| error.to_string())?;
    if !title.is_empty() {
        window.set_title(&title).map_err(|error| error.to_string())?;
    }
    window.show().map_err(|error| error.to_string())?;
    crate::system::window_focus::focus_window(&window)
}

#[tauri::command]
pub fn close_vault_editor(app: AppHandle) -> Result<(), String> {
    if let Some(window) = app.get_webview_window(WINDOW_LABEL) {
        window.close().map_err(|error| error.to_string())?;
    }
    Ok(())
}

#[tauri::command]
pub async fn copy_vault_login(account: String, secret: String) -> Result<(), String> {
    let text = format!("{account}\n{secret}");
    tauri::async_runtime::spawn_blocking(move || crate::clipboard_history::copy_transient_text(&text))
        .await
        .map_err(|error| error.to_string())?
}

#[tauri::command]
pub fn submit_vault_entry(app: AppHandle, purpose: String, account: String, secret: String) -> Result<(), String> {
    app.emit_to(crate::island::WINDOW_LABEL, DRAFT_EVENT, VaultDraft { purpose, account, secret })
        .map_err(|error| error.to_string())
}

macro_rules! tauri_commands {
    ($next:ident [$($done:tt)*] $($rest:tt)*) => {
        $next!([$($done)*
            crate::vault_editor::open_vault_editor,
            crate::vault_editor::show_vault_editor,
            crate::vault_editor::close_vault_editor,
            crate::vault_editor::copy_vault_login,
            crate::vault_editor::lock::remember_vault_password,
            crate::vault_editor::lock::read_vault_password,
            crate::vault_editor::submit_vault_entry,
        ] $($rest)*)
    };
}
pub(crate) use tauri_commands;
