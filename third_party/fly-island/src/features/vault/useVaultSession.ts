import { useEffect } from "react";
import { isTauri } from "@tauri-apps/api/core";
import { closeVaultEditor, listenVaultDraft, listenWorkstationLocked, notifyVaultDraftRejected, readVaultPassword } from "./api";
import { loadVault } from "./storage";
import { acceptVaultDraft, lockVaultSession, retainVaultSession, unlockVaultSession } from "./session";

/** The account table stays open until Windows locks. A lock hides the passwords again. */
export function useVaultSession() {
  useEffect(() => {
    if (!isTauri()) return;
    void readVaultPassword().then(async (password) => {
      const record = loadVault();
      if (password && record) await unlockVaultSession(record, password);
    }).catch(() => {});
    const locked = listenWorkstationLocked(() => {
      lockVaultSession();
      void closeVaultEditor().catch(() => {});
    });
    const drafts = listenVaultDraft((event) => {
      void acceptVaultDraft(event.payload).then((saved) => {
        if (saved) void closeVaultEditor().catch(() => {});
        else void notifyVaultDraftRejected().catch(() => {});
      });
    });
    return () => {
      void locked.then((stop) => stop());
      void drafts.then((stop) => stop());
    };
  }, []);

  useEffect(() => {
    retainVaultSession();
    const onStorage = () => retainVaultSession();
    window.addEventListener("storage", onStorage);
    return () => window.removeEventListener("storage", onStorage);
  }, []);
}
