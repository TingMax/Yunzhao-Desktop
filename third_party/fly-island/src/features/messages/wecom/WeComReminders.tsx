import { CheckCheck, Power } from "lucide-react";
import type { ReactNode } from "react";
import { useTranslation } from "../../../i18n";
import { MessageSourceIcon } from "../MessageSourceIcon";
import "../qq-notifications/QQNotifications.css";
import "../wechat/WeChatReminders.css";
import type { WeComRemindersController } from "./useWeComReminders";

export type { WeComRemindersController };

export function WeComReminderPanel({ controller, navigation }: { controller: WeComRemindersController; navigation: ReactNode }) {
  const { snapshot, busy, actionError, openWeCom, clear, setEnabled } = controller;
  const t = useTranslation();
  const pending = snapshot.enabled && snapshot.status === "listening" && snapshot.pending;
  const label = snapshot.status === "listening" ? pending ? t.messages.newReminder : t.wecom.noReminders : t.wecom.status[snapshot.status];
  return (
    <section className="qq-panel" aria-label={t.messages.wecomReminders}>
      <header className="qq-panel__header">
        {navigation}
        <div className="qq-panel__toolbar">
          <button className="icon-button" type="button" aria-label={t.messages.clearAll} title={t.messages.clearAll} disabled={busy || !snapshot.pending} onClick={() => void clear()}>
            <CheckCheck size={16} strokeWidth={1.5} absoluteStrokeWidth />
          </button>
          <button className="icon-button message-receiver-toggle" type="button" role="switch" aria-checked={snapshot.enabled} aria-label={t.wecom.receive} disabled={busy} onClick={() => void setEnabled(!snapshot.enabled)}>
            <Power size={15} strokeWidth={1.8} />
          </button>
        </div>
      </header>
      {(actionError || snapshot.error) && <p className="qq-panel__error" role="alert">{actionError || snapshot.error}</p>}
      <div className="qq-panel__empty">
        <button className={`wechat-launch${pending ? " wechat-launch--pending" : ""}`} type="button" aria-label={`${label}，${t.wecom.clickToOpen}`} disabled={busy} onClick={() => void openWeCom()}>
          <span className="wechat-launch__icon"><MessageSourceIcon source="wecom" size={30} /></span>
          <span className="wechat-launch__state" role="status">{label}</span>
          <span className="wechat-launch__hint">{t.wecom.clickToOpen}</span>
        </button>
      </div>
    </section>
  );
}
