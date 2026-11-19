import { readJson, writeJson } from "../../lib/storage";
import type { VaultEntry, VaultRecord } from "./crypto";

const VAULT_KEY = "yunzhao-island-vault";

function isEntry(value: unknown): value is VaultEntry {
  if (!value || typeof value !== "object") return false;
  const entry = value as VaultEntry;
  return typeof entry.id === "string"
    && typeof entry.purpose === "string"
    && typeof entry.account === "string"
    && typeof entry.secret === "string";
}

export function loadVault(): VaultRecord | null {
  const value = readJson(VAULT_KEY);
  if (!value || typeof value !== "object") return null;
  const record = value as Partial<VaultRecord>;
  if (typeof record.salt !== "string" || typeof record.check !== "string" || !Array.isArray(record.entries)) return null;
  return { salt: record.salt, check: record.check, entries: record.entries.filter(isEntry) };
}

export function saveVault(record: VaultRecord): boolean {
  return writeJson(VAULT_KEY, record);
}
