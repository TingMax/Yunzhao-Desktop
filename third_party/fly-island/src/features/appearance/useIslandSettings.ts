import { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { saveLocale } from "../../i18n";
import { saveTranslationSecret, setTranslationShortcut } from "../translation/api";
import { loadSettings as loadTranslationSettings, saveTranslationSettings } from "../translation/storage";
import { isTranslationProvider } from "../translation/providers";
import { checkForUpdate, setAutoUpdate } from "../updater/updaterController";
import { quitIsland, saveLaunchAtStartup } from "../../windows/settings/api";
import { requestIslandPositionReset, saveDetachedSettings } from "../../windows/settings/storage";
import { applyAppearancePreset, addAppearancePreset, loadAppearancePresets, removeAppearancePreset, saveAppearancePresets } from "./appearancePresets";
import { isSameSettings } from "./settings";
import { STORAGE_KEY, loadSettings, saveSettings } from "./storage";
import type { IslandSettings } from "./types";

/** The island's appearance settings, persisted and kept in sync with the settings window. */
export function useIslandSettings() {
  const [settings, setSettings] = useState<IslandSettings>(loadSettings);
  // Settings just read from the other window's write. Saving them back would echo: while a colour
  // is dragged, the other window has already written a newer value, and the echo would put the
  // older one over it, pull that window back, and the colour would flicker and stick.
  const readFromStorage = useRef<IslandSettings | null>(null);

  useEffect(() => {
    if (readFromStorage.current === settings) return;
    saveSettings(settings);
  }, [settings]);

  // The settings window edits this key; storage events deliver its changes here.
  useEffect(() => {
    const syncFromSettingsWindow = (event: StorageEvent) => {
      if (event.key === STORAGE_KEY) {
        const nextSettings = loadSettings();
        setSettings((current) => {
          if (isSameSettings(current, nextSettings)) return current;
          readFromStorage.current = nextSettings;
          return nextSettings;
        });
      }
    };

    window.addEventListener("storage", syncFromSettingsWindow);
    return () => window.removeEventListener("storage", syncFromSettingsWindow);
  }, []);

  // Yunzhao's settings page writes one changed field at a time. Merge it here so the page does not
  // have to open this process's own settings window.
  useEffect(() => {
    if (getCurrentWindow().label !== "main") return;
    let lastAt = 0;
    const timer = window.setInterval(() => {
      void invoke<Record<string, unknown> | null>("read_host_island_settings")
        .then((patch) => {
          if (!patch) return;
          const at = Number(patch.at ?? 0);
          if (at && at === lastAt) return;
          if (at) lastAt = at;
          applyHostedIslandCommand(patch);
          setSettings((current) => {
            const next = { ...current };
            let changed = false;
            for (const [key, value] of Object.entries(patch)) {
              if (key === "at" || !(key in current)) continue;
              const same = Array.isArray(value) && Array.isArray(current[key as keyof IslandSettings])
                ? value.join() === (current[key as keyof IslandSettings] as unknown[]).join()
                : current[key as keyof IslandSettings] === value;
              if (same) continue;
              (next as Record<string, unknown>)[key] = value;
              changed = true;
            }
            if (typeof patch.applyAppearancePreset === "string") {
              const preset = loadAppearancePresets().find((item) => item.id === patch.applyAppearancePreset);
              if (preset) return applyAppearancePreset(current, preset);
            }
            if (typeof patch.saveAppearancePreset === "string") {
              const presets = loadAppearancePresets();
              const next = addAppearancePreset(presets, patch.saveAppearancePreset, current, crypto.randomUUID());
              if (next !== presets) saveAppearancePresets(next);
              return { ...current };
            }
            if (typeof patch.deleteAppearancePreset === "string") return { ...current };
            return changed ? next : current;
          });
        })
        .catch(() => {});
    }, 300);
    return () => window.clearInterval(timer);
  }, []);

  return [settings, setSettings] as const;
}

/** Settings that live beside the island's own settings object: language, startup, translation and presets. */
function applyHostedIslandCommand(patch: Record<string, unknown>) {
  if (patch.locale === "zh-CN" || patch.locale === "en") saveLocale(patch.locale);
  if (typeof patch.detachedSettings === "boolean") saveDetachedSettings(patch.detachedSettings);
  if (typeof patch.launchAtStartup === "boolean") void saveLaunchAtStartup(patch.launchAtStartup).catch(() => {});
  if (patch.resetPosition === true) requestIslandPositionReset();
  if (patch.quitIsland === true) void quitIsland().catch(() => {});
  if (patch.checkUpdate === true) void checkForUpdate().catch(() => {});
  if (typeof patch.autoUpdate === "boolean") setAutoUpdate(patch.autoUpdate);
  if (typeof patch.deleteAppearancePreset === "string") {
    saveAppearancePresets(removeAppearancePreset(loadAppearancePresets(), patch.deleteAppearancePreset));
  }
  const translation = loadTranslationSettings();
  let translationChanged = false;
  const nextTranslation = { ...translation, models: { ...translation.models } };
  if (typeof patch.translationProvider === "string" && isTranslationProvider(patch.translationProvider)) {
    nextTranslation.provider = patch.translationProvider;
    translationChanged = true;
  }
  if (typeof patch.translationBaseUrl === "string") {
    nextTranslation.customBaseUrl = patch.translationBaseUrl;
    translationChanged = true;
  }
  if (typeof patch.translationModel === "string") {
    nextTranslation.models[nextTranslation.provider] = patch.translationModel;
    translationChanged = true;
  }
  if (typeof patch.translationShortcut === "string" && patch.translationShortcut.trim()) {
    nextTranslation.shortcut = patch.translationShortcut.trim();
    translationChanged = true;
    void setTranslationShortcut(nextTranslation.shortcut).catch(() => {});
  }
  if (translationChanged) {
    saveTranslationSettings(nextTranslation);
    window.dispatchEvent(new Event("focusd-island-translation-settings-saved"));
  }
  if (typeof patch.translationSecret === "string" && patch.translationSecret.trim()) {
    void saveTranslationSecret(patch.translationSecret.trim(), nextTranslation.provider).catch(() => {});
  }
  window.dispatchEvent(new CustomEvent("yunzhao-host-extra", { detail: patch }));
}
