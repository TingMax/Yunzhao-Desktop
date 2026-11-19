mod bounds;
pub(crate) mod sizing;

use sizing::Size;
use std::{thread, time::Duration};
use tauri::{
    window::Color, AppHandle, Emitter, Manager, Monitor, PhysicalPosition, WebviewWindow, WebviewWindowBuilder, WindowEvent,
};

pub const WINDOW_LABEL: &str = "settings";
// Mirrored by SETTINGS_OPEN_REQUESTED_EVENT in src/windows/settings/api.ts.
const SETTINGS_OPEN_REQUESTED_EVENT: &str = "settings-open-requested";

/// Shows a new settings window even if its page never reports in (a load failure),
/// so it cannot stay invisible. Normally the page asks well before this.
const SHOW_FALLBACK_DELAY: Duration = Duration::from_secs(2);

/// The settings window is declared with `create: false` and built on demand,
/// so a closed window does not keep a webview running in the background.
///
/// A new window starts hidden and the page shows it (show_settings_window) after
/// its first paint; shown at once, it sat empty while the webview loaded. It always
/// opens at the configured size, shrunk to fit a small screen, at the place it had
/// when last closed (see `bounds`); a first window is centered on the island's screen.
pub fn open(app: &AppHandle) -> Result<(), String> {
    if let Some(window) = app.get_webview_window(WINDOW_LABEL) {
        let _ = window.unminimize();
        return reveal(&window);
    }

    let config = app
        .config()
        .app
        .windows
        .iter()
        .find(|window| window.label == WINDOW_LABEL)
        .ok_or_else(|| "Settings window config was not found.".to_string())?
        .clone();
    let mut config = config;
    let min = Size {
        width: config.min_width.unwrap_or(0.0),
        height: config.min_height.unwrap_or(0.0),
    };
    let monitors = app.available_monitors().unwrap_or_default();
    let saved = bounds::load(app);
    let saved_screen = saved.and_then(|saved| bounds::screen_of(saved, &monitors));
    let screen = saved_screen.cloned().or_else(|| island_screen(app));
    let screen_size = screen.as_ref().map(logical_work_area);

    let wanted = Size {
        width: config.width,
        height: config.height,
    };
    let size = match screen_size {
        Some(screen_size) => sizing::fit(wanted, min, screen_size),
        None => Size {
            width: wanted.width.max(min.width),
            height: wanted.height.max(min.height),
        },
    };
    config.width = size.width;
    config.height = size.height;

    let window = WebviewWindowBuilder::from_config(app, &config)
        .map_err(|error| error.to_string())?
        .visible(false)
        .build()
        .map_err(|error| error.to_string())?;
    if yunzhao_host() {
        let _ = window.set_title("Yunzhao 灵动岛");
        let _ = window.set_decorations(false);
        let _ = window.set_skip_taskbar(true);
        let _ = window.set_always_on_top(false);
        // Yunzhao's settings page owns this window. If that owner is gone, a frameless
        // window would sit on the desktop with no close button, so hide it.
        let watch = window.clone();
        thread::spawn(move || {
            loop {
                thread::sleep(Duration::from_millis(400));
                let Ok(hwnd) = watch.hwnd() else { break };
                let visible = watch.is_visible().unwrap_or(false);
                if visible && !crate::win32::window::has_owner(hwnd) {
                    let _ = watch.hide();
                }
            }
        });
    }
    match (saved, saved_screen, screen.as_ref()) {
        (Some(saved), Some(_), _) => {
            let _ = window.set_position(PhysicalPosition::new(saved.x, saved.y));
        }
        (_, _, Some(screen)) => {
            let _ = window.set_position(centered(screen, size));
        }
        _ => {}
    }
    let closing = window.clone();
    window.on_window_event(move |event| {
        if matches!(event, WindowEvent::CloseRequested { .. }) {
            bounds::save(&closing);
            // Tearing down the webview takes a moment, during which the window would stay
            // on screen; hide it first so the close button takes effect at once.
            let _ = closing.hide();
        }
    });

    if !yunzhao_host() {
        thread::spawn(move || {
            thread::sleep(SHOW_FALLBACK_DELAY);
            if !window.is_visible().unwrap_or(true) {
                let _ = reveal(&window);
            }
        });
    }
    Ok(())
}

/// The screen the island is on, so settings open where the user is looking;
/// the primary screen when the island has none.
fn island_screen(app: &AppHandle) -> Option<Monitor> {
    app.get_webview_window(crate::island::WINDOW_LABEL)
        .and_then(|island| island.current_monitor().ok().flatten())
        .or_else(|| app.primary_monitor().ok().flatten())
}

fn logical_work_area(monitor: &Monitor) -> Size {
    let area = monitor.work_area().size;
    Size {
        width: f64::from(area.width) / monitor.scale_factor(),
        height: f64::from(area.height) / monitor.scale_factor(),
    }
}

/// Where a window of this logical size sits centered in the screen's work area.
fn centered(monitor: &Monitor, size: Size) -> PhysicalPosition<i32> {
    let area = monitor.work_area();
    let scale = monitor.scale_factor();
    let offset = |available: u32, length: f64| {
        ((f64::from(available) - length * scale) / 2.0).max(0.0) as i32
    };
    PhysicalPosition::new(
        area.position.x + offset(area.size.width, size.width),
        area.position.y + offset(area.size.height, size.height),
    )
}

/// Saves the open settings window's size and place before the app quits, which
/// closes it without a close request.
pub fn remember_bounds(app: &AppHandle) {
    if let Some(window) = app.get_webview_window(WINDOW_LABEL) {
        bounds::save(&window);
    }
}

/// Quits the island from the settings window's About page, as the tray's Quit does.
#[tauri::command]
pub fn quit_island(app: AppHandle) {
    remember_bounds(&app);
    app.exit(0);
}

fn yunzhao_host() -> bool {
    std::env::var_os("YUNZHAO_HOST").is_some()
}

fn reveal(window: &WebviewWindow) -> Result<(), String> {
    // Yunzhao keeps this window hidden until its own page places it.
    if yunzhao_host() {
        return Ok(());
    }
    window.show().map_err(|error| error.to_string())?;
    crate::system::window_focus::focus_window(window)
}

// Async keeps window creation off the IPC thread, which can deadlock on Windows.
#[tauri::command]
pub async fn open_settings_window(app: AppHandle) -> Result<(), String> {
    open(&app)
}

/// Asks the island to open settings where the user wants them: as its own page or in
/// this window (the choice is the island's, saved in its storage). Shows the island first.
pub fn request(app: &AppHandle) -> Result<(), String> {
    crate::island::show_island(app)?;
    app.emit_to("main", SETTINGS_OPEN_REQUESTED_EVENT, ())
        .map_err(|error| error.to_string())
}

/// Settings move back into the island: it opens its settings page, and this window closes.
#[tauri::command]
pub fn dock_settings_window(app: AppHandle) -> Result<(), String> {
    request(&app)?;
    if let Some(window) = app.get_webview_window(WINDOW_LABEL) {
        bounds::save(&window);
        window.close().map_err(|error| error.to_string())?;
    }
    Ok(())
}

/// A notice Yunzhao wrote for the island to show. Absent when there is nothing new.
#[derive(serde::Serialize)]
pub struct HostNotice {
    pub app: String,
    pub title: String,
    pub body: String,
    pub at: i64,
}

fn yunzhao_dir() -> Option<std::path::PathBuf> {
    let dir = std::env::var("LOCALAPPDATA").ok()?;
    Some(std::path::PathBuf::from(dir).join("Yunzhao"))
}

/// Settings written by the Yunzhao settings page. The island merges this into its own settings.
#[tauri::command]
pub fn read_host_island_settings() -> Option<serde_json::Value> {
    let text = std::fs::read_to_string(yunzhao_dir()?.join("island-ui.json")).ok()?;
    serde_json::from_str(&text).ok()
}

/// The island's current settings, so the Yunzhao page can show the same values.
#[tauri::command]
pub fn publish_host_island_settings(settings: serde_json::Value) -> Result<(), String> {
    let dir = yunzhao_dir().ok_or_else(|| "LOCALAPPDATA is not set.".to_string())?;
    std::fs::create_dir_all(&dir).map_err(|error| error.to_string())?;
    let text = serde_json::to_string(&settings).map_err(|error| error.to_string())?;
    std::fs::write(dir.join("island-ui-state.json"), text).map_err(|error| error.to_string())
}

#[tauri::command]
pub fn read_host_notice() -> Option<HostNotice> {
    let text = std::fs::read_to_string(yunzhao_dir()?.join("island-notice.json")).ok()?;
    let value: serde_json::Value = serde_json::from_str(&text).ok()?;
    Some(HostNotice {
        app: value.get("app").and_then(|item| item.as_str()).unwrap_or("").to_string(),
        title: value.get("title").and_then(|item| item.as_str()).unwrap_or("").to_string(),
        body: value.get("body").and_then(|item| item.as_str()).unwrap_or("").to_string(),
        at: value.get("at").and_then(|item| item.as_i64()).unwrap_or(0),
    })
}

/// Called by the settings page once it has painted, to show its hidden new window.
#[tauri::command]
pub async fn show_settings_window(app: AppHandle) -> Result<(), String> {
    let window = app
        .get_webview_window(WINDOW_LABEL)
        .ok_or_else(|| "Settings window was not found.".to_string())?;
    reveal(&window)
}

/// The page's background before it paints and while the window is resized, per colour scheme
/// (`--color-window` in src/styles/tokens.css).
const DARK_BACKGROUND: Color = Color(0x17, 0x19, 0x1d, 0xff);
const LIGHT_BACKGROUND: Color = Color(0xf3, 0xf3, 0xf6, 0xff);

/// Paints the settings window's title bar and background in the page's colour scheme. The window
/// theme is left alone: setting it also sets the preferred colour scheme of the one WebView2
/// profile every window shares, and "follow Windows" would then read the island's choice back.
#[tauri::command]
pub fn set_settings_window_theme(window: WebviewWindow, dark: bool) -> Result<(), String> {
    let hwnd = window.hwnd().map_err(|error| error.to_string())?;
    crate::win32::window::set_dark_title_bar(hwnd, dark);
    window
        .set_background_color(Some(if dark { DARK_BACKGROUND } else { LIGHT_BACKGROUND }))
        .map_err(|error| error.to_string())
}

/// This module's Tauri commands. `lib.rs` chains every module's list into one
/// `generate_handler!`, so adding a command only touches this file.
macro_rules! tauri_commands {
    ($next:ident [$($done:tt)*] $($rest:tt)*) => {
        $next!([$($done)*
            crate::settings_window::open_settings_window,
            crate::settings_window::read_host_notice,
            crate::settings_window::read_host_island_settings,
            crate::settings_window::publish_host_island_settings,
            crate::settings_window::show_settings_window,
            crate::settings_window::dock_settings_window,
            crate::settings_window::quit_island,
            crate::settings_window::set_settings_window_theme,
        ] $($rest)*)
    };
}
pub(crate) use tauri_commands;

#[cfg(test)]
mod tests {
    use super::bounds::title_bar_on_screen;

    #[test]
    fn a_saved_place_on_a_screen_is_reused() {
        let screens = [(0, 0, 1920, 1040), (1920, 0, 2560, 1400)];
        assert!(title_bar_on_screen(100, 80, &screens));
        assert!(title_bar_on_screen(2400, 200, &screens));
    }

    #[test]
    fn a_saved_place_off_every_screen_is_dropped() {
        let screens = [(0, 0, 1920, 1040)];
        assert!(!title_bar_on_screen(2400, 200, &screens));
        assert!(!title_bar_on_screen(1850, 200, &screens));
        assert!(!title_bar_on_screen(100, 1030, &screens));
        assert!(!title_bar_on_screen(-32000, -32000, &screens));
    }
}
