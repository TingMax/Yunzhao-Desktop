# Removes this distribution only. Personal settings and widget layouts are retained.
param([switch]$Confirmed, [switch]$StartupOnly)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms

try {
    $installRoot = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
    $coreExe = Join-Path $installRoot 'Yunzhao.exe'
    $settingsExe = Join-Path $installRoot 'Yunzhao.Settings.exe'
    if (!(Test-Path -LiteralPath $settingsExe)) { $settingsExe = Join-Path $installRoot 'Settings\Yunzhao.Settings.exe' }
    if ($installRoot -eq [IO.Path]::GetPathRoot($installRoot).TrimEnd('\') -or
        !(Test-Path -LiteralPath $coreExe -PathType Leaf) -or
        !(Test-Path -LiteralPath $settingsExe -PathType Leaf)) {
        throw '请从 Yunzhao 安装目录运行卸载程序。'
    }

    if (!$Confirmed -and !$StartupOnly) {
        $answer = [Windows.Forms.MessageBox]::Show(
            '卸载 Yunzhao？个人设置和桌面小组件布局将会保留。',
            'Yunzhao', 'YesNo', 'Question', 'Button2')
        if ($answer -ne [Windows.Forms.DialogResult]::Yes) { exit 0 }
    }

    $settings = @(Get-Process -Name 'Yunzhao.Settings' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq $settingsExe })
    foreach ($process in $settings) {
        [void]$process.CloseMainWindow()
        if (!$process.WaitForExit(30000)) { throw '请关闭 Yunzhao 设置中心后重试。' }
    }
    $quit = Start-Process -FilePath $coreExe -ArgumentList '--exit' -WindowStyle Hidden -PassThru
    if (!$quit.WaitForExit(15000) -or $quit.ExitCode -ne 0) { throw 'Yunzhao 未能退出，请关闭后重试。' }

    # Everything in this folder belongs to the install. Account data lives in %LOCALAPPDATA%\Yunzhao.
    $files = @(Get-ChildItem -LiteralPath $installRoot -Force | Where-Object { $_.Name -ne 'Uninstall.exe' } | ForEach-Object {
        $target = [IO.Path]::GetFullPath($_.FullName)
        if (!$target.StartsWith($installRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw '安装目录无效。' }
        $target
    })
    $shell = New-Object -ComObject WScript.Shell
    $programs = [Environment]::GetFolderPath('Programs')
    foreach ($name in @('Yunzhao.lnk', 'Yunzhao.Core.lnk')) {
        $shortcut = Join-Path $programs $name
        if (Test-Path -LiteralPath $shortcut) {
            $target = $shell.CreateShortcut($shortcut).TargetPath
            if ($target -eq $coreExe -or $target -eq (Join-Path $installRoot 'Yunzhao.Core.exe')) {
                Remove-Item -LiteralPath $shortcut -Force
            }
        }
    }
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    foreach ($taskName in @("Yunzhao Startup-$sid", "Atrium Startup-$sid")) {
        $task = Get-ScheduledTask -TaskPath '\' -TaskName $taskName -ErrorAction SilentlyContinue
        if ($task -and $task.Actions[0].Execute.Trim('"') -eq $coreExe) {
            Unregister-ScheduledTask -TaskPath '\' -TaskName $taskName -Confirm:$false
        }
    }
    $runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
    foreach ($runName in @('Yunzhao', 'Atrium')) {
        $startup = (Get-ItemProperty -LiteralPath $runKey -Name $runName -ErrorAction SilentlyContinue).$runName
        if ($startup -eq ('"' + $coreExe + '"') -or $startup -eq ('"' + (Join-Path $installRoot 'Yunzhao.Core.exe') + '"') -or
            $startup -eq ('"' + (Join-Path $installRoot 'Atrium.exe') + '"') -or $startup -eq ('"' + (Join-Path $installRoot 'Atrium.Core.exe') + '"')) {
            Remove-ItemProperty -LiteralPath $runKey -Name $runName
        }
    }
    if ($StartupOnly) { exit 0 }

    foreach ($file in $files) {
        if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Recurse -Force }
    }
    foreach ($appKey in @(
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Yunzhao',
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Atrium'
    )) {
        if ((Get-ItemProperty -LiteralPath $appKey -ErrorAction SilentlyContinue).InstallLocation -eq $installRoot) {
            Remove-Item -LiteralPath $appKey -Recurse -Force
        }
    }
    # Leave any files the user added to the application folder.
    # An app some process started there may still be in it: the empty folder then stays.
    if (!(Get-ChildItem -LiteralPath $installRoot -Force | Select-Object -First 1)) {
        try { Remove-Item -LiteralPath $installRoot } catch { }
    }
}
catch {
    [void][Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Yunzhao', 'OK', 'Error')
    exit 1
}
