using System;
using System.Globalization;
using System.Windows;
using System.Windows.Media;
using Microsoft.Win32;

namespace Pcsx5Ui
{
    /// <summary>
    /// The shell's theme: one token palette (ADR-003) written into
    /// Application.Resources so every DynamicResource reference follows it
    /// live. Owns the two built-in palettes, the system-mode lookup and the
    /// user's accent and ground colours. Nothing else writes these resources.
    /// </summary>
    public static class Theme
    {
        public const string ModeDark = "dark";
        public const string ModeLight = "light";
        public const string ModeSystem = "system";

        // Token names; every new screen references these with DynamicResource.
        public static readonly string[] Tokens =
        {
            "ThemeGround", "ThemeSurface", "ThemeRaised", "ThemeHairline",
            "ThemeText", "ThemeTextMuted", "ThemeAccent", "ThemeAccentSoft",
            "ThemeOnAccent", "ThemeDanger", "ThemeWarning", "ThemeSuccess",
        };

        /// <summary>Curated accents offered as pad-cyclable swatches. The first
        /// is the concept's cyan; the rest keep similar chroma and lightness so
        /// contrast on either ground holds.</summary>
        public static readonly (string Name, string Hex)[] AccentSwatches =
        {
            ("Cyan", "#5FE3FF"), ("Mint", "#6FF0C3"), ("Lime", "#B8F26A"),
            ("Amber", "#F5C56B"), ("Coral", "#FF8A80"), ("Rose", "#FF7EB6"),
            ("Violet", "#B69CFF"), ("Blue", "#7FB4FF"), ("White", "#F2F4F8"),
        };

        private struct Palette
        {
            public string Ground, Surface, Raised, Hairline, Text, TextMuted, Danger, Warning, Success;
        }

        // Dark: the concept's "Void console" values.
        private static readonly Palette Dark = new Palette
        {
            Ground = "#0B0D12", Surface = "#0E1118", Raised = "#11151D", Hairline = "#1A1F2A",
            Text = "#E6E9F0", TextMuted = "#8B91A0", Danger = "#FF6F7D", Warning = "#F0C96F", Success = "#6FF0C3",
        };

        // Light: the same structure inverted, subtly cool. Text #1B1F27 on
        // #F5F6F8 is 15:1; muted #5B6373 on it is 5.6:1 (AA for body text).
        private static readonly Palette Light = new Palette
        {
            Ground = "#F5F6F8", Surface = "#FFFFFF", Raised = "#EEF0F4", Hairline = "#DDE1E8",
            Text = "#1B1F27", TextMuted = "#5B6373", Danger = "#C4262E", Warning = "#8A6100", Success = "#0B7A55",
        };

        /// <summary>The mode currently painted, after resolving "system".</summary>
        public static string EffectiveMode { get; private set; } = ModeDark;

        /// <summary>Apply a mode ("dark", "light", "system"), an accent (#RRGGBB)
        /// and an optional ground override (#RRGGBB or empty). Invalid colours
        /// fall back to the palette's own; the caller is told through the
        /// return value so Settings can show the rejection.</summary>
        public static bool Apply(string mode, string accentHex, string groundHex)
        {
            bool ok = true;
            EffectiveMode = ResolveMode(mode);
            Palette p = EffectiveMode == ModeLight ? Light : Dark;

            if (!TryParse(accentHex, out Color accent)) { accent = ParseOrThrow(AccentSwatches[0].Hex); ok = string.IsNullOrEmpty(accentHex); }
            Color ground = ParseOrThrow(p.Ground);
            if (!string.IsNullOrEmpty(groundHex))
            {
                if (TryParse(groundHex, out var g)) ground = g; else ok = false;
            }

            var res = Application.Current.Resources;
            Set(res, "ThemeGround", ground);
            Set(res, "ThemeSurface", ParseOrThrow(p.Surface));
            Set(res, "ThemeRaised", ParseOrThrow(p.Raised));
            Set(res, "ThemeHairline", ParseOrThrow(p.Hairline));
            Set(res, "ThemeText", ParseOrThrow(p.Text));
            Set(res, "ThemeTextMuted", ParseOrThrow(p.TextMuted));
            Set(res, "ThemeAccent", accent);
            Set(res, "ThemeAccentSoft", Color.FromArgb(0x2E, accent.R, accent.G, accent.B));
            Set(res, "ThemeOnAccent", Luminance(accent) > 0.4 ? ParseOrThrow("#0B0D12") : ParseOrThrow("#FFFFFF"));
            Set(res, "ThemeDanger", ParseOrThrow(p.Danger));
            Set(res, "ThemeWarning", ParseOrThrow(p.Warning));
            Set(res, "ThemeSuccess", ParseOrThrow(p.Success));

            // The older focus-ring and picker brushes follow the accent too, so
            // screens not yet rebuilt still show the user's colour where it
            // matters most: the focus ring.
            Set(res, "PickerAccentBrush", accent);
            return ok;
        }

        /// <summary>"system" follows Windows' apps-theme setting; a missing key
        /// means dark, matching the console default.</summary>
        public static string ResolveMode(string mode)
        {
            if (mode == ModeLight) return ModeLight;
            if (mode == ModeDark) return ModeDark;
            try
            {
                using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
                var v = key?.GetValue("AppsUseLightTheme");
                if (v is int i && i == 1) return ModeLight;
            }
            catch { }
            return ModeDark;
        }

        public static string NextMode(string mode) => mode == ModeDark ? ModeLight : mode == ModeLight ? ModeSystem : ModeDark;

        public static int SwatchIndex(string hex)
        {
            for (int i = 0; i < AccentSwatches.Length; i++)
                if (string.Equals(AccentSwatches[i].Hex, hex, StringComparison.OrdinalIgnoreCase)) return i;
            return -1;
        }

        public static string SwatchLabel(string hex)
        {
            int i = SwatchIndex(hex);
            return i >= 0 ? $"{AccentSwatches[i].Name} {hex.ToUpperInvariant()}" : (string.IsNullOrEmpty(hex) ? AccentSwatches[0].Name : hex.ToUpperInvariant());
        }

        public static bool TryParse(string hex, out Color c)
        {
            c = default;
            if (string.IsNullOrWhiteSpace(hex)) return false;
            string h = hex.Trim();
            if (h[0] != '#') h = "#" + h;
            if (h.Length != 7) return false;
            if (!int.TryParse(h.Substring(1), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out int rgb)) return false;
            c = Color.FromRgb((byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb);
            return true;
        }

        private static Color ParseOrThrow(string hex)
        {
            if (!TryParse(hex, out var c)) throw new ArgumentException("bad palette colour " + hex);
            return c;
        }

        private static void Set(ResourceDictionary res, string key, Color c)
        {
            var b = new SolidColorBrush(c);
            b.Freeze();
            res[key] = b;
        }

        /// <summary>Relative luminance (sRGB), used to pick text-on-accent.</summary>
        private static double Luminance(Color c)
        {
            static double Lin(byte v) { double s = v / 255.0; return s <= 0.03928 ? s / 12.92 : Math.Pow((s + 0.055) / 1.055, 2.4); }
            return 0.2126 * Lin(c.R) + 0.7152 * Lin(c.G) + 0.0722 * Lin(c.B);
        }
    }
}
