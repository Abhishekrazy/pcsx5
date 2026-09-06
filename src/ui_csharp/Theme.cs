using System;
using System.Globalization;
using System.Windows;
using System.Windows.Media;
using System.Collections.Generic;
using System.Windows.Controls;
using System.Windows.Shapes;
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

        // Corner style (asked 2026-09-06): rounded, hard edge, or corner cut.
        public const string CornersRounded = "rounded";
        public const string CornersSharp = "sharp";
        public const string CornersCut = "cut";
        public static string Corners { get; private set; } = CornersRounded;

        /// <summary>Raised after Apply() so clipped elements can recompute.</summary>
        public static event Action Changed;

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
        public static bool Apply(string mode, string accentHex, string groundHex) => Apply(mode, accentHex, groundHex, Corners);

        public static bool Apply(string mode, string accentHex, string groundHex, string corners)
        {
            bool ok = true;
            Corners = corners == CornersSharp || corners == CornersCut ? corners : CornersRounded;
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
            // The in-app folder picker's own brushes (App.xaml) were fixed dark
            // values; they now follow the palette (TASKS 4.13 step 11).
            Set(res, "PickerSurfaceBrush", ParseOrThrow(p.Surface));
            Set(res, "PickerListBrush", ground);
            Set(res, "PickerBorderBrush", ParseOrThrow(p.Hairline));
            Set(res, "PickerTextBrush", ParseOrThrow(p.Text));
            Set(res, "PickerMutedBrush", ParseOrThrow(p.TextMuted));

            // Corner radii. Sharp and cut both use 0 here; cut gets its shape
            // from the ClipCorners attached property below.
            bool round = Corners == CornersRounded;
            res["ThemeCornerS"] = new CornerRadius(round ? 6 : 0);
            res["ThemeCornerM"] = new CornerRadius(round ? 10 : 0);
            res["ThemeCornerL"] = new CornerRadius(round ? 16 : 0);
            res["ThemeCornerXL"] = new CornerRadius(round ? 24 : 0);
            res["ThemeCornerPill"] = new CornerRadius(round ? 999 : 0);

            Changed?.Invoke();
            foreach (var wr in _clipped.ToArray())
            {
                if (wr.TryGetTarget(out var fe)) RefreshClip(fe); else _clipped.Remove(wr);
            }
            return ok;
        }

        // ---- ClipCorners attached property ---------------------------------
        // A Border's CornerRadius does not clip its children, so a cover image
        // inside a rounded card pokes square corners out. Setting
        // local:Theme.ClipCorners="12" on an element clips it to the current
        // corner style: rounded (radius 12), sharp (no clip), or cut (a 12 px
        // chamfer on each corner). The clip follows the element's size and the
        // theme setting.
        public static readonly DependencyProperty ClipCornersProperty = DependencyProperty.RegisterAttached(
            "ClipCorners", typeof(double), typeof(Theme), new PropertyMetadata(0.0, OnClipCornersChanged));
        public static void SetClipCorners(DependencyObject d, double v) => d.SetValue(ClipCornersProperty, v);
        public static double GetClipCorners(DependencyObject d) => (double)d.GetValue(ClipCornersProperty);
        private static readonly List<WeakReference<FrameworkElement>> _clipped = new();

        private static void OnClipCornersChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        {
            if (d is not FrameworkElement fe) return;
            fe.SizeChanged -= ClippedSizeChanged;
            fe.SizeChanged += ClippedSizeChanged;
            // A Border's own stroke must not be clipped (it would lose its
            // outer half at every corner), so a Border clips its CHILD; the
            // child may be laid out later than the border, so listen to both.
            if (fe is Border b && b.Child is FrameworkElement ch)
            {
                ch.SizeChanged -= ClippedChildSizeChanged;
                ch.SizeChanged += ClippedChildSizeChanged;
            }
            _clipped.Add(new WeakReference<FrameworkElement>(fe));
            RefreshClip(fe);
        }

        private static void ClippedSizeChanged(object sender, SizeChangedEventArgs e) => RefreshClip((FrameworkElement)sender);
        private static void ClippedChildSizeChanged(object sender, SizeChangedEventArgs e)
        {
            if (sender is FrameworkElement ch && ch.Parent is FrameworkElement parent) RefreshClip(parent);
        }

        private static void RefreshClip(FrameworkElement fe)
        {
            double size = GetClipCorners(fe);
            FrameworkElement target = fe;
            // Rounded: clip the child so the stroke keeps its full width along
            // the curve. Cut: clip the element itself so the stroke follows the
            // chamfer too (a square stroke around a chamfered image reads as a
            // bug, and the cut-away corner is surface-on-ground, near invisible).
            if (fe is Border b && b.Child is FrameworkElement ch)
            {
                if (Corners == CornersCut) { ch.Clip = null; }
                else
                {
                    fe.Clip = null;
                    target = ch;
                // The child sits inside the stroke, so its corner is tighter by
                // the stroke width (a rounded outer 16 with a 1 px stroke is an
                // inner 15).
                    size = Math.Max(0, size - Math.Max(b.BorderThickness.Left, b.BorderThickness.Top));
                }
            }
            double w = target.ActualWidth, h = target.ActualHeight;
            if (size <= 0 || w <= 0 || h <= 0) { target.Clip = null; return; }
            if (Corners == CornersSharp) { target.Clip = null; return; }
            if (Corners == CornersRounded)
            {
                target.Clip = new RectangleGeometry(new Rect(0, 0, w, h), size, size);
                return;
            }
            double c = Math.Min(size, Math.Min(w, h) / 2);
            var g = new StreamGeometry();
            using (var ctx = g.Open())
            {
                ctx.BeginFigure(new Point(c, 0), true, true);
                ctx.LineTo(new Point(w - c, 0), false, false);
                ctx.LineTo(new Point(w, c), false, false);
                ctx.LineTo(new Point(w, h - c), false, false);
                ctx.LineTo(new Point(w - c, h), false, false);
                ctx.LineTo(new Point(c, h), false, false);
                ctx.LineTo(new Point(0, h - c), false, false);
                ctx.LineTo(new Point(0, c), false, false);
            }
            g.Freeze();
            target.Clip = g;
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
