//! The view password stays in Windows Credential Manager while the session is
//! unlocked, and is removed when the workstation locks.

use keyring::{Entry, Error as KeyringError};
use tauri::{AppHandle, Emitter};

const SERVICE: &str = "com.focusd.island.vault";
const ACCOUNT: &str = "view-password";
pub const LOCKED_EVENT: &str = "vault-workstation-locked";

fn entry() -> Result<Entry, String> {
    Entry::new(SERVICE, ACCOUNT).map_err(|_| "无法访问查看密码。".to_string())
}

#[tauri::command]
pub fn remember_vault_password(password: String) -> Result<(), String> {
    entry()?.set_password(&password).map_err(|_| "无法记住查看密码。".to_string())
}

#[tauri::command]
pub fn read_vault_password() -> Result<Option<String>, String> {
    match entry()?.get_password() {
        Ok(password) => Ok(Some(password)),
        Err(KeyringError::NoEntry) => Ok(None),
        Err(_) => Err("无法读取查看密码。".into()),
    }
}

pub fn forget_vault_password() {
    let Ok(saved) = entry() else { return };
    let _ = saved.delete_credential();
}

pub fn start(app: AppHandle) {
    std::thread::spawn(move || {
        if let Err(error) = crate::win32::session_lock::run(move || {
            forget_vault_password();
            let _ = app.emit_to(crate::island::WINDOW_LABEL, LOCKED_EVENT, ());
        }) {
            eprintln!("failed to watch workstation lock: {error}");
        }
    });
}
