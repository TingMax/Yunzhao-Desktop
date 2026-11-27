<script setup>
import { computed, useId } from 'vue'

const props = defineProps({
  modelValue: { type: String, default: '' },
  label: { type: String, default: '备注' },
  optional: { type: Boolean, default: true },
  placeholder: { type: String, default: '添加补充说明…' },
  rows: { type: Number, default: 2 }
})
const emit = defineEmits(['update:modelValue'])
const fieldId = useId()
const text = computed({
  get: () => props.modelValue,
  set: (value) => emit('update:modelValue', value)
})
</script>

<template>
  <div class="note-remark-field">
    <label :for="fieldId">
      <span>{{ label }}</span
      ><span v-if="optional" class="note-remark-field__optional">（可选）</span>
    </label>
    <textarea
      :id="fieldId"
      v-model="text"
      class="note-remark-field__input scroll-y"
      :rows="rows"
      :aria-label="optional ? `${label}（可选）` : label"
      :placeholder="placeholder"
    />
  </div>
</template>

<style scoped>
.note-remark-field {
  display: flex;
  min-width: 0;
  max-width: 100%;
  flex-direction: column;
  gap: 7rem;
  margin-top: 10rem;
}

.note-remark-field > label {
  display: flex;
  min-width: 0;
  flex-wrap: wrap;
  align-items: baseline;
  gap: 2rem 4rem;
  color: var(--text-color-secondary);
  font-size: var(--fs-secondary);
  font-weight: 500;
  line-height: 1.4;
  overflow-wrap: anywhere;
}

.note-remark-field__optional {
  font-size: 0.86em;
  font-weight: 400;
}

.note-remark-field__input {
  display: block;
  box-sizing: border-box;
  width: 100%;
  min-width: 0;
  max-width: 100%;
  min-height: calc(var(--fs-secondary) * 3.1 + 20rem + 2px);
  padding: 10rem min(12rem, 4vw);
  border: 1px solid var(--ui-border-control);
  border-radius: 8rem;
  outline: none;
  background: var(--ui-surface-control);
  color: var(--text-color);
  font-family: inherit;
  font-size: var(--fs-secondary);
  font-weight: 400;
  line-height: 1.55;
  caret-color: var(--text-color);
  overflow-wrap: anywhere;
  resize: vertical;
  transition: border-color var(--motion-control) var(--ease-standard);
}

.note-remark-field__input:hover,
.note-remark-field__input:focus {
  border-color: var(--ui-border-hover);
}

.note-remark-field__input::placeholder {
  color: var(--text-color-secondary);
}
</style>
