import { reactive, readonly } from 'vue'

const commands = {
  initialized: { method: 'startProgress', to: 'in_progress' },
  in_progress: { method: 'completeNote', to: 'completed', asksResult: true },
  completed: { method: 'reopenNote', to: 'in_progress' }
}

/** Owns command identity and presentation lifetime; views retain ordering and refresh policy. */
export function useNoteStatusActions({
  api,
  onCommit = () => {},
  onFinish = () => {},
  onError = () => {},
  // A tick that completes a note has to say what came of it. The view answers with the
  // typed result, or null when the user backed out; the note then stays as it was.
  askResult = null
}) {
  const transitions = reactive(new Map()),
    operations = new Map()
  const publicTransitions = readonly(transitions)
  let disposed = false
  const current = (operation) => !disposed && operations.get(operation.id) === operation
  const context = (operation) => ({ from: operation.from, to: operation.to })
  const cancelled = (operation) => ({
    status: 'cancelled',
    committed: operation?.committed ?? false
  })

  function settle(operation, result) {
    if (operation.requestSettled) return
    operation.requestSettled = true
    operation.resolveRequest(result)
  }
  function clearTimers(operation) {
    for (const timer of operation.timers.values()) clearTimeout(timer)
    operation.timers.clear()
  }
  function release(operation) {
    clearTimers(operation)
    if (operations.get(operation.id) !== operation) return
    operations.delete(operation.id)
    transitions.delete(operation.id)
  }
  function phase(operation, value) {
    if (current(operation)) transitions.set(operation.id, { ...context(operation), phase: value })
  }
  async function report(operation, error) {
    if (!current(operation)) return
    try {
      await onError(error, operation.note, {
        ...context(operation),
        committed: operation.committed
      })
    } catch (failure) {
      if (current(operation)) console.error('[NoteStatusActions] 状态反馈失败:', failure)
    }
  }
  async function finish(operation, reconcile = false) {
    if (!current(operation) || operation.finishing) return
    operation.finishing = true
    clearTimers(operation)
    // A view's queued refresh must see that this card's animation has ended.
    transitions.delete(operation.id)
    try {
      if (operation.commitTask) await operation.commitTask
      if (!current(operation)) return
      await onFinish(operation.updated, operation.note, {
        ...context(operation),
        reconcile: reconcile || Boolean(operation.commitError)
      })
    } catch (error) {
      if (current(operation)) await report(operation, error)
    } finally {
      release(operation)
    }
  }
  async function fail(operation, error) {
    if (!current(operation)) return
    clearTimers(operation)
    if (operation.committed) {
      operation.commitError = error
      settle(operation, { status: 'committed', updated: operation.updated })
      await report(operation, error)
      if (current(operation)) await finish(operation, true)
      return
    }
    phase(operation, 'error')
    schedule(operation, 'release', 320, () => release(operation))
    settle(operation, { status: 'error', committed: false, error })
    await report(operation, error)
  }
  function schedule(operation, kind, delay, callback) {
    clearTimeout(operation.timers.get(kind))
    const timer = setTimeout(() => {
      if (!current(operation) || operation.timers.get(kind) !== timer) return
      operation.timers.delete(kind)
      // Observe every async timer callback; even a failed error reporter must
      // release ownership rather than create an unhandled rejection or loop.
      const task = (async () => {
        if (current(operation)) await callback()
      })()
      void task.catch((error) => {
        void fail(operation, error).catch((failure) => {
          if (!current(operation)) return
          console.error('[NoteStatusActions] 状态后处理失败:', failure)
          settle(operation, cancelled(operation))
          release(operation)
        })
      })
    }, delay)
    operation.timers.set(kind, timer)
  }
  async function commit(operation) {
    if (!current(operation)) return
    operation.commitTask = (async () => {
      try {
        await onCommit(operation.updated, operation.note, context(operation))
      } catch (error) {
        if (!current(operation)) return
        operation.commitError = error
        await report(operation, error)
      }
    })()
    await operation.commitTask
    if (current(operation) && operation.commitError) await finish(operation, true)
  }
  async function perform(operation, method, args) {
    try {
      const updated = await api[method](...args)
      if (!current(operation)) return
      if (!updated) throw new Error('状态接口未返回更新后的待办')
      operation.committed = true
      operation.updated = updated
      clearTimers(operation)
      phase(operation, 'playing')
      schedule(operation, 'commit', operation.to === 'completed' ? 920 : 125, () =>
        commit(operation)
      )
      schedule(operation, 'finish', 1000, () => finish(operation))
      settle(operation, { status: 'committed', updated })
    } catch (error) {
      await fail(operation, error)
    }
  }
  /**
   * The result a completion has to carry, or null when the user backed out of it.
   * Only views that can ask for one get the extra step; a caller without `askResult`
   * keeps the old one-click completion instead of silently cancelling it.
   */
  async function result(operation) {
    try {
      const typed = await askResult({ ...operation.note, from: operation.from, to: operation.to })
      if (!current(operation)) return null
      const text = String(typed ?? '')
      return text.trim() ? text : null
    } catch (error) {
      if (current(operation)) await report(operation, error)
      return null
    }
  }
  function execute(note) {
    const command = Object.hasOwn(commands, note?.status) ? commands[note.status] : null
    if (disposed || note?.id == null || !command) return Promise.resolve(cancelled())
    const existing = operations.get(note.id)
    if (existing) return existing.request
    const operation = {
      id: note.id,
      note: { ...note },
      from: note.status,
      to: command.to,
      committed: false,
      updated: null,
      timers: new Map(),
      finishing: false,
      commitTask: null,
      commitError: null,
      requestSettled: false
    }
    operation.request = new Promise((resolve) => {
      operation.resolveRequest = resolve
    })
    operations.set(operation.id, operation)
    // A completion first asks what came of it; the status only moves once the user has
    // said so. Everything else keeps its immediate feedback.
    const begin = async () => {
      if (!current(operation)) return
      let args
      // Only a view that can ask for a result gets the extra step; one that cannot
      // completes straight away, as it always did.
      if (command.asksResult && typeof askResult === 'function') {
        const text = await result(operation)
        if (!current(operation)) return
        if (text === null) {
          // Backing out of the result leaves the note exactly as it was, with no error.
          settle(operation, cancelled(operation))
          release(operation)
          return
        }
        args = [operation.id, text]
      } else {
        args = [operation.id]
      }
      if (!current(operation)) return
      phase(operation, 'acknowledging')
      schedule(operation, 'waiting', 120, () => {
        if (transitions.get(operation.id)?.phase === 'acknowledging') phase(operation, 'waiting')
      })
      await perform(operation, command.method, args)
    }
    void begin().catch((error) => {
      if (!current(operation)) return
      console.error('[NoteStatusActions] 状态操作未能结束:', error)
      settle(operation, cancelled(operation))
      release(operation)
    })
    return operation.request
  }
  return {
    transitions: publicTransitions,
    transitionFor: (id) => publicTransitions.get(id) || null,
    execute,
    dispose() {
      if (disposed) return
      disposed = true
      for (const operation of operations.values()) {
        clearTimers(operation)
        settle(operation, cancelled(operation))
      }
      operations.clear()
      transitions.clear()
    }
  }
}
