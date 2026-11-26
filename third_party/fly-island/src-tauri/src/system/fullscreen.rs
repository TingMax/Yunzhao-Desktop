//! Watches for a full-screen app (usually a game) in the foreground, for the island's
//! game mode: it stops opening and taking the focus on hover, and can tuck away.

use serde::Serialize;
use std::{sync::Mutex, thread, time::Duration};
use tauri::{AppHandle, Emitter};
use windows::Win32::Foundation::RECT;

use crate::{island, win32};

// Mirrored by FOREGROUND_FULLSCREEN_CHANGED_EVENT in src/windows/island/api.ts. The
// payload is a ForegroundFullscreen.
pub(crate) const FOREGROUND_FULLSCREEN_CHANGED_EVENT: &str = "foreground-fullscreen-changed";

// Slow enough to cost nothing, quick enough that the island is out of the way soon
// after a game starts.
const POLL_INTERVAL: Duration = Duration::from_millis(800);

/// Mirrored by ForegroundFullscreen in src/windows/island/api.ts.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ForegroundFullscreen {
    /// A full-screen app (game, full-screen video, slides) is in the foreground.
    active: bool,
    /// It fills the monitor the island is on, so the island sits over it.
    on_island_monitor: bool,
}

static STATE: Mutex<ForegroundFullscreen> = Mutex::new(ForegroundFullscreen {
    active: false,
    on_island_monitor: false,
});

/// Shell windows that can sit in front covering a whole monitor without being an app in
/// full screen: the desktop (after Win+D or a click on it) and the taskbars.
const SHELL_CLASSES: [&str; 4] = ["Progman", "WorkerW", "Shell_TrayWnd", "Shell_SecondaryTrayWnd"];

/// Whether the foreground counts as a full-screen app. The shell's QUNS_BUSY is only a
/// guess, and some machines report it for long stretches with no full-screen app in front
/// (overlays, the desktop, a window that once went full screen), which held back every
/// message preview through game mode. So it must be confirmed by the foreground window
/// itself covering its whole monitor. Presentation settings are the user's own choice.
fn is_fullscreen_foreground(
    signal: win32::notification_state::FullscreenSignal,
    window: Option<RECT>,
    monitor: Option<RECT>,
    class: &str,
) -> bool {
    use win32::notification_state::FullscreenSignal;
    match signal {
        FullscreenSignal::None => false,
        FullscreenSignal::Presentation => true,
        FullscreenSignal::FullscreenApp => covers_monitor(window, monitor, class),
    }
}

fn read(app: &AppHandle) -> ForegroundFullscreen {
    let foreground = win32::window::foreground_window();
    let window = (!foreground.is_invalid() && win32::window::is_visible(foreground))
        .then(|| win32::window::window_rect(foreground))
        .flatten();
    let monitor = win32::window::monitor_rect(foreground);
    let class = win32::window::class_name(foreground);
    // A maximized app fills the work area but keeps its frame: the island may take the focus from
    // it (nothing drops to the desktop). Only a window that covers the screen without being
    // maximized is a full-screen app, exclusive or borderless alike.
    let maximized = win32::window::is_maximized(foreground);
    let active = is_fullscreen_foreground(win32::notification_state::fullscreen_signal(), window, monitor, &class)
        // The system only reports an exclusive full screen. A game in a borderless window is a
        // normal window as far as it is concerned (QUNS_NONE), yet it covers the screen just the
        // same and must not be pulled to the desktop either. Judge that case by the window itself.
        || (!maximized && covers_monitor(window, monitor, &class));
    let on_island_monitor = active
        && island::main_window(app)
            .and_then(|window| window.hwnd().map_err(|error| error.to_string()))
            // When the island cannot be found, assume it shares the screen.
            .map_or(true, |island| {
                win32::window::share_monitor(win32::window::foreground_window(), island)
            });
    ForegroundFullscreen { active, on_island_monitor }
}

/// The foreground window covers its whole monitor and is an app: a borderless full-screen game,
/// a full-screen video. Explorer's shell windows always cover the screen and are not apps.
fn covers_monitor(window: Option<RECT>, monitor: Option<RECT>, class: &str) -> bool {
    if SHELL_CLASSES.contains(&class) {
        return false;
    }
    matches!((window, monitor), (Some(window), Some(monitor))
        if window.left <= monitor.left && window.top <= monitor.top
            && window.right >= monitor.right && window.bottom >= monitor.bottom)
}

pub(crate) fn start(app: &AppHandle) {
    let app = app.clone();
    thread::spawn(move || loop {
        let next = read(&app);
        let changed = {
            let mut state = STATE.lock().unwrap_or_else(|poison| poison.into_inner());
            std::mem::replace(&mut *state, next) != next
        };
        if changed {
            let _ = app.emit(FOREGROUND_FULLSCREEN_CHANGED_EVENT, next);
        }
        thread::sleep(POLL_INTERVAL);
    });
}

/// The state at the last check.
#[tauri::command]
pub fn get_foreground_fullscreen() -> ForegroundFullscreen {
    *STATE.lock().unwrap_or_else(|poison| poison.into_inner())
}

/// A full-screen app on the island's monitor is in front right now. Showing the island then must
/// not take the focus: doing so drops the game out of its exclusive full screen onto the desktop,
/// which the user sees as being thrown out of the game on every message or lyric change.
pub(crate) fn foreground_fullscreen(app: &AppHandle) -> bool {
    // Ask the live windows as well as the polled state: a game can go full screen between polls,
    // and the island is often shown by a message that arrives in that gap.
    let live = read(app);
    let polled = *STATE.lock().unwrap_or_else(|poison| poison.into_inner());
    (live.active && live.on_island_monitor) || (polled.active && polled.on_island_monitor)
}

#[cfg(test)]
mod tests {
    use super::*;
    use win32::notification_state::FullscreenSignal;

    const MONITOR: RECT = RECT { left: 0, top: 0, right: 1920, bottom: 1080 };

    #[test]
    fn a_window_covering_its_monitor_confirms_the_shell() {
        let covering = RECT { left: -8, top: -8, right: 1928, bottom: 1088 };
        assert!(is_fullscreen_foreground(FullscreenSignal::FullscreenApp, Some(covering), Some(MONITOR), "UnityWndClass"));
        assert!(is_fullscreen_foreground(FullscreenSignal::FullscreenApp, Some(MONITOR), Some(MONITOR), "Chrome_WidgetWin_1"));
    }

    #[test]
    fn a_busy_shell_without_a_full_screen_window_is_not_game_mode() {
        let maximized = RECT { left: 0, top: 0, right: 1920, bottom: 1040 };
        assert!(!is_fullscreen_foreground(FullscreenSignal::FullscreenApp, Some(maximized), Some(MONITOR), "Notepad"));
        assert!(!is_fullscreen_foreground(FullscreenSignal::FullscreenApp, None, Some(MONITOR), "Notepad"));
        for shell in SHELL_CLASSES {
            assert!(!is_fullscreen_foreground(FullscreenSignal::FullscreenApp, Some(MONITOR), Some(MONITOR), shell));
        }
    }

    #[test]
    fn presentation_settings_count_and_a_quiet_shell_never_does() {
        assert!(is_fullscreen_foreground(FullscreenSignal::Presentation, None, None, ""));
        assert!(!is_fullscreen_foreground(FullscreenSignal::None, Some(MONITOR), Some(MONITOR), "UnityWndClass"));
    }

    /// A borderless game reports QUNS_NONE, so the signal alone misses it. `covers_monitor` is what
    /// catches it; this pins that judgement down, and that a maximized window is not one.
    #[test]
    fn a_borderless_full_screen_is_judged_by_its_window() {
        let covering = RECT { left: -8, top: -8, right: 1928, bottom: 1088 };
        assert!(covers_monitor(Some(covering), Some(MONITOR), "RiotWindowClass"));
        assert!(covers_monitor(Some(MONITOR), Some(MONITOR), "UnityWndClass"));
        // A maximized window keeps the work area's height: not full screen.
        let maximized = RECT { left: 0, top: 0, right: 1920, bottom: 1040 };
        assert!(!covers_monitor(Some(maximized), Some(MONITOR), "Chrome_WidgetWin_1"));
        // The shell covers the screen without being an app.
        for shell in SHELL_CLASSES {
            assert!(!covers_monitor(Some(MONITOR), Some(MONITOR), shell));
        }
        assert!(!covers_monitor(None, Some(MONITOR), "UnityWndClass"));
    }
}
