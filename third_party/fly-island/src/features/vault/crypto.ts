const VERIFIER = "yunzhao-vault";
const ITERATIONS = 100_000;

export type VaultEntry = {
  id: string;
  purpose: string;
  account: string;
  secret: string;
};

export type VaultRecord = {
  salt: string;
  check: string;
  entries: VaultEntry[];
};

function bytesToBase64(bytes: Uint8Array): string {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
}

function base64ToBytes(value: string): Uint8Array {
  const binary = atob(value);
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) bytes[index] = binary.charCodeAt(index);
  return bytes;
}

async function deriveKey(password: string, salt: Uint8Array): Promise<CryptoKey> {
  const material = await crypto.subtle.importKey(
    "raw",
    new TextEncoder().encode(password),
    "PBKDF2",
    false,
    ["deriveKey"],
  );
  return crypto.subtle.deriveKey(
    { name: "PBKDF2", salt, iterations: ITERATIONS, hash: "SHA-256" },
    material,
    { name: "AES-GCM", length: 256 },
    false,
    ["encrypt", "decrypt"],
  );
}

export async function encryptText(key: CryptoKey, text: string): Promise<string> {
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const cipher = await crypto.subtle.encrypt(
    { name: "AES-GCM", iv },
    key,
    new TextEncoder().encode(text),
  );
  return `${bytesToBase64(iv)}.${bytesToBase64(new Uint8Array(cipher))}`;
}

export async function decryptText(key: CryptoKey, packed: string): Promise<string> {
  const [iv, cipher] = packed.split(".");
  if (!iv || !cipher) throw new Error("bad-cipher");
  const plain = await crypto.subtle.decrypt(
    { name: "AES-GCM", iv: base64ToBytes(iv) },
    key,
    base64ToBytes(cipher),
  );
  return new TextDecoder().decode(plain);
}

export async function createVault(password: string): Promise<VaultRecord> {
  const salt = crypto.getRandomValues(new Uint8Array(16));
  const key = await deriveKey(password, salt);
  return { salt: bytesToBase64(salt), check: await encryptText(key, VERIFIER), entries: [] };
}

/** The key that opens this vault, or null when the password is wrong. */
export async function unlockVault(record: VaultRecord, password: string): Promise<CryptoKey | null> {
  try {
    const key = await deriveKey(password, base64ToBytes(record.salt));
    return (await decryptText(key, record.check)) === VERIFIER ? key : null;
  } catch {
    return null;
  }
}

/** Rewraps every secret with a new password. Null when the current password is wrong. */
export async function changeVaultPassword(record: VaultRecord, current: string, next: string): Promise<VaultRecord | null> {
  const key = await unlockVault(record, current);
  if (!key) return null;
  const plains = await Promise.all(record.entries.map((entry) => decryptText(key, entry.secret)));
  const created = await createVault(next);
  const nextKey = await unlockVault(created, next);
  if (!nextKey) return null;
  const entries = await Promise.all(record.entries.map(async (entry, index) => ({
    ...entry,
    secret: await encryptText(nextKey, plains[index] ?? ""),
  })));
  return { ...created, entries };
}
