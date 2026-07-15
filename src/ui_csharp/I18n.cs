using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;

namespace Pcsx5Ui
{
    public static class I18n
    {
        private static Dictionary<string, string> _translations = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        private static string _currentLanguage = "en-US";

        public static string CurrentLanguage => _currentLanguage;

        public static void Load(string language)
        {
            if (string.IsNullOrEmpty(language)) language = "en-US";
            _currentLanguage = language;
            _translations.Clear();

            try
            {
                string appDir = AppDomain.CurrentDomain.BaseDirectory;
                string langDir = Path.Combine(appDir, "assets", "lang");

                // Search up to 5 levels to locate the assets folder
                if (!Directory.Exists(langDir))
                {
                    string temp = appDir;
                    for (int i = 0; i < 5; i++)
                    {
                        if (temp == null) break;
                        string testDir = Path.Combine(temp, "assets", "lang");
                        if (Directory.Exists(testDir))
                        {
                            langDir = testDir;
                            break;
                        }
                        temp = Path.GetDirectoryName(temp);
                    }
                }

                string filePath = Path.Combine(langDir, $"{language}.json");
                if (!File.Exists(filePath))
                {
                    filePath = Path.Combine(langDir, "en-US.json");
                }

                if (File.Exists(filePath))
                {
                    string json = File.ReadAllText(filePath, Encoding.UTF8);
                    var dict = JsonSerializer.Deserialize<Dictionary<string, string>>(json);
                    if (dict != null)
                    {
                        foreach (var kvp in dict)
                        {
                            _translations[kvp.Key] = kvp.Value;
                        }
                    }
                }
            }
            catch
            {
                // Fallback to empty if load fails
            }
        }

        public static string Tr(string key, params object[] args)
        {
            if (_translations.TryGetValue(key, out string val))
            {
                if (args != null && args.Length > 0)
                {
                    try
                    {
                        string formattedVal = ConvertFormatSpecifiers(val);
                        return string.Format(formattedVal, args);
                    }
                    catch
                    {
                        return val;
                    }
                }
                return val;
            }
            return key;
        }

        private static string ConvertFormatSpecifiers(string val)
        {
            int index = 0;
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < val.Length; i++)
            {
                if (val[i] == '%' && i + 1 < val.Length)
                {
                    char type = val[i + 1];
                    if (type == 's' || type == 'd')
                    {
                        sb.Append("{" + index + "}");
                        index++;
                        i++;
                    }
                    else if (type == '.' && i + 3 < val.Length && char.IsDigit(val[i + 2]) && val[i + 3] == 'f')
                    {
                        sb.Append("{" + index + ":F" + val[i + 2] + "}");
                        index++;
                        i += 3;
                    }
                    else if (type == 'f')
                    {
                        sb.Append("{" + index + "}");
                        index++;
                        i++;
                    }
                    else
                    {
                        sb.Append('%');
                    }
                }
                else
                {
                    sb.Append(val[i]);
                }
            }
            return sb.ToString();
        }
    }
}
