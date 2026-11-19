import type { WecomText } from "../../../i18n/sections/wecom";

const PREFIX = "wecom-open:";

export function weComOpenErrorText(error: unknown, text: WecomText["openErrors"]) {
  const message = String(error);
  if (!message.startsWith(PREFIX)) return message;
  const code = message.slice(PREFIX.length);
  return Object.prototype.hasOwnProperty.call(text, code) ? text[code as keyof WecomText["openErrors"]] : message;
}
