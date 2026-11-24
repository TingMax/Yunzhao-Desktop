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
    foreach ($leaf in @('Settings\Yunzhao.Settings.exe', 'Settings\Atrium.Settings.exe',
                        'Yunzhao.Settings.exe', 'Atrium.Settings.exe')) {
        $path = Join-Path $Root $leaf
        if (Test-Path -LiteralPath $path) { return $path }
    }
    # Last resort: any settings executable one level down (a reorganized package).
    $any = @(Get-ChildItem -LiteralPath $Root -Filter '*Settings.exe' -File -Recurse -Depth 1 -ErrorAction SilentlyContinue |
             Select-Object -First 1)
    if ($any.Count -gt 0) { return $any[0].FullName }
    return $null
}

# ---- The desktop progress window -------------------------------------------------
# Shown for the whole install, so the update stays visible after the settings window
# goes away. Cartoon-soft, macOS-sheet-like: a springy bar, a bobbing icon tile and a
# rotating ring. The percentage comes from real work (files copied, files replaced,
# then the start-up verify), never from a timer.

$script:uiPercent = 0
$script:uiText = '正在准备更新…'
$script:uiForm = $null
$script:uiFailed = $false

function Show-UpdateWindow([string]$Version) {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        Add-Type -AssemblyName System.Drawing
        if (-not ('YunzhaoUpdateWindow' -as [type])) {
            Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.Runtime.InteropServices;
using System.Windows.Forms;

public class YunzhaoUpdateWindow : Form
{
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int cx, int cy, uint flags);

    readonly string version;
    readonly Timer timer = new Timer();
    readonly Stopwatch clock = Stopwatch.StartNew();
    float target;               // the percentage the install asked for
    float shown;                // what the bar shows (springs towards target)
    float velocity;
    bool failed;
    bool done;                  // 100%: the check marks pops in
    float doneScale;
    string status = "正在准备更新…";
    float statusFade = 1f;

    public YunzhaoUpdateWindow(string version)
    {
        this.version = version;
        FormBorderStyle = FormBorderStyle.None;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(400, 150);
        ShowInTaskbar = false;
        AutoScaleMode = AutoScaleMode.None;
        BackColor = Color.FromArgb(18, 20, 26);
        DoubleBuffered = true;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint, true);
        using (GraphicsPath path = Rounded(new Rectangle(0, 0, ClientSize.Width, ClientSize.Height), 18))
            Region = new Region(path);
        timer.Interval = 16;
        timer.Tick += delegate { Step(); };
        timer.Start();
    }

    public void SetProgress(int percent, string text)
    {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        if (percent > target) target = percent;
        if (!string.IsNullOrEmpty(text) && text != status) { status = text; statusFade = 0f; }
        if (percent >= 100) done = true;
        Invalidate();
    }

    public void SetFailed(string text)
    {
        failed = true;
        status = text;
        statusFade = 0f;
        Invalidate();
    }

    // A little overshoot and settle, so the bar feels drawn rather than timed.
    void Step()
    {
        const float dt = 0.016f, stiffness = 120f, damping = 18f;
        velocity += (target - shown) * stiffness * dt - velocity * damping * dt;
        shown += velocity * dt;
        if (shown > 100f) { shown = 100f; velocity = 0f; }
        if (shown < 0f) { shown = 0f; velocity = 0f; }
        if (Math.Abs(target - shown) < 0.05f && Math.Abs(velocity) < 0.6f) { shown = target; velocity = 0f; }
        if (statusFade < 1f) { statusFade += dt * 5f; if (statusFade > 1f) statusFade = 1f; }
        // Fade the sheet in, like a macOS sheet sliding down.
        if (Opacity < 1.0) Opacity = Math.Min(1.0, Opacity + 0.14);
        // The check pops in with a cartoon overshoot once the bar is full.
        if (done && shown >= 99.5f && doneScale < 1f)
        {
            doneScale += dt * 3.2f;
            if (doneScale > 1f) doneScale = 1f;
        }
        Invalidate();
    }

    // Shown without taking the focus from whatever the user is doing.
    public void ShowQuiet()
    {
        Opacity = 0.0; // fades in on the first ticks
        Show();
        ShowWindow(Handle, 4 /* SW_SHOWNOACTIVATE */);
        SetWindowPos(Handle, new IntPtr(-1) /* HWND_TOPMOST */, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        Graphics g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
        float t = (float)clock.Elapsed.TotalSeconds;

        // The sheet: a dark gradient, a soft glow in the corner, a hairline border.
        using (GraphicsPath shell = Rounded(new Rectangle(0, 0, ClientSize.Width - 1, ClientSize.Height - 1), 18))
        {
            using (LinearGradientBrush bg = new LinearGradientBrush(
                       new Rectangle(0, 0, ClientSize.Width, ClientSize.Height),
                       Color.FromArgb(31, 34, 44), Color.FromArgb(14, 15, 20), 90f))
                g.FillPath(bg, shell);
            using (GraphicsPath glow = new GraphicsPath())
            {
                glow.AddEllipse(-80, -110, 320, 240);
                using (PathGradientBrush pb = new PathGradientBrush(glow))
                {
                    pb.CenterColor = failed ? Color.FromArgb(80, 255, 96, 92) : Color.FromArgb(72, 10, 132, 255);
                    pb.SurroundColors = new Color[] { Color.FromArgb(0, 10, 132, 255) };
                    g.FillPath(pb, glow);
                }
            }
            using (Pen pen = new Pen(Color.FromArgb(40, 255, 255, 255), 1f))
                g.DrawPath(pen, shell);
        }

        // The app tile: bobbing gently, with a specular sheen and a rotating ring.
        float bob = (float)Math.Sin(t * 2.4) * 2.2f;
        RectangleF tile = new RectangleF(30f, 30f + bob, 56f, 56f);
        using (GraphicsPath p = Rounded(Rectangle.Round(tile), 16))
        {
            using (LinearGradientBrush b = new LinearGradientBrush(tile,
                       failed ? Color.FromArgb(255, 138, 132) : Color.FromArgb(104, 182, 255),
                       failed ? Color.FromArgb(214, 66, 62) : Color.FromArgb(8, 96, 246), 62f))
                g.FillPath(b, p);
            using (LinearGradientBrush hi = new LinearGradientBrush(tile,
                       Color.FromArgb(96, 255, 255, 255), Color.FromArgb(0, 255, 255, 255), 90f))
                g.FillPath(hi, p);
        }
        using (Font f = new Font("Segoe UI", 27f, FontStyle.Bold, GraphicsUnit.Pixel))
        using (StringFormat sf = Central())
        using (SolidBrush ink = new SolidBrush(Color.FromArgb(252, 253, 255)))
        {
            if (done && doneScale > 0f)
            {
                // A drawn check, scaled with a soft overshoot.
                float k = doneScale < 0.8f ? doneScale * 1.18f : 1f - (doneScale - 0.8f) * 0.18f;
                k = Math.Max(0.01f, Math.Min(1.06f, k));
                g.TranslateTransform(tile.X + tile.Width / 2f, tile.Y + tile.Height / 2f);
                g.ScaleTransform(k, k);
                using (Pen check = new Pen(Color.FromArgb(252, 253, 255), 5.4f))
                {
                    check.StartCap = LineCap.Round;
                    check.EndCap = LineCap.Round;
                    check.LineJoin = LineJoin.Round;
                    g.DrawLines(check, new PointF[]
                    {
                        new PointF(-11f, 1f), new PointF(-3.4f, 9f), new PointF(12f, -9f)
                    });
                }
                g.ResetTransform();
            }
            else
            {
                g.DrawString("Y", f, ink, new RectangleF(tile.X, tile.Y - 1f, tile.Width, tile.Height), sf);
            }
        }

        if (!failed && shown < 99.5f)
        {
            float sweep = 96f + 190f * (0.5f + 0.5f * (float)Math.Sin(t * 1.7));
            using (Pen ring = new Pen(Color.FromArgb(132, 255, 255, 255), 3f))
            {
                ring.StartCap = LineCap.Round;
                ring.EndCap = LineCap.Round;
                g.DrawArc(ring, tile.X - 7f, tile.Y - 7f, tile.Width + 14f, tile.Height + 14f, (t * 210f) % 360f, sweep);
            }
        }

        using (Font tf = new Font("Microsoft YaHei UI", 13f, FontStyle.Bold))
        using (SolidBrush ink = new SolidBrush(Color.FromArgb(244, 245, 248)))
            g.DrawString("正在安装 Yunzhao " + version, tf, ink, 104f, 33f);

        int percent = (int)Math.Round(shown);
        using (Font sf2 = new Font("Microsoft YaHei UI", 9f))
        using (SolidBrush sub = new SolidBrush(Color.FromArgb((int)(205 * statusFade), 168, 172, 184)))
            g.DrawString(status, sf2, sub, 104f, 61f);
        using (Font pf = new Font("Segoe UI", 9f, FontStyle.Bold))
        using (SolidBrush pi = new SolidBrush(failed ? Color.FromArgb(255, 112, 107) : Color.FromArgb(124, 192, 255)))
        using (StringFormat right = new StringFormat())
        {
            right.Alignment = StringAlignment.Far;
            g.DrawString(percent + "%", pf, pi, new RectangleF(282f, 61f, 88f, 18f), right);
        }

        // The bar: a pill track, a springy gradient fill, and a highlight that travels.
        RectangleF track = new RectangleF(30f, 112f, 340f, 10f);
        using (GraphicsPath p = Rounded(Rectangle.Round(track), 5))
        using (SolidBrush b = new SolidBrush(Color.FromArgb(34, 255, 255, 255)))
            g.FillPath(b, p);

        float w = Math.Min(track.Width, track.Width * shown / 100f);
        if (w > 0.5f)
        {
            if (w < track.Height) w = track.Height;
            RectangleF fill = new RectangleF(track.X, track.Y, w, track.Height);
            using (GraphicsPath p = Rounded(Rectangle.Round(fill), 5))
            {
                using (LinearGradientBrush b = new LinearGradientBrush(track,
                           failed ? Color.FromArgb(255, 104, 98) : Color.FromArgb(92, 184, 255),
                           failed ? Color.FromArgb(206, 62, 58) : Color.FromArgb(10, 92, 238), 0f))
                    g.FillPath(b, p);

                float band = 70f;
                float x = fill.X - band + ((t * 170f) % (band + fill.Width + 40f));
                Region old = g.Clip;
                g.SetClip(p, CombineMode.Intersect);
                RectangleF sweep = new RectangleF(x, fill.Y, band, fill.Height);
                using (LinearGradientBrush gb = new LinearGradientBrush(sweep,
                           Color.FromArgb(0, 255, 255, 255), Color.FromArgb(0, 255, 255, 255), 0f))
                {
                    ColorBlend blend = new ColorBlend(3);
                    blend.Colors = new Color[]
                    {
                        Color.FromArgb(0, 255, 255, 255),
                        Color.FromArgb(165, 255, 255, 255),
                        Color.FromArgb(0, 255, 255, 255)
                    };
                    blend.Positions = new float[] { 0f, 0.5f, 1f };
                    gb.InterpolationColors = blend;
                    g.FillRectangle(gb, sweep);
                }
                g.Clip = old;
                old.Dispose();
            }
        }
    }

    static StringFormat Central()
    {
        StringFormat sf = new StringFormat();
        sf.Alignment = StringAlignment.Center;
        sf.LineAlignment = StringAlignment.Center;
        return sf;
    }

    static GraphicsPath Rounded(Rectangle r, int radius)
    {
        int d = Math.Min(Math.Max(1, radius * 2), Math.Min(r.Width, r.Height));
        GraphicsPath path = new GraphicsPath();
        path.AddArc(r.X, r.Y, d, d, 180, 90);
        path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        path.CloseFigure();
        return path;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) timer.Dispose();
        base.Dispose(disposing);
    }
}
'@
        }
        $script:uiForm = New-Object YunzhaoUpdateWindow $Version
        $script:uiForm.ShowQuiet()
        [System.Windows.Forms.Application]::DoEvents()
    } catch {
        $script:uiForm = $null # the install still runs without the window
    }
}

function Set-Progress([int]$Percent, [string]$Text) {
    if ($Percent -lt 0) { $Percent = 0 }
    if ($Percent -gt 100) { $Percent = 100 }
    if ($Percent -gt $script:uiPercent) { $script:uiPercent = $Percent }
    if ($Text) { $script:uiText = $Text }
    if (-not $script:uiForm) { return }
    try {
        $script:uiForm.SetProgress($script:uiPercent, $script:uiText)
        [System.Windows.Forms.Application]::DoEvents()
    } catch { }
}

function Set-ProgressFailed([string]$Text) {
    $script:uiFailed = $true
    $script:uiText = $Text
    if (-not $script:uiForm) { return }
    try {
        $script:uiForm.SetFailed($Text)
        [System.Windows.Forms.Application]::DoEvents()
    } catch { }
}

function Close-UpdateWindow {
    if (-not $script:uiForm) { return }
    try {
        $script:uiForm.Hide()
        $script:uiForm.Dispose()
    } catch { }
    $script:uiForm = $null
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
    $packageCore = Get-CoreExe $package
    if (!$packageCore -or !(Get-SettingsExe $package)) {
        $missing = @()
        if (!$packageCore) { $missing += 'Yunzhao.exe' }
        if (!(Get-SettingsExe $package)) { $missing += 'Settings\Yunzhao.Settings.exe' }
        throw "更新包不完整（缺少 $($missing -join '、')），请重新下载。"
    }
    $installCore = Get-CoreExe $install
    $installSettings = Get-SettingsExe $install
    if (!$installCore -or !$installSettings) {
        $missing = @()
        if (!$installCore) { $missing += 'Yunzhao.exe' }
        if (!$installSettings) { $missing += 'Settings\Yunzhao.Settings.exe' }
        throw ("当前安装目录不完整（缺少 $($missing -join '、')）：$install。" +
               "请在关于页重新下载完整安装包，或用安装程序修复后再试。")
    }
    # Every file still to be copied: the progress comes from this count, not a timer.
    $packageFiles = @(Get-ChildItem -LiteralPath $package -Recurse -File -Force)
    $totalFiles = [Math]::Max(1, $packageFiles.Count)
    $copied = 0
    Show-UpdateWindow (Split-Path -Leaf $package)
    Set-Progress 2 '正在准备更新…'
    # Copy before stopping the app; disk/access failures leave the old app running.
    New-Item -ItemType Directory -Path $incoming | Out-Null
    foreach ($file in $packageFiles) {
        $relative = $file.FullName.Substring($package.Length).TrimStart('\')
        $target = Join-Path $incoming $relative
        $directory = Split-Path -Parent $target
        if (!(Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
        Copy-Item -LiteralPath $file.FullName -Destination $target -Force
        $copied++
        if (($copied % 40) -eq 0 -or $copied -eq $totalFiles) {
            Set-Progress (2 + [int](68.0 * $copied / $totalFiles)) '正在复制新版文件…'
        }
    }
    # A downloaded release must retain the development package's explicit identity.
    if (Test-Path -LiteralPath (Join-Path $install 'development.txt')) {
        Copy-Item -LiteralPath (Join-Path $install 'development.txt') -Destination $incoming -Force
    }
    Set-Progress 72 '正在关闭 Yunzhao…'
    $settingsProcess = Get-Process -Id $WaitForProcessId -ErrorAction SilentlyContinue
    if ($settingsProcess -and !$settingsProcess.WaitForExit(30000)) { throw '设置中心尚未退出，更新未安装。' }
    $quit = Start-Process -FilePath $installCore -ArgumentList '--exit' -WindowStyle Hidden -PassThru
    try {
        if (!$quit.WaitForExit(15000) -or $quit.ExitCode -ne 0) { throw 'Yunzhao 尚未正常退出，更新未安装。' }
    } finally { $quit.Dispose() }
    $coreStopped = $true
    Stop-InstallTree
    Set-Progress 80 '正在替换程序文件…'
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
    Set-Progress 90 '正在启动新版…'
    if ($StartCore -eq '1') { Start-Verified (Join-Path $install 'Yunzhao.exe') 'Core' '' }
    Set-Progress 95 '正在检查新版…'
    $settingsArguments = if ($StartCore -eq '1') { '--page about --require-core-ready' } else { '--page about' }
    Start-Verified (Join-Path $install 'Settings\Yunzhao.Settings.exe') 'Settings' $settingsArguments
    foreach ($process in $started) {
        if ($process.HasExited) { throw '新版程序在启动确认期间退出。' }
    }
    # With the core intentionally off, only settings is launched and verified.
    Set-Progress 100 '更新完成'
    Start-Sleep -Milliseconds 400
    $confirmed = $true
    Remove-Item -LiteralPath $StatePath -Force
    Write-Result ''
} catch {
    $failure = $_.Exception.Message
    $short = if ($failure.Length -gt 60) { $failure.Substring(0, 60) + '…' } else { $failure }
    Set-ProgressFailed $short
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
    # The progress window shows the failure; the settings window reads result.json
    # when it reopens and displays the same text, so no blocking dialog here.
    Set-ProgressFailed '更新未安装，可在关于页查看原因'
    Start-Sleep -Seconds 3
} finally {
    Close-UpdateWindow
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
    # Clean the staged package once the new version is confirmed.
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