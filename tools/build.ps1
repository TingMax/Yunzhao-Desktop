<#
.SYNOPSIS
  Builds Yunzhao and assembles a portable folder: dist/Yunzhao/.

.DESCRIPTION
  native (CMake, MSVC) -> settings (dotnet publish, self-contained WinUI 3) -> dist.
  Layout:
    dist/Yunzhao/Yunzhao.exe
    dist/Yunzhao/Yunzhao.Settings.exe and the WinUI runtime
    dist/Yunzhao/Fonts/ (MiSans; the core reads it from there)
  -Zip also writes dist/Yunzhao-<version>-win-x64.zip.
#>
param(
    [string]$Configuration = "Release",
    [switch]$Zip,
    [switch]$Development
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root $(if ($Development) { "dist/Development/Yunzhao" } else { "dist/Yunzhao" })
$buildDirectory = if ($Development) { 'native/build/development' } else { 'native/build/win-x64' }
$developmentFlag = if ($Development) { 'ON' } else { 'OFF' }

Push-Location $root
try {
    cmake --preset win-x64 -S native -B $buildDirectory "-DYUNZHAO_DEVELOPMENT=$developmentFlag"
    if ($LASTEXITCODE) { throw "cmake configure failed" }
    cmake --build $buildDirectory --config $Configuration
    if ($LASTEXITCODE) { throw "native build failed" }

    if (Test-Path $dist) {
        # A running Yunzhao (or its settings window) locks files: stop before deleting anything.
        $full = (Resolve-Path $dist).Path
        $expected = [IO.Path]::GetFullPath((Join-Path $root $(if ($Development) { 'dist/Development/Yunzhao' } else { 'dist/Yunzhao' })))
        if (![string]::Equals($full, $expected, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected distribution path' }
        $busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($full + '\', [StringComparison]::OrdinalIgnoreCase) })
        if ($busy) { throw "dist/Yunzhao is in use by $(($busy | ForEach-Object { "$($_.Name) ($($_.Id))" }) -join ', '); close it first" }
        Remove-Item -Recurse -Force $dist
    }
    New-Item -ItemType Directory -Force $dist | Out-Null
    dotnet publish settings/Yunzhao.Settings/Yunzhao.Settings.csproj -c $Configuration -p:YunzhaoDevelopment=$($Development.IsPresent.ToString().ToLowerInvariant()) -o $dist
    if ($LASTEXITCODE) { throw "settings publish failed" }

    Copy-Item (Join-Path "$buildDirectory/$Configuration" "Yunzhao.exe") $dist
    $island = Join-Path $root "third_party\fly-island\src-tauri\target\release\focusd-island.exe"
    if (Test-Path -LiteralPath $island) {
        $islandDir = Join-Path $dist "Island"
        New-Item -ItemType Directory -Force -Path $islandDir | Out-Null
        Copy-Item -LiteralPath $island -Destination (Join-Path $islandDir "focusd-island.exe")
    }
    Get-ChildItem $dist -Recurse -File | Where-Object { $_.Extension -eq '.pdb' -or $_.Name -eq 'createdump.exe' -or $_.Name -eq 'Microsoft.DiaSymReader.Native.amd64.dll' } | Remove-Item -Force
    Get-ChildItem $dist -Directory -Recurse | Where-Object { $_.Name -match '^(cs|de|es|fr|it|ja|ko|pl|pt-BR|ru|tr|zh-Hant)$' } | Remove-Item -Recurse -Force
    if ($Development) { Set-Content -LiteralPath (Join-Path $dist 'development.txt') -Value 'Development distribution: does not register or change Windows startup.' }
    Copy-Item (Join-Path $PSScriptRoot "Uninstall.ps1") $dist
    Copy-Item LICENSE $dist

    if ($Zip) {
        $version = (Select-String -Path native/CMakeLists.txt -Pattern 'VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
        $zipPath = if ($Development) { Join-Path $root "dist/Development/Yunzhao-$version-development-win-x64.zip" }
                   else { Join-Path $root "dist/Yunzhao-$version-win-x64.zip" }
        if (Test-Path $zipPath) { Remove-Item $zipPath }
        Compress-Archive -Path $dist -DestinationPath $zipPath
        Write-Host "wrote $zipPath"
    }
    Write-Host "dist ready: $dist"
}
finally {
    Pop-Location
}
