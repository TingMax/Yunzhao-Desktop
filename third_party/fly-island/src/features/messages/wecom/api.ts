import { invoke } from "@tauri-apps/api/core";
import { listen, type EventCallback } from "@tauri-apps/api/event";

export type WeComSnapshot = {
  revision: number;
  enabled: boolean;
  status: "disabled" | "waiting" | "listening" | "error";
  pending: boolean;
  lastAttentionAt: number | null;
  error: string | null;
};

export type WeComReminderEvent = { snapshot: WeComSnapshot; windowVisible: boolean };

export const WECOM_REMINDER_CHANGED_EVENT = "wecom-reminder-changed";

export function getWeComReminderState() {
  return invoke<WeComSnapshot>("get_wecom_reminder_state");
}

export function clearWeComReminders() {
  return invoke<WeComSnapshot>("clear_wecom_reminders");
}

export function setWeComRemindersEnabled(enabled: boolean) {
  return invoke<WeComSnapshot>("set_wecom_reminders_enabled", { enabled });
}

export function launchWeCom() {
  return invoke("open_wecom");
}

export function listenWeComReminderChanged(handler: EventCallback<WeComReminderEvent>) {
  return listen<WeComReminderEvent>(WECOM_REMINDER_CHANGED_EVENT, handler);
}
