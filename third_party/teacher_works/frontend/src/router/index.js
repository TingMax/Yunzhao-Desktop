import { createRouter, createWebHistory } from 'vue-router'
import Layout from '../layout/index.vue'
import { desktopSession } from '../api'

const routes = [
  {
    path: '/login',
    name: 'Login',
    component: () => import('../views/login/index.vue'),
    meta: { title: '登录' }
  },
  {
    path: '/',
    component: Layout,
    redirect: '/dashboard',
    children: [
      {
        path: 'dashboard',
        name: 'Dashboard',
        component: () => import('../views/dashboard/index.vue'),
        meta: { title: '数据看板', icon: 'Odometer' }
      },
      // 教师工作
      {
        path: 'teacher/resources',
        name: 'Resources',
        component: () => import('../views/teacher/resources.vue'),
        meta: { title: '资源管理', icon: 'Files' }
      },
      {
        path: 'teacher/exams',
        name: 'Exams',
        component: () => import('../views/teacher/exams.vue'),
        meta: { title: '试卷管理', icon: 'Document' }
      },
      {
        path: 'teacher/ai-grading',
        name: 'AiGrading',
        component: () => import('../views/teacher/ai-grading.vue'),
        meta: { title: 'AI 批改', icon: 'MagicStick' }
      },
      {
        path: 'teacher/recitations',
        name: 'Recitations',
        component: () => import('../views/teacher/recitations.vue'),
        meta: { title: '背书情况', icon: 'Reading' }
      },
      {
        path: 'teacher/homework',
        name: 'Homework',
        component: () => import('../views/teacher/homework.vue'),
        meta: { title: '作业管理', icon: 'EditPen' }
      },
      {
        path: 'teacher/schedule',
        name: 'Schedule',
        component: () => import('../views/teacher/schedule.vue'),
        meta: { title: '我的课程表', icon: 'Calendar' }
      },
      {
        path: 'teacher/tasks',
        name: 'Tasks',
        component: () => import('../views/teacher/tasks.vue'),
        meta: { title: '临时工作区', icon: 'List' }
      },
      // 班主任工作
      {
        path: 'advisor/students',
        name: 'Students',
        component: () => import('../views/advisor/students.vue'),
        meta: { title: '学生档案', icon: 'User' }
      },
      {
        path: 'advisor/scores',
        name: 'Scores',
        component: () => import('../views/advisor/scores.vue'),
        meta: { title: '成绩分析', icon: 'DataLine' }
      },
      {
        path: 'advisor/points',
        name: 'Points',
        component: () => import('../views/advisor/points.vue'),
        meta: { title: '积分管理', icon: 'Trophy' }
      },
      {
        path: 'advisor/leaves',
        name: 'Leaves',
        component: () => import('../views/advisor/leaves.vue'),
        meta: { title: '请假管理', icon: 'Calendar' }
      },
      {
        path: 'advisor/evaluations',
        name: 'Evaluations',
        component: () => import('../views/advisor/evaluations.vue'),
        meta: { title: '期末评价', icon: 'Star' }
      },
      {
        path: 'advisor/communications',
        name: 'Communications',
        component: () => import('../views/advisor/communications.vue'),
        meta: { title: '家校沟通', icon: 'ChatDotRound' }
      },
      {
        path: 'advisor/seats',
        name: 'Seats',
        component: () => import('../views/advisor/seats.vue'),
        meta: { title: '座位表', icon: 'Grid' }
      }
    ]
  }
]

const router = createRouter({
  history: createWebHistory(),
  routes
})

async function ensureDesktopSession() {
  if (localStorage.getItem('token') && localStorage.getItem('yunzhao-account')) return true
  try {
    const data = await desktopSession()
    localStorage.setItem('token', data.token)
    localStorage.setItem('username', data.username || data.name || '')
    localStorage.setItem('yunzhao-account', JSON.stringify({
      name: data.name || '',
      username: data.username || '',
      email: data.email || ''
    }))
    return true
  } catch {
    return false
  }
}

router.beforeEach(async (to, from) => {
  if (from.query.solo === '1' && to.query.solo !== '1') {
    return { path: to.path, query: { ...to.query, solo: '1' }, hash: to.hash }
  }
  const ready = await ensureDesktopSession()
  if (!ready && to.path !== '/login') return '/login'
  if (ready && to.path === '/login') return '/dashboard'
})

export default router
