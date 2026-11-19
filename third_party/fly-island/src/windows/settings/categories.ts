import { PLUGINS_ENABLED } from "../../features/releaseFlags.ts";

/** Includes unavailable pages in the type so saved category names remain compatible. */
const ALL_SETTINGS_CATEGORIES = [
  "general",
  "appearance",
  "music",
  "translation",
  "vault",
  "messages",
  "agents",
  "agentConnect",
  "plugins",
  "about",
] as const;

export type SettingsCategory = (typeof ALL_SETTINGS_CATEGORIES)[number];
export const SETTINGS_CATEGORIES = ALL_SETTINGS_CATEGORIES.filter((category) => PLUGINS_ENABLED || category !== "plugins");

export type SettingsGroup = "basics" | "features" | "agents";

/**
 * Sidebar sections: app-wide settings, one page per module (QQ, WeChat and Mail share Messages,
 * the file shelf shares Clipboard), the two AI Agent pages (sessions and alerts, connection). A
 * null label sets Plugins and About apart as the last group of the list, below Connection.
 */
export const SETTINGS_GROUPS: readonly { label: SettingsGroup | null; categories: readonly SettingsCategory[] }[] = [
  { label: "basics", categories: ["general", "appearance"] },
  { label: "features", categories: ["music", "translation", "vault", "messages"] },
  { label: null, categories: PLUGINS_ENABLED ? ["plugins", "about"] : ["about"] },
];

export const DEFAULT_SETTINGS_CATEGORY: SettingsCategory = "general";

/** Pages that were merged into another, by their old name: what the island or a link still asks for. */
const MERGED_CATEGORIES: Readonly<Record<string, SettingsCategory>> = {
  qq: "messages",
  wechat: "messages",
  mail: "messages",
  updates: "about",
  shelf: "general",
  todos: "general",
  clipboard: "general",
  agentAlerts: "general",
  sounds: "general",
  agentApprovals: "general",
  agents: "general",
  agentConnect: "general",
};

/** The page a category name opens, old names included; null when there is no such page. */
export function settingsCategoryFor(name: string): SettingsCategory | null {
  if (name === "agents" || name === "agentConnect" || name === "agentAlerts" || name === "sounds" || name === "agentApprovals") return "general";
  if ((SETTINGS_CATEGORIES as readonly string[]).includes(name)) return name as SettingsCategory;
  return MERGED_CATEGORIES[name] ?? null;
}
