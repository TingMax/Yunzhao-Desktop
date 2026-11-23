# Starts the real Yunzhao Note app. Yunzhao writes yunzhao-place.json to choose the monitor and the view.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\third_party\yunzhao_note2')).Path
$logDir = Join-Path $env:LOCALAPPDATA 'Yunzhao\logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir 'yunzhao-note.log'
function Write-Log([string]$message) {
    Add-Content -LiteralPath $log -Value ("{0} {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $message)
}

Set-Location $root
if (-not (Test-Path -LiteralPath (Join-Path $root 'node_modules'))) {
    Write-Log 'npm install'
    npm install --no-fund --no-audit
    if ($LASTEXITCODE) { Write-Log "npm install failed: $LASTEXITCODE"; exit $LASTEXITCODE }
}
$dll = Join-Path $root 'native_blur\build\bin\blur_engine.dll'
if (-not (Test-Path -LiteralPath $dll)) {
    Write-Log 'build native blur'
    powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'native_blur\build.ps1')
    if ($LASTEXITCODE) { Write-Log "native build failed: $LASTEXITCODE"; exit $LASTEXITCODE }
}
$running = Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -and $_.CommandLine -match 'electron-vite' -and $_.CommandLine -match 'yunzhao_note2' }
if ($running) {
    Write-Log 'already running'
    exit 0
}
Write-Log 'dev'
npm run dev
