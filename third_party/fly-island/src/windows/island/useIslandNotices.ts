import { useQQNotifications } from "../../features/messages/qq-notifications/QQNotifications";
import { useWeChatReminders } from "../../features/messages/wechat/WeChatReminders";
import { useWeComReminders } from "../../features/messages/wecom/useWeComReminders";
import { useMail } from "../../features/messages/mail/MailPanel";
import { useHostNotice } from "../../features/messages/hostNotice";
import { usePluginPreview } from "../../features/messages/pluginNotifications";
import type { IslandSettings } from "../../features/appearance/types";
import type { IslandMode } from "./types";

/** What can pop up on the collapsed island: message previews (QQ, WeChat, mail). */
export function useIslandNotices(settings: IslandSettings, { mode, isTucked, quiet }: { mode: IslandMode; isTucked: boolean; quiet: boolean }) {
  // A tucked island still takes message previews when it will slide out to show them.
  const canPreviewMessages = !quiet && mode === "collapsed" && (!isTucked || settings.revealOnMessage);
  const qqNotifications = useQQNotifications(canPreviewMessages, settings.messagePreviewSeconds);
  const wechatReminders = useWeChatReminders(canPreviewMessages, { previewSeconds: settings.messagePreviewSeconds });
  const wecomReminders = useWeComReminders(canPreviewMessages, { previewSeconds: settings.messagePreviewSeconds });
  const qqMail = useMail(canPreviewMessages, { previewSeconds: settings.messagePreviewSeconds, autoCopyOtp: settings.autoCopyOtp });
  const pluginPreview = usePluginPreview(canPreviewMessages);
  const hostPreview = useHostNotice(canPreviewMessages);
  const hasMessagePreview = Boolean(
    qqNotifications.preview || wechatReminders.preview || wecomReminders.preview || qqMail.preview || pluginPreview || hostPreview,
  );
  return { qqNotifications, wechatReminders, wecomReminders, qqMail, hasMessagePreview, hostPreview };
}
