import { useCallback, useEffect, useLayoutEffect, useRef, useState } from "react";
import { isTauri } from "@tauri-apps/api/core";
import { useTranslation } from "../../../i18n";
import type { MessagePreview } from "../messagePreview";
import { clearWeComReminders, getWeComReminderState, launchWeCom, listenWeComReminderChanged, setWeComRemindersEnabled, type WeComSnapshot } from "./api";
import { weComOpenErrorText } from "./openError";
import { isWeComRemindersEnabled, saveWeComRemindersEnabled } from "./storage";

const EMPTY_SNAPSHOT: WeComSnapshot = {
  revision: 0, enabled: false, status: "disabled", pending: false, lastAttentionAt: null, error: null,
};

export function useWeComReminders(canPreview: boolean, { restore = true, previewSeconds = 5 } = {}) {
  const t = useTranslation();
  const [snapshot, setSnapshot] = useState(EMPTY_SNAPSHOT);
  const [busy, setBusy] = useState(isTauri());
  const [actionError, setActionError] = useState<string | null>(null);
  const [previewWindow, setPreviewWindow] = useState<{ receivedAt: number; expiresAt: number } | null>(null);
  const previewSecondsRef = useRef(previewSeconds);
  const canPreviewRef = useRef(canPreview);
  const revisionRef = useRef(-1);
  const attentionRef = useRef<number | null>(null);
  const actionPending = useRef(false);

  useLayoutEffect(() => { previewSecondsRef.current = previewSeconds; }, [previewSeconds]);
  useLayoutEffect(() => {
    canPreviewRef.current = canPreview;
    if (!canPreview) setPreviewWindow(null);
  }, [canPreview]);

  const acceptSnapshot = useCallback((next: WeComSnapshot) => {
    if (next.revision < revisionRef.current) return false;
    revisionRef.current = next.revision;
    attentionRef.current = next.lastAttentionAt;
    setSnapshot(next);
    return true;
  }, []);

  useEffect(() => {
    if (!isTauri()) return;
    let cancelled = false;
    let unlisten: (() => void) | undefined;
    void (async () => {
      unlisten = await listenWeComReminderChanged(({ payload }) => {
        const newlyPending = payload.snapshot.pending && payload.snapshot.lastAttentionAt !== attentionRef.current;
        if (cancelled || !acceptSnapshot(payload.snapshot)) return;
        if (!payload.snapshot.enabled || !payload.snapshot.pending || payload.snapshot.status !== "listening" || !payload.windowVisible || !canPreviewRef.current) {
          setPreviewWindow(null);
        } else if (newlyPending) {
          setPreviewWindow((previous) => {
            const receivedAt = Math.max(Date.now(), (previous?.receivedAt ?? 0) + 1);
            return { receivedAt, expiresAt: receivedAt + previewSecondsRef.current * 1_000 };
          });
        }
      });
      if (cancelled) { unlisten(); return; }
      const next = restore ? await setWeComRemindersEnabled(isWeComRemindersEnabled()) : await getWeComReminderState();
      if (!cancelled) acceptSnapshot(next);
    })().catch((error: unknown) => {
      if (!cancelled) setActionError(String(error));
    }).finally(() => {
      if (!cancelled) setBusy(false);
    });
    return () => { cancelled = true; unlisten?.(); };
  }, [acceptSnapshot, restore]);

  useEffect(() => {
    if (previewWindow === null) return;
    const timer = window.setTimeout(() => setPreviewWindow(null), Math.max(0, previewWindow.expiresAt - Date.now()));
    return () => window.clearTimeout(timer);
  }, [previewWindow]);

  const run = useCallback(async (action: () => Promise<void | boolean>) => {
    if (actionPending.current) return false;
    if (!isTauri()) {
      setActionError(t.wecom.desktopOnly);
      return false;
    }
    actionPending.current = true;
    setBusy(true);
    setActionError(null);
    try {
      return (await action()) !== false;
    } catch (error) {
      setActionError(weComOpenErrorText(error, t.wecom.openErrors));
      return false;
    } finally {
      actionPending.current = false;
      setBusy(false);
    }
  }, [t]);

  const setEnabled = useCallback((enabled: boolean) => run(async () => {
    acceptSnapshot(await setWeComRemindersEnabled(enabled));
    saveWeComRemindersEnabled(enabled);
    if (!enabled) setPreviewWindow(null);
  }), [acceptSnapshot, run]);

  const clear = useCallback(() => run(async () => {
    acceptSnapshot(await clearWeComReminders());
    setPreviewWindow(null);
  }), [acceptSnapshot, run]);

  const openWeCom = useCallback(() => run(async () => {
    await launchWeCom();
    acceptSnapshot(await clearWeComReminders());
    setPreviewWindow(null);
  }), [acceptSnapshot, run]);

  const preview: MessagePreview | null = canPreview && snapshot.pending && previewWindow !== null ? {
    source: "wecom",
    title: t.messages.wecom,
    body: t.messages.wecomPreview,
    receivedAt: previewWindow.receivedAt,
  } : null;

  return { snapshot, busy, actionError, clear, setEnabled, openWeCom, preview };
}

export type WeComRemindersController = ReturnType<typeof useWeComReminders>;
