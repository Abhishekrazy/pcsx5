using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace Pcsx5Ui
{
    /// <summary>
    /// When each title was last launched from the shell, so the library shelf
    /// can show recently played games first (asked 2026-09-06).
    /// </summary>
    /// <remarks>
    /// Shell-only state, so it lives in its own file beside <c>config.ini</c>
    /// rather than in <c>global.json</c>, which the native core also reads and
    /// whose schema is versioned. A missing or unreadable file means nothing has
    /// been played; a failed save is logged by the caller and never fatal.
    /// </remarks>
    public sealed class RecentPlays
    {
        private readonly string _path;
        private readonly Dictionary<string, DateTime> _lastPlayed = new(StringComparer.OrdinalIgnoreCase);

        public RecentPlays(string path)
        {
            _path = path;
            Load();
        }

        /// <summary>UTC time the title was last launched, or null if never.</summary>
        public DateTime? LastPlayed(string titleId)
        {
            if (string.IsNullOrEmpty(titleId)) return null;
            return _lastPlayed.TryGetValue(titleId, out var t) ? t : null;
        }

        /// <summary>Stamp the title as launched now and persist. Returns false
        /// (after recording in memory) if the file could not be written.</summary>
        public bool Record(string titleId)
        {
            if (string.IsNullOrEmpty(titleId)) return false;
            _lastPlayed[titleId] = DateTime.UtcNow;
            return Save();
        }

        private void Load()
        {
            try
            {
                if (!File.Exists(_path)) return;
                var map = JsonSerializer.Deserialize<Dictionary<string, DateTime>>(File.ReadAllText(_path));
                if (map == null) return;
                foreach (var kv in map) _lastPlayed[kv.Key] = kv.Value;
            }
            catch
            {
                // Treated as "nothing played yet"; the next Record() rewrites the file.
            }
        }

        private bool Save()
        {
            try
            {
                string dir = Path.GetDirectoryName(_path);
                if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir)) Directory.CreateDirectory(dir);
                File.WriteAllText(_path, JsonSerializer.Serialize(_lastPlayed, new JsonSerializerOptions { WriteIndented = true }));
                return true;
            }
            catch
            {
                return false;
            }
        }
    }
}
