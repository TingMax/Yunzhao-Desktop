import { invoke } from "@tauri-apps/api/core";
import { emitTo, listen, type EventCallback } from "@tauri-apps/api/event";

// Mirrors the event names in src-tauri/src/vault_editor/mod.rs.
const DRAFT_EVENT = "vault-entry-draft";
const RESET_EVENT = "vault-editor-reset";
const REJECTED_EVENT = "vault-entry-rejected";
const EDITOR_WINDOW = "vault-editor";

export type VaultDraft = { purpose: string; account: string; secret: string };

export function openVaultEditor() {
  return invoke<void>("open_vault_editor");
}

export function showVaultEditor(dark: boolean, title: string) {
  return invoke<void>("show_vault_editor", { dark, title });
}

export function closeVaultEditor() {
  return invoke<void>("close_vault_editor");
}

/** Copies this row's account and password, and nothing else. */
export function copyVaultLogin(account: string, secret: string) {
  return invoke<void>("copy_vault_login", { account, secret });
}

export function rememberVaultPassword(password: string) {
  return invoke<void>("remember_vault_password", { password });
}

export function readVaultPassword() {
  return invoke<string | null>("read_vault_password");
}

export const WORKSTATION_LOCKED_EVENT = "vault-workstation-locked";

export function listenWorkstationLocked(handler: () => void) {
  return listen(WORKSTATION_LOCKED_EVENT, handler);
}

export function submitVaultEntry(draft: VaultDraft) {
  return invoke<void>("submit_vault_entry", draft);
}

export function listenVaultDraft(handler: EventCallback<VaultDraft>) {
  return listen<VaultDraft>(DRAFT_EVENT, handler);
}

export function listenVaultEditorReset(handler: () => void) {
  return listen(RESET_EVENT, handler);
}

export function listenVaultDraftRejected(handler: () => void) {
  return listen(REJECTED_EVENT, handler);
}

export function notifyVaultDraftRejected() {
  return emitTo(EDITOR_WINDOW, REJECTED_EVENT, {});
}
