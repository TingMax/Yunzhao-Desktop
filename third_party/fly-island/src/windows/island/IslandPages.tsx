import type { SessionRemoval } from "../../features/agent-status/useHiddenSessions";
import type { ReactNode } from "react";
import { MessagesPanel } from "../../features/messages/Messages";
import type { useQQNotifications } from "../../features/messages/qq-notifications/QQNotifications";
import type { useWeChatReminders } from "../../features/messages/wechat/WeChatReminders";
import type { WeComRemindersController } from "../../features/messages/wecom/useWeComReminders";
import type { useMail } from "../../features/messages/mail/MailPanel";
import { TranslationPanel } from "../../features/translation/TranslationPanel";
import { openSettingsWindow } from "../settings/api";
import { SettingsPanel } from "../settings/SettingsPanel";
import "../settings/IslandSettingsPage.css";
import { AgentSessionsPanel } from "../../features/agent-status/AgentSessionsPanel";
import type { AgentSessionSummary } from "../../features/agent-status/api";
import type { UnseenSessions } from "../../features/agent-status/useUnseenSessions";
import type { AgentUsages } from "../../features/agent-status/usage";
import type { IslandSettings } from "../../features/appearance/types";
import { MusicPlayerPanel } from "../../features/media/MusicPlayerPanel";
import type { useMediaState } from "../../features/media/useMediaState";
import { VaultPanel } from "../../features/vault/VaultPanel";
import type { useIslandAgentAlerts } from "./useIslandAgentAlerts";
import type { useIslandFeatures } from "./useIslandFeatures";
import type { useIslandNavigation } from "./useIslandNavigation";
import type { useMessageView } from "./useMessageView";

type MessageView = ReturnType<typeof useMessageView>;
type AgentAlerts = ReturnType<typeof useIslandAgentAlerts>;

type IslandPagesProps = {
  page: ReturnType<typeof useIslandNavigation>["page"];
  mode: ReturnType<typeof useIslandNavigation>["mode"];
  settings: IslandSettings;
  onSettingsChange: (settings: IslandSettings | ((current: IslandSettings) => IslandSettings)) => void;
  /** Whether settings open in their own window (then this page is not used). */
  detachedSettings: boolean;
  onDetachedSettingsChange: (detached: boolean) => void;
  /** The height the open settings page needs (useSettingsPageHeight). */
  onSettingsHeight: (height: number) => void;
  features: ReturnType<typeof useIslandFeatures>["features"];
  qqMail: ReturnType<typeof useMail>;
  qqNotifications: ReturnType<typeof useQQNotifications>;
  wechatReminders: ReturnType<typeof useWeChatReminders>;
  wecomReminders: WeComRemindersController;
  messageSource: MessageView["messageSource"];
  openMessages: MessageView["openMessages"];
  media: ReturnType<typeof useMediaState>;
  agentSessions: AgentSessionSummary[];
  unseenAgentSessions: UnseenSessions;
  agentRemoval: SessionRemoval;
  alertedSession: AgentAlerts["alertedSession"];
  agentApprovals: AgentAlerts["approvals"];
  agentUsages: AgentUsages;
};

/** The expanded island's page panels; the one for `page` shows. */
export function IslandPages({
  page,
  mode,
  settings,
  onSettingsChange,
  detachedSettings,
  onDetachedSettingsChange,
  onSettingsHeight,
  features,
  qqMail,
  qqNotifications,
  wechatReminders,
  wecomReminders,
  messageSource,
  openMessages,
  media,
  agentSessions,
  unseenAgentSessions,
  agentRemoval,
  alertedSession,
  agentApprovals,
  agentUsages,
}: IslandPagesProps): ReactNode {
  const { mediaState, isMediaTrackChanging, mediaCommandError, runMediaCommand, seekTo, calibrate } = media;
  return (
    <>
      {page === "settings" && (
        <div className="island-settings">
          <SettingsPanel
            settings={settings}
            onSettingsChange={onSettingsChange}
            qqNotifications={qqNotifications}
            wechatReminders={wechatReminders}
            wecomReminders={wecomReminders}
            detached={detachedSettings}
            onDetachedChange={onDetachedSettingsChange}
            onContentHeight={onSettingsHeight}
          />
        </div>
      )}
      {page === "qq" && (
        <div className="message-panel">
          <MessagesPanel qqMail={qqMail} qqNotifications={qqNotifications} wechatReminders={wechatReminders} wecomReminders={wecomReminders} source={messageSource} enabledSources={features} onSourceChange={openMessages} onOpenMailSettings={() => openSettingsWindow("mail")} />
        </div>
      )}
      {page === "vault" && <VaultPanel surface="island" />}
      {page === "music" && (
        <MusicPlayerPanel
          mediaState={mediaState}
          isTrackChanging={isMediaTrackChanging}
          commandError={mediaCommandError}
          onOpenSettings={() => openSettingsWindow("music")}
          onPlayPause={() => void runMediaCommand("media_play_pause")}
          onNext={() => void runMediaCommand("media_next")}
          onPrevious={() => void runMediaCommand("media_previous")}
          onSeek={(positionMs) => void seekTo(positionMs)}
          onCalibrate={calibrate}
          lyricsSettings={settings}
        />
      )}
      {page === "agents" && <AgentSessionsPanel sessions={agentSessions} view={settings.agentSessionView} sort={settings.agentSessionSort} group={settings.agentSessionGroup} archiveMotion={settings.agentArchiveMotion} cardEffects={settings.agentCardEffects} hostDisplay={settings.agentHostDisplay} showElapsed={settings.agentShowElapsed} showModel={settings.agentShowModel} showActivity={settings.agentShowActivity} unseen={unseenAgentSessions} removal={agentRemoval} reveal={alertedSession} showUsage={settings.agentShowUsage} usages={agentUsages} approvals={agentApprovals} />}
      {page === "translation" && <TranslationPanel isOpen={mode === "expanded"} onOpenSettings={() => openSettingsWindow("translation")} />}
    </>
  );
}
