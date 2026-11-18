<template>
  <div class="login-container">
    <div class="login-card">
      <h1>请先在 Yunzhao 登录</h1>
      <p>教师工作台使用 Yunzhao 的账号，这里不再单独登录。</p>
      <el-button type="primary" :loading="loading" @click="retry">重新读取账号</el-button>
    </div>
  </div>
</template>

<script setup>
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import { desktopSession } from '../../api'

const router = useRouter()
const loading = ref(false)

async function retry() {
  loading.value = true
  try {
    const data = await desktopSession()
    localStorage.setItem('token', data.token)
    localStorage.setItem('username', data.username || data.name || '')
    localStorage.setItem('yunzhao-account', JSON.stringify({
      name: data.name || '',
      username: data.username || '',
      email: data.email || ''
    }))
    router.replace('/dashboard')
  } catch {
    loading.value = false
  }
}
</script>

<style scoped>
.login-container {
  min-height: 100vh;
  display: grid;
  place-items: center;
  background: #f5f5f7;
}
.login-card {
  width: min(420px, calc(100% - 48px));
  padding: 36px 32px;
  border-radius: 16px;
  background: #fff;
  text-align: left;
}
.login-card h1 {
  margin: 0 0 8px;
  font-size: 28px;
  font-weight: 650;
  color: #1d1d1f;
}
.login-card p {
  margin: 0 0 20px;
  color: #6e6e73;
  line-height: 1.5;
}
</style>
