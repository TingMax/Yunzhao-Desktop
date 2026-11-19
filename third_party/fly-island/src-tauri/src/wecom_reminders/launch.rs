//! Brings WeCom forward, or starts it, without a second copy when one is already running.

use super::source::{existing_windows, is_wecom_executable, preferred_window, running_executable};
use crate::win32;
use std::{path::{Path, PathBuf}, process::Command};
use windows::Win32::System::Registry::{HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};

pub(super) const NOT_INSTALLED: &str = "wecom-open:not-installed";
pub(super) const NO_MAIN_WINDOW: &str = "wecom-open:no-main-window";
pub(super) const RESTORE_FAILED: &str = "wecom-open:restore-failed";
pub(super) const LAUNCH_FAILED: &str = "wecom-open:launch-failed";

fn registry_executable() -> Option<PathBuf> {
    for root in [HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE] {
        for key in ["Software\\Tencent\\WXWork", "Software\\WOW6432Node\\Tencent\\WXWork"] {
            let Some(folder) = win32::registry::read_string(root, key, "InstallPath") else {
                continue;
            };
            let path = PathBuf::from(folder.trim_matches('"')).join("WXWork.exe");
            if path.is_file() {
                return Some(path);
            }
        }
    }
    None
}

fn common_install_paths() -> Vec<PathBuf> {
    let mut paths = Vec::new();
    for variable in ["ProgramFiles", "ProgramFiles(x86)", "ProgramW6432", "LOCALAPPDATA"] {
        let Some(root) = std::env::var_os(variable).map(PathBuf::from) else {
            continue;
        };
        paths.push(root.join("WXWork\\WXWork.exe"));
        paths.push(root.join("Tencent\\WXWork\\WXWork.exe"));
    }
    paths
}

fn installed_executable() -> Option<PathBuf> {
    running_executable()
        .or_else(registry_executable)
        .or_else(|| common_install_paths().into_iter().find(|path| path.is_file()))
        .filter(|path| is_wecom_executable(&path.to_string_lossy()))
}

fn start(executable: &Path) -> bool {
    let mut command = Command::new(executable);
    if let Some(folder) = executable.parent() {
        command.current_dir(folder);
    }
    command.spawn().is_ok()
}

pub(super) fn open_wecom() -> Result<(), String> {
    let windows = existing_windows()?;
    if let Some(target) = preferred_window(&windows) {
        win32::window::allow_set_foreground_window(target.pid);
        if win32::window::force_foreground(target.handle) || win32::window::bring_to_foreground(target.handle) {
            return Ok(());
        }
        return Err(RESTORE_FAILED.into());
    }
    if running_executable().is_some() {
        return Err(NO_MAIN_WINDOW.into());
    }
    let executable = installed_executable().ok_or(NOT_INSTALLED)?;
    if start(&executable) {
        Ok(())
    } else {
        Err(LAUNCH_FAILED.into())
    }
}
