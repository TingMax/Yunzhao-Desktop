import { useEffect, useMemo, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { getCurrentWindow } from "@tauri-apps/api/window";
// First: the message panels it loads bring CSS that must come before the page panels' and the shell's.
import { useIslandNotices } from "./useIslandNotices";
// The page panels; before IslandShell so their CSS keeps its place in the cascade.
import { IslandPages } from "./IslandPages";
import { useTranslation } from "../../i18n";
import { useSettingsPresentation } from "../settings/useSettingsPresentation";
import { useSettingsNavigation } from "./useSettingsNavigation";
import { useIslandSettings } from "../../features/appearance/useIslandSettings";
import { loadAppearancePresets } from "../../features/appearance/appearancePresets";
import { loadSettings as loadTranslationSettings } from "../../features/translation/storage";
import { getUpdateState } from "../../features/updater/updaterController";
import { readLaunchAtStartup } from "../settings/api";
import { useDocumentTheme } from "../../features/appearance/useDocumentTheme";
import { resolveSchemeColors } from "../../features/appearance/theme";
import { IslandShell } from "./IslandShell";
import { useMediaState } from "../../features/media/useMediaState";
import type { TodoItem } from "../../features/todos/types";
import { useIslandWindow } from "./useIslandWindow";
import { getExpandedIslandHeight } from "./expandedHeight";
import { getIslandStageStyle } from "./stageStyle";
import { useCollapsedTodo } from "./useCollapsedTodo";
import { useGameMode } from "./useGameMode";
import { useIslandKeyboard } from "./useIslandKeyboard";
import { useIslandFeatures } from "./useIslandFeatures";
import { useIslandAgentAlerts } from "./useIslandAgentAlerts";
import { useIslandAgents } from "./useIslandAgents";
import { useQuotaAlert } from "../../features/agent-status/useQuotaAlert";
import { AgentArrangeButton } from "../../features/agent-status/AgentArrangeButton";
import { useAgentIndicator } from "./useAgentIndicator";
import { usageLineCount } from "../../features/agent-status/usage";
import { useIslandNavigation } from "./useIslandNavigation";
import { useVaultSession } from "../../features/vault/useVaultSession";
import { useIslandTodos } from "./useIslandTodos";
import { useIslandTucking } from "./useIslandTucking";
import { useIslandHover } from "./useIslandHover";
import { useIdleUpdateInstall } from "../../features/updater/useIdleUpdateInstall";
import { useIslandTranslation } from "./useIslandTranslation";
import { useMessageView } from "./useMessageView";
import { useSettingsPageHeight } from "./useSettingsPageHeight";
import { useIslandPickerOpen } from "../../lib/islandPicker";
import { TODO_DATE_PICKER_EXPANDED_ISLAND_HEIGHT } from "./layout";
// Must stay the last import: IslandApp.css overrides the feature styles above.
import "./IslandApp.css";

// Stands in for the todo list while todos are switched off, so the collapsed island shows none.
const NO_TODOS: TodoItem[] = [];

/**
 * The island window: puts the feature hooks together and hands their state to the shell and the
 * page panels. Each concern's wiring lives in its own use* hook next to this file; add new wiring
 * there rather than here, so parallel changes do not all meet in this file.
 */
export function IslandApp() {
  const t = useTranslation();
  const navigation = useIslandNavigation();
  const { mode, isTucked, page, setPage, collapseIsland, tuckIsland, revealIsland, minimizeToEdge, openIslandPage } = navigation;
  useVaultSession();
  const [settings, setSettings] = useIslandSettings();
  const [launchAtStartup, setLaunchAtStartup] = useState(false);
  useEffect(() => {
    void readLaunchAtStartup().then(setLaunchAtStartup).catch(() => {});
  }, []);
  const detachedSettings = useSettingsPresentation();
  const { openSettings, changePresentation } = useSettingsNavigation(openIslandPage, setPage);
  const { settingsContentHeight, reportSettingsHeight } = useSettingsPageHeight();
  // Quiet while a full-screen app (usually a game) is in front, if game mode asks for it.
  const game = useGameMode(settings, { mode, isTucked, collapseIsland, tuckIsland, revealIsland });
  const notices = useIslandNotices(settings, { mode, isTucked, quiet: game.quiet });
  useIdleUpdateInstall(mode === "collapsed", game.quiet);
  const { qqNotifications, wechatReminders, wecomReminders, qqMail, hasMessagePreview, hostPreview } = notices;
  useEffect(() => {
    const onExtra = (event: Event) => {
      const patch = (event as CustomEvent<Record<string, unknown>>).detail ?? {};
      if (typeof patch.qqEnabled === "boolean") void qqNotifications.setEnabled(patch.qqEnabled);
      if (typeof patch.wechatEnabled === "boolean") void wechatReminders.setEnabled(patch.wechatEnabled);
      if (typeof patch.wechatPreview === "boolean") void wechatReminders.setPreviewEnabled(patch.wechatPreview);
      if (typeof patch.wechatHideTaskbar === "boolean") void wechatReminders.setHideMinimizedTaskbar(patch.wechatHideTaskbar);
      if (typeof patch.wecomEnabled === "boolean") void wecomReminders.setEnabled(patch.wecomEnabled);
      if (typeof patch.launchAtStartup === "boolean") setLaunchAtStartup(patch.launchAtStartup);
    };
    window.addEventListener("yunzhao-host-extra", onExtra);
    return () => window.removeEventListener("yunzhao-host-extra", onExtra);
  }, [qqNotifications, wechatReminders, wecomReminders]);
  useEffect(() => {
    if (getCurrentWindow().label !== "main") return;
    const translation = loadTranslationSettings();
    void invoke("publish_host_island_settings", {
      settings: {
        ...settings,
        launchAtStartup,
        locale: localStorage.getItem("focusd-island-locale") ?? "zh-CN",
        detachedSettings: localStorage.getItem("focusd-island-detached-settings") !== "false",
        appearancePresets: loadAppearancePresets(),
        translationProvider: translation.provider,
        translationModel: translation.models[translation.provider] ?? "",
        translationBaseUrl: translation.customBaseUrl,
        translationShortcut: translation.shortcut,
        autoUpdate: getUpdateState().autoUpdate,
        qqEnabled: qqNotifications.snapshot.enabled,
        wechatEnabled: wechatReminders.snapshot.enabled,
        wechatPreview: wechatReminders.snapshot.previewEnabled,
        wechatHideTaskbar: wechatReminders.snapshot.hideMinimizedTaskbar,
        wecomEnabled: wecomReminders.snapshot.enabled,
      },
    }).catch(() => {});
  }, [settings, launchAtStartup, qqNotifications.snapshot, wechatReminders.snapshot, wecomReminders.snapshot]);
  // The output level shows only on the collapsed island (its waveform).
  const media = useMediaState(settings.musicEnabled, mode === "collapsed");
  const { agentStatus, agentSessions, listedAgentSessions, activeAgentSessionCount, unseenAgentSessions, agentRemoval, agentSession, agentUsages, alertUsages } =
    useIslandAgents(settings);
  const quotaAlert = useQuotaAlert(alertUsages, 0, {
    quiet: game.quiet,
    canPopUp: !game.quiet && mode === "collapsed" && !isTucked,
  });
  const { todos, activeTodoId, timer, todoPageMode, todoListHeight } = useIslandTodos({
    settings: { ...settings, todoEnabled: false },
    navigation,
    quiet: game.quiet,
  });

  const songPlaying = settings.musicEnabled && media.mediaState.playbackStatus === "playing" && Boolean(media.mediaState.title);
  const cover = settings.collapsedShowCover && songPlaying ? media.mediaState.thumbnail : null;
  const { collapsedTaskTitle, collapsedTodoCount, collapsedItem, collapsedIslandWidth, todoRotation } =
    useCollapsedTodo(
      NO_TODOS,
      null,
      { ...settings, todoEnabled: false, showIdleTodoCount: false },
      mode !== "collapsed" || isTucked || !settings.showCollapsedText || hasMessagePreview,
      null,
      { lyricsPlaying: songPlaying, coverWanted: Boolean(cover), timer: null,
        activeIds: [], timers: [] },
    );
  // The cover goes with the lyric line: when something ranked above lyrics is shown, it goes too.
  const collapsedCover = collapsedItem === "lyrics" ? cover : null;
  const { agentVisualState, agentStatusLabel } = useAgentIndicator(agentStatus, settings, activeAgentSessionCount, t);
  useIslandTucking({
    settings,
    navigation,
    hasMessagePreview,
    activity: { agentVisualState, collapsedItem, songPlaying, activeTodoId, timer },
    gameHolds: game.quiet || game.tuck,
  });

  const { messageSource, setMessageSource, openMessages } = useMessageView(qqMail.closeReader, openIslandPage);

  const { features, openEnabledPage } = useIslandFeatures({
    settings,
    qqEnabled: qqNotifications.snapshot.enabled,
    wechatEnabled: wechatReminders.snapshot.enabled,
    wecomEnabled: wecomReminders.snapshot.enabled,
    qqMail,
    page,
    messageSource,
    setPage,
    setMessageSource,
    openIslandPage,
  });
  const { alertedSession, approvals: agentApprovals, agentCardHeight } = useIslandAgentAlerts({
    sessions: agentSessions,
    unseen: unseenAgentSessions.keys,
    settings,
    enabled: features.agents && !game.quiet,
    mode,
    page,
    openIslandPage,
    collapseIsland,
  });
  const pickerOpen = useIslandPickerOpen();
  const expandedIslandHeight = Math.max(pickerOpen && page === "todo" ? TODO_DATE_PICKER_EXPANDED_ISLAND_HEIGHT : 0, getExpandedIslandHeight({
    page,
    todoPageMode,
    todos,
    todoListHeight,
    hasMediaCommandError: Boolean(media.mediaCommandError),
    messageSource,
    isReadingMail: qqMail.wantsTallPanel,
    maxPanelHeight: settings.maxPanelHeight,
    clipboardVisibleRows: settings.clipboardVisibleRows,
    agentVisibleRows: settings.agentVisibleRows,
    agentSessionView: settings.agentSessionView,
    fontScale: settings.islandFontPercent / 100,
    agentUsageLines: settings.agentShowUsage ? usageLineCount(agentUsages) : 0,
    agentAlertRowHeight: agentCardHeight,
    settingsContentHeight,
    shelfItems: 0,
  }));
  const scheme = useDocumentTheme(settings);
  const colors = useMemo(() => resolveSchemeColors(settings, scheme), [settings, scheme]);
  const stageStyle = useMemo(
    () => getIslandStageStyle(settings, colors, collapsedIslandWidth, expandedIslandHeight),
    [collapsedIslandWidth, colors, expandedIslandHeight, settings],
  );


  const {
    isPositionDetached,
    nativeGlassState,
    minimizeIsland,
    startIslandDrag,
    resetIslandPosition,
  } = useIslandWindow({
    mode,
    isTucked,
    settings,
    setSettings,
    expandedIslandHeight,
    collapsedIslandWidth,
    onCollapse: collapseIsland,
  });

  const hoverTucked = useIslandHover({
    settings,
    navigation,
    quiet: game.quiet,
    hasMessagePreview,
    namesAgent: Boolean(agentSession),
    openEnabledPage,
  });

  useIslandKeyboard(false, "", () => {}, collapseIsland);

  const { openTranslator } = useIslandTranslation(
    settings.translationEnabled,
    features.translation,
    mode,
    page,
    openIslandPage,
    collapseIsland,
  );

  return (
    <main className={`stage stage--font-${settings.fontFamily}`} data-tone={colors.tone} style={stageStyle}>
      <IslandShell
        qqMail={qqMail}
        qqNotifications={qqNotifications}
        wechatReminders={wechatReminders}
        wecomReminders={wecomReminders}
        onOpenMessages={openMessages}
        hostPreview={hostPreview}
        mode={mode}
        page={page}
        expandedHeight={expandedIslandHeight}
        material={settings.material}
        nativeGlassState={nativeGlassState}
        isTucked={isTucked}
        tuckStyle={settings.tuckStyle}
        topEdgeShoulders={settings.topEdgeShoulders}
        positionLocked={settings.positionLocked}
        isDetached={isPositionDetached || settings.marginY > 0}
        showTitle={settings.showTitle}
        showCollapsedText={settings.showCollapsedText}
        collapsedTaskTitle={collapsedTaskTitle}
        todoRotation={todoRotation}
        todoTimer={null}
        collapsedTodoCount={collapsedTodoCount}
        collapsedItem={collapsedItem}
        collapsedLayout={settings.collapsedLayout}
        collapsedCover={collapsedCover}
        lyricsSettings={settings}
        pageOrder={settings.pageOrder}
        pageButtonStyle={settings.pageButtonStyle}
        onPageOrderChange={(pageOrder) => setSettings({ ...settings, pageOrder })}
        agentSession={agentSession}
        activeAgentSessionCount={activeAgentSessionCount}
        mediaState={media.mediaState}
        agentVisualState={agentVisualState}
        agentStatusLabel={agentStatusLabel}
        features={features}
        shelfCount={null}
        quotaAlert={quotaAlert}
        onOpenPage={openEnabledPage}
        onWindowDragStart={startIslandDrag}
        onCollapse={collapseIsland}
        onResetPosition={resetIslandPosition}
        minimizesToTray={settings.minimizeAction === "tray"}
        onMinimize={settings.minimizeAction === "tray" ? minimizeIsland : minimizeToEdge}
        onOpenTranslator={settings.translationEnabled ? openTranslator : null}
        onOpenSettings={openSettings}
        onTuck={tuckIsland}
        onReveal={hoverTucked}
        onPageChange={setPage}
        pageActions={page === "agents" && (
          <AgentArrangeButton sort={settings.agentSessionSort} group={settings.agentSessionGroup} onChange={(patch) => setSettings({ ...settings, ...patch })} />
        )}
      >
        <IslandPages
          page={page}
          mode={mode}
          settings={settings}
          onSettingsChange={setSettings}
          detachedSettings={detachedSettings}
          onDetachedSettingsChange={(value) => void changePresentation(value)}
          onSettingsHeight={reportSettingsHeight}
          features={features}
          qqMail={qqMail}
          qqNotifications={qqNotifications}
          wechatReminders={wechatReminders}
          wecomReminders={wecomReminders}
          messageSource={messageSource}
          openMessages={openMessages}
          media={media}
          agentSessions={listedAgentSessions}
          unseenAgentSessions={unseenAgentSessions}
          agentRemoval={agentRemoval}
          alertedSession={alertedSession}
          agentApprovals={agentApprovals}
          agentUsages={agentUsages}
        />
      </IslandShell>
    </main>
  );
}
