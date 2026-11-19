export type VaultText = {
  passwordTitle: string;
  settingsHint: string;
  unlockHint: string;
  unlock: string;
  addAccount: string;
  editorTitle: string;
  save: string;
  setPassword: string;
  confirmPassword: string;
  currentPassword: string;
  newPassword: string;
  savePassword: string;
  changePassword: string;
  passwordSet: string;
  mismatch: string;
  tooShort: string;
  wrongPassword: string;
  saveFailed: string;
  locked: string;
  openSettings: string;
  purpose: string;
  account: string;
  secret: string;
  index: string;
  remove: string;
  copyLogin: string;
  copied: string;
  copyFailed: string;
  paste: string;
  pasteHint: string;
  pasteUnrecognized: string;
  editorFailed: string;
  empty: string;
  required: string;
  cancel: string;
};

export const zhCN: VaultText = {
  passwordTitle: "查看密码",
  settingsHint: "查看密码在电脑保持解锁时会记住。电脑锁屏再解锁后，需要重新输入。密码只保存在这台电脑上，忘记后无法恢复。",
  unlockHint: "电脑锁屏后再查看账号，需要输入查看密码。通过后，全部密码一起显示。",
  unlock: "查看",
  addAccount: "添加账号密码",
  editorTitle: "添加账号",
  save: "保存",
  setPassword: "设置查看密码",
  confirmPassword: "再输入一次",
  currentPassword: "当前查看密码",
  newPassword: "新的查看密码",
  savePassword: "保存查看密码",
  changePassword: "修改查看密码",
  passwordSet: "查看密码已设置。账号表在灵动岛的账号页。",
  mismatch: "两次输入的密码不一致。",
  tooShort: "查看密码至少 4 位。",
  wrongPassword: "查看密码不正确。",
  saveFailed: "没能保存，请重试。",
  locked: "使用账号表之前，先设置一个查看密码。",
  openSettings: "去设置查看密码",
  purpose: "用途",
  account: "账号",
  secret: "密码",
  index: "序号",
  remove: "删除",
  copyLogin: "复制",
  copied: "已复制",
  copyFailed: "没能复制，请重试。",
  paste: "粘贴识别",
  pasteHint: "把账号和密码粘贴到这里，会自动填入下面",
  pasteUnrecognized: "没能识别账号和密码，请分开填写。",
  editorFailed: "没能打开添加窗口，请重试。",
  empty: "还没有账号。",
  required: "账号和密码都要填写。",
  cancel: "取消",
};

export const en: VaultText = {
  passwordTitle: "View password",
  settingsHint: "The view password stays remembered while this computer is unlocked. After the screen locks and unlocks, it is asked again. Passwords stay on this computer. A forgotten password cannot recover what was saved.",
  unlockHint: "After the screen locks, viewing accounts asks for the view password. Then every password is shown.",
  unlock: "Show",
  addAccount: "Add account",
  editorTitle: "Add account",
  save: "Save",
  setPassword: "Set a view password",
  confirmPassword: "Enter it again",
  currentPassword: "Current view password",
  newPassword: "New view password",
  savePassword: "Save view password",
  changePassword: "Change view password",
  passwordSet: "A view password is set. The account table is on the island's Accounts page.",
  mismatch: "The two passwords do not match.",
  tooShort: "Use at least 4 characters.",
  wrongPassword: "That view password is wrong.",
  saveFailed: "Could not save. Try again.",
  locked: "Set a view password before using the account table.",
  openSettings: "Set the view password",
  purpose: "Use",
  account: "Account",
  secret: "Password",
  index: "No.",
  remove: "Delete",
  copyLogin: "Copy",
  copied: "Copied",
  copyFailed: "Could not copy. Try again.",
  paste: "Paste",
  pasteHint: "Paste an account and password here to fill the fields",
  pasteUnrecognized: "Could not find an account and password. Fill the fields instead.",
  editorFailed: "Could not open the add window. Try again.",
  empty: "No accounts yet.",
  required: "Account and password are both required.",
  cancel: "Cancel",
};
