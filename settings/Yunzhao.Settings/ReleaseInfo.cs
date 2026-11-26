using System.Net;
using System.Net.Http;
using System.Net.Security;
using System.Security.Authentication;
using System.Text.Json;

namespace Yunzhao.Settings;

// Official packages are published at yunzhao-desktop.com/releases/latest.json.
internal sealed record ReleaseInfo(Version Version, string Tag, Uri Page, Uri? Download, string? Digest = null, string? AssetUrl = null)
{
    // Login already reaches this host. Prefer a direct connection: a system proxy
    // that points at a closed local port makes HttpClient fail before it leaves.
    // Some networks also reset reused TLS sockets; each attempt uses a fresh client
    // and Connection: close. If the direct route fails on every attempt (a network
    // where this host is only reachable through the system proxy), the proxy route
    // is tried as well — that is what BITS, which downloaded the package before,
    // uses on such machines.
    public static async Task<ReleaseInfo?> FetchLatestAsync(CancellationToken cancel)
    {
        Exception? last = null;
        // Direct first, then whatever the system configures.
        foreach (bool useProxy in new[] { false, true })
        {
            for (int attempt = 0; attempt < 3; attempt++)
            {
                if (attempt > 0)
                    await Task.Delay(400 * attempt, cancel).ConfigureAwait(false);
                try
                {
                    using var handler = new SocketsHttpHandler
                    {
                        UseProxy = useProxy,
                        Proxy = useProxy ? HttpClient.DefaultProxy : null,
                        AutomaticDecompression = DecompressionMethods.All,
                        ConnectTimeout = TimeSpan.FromSeconds(10),
                        PooledConnectionLifetime = TimeSpan.Zero,
                        MaxConnectionsPerServer = 2,
                        SslOptions = new SslClientAuthenticationOptions
                        {
                            // Both TLS 1.2 and 1.3 are offered: pinning 1.2 made some
                            // networks (and one nginx build) reset the handshake.
                            EnabledSslProtocols = SslProtocols.None,
                            CertificateRevocationCheckMode = System.Security.Cryptography.X509Certificates.X509RevocationMode.NoCheck,
                        },
                    };
                    using var http = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(20) };
                    using var request = new HttpRequestMessage(HttpMethod.Get, "https://yunzhao-desktop.com/releases/latest.json");
                    request.Version = HttpVersion.Version11;
                    request.VersionPolicy = HttpVersionPolicy.RequestVersionOrLower;
                    request.Headers.TryAddWithoutValidation("User-Agent", "Yunzhao-Desktop/1.0");
                    request.Headers.TryAddWithoutValidation("Accept", "application/json");
                    request.Headers.ConnectionClose = true;
                    using var response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancel).ConfigureAwait(false);
                    if (response.StatusCode == HttpStatusCode.NotFound) return null;
                    response.EnsureSuccessStatusCode();
                    await using var stream = await response.Content.ReadAsStreamAsync(cancel).ConfigureAwait(false);
                    using var json = await JsonDocument.ParseAsync(stream, cancellationToken: cancel).ConfigureAwait(false);
                    return json.RootElement.ValueKind == JsonValueKind.Object ? FromManifest(json.RootElement) : null;
                }
                catch (Exception error) when (error is HttpRequestException or TaskCanceledException or IOException or UnauthorizedAccessException or AuthenticationException)
                {
                    last = error;
                }
            }
        }
        string detail = last switch
        {
            TaskCanceledException => "连接超时",
            HttpRequestException { InnerException: { } inner } => $"{inner.Message}（{Describe(inner)}）",
            HttpRequestException http => $"{http.Message}（{Describe(http)}）",
            _ => last?.Message ?? "未知错误",
        };
        throw new HttpRequestException("无法连接更新服务器：" + detail, last);
    }

    // The Windows error behind a reset helps tell an unreachable host from a refused
    // handshake; the message alone reads the same for both.
    private static string Describe(Exception error)
    {
        if (error is System.Net.Sockets.SocketException socket)
            return $"socket {socket.SocketErrorCode} / {socket.ErrorCode}";
        if (error is System.Security.Authentication.AuthenticationException auth)
            return $"TLS {auth.HResult:X8}";
        return $"0x{error.HResult:X8}";
    }

    internal static ReleaseInfo? FromManifest(JsonElement manifest)
    {
        // Official packages only: main channel, the versioned zip, and this host.
        if (!manifest.TryGetProperty("channel", out var channel) || channel.GetString() != "main") return null;
        string number = manifest.GetProperty("version").GetString() ?? "";
        if (!Version.TryParse(number, out var version)) throw new JsonException("Invalid release version.");
        version = new Version(version.Major, version.Minor, Math.Max(0, version.Build), Math.Max(0, version.Revision));
        string name = manifest.GetProperty("name").GetString() ?? "";
        if (name != $"Yunzhao-{number}-win-x64.zip") return null;
        string urlText = manifest.GetProperty("url").GetString() ?? "";
        if (!Uri.TryCreate(urlText, UriKind.Absolute, out var download) || !IsPackage(download, name)) return null;
        string tag = manifest.TryGetProperty("tag", out var tagValue) && tagValue.GetString() is { Length: > 0 } given
            ? given : "v" + number;
        string pageText = manifest.TryGetProperty("page", out var pageValue) && pageValue.GetString() is { Length: > 0 } page
            ? page : "https://yunzhao-desktop.com/";
        string? digest = manifest.TryGetProperty("digest", out var hash) && hash.ValueKind == JsonValueKind.String
            ? hash.GetString() : null;
        return new ReleaseInfo(version, tag, new Uri(pageText), download, digest);
    }

    internal Uri ResolveFile()
    {
        if (Download == null) throw new IOException("更新包没有下载地址。");
        string name = Download.Segments.Length == 0 ? "" : Uri.UnescapeDataString(Download.Segments[^1]);
        if (!IsPackage(Download, name)) throw new IOException("更新包地址不在 yunzhao-desktop.com。");
        return Download;
    }

    private static bool IsPackage(Uri url, string name) =>
        url.Scheme == "https"
        && url.Host.Equals("yunzhao-desktop.com", StringComparison.OrdinalIgnoreCase)
        && url.AbsolutePath == "/releases/" + name
        && name.StartsWith("Yunzhao-", StringComparison.Ordinal)
        && name.EndsWith("-win-x64.zip", StringComparison.Ordinal);
}
