using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text.Json;

namespace Atrium.Settings;

// The public latest-release endpoint excludes drafts and prereleases.
internal sealed record ReleaseInfo(Version Version, string Tag, Uri Page, Uri? Download, string? Digest = null)
{
    internal static readonly HttpClient Http = new()
    {
        Timeout = TimeSpan.FromSeconds(15),
        DefaultRequestHeaders =
        {
            UserAgent = { new ProductInfoHeaderValue("Yunzhao-Desktop", "1.0") },
            Accept = { new MediaTypeWithQualityHeaderValue("application/vnd.github+json") },
        },
    };

    public static async Task<ReleaseInfo?> FetchLatestAsync(CancellationToken cancel)
    {
        using var response = await Http.GetAsync("https://api.github.com/repos/TingMax/Yunzhao-Desktop/releases?per_page=20", cancel);
        if (response.StatusCode == HttpStatusCode.NotFound) return null;
        response.EnsureSuccessStatusCode();
        using var stream = await response.Content.ReadAsStreamAsync(cancel);
        using var json = await JsonDocument.ParseAsync(stream, cancellationToken: cancel);
        if (json.RootElement.ValueKind != JsonValueKind.Array) return null;
        foreach (var release in json.RootElement.EnumerateArray())
        {
            if (!release.TryGetProperty("target_commitish", out var target) || target.GetString() != "main") continue;
            if (release.TryGetProperty("draft", out var draft) && draft.GetBoolean()) continue;
            if (release.TryGetProperty("prerelease", out var pre) && pre.GetBoolean()) continue;
            return FromJson(release);
        }
        return null;
    }

    internal static ReleaseInfo FromJson(JsonElement release)
    {
        string tag = release.GetProperty("tag_name").GetString()!;
        string number = tag.TrimStart('v', 'V').Split('+')[0];
        if (!Version.TryParse(number, out var version)) throw new JsonException("Invalid release version.");
        // Assembly versions have four components; v1.0.0 must equal 1.0.0.0.
        version = new Version(version.Major, version.Minor, Math.Max(0, version.Build), Math.Max(0, version.Revision));
        Uri? download = null;
        string? digest = null;
        foreach (var asset in release.GetProperty("assets").EnumerateArray())
            if (asset.GetProperty("name").GetString() == $"Yunzhao-{number}-win-x64.zip")
            {
                download = new Uri(asset.GetProperty("browser_download_url").GetString()!);
                digest = asset.TryGetProperty("digest", out var hash) ? hash.GetString() : null;
                break;
            }
        return new ReleaseInfo(version, tag, new Uri(release.GetProperty("html_url").GetString()!), download, digest);
    }
}
