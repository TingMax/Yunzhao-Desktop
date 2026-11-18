using System.Runtime.InteropServices;
using System.Text.Json;

namespace Atrium.Settings;

// The desktop login is the account for the calendar and the table.
// Settings reads the same sealed session and never keeps the token.
internal sealed class DesktopAccount
{
    public string Id { get; init; } = "";
    public string Name { get; init; } = "";
    public string Username { get; init; } = "";
    public string Email { get; init; } = "";

    public static string SessionPath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Yunzhao", "session.bin");

    public static DesktopAccount? Read()
    {
        try
        {
            if (!File.Exists(SessionPath)) return null;
            var plain = Unprotect(File.ReadAllBytes(SessionPath));
            if (string.IsNullOrWhiteSpace(plain)) return null;
            using var doc = JsonDocument.Parse(plain);
            var root = doc.RootElement;
            string Text(string key) =>
                root.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.String
                    ? value.GetString() ?? ""
                    : "";
            var id = Text("id");
            if (id.Length < 32 || id.Contains('\\') || id.Contains('/')) return null;
            var username = Text("username");
            var name = Text("name");
            if (string.IsNullOrWhiteSpace(name)) name = username;
            return new DesktopAccount { Id = id, Name = name, Username = username, Email = Text("email") };
        }
        catch
        {
            return null;
        }
    }

    public void RequestSignOut()
    {
        var dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Yunzhao", "users", Id);
        Directory.CreateDirectory(dir);
        File.WriteAllText(Path.Combine(dir, "sign-out.request"), "1");
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Blob
    {
        public int cbData;
        public IntPtr pbData;
    }

    [DllImport("crypt32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool CryptUnprotectData(
        ref Blob input, IntPtr description, IntPtr entropy, IntPtr reserved, IntPtr prompt, int flags, ref Blob output);

    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr handle);

    private static string Unprotect(byte[] data)
    {
        var input = new Blob { cbData = data.Length, pbData = Marshal.AllocHGlobal(data.Length) };
        Marshal.Copy(data, 0, input.pbData, data.Length);
        var output = new Blob();
        try
        {
            if (!CryptUnprotectData(ref input, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, 1, ref output))
                return "";
            var plain = new byte[output.cbData];
            Marshal.Copy(output.pbData, plain, 0, output.cbData);
            return System.Text.Encoding.UTF8.GetString(plain);
        }
        finally
        {
            Marshal.FreeHGlobal(input.pbData);
            if (output.pbData != IntPtr.Zero) LocalFree(output.pbData);
        }
    }
}
