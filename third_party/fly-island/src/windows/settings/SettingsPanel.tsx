import { useCallback, useEffect, useState } from "react";
import { AppearanceSettingsPage } from "./AppearanceSettingsPage";
import { DEFAULT_SETTINGS_CATEGORY, type SettingsCategory } from "./categories";
import { GeneralSettingsPage } from "./GeneralSettingsPage";
import { AgentSettingsPage } from "./AgentSettingsPage";
import { AgentConnectSettingsPage } from "./AgentConnectSettingsPage";
import { VaultPanel } from "../../features/vault/VaultPanel";
import { MusicSettingsPage } from "./MusicSettingsPage";
import { MessagesSettingsPage } from "./MessagesSettingsPage";
import { PluginsSettingsPage } from "./PluginsSettingsPage";
import { PLUGINS_ENABLED } from "../../features/plugins";
import { SETTINGS_FOCUS_STORAGE_KEY, takeFocusRequest, requestPanelPreview } from "./storage";
import { LayoutEditor } from "./LayoutEditor";
import { TranslationSettingsPage } from "./TranslationSettingsPage";
import { AboutSettingsPage } from "./AboutSettingsPage";
import { DEFAULT_SETTINGS, withAppearanceFrom } from "../../features/appearance/settings";
import { ResetAppearanceButton } from "../../features/appearance/ResetAppearanceButton";
import { withColorBackup } from "../../features/appearance/storage";
import type { IslandSettings } from "../../features/appearance/types";
import type { QQNotificationsController } from "../../features/messages/qq-notifications/QQNotifications";
import type { WeChatRemindersController } from "../../features/messages/wechat/WeChatReminders";
import type { WeComRemindersController } from "../../features/messages/wecom/useWeComReminders";
import { useLocale } from "../../i18n";

/**
 * Every settings page with its sidebar. Shown as a page of the island or in the
 * standalone settings window; the host owns the settings and where they are saved.
 */
export function SettingsPanel({
  settings,
  onSettingsChange,
  qqNotifications,
  wechatReminders,
  wecomReminders,
  detached,
  onDetachedChange,
  onContentHeight,
}: {
  settings: IslandSettings;
  onSettingsChange: (settings: IslandSettings | ((current: IslandSettings) => IslandSettings)) => void;
  qqNotifications: QQNotificationsController;
  wechatReminders: WeChatRemindersController;
  wecomReminders: WeComRemindersController;
  /** Whether settings open in their own window (General → Separate settings window). */
  detached: boolean;
  onDetachedChange: (detached: boolean) => void;
  /** As a page of the island: told the height the open page needs, to size the island to it. */
  onContentHeight?: (height: number) => void;
}) {
  const { locale, setLocale } = useLocale();
  const setSettings = onSettingsChange;
  const [category, setCategory] = useState<SettingsCategory>(DEFAULT_SETTINGS_CATEGORY);

  const focusRequestedSection = useCallback(() => {
    const section = takeFocusRequest();
    if (section === "translation") setCategory("translation");
    if (section === "music") setCategory("music");
    if (section === "mail") setCategory("messages");
    if (section === "updates") setCategory("about");
    if (section === "vault") setCategory("vault");
  }, []);

  useEffect(() => {
    focusRequestedSection();
    const onRequest = (event: StorageEvent) => {
      if (event.key === SETTINGS_FOCUS_STORAGE_KEY && event.newValue !== null) focusRequestedSection();
    };
    window.addEventListener("storage", onRequest);
    return () => window.removeEventListener("storage", onRequest);
  }, [focusRequestedSection]);

  const resetAppearance = useCallback(() => {
    const next = withColorBackup(settings, withAppearanceFrom(settings, DEFAULT_SETTINGS));
    setSettings(next);
    if (next !== settings) requestPanelPreview("collapsed");
  }, [settings, setSettings]);

  return (
    <LayoutEditor
      category={category}
      onCategoryChange={setCategory}
      onContentHeight={onContentHeight}
      titleAction={category === "appearance" && <ResetAppearanceButton onReset={resetAppearance} />}
    >
      {category === "general" && (
        <GeneralSettingsPage
          locale={locale}
          settings={settings}
          detachedSettings={detached}
          onLocaleChange={setLocale}
          onSettingsChange={setSettings}
          onDetachedSettingsChange={onDetachedChange}
        />
      )}
      {category === "appearance" && (
        <AppearanceSettingsPage settings={settings} onSettingsChange={setSettings} />
      )}
      {category === "vault" && <VaultPanel surface="settings" />}
      {category === "messages" && (
        <MessagesSettingsPage settings={settings} qqNotifications={qqNotifications} wechatReminders={wechatReminders} wecomReminders={wecomReminders} onSettingsChange={setSettings} />
      )}
      {category === "translation" && <TranslationSettingsPage settings={settings} onSettingsChange={setSettings} />}
      {category === "music" && <MusicSettingsPage settings={settings} onSettingsChange={setSettings} />}
      {category === "agents" && <AgentSettingsPage settings={settings} onSettingsChange={setSettings} onOpenConnect={() => setCategory("agentConnect")} />}
      {category === "agentConnect" && <AgentConnectSettingsPage />}
      {PLUGINS_ENABLED && category === "plugins" && <PluginsSettingsPage />}
      {category === "about" && <AboutSettingsPage />}
    </LayoutEditor>
  );
}
