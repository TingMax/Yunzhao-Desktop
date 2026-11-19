import { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import type { MessagePreview } from "./messagePreview";

type HostNotice = { app: string; title: string; body: string; at: number };

const VISIBLE_MS = 8000;

/** Yunzhao writes each notice to a file. The island shows the latest one on the collapsed bar. */
export function useHostNotice(enabled: boolean): MessagePreview | null {
  const [notice, setNotice] = useState<HostNotice | null>(null);
  useEffect(() => {
    if (!enabled) return;
    let stop = false;
    const read = () => {
      void invoke<HostNotice | null>("read_host_notice")
        .then((next) => {
          if (!stop) setNotice(next);
        })
        .catch(() => {
          if (!stop) setNotice(null);
        });
    };
    read();
    const timer = window.setInterval(read, 1000);
    return () => {
      stop = true;
      window.clearInterval(timer);
    };
  }, [enabled]);
  if (!enabled || !notice || notice.at <= 0 || Date.now() - notice.at > VISIBLE_MS) return null;
  const title = notice.app.trim() || notice.title.trim() || "通知";
  const body = notice.body.trim() || notice.title.trim();
  return { source: "plugin", title, body, receivedAt: notice.at, host: true };
}
