param([switch]$Open)

$ErrorActionPreference = 'Continue'
$repo = Split-Path $PSScriptRoot -Parent
$root = Join-Path $repo 'third_party\smart_table'
$backend = Join-Path $root 'smarttable-backend'
$frontend = Join-Path $root 'smart-table'
$logDir = Join-Path $env:LOCALAPPDATA 'Yunzhao\logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir 'smart-table.log'

function Write-Log([string]$line) {
    Add-Content -Path $log -Value ("{0} {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $line)
}

function Test-Port([int]$port) {
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $wait = $client.BeginConnect('127.0.0.1', $port, $null, $null)
        $ok = $wait.AsyncWaitHandle.WaitOne(400) -and $client.Connected
        return [bool]$ok
    } catch {
        return $false
    } finally {
        $client.Close()
    }
}

function Wait-Port([int]$port, [int]$seconds) {
    for ($i = 0; $i -lt $seconds; $i++) {
        if (Test-Port $port) { return $true }
        Start-Sleep -Seconds 1
    }
    return $false
}

if (-not (Test-Path $backend) -or -not (Test-Path $frontend)) {
    Write-Log "smart_table is missing under $root"
    exit 1
}

$py = Join-Path $backend '.venv\Scripts\python.exe'
if (-not (Test-Path $py)) {
    Write-Log 'creating backend virtualenv'
    python -m venv (Join-Path $backend '.venv')
}
$marker = Join-Path $backend '.venv\atrium-ready.txt'
if (-not (Test-Path $marker)) {
    Write-Log 'installing backend packages'
    & $py -m pip install -U pip | Out-File -Append $log
    $filtered = Join-Path $env:TEMP 'atrium-smart-table-requirements.txt'
    Get-Content (Join-Path $backend 'requirements.txt') | Where-Object { $_ -notmatch '^\s*WeasyPrint' } | Set-Content $filtered
    & $py -m pip install -r $filtered | Out-File -Append $log
    if ($LASTEXITCODE -eq 0) { Set-Content $marker 'ok' }
}

$configDir = Join-Path $root 'config'
New-Item -ItemType Directory -Force -Path $configDir | Out-Null
$envFile = Join-Path $configDir '.env'
$envExample = Join-Path $backend '.env.example'
if (-not (Test-Path $envFile)) {
    $text = [System.IO.File]::ReadAllText($envExample)
    $text = $text -replace 'FLASK_DEBUG=True', 'FLASK_DEBUG=False'
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($envFile, $text, $utf8)
}

if (-not (Test-Port 5000)) {
    Write-Log 'starting backend'
    Start-Process -FilePath $py -ArgumentList 'run.py' -WorkingDirectory $backend -WindowStyle Hidden
}

if (-not (Test-Path (Join-Path $frontend 'node_modules'))) {
    Write-Log 'installing frontend packages'
    Push-Location $frontend
    npm install --no-fund --no-audit --legacy-peer-deps | Out-File -Append $log
    Pop-Location
}

if (-not (Test-Port 3000)) {
    Write-Log 'starting frontend'
    $env:BROWSER = 'none'
    Start-Process -FilePath 'npm.cmd' -ArgumentList 'run','dev','--','--host','127.0.0.1','--port','3000' -WorkingDirectory $frontend -WindowStyle Hidden
}

if ($Open) {
    if (Wait-Port 3000 180) {
        Start-Process 'http://127.0.0.1:3000/'
        Write-Log 'opened http://127.0.0.1:3000/'
    } else {
        Write-Log 'frontend did not open port 3000'
        exit 1
    }
}
