import { useEffect, useState, type FormEvent } from "react";
import { SettingsGroup } from "../../components/settings/SettingsGroup";
import { SettingsRow } from "../../components/settings/SettingsRow";
import { useTranslation } from "../../i18n";
import { openSettingsWindow } from "../../windows/settings/api";
import { copyVaultLogin, openVaultEditor, rememberVaultPassword } from "./api";
import { changeVaultPassword, createVault, type VaultRecord } from "./crypto";
import { loadVault, saveVault } from "./storage";
import { removeVaultEntry, subscribeVaultSession, unlockVaultSession, visibleEntries } from "./session";
import "./VaultPanel.css";

export function VaultPanel({ surface }: { surface: "island" | "settings" }) {
  const t = useTranslation();
  const copy = t.vault;
  const [record, setRecord] = useState<VaultRecord | null>(() => loadVault());
  const [setup, setSetup] = useState({ next: "", confirm: "" });
  const [change, setChange] = useState({ current: "", next: "", confirm: "" });
  const [formError, setFormError] = useState<string | null>(null);
  const [viewPassword, setViewPassword] = useState("");
  const [copiedId, setCopiedId] = useState<string | null>(null);

  useEffect(() => {
    if (!copiedId) return;
    const timer = window.setTimeout(() => setCopiedId(null), 1500);
    return () => window.clearTimeout(timer);
  }, [copiedId]);

  useEffect(() => subscribeVaultSession(() => setRecord(loadVault())), []);

  const persist = (next: VaultRecord) => {
    if (!saveVault(next)) {
      setFormError(copy.saveFailed);
      return false;
    }
    setRecord(next);
    setFormError(null);
    return true;
  };

  const submitSetup = async (event: FormEvent) => {
    event.preventDefault();
    if (setup.next.length < 4) return setFormError(copy.tooShort);
    if (setup.next !== setup.confirm) return setFormError(copy.mismatch);
    const password = setup.next;
    const created = await createVault(password);
    if (!persist(created)) return;
    await unlockVaultSession(created, password);
    void rememberVaultPassword(password).catch(() => {});
    setSetup({ next: "", confirm: "" });
  };

  const submitChange = async (event: FormEvent) => {
    event.preventDefault();
    if (!record) return;
    if (change.next.length < 4) return setFormError(copy.tooShort);
    if (change.next !== change.confirm) return setFormError(copy.mismatch);
    const next = await changeVaultPassword(record, change.current, change.next);
    if (!next) return setFormError(copy.wrongPassword);
    if (persist(next)) {
      await unlockVaultSession(next, change.next);
      void rememberVaultPassword(change.next).catch(() => {});
      setChange({ current: "", next: "", confirm: "" });
    }
  };

  const passwordFields = record ? (
    <form className="vault-form" onSubmit={(event) => void submitChange(event)}>
      <PasswordField label={copy.currentPassword} value={change.current} onChange={(current) => setChange({ ...change, current })} />
      <PasswordField label={copy.newPassword} value={change.next} onChange={(next) => setChange({ ...change, next })} />
      <PasswordField label={copy.confirmPassword} value={change.confirm} onChange={(confirm) => setChange({ ...change, confirm })} />
      {formError && <p className="vault-panel__error" role="alert">{formError}</p>}
      <button className="vault-primary" type="submit">{copy.changePassword}</button>
    </form>
  ) : (
    <form className="vault-form" onSubmit={(event) => void submitSetup(event)}>
      <PasswordField label={copy.setPassword} value={setup.next} onChange={(next) => setSetup({ ...setup, next })} />
      <PasswordField label={copy.confirmPassword} value={setup.confirm} onChange={(confirm) => setSetup({ ...setup, confirm })} />
      {formError && <p className="vault-panel__error" role="alert">{formError}</p>}
      <button className="vault-primary" type="submit">{copy.savePassword}</button>
    </form>
  );

  if (surface === "settings") {
    return (
      <SettingsGroup title={copy.passwordTitle} footer={record ? copy.passwordSet : copy.settingsHint}>
        <SettingsRow title={copy.passwordTitle} description={copy.settingsHint}>
          {passwordFields}
        </SettingsRow>
      </SettingsGroup>
    );
  }

  if (!record) {
    return (
      <section className="vault-panel" aria-label={copy.passwordTitle}>
        <p className="vault-panel__hint">{copy.locked}</p>
        {passwordFields}
        <button type="button" onClick={() => openSettingsWindow("vault")}>{copy.openSettings}</button>
      </section>
    );
  }

  const rows = visibleEntries(record);
  if (!rows) {
    const submitUnlock = async (event: FormEvent) => {
      event.preventDefault();
      if (await unlockVaultSession(record, viewPassword)) {
        void rememberVaultPassword(viewPassword).catch(() => {});
        setViewPassword("");
        setFormError(null);
        return;
      }
      setFormError(copy.wrongPassword);
    };
    return (
      <section className="vault-panel" aria-label={copy.passwordTitle}>
        <p className="vault-panel__hint">{copy.unlockHint}</p>
        <form className="vault-form" onSubmit={(event) => void submitUnlock(event)}>
          <PasswordField label={copy.passwordTitle} value={viewPassword} onChange={setViewPassword} />
          {formError && <p className="vault-panel__error" role="alert">{formError}</p>}
          <button className="vault-primary" type="submit">{copy.unlock}</button>
        </form>
      </section>
    );
  }

  return (
    <section className="vault-panel" aria-label={copy.account}>
      <button className="vault-primary" type="button" onClick={() => void openVaultEditor().catch(() => setFormError(copy.editorFailed))}>{copy.addAccount}</button>
      {formError && <p className="vault-panel__error" role="alert">{formError}</p>}
      {rows.length === 0 ? <p className="vault-panel__hint">{copy.empty}</p> : (
        <table className="vault-table">
          <thead>
            <tr>
              <th className="vault-index">{copy.index}</th>
              <th>{copy.purpose}</th>
              <th>{copy.account}</th>
              <th>{copy.secret}</th>
              <th className="vault-actions" />
            </tr>
          </thead>
          <tbody>
            {rows.map((entry, index) => (
              <tr key={entry.id}>
                <td>{index + 1}</td>
                <td title={entry.purpose}>{entry.purpose}</td>
                <td title={entry.account}>{entry.account}</td>
                <td className="vault-secret" title={entry.secret}>{entry.secret}</td>
                <td>
                  <div className="vault-row-actions">
                    <button type="button" onClick={() => { if (!removeVaultEntry(entry.id)) setFormError(copy.saveFailed); }}>{copy.remove}</button>
                    <button type="button" onClick={() => {
                      void copyVaultLogin(entry.account, entry.secret).then(() => {
                        setCopiedId(entry.id);
                        setFormError(null);
                      }).catch(() => setFormError(copy.copyFailed));
                    }}>{copiedId === entry.id ? copy.copied : copy.copyLogin}</button>
                  </div>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </section>
  );
}

function PasswordField({ label, value, onChange }: { label: string; value: string; onChange: (value: string) => void }) {
  return <label>{label}<input type="password" autoComplete="off" value={value} onChange={(event) => onChange(event.target.value)} /></label>;
}
