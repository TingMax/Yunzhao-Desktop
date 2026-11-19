//! Tells the island when this Windows session locks, so saved account passwords
//! can be hidden until the view password is entered again.

use std::cell::RefCell;

use windows::{
    core::BOOL,
    Win32::Foundation::{HWND, LPARAM, LRESULT, WPARAM},
};

use super::message_window::{self, WindowMessages};

const CLASS_NAME: &str = "YunzhaoVaultLockWatch";
const WM_WTSSESSION_CHANGE: u32 = 0x02B1;
const WTS_SESSION_LOCK: usize = 7;
const NOTIFY_FOR_THIS_SESSION: u32 = 0;

thread_local! {
    static ON_LOCK: RefCell<Option<Box<dyn FnMut()>>> = const { RefCell::new(None) };
}

#[link(name = "wtsapi32")]
extern "system" {
    fn WTSRegisterSessionNotification(hwnd: HWND, flags: u32) -> BOOL;
}

struct LockWatch;

impl WindowMessages for LockWatch {
    fn handle(_window: HWND, message: u32, wparam: WPARAM, _lparam: LPARAM) -> Option<LRESULT> {
        if message == WM_WTSSESSION_CHANGE && wparam.0 == WTS_SESSION_LOCK {
            ON_LOCK.with(|slot| {
                if let Some(callback) = slot.borrow_mut().as_mut() {
                    callback();
                }
            });
            return Some(LRESULT(0));
        }
        None
    }
}

/// Runs on this thread until the process exits. `on_lock` runs when the workstation locks.
pub fn run(on_lock: impl FnMut() + 'static) -> Result<(), String> {
    ON_LOCK.with(|slot| *slot.borrow_mut() = Some(Box::new(on_lock)));
    let instance = message_window::module_instance().map_err(|error| error.to_string())?;
    message_window::register_class::<LockWatch>(instance, CLASS_NAME).map_err(|error| error.to_string())?;
    let window = message_window::create_tool_popup_window(instance, CLASS_NAME, "Yunzhao vault lock")
        .map_err(|error| error.to_string())?;
    // SAFETY: `window` is a live top-level window created above. The flag asks
    // for this session only, and the call does not take a pointer.
    let registered = unsafe { WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION) };
    if !registered.as_bool() {
        return Err("无法监听电脑锁屏。".into());
    }
    message_window::run_message_loop();
    Ok(())
}
