import { readString, writeString } from "../../../lib/storage";

const ENABLED_KEY = "yunzhao-island-wecom-reminders-enabled";

export function isWeComRemindersEnabled() {
  return readString(ENABLED_KEY) !== "false";
}

export function saveWeComRemindersEnabled(enabled: boolean) {
  writeString(ENABLED_KEY, String(enabled));
}
