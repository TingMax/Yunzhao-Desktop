using System.Diagnostics;
using System.Net;
using System.Net.Http;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Composition.SystemBackdrops;
using Microsoft.UI.Xaml;
using Microsoft.Web.WebView2.Core;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace Yunzhao.Settings;

public sealed partial class MainWindow : Window
{
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);
    private delegate IntPtr SubclassProc(IntPtr hwnd, uint message, UIntPtr wparam, IntPtr lparam, UIntPtr id, UIntPtr data);
    [DllImport("comctl32.dll")] private static extern bool SetWindowSubclass(IntPtr hwnd, SubclassProc proc, UIntPtr id, UIntPtr data);
    [DllImport("comctl32.dll")] private static extern bool RemoveWindowSubclass(IntPtr hwnd, SubclassProc proc, UIntPtr id);
    [DllImport("comctl32.dll")] private static extern IntPtr DefSubclassProc(IntPtr hwnd, uint message, UIntPtr wparam, IntPtr lparam);
    private SubclassProc? _updateMessage;

    // One slider tile: its group, UI range, and the mapping between the stored
    // value and the slider value (percentages are stored as 0..1).
    private sealed record SliderDef(string Group, string Title, double Min, double Max, double Step, string Unit,
        Func<BarSettings, double> Get, Action<BarSettings, double> Set, bool Material);

    private SliderDef[] Sliders => new SliderDef[]
    {
        new("clarity", "透明度", 0, 100, 1, "%",
            s => s.Transparency * 100, (s, v) => s.Transparency = v / 100, true),
        new("clarity", "模糊", 0, 100, 1, "%",
            s => s.Blur * 100, (s, v) => s.Blur = v / 100, true),
        new("edge", "折射强度", 0, 60, 1, "px",
            s => s.Refraction, (s, v) => s.Refraction = v, true),
        new("edge", "折射边缘宽度", 1, 80, 1, "px",
            s => s.Edge, (s, v) => s.Edge = v, true),
        new("edge", "色散", 0, 50, 1, "%",
            s => s.Dispersion * 100, (s, v) => s.Dispersion = v / 100, true),
        new("light", "高光", 0, 100, 1, "%",
            s => s.Highlight * 100, (s, v) => s.Highlight = v / 100, true),
        new("light", "阴影", 0, 100, 1, "%",
            s => s.Shadow * 100, (s, v) => s.Shadow = v / 100, true),

        new("size", "高度", 32, 120, 1, "px",
            s => s.Height, (s, v) => s.Height = v, false),
        new("size", "圆角", 0, 80, 1, "px",
            s => s.CornerRadius, (s, v) => s.CornerRadius = v, false),
        new("dock", "图标大小", 16, 64, 1, "px",
            s => s.IconSize, (s, v) => s.IconSize = v, false),
        new("width", "最大宽度", 300, 2400, 10, "px",
            s => s.Width, (s, v) => s.Width = v, false),
        new("dock", "底部间距", 0, 200, 1, "px",
            s => s.Margin, (s, v) => s.Margin = v, false),
        new("dock", "悬停放大", 100, 200, 5, "%",
            s => s.Magnify * 100, (s, v) => s.Magnify = v / 100, false),

        new("notify", "停留时间", 3, 30, 1, "s",
            s => s.NotifyTimeout, (s, v) => s.NotifyTimeout = v, false),
    };

    private static readonly string[] PageTitles =
    {
        "材质", "形状", "任务栏", "通知", "组件", "灵动岛", "显示器", "系统", "设计语言", "关于",
        "开始菜单", "托盘", "状态栏", "时间", // the bar's parts, listed under it
        "账号",
        "教师工作台",
        "日历",
    };
    private readonly CoreClient _client = new();
    private readonly List<(SliderDef Def, SliderTile Tile)> _rows = new();
    private readonly List<(uint Rgb, Button Button, FrameworkElement Ring)> _tintButtons = new();
    private BarSettings _saved = new();
    private BarSettings _current = new();
    private bool _loading;
    private bool _previewBusy, _previewPending;
    private bool _closePending;
    private FrameworkElement[] _mains = Array.Empty<FrameworkElement>();
    private FrameworkElement?[] _sides = Array.Empty<FrameworkElement?>();
    private int _page;
    private bool _folded; // a narrow window: the inspector's groups under the workspace
    private int _flashId;
    private bool _updatingCoreSwitch, _coreChanging = true;
    private Process? _coreProcess;
    private CancellationTokenSource? _updateCheck;

    public MainWindow()
    {
        InitializeComponent();
        if (MicaController.IsSupported())
            SystemBackdrop = new MicaBackdrop { Kind = MicaKind.Base };
        else
            RootGrid.Background = (Brush)Application.Current.Resources["PageBrush"];
        RootGrid.Loaded += (_, _) => ContinuousCorners.Install(RootGrid);
        RootGrid.Loaded += (_, _) => LoadStageArt(); // the Stage Manager page's pictures, ready before it opens
#if YUNZHAO_DEVELOPMENT
        RootGrid.Loaded += (_, _) => ExportStageArtIfAsked();
#endif
        ExtendsContentIntoTitleBar = true;
        AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;
        SetTitleBar(AppTitleBar);
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "yunzhao.ico"));
        double scale = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0;
        // A roomy default (the preview is the centre of the window), never larger than the screen.
        var work = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary).WorkArea;
        int width = Math.Min((int)(1440 * scale), work.Width * 92 / 100), height = Math.Min((int)(920 * scale), work.Height * 92 / 100);
        AppWindow.MoveAndResize(new Windows.Graphics.RectInt32(work.X + (work.Width - width) / 2, work.Y + (work.Height - height) / 2, width, height));
        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.PreferredMinimumWidth = (int)(980 * scale);
            presenter.PreferredMinimumHeight = (int)(680 * scale);
        }
        AppWindow.Closing += OnClosing;
        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        _updateMessage = (handle, message, wparam, lparam, id, data) =>
        {
            if (message == App.OpenAboutMessage) { SelectPage(9); return IntPtr.Zero; }
            if (message is 0x007E or 0x02E0 || (message == 0x001A && wparam.ToUInt64() == 0x002F))
                DispatcherQueue.TryEnqueue(RefreshDisplays); // hotplug, DPI or work-area changed
            // The island settings window is a separate top-level window parked over this page.
            // Minimizing does not take it with us, and the dock timer would show it again
            // at the restored place, so hide it for as long as this window is minimized.
            if (message == 0x0005)
            {
                var size = wparam.ToUInt32();
                if (size == 1)
                {
                    _hostMinimized = true;
                    DispatcherQueue.TryEnqueue(HideDockedIsland);
                }
                else if (size is 0 or 2)
                {
                    _hostMinimized = false;
                    DispatcherQueue.TryEnqueue(() => { if (_islandPage) DockIsland(); });
                }
            }
            return DefSubclassProc(handle, message, wparam, lparam);
        };
        SetWindowSubclass(hwnd, _updateMessage, (UIntPtr)1, UIntPtr.Zero);
        Closed += (_, _) =>
        {
            _islandPage = false;
            _islandDock?.Stop();
            var islandSettings = FindIslandWindow();
            if (islandSettings != IntPtr.Zero) ShowWindow(islandSettings, 0);
            var calendarSettings = FindCalendarSettingsWindow();
            if (calendarSettings != IntPtr.Zero) ShowWindow(calendarSettings, 0);
            _client.Dispose();
            RemoveWindowSubclass(hwnd, _updateMessage, (UIntPtr)1);
            _updateCheck?.Cancel();
            _updatesClosed = true;
            _downloadTimer?.Stop();
            _reconnectTimer?.Stop();
            if (_teacherOwned && _teacherServer is { HasExited: false })
            {
                try { _teacherServer.Kill(entireProcessTree: true); } catch { /* the page is already going away */ }
            }
            _teacherServer?.Dispose();
            _coreProcess?.Dispose();
            _widgetWatcher?.Dispose();
            _settingsWatcher?.Dispose();
            StopDesignAnimations();
            _renderWorker?.Stop();
            _swatchWorker?.Stop();
            _fixedSwatchWorker?.Stop();
            _photoThawTimer?.Stop();
            ++_photoSelection;
            StopTiles();
            StopModules();
        };

        _mains = new FrameworkElement[] { MainMaterial, MainShape, MainDock, MainNotify, MainWidgets, MainStage, MainDisplays, MainSystem, DesignStack, MainAbout,
                                          MainStart, MainTray, MainStatus, MainClock, MainAccount, PageSlot15, MainCalendar };
        _sides = new FrameworkElement?[] { SideMaterial, SideShape, SideDock, SideNotify, SideWidgets, null, null, null, null, null,
                                           SideStart, SideTray, SideStatus, SideClock, null, null, null };
        RootGrid.Loaded += (_, _) => { HideCalendarSettings(); HideIslandHost(); };
        InitDisplays();
        LoupeSwitch.IsOn = true;
        BuildRows();
        BarRevealDelay.Flat = true;
        BarRevealDelay.Configure("出现前停留", 200, 5000, 100, "ms");
        BarRevealDelay.UserValueChanged += v =>
        {
            if (_loading) return;
            _current.BarRevealDelay = v;
            Changed();
        };
        BarHideDelay.Flat = true;
        BarHideDelay.Configure("离开后隐藏", 200, 5000, 100, "ms");
        BarHideDelay.UserValueChanged += v =>
        {
            if (_loading) return;
            _current.BarHideDelay = v;
            Changed();
        };
        DateSizeSlider.Flat = true;
        DateSizeSlider.Configure("日期字号", 12, 36, 1, "px");
        DateSizeSlider.UserValueChanged += v =>
        {
            if (_loading) return;
            _current.CalendarDateSize = v;
            Changed();
        };
        TableSizeSlider.Flat = true;
        TableSizeSlider.Configure("课表日期", 16, 48, 1, "px");
        TableSizeSlider.UserValueChanged += v =>
        {
            if (_loading) return;
            _current.TimetableDateSize = v;
            Changed();
        };
        BadgeScaleSlider.Flat = true;
        BadgeScaleSlider.Configure("角标大小", 40, 160, 5, "%");
        BadgeScaleSlider.UserValueChanged += v =>
        {
            if (_loading) return;
            _current.TimetableBadgeScale = v / 100;
            Changed();
        };
        DockSpeedSlider.Flat = true;
        DockSpeedSlider.Configure("收起速度", 80, 1200, 1, "ms");
        DockSpeedSlider.UserValueChanged += v =>
        {
            if (_loading) return;
            _current.CalendarDockMs = v;
            Changed();
        };
        IconScaleSlider.Flat = true;
        IconScaleSlider.Configure("导航图标大小", 100, 150, 5, "%");
        IconScaleSlider.UserValueChanged += v => SendCalendarUi("appearance.titlebarIconScale", (int)v);
        FontSizeSlider.Flat = true;
        FontSizeSlider.Configure("字体大小", 8, 40, 1, "");
        FontSizeSlider.UserValueChanged += v => SendCalendarUi("css.fontSizeBase", (int)v);
        RemarkSizeSlider.Flat = true;
        RemarkSizeSlider.Configure("备注字号", 8, 40, 1, "");
        RemarkSizeSlider.UserValueChanged += v => SendCalendarUi("css.noteRemarkFontSize", (int)v);
        CompactFontSlider.Flat = true;
        CompactFontSlider.Configure("灵动岛字号", 8, 40, 1, "");
        CompactFontSlider.UserValueChanged += v => SendCalendarUi("window.compactFontSize", (int)v);
        WindowOpacitySlider.Flat = true;
        WindowOpacitySlider.Configure("玻璃浓度", 0, 100, 1, "%");
        WindowOpacitySlider.UserValueChanged += v => SendCalendarUi("css.windowOpacity", v / 100);
        BlurRadiusSlider.Flat = true;
        BlurRadiusSlider.Configure("模糊半径", 0, 40, 1, "DIP");
        BlurRadiusSlider.UserValueChanged += v => SendCalendarUi("blur.radius", (int)v);
        BlurSaturationSlider.Flat = true;
        BlurSaturationSlider.Configure("饱和度", 0, 2, 0.1, "x");
        BlurSaturationSlider.UserValueChanged += v => SendCalendarUi("blur.saturation", Math.Round(v, 1));
        CornerSlider.Flat = true;
        CornerSlider.Configure("窗口圆角", 0, 30, 1, "px");
        CornerSlider.UserValueChanged += v => SendCalendarUi("blur.cornerRadius", (int)v);
        GlassBlurSlider.Flat = true;
        GlassBlurSlider.Configure("模糊基准", 5, 30, 1, "px");
        GlassBlurSlider.UserValueChanged += v => SendCalendarUi("css.bgBlur", (int)v);
        GlassOpacitySlider.Flat = true;
        GlassOpacitySlider.Configure("霜层基准", 0, 100, 1, "%");
        GlassOpacitySlider.UserValueChanged += v => SendCalendarUi("css.popupOpacity", v / 100);
        BuildTintSwatches();
        InitPins();
        InitHero();
        PhotoChanged += () =>
        {
            if (_page == 4) { ShowWidgetMap(); RefreshGallery(); }
            if (_page == 16) RefreshGallery();
            if (_page == 3) ShowNotifyPreview(false);
            if (_page == 5) ShowStagePreview(false);
            if (_page >= 10) ShowModulePreview(_page);
        };
        PathText.Text = CoreClient.SettingsPath;
        StartupSwitch.IsEnabled = !CoreClient.Development;
        if (CoreClient.Development) ToolTipService.SetToolTip(StartupSwitch, "开发构建不修改 Windows 开机启动项。");
        var v = typeof(MainWindow).Assembly.GetName().Version;
        VersionText.Text = "版本 " + (v?.ToString(3) ?? "1.0.0");
        ShowStatus(false);
        // `--page widgets` (the widgets' menu): open on that page.
        var args = Environment.GetCommandLineArgs();
        int page = Array.IndexOf(args, "--page") is int i && i >= 0 && i + 1 < args.Length
            ? args[i + 1] switch { "dock" => 2, "notify" => 3, "widgets" => 4, "stage" => 5, "displays" => 6, "system" => 7, "about" => 9,
                                 "start" => 10, "tray" => 11, "status" => 12, "clock" => 13, "calendar" => 16, _ => 0 } : 0;
        SelectPage(page);
        _ = ConnectAndLoadAsync();
    }

    // ---- Pages ----

    // The navigation's items name their pages (Tag): the bar's parts sit under it, numbered after the others.
    private void OnNavChanged(object sender, SelectionChangedEventArgs e)
    {
        if (Nav.SelectedItem is ListViewItem { Tag: string tag } && int.TryParse(tag, out int page)) ShowPage(page);
    }

    private void SelectPage(int page)
    {
        foreach (var item in Nav.Items)
            if (item is ListViewItem { Tag: string tag } li && tag == page.ToString()) { Nav.SelectedItem = li; return; }
    }

    // A page is its workspace part (under the title, with the live preview on the first three) and its
    // inspector part; pages without parameters of their own (system, design, about) take the full width.
    private const int TeacherPage = 15;
    private const string TeacherOrigin = "http://127.0.0.1:3090/";
    private static readonly HttpClient TeacherHttp = new() { Timeout = TimeSpan.FromSeconds(2) };
    private Process? _teacherServer;
    private bool _teacherOwned;
    private bool _teacherBooting;

    private void ShowPage(int index)
    {
        if (index == TeacherPage)
        {
            if (_page == 8) StopDesignAnimations();
            _page = index;
            for (int i = 0; i < _mains.Length; i++)
            {
                _mains[i].Visibility = Visibility.Collapsed;
                if (_sides[i] is { } side) side.Visibility = Visibility.Collapsed;
            }
            LeaveModulePages();
            HeroBlock.Visibility = Visibility.Collapsed;
            LoupeToggle.Visibility = Visibility.Collapsed;
            Loupe.Visibility = Visibility.Collapsed;
            CenterScroll.Visibility = Visibility.Collapsed;
            InspectorPane.Visibility = Visibility.Collapsed;
            FoldedInspector.Visibility = Visibility.Collapsed;
            TeacherHost.Visibility = Visibility.Visible;
            HideIslandHost();
            _ = OpenTeacherDeskAsync();
            return;
        }

        TeacherHost.Visibility = Visibility.Collapsed;
        CenterScroll.Visibility = Visibility.Visible;
        if (_page == 8 && index != 8) StopDesignAnimations();
        _page = index;
        for (int i = 0; i < _mains.Length; i++)
        {
            _mains[i].Visibility = i == index ? Visibility.Visible : Visibility.Collapsed;
            if (_sides[i] is { } side) side.Visibility = i == index ? Visibility.Visible : Visibility.Collapsed;
        }
        if (_mains[index] == DesignStack)
        {
            BuildDesignPage();
            RefreshDesignDetails(); // the "current" column follows the settings
        }
        if (_mains[index] == MainWidgets)
        {
            if (WidgetGallery.Children.Count == 0) BuildWidgetGallery(); // built when first shown
            else RefreshGallery();
            ShowWidgetMap();
        }
        if (_mains[index] == MainCalendar)
        {
            _ = RefreshCalendarSwitchAsync();
            LoadCalendarUi();
            HideCalendarSettings();
        }
        else HideCalendarSettings();
        if (_mains[index] == MainNotify) ShowNotifyPreview(true);
        if (_mains[index] == MainStage) ShowStagePreview(true);
        if (_mains[index] == MainDisplays) RefreshDisplays();
        if (_mains[index] == MainAccount) RefreshAccount();
        if (_mains[index] == MainAbout) _ = CheckForUpdatesAsync();
        HideIslandHost();
        if (_mains[index] == MainStage) LoadIslandUi();
        if (index >= 10) EnterModulePage(index);
        else LeaveModulePages();
        PageTitle.Text = PageTitles[index];
        ContinuousCorners.Install(_mains[index]);
        HeroBlock.Visibility = index < 3 ? Visibility.Visible : Visibility.Collapsed;
        LoupeToggle.Visibility = index == 0 ? Visibility.Visible : Visibility.Collapsed;
        Loupe.Visibility = index == 0 && LoupeSwitch.IsOn ? Visibility.Visible : Visibility.Collapsed;
        // The design page reads wider than a settings page.
        CenterStack.MaxWidth = _mains[index] == DesignStack ? 1280 : 1120;
        ArrangeInspector();
        CenterScroll.ChangeView(null, 0, null, true);
        InspectorScroll.ChangeView(null, 0, null, true);
        if (index < 3) UpdateHero();
        if (index == 0) UpdateCustomSwatch();
        AnimateIn(_mains[index]);
        if (_sides[index] is { } shown) AnimateIn(shown);
    }

    // The inspector: its own column when there is room for the workspace beside it, else folded
    // under the workspace (the groups move, keeping their state).
    private const double FoldBelow = 900;

    private void OnContentSizeChanged(object sender, SizeChangedEventArgs e)
    {
        bool folded = e.NewSize.Width < FoldBelow;
        if (folded == _folded && _mains.Length > 0) return;
        _folded = folded;
        if (folded)
        {
            InspectorScroll.Content = null;
            InspectorStack.Margin = new Thickness(0, 8, 0, 0);
            FoldedInspector.Child = InspectorStack;
        }
        else
        {
            FoldedInspector.Child = null;
            InspectorStack.ClearValue(FrameworkElement.MarginProperty);
            InspectorScroll.Content = InspectorStack;
        }
        ArrangeInspector();
    }

    private void ArrangeInspector()
    {
        bool has = _sides.Length > _page && _sides[_page] != null;
        InspectorPane.Visibility = has && !_folded ? Visibility.Visible : Visibility.Collapsed;
        FoldedInspector.Visibility = has && _folded ? Visibility.Visible : Visibility.Collapsed;
    }

    // Every page uses the viewport width, including pages with small intrinsic content.
    private void OnWorkspaceSizeChanged(object sender, SizeChangedEventArgs e) =>
        CenterStack.Width = Math.Max(0, e.NewSize.Width - CenterStack.Margin.Left - CenterStack.Margin.Right);

    // The previews keep a natural proportion as the workspace widens.
    private void OnCenterSizeChanged(object sender, SizeChangedEventArgs e)
    {
        double w = e.NewSize.Width;
        HeroFrame.Height = Math.Clamp(Math.Round(w * 0.52), 280, 580);
        NotifyStage.Height = Math.Clamp(Math.Round(w * 0.42), 260, 440);
    }

    private bool _settingCalendarSwitch;
    private bool _settingCalendarUi;

    private static string CalendarUiPath(string name) =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Yunzhao", name);

    private void SendCalendarUi(string id, object value)
    {
        if (_loading || _settingCalendarUi) return;
        var file = CalendarUiPath("calendar-ui.json");
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        File.WriteAllText(file, System.Text.Json.JsonSerializer.Serialize(new
        {
            at = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
            id,
            value
        }));
    }

    private void LoadCalendarUi()
    {
        var file = CalendarUiPath("calendar-ui-state.json");
        if (!File.Exists(file)) return;
        try
        {
            using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(file));
            var root = doc.RootElement;
            _settingCalendarUi = true;
            if (root.TryGetProperty("titlebarIconScale", out var icon)) IconScaleSlider.SetValue(icon.GetDouble());
            if (root.TryGetProperty("fontSizeBase", out var font)) FontSizeSlider.SetValue(font.GetDouble());
            if (root.TryGetProperty("noteRemarkFontSize", out var remark)) RemarkSizeSlider.SetValue(remark.GetDouble());
            if (root.TryGetProperty("compactFontSize", out var compact)) CompactFontSlider.SetValue(compact.GetDouble());
            if (root.TryGetProperty("popupOpacity", out var glass)) GlassOpacitySlider.SetValue(glass.GetDouble() * 100);
            if (root.TryGetProperty("bgBlur", out var blur)) GlassBlurSlider.SetValue(blur.GetDouble());
            if (root.TryGetProperty("blurRadius", out var radius)) BlurRadiusSlider.SetValue(radius.GetDouble());
            if (root.TryGetProperty("blurSaturation", out var saturation)) BlurSaturationSlider.SetValue(saturation.GetDouble());
            if (root.TryGetProperty("cornerRadius", out var corner)) CornerSlider.SetValue(corner.GetDouble());
            if (root.TryGetProperty("windowOpacity", out var opacity)) WindowOpacitySlider.SetValue(opacity.GetDouble() * 100);
            if (root.TryGetProperty("windowBorder", out var border)) CalendarBorderSwitch.IsOn = border.GetBoolean();
            if (root.TryGetProperty("blurEnabled", out var blurOn)) CalendarBlurSwitch.IsOn = blurOn.GetBoolean();
            if (root.TryGetProperty("tagColorEnabled", out var tags)) TagColorSwitch.IsOn = tags.GetBoolean();
            if (root.TryGetProperty("doubleClickQuickEdit", out var quick)) QuickEditSwitch.IsOn = quick.GetBoolean();
            if (root.TryGetProperty("minimalMode", out var minimal)) MinimalModeSwitch.IsOn = minimal.GetBoolean();
            if (root.TryGetProperty("hideMainViewDuringScreenshot", out var hideShot)) HideShotSwitch.IsOn = hideShot.GetBoolean();
            if (root.TryGetProperty("weatherEnabled", out var weather)) WeatherSwitch.IsOn = weather.GetBoolean();
            if (root.TryGetProperty("receiveNotices", out var notices)) NoticeSwitch.IsOn = notices.GetBoolean();
            if (root.TryGetProperty("uploadDeviceInfo", out var device)) DeviceSwitch.IsOn = device.GetBoolean();
            if (root.TryGetProperty("iconColor", out var color))
                (color.GetString() == "white" ? IconWhite : IconBlack).IsChecked = true;
            if (root.TryGetProperty("bgColor", out var bg)) CalendarBgColor.Text = RgbToHex(bg.GetString());
            if (root.TryGetProperty("textColor", out var text)) CalendarTextColor.Text = text.GetString() ?? "";
            if (root.TryGetProperty("viewVisibility", out var viewKey)) ShortcutView.Text = viewKey.GetString() ?? "";
            if (root.TryGetProperty("newNote", out var newNote)) ShortcutNewNote.Text = newNote.GetString() ?? "";
            if (root.TryGetProperty("screenshot", out var shot)) ShortcutShot.Text = shot.GetString() ?? "";
            if (root.TryGetProperty("screenshotCopy", out var shotCopy)) ShortcutShotCopy.Text = shotCopy.GetString() ?? "";
            if (root.TryGetProperty("uiScale", out var scale)) SelectCombo(CalendarUiScale, scale.GetDouble().ToString(System.Globalization.CultureInfo.InvariantCulture));
            if (root.TryGetProperty("zOrderMode", out var layer)) SelectCombo(CalendarZOrder, layer.GetString());
            if (root.TryGetProperty("revealHandleMode", out var reveal)) SelectCombo(CalendarDockReveal, reveal.GetString());
            if (root.TryGetProperty("enabledEdges", out var edges) && edges.ValueKind == System.Text.Json.JsonValueKind.Array)
            {
                var selected = new HashSet<string>(edges.EnumerateArray().Select(item => item.GetString() ?? ""));
                DockEdgeTop.IsChecked = selected.Contains("top");
                DockEdgeLeft.IsChecked = selected.Contains("left");
                DockEdgeRight.IsChecked = selected.Contains("right");
            }
        }
        catch
        {
            // A missing or partial snapshot keeps the controls at their defaults.
        }
        finally
        {
            _settingCalendarUi = false;
        }
    }

    private static string RgbToHex(string? rgb)
    {
        var parts = (rgb ?? "").Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length != 3 || !parts.All(part => byte.TryParse(part, out _))) return "#FFFFFF";
        return "#" + string.Concat(parts.Select(part => byte.Parse(part).ToString("X2")));
    }

    private static string? HexToRgb(string? hex)
    {
        hex = (hex ?? "").Trim();
        if (hex.StartsWith('#')) hex = hex[1..];
        if (hex.Length != 6 || !hex.All(Uri.IsHexDigit)) return null;
        return $"{Convert.ToInt32(hex[..2], 16)} {Convert.ToInt32(hex[2..4], 16)} {Convert.ToInt32(hex[4..], 16)}";
    }

    private void OnCalendarIconColorChecked(object sender, RoutedEventArgs e)
    {
        if (sender is RadioButton { IsChecked: true, Tag: string color }) SendCalendarUi("appearance.iconColor", color);
    }

    private void OnCalendarBorderToggled(object sender, RoutedEventArgs e) =>
        SendCalendarUi("css.windowBorder", CalendarBorderSwitch.IsOn);

    private void OnCalendarBlurToggled(object sender, RoutedEventArgs e) =>
        SendCalendarUi("blur.enabled", CalendarBlurSwitch.IsOn);

    private void OnCalendarUiScaleChanged(object sender, SelectionChangedEventArgs e)
    {
        if (CalendarUiScale.SelectedItem is ComboBoxItem { Tag: string tag } &&
            double.TryParse(tag, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var scale))
            SendCalendarUi("appearance.uiScale", scale);
    }

    private void OnCalendarBgColorChanged(object sender, RoutedEventArgs e)
    {
        var rgb = HexToRgb(CalendarBgColor.Text);
        if (rgb != null) SendCalendarUi("css.bgColor", rgb);
    }

    private void OnCalendarTextColorChanged(object sender, RoutedEventArgs e)
    {
        var hex = (CalendarTextColor.Text ?? "").Trim();
        if (hex.Length == 7 && hex[0] == '#' && hex[1..].All(Uri.IsHexDigit))
            SendCalendarUi("css.textColor", hex.ToLowerInvariant());
    }

    private static void SelectCombo(ComboBox box, string? tag)
    {
        if (string.IsNullOrEmpty(tag)) return;
        foreach (ComboBoxItem item in box.Items)
            if (item.Tag as string == tag) { box.SelectedItem = item; return; }
    }

    private void OnCalendarZOrderChanged(object sender, SelectionChangedEventArgs e)
    {
        if (CalendarZOrder.SelectedItem is ComboBoxItem { Tag: string tag }) SendCalendarUi("window.zOrderMode", tag);
    }

    private void OnCalendarDockRevealChanged(object sender, SelectionChangedEventArgs e)
    {
        if (CalendarDockReveal.SelectedItem is ComboBoxItem { Tag: string tag }) SendCalendarUi("dock.revealHandleMode", tag);
    }

    private void OnCalendarDockEdgesChanged(object sender, RoutedEventArgs e)
    {
        var edges = new List<string>();
        if (DockEdgeTop.IsChecked == true) edges.Add("top");
        if (DockEdgeLeft.IsChecked == true) edges.Add("left");
        if (DockEdgeRight.IsChecked == true) edges.Add("right");
        SendCalendarUi("dock.enabledEdges", edges);
    }

    private void OnCalendarShortcutChanged(object sender, RoutedEventArgs e)
    {
        if (sender == ShortcutView) SendCalendarUi("shortcuts.viewVisibility", (ShortcutView.Text ?? "").Trim());
        else if (sender == ShortcutNewNote) SendCalendarUi("shortcuts.newNote", (ShortcutNewNote.Text ?? "").Trim());
        else if (sender == ShortcutShot) SendCalendarUi("shortcuts.screenshot", (ShortcutShot.Text ?? "").Trim());
        else if (sender == ShortcutShotCopy) SendCalendarUi("shortcuts.screenshotCopy", (ShortcutShotCopy.Text ?? "").Trim());
    }

    private void OnCalendarTagColorToggled(object sender, RoutedEventArgs e) =>
        SendCalendarUi("notes.tagColorEnabled", TagColorSwitch.IsOn);

    private void OnCalendarQuickEditToggled(object sender, RoutedEventArgs e) =>
        SendCalendarUi("interaction.doubleClickQuickEdit", QuickEditSwitch.IsOn);

    private void OnCalendarMinimalToggled(object sender, RoutedEventArgs e) =>
        SendCalendarUi("listAppearance.minimalMode", MinimalModeSwitch.IsOn);

    private void OnCalendarHideShotToggled(object sender, RoutedEventArgs e) =>
        SendCalendarUi("interaction.hideMainViewDuringScreenshot", HideShotSwitch.IsOn);

    private void OnCalendarWeatherToggled(object sender, RoutedEventArgs e) =>
        SendCalendarUi("weather.enabled", WeatherSwitch.IsOn);

    private void OnCalendarNoticeToggled(object sender, RoutedEventArgs e) =>
        SendCalendarUi("remote.receiveNotices", NoticeSwitch.IsOn);

    private void OnCalendarDeviceToggled(object sender, RoutedEventArgs e) =>
        SendCalendarUi("remote.uploadDeviceInfo", DeviceSwitch.IsOn);


    private async Task RefreshCalendarSwitchAsync()
    {
        bool open = await _client.CalendarOpenAsync();
        if (_updatesClosed) return;
        _settingCalendarSwitch = true;
        CalendarOpenSwitch.IsOn = open;
        _settingCalendarSwitch = false;
    }

    private async void OnCalendarOpenToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || _settingCalendarSwitch) return;
        await _client.SetCalendarOpenAsync(CalendarOpenSwitch.IsOn);
    }

    private void OnEdgeHideToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.CalendarEdgeHide = EdgeHideSwitch.IsOn;
        Changed();
    }

    private void OnLoupeToggled(object sender, RoutedEventArgs e)
    {
        Loupe.Visibility = _page == 0 && LoupeSwitch.IsOn ? Visibility.Visible : Visibility.Collapsed;
        if (Loupe.Visibility == Visibility.Collapsed)
        {
            _loupeBrush.ImageSource = null;
            _loupeBmp = null;
        }
        UpdateHero();
    }

    // Fade and rise a few pixels; the transitions are attached one tick later so the start values stick.
    private void AnimateIn(UIElement e)
    {
        e.OpacityTransition = null;
        e.TranslationTransition = null;
        e.Opacity = 0;
        e.Translation = new Vector3(0, 10, 0);
        DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () =>
        {
            e.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(240) };
            e.TranslationTransition = new Vector3Transition { Duration = TimeSpan.FromMilliseconds(300) };
            e.Opacity = 1;
            e.Translation = Vector3.Zero;
        });
    }

    // ---- Rows ----

    // Every slider is a tile in its group's grid: after the group's other tiles, or before them
    // where the sliders lead (the tint, the notification switch).
    private void BuildRows()
    {
        // The basic three are tiles in the workspace; the rest are plain rows in the inspector.
        var groups = new Dictionary<string, (Panel Panel, bool First, bool Flat)>
        {
            ["clarity"] = (ClarityTiles, true, false), ["edge"] = (EdgeRows, false, true), ["light"] = (LightRows, false, true),
            ["size"] = (SizeRows, false, true), ["width"] = (WidthRows, false, true), ["dock"] = (DockRows, false, true), ["notify"] = (NotifyRows, true, true),
        };
        var placed = new Dictionary<string, int>();
        foreach (var d in Sliders)
        {
            var (panel, first, flat) = groups[d.Group];
            var tile = new SliderTile { Flat = flat };
            tile.Configure(d.Title, d.Min, d.Max, d.Step, d.Unit);
            tile.UserValueChanged += v => OnSliderChanged(d, v);
            tile.Engaged += on => OnEngage(d.Title, on);
            _rows.Add((d, tile));
            int at = first ? placed.GetValueOrDefault(d.Group) : panel.Children.Count;
            panel.Children.Insert(at, tile);
            placed[d.Group] = at + 1;
        }
        AutoWidthSwitch.GotFocus += (_, _) => OnEngage("最大宽度", true);
        AutoWidthSwitch.LostFocus += (_, _) => OnEngage("最大宽度", false);
    }

    private static readonly (string Name, uint Rgb)[] TintChoices =
    {
        ("白色", 0xFFFFFF), ("暖白", 0xFFE6C8), ("冷蓝", 0xD3E6FF), ("玫瑰", 0xFFD0DC), ("薄荷", 0xCCF4E0), ("石墨", 0x2E3138),
    };

    private void BuildTintSwatches()
    {
        foreach (var (name, rgb) in TintChoices)
        {
            var ring = new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 28, Height = 28, Stroke = Tokens.Brush("AccentBrush"), StrokeThickness = 2, Opacity = 0 };
            ring.OpacityTransition = new ScalarTransition { Duration = TimeSpan.FromMilliseconds(160) };
            var dot = new Microsoft.UI.Xaml.Shapes.Ellipse
            {
                Width = 22, Height = 22, Fill = new SolidColorBrush(TintColor(rgb)), Stroke = Tokens.Brush("CardStrokeBrush"), StrokeThickness = 1,
            };
            var button = new Button
            {
                Content = new Grid { Width = 28, Height = 28, Children = { dot, ring } },
                Style = (Style)Application.Current.Resources["IconButtonStyle"], Width = 32, Height = 32, CornerRadius = new CornerRadius(16),
            };
            AutomationProperties.SetName(button, "染色 " + name);
            ToolTipService.SetToolTip(button, name);
            var captured = rgb;
            button.Click += (_, _) => SetTint(captured);
            _tintButtons.Add((rgb, button, ring));
            TintSwatches.Children.Add(button);
        }
    }

    private void SetTint(uint rgb)
    {
        _current.Tint = rgb;
        MarkCustom();
        _loading = true;
        TintPicker.Color = TintColor(rgb);
        _loading = false;
        ShowTint();
        Changed();
    }

    private void ShowTint()
    {
        var c = TintColor(_current.Tint);
        TintSwatch.Fill = new SolidColorBrush(c);
        TintHex.Text = "#" + _current.Tint.ToString("X6", System.Globalization.CultureInfo.InvariantCulture);
        foreach (var (rgb, _, ring) in _tintButtons) ring.Opacity = rgb == _current.Tint ? 1 : 0;
    }

    // ---- Updates ----

    private ReleaseInfo? _latestRelease;
    private ReleaseInstaller.Prepared? _preparedRelease, _restartUpdate;
    private ReleaseInstaller.TransferStatus? _transferStatus;
    private DispatcherTimer? _downloadTimer;
    private bool _downloadRefreshBusy, _queueingDownload, _updatesClosed;
    private string? _updateError;

    private bool HasNewerRelease()
    {
        return _latestRelease?.Download != null && _latestRelease.Version > ReleaseInstaller.ProductVersion();
    }

    private bool HasInstallOffer()
    {
        return _preparedRelease != null || _transferStatus != null || HasNewerRelease();
    }

    private void ShowInstallAction()
    {
        bool ready = _preparedRelease != null;
        bool transferring = _transferStatus != null && !ready;
        bool offer = ready || transferring || HasNewerRelease();
        // A failed transfer retries from the manifest again: the package it was
        // fetching may have been replaced on the server since (404 on resume).
        bool retry = transferring && _transferStatus!.CanRetry;
        CheckUpdateButton.Content = ready ? "点击安装" : retry ? "重新下载" : transferring ? "正在下载" : offer ? "点击安装" : "检查更新";
        CheckUpdateButton.IsEnabled = _updateCheck == null && !_queueingDownload && !(transferring && !_transferStatus!.CanRetry);
        UpdateAvailableButton.Visibility = offer ? Visibility.Visible : Visibility.Collapsed;
        if (!string.IsNullOrEmpty(_updateError)) UpdateStatus.Text = _updateError;
        else if (ready) UpdateStatus.Text = "新版本已准备好，点击安装";
        else if (retry) UpdateStatus.Text = _transferStatus!.Text + "，点击重新下载";
        else if (transferring) UpdateStatus.Text = _transferStatus!.Text;
        else if (offer) UpdateStatus.Text = $"发现新版本 {_latestRelease!.Version.ToString(3)}，点击安装开始下载";
    }

    private void ObserveDownload()
    {
        if (_downloadTimer == null)
        {
            _downloadTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
            _downloadTimer.Tick += async (_, _) => await RefreshDownloadAsync();
        }
        _downloadTimer.Start();
    }

    private async Task RefreshDownloadAsync()
    {
        if (_downloadRefreshBusy || _updatesClosed) return;
        _downloadRefreshBusy = true;
        try
        {
            var state = await Task.Run(() => (Transfer: ReleaseInstaller.ReadTransferStatus(),
                Ready: ReleaseInstaller.ReadPrepared(typeof(MainWindow).Assembly.GetName().Version!)));
            if (_updatesClosed) return;
            _preparedRelease = state.Ready;
            _transferStatus = state.Transfer;
            if (_preparedRelease != null)
                _downloadTimer?.Stop();
            else if (_transferStatus != null)
            {
                if (_transferStatus.CanRetry) _downloadTimer?.Stop();
                else ObserveDownload();
            }
            else _downloadTimer?.Stop();
            ShowInstallAction();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or COMException or JsonException)
        {
            if (!_updatesClosed) UpdateStatus.Text = $"无法读取下载进度：{error.Message}";
            _downloadTimer?.Stop();
        }
        finally { _downloadRefreshBusy = false; }
    }

    private async Task QueueDownloadAsync(ReleaseInfo? release)
    {
        if (_queueingDownload) return;
        _queueingDownload = true;
        _updateError = null;
        UpdateStatus.Text = "正在开始下载…";
        CheckUpdateButton.IsEnabled = false;
        try
        {
            using var worker = release != null ? ReleaseInstaller.StartBackgroundDownload(release)
                : ReleaseInstaller.ResumeBackgroundDownload();
            await worker.WaitForExitAsync(); // closing this window never cancels the transfer
            if (_updatesClosed) return;
            await RefreshDownloadAsync();
            if (worker.ExitCode != 0 && _transferStatus == null)
                _updateError = ReleaseInstaller.ReadFailure() ?? "无法开始后台下载，请重试";
        }
        catch (Exception error) when (error is IOException or System.ComponentModel.Win32Exception)
        {
            if (!_updatesClosed) _updateError = $"无法开始后台下载：{error.Message}";
        }
        finally
        {
            _queueingDownload = false;
            if (!_updatesClosed) ShowInstallAction();
        }
    }

    private async void OnInstallRelease(object sender, RoutedEventArgs e)
    {
        if (_updateCheck != null || _queueingDownload) return;
        if (_transferStatus is { CanRetry: false } && _preparedRelease == null) return;
        _updateError = null;
        if (_preparedRelease != null)
        {
            _restartUpdate = _preparedRelease;
            await CloseRequestedAsync();
            return;
        }
        // A failed transfer restarts from the manifest: the package it was
        // fetching may already be gone from the server (its zip was replaced).
        if (_transferStatus is { CanRetry: true })
        {
            await CheckForUpdatesAsync(startDownload: true);
            return;
        }
        if (_latestRelease?.Download == null && _transferStatus == null) return;
        await QueueDownloadAsync(_latestRelease);
    }

    private void OnOpenUpdate(object sender, RoutedEventArgs e) => SelectPage(9);

    private async void OnCheckForUpdates(object sender, RoutedEventArgs e)
    {
        if (HasInstallOffer())
        {
            OnInstallRelease(sender, e);
            return;
        }
        await CheckForUpdatesAsync(startDownload: true);
    }

    private async Task CheckForUpdatesAsync(bool startDownload = false)
    {
        if (_updateCheck != null) return;
        _updateError = null;
        using var cancel = new CancellationTokenSource();
        _updateCheck = cancel;
        CheckUpdateButton.IsEnabled = false;
        UpdateProgress.Visibility = Visibility.Visible;
        UpdateProgress.IsActive = true;
        UpdateStatus.Text = "正在检查…";
        try
        {
            await RefreshDownloadAsync();
            if (cancel.IsCancellationRequested) return;
            if (_preparedRelease != null)
            {
                ShowInstallAction();
                return;
            }
            var release = await ReleaseInfo.FetchLatestAsync(cancel.Token);
            if (cancel.IsCancellationRequested) return;
            bool newer = release?.Download != null && release.Version > ReleaseInstaller.ProductVersion();
            _latestRelease = newer ? release : null;
            if (!newer && _transferStatus == null)
            {
                UpdateAvailableButton.Visibility = Visibility.Collapsed;
                CheckUpdateButton.Content = "检查更新";
                UpdateStatus.Text = "软件暂无更新";
                return;
            }
            ShowInstallAction();
            // A retry of a failed transfer goes straight on to downloading the
            // package the manifest now names, instead of asking again.
            if (startDownload && _latestRelease?.Download != null) QueueDownloadAsync(_latestRelease);
        }
        catch (Exception error) when (error is HttpRequestException or OperationCanceledException
            or JsonException or FormatException or InvalidOperationException or KeyNotFoundException or IOException or UnauthorizedAccessException)
        {
            if (cancel.IsCancellationRequested) return; // the window closed while checking
            _updateError = _transferStatus?.Text ?? (error switch
            {
                OperationCanceledException => "连接超时，请重试",
                HttpRequestException { StatusCode: HttpStatusCode.Forbidden or HttpStatusCode.TooManyRequests }
                    => "更新服务器暂时无法响应，请稍后重试",
                HttpRequestException => error.Message.StartsWith("无法连接", StringComparison.Ordinal)
                    ? error.Message
                    : "无法连接更新服务器：" + (error.InnerException?.Message ?? error.Message),
                IOException or UnauthorizedAccessException => $"无法准备更新：{error.Message}",
                _ => "无法读取版本信息，请稍后重试",
            });
        }
        finally
        {
            _updateCheck = null;
            if (!cancel.IsCancellationRequested)
            {
                UpdateProgress.IsActive = false;
                UpdateProgress.Visibility = Visibility.Collapsed;
                CheckUpdateButton.IsEnabled = true;
                ShowInstallAction();
            }
        }
    }

    // ---- Connection ----

    private async Task ConnectAndLoadAsync()
    {
        await _client.ConnectAsync(800);
        var (saved, current) = await _client.GetAsync();
        if (_updatesClosed) return;
        _saved = saved;
        _current = current;
        _coreChanging = false;
        WatchCore();
        WatchSettingsFile();
        ShowStatus(_client.Connected);
        LoadIntoControls();
        UpdateButtons();
        if (Content is FrameworkElement { IsLoaded: true }) App.SignalStartupReady(_client.Connected && _client.StateConfirmed);
        else if (Content is FrameworkElement root) root.Loaded += (_, _) => App.SignalStartupReady(_client.Connected && _client.StateConfirmed);
        if (_reconnectTimer == null && !_updatesClosed)
        {
            _reconnectTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
            _reconnectTimer.Tick += async (_, _) => await ReconnectAsync();
            _reconnectTimer.Start();
        }
    }

    // The core started again on its own (an update, the guard after a crash, a launch from
    // elsewhere): this window connects to it by itself. Edits made meanwhile stay and are previewed.
    private DispatcherTimer? _reconnectTimer;
    private bool _reconnecting;

    private async Task ReconnectAsync()
    {
        if (_reconnecting || _coreChanging || _updatesClosed || _client.Connected) return;
        _reconnecting = true;
        try
        {
            if (!await _client.ConnectAsync(300) || _updatesClosed)
            {
                if (!_updatesClosed && !_coreChanging) ShowStatus(_client.Connected);
                return;
            }
            bool dirty = !_current.SameAs(_saved);
            var (saved, current) = await _client.GetAsync();
            if (_updatesClosed) return;
            _saved = saved;
            if (!dirty) _current = current;
            LoadIntoControls();
            UpdateButtons();
            if (dirty) SchedulePreview();
            WatchCore();
            ShowStatus(_client.Connected);
        }
        finally { _reconnecting = false; }
    }

    private static readonly SolidColorBrush RunningBrush = new(Color.FromArgb(255, 76, 194, 106));
    private static readonly SolidColorBrush IdleBrush = new(Color.FromArgb(255, 150, 150, 156));

    private void ShowStatus(bool connected)
    {
        StatusDot.Fill = connected ? RunningBrush : IdleBrush;
        _updatingCoreSwitch = true;
        CoreSwitch.IsOn = connected || (_coreProcess != null && !_coreProcess.HasExited);
        _updatingCoreSwitch = false;
        CoreSwitch.IsEnabled = !_coreChanging && CoreClient.CoreExe() != null;
        if (!_coreChanging) StatusMessage.Text = connected ? "正在运行" : _client.ConnectionError ??
            (_coreProcess != null && !_coreProcess.HasExited ? "核心进程仍在运行，设置连接已断开" : "已关闭，系统任务栏已恢复");
        var account = DesktopAccount.Read();
        StatusTitle.Text = string.IsNullOrWhiteSpace(account?.Name) ? "Yunzhao" : account.Name;
    }

    private static string? TeacherRoot()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
        {
            var server = Path.Combine(dir.FullName, "third_party", "teacher_works", "backend", "server.js");
            if (File.Exists(server)) return Path.GetDirectoryName(Path.GetDirectoryName(server));
        }
        return null;
    }

    private static async Task<bool> TeacherHealthy()
    {
        try
        {
            using var response = await TeacherHttp.GetAsync(TeacherOrigin + "api/v1/health");
            return response.IsSuccessStatusCode;
        }
        catch
        {
            return false;
        }
    }

    // The desk is the transplanted teacher-works app. Its own pages stay intact;
    // this window only hosts them in the content pane.
    private async Task OpenTeacherDeskAsync()
    {
        if (_teacherBooting) return;
        if (TeacherView.CoreWebView2 != null && TeacherView.Source is { } current &&
            current.AbsoluteUri.StartsWith(TeacherOrigin, StringComparison.OrdinalIgnoreCase))
        {
            TeacherStatus.Visibility = Visibility.Collapsed;
            TeacherView.Reload();
            return;
        }

        _teacherBooting = true;
        TeacherStatus.Visibility = Visibility.Visible;
        TeacherStatus.Text = "正在打开教师工作台…";
        try
        {
            var root = TeacherRoot();
            if (root == null)
            {
                TeacherStatus.Text = "没有找到教师工作台。";
                return;
            }
            if (!File.Exists(Path.Combine(root, "frontend", "dist", "index.html")))
            {
                TeacherStatus.Text = "教师工作台还没有构建好。";
                return;
            }
            if (!await TeacherHealthy())
            {
                TeacherStatus.Text = "正在启动教师工作台…";
                var start = new ProcessStartInfo
                {
                    FileName = "node",
                    Arguments = "server.js",
                    WorkingDirectory = Path.Combine(root, "backend"),
                    UseShellExecute = false,
                    CreateNoWindow = true,
                };
                start.Environment["PORT"] = "3090";
                _teacherServer = Process.Start(start);
                _teacherOwned = _teacherServer != null;
                for (var i = 0; i < 40 && !await TeacherHealthy(); i++) await Task.Delay(250);
            }
            if (!await TeacherHealthy())
            {
                TeacherStatus.Text = "教师工作台没有启动。请确认本机可以运行 node。";
                return;
            }

            var folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Yunzhao", "teacher-webview");
            Directory.CreateDirectory(folder);
            Environment.SetEnvironmentVariable("WEBVIEW2_USER_DATA_FOLDER", folder);
            await TeacherView.EnsureCoreWebView2Async();
            TeacherView.DefaultBackgroundColor = Color.FromArgb(255, 245, 245, 247);
            TeacherView.Source = new Uri(TeacherOrigin);
        }
        catch (Exception ex)
        {
            TeacherStatus.Text = "教师工作台打不开：" + ex.Message;
        }
        finally
        {
            _teacherBooting = false;
        }
    }

    private void OnTeacherNavigated(WebView2 sender, CoreWebView2NavigationCompletedEventArgs args)
    {
        if (args.IsSuccess) TeacherStatus.Visibility = Visibility.Collapsed;
        else if (TeacherStatus.Visibility == Visibility.Visible) TeacherStatus.Text = "教师工作台页面没有打开。";
    }

    private static string? FlyIslandRoot()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, "third_party", "fly-island");
            if (Directory.Exists(candidate)) return candidate;
        }
        return null;
    }

    private static string? FlyIslandExe()
    {
        var root = FlyIslandRoot();
        if (root == null) return null;
        foreach (var relative in new[]
        {
            Path.Combine("src-tauri", "target", "release", "focusd-island.exe"),
            Path.Combine("src-tauri", "target", "debug", "focusd-island.exe"),
        })
        {
            var path = Path.Combine(root, relative);
            if (File.Exists(path)) return path;
        }
        return null;
    }

    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr param);
    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr param);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr hwnd, System.Text.StringBuilder text, int count);
    [DllImport("user32.dll")]
    private static extern int GetWindowLong(IntPtr hwnd, int index);
    [DllImport("user32.dll")]
    private static extern int SetWindowLong(IntPtr hwnd, int index, int value);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")]
    private static extern IntPtr SetWindowLongPtr(IntPtr hwnd, int index, IntPtr value);
    [DllImport("user32.dll")]
    private static extern bool SetWindowPos(IntPtr hwnd, IntPtr insertAfter, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hwnd, int command);
    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")]
    private static extern bool IsIconic(IntPtr hwnd);

    private bool _islandPage;
    private bool _calendarSettingsPage;
    private bool _hostMinimized;
    private bool _islandLaunching;
    private DispatcherTimer? _islandDock;
    private DispatcherTimer? _calendarSettingsDock;
    private EnumWindowsProc? _islandEnum;
    private EnumWindowsProc? _calendarSettingsEnum;

    private IntPtr FindIslandWindow()
    {
        var ids = new HashSet<uint>();
        foreach (var process in Process.GetProcessesByName("focusd-island"))
            ids.Add((uint)process.Id);
        if (ids.Count == 0) return IntPtr.Zero;
        IntPtr found = IntPtr.Zero;
        _islandEnum = (hwnd, _) =>
        {
            if (GetWindowThreadProcessId(hwnd, out var pid) == 0 || !ids.Contains(pid)) return true;
            var text = new System.Text.StringBuilder(256);
            GetWindowText(hwnd, text, text.Capacity);
            var title = text.ToString();
            if (title.Contains("设置") || title == "Yunzhao 灵动岛")
            {
                found = hwnd;
                return false;
            }
            return true;
        };
        EnumWindows(_islandEnum, IntPtr.Zero);
        return found;
    }

    private void EnsureFlyIsland()
    {
        if (_islandLaunching || FindIslandWindow() != IntPtr.Zero || FlyIslandExe() == null) return;
        if (Process.GetProcessesByName("focusd-island").Length > 0) return;
        var exe = FlyIslandExe()!;
        var info = new ProcessStartInfo(exe)
        {
            WorkingDirectory = Path.GetDirectoryName(exe)!,
            UseShellExecute = false,
        };
        info.Environment["YUNZHAO_HOST"] = "1";
        _islandLaunching = true;
        Process.Start(info);
    }

    private void RefreshIsland()
    {
        _islandPage = true;
        EnsureFlyIsland();
        if (_islandDock == null)
        {
            _islandDock = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(32) };
            _islandDock.Tick += (_, _) => DockIsland();
            AppWindow.Changed += (_, _) => { if (_islandPage) DockIsland(); };
            IslandHost.SizeChanged += (_, _) => DockIsland();
            CenterScroll.ViewChanged += (_, _) => { if (_islandPage) DockIsland(); };
        }
        _islandDock.Start();
        double room = CenterScroll.ViewportHeight - 8;
        if (room > 480) IslandHost.Height = room;
        DockIsland();
    }

    private void HideIslandHost()
    {
        _islandPage = false;
        _islandLaunching = false;
        _islandDock?.Stop();
        var hwnd = FindIslandWindow();
        if (hwnd != IntPtr.Zero) ShowWindow(hwnd, 0);
    }

    private bool HostMinimized()
    {
        if (_hostMinimized) return true;
        return IsIconic(WinRT.Interop.WindowNative.GetWindowHandle(this));
    }

    private void HideDockedIsland()
    {
        var hwnd = FindIslandWindow();
        if (hwnd != IntPtr.Zero) ShowWindow(hwnd, 0);
    }

    private void DockIsland()
    {
        if (!_islandPage) return;
        if (HostMinimized())
        {
            _hostMinimized = true;
            HideDockedIsland();
            return;
        }
        var hwnd = FindIslandWindow();
        if (hwnd == IntPtr.Zero)
        {
            EnsureFlyIsland();
            return;
        }
        _islandLaunching = false;
        FitHostToWindow(IslandHost);
        PlaceDocked(IslandHost, hwnd);
    }

    private IntPtr FindCalendarSettingsWindow()
    {
        IntPtr found = IntPtr.Zero;
        _calendarSettingsEnum = (hwnd, _) =>
        {
            var text = new System.Text.StringBuilder(256);
            GetWindowText(hwnd, text, text.Capacity);
            if (text.ToString() == "Yunzhao 日历设置")
            {
                found = hwnd;
                return false;
            }
            return true;
        };
        EnumWindows(_calendarSettingsEnum, IntPtr.Zero);
        return found;
    }

    private void HideCalendarSettings()
    {
        _calendarSettingsPage = false;
        _calendarSettingsDock?.Stop();
        var hwnd = FindCalendarSettingsWindow();
        if (hwnd != IntPtr.Zero) ShowWindow(hwnd, 0);
    }

    // The hosted surface is a separate window. Keep it inside this window, including while the page scrolls.
    private void FitHostToWindow(FrameworkElement host)
    {
        if (host.XamlRoot == null) return;
        double scale = host.XamlRoot.RasterizationScale;
        if (scale <= 0) return;
        var origin = host.TransformToVisual(null).TransformPoint(new Windows.Foundation.Point(0, 0));
        double available = AppWindow.Size.Height / scale - 12 - origin.Y;
        if (available < 48) return;
        if (double.IsNaN(host.Height) || Math.Abs(host.Height - available) > 2)
            host.Height = available;
    }

    private void PlaceDocked(FrameworkElement host, IntPtr hwnd)
    {
        if (host.XamlRoot == null || host.ActualWidth < 40 || host.ActualHeight < 40)
        {
            ShowWindow(hwnd, 0);
            return;
        }
        double scale = host.XamlRoot.RasterizationScale;
        var origin = host.TransformToVisual(null).TransformPoint(new Windows.Foundation.Point(0, 0));
        var pos = AppWindow.Position;
        int hostLeft = pos.X + (int)Math.Round(origin.X * scale);
        int hostTop = pos.Y + (int)Math.Round(origin.Y * scale);
        int hostRight = pos.X + (int)Math.Round((origin.X + host.ActualWidth) * scale);
        int hostBottom = pos.Y + (int)Math.Round((origin.Y + host.ActualHeight) * scale);
        var owner = WinRT.Interop.WindowNative.GetWindowHandle(this);
        if (!GetWindowRect(owner, out var frame))
        {
            ShowWindow(hwnd, 0);
            return;
        }
        int x = Math.Max(hostLeft, frame.Left);
        int y = Math.Max(hostTop, frame.Top);
        int right = Math.Min(hostRight, frame.Right);
        int bottom = Math.Min(hostBottom, frame.Bottom);
        int width = right - x;
        int height = bottom - y;
        if (width < 80 || height < 48)
        {
            ShowWindow(hwnd, 0);
            return;
        }
        SetWindowLongPtr(hwnd, -8, owner);
        const int extendedStyle = -20;
        var style = GetWindowLong(hwnd, extendedStyle);
        SetWindowLong(hwnd, extendedStyle, (style | 0x80) & ~0x40000);
        SetWindowPos(hwnd, IntPtr.Zero, x, y, width, height, 0x0014);
        ShowWindow(hwnd, 4);
    }

    private void RefreshAccount()
    {
        var account = DesktopAccount.Read();
        if (account == null)
        {
            AccountName.Text = "未登录";
            AccountUser.Text = "登录后，日历和课表使用同一个账号。";
            AccountMail.Text = "";
            SignOutButton.IsEnabled = false;
            StatusTitle.Text = "Yunzhao";
            return;
        }
        AccountName.Text = account.Name;
        AccountUser.Text = string.IsNullOrWhiteSpace(account.Username) ? "" : "用户名  " + account.Username;
        AccountMail.Text = account.Email;
        SignOutButton.IsEnabled = true;
        StatusTitle.Text = account.Name;
    }

    private async void OnSignOut(object sender, RoutedEventArgs e)
    {
        var account = DesktopAccount.Read();
        if (account == null)
        {
            RefreshAccount();
            return;
        }
        SignOutButton.IsEnabled = false;
        AccountStatus.Text = "正在把数据上传到服务器";
        account.RequestSignOut();
        for (var i = 0; i < 40; i++)
        {
            await Task.Delay(500);
            if (_updatesClosed) return;
            if (!File.Exists(DesktopAccount.SessionPath))
            {
                AccountStatus.Text = "已退出";
                RefreshAccount();
                return;
            }
        }
        AccountStatus.Text = "上传时间较长，退出完成后会回到登录页";
    }

    private void ShowMessage(InfoBarSeverity severity, string title, string message)
    {
        Status.Severity = severity;
        Status.Title = title;
        Status.Message = message;
        Status.IsOpen = true;
    }

    // Follow the process exit event rather than polling the pipe while the settings window is idle.
    private void WatchCore()
    {
        if (_updatesClosed) return;
        if (!_client.Connected && _coreProcess != null && !_coreProcess.HasExited) return;
        _coreProcess?.Dispose();
        _coreProcess = null;
        if (!_client.Connected || _updatesClosed) return;
        try
        {
            var process = Process.GetProcessById(_client.ProcessId);
            _coreProcess = process;
            process.Exited += (_, _) => DispatcherQueue.TryEnqueue(() =>
            {
                if (_coreProcess != process) return;
                _client.Disconnect(coreStopped: true);
                _coreProcess.Dispose();
                _coreProcess = null;
                ShowStatus(false);
            });
            process.EnableRaisingEvents = true;
        }
        catch (ArgumentException) { _client.Disconnect(coreStopped: true); }
    }

    private async void OnCoreToggled(object sender, RoutedEventArgs e)
    {
        if (_updatingCoreSwitch || _coreChanging) return;
        bool start = CoreSwitch.IsOn;
        _coreChanging = true;
        CoreSwitch.IsEnabled = false;
        StatusMessage.Text = start ? "正在启动…" : "正在关闭…";
        Status.IsOpen = false;
        try
        {
            var exe = CoreClient.CoreExe() ?? throw new FileNotFoundException("没有找到 Yunzhao 程序");
            if (start)
            {
                using var launched = Process.Start(new ProcessStartInfo(exe) { UseShellExecute = true });
                if (!await _client.ConnectAsync(8000)) throw new TimeoutException("未能连接 Yunzhao，请稍后重试。");
                bool dirty = !_current.SameAs(_saved);
                var (saved, current) = await _client.GetAsync();
                _saved = saved;
                if (!dirty) _current = current;
                LoadIntoControls();
                UpdateButtons();
                if (dirty) SchedulePreview();
            }
            else
            {
                _client.Disconnect();
                WatchCore();
                using var quit = Process.Start(new ProcessStartInfo(exe, "--exit") { UseShellExecute = false, CreateNoWindow = true })
                    ?? throw new InvalidOperationException("未能发送退出请求。");
                await quit.WaitForExitAsync();
                if (quit.ExitCode != 0) throw new InvalidOperationException("Yunzhao 尚未退出，请稍后重试。");
                _client.Disconnect(coreStopped: true);
            }
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException or TimeoutException or IOException)
        {
            ShowMessage(InfoBarSeverity.Error, start ? "启动失败" : "关闭失败", error.Message);
        }
        finally
        {
            _coreChanging = false;
            WatchCore();
            ShowStatus(_client.Connected);
        }
    }

    // ---- Changes made elsewhere ----

    // The core writes settings.json for changes made at the bar (its menu, the control centre):
    // what changed there comes into the controls, the edits made here meanwhile stay.
    private FileSystemWatcher? _settingsWatcher;
    private int _settingsReload;

    private void WatchSettingsFile()
    {
        string? dir = Path.GetDirectoryName(CoreClient.SettingsPath);
        if (_settingsWatcher != null || dir == null || !Directory.Exists(dir)) return;
        _settingsWatcher = new FileSystemWatcher(dir, "settings.json")
        {
            NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.FileName | NotifyFilters.Size, EnableRaisingEvents = true,
        };
        FileSystemEventHandler changed = (_, _) => DispatcherQueue.TryEnqueue(ReloadExternal);
        _settingsWatcher.Changed += changed;
        _settingsWatcher.Created += changed;
        _settingsWatcher.Renamed += (_, _) => DispatcherQueue.TryEnqueue(ReloadExternal);
    }

    private async void ReloadExternal()
    {
        int id = ++_settingsReload;
        await Task.Delay(120); // one reload for a burst of writes
        if (id != _settingsReload || _updatesClosed || !_client.Connected) return;
        var (saved, _) = await _client.GetAsync();
        if (id != _settingsReload || _updatesClosed) return;
        var before = _saved.ToJson();
        var after = saved.ToJson();
        var changed = after.Select(kv => kv.Key).Where(k => !JsonNode.DeepEquals(before[k], after[k])).ToList();
        if (changed.Count == 0) return;
        var current = _current.ToJson();
        foreach (var k in changed) current[k] = after[k]?.DeepClone();
        _saved = saved;
        _current = BarSettings.FromJson(current, _current);
        LoadIntoControls();
        UpdateButtons();
        if (_page == 4) ShowWidgetMap();
        if (_page == 5) ShowStagePreview(true);
        if (!_current.SameAs(_saved)) SchedulePreview(); // the edits made here, over the new state
    }

    // ---- Controls <-> settings ----

    private void LoadIntoControls()
    {
        _loading = true;
        foreach (var (d, card) in _rows)
            card.SetValue(d.Get(_current));
        (_current.Preset switch { "liquid" => PresetLiquid, "frosted" => PresetFrosted, _ => PresetCustom }).IsChecked = true;
        (Photo.Selected switch { "wallpaper" => BgWallpaper, "paper" => BgPaper, "geometry" => BgGeometry, "layers" => BgLayers, _ => BgSpecimen }).IsChecked = true;
        ClockSwitch.IsOn = _current.ShowClock;
        TraySwitch.IsOn = _current.ShowTray;
        StartTraySwitch.IsOn = _current.StartTray;
        StartStatusSwitch.IsOn = _current.StartStatus;
        (_current.StartSize switch { "small" => StartSmall, "large" => StartLarge, _ => StartMedium }).IsChecked = true;
        StatusSwitch.IsOn = _current.ShowStatus;
        NotificationsSwitch.IsOn = _current.ShowNotifications;
        AutoWidthSwitch.IsOn = _current.AutoWidth;
        HoverRiseSwitch.IsOn = _current.HoverRise;
        RunningSwitch.IsOn = _current.ShowRunning;
        HideBodySwitch.IsOn = _current.NotifyHideBody;
        (_current.NotifyPlacement == "topRight" ? NotifyAtTopRight : NotifyAtDock).IsChecked = true;
        ListenerSwitch.IsOn = _current.NotifyListener;
        WeChatSwitch.IsOn = _current.NotifyWeChat;
        FeishuSwitch.IsOn = _current.NotifyFeishu;
        BarSwitch.IsOn = _current.Bar;
        LaunchpadSwitch.IsOn = _current.ShowLaunchpad;
        FolderStacksSwitch.IsOn = _current.ShowFolderStacks;
        NowPlayingSwitch.IsOn = _current.ShowNowPlaying;
        UpdateBarModule();
        WidgetsSwitch.IsOn = _current.Widgets;
        (_current.WidgetsOnRight ? WidgetsRight : WidgetsLeft).IsChecked = true;
        HideTaskbarSwitch.IsOn = _current.HideTaskbar;
        StageSwitch.IsOn = _current.StageManager;
        (_current.StageOnRight ? StageRight : StageLeft).IsChecked = true;
        (_current.StageAutoHide ? StageAutoHide : StageAlways).IsChecked = true;
        StageMinimizeSwitch.IsOn = _current.StageMinimize;
        (_current.WidgetBackdrop == "wallpaper" ? BackdropWallpaper : BackdropDesktop).IsChecked = true;
        (_current.WidgetMaterial switch { "dusk" => MaterialDusk, "color" => MaterialColor, _ => MaterialGlass }).IsChecked = true;
        DateSizeSlider.SetValue(_current.CalendarDateSize);
        TableSizeSlider.SetValue(_current.TimetableDateSize);
        BadgeScaleSlider.SetValue(_current.TimetableBadgeScale * 100);
        DockSpeedSlider.SetValue(_current.CalendarDockMs);
        EdgeHideSwitch.IsOn = _current.CalendarEdgeHide;
        HideBannersSwitch.IsOn = _current.HideBanners;
        // Global Win-key hooks are disabled in the core until conflict handling exists.
        _current.WinKeyStart = false;
        _current.WinNumbers = false;
        WinKeySwitch.IsOn = false;
        WinNumbersSwitch.IsOn = false;
        WinKeySwitch.IsEnabled = false;
        WinNumbersSwitch.IsEnabled = false;
        (_current.StatusStyle == "icons" ? StatusIcons : StatusRing).IsChecked = true;
        (_current.AutoHide switch { "never" => HideNever, "always" => HideAlways, _ => HideFullscreen }).IsChecked = true;
        BarRevealDelay.SetValue(_current.BarRevealDelay);
        BarHideDelay.SetValue(_current.BarHideDelay);
        StartupSwitch.IsOn = _current.StartOnLogin;
        LoadDisplayControls();
        (_current.TextColor switch { "light" => TextLight, "dark" => TextDark, _ => TextAuto }).IsChecked = true;
        UpdateGallery();
        RebuildPins();
        WindowsContextMenuSwitch.IsOn = _current.WindowsContextMenu;
        TintPicker.Color = TintColor(_current.Tint);
        ShowTint();
        _loading = false;
        UpdateHero();
        UpdateCustomSwatch();
        if (_page == 3) ShowNotifyPreview(false);
        if (_page == 4) ShowWidgetMap();
        if (_page == 5) ShowStagePreview(false);
        if (_page >= 10) ShowModulePreview(_page);
    }

    private static Color TintColor(uint rgb) => Color.FromArgb(255, (byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb);

    private void OnSliderChanged(SliderDef d, double v)
    {
        if (_loading) return;
        d.Set(_current, v);
        if (d.Material) MarkCustom();
        Changed();
    }

    private void MarkCustom()
    {
        _current.Preset = "custom";
        bool was = _loading;
        _loading = true;
        PresetCustom.IsChecked = true;
        _loading = was;
    }

    private void OnPresetChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        if (tag == "liquid") _current.ApplyMaterial(BarSettings.Liquid());
        else if (tag == "frosted") _current.ApplyMaterial(BarSettings.Frosted());
        else _current.Preset = "custom";
        LoadIntoControls();
        Changed();
    }

    private void OnTintChanged(ColorPicker sender, ColorChangedEventArgs args)
    {
        if (_loading) return;
        _current.Tint = ((uint)args.NewColor.R << 16) | ((uint)args.NewColor.G << 8) | args.NewColor.B;
        MarkCustom();
        ShowTint();
        Changed();
    }

    private void OnClockToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.ShowClock = ClockSwitch.IsOn;
        Changed();
    }

    private void OnBarContentToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.ShowTray = TraySwitch.IsOn;
        _current.StartTray = StartTraySwitch.IsOn;
        _current.StartStatus = StartStatusSwitch.IsOn;
        _current.ShowStatus = StatusSwitch.IsOn;
        _current.ShowNotifications = NotificationsSwitch.IsOn;
        Changed();
    }

    private void OnWidgetAlignmentChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.WidgetsOnRight = tag == "right";
        // With Stage Manager on, it keeps to the widgets' other side.
        if (_current.StageManager) SetStageSide(!_current.WidgetsOnRight);
        ShowWidgetMap();
        Changed();
    }

    private void SetStageSide(bool right)
    {
        _current.StageOnRight = right;
        _loading = true;
        (right ? StageRight : StageLeft).IsChecked = true;
        _loading = false;
    }

    private void SetWidgetSide(bool right)
    {
        _current.WidgetsOnRight = right;
        _loading = true;
        (right ? WidgetsRight : WidgetsLeft).IsChecked = true;
        _loading = false;
    }

    private void OnAutoWidthToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.AutoWidth = AutoWidthSwitch.IsOn;
        Changed();
    }

    private void OnHoverRiseToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.HoverRise = HoverRiseSwitch.IsOn;
        Changed();
    }

    private void OnRunningToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.ShowRunning = RunningSwitch.IsOn;
        Changed();
    }

    private void OnHideBodyToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.NotifyHideBody = HideBodySwitch.IsOn;
        Changed();
    }

    private void OnListenerToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.NotifyListener = ListenerSwitch.IsOn;
        Changed();
    }

    private void OnFeishuToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.NotifyFeishu = FeishuSwitch.IsOn;
        Changed();
    }

    // The bar module: off, its own options and the notification cards have nothing to act on.
    private void OnBarToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.Bar = BarSwitch.IsOn;
        UpdateBarModule();
        Changed();
    }

    private void OnLaunchpadToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.ShowLaunchpad = LaunchpadSwitch.IsOn;
        Changed();
    }

    private void OnFolderStacksToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.ShowFolderStacks = FolderStacksSwitch.IsOn;
        Changed();
    }

    private void OnNowPlayingToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.ShowNowPlaying = NowPlayingSwitch.IsOn;
        Changed();
    }

    private void UpdateBarModule()
    {
        foreach (var part in new FrameworkElement[] { DockContent, DockLayout, SideNotify })
        {
            part.Opacity = _current.Bar ? 1 : 0.45;
            part.IsHitTestVisible = _current.Bar;
        }
    }

    private void OnWidgetsToggled(object sender, RoutedEventArgs e)
    {
        UpdateGallery();
        if (_loading) return;
        _current.Widgets = WidgetsSwitch.IsOn;
        Changed();
    }

    // The widgets' material: the map and the gallery show it at once, the core with the preview.
    private void OnWidgetMaterialChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag } || tag == _current.WidgetMaterial) return;
        _current.WidgetMaterial = tag;
        Changed();
        if (_page == 4) { ShowWidgetMap(); RefreshGallery(); }
        if (_page == 16) RefreshGallery();
    }

    private void OnWidgetBackdropChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.WidgetBackdrop = tag;
        Changed();
    }

    private void OnHideTaskbarToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.HideTaskbar = HideTaskbarSwitch.IsOn;
        Changed();
    }

    private void OnStageToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.StageManager = StageSwitch.IsOn;
        // The widgets go to the other side.
        if (_current.StageManager) SetWidgetSide(!_current.StageOnRight);
        Changed();
        if (_page == 5) ShowStagePreview(true);
    }

    private void OnStageSideChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.StageOnRight = tag == "right";
        if (_current.StageManager) SetWidgetSide(!_current.StageOnRight);
        Changed();
        if (_page == 5) ShowStagePreview(true);
    }

    private void OnStageModeChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.StageAutoHide = tag == "hover";
        Changed();
        if (_page == 5) ShowStagePreview(true);
    }


    private void OnStartupToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.StartOnLogin = StartupSwitch.IsOn;
        Changed();
    }

    private void OnHideBannersToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.HideBanners = HideBannersSwitch.IsOn;
        Changed();
    }

    private void OnStageMinimizeToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.StageMinimize = StageMinimizeSwitch.IsOn;
        Changed();
    }

    private void OnWinKeyToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.WinKeyStart = WinKeySwitch.IsOn;
        Changed();
    }

    private void OnStartSizeChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.StartSize = tag;
        Changed();
    }

    private void OnWinNumbersToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.WinNumbers = WinNumbersSwitch.IsOn;
        Changed();
    }

    private void OnStatusStyleChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.StatusStyle = tag;
        Changed();
    }

    private void OnAutoHideChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.AutoHide = tag;
        Changed();
    }

    private void OnWeChatToggled(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.NotifyWeChat = WeChatSwitch.IsOn;
        Changed();
    }

    private void OnTextColorChecked(object sender, RoutedEventArgs e)
    {
        if (_loading || sender is not RadioButton { Tag: string tag }) return;
        _current.TextColor = tag;
        Changed();
    }

    private void OnNotifyPlacementChecked(object sender, RoutedEventArgs e)
    {
        if (_loading) return;
        _current.NotifyPlacement = (string)((RadioButton)sender).Tag;
        _notifyEntrancePending = true;
        Changed();
    }

    private void Changed()
    {
        UpdateButtons();
        UpdateHero();
        UpdateCustomSwatch();
        if (_page == 3) ShowNotifyPreview(false);
        if (_page == 5) StageRenderGlass(StageCanvas.Width, StageCanvas.Height); // the material changed
        if (_page >= 10) ShowModulePreview(_page);
        SchedulePreview();
    }

    // Title-bar actions appear while there is something to save, and briefly after a save.
    private void UpdateButtons()
    {
        bool dirty = !_current.SameAs(_saved);
        SaveButton.IsEnabled = dirty;
        RevertButton.IsEnabled = dirty;
        _flashId++;
        DirtyText.Text = "未保存";
        DirtyDot.Fill = new SolidColorBrush(Color.FromArgb(255, 0xFF, 0x9F, 0x0A));
        RevertButton.Visibility = SaveButton.Visibility = Visibility.Visible;
        ShowSaveBar(dirty);
    }

    private void ShowSaveBar(bool on)
    {
        SaveBar.OpacityTransition ??= new ScalarTransition { Duration = TimeSpan.FromMilliseconds(160) };
        SaveBar.Opacity = on ? 1 : 0;
        SaveBar.IsHitTestVisible = on;
    }

    private async void FlashSaved()
    {
        int id = ++_flashId;
        DirtyText.Text = "已保存";
        DirtyDot.Fill = new SolidColorBrush(Color.FromArgb(255, 0x30, 0xD1, 0x58));
        RevertButton.Visibility = SaveButton.Visibility = Visibility.Collapsed;
        ShowSaveBar(true);
        await Task.Delay(1600);
        if (id == _flashId) ShowSaveBar(false);
    }

    // At most one preview in flight; the newest state is sent when it returns,
    // so dragging a slider never queues up stale frames.
    private async void SchedulePreview()
    {
        if (!_client.Connected || _updatesClosed) return;
        if (_previewBusy)
        {
            _previewPending = true;
            return;
        }
        _previewBusy = true;
        do
        {
            _previewPending = false;
            await _client.PreviewAsync(_current.Clone());
            await Task.Delay(16);
        } while (_previewPending && !_updatesClosed);
        _previewBusy = false;
        if (!_updatesClosed && !_client.Connected) ShowStatus(false);
    }

    private async void OnSave(object sender, RoutedEventArgs e) => await SaveAsync();

    private async Task<bool> SaveAsync()
    {
        // What is saved is this snapshot: edits made while it is on its way stay unsaved.
        var sent = _current.Clone();
        var error = await _client.CommitAsync(sent.Clone());
        if (_updatesClosed) return false;
        if (error != null)
        {
            ShowMessage(InfoBarSeverity.Error, "保存失败", error);
            return false;
        }
        _saved = sent;
        Status.IsOpen = false;
        UpdateButtons();
        if (_client.LastSaveWarning is { } warning)
            ShowMessage(InfoBarSeverity.Warning, "设置已保存", warning);
        else if (_current.SameAs(_saved)) FlashSaved(); // edits made meanwhile keep the capsule as it is
        ShowStatus(_client.Connected);
        return true;
    }

    private async void OnRevert(object sender, RoutedEventArgs e)
    {
        _current = _saved.Clone();
        Status.IsOpen = false;
        LoadIntoControls();
        UpdateButtons();
        await _client.CancelAsync();
    }

    // A group's "restore defaults": its parameters (and what goes with them) back to the defaults.
    private void OnGroupReset(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement { Tag: string group }) return;
        var d = BarSettings.Liquid();
        foreach (var (def, _) in _rows)
            if (def.Group == group) def.Set(_current, def.Get(d));
        if (group == "clarity") _current.Tint = d.Tint;
        if (group == "width") _current.AutoWidth = d.AutoWidth;
        if (group == "size") _current.HoverRise = d.HoverRise;
        if (group == "notify")
        {
            _current.NotifyHideBody = d.NotifyHideBody;
            _current.NotifyPlacement = d.NotifyPlacement;
        }
        if (group is "clarity" or "edge" or "light")
            _current.Preset = _current.SameMaterial(BarSettings.Liquid()) ? "liquid" : _current.SameMaterial(BarSettings.Frosted()) ? "frosted" : "custom";
        LoadIntoControls();
        Changed();
    }

    // The material back to the Liquid preset (layout, apps and sources stay as they are).
    private void OnDefaults(object sender, RoutedEventArgs e)
    {
        _current.ApplyMaterial(BarSettings.Liquid());
        LoadIntoControls();
        Changed();
    }

    private async void OnClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        args.Cancel = true;
        await CloseRequestedAsync();
    }

    // Window.Close does not raise AppWindow.Closing. Both the title-bar close
    // and update button explicitly use the same save/exit path before calling it.
    private async Task CloseRequestedAsync()
    {
        if (_closePending || _coreChanging) return;
        _closePending = true;
        try
        {
            if (!_current.SameAs(_saved))
            {
                var dialog = new ContentDialog
                {
                    Style = (Style)Application.Current.Resources["YunzhaoDialogStyle"],
                    XamlRoot = Content.XamlRoot,
                    Title = "保存修改？",
                    Content = "未保存的修改在关闭后会被撤销。",
                    PrimaryButtonText = "保存",
                    SecondaryButtonText = "不保存",
                    CloseButtonText = "取消",
                    DefaultButton = ContentDialogButton.Primary,
                };
                var result = await dialog.ShowAsync();
                if (result == ContentDialogResult.None) { _restartUpdate = null; return; }
                if (result == ContentDialogResult.Primary && !await SaveAsync()) { _restartUpdate = null; return; }
            }
            if (_restartUpdate != null)
            {
                try { ReleaseInstaller.Restart(_restartUpdate, _client.Connected); }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
                {
                    _restartUpdate = null;
                    UpdateStatus.Text = error is UnauthorizedAccessException ? "安装目录不可写，更新未安装" : $"无法重启更新：{error.Message}";
                    return;
                }
            }
            _client.Dispose(); // the core reverts any unsaved preview on disconnect
            Close();
        }
        finally { _closePending = false; }
    }
}
