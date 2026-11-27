<script setup>
/**
 * NoteResultDialog.vue — 填写或修改待办的处理结果/进度。
 *
 * 两种用法：勾选完成时它挡住那一步（completion 为真，结果为空就不完成），以及平时主动
 * 补写或修改结果（completion 为假，清空即表示这条待办暂时没有结果）。
 */
import { computed, nextTick, ref, watch } from 'vue'
import AppModalShell from '../ui/AppModalShell.vue'
import BaseButton from '../ui/BaseButton.vue'

const props = defineProps({
  visible: { type: Boolean, default: false },
  note: { type: Object, default: null },
  // 勾选完成时结果必填；主动编辑时允许清空。
  completion: { type: Boolean, default: true }
})
const emit = defineEmits(['update:visible', 'confirm', 'cancel'])
let settled = false

const result = ref('')
const fieldRef = ref(null)
const invalid = ref(false)

const title = computed(() => {
  const text = String(props.note?.content || '').trim()
  if (!text) return props.completion ? '这条待办的处理结果' : '补充处理结果 / 进度'
  return text.length > 28 ? `${text.slice(0, 28)}…` : text
})

watch(
  () => props.visible,
  async (visible) => {
    if (!visible) {
      invalid.value = false
      return
    }
    settled = false
    // A note that already carries a progress keeps it here, so finishing it is a
    // correction rather than retyping the whole sentence.
    result.value = String(props.note?.progress ?? '')
    invalid.value = false
    await nextTick()
    fieldRef.value?.focus()
    fieldRef.value?.setSelectionRange?.(result.value.length, result.value.length)
  },
  { immediate: true }
)

function confirm() {
  if (settled) return
  const text = result.value.replace(/\r\n?/g, '\n').trim()
  if (!text && props.completion) {
    invalid.value = true
    fieldRef.value?.focus()
    return
  }
  settled = true
  emit('update:visible', false)
  emit('confirm', result.value.replace(/\r\n?/g, '\n'))
}

function cancel() {
  // AppModalShell also emits update:visible when it closes itself; only the first
  // answer counts, or a confirm would be followed by a spurious cancel.
  if (settled) return
  settled = true
  emit('update:visible', false)
  emit('cancel')
}

function onKeydown(event) {
  if (event.key !== 'Enter' || event.isComposing) return
  if (event.shiftKey) return
  event.preventDefault()
  confirm()
}
</script>

<template>
  <AppModalShell
    :visible="visible"
    :title="title"
    :eyebrow="completion ? '标记完成' : '处理结果'"
    :subtitle="
      completion
        ? '写下这次处理的结果或目前的进度，之后在日历上能直接看到。'
        : '随时记下这件事做到哪一步；清空即表示暂时没有结果。'
    "
    :aria-label="completion ? '填写待办的处理结果或进度' : '修改待办的处理结果或进度'"
    width="min(420rem, calc(100vw - 40rem))"
    @update:visible="cancel"
  >
    <div class="note-result">
      <label class="note-result__label" for="note-result-field">处理结果 / 进度</label>
      <textarea
        id="note-result-field"
        ref="fieldRef"
        v-model="result"
        class="note-result__input scroll-y"
        :class="{ 'is-invalid': invalid }"
        rows="4"
        placeholder="例如：已完成并交付；或 已确认方案，等供应商报价…"
        :aria-invalid="invalid || undefined"
        aria-describedby="note-result-hint"
        @input="invalid = false"
        @keydown="onKeydown"
      />
      <p id="note-result-hint" class="note-result__hint" :class="{ 'is-invalid': invalid }">
        {{
          invalid
            ? '请先写下处理结果或进度，再标记完成'
            : completion
              ? 'Enter 完成 · Shift + Enter 换行'
              : 'Enter 保存 · Shift + Enter 换行'
        }}
      </p>
    </div>
    <template #footer>
      <BaseButton variant="default" @click="cancel">取消</BaseButton>
      <BaseButton variant="primary" @click="confirm">{{
        completion ? '标记完成' : '保存'
      }}</BaseButton>
    </template>
  </AppModalShell>
</template>

<style scoped>
.note-result {
  display: flex;
  min-width: 0;
  flex-direction: column;
  gap: 7rem;
}

.note-result__label {
  color: var(--text-color-secondary);
  font-size: var(--fs-secondary);
  font-weight: 500;
  line-height: 1.4;
}

.note-result__input {
  display: block;
  box-sizing: border-box;
  width: 100%;
  min-width: 0;
  min-height: calc(var(--fs-secondary) * 4.4);
  padding: 10rem min(12rem, 4vw);
  border: 1px solid var(--ui-border-control);
  border-radius: 8rem;
  outline: none;
  background: var(--ui-surface-control);
  color: var(--text-color);
  font: inherit;
  font-size: var(--fs-secondary);
  line-height: 1.55;
  overflow-wrap: anywhere;
  resize: vertical;
  transition: border-color var(--motion-control) var(--ease-standard);
}

.note-result__input::placeholder {
  color: var(--text-color-secondary);
}

.note-result__input:hover,
.note-result__input:focus {
  border-color: var(--ui-border-hover);
}

.note-result__input.is-invalid {
  border-color: var(--ui-danger);
}

.note-result__hint {
  margin: 0;
  color: var(--text-color-secondary);
  font-size: calc(var(--fs-secondary) * 0.86);
  line-height: 1.35;
}

.note-result__hint.is-invalid {
  color: var(--ui-danger);
}
</style>
