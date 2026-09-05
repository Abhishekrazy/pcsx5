using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace Pcsx5Ui
{
    /// <summary>
    /// The user's favourite titles, by title ID, persisted in favourites.json
    /// beside config.ini (shell-only state, like RecentPlays). A missing or
    /// unreadable file means no favourites; a failed save is reported to the
    /// caller and never fatal.
    /// </summary>
    public sealed class Favourites
    {
        private readonly string _path;
        private readonly HashSet<string> _ids = new(StringComparer.OrdinalIgnoreCase);

        public Favourites(string path)
        {
            _path = path;
            try
            {
                if (File.Exists(_path))
                {
                    var list = JsonSerializer.Deserialize<List<string>>(File.ReadAllText(_path));
                    if (list != null) foreach (var id in list) if (!string.IsNullOrEmpty(id)) _ids.Add(id);
                }
            }
            catch { }
        }

        public bool Contains(string titleId) => !string.IsNullOrEmpty(titleId) && _ids.Contains(titleId);

        /// <summary>Flip the flag and persist. Returns the new state; sets
        /// <paramref name="saved"/> false if the file could not be written.</summary>
        public bool Toggle(string titleId, out bool saved)
        {
            saved = true;
            if (string.IsNullOrEmpty(titleId)) return false;
            bool now = !_ids.Remove(titleId);
            if (now) _ids.Add(titleId);
            try
            {
                string dir = Path.GetDirectoryName(_path);
                if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir)) Directory.CreateDirectory(dir);
                File.WriteAllText(_path, JsonSerializer.Serialize(new List<string>(_ids), new JsonSerializerOptions { WriteIndented = true }));
            }
            catch { saved = false; }
            return now;
        }
    }
}
