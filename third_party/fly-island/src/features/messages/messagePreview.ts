import zhCN from "../../i18n/locales/zh-CN.ts";
import type { Translations } from "../../i18n/types";

export type MessageSource = "qq" | "wechat" | "wecom" | "qqMail" | "plugin";
export type MessagePreview = {
  source: MessageSource;
  title: string;
  body: string;
  receivedAt: number;
  wechatMessageId?: string;
  otpCode?: string | null;
  /** A notice forwarded by Yunzhao. Clicking it does not open another app. */
  host?: boolean;
};

export const MESSAGE_SOURCE_LABELS: Record<MessageSource, string> = zhCN.messages.sourceLabels;

export function messagePreviewText(preview: MessagePreview, copy: Translations["messages"] = zhCN.messages): string {
  if (preview.source === "wechat" && !preview.title.trim()) return copy.wechatPreview;
  if (preview.source === "wecom") return preview.body.trim() || copy.wecomPreview;
  if (preview.source === "qqMail") return preview.otpCode
    ? `${preview.body.trim() || copy.newMail} · ${preview.otpCode}` : preview.body.trim() || copy.newMail;
  return copy.qqPreview(preview.title.trim() || "QQ", preview.body.trim() || copy.newMessage);
}

export function latestMessagePreview(candidates: (MessagePreview | null)[], latestReceivedAt = -Infinity): MessagePreview | null {
  return candidates.reduce<MessagePreview | null>(
    (latest, candidate) => candidate && candidate.receivedAt >= latestReceivedAt
      && (!latest || candidate.receivedAt > latest.receivedAt) ? candidate : latest,
    null,
  );
}

export function mailBadgeCount(count: number): string {
  return count > 99 ? "99+" : String(count);
}
