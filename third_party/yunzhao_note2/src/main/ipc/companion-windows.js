const companions = new Set()

export function addCompanionWindow(window) {
  if (window) companions.add(window)
}

export function removeCompanionWindow(window) {
  companions.delete(window)
}

export function listCompanionWindows() {
  const live = []
  for (const window of companions) {
    if (!window || window.isDestroyed?.()) companions.delete(window)
    else live.push(window)
  }
  return live
}

export function isCompanionSender(sender) {
  if (!sender) return false
  return listCompanionWindows().some((window) => window.webContents === sender)
}
