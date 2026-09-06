using System;
using System.Linq;
using System.Net.Http;
using System.Text.Json;
using System.Threading.Tasks;

namespace Pcsx5Ui
{
    /// <summary>What a release check found. Presentation is the window's job;
    /// this class only knows versions and where the release lives (Rule 11).</summary>
    internal sealed class UpdateCheckResult
    {
        public string CurrentVersion = "";
        public string LatestVersion = "";
        public bool IsNewer;
        /// <summary>True when this copy was installed through Squirrel, so the
        /// update can be downloaded and applied in place. A copy run from the
        /// zip can only be pointed at the release page.</summary>
        public bool CanInstallInPlace;
        public string ReleaseUrl = Repo + "/releases/latest";
        public string Error;

        public const string Repo = "https://github.com/Abhishekrazy/pcsx5";
    }

    /// <summary>
    /// Release checks against the GitHub repository: through Squirrel when the
    /// app was installed by it (RELEASES file), otherwise through the GitHub
    /// releases API so a zip user still hears about a new build. Every failure
    /// is returned in <see cref="UpdateCheckResult.Error"/>, never thrown.
    /// </summary>
    internal static class UpdateChecker
    {
        private static readonly HttpClient _http = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };

        static UpdateChecker()
        {
            _http.DefaultRequestHeaders.UserAgent.ParseAdd("PCSX5-shell");
            _http.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        }

        public static async Task<UpdateCheckResult> CheckAsync(string currentVersion)
        {
            var r = new UpdateCheckResult { CurrentVersion = Normalize(currentVersion) };
            // Squirrel first: it knows the installed version precisely and can apply.
            try
            {
                using var mgr = new Squirrel.UpdateManager(UpdateCheckResult.Repo);
                if (mgr.IsInstalledApp)
                {
                    r.CanInstallInPlace = true;
                    var installed = mgr.CurrentlyInstalledVersion();
                    if (installed != null) r.CurrentVersion = installed.ToString();
                    var info = await mgr.CheckForUpdate();
                    var future = info?.FutureReleaseEntry?.Version;
                    if (future != null)
                    {
                        r.LatestVersion = future.ToString();
                        r.IsNewer = info.ReleasesToApply.Any();
                        r.ReleaseUrl = UpdateCheckResult.Repo + "/releases/tag/v" + r.LatestVersion;
                        return r;
                    }
                }
            }
            catch (Exception ex) { r.Error = ex.Message; }

            // Zip copy (or Squirrel could not answer): ask the releases API.
            try
            {
                string json = await _http.GetStringAsync("https://api.github.com/repos/Abhishekrazy/pcsx5/releases/latest");
                using var doc = JsonDocument.Parse(json);
                string tag = doc.RootElement.TryGetProperty("tag_name", out var t) ? t.GetString() : null;
                if (doc.RootElement.TryGetProperty("html_url", out var u) && u.GetString() is string url) r.ReleaseUrl = url;
                r.LatestVersion = Normalize(tag);
                r.IsNewer = Compare(r.LatestVersion, r.CurrentVersion) > 0;
                r.Error = null;
            }
            catch (Exception ex) { if (r.Error == null) r.Error = ex.Message; }
            return r;
        }

        /// <summary>Download and apply the pending Squirrel releases. Progress is
        /// 0-100 across download then apply. Returns false when nothing applied.</summary>
        public static async Task<bool> InstallAsync(IProgress<int> progress)
        {
            using var mgr = new Squirrel.UpdateManager(UpdateCheckResult.Repo);
            if (!mgr.IsInstalledApp) return false;
            var info = await mgr.CheckForUpdate();
            if (info == null || !info.ReleasesToApply.Any()) return false;
            await mgr.DownloadReleases(info.ReleasesToApply, p => progress?.Report(p / 2));
            await mgr.ApplyReleases(info, p => progress?.Report(50 + p / 2));
            return true;
        }

        public static void RestartApp() => Squirrel.UpdateManager.RestartApp();

        /// <summary>"v0.1.1", "0.1.1-dev+abc" -> "0.1.1".</summary>
        public static string Normalize(string v)
        {
            if (string.IsNullOrWhiteSpace(v)) return "";
            v = v.Trim();
            if (v.StartsWith("v", StringComparison.OrdinalIgnoreCase)) v = v.Substring(1);
            int cut = v.IndexOfAny(new[] { '-', '+', ' ' });
            return cut > 0 ? v.Substring(0, cut) : v;
        }

        public static int Compare(string a, string b)
        {
            Version.TryParse(Pad(a), out var va);
            Version.TryParse(Pad(b), out var vb);
            if (va == null || vb == null) return string.CompareOrdinal(a, b);
            return va.CompareTo(vb);
        }

        private static string Pad(string v) => v.Count(c => c == '.') == 0 ? v + ".0" : v;
    }
}
