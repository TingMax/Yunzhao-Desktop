/** Owns the main-window restoration ticket for one capture. Host/assets/ACK stay in CaptureCoordinator. */
function parkWindow(window) {
  if (!window || window.isDestroyed?.()) return null
  const canFade = typeof window.setOpacity === 'function' && typeof window.getOpacity === 'function'
  const canIgnore =
    typeof window.setIgnoreMouseEvents === 'function' &&
    typeof window.isIgnoreMouseEvents === 'function'
  if (!canFade && !canIgnore) return null
  const parked = {
    window,
    opacity: canFade ? window.getOpacity() : null,
    ignore: canIgnore ? window.isIgnoreMouseEvents() : null
  }
  if (canFade) window.setOpacity(0)
  if (canIgnore) window.setIgnoreMouseEvents(true)
  return parked
}

function restoreParked(parked) {
  for (const entry of parked || []) {
    if (!entry?.window || entry.window.isDestroyed?.()) continue
    if (entry.opacity !== null) entry.window.setOpacity(entry.opacity)
    if (entry.ignore !== null) entry.window.setIgnoreMouseEvents(entry.ignore)
  }
}

export function createCaptureWindowIntegration({
  getWindow,
  getWindows = () => [],
  shouldUncover = () => false,
  isDockHidden = () => false,
  shouldHide,
  isQuitting,
  suspendDock,
  resumeDock,
  hideWindow,
  reassertZOrder,
  openBusiness
}) {
  let ticket = null
  const currentTicketWindow = (snapshot) => {
    const window = getWindow()
    return window && window === snapshot.window && !window.isDestroyed() ? window : null
  }
  return {
    isActive: () => Boolean(ticket),
    snapshot: () => ({
      active: Boolean(ticket),
      wasVisible: ticket?.wasVisible ?? false,
      wasFocused: ticket?.wasFocused ?? false,
      hidden: ticket?.hidden ?? false
    }),
    openBusiness,
    begin() {
      if (ticket) return
      const window = getWindow()
      const live = window && !window.isDestroyed()
      const dockHidden = Boolean(live && isDockHidden())
      const parked = []
      ticket = {
        window,
        wasVisible: Boolean(live && window.isVisible()),
        wasFocused: Boolean(live && window.isFocused()),
        hidden: Boolean(shouldHide() && live && window.isVisible() && !dockHidden),
        parked
      }
      try {
        if (shouldUncover()) {
          const seen = new Set()
          for (const candidate of [window, ...getWindows()]) {
            if (!candidate || seen.has(candidate)) continue
            seen.add(candidate)
            const entry = parkWindow(candidate)
            if (entry) parked.push(entry)
          }
        }
        suspendDock('screenshot', { preserveHidden: true })
        if (ticket.hidden) hideWindow(window)
      } catch (error) {
        restoreParked(parked)
        ticket = null
        throw error
      }
    },
    finish(_origin, business = {}) {
      if (!ticket) return
      const snapshot = ticket
      ticket = null
      resumeDock('screenshot')
      restoreParked(snapshot.parked)
      if (isQuitting()) return
      const window = currentTicketWindow(snapshot)
      if (!window) return
      if (snapshot.hidden) {
        window.showInactive()
        if (snapshot.wasFocused) window.focus()
        reassertZOrder('screenshot-finished')
      }
      if (business.businessOpened && !business.businessAccepted && !snapshot.wasVisible) {
        hideWindow(window)
      }
    }
  }
}
