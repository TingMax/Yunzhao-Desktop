import { KeyRound, Languages, Music2, type LucideIcon } from "lucide-react";
import type { PageDot } from "./types";

export const PAGE_ICONS: Record<PageDot, LucideIcon> = {
  music: Music2,
  vault: KeyRound,
  translation: Languages,
};
