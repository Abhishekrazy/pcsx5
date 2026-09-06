using System;
using System.Collections.Generic;
using System.Net.Http;
using System.Text.Json;
using System.Threading.Tasks;

namespace Pcsx5Ui
{
    /// <summary>
    /// Reads a title's community compatibility status from the public database
    /// (github.com/Abhishekrazy/Pcsx5-Game-Compatibility), which tracks one
    /// GitHub issue per title with a mutually-exclusive status label. Read-only
    /// and unauthenticated (public repo); every failure is swallowed and returns
    /// null so a missing network or a rate limit never disturbs the shell.
    /// </summary>
    public static class CompatDatabase
    {
        public const string Repo = "Abhishekrazy/Pcsx5-Game-Compatibility";

        private static readonly HttpClient _http = CreateClient();

        private static HttpClient CreateClient()
        {
            var c = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };
            // GitHub requires a User-Agent; the Accept header selects the v3 API.
            c.DefaultRequestHeaders.UserAgent.ParseAdd("PCSX5-Shell");
            c.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
            return c;
        }

        // The database's status label -> the emulator's badge word.
        private static readonly Dictionary<string, string> LabelToStatus = new(StringComparer.OrdinalIgnoreCase)
        {
            ["status-playable"] = "PLAYABLE",
            ["status-ingame"]   = "INGAME",
            ["status-menus"]    = "MENUS",
            ["status-boots"]    = "BOOTS",
            ["status-nothing"]  = "NOTHING",
        };

        /// <summary>The status word for a title from the database, or null when
        /// the title is not listed, the request failed, or it carries no status
        /// label. Uppercase, matching the emulator's badge vocabulary.</summary>
        public static async Task<string> FetchStatusAsync(string titleId)
        {
            if (string.IsNullOrWhiteSpace(titleId)) return null;
            try
            {
                string q = Uri.EscapeDataString($"repo:{Repo} {titleId} in:title type:issue");
                string url = $"https://api.github.com/search/issues?q={q}&per_page=10";
                using var resp = await _http.GetAsync(url).ConfigureAwait(false);
                if (!resp.IsSuccessStatusCode) return null;
                using var doc = JsonDocument.Parse(await resp.Content.ReadAsStringAsync().ConfigureAwait(false));
                if (!doc.RootElement.TryGetProperty("items", out var items)) return null;
                foreach (var item in items.EnumerateArray())
                {
                    string title = item.TryGetProperty("title", out var t) ? t.GetString() ?? "" : "";
                    if (title.IndexOf(titleId, StringComparison.OrdinalIgnoreCase) < 0) continue;
                    if (!item.TryGetProperty("labels", out var labels)) continue;
                    foreach (var lbl in labels.EnumerateArray())
                    {
                        string name = lbl.TryGetProperty("name", out var n) ? n.GetString() ?? "" : "";
                        if (LabelToStatus.TryGetValue(name, out var status)) return status;
                    }
                }
            }
            catch
            {
                // Offline, rate-limited, or a shape change: keep the local status.
            }
            return null;
        }
    }
}
