import { useEffect, useState, type FormEvent } from "react";
import { useTranslation } from "../../i18n";
import { closeVaultEditor, listenVaultDraftRejected, listenVaultEditorReset, showVaultEditor, submitVaultEntry } from "./api";
import { parseLoginPaste } from "./paste";
import "./VaultPanel.css";
import "./VaultEditor.css";

const EMPTY = { purpose: "", account: "", secret: "" };

/** Add-account form, shown in its own window centered on the monitor. */
export function VaultEditor() {
  const t = useTranslation();
  const copy = t.vault;
  const [draft, setDraft] = useState(EMPTY);
  const [pasted, setPasted] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  useEffect(() => {
    let stopReset = () => {};
    let stopRejected = () => {};
    let alive = true;
    void (async () => {
      const reset = await listenVaultEditorReset(() => {
        setDraft(EMPTY);
        setPasted("");
        setError(null);
        setSaving(false);
      });
      const rejected = await listenVaultDraftRejected(() => {
        setSaving(false);
        setError(copy.saveFailed);
      });
      if (!alive) {
        reset();
        rejected();
        return;
      }
      stopReset = reset;
      stopRejected = rejected;
      await showVaultEditor(document.documentElement.dataset.theme !== "light", copy.editorTitle);
    })().catch(() => setError(copy.saveFailed));
    return () => {
      alive = false;
      stopReset();
      stopRejected();
    };
  }, [copy.editorTitle, copy.saveFailed]);

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.key === "Escape") void closeVaultEditor().catch(() => {});
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);

  const applyPasted = (value: string) => {
    const parsed = parseLoginPaste(value);
    if (parsed) {
      setDraft({ purpose: parsed.purpose, account: parsed.account, secret: parsed.secret });
      setPasted("");
      setError(null);
      return;
    }
    setPasted(value);
    const tried = value.includes("\n") || value.includes("：") || value.includes(":");
    setError(tried ? copy.pasteUnrecognized : null);
  };

  const submit = async (event: FormEvent) => {
    event.preventDefault();
    if (!draft.account.trim() || !draft.secret) {
      setError(copy.required);
      return;
    }
    setSaving(true);
    setError(null);
    try {
      await submitVaultEntry(draft);
    } catch {
      setSaving(false);
      setError(copy.saveFailed);
    }
  };

  return (
    <form className="vault-editor vault-form" onSubmit={(event) => void submit(event)}>
      <h1>{copy.editorTitle}</h1>
      <label>{copy.paste}<textarea autoFocus placeholder={copy.pasteHint} value={pasted} onChange={(event) => applyPasted(event.target.value)} /></label>
      <label>{copy.purpose}<input autoComplete="off" value={draft.purpose} onChange={(event) => setDraft({ ...draft, purpose: event.target.value })} /></label>
      <label>{copy.account}<input autoComplete="off" value={draft.account} onChange={(event) => setDraft({ ...draft, account: event.target.value })} /></label>
      <label>{copy.secret}<input autoComplete="off" value={draft.secret} onChange={(event) => setDraft({ ...draft, secret: event.target.value })} /></label>
      {error && <p className="vault-panel__error" role="alert">{error}</p>}
      <div className="vault-panel__actions">
        <button className="vault-primary" type="submit" disabled={saving}>{copy.save}</button>
        <button type="button" onClick={() => void closeVaultEditor().catch(() => {})} disabled={saving}>{copy.cancel}</button>
      </div>
    </form>
  );
}
