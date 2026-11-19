export type ParsedLogin = { purpose: string; account: string; secret: string };

const ACCOUNT = "账号|帐号|账户|用户名|用户|邮箱|email|e-mail|account|username|user";
const SECRET = "密码|口令|password|pass|pwd";
const PURPOSE = "用途|备注|网站|名称|标题|purpose|note|site|title|name";
const LABELS = `${ACCOUNT}|${SECRET}|${PURPOSE}`;

function kind(label: string): keyof ParsedLogin | null {
  if (new RegExp(`^(?:${ACCOUNT})$`, "i").test(label)) return "account";
  if (new RegExp(`^(?:${SECRET})$`, "i").test(label)) return "secret";
  if (new RegExp(`^(?:${PURPOSE})$`, "i").test(label)) return "purpose";
  return null;
}

function fromLabels(text: string): ParsedLogin | null {
  const found: ParsedLogin = { purpose: "", account: "", secret: "" };
  const pattern = new RegExp(
    `(${LABELS})(?![A-Za-z0-9])\\s*[:：]\\s*([\\s\\S]*?)(?=(?:${LABELS})(?![A-Za-z0-9])\\s*[:：]|$)`,
    "gi",
  );
  for (const match of text.matchAll(pattern)) {
    const field = kind(match[1] ?? "");
    const value = (match[2] ?? "").replace(/\s+/g, " ").trim();
    if (field && value) found[field] = value;
  }
  if (!found.account || !found.secret) return null;
  return found;
}

function fromLines(lines: string[]): ParsedLogin | null {
  if (lines.length === 2) return { purpose: "", account: lines[0], secret: lines[1] };
  if (lines.length === 3) return { purpose: lines[0], account: lines[1], secret: lines[2] };
  if (lines.length === 1 && lines[0].includes("\t")) {
    const parts = lines[0].split("\t").map((part) => part.trim()).filter(Boolean);
    if (parts.length === 2) return { purpose: "", account: parts[0], secret: parts[1] };
    if (parts.length >= 3) return { purpose: parts[0], account: parts[1], secret: parts[2] };
  }
  if (lines.length === 1) {
    const parts = lines[0].split(/\s+/);
    if (parts.length === 2) return { purpose: "", account: parts[0], secret: parts[1] };
  }
  return null;
}

/** Reads an account and password out of pasted text. Anything else stays empty. */
export function parseLoginPaste(text: string): ParsedLogin | null {
  const trimmed = text.replace(/\r\n/g, "\n").trim();
  if (!trimmed || trimmed.length > 2000) return null;
  const labeled = fromLabels(trimmed);
  if (labeled) return labeled;
  const lines = trimmed.split("\n").map((line) => line.trim()).filter(Boolean);
  if (lines.some((line) => line.length > 300)) return null;
  const parsed = fromLines(lines);
  if (!parsed?.account || !parsed.secret) return null;
  return parsed;
}
