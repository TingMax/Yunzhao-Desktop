<template>
  <router-view></router-view>
</template>

<style>
/* Base styles to match requirements */
html, body {
  margin: 0;
  padding: 0;
  height: 100%;
  background-color: #F5F5F7;
}

#app {
  height: 100%;
}

/* Global rounded corners for cards */
.el-card {
  border-radius: 16px !important;
  border: 1px solid rgba(0, 0, 0, 0.06);
  box-shadow: none;
}

:root {
  --el-color-primary: #0A84FF;
  --el-color-warning: #E6A23C;
}
</style>
