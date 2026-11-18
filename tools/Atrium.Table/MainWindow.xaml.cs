using System.Runtime.InteropServices;
using System.Net.Http;
using System.Windows;
using System.Windows.Interop;

namespace Atrium.Table;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        SourceInitialized += (_, _) => PlaceOnMonitor();
        Loaded += async (_, _) =>
        {
            PlaceOnMonitor();
            await OpenTable();
        };
    }

    bool TryMonitor(out int left, out int top, out int right, out int bottom)
    {
        left = top = right = bottom = 0;
        foreach (string arg in Environment.GetCommandLineArgs())
        {
            const string key = "--bounds=";
            if (!arg.StartsWith(key, StringComparison.OrdinalIgnoreCase))
                continue;
            string[] parts = arg[key.Length..].Split(',');
            if (parts.Length == 4
                && int.TryParse(parts[0], out left)
                && int.TryParse(parts[1], out top)
                && int.TryParse(parts[2], out right)
                && int.TryParse(parts[3], out bottom)
                && right > left
                && bottom > top)
                return true;
        }
        return false;
    }

    void PlaceOnMonitor()
    {
        if (!TryMonitor(out int left, out int top, out int right, out int bottom))
            return;
        int monitorW = right - left;
        int monitorH = bottom - top;
        int width = Math.Clamp(monitorW - 160, 880, 1400);
        int height = Math.Clamp(monitorH - 120, 560, 900);
        int x = left + (monitorW - width) / 2;
        int y = top + (monitorH - height) / 2;
        IntPtr hwnd = new WindowInteropHelper(this).Handle;
        if (hwnd == IntPtr.Zero)
            return;
        SetWindowPos(hwnd, IntPtr.Zero, x, y, width, height, 0x0040);
    }

    async Task OpenTable()
    {
        using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(2) };
        for (int i = 0; i < 90; i++)
        {
            try
            {
                using HttpResponseMessage response = await http.GetAsync("http://127.0.0.1:3000/");
                if (response.IsSuccessStatusCode)
                    break;
            }
            catch
            {
                // The local table service is still starting.
            }
            await Task.Delay(1000);
        }
        try
        {
            await View.EnsureCoreWebView2Async();
            View.CoreWebView2.Settings.AreDefaultContextMenusEnabled = true;
            View.Source = new Uri("http://127.0.0.1:3000/");
        }
        catch (Exception ex)
        {
            MessageBox.Show(ex.Message, "Yunzhao_table");
        }
    }

    [DllImport("user32.dll", SetLastError = true)]
    static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int x, int y, int cx, int cy, uint flags);
}
