using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Atrium.Settings;

public sealed partial class MainWindow
{
    private bool _settingIslandUi;
    private string _islandCategory = "general";
    private JsonDocument? _islandDoc;
    private readonly Dictionary<string, JsonNode> _islandEdits = new();

    private void OnIslandCategoryChecked(object sender, RoutedEventArgs e)
    {
        if (sender is RadioButton { IsChecked: true, Tag: string tag })
        {
            _islandCategory = tag;
            BuildIslandPage();
        }
    }

    private void LoadIslandUi()
    {
        var file = CalendarUiPath("island-ui-state.json");
        _islandEdits.Clear();
        _islandDoc?.Dispose();
        _islandDoc = null;
        if (File.Exists(file))
        {
            try { _islandDoc = JsonDocument.Parse(File.ReadAllText(file)); }
            catch { _islandDoc = null; }
        }
        BuildIslandPage();
    }

    private void SendIslandValue(string key, object value)
    {
        if (_loading || _settingIslandUi) return;
        JsonNode? written = value switch
        {
            bool flag => JsonValue.Create(flag),
            int number => JsonValue.Create(number),
            double number => JsonValue.Create(number),
            string text => JsonValue.Create(text),
            IEnumerable<string> list => new JsonArray(list.Select(item => (JsonNode)JsonValue.Create(item)!).ToArray()),
            _ => JsonValue.Create(value.ToString())
        };
        var file = CalendarUiPath("island-ui.json");
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        if (written != null && key != "translationSecret") _islandEdits[key] = written.DeepClone();
        var node = new JsonObject
        {
            ["at"] = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
            [key] = written
        };
        File.WriteAllText(file, node.ToJsonString());
    }

    private bool IslandBool(string key, bool fallback)
    {
        if (_islandEdits.TryGetValue(key, out var edited) && edited is JsonValue editedBool && editedBool.TryGetValue<bool>(out var flag))
            return flag;
        if (_islandDoc != null && _islandDoc.RootElement.TryGetProperty(key, out var value) &&
            (value.ValueKind == JsonValueKind.True || value.ValueKind == JsonValueKind.False))
            return value.GetBoolean();
        return fallback;
    }

    private double IslandNumber(string key, double fallback)
    {
        if (_islandEdits.TryGetValue(key, out var edited) && edited is JsonValue editedNumber && editedNumber.TryGetValue<double>(out var editedDouble))
            return editedDouble;
        if (_islandDoc != null && _islandDoc.RootElement.TryGetProperty(key, out var value) && value.TryGetDouble(out var number))
            return number;
        return fallback;
    }

    private string IslandText(string key, string fallback)
    {
        if (_islandEdits.TryGetValue(key, out var edited) && edited is JsonValue editedText && editedText.TryGetValue<string>(out var text) && text != null)
            return text;
        if (_islandDoc != null && _islandDoc.RootElement.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String)
            return value.GetString() ?? fallback;
        return fallback;
    }

    private List<string> IslandList(string key, IEnumerable<string> fallback)
    {
        if (_islandEdits.TryGetValue(key, out var edited) && edited is JsonArray editedList)
            return editedList.Select(item => item is JsonValue entry && entry.TryGetValue<string>(out var text) ? text ?? "" : "")
                .Where(item => item.Length > 0).ToList();
        if (_islandDoc != null && _islandDoc.RootElement.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.Array)
            return value.EnumerateArray().Select(item => item.GetString() ?? "").Where(item => item.Length > 0).ToList();
        return fallback.ToList();
    }

    private void BuildIslandPage()
    {
        if (IslandPage == null) return;
        _settingIslandUi = true;
        IslandPage.Children.Clear();
        switch (_islandCategory)
        {
            case "appearance": BuildIslandAppearance(); break;
            case "music": BuildIslandMusic(); break;
            case "translation": BuildIslandTranslation(); break;
            case "messages": BuildIslandMessages(); break;
            case "vault": BuildIslandVault(); break;
            case "about": BuildIslandAbout(); break;
            default: BuildIslandGeneral(); break;
        }
        _settingIslandUi = false;
    }

    private void AddSection(string title) =>
        IslandPage.Children.Add(new TextBlock { Text = title, Style = (Style)Application.Current.Resources["SectionStyle"] });

    private void AddNote(string text) =>
        IslandPage.Children.Add(new TextBlock { Text = text, Style = (Style)Application.Current.Resources["RowNoteStyle"], TextWrapping = TextWrapping.Wrap });

    private void AddToggle(string title, string key, bool fallback)
    {
        var tile = new ToggleTile { Title = title, SwitchName = title, Flat = true, IsOn = IslandBool(key, fallback) };
        tile.Toggled += (_, _) => SendIslandValue(key, tile.IsOn);
        IslandPage.Children.Add(tile);
    }

    private void AddSlider(string title, string key, double min, double max, double step, string unit, double fallback, bool integer)
    {
        var tile = new SliderTile { Flat = true };
        tile.Configure(title, min, max, step, unit);
        tile.SetValue(IslandNumber(key, fallback));
        tile.UserValueChanged += value => SendIslandValue(key, integer ? (int)Math.Round(value) : Math.Round(value, step < 0.1 ? 2 : 1));
        IslandPage.Children.Add(tile);
    }

    private void AddChoice(string title, string key, string fallback, params (string label, string value)[] options)
    {
        var row = new StackPanel { Style = (Style)Application.Current.Resources["FlatRowStyle"] };
        row.Children.Add(new TextBlock { Text = title, Style = (Style)Application.Current.Resources["RowTitleStyle"] });
        var track = new SegmentTrack();
        var current = IslandText(key, fallback);
        foreach (var option in options)
        {
            var button = new RadioButton
            {
                Content = option.label,
                Tag = option.value,
                GroupName = "island-" + key,
                Style = (Style)Application.Current.Resources["SegmentStyle"],
                IsChecked = option.value == current
            };
            button.Checked += (_, _) =>
            {
                if (button.IsChecked == true && button.Tag is string picked) SendIslandValue(key, picked);
            };
            track.Children.Add(button);
        }
        row.Children.Add(track);
        IslandPage.Children.Add(row);
    }

    private void AddField(string title, string key, string fallback, string placeholder)
    {
        var row = new StackPanel { Style = (Style)Application.Current.Resources["FlatRowStyle"] };
        row.Children.Add(new TextBlock { Text = title, Style = (Style)Application.Current.Resources["RowTitleStyle"] });
        var box = new TextBox { Width = 220, Text = IslandText(key, fallback), PlaceholderText = placeholder };
        box.LostFocus += (_, _) => SendIslandValue(key, (box.Text ?? "").Trim());
        row.Children.Add(box);
        IslandPage.Children.Add(row);
    }

    private void AddAction(string title, string label, Action act)
    {
        var row = new StackPanel { Style = (Style)Application.Current.Resources["FlatRowStyle"] };
        row.Children.Add(new TextBlock { Text = title, Style = (Style)Application.Current.Resources["RowTitleStyle"] });
        var button = new Button { Content = label };
        button.Click += (_, _) => act();
        row.Children.Add(button);
        IslandPage.Children.Add(row);
    }

    private void AddOrder(string key, params (string id, string label)[] known)
    {
        var stored = IslandList(key, known.Select(item => item.id));
        var visible = stored.Where(id => known.Any(item => item.id == id)).ToList();
        foreach (var item in known)
            if (!visible.Contains(item.id)) visible.Add(item.id);
        for (int index = 0; index < visible.Count; index++)
        {
            var id = visible[index];
            var label = known.First(item => item.id == id).label;
            int at = index;
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Style = (Style)Application.Current.Resources["FlatRowStyle"] };
            row.Children.Add(new TextBlock { Text = label, Style = (Style)Application.Current.Resources["RowTitleStyle"], VerticalAlignment = VerticalAlignment.Center });
            var up = new Button { Content = "上移", IsEnabled = index > 0 };
            var down = new Button { Content = "下移", IsEnabled = index < visible.Count - 1 };
            up.Click += (_, _) => MoveIslandOrder(key, known, at, -1);
            down.Click += (_, _) => MoveIslandOrder(key, known, at, 1);
            row.Children.Add(up);
            row.Children.Add(down);
            IslandPage.Children.Add(row);
        }
    }

    private void MoveIslandOrder(string key, (string id, string label)[] known, int index, int delta)
    {
        var stored = IslandList(key, known.Select(item => item.id));
        var hidden = stored.Where(id => known.All(item => item.id != id)).ToList();
        var visible = stored.Where(id => known.Any(item => item.id == id)).ToList();
        foreach (var item in known)
            if (!visible.Contains(item.id)) visible.Add(item.id);
        int next = index + delta;
        if (next < 0 || next >= visible.Count) return;
        (visible[index], visible[next]) = (visible[next], visible[index]);
        SendIslandValue(key, hidden.Concat(visible).ToList());
        if (_islandDoc != null && _islandDoc.RootElement.TryGetProperty(key, out _))
        {
            // The page reads the file the island publishes. Show the order just sent.
        }
        BuildIslandPage();
    }

    private void BuildIslandGeneral()
    {
        AddSection("启动与语言");
        AddToggle("开机自启动", "launchAtStartup", false);
        AddChoice("界面语言", "locale", "zh-CN", ("简体中文", "zh-CN"), ("English", "en"));
        AddToggle("独立设置窗口", "detachedSettings", true);
        AddNote("关闭时设置作为岛屿的一页打开。");
        AddSection("展开与收起");
        AddChoice("展开方式", "expandTrigger", "hover", ("点击", "click"), ("鼠标悬停", "hover"));
        AddSlider("展开后自动收起", "autoCollapseSeconds", 0, 600, 1, "秒", 30, true);
        AddNote("鼠标离开展开的岛屿多少秒后收回成小岛；填 0 不自动收起。");
        AddToggle("游戏模式", "gameDoNotDisturb", false);
        AddNote("全屏游戏、视频或演示在前台时，岛屿不悬停展开、不抢焦点、不弹消息。");
        AddSection("贴边隐藏");
        AddToggle("闲置时贴边隐藏", "autoTuck", false);
        AddNote("收起的岛屿一段时间没有操作后，贴到屏幕顶部，只露出一条边。");
        AddSlider("收起后贴边", "tuckDelaySeconds", 3, 600, 1, "秒", 30, true);
        AddNote("收起的小岛闲置多少秒后贴到屏幕顶部。");
        AddChoice("贴边样式", "tuckStyle", "line", ("露边", "edge"), ("细线", "line"), ("隐藏", "hidden"));
        AddSection("这些时候不贴边");
        AddNote("开启后，小岛在这些时候不会自动贴到屏幕顶部，结束后再按设定的时间贴边。");
        AddToggle("Agent 正在运行", "keepVisibleWhileAgentRunning", false);
        AddToggle("小岛在显示歌词", "keepVisibleWhileLyrics", false);
        AddToggle("待办已经开始计时", "keepVisibleWhileTodoRunning", false);
        AddSection("位置");
        AddNote("按住 Ctrl 键，用鼠标左键拖动灵动岛的任意位置即可移动它，展开时也可以；收起时直接拖动也行。拖到屏幕顶部附近会自动贴齐。");
        AddAction("回到默认位置", "复位", () => SendIslandValue("resetPosition", true));
        AddToggle("锁定小岛位置", "positionLocked", false);
        AddNote("开启后无法通过拖动移动小岛，仍可使用复位。");
    }

    private void BuildIslandAppearance()
    {
        AddSection("主题");
        AddChoice("配色", "colorScheme", "system", ("深色", "dark"), ("浅色", "light"), ("跟随系统", "system"));
        AddChoice("质感", "material", "classic", ("经典", "classic"), ("液态玻璃", "liquidGlass"), ("磨砂玻璃", "frostedGlass"));
        AddSlider("玻璃强度", "glassIntensity", 25, 100, 1, "%", 72, true);
        AddSlider("不透明度", "opacity", 50, 100, 1, "%", 100, true);
        AddSection("颜色");
        AddField("岛屿背景", "islandBackgroundColor", "", "#RRGGBB");
        AddField("任务/待办字样", "taskTextColor", "#FFFFFF", "#RRGGBB");
        AddField("亮点颜色", "pulseColor", "", "#RRGGBB");
        AddField("页面文字", "pageTextColor", "", "留空则跟随背景");
        AddAction("页面文字", "跟随背景", () => SendIslandValue("pageTextColor", ""));
        AddSection("字体与大小");
        AddChoice("字体", "fontFamily", "system", ("默认字体", "system"), ("像素字体", "pixel"));
        AddSlider("岛屿字号", "islandFontPercent", 90, 120, 5, "%", 100, true);
        AddSlider("收起时的大小", "collapsedScale", 0.75, 1.4, 0.01, "x", 1, false);
        AddSlider("展开后的大小", "expandedScale", 0.85, 1.2, 0.01, "x", 1, false);
        AddSection("岛上的排列");
        AddNote("放在文字左边的会显示在岛的左侧。灰色的是还没打开的功能，位置会保留。");
        AddOrder("collapsedLayout", ("text", "文字"), ("mail", "新邮件"), ("shelf", "置物架文件数"), ("music", "音乐波形"), ("agents", "Agent 会话数"));
        AddChoice("页面按钮", "pageButtonStyle", "icon", ("图标", "icon"), ("胶囊", "capsule"));
        AddSection("岛屿展示优先级");
        AddNote("几样同时有内容时，收起的岛显示排在最前面的那一样。");
        AddOrder("collapsedPriority", ("lyrics", "歌词"), ("todo", "待办"));
        AddToggle("显示歌曲封面", "collapsedShowCover", false);
        AddNote("收起的岛显示歌词时，在歌词前显示这首歌的封面。");
        AddSection("刘海与外形");
        AddNote("宽高会随收起时的整体大小一起缩放。");
        AddToggle("自动宽度", "collapsedAutoWidth", true);
        AddSlider("刘海宽度", "collapsedWidth", 240, 480, 1, "px", 340, true);
        AddSlider("刘海高度", "collapsedHeight", 32, 60, 1, "px", 38, true);
        AddToggle("贴边圆角", "topEdgeShoulders", false);
        AddSlider("圆角大小", "topEdgeShoulderSize", 4, 20, 1, "", 10, true);
        AddSection("外观预设");
        AddNote("把当前所有外观设置（材质、颜色、大小、字体、圆角等）存成一份，之后一键应用，应用后还能继续调整。");
        AddPresetRows();
    }

    private void AddPresetRows()
    {
        if (_islandDoc != null && _islandDoc.RootElement.TryGetProperty("appearancePresets", out var presets) && presets.ValueKind == JsonValueKind.Array && presets.GetArrayLength() > 0)
        {
            foreach (var preset in presets.EnumerateArray())
            {
                var id = preset.TryGetProperty("id", out var idValue) ? idValue.GetString() ?? "" : "";
                var name = preset.TryGetProperty("name", out var nameValue) ? nameValue.GetString() ?? "" : "";
                if (id.Length == 0 || name.Length == 0) continue;
                var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Style = (Style)Application.Current.Resources["FlatRowStyle"] };
                row.Children.Add(new TextBlock { Text = name, Style = (Style)Application.Current.Resources["RowTitleStyle"], VerticalAlignment = VerticalAlignment.Center });
                var apply = new Button { Content = "应用" };
                var remove = new Button { Content = "删除" };
                var presetId = id;
                apply.Click += (_, _) => SendIslandValue("applyAppearancePreset", presetId);
                remove.Click += (_, _) => SendIslandValue("deleteAppearancePreset", presetId);
                row.Children.Add(apply);
                row.Children.Add(remove);
                IslandPage.Children.Add(row);
            }
        }
        else AddNote("还没有保存的外观预设。");
        var entry = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Style = (Style)Application.Current.Resources["FlatRowStyle"] };
        var box = new TextBox { Width = 180, PlaceholderText = "例如：深色玻璃" };
        var save = new Button { Content = "保存" };
        save.Click += (_, _) =>
        {
            var name = (box.Text ?? "").Trim();
            if (name.Length > 0) SendIslandValue("saveAppearancePreset", name);
        };
        entry.Children.Add(box);
        entry.Children.Add(save);
        IslandPage.Children.Add(entry);
    }

    private void BuildIslandMusic()
    {
        AddSection("音乐");
        AddToggle("启用音乐控制", "musicEnabled", true);
        AddNote("关闭后，岛上不再显示这个功能的入口，后台也停止运行；已有数据会保留。");
        AddNote("几个播放器同时在播时，灵动岛按已经记住的播放器顺序决定跟哪一个。播放器要先被发现，才会出现在原来的列表里。");
        AddSection("歌词");
        AddNote("找到带时间轴的歌词时，音乐页会跟着播放滚动显示。某首歌的歌词对不上时，可以在音乐页上单独调它的快慢。");
        AddToggle("在线查找歌词", "lyricsOnline", false);
        AddNote("查 LRCLIB，会把歌名、歌手、专辑和时长发过去。每首歌只查一次，结果存在本机。");
        AddToggle("也查 QQ 音乐、酷狗和网易云", "lyricsChinese", false);
        AddToggle("逐字歌词填充", "lyricWordFill", false);
    }

    private void BuildIslandTranslation()
    {
        AddSection("翻译");
        AddToggle("启用快捷翻译", "translationEnabled", false);
        AddNote("关闭后，岛上不再显示这个功能的入口，后台也停止运行；已有数据会保留。");
        AddNote("全局快捷键暂时全部停用，避免和其他软件冲突。");
        AddSection("翻译服务");
        AddChoice("翻译服务", "translationProvider", "siliconflow",
            ("DeepSeek", "deepseek"), ("火山方舟", "volcengine"), ("通义千问", "dashscope"), ("Kimi", "moonshot"),
            ("硅基流动", "siliconflow"), ("OpenAI", "openai"), ("自定义（OpenAI 兼容）", "custom"));
        AddField("接口地址", "translationBaseUrl", "", "https://example.com/v1");
        AddField("模型", "translationModel", "", "留空使用服务默认模型");
        AddNote("API 密钥只保存在本机 Windows 凭据管理器。在这里输入会交给灵动岛保存，不会留在设置文件里当展示值。");
        AddField("API 密钥", "translationSecret", "", "输入 API 密钥");
        AddSection("快捷键");
        AddField("全局快捷键", "translationShortcut", "", "暂时停用");
        AddNote("写成 Control、Shift、Alt 加一个键，例如 Ctrl+Shift+F。当前版本不会注册全局快捷键。");
    }

    private void BuildIslandMessages()
    {
        AddSection("岛上通知");
        AddSlider("停留时长", "messagePreviewSeconds", 3, 30, 1, "秒", 5, true);
        AddSection("QQ 通知");
        AddNote("通过 Windows 通知读取 QQ 消息的标题和预览，只保留在本次运行的内存中。需要 QQ 已登录并开启系统通知。");
        AddToggle("接收 QQ 消息", "qqEnabled", false);
        AddSection("微信提醒");
        AddNote("只监听电脑版微信在任务栏的闪烁提醒。需要微信保持登录。");
        AddToggle("接收微信提醒", "wechatEnabled", false);
        AddToggle("显示联系人和消息内容", "wechatPreview", false);
        AddToggle("最小化时隐藏微信任务栏图标", "wechatHideTaskbar", false);
        AddSection("企业微信提醒");
        AddNote("只监听企业微信在任务栏的闪烁提醒。需要企业微信保持登录。");
        AddToggle("接收企业微信提醒", "wecomEnabled", false);
        AddSection("邮箱");
        AddToggle("启用邮箱", "mailEnabled", true);
        AddNote("关闭后，岛上不再显示这个功能的入口，后台也停止运行；已有数据会保留。");
        AddToggle("自动复制验证码", "autoCopyOtp", true);
        AddNote("邮箱账号的添加和重新授权仍在灵动岛的消息页完成，那里要填写授权码。");
    }

    private void BuildIslandVault()
    {
        AddSection("账号");
        AddNote("查看密码在电脑保持解锁时会记住。电脑锁屏再解锁后，需要重新输入。密码只保存在这台电脑上，忘记后无法恢复。");
        AddNote("设置查看密码、添加账号和解锁账号表仍在灵动岛的账号页完成。这些操作要输入本机密码，不经过设置文件。");
        AddNote("还没有账号时，原来的页面会显示设置查看密码和添加账号。");
    }

    private void BuildIslandAbout()
    {
        AddSection("关于小岛");
        AddNote("在屏幕顶部集中待办、剪贴板、音乐、快捷翻译和消息提醒。");
        AddSection("更新");
        AddToggle("自动更新", "autoUpdate", true);
        AddNote("每 3 小时检查一次，后台下载；小岛收起且 1 分钟没人碰时自动安装并重启。关闭后只在这里手动检查。");
        AddAction("检查更新", "检查更新", () => SendIslandValue("checkUpdate", true));
        AddSection("退出");
        AddNote("和托盘菜单里的「退出」一样；要再打开，从开始菜单或桌面快捷方式启动。");
        AddAction("退出岛屿", "退出", () => SendIslandValue("quitIsland", true));
    }
}
