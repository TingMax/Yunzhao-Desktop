# Launched only after the user chooses to restart. Runs outside the install folder.
param(
    [Parameter(Mandatory)][string]$InstallDirectory,
    [Parameter(Mandatory)][string]$PackageDirectory,
    [Parameter(Mandatory)][string]$StatePath,
    [Parameter(Mandatory)][int]$WaitForProcessId,
    [ValidateSet('0', '1')][string]$StartCore = '1',
    [ValidateRange(1, 60)][int]$StartupTimeoutSeconds = 20
)
$ErrorActionPreference = 'Stop'
$install = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\')
$package = [IO.Path]::GetFullPath($PackageDirectory).TrimEnd('\')
$StatePath = [IO.Path]::GetFullPath($StatePath)
$cache = Split-Path -Parent $StatePath
$parent = Split-Path -Parent $install
$name = Split-Path -Leaf $install
$suffix = [Guid]::NewGuid().ToString('N')
$incoming = Join-Path $parent "$name.update-$suffix"
$backup = Join-Path $parent "$name.previous-$suffix"
$result = Join-Path $cache 'result.json'
$coreStopped = $false
$swapped = $false
$confirmed = $false
$restored = $false
$started = New-Object 'System.Collections.Generic.List[System.Diagnostics.Process]'
# Never the install folder: what starts there would hold it, and the next update could not replace it.
$workingDirectory = [Environment]::GetFolderPath('UserProfile')

function Assert-TemporaryPath([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    if ((Split-Path -Parent $absolute) -ne $parent -or
        ((Split-Path -Leaf $absolute) -ne "$name.update-$suffix" -and
         (Split-Path -Leaf $absolute) -ne "$name.previous-$suffix")) { throw '更新临时目录无效。' }
}

# Current installs may still use Atrium.Settings.exe from before the rename.
function Get-CoreExe([string]$Root) {
    foreach ($leaf in @('Yunzhao.exe', 'Atrium.exe')) {
        $path = Join-Path $Root $leaf
        if (Test-Path -LiteralPath $path) { return $path }
    }
    return $null
}

function Get-SettingsExe([string]$Root) {
    foreach ($leaf in @('Settings\Yunzhao.Settings.exe', 'Settings\Atrium.Settings.exe')) {
        $path = Join-Path $Root $leaf
        if (Test-Path -LiteralPath $path) { return $path }
    }
    return $null
}

function Test-SameFile([string]$Left, [string]$Right) {
    if (!(Test-Path -LiteralPath $Left) -or !(Test-Path -LiteralPath $Right)) { return $false }
    $a = Get-Item -LiteralPath $Left
    $b = Get-Item -LiteralPath $Right
    if ($a.PSIsContainer -or $b.PSIsContainer -or $a.Length -ne $b.Length) { return $false }
    return (Get-FileHash -LiteralPath $Left -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $Right -Algorithm SHA256).Hash
}

function Get-FileHolders([string]$Path) {
    # Restart Manager can wait forever for an answer from the process holding the file.
    return ''
}

# Moves what is in $From into $To (merging folders). A folder some process is in (its current
# directory, e.g. an app started from an older Yunzhao) cannot be moved itself: its contents go, it stays.
# KeepIdentical leaves a locked file in place when it already matches the downloaded package.
function Move-Tree([string]$From, [string]$To, [switch]$Keep, [switch]$KeepIdentical) {
    if (!(Test-Path -LiteralPath $To)) { New-Item -ItemType Directory -Path $To | Out-Null }
    foreach ($item in @(Get-ChildItem -LiteralPath $From -Force)) {
        $target = Join-Path $To $item.Name
        if ($item.PSIsContainer -and (Test-Path -LiteralPath $target)) { Move-Tree $item.FullName $target -KeepIdentical:$KeepIdentical; continue }
        if ($item.PSIsContainer) {
            try { Move-Item -LiteralPath $item.FullName -Destination $target }
            catch { Move-Tree $item.FullName $target -KeepIdentical:$KeepIdentical }
            continue
        }
        # A file just written may be open for a moment (a virus scanner reading it).
        for ($try = 1; ; $try++) {
            try { Move-Item -LiteralPath $item.FullName -Destination $target; break }
            catch {
                if ($try -lt 6) { Start-Sleep -Milliseconds 300; continue }
                $remaining = $null
                if ($item.FullName.StartsWith($install + '\', [StringComparison]::OrdinalIgnoreCase)) { $remaining = $item.FullName }
                elseif ($target.StartsWith($install + '\', [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $target)) { $remaining = $target }
                if ($KeepIdentical -and $remaining) {
                    $twin = Join-Path $package $remaining.Substring($install.Length).TrimStart('\')
                    if (Test-SameFile $remaining $twin) { break }
                }
                $holders = Get-FileHolders $item.FullName
                $who = if ($holders) { "，占用程序：$holders" } else { '' }
                throw "无法替换 $($item.FullName)（$($_.Exception.Message.TrimEnd('。', '.'))$who），请关闭正在使用它的程序后重试。"
            }
        }
    }
    if (!$Keep) {
        try { Remove-Item -LiteralPath $From -Force } catch { }
    }
}

# What settings wrote to its crash log since $Since (its exception's first line), for the message.
function Get-CrashCause([datetime]$Since) {
    $log = Join-Path $env:LOCALAPPDATA 'Yunzhao\logs\settings.log'
    try {
        if (!(Test-Path -LiteralPath $log) -or (Get-Item -LiteralPath $log).LastWriteTime -lt $Since) { return '' }
        $last = Get-Content -LiteralPath $log -Tail 60 -Encoding UTF8 | Where-Object { $_ -match '^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} ' } | Select-Object -Last 1
        if (!$last) { return '' }
        $cause = $last.Substring(20).Trim()
        if ($cause.Length -gt 200) { $cause = $cause.Substring(0, 200) + '...' }
        return "：$cause"
    } catch { return '' }
}

# Smart App Control or an App Control (WDAC) policy refused a file: ERROR_SYSTEM_INTEGRITY_* 4550-4559 and
# 4580-4582. Windows PowerShell's Start-Process keeps only the system message, not the code, so compare that.
function Test-AppControlBlock([string]$Message) {
    foreach ($code in @(4550..4559) + @(4580..4582)) {
        $text = ([ComponentModel.Win32Exception]::new($code)).Message.Trim().TrimEnd('。', '.')
        if ($text -and $Message.Contains($text)) { return $true }
    }
    return $false
}

function Start-Verified([string]$Executable, [string]$Kind, [string]$Arguments) {
    $eventName = "Local\Yunzhao.Update.$Kind.$suffix"
    $ready = New-Object System.Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $eventName)
    try {
        $startedAt = (Get-Date).AddSeconds(-1)
        try {
            $process = Start-Process -FilePath $Executable -ArgumentList "$Arguments --ready-event $eventName" -WorkingDirectory $workingDirectory -WindowStyle Hidden -PassThru
        } catch {
            if (!(Test-AppControlBlock $_.Exception.Message)) { throw }
            # The release is not code-signed; each version's new files have no reputation yet.
            throw ("Windows 应用程序控制阻止了新版 $Kind 运行，已保留旧版本。新版文件没有代码签名，可能被智能应用控制或单位的应用控制策略拦截：" +
                   "可在 Windows 安全中心 › 应用和浏览器控制 › 智能应用控制中查看；单位电脑请联系管理员放行 Yunzhao 所在目录。（$($_.Exception.Message.Trim())）")
        }
        $started.Add($process)
        $until = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
        do {
            if ($process.HasExited) {
                $cause = if ($Kind -eq 'Settings') { Get-CrashCause $startedAt } else { '' }
                throw ("$Kind 启动失败（退出码 0x{0:X8}）{1}。" -f $process.ExitCode, $cause)
            }
            if ($ready.WaitOne(100)) {
                if ($process.HasExited) { throw "$Kind 在就绪后退出。" }
                return
            }
        } while ([DateTime]::UtcNow -lt $until)
        throw "$Kind 未确认启动就绪，已保留旧版本。"
    } finally { $ready.Dispose() }
}

function Stop-NewProcesses {
    foreach ($process in $started) {
        if (!$process.HasExited) {
            [void]$process.CloseMainWindow()
            if (!$process.WaitForExit(3000)) { Stop-Process -Id $process.Id -Force }
            [void]$process.WaitForExit(3000)
        }
    }
}

# The core's --exit does not close the calendar, capture helper, or island. Those keep
# files such as Calendar\resources\app.asar open, and the folder swap then fails.
function Stop-InstallTree {
    $prefix = $install + '\'
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        $left = @(Get-CimInstance Win32_Process | Where-Object {
            $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
        })
        if ($left.Count -eq 0) { return }
        foreach ($proc in $left) { Stop-Process -Id $proc.ProcessId -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Milliseconds 300
    } while ([DateTime]::UtcNow -lt $deadline)
    throw '仍有程序占用安装目录，更新未安装。'
}

function Write-Result([string]$ErrorText) {
    $temporary = $result + '.tmp'
    @{ error = $ErrorText } | ConvertTo-Json | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination $result -Force
}

try {
    if (!$parent -or !$name -or $install -eq [IO.Path]::GetPathRoot($install).TrimEnd('\') -or
        !$package.StartsWith($cache + '\', [StringComparison]::OrdinalIgnoreCase)) { throw '更新目录无效。' }
    Assert-TemporaryPath $incoming
    Assert-TemporaryPath $backup
    # The new package must be Yunzhao-named. The install being replaced may still
    # be an older Atrium.Settings layout from before the rename.
    if (!(Test-Path -LiteralPath (Join-Path $package 'Yunzhao.exe')) -or
        !(Test-Path -LiteralPath (Join-Path $package 'Settings\Yunzhao.Settings.exe'))) {
        throw '找不到完整的 Yunzhao 程序文件（更新包）。'
    }
    if (!(Get-CoreExe $install) -or !(Get-SettingsExe $install)) {
        throw '找不到完整的 Yunzhao 程序文件（当前安装）。'
    }
    # Copy before stopping the app; disk/access failures leave the old app running.
    New-Item -ItemType Directory -Path $incoming | Out-Null
    Get-ChildItem -LiteralPath $package -Force | Copy-Item -Destination $incoming -Recurse -Force
    # A downloaded release must retain the development package's explicit identity.
    if (Test-Path -LiteralPath (Join-Path $install 'development.txt')) {
        Copy-Item -LiteralPath (Join-Path $install 'development.txt') -Destination $incoming -Force
    }
    $settingsProcess = Get-Process -Id $WaitForProcessId -ErrorAction SilentlyContinue
    if ($settingsProcess -and !$settingsProcess.WaitForExit(30000)) { throw '设置中心尚未退出，更新未安装。' }
    $quit = Start-Process -FilePath (Get-CoreExe $install) -ArgumentList '--exit' -WindowStyle Hidden -PassThru
    try {
        if (!$quit.WaitForExit(15000) -or $quit.ExitCode -ne 0) { throw 'Yunzhao 尚未正常退出，更新未安装。' }
    } finally { $quit.Dispose() }
    $coreStopped = $true
    Stop-InstallTree
    Assert-TemporaryPath $backup
    Assert-TemporaryPath $incoming
    # The folder itself stays (an app may be in it); what is in it is replaced.
    try { Move-Tree $install $backup -Keep -KeepIdentical }
    catch {
        Move-Tree $backup $install
        $restored = $true
        throw
    }
    try { Move-Tree $incoming $install -KeepIdentical }
    catch {
        Move-Tree $install $incoming -Keep
        Move-Tree $backup $install
        $restored = $true
        throw
    }
    $swapped = $true
    if ($StartCore -eq '1') { Start-Verified (Join-Path $install 'Yunzhao.exe') 'Core' '' }
    $settingsArguments = if ($StartCore -eq '1') { '--page about --require-core-ready' } else { '--page about' }
    Start-Verified (Join-Path $install 'Settings\Yunzhao.Settings.exe') 'Settings' $settingsArguments
    foreach ($process in $started) {
        if ($process.HasExited) { throw '新版程序在启动确认期间退出。' }
    }
    # With the core intentionally off, only settings is launched and verified.
    $confirmed = $true
    Remove-Item -LiteralPath $StatePath -Force
    Write-Result ''
} catch {
    $failure = $_.Exception.Message
    if ($swapped -and !$confirmed) {
        try {
            Stop-NewProcesses
            Assert-TemporaryPath $incoming
            Move-Tree $install $incoming -Keep
            Assert-TemporaryPath $backup
            Move-Tree $backup $install
            $restored = $true
        } catch { $failure += " 恢复旧包未完成：$($_.Exception.Message)；恢复副本保留在 $backup。" }
    }
    Write-Result $failure
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show($failure, 'Yunzhao 更新未完成') | Out-Null
} finally {
    # Never delete an unconfirmed recovery point, even when rollback itself failed.
    $cleanup = @($incoming)
    if ($confirmed) { $cleanup += $backup }
    foreach ($temporary in $cleanup) {
        Assert-TemporaryPath $temporary
        if (Test-Path -LiteralPath $temporary) {
            try { Remove-Item -LiteralPath $temporary -Recurse -Force } catch { Write-Warning $_.Exception.Message }
        }
    }
    if (!$confirmed -and (!$swapped -or $restored)) {
        $core = Get-CoreExe $install
        $settings = Get-SettingsExe $install
        if ($coreStopped -and $StartCore -eq '1' -and $core) {
            Start-Process -FilePath $core -WorkingDirectory $workingDirectory -WindowStyle Hidden
        }
        if ($settings) {
            Start-Process -FilePath $settings -ArgumentList '--page about' -WorkingDirectory $workingDirectory -WindowStyle Hidden
        }
    }
    if ($confirmed) {
        $work = $package
        while ($work.StartsWith($cache + '\', [StringComparison]::OrdinalIgnoreCase) -and (Split-Path -Parent $work) -ne $cache) {
            $work = Split-Path -Parent $work
        }
        $absoluteWork = [IO.Path]::GetFullPath($work)
        if ((Split-Path -Parent $absoluteWork) -eq $cache -and (Split-Path -Leaf $absoluteWork) -match '^package-(background|[0-9a-f]{32})$') {
            try { Remove-Item -LiteralPath $absoluteWork -Recurse -Force } catch { Write-Warning $_.Exception.Message }
        }
    }
    foreach ($process in $started) { $process.Dispose() }
}
