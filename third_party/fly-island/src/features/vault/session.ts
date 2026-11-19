import { decryptText, encryptText, unlockVault, type VaultRecord } from "./crypto";
import { loadVault, saveVault } from "./storage";

export type VisibleEntry = { id: string; purpose: string; account: string; secret: string };

type Session = {
  salt: string;
  check: string;
  key: CryptoKey;
  secrets: Map<string, string>;
};

let session: Session | null = null;
const listeners = new Set<() => void>();

function matches(record: VaultRecord): boolean {
  return session !== null && session.salt === record.salt && session.check === record.check;
}

function emit() {
  listeners.forEach((listener) => listener());
}

export function subscribeVaultSession(listener: () => void) {
  listeners.add(listener);
  return () => { listeners.delete(listener); };
}

/** Drops the open passwords when the saved vault is no longer the one they unlocked. */
export function retainVaultSession() {
  if (!session) return;
  const record = loadVault();
  if (!record || session.salt !== record.salt || session.check !== record.check) lockVaultSession();
}

/** Drops every revealed password. Opening the account page asks for the view password again. */
export function lockVaultSession() {
  if (!session) return;
  session = null;
  emit();
}

/** Plain passwords for the open island, or null when the view password is required again. */
export function visibleEntries(record: VaultRecord): VisibleEntry[] | null {
  const current = session;
  if (!current || current.salt !== record.salt || current.check !== record.check) return null;
  return record.entries.map((entry) => ({
    id: entry.id,
    purpose: entry.purpose,
    account: entry.account,
    secret: current.secrets.get(entry.id) ?? "",
  }));
}

export async function unlockVaultSession(record: VaultRecord, password: string): Promise<boolean> {
  const key = await unlockVault(record, password);
  if (!key) return false;
  const secrets = new Map<string, string>();
  try {
    for (const entry of record.entries) secrets.set(entry.id, await decryptText(key, entry.secret));
  } catch {
    return false;
  }
  session = { salt: record.salt, check: record.check, key, secrets };
  emit();
  return true;
}

export async function acceptVaultDraft(draft: { purpose: string; account: string; secret: string }): Promise<boolean> {
  const purpose = draft.purpose.trim();
  const account = draft.account.trim();
  if (!account || !draft.secret) return false;
  const record = loadVault();
  const current = session;
  if (!record || !current || current.salt !== record.salt || current.check !== record.check) return false;
  const secret = await encryptText(current.key, draft.secret);
  if (session !== current) return false;
  const entry = { id: crypto.randomUUID(), purpose, account, secret };
  if (!saveVault({ ...record, entries: [...record.entries, entry] })) return false;
  current.secrets.set(entry.id, draft.secret);
  emit();
  return true;
}

export function removeVaultEntry(id: string): boolean {
  const record = loadVault();
  if (!record || !session || !matches(record)) return false;
  if (!saveVault({ ...record, entries: record.entries.filter((entry) => entry.id !== id) })) return false;
  session.secrets.delete(id);
  emit();
  return true;
}
