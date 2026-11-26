param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Icon,
    [Parameter(Mandatory)][string]$Description,
    [string]$Product = "Yunzhao",
    [string]$Company = "Yunzhao",
    [string]$Version = "2.6.23.0",
    [string]$Copyright = "MIT License"
)
$ErrorActionPreference = 'Stop'

# electron-builder leaves the Electron runtime's own resource block in place
# ("Electron", "GitHub, Inc."), so the taskbar and the file properties name a
# different product. rcedit rewrites that block and the icon in one pass.
$rcedit = Get-ChildItem "$env:LOCALAPPDATA\electron-builder\Cache\winCodeSign" -Recurse -Filter "rcedit-x64.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $rcedit) { throw "rcedit-x64.exe not found in the electron-builder cache" }
if (-not (Test-Path $Exe)) { throw "missing $Exe" }

& $rcedit.FullName $Exe `
    --set-icon $Icon `
    --set-version-string "FileDescription" $Description `
    --set-version-string "ProductName" $Product `
    --set-version-string "CompanyName" $Company `
    --set-version-string "LegalCopyright" $Copyright `
    --set-file-version $Version `
    --set-product-version $Version
if ($LASTEXITCODE) { throw "rcedit failed for $Exe ($LASTEXITCODE)" }

$vi = (Get-Item $Exe).VersionInfo
Write-Output ("{0}: {1} / {2} / {3}" -f (Split-Path $Exe -Leaf), $vi.FileDescription, $vi.ProductName, $vi.FileVersion)
