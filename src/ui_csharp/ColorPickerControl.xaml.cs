using System;
using System.Collections.Generic;
using System.Globalization;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace Pcsx5Ui
{
    /// <summary>
    /// A compact HSV colour picker for the lightbar: a saturation/brightness
    /// plane (mouse, or the right stick), a hue strip (left stick, keyboard,
    /// mouse) and a hex field, so every route to a colour is reachable without
    /// a mouse (Rule 12). Presentation only; whoever listens to
    /// <see cref="ColorChanged"/> owns what the colour means (Rule 11).
    /// </summary>
    public partial class ColorPickerControl : UserControl
    {
        public static readonly DependencyProperty SelectedColorProperty = DependencyProperty.Register(
            nameof(SelectedColor), typeof(Color), typeof(ColorPickerControl),
            new FrameworkPropertyMetadata(Color.FromRgb(0, 90, 255), FrameworkPropertyMetadataOptions.BindsTwoWayByDefault, OnSelectedColorChanged));

        public Color SelectedColor
        {
            get => (Color)GetValue(SelectedColorProperty);
            set => SetValue(SelectedColorProperty, value);
        }

        public event EventHandler<Color> ColorChanged;

        private double _h, _s = 1, _v = 1;   // working HSV
        private bool _syncing;                // guards slider/hex feedback loops
        private bool _dragging;

        public ColorPickerControl()
        {
            InitializeComponent();
            Loaded += (s, e) => SyncFromColor(SelectedColor);
            SvPlane.SizeChanged += (s, e) => PlaceThumb();
        }

        /// <summary>The controls pad navigation may land on, in reading order.</summary>
        public IEnumerable<Control> NavControls
        {
            get
            {
                yield return HueSlider;
                yield return HexBox;
            }
        }

        private static void OnSelectedColorChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        {
            var p = (ColorPickerControl)d;
            if (!p._syncing) p.SyncFromColor((Color)e.NewValue);
            p.ColorChanged?.Invoke(p, (Color)e.NewValue);
        }

        private void SyncFromColor(Color c)
        {
            if (HueSlider == null) return;
            RgbToHsv(c, out double h, out double s, out double v);
            // Keep the hue when the colour is grey (hue is undefined there).
            if (s > 0.001) _h = h;
            _s = s; _v = v;
            _syncing = true;
            HueSlider.Value = _h;
            SvHueFill.Fill = new SolidColorBrush(HsvToRgb(_h, 1, 1));
            Preview.Background = new SolidColorBrush(c);
            HexBox.Text = $"#{c.R:X2}{c.G:X2}{c.B:X2}";
            _syncing = false;
            PlaceThumb();
        }

        private void Commit()
        {
            var c = HsvToRgb(_h, _s, _v);
            _syncing = true;
            HueSlider.Value = _h;   // the bar's thumb follows stick/plane changes too
            SvHueFill.Fill = new SolidColorBrush(HsvToRgb(_h, 1, 1));
            Preview.Background = new SolidColorBrush(c);
            HexBox.Text = $"#{c.R:X2}{c.G:X2}{c.B:X2}";
            SelectedColor = c;
            _syncing = false;
            PlaceThumb();
        }

        /// <summary>Pad control: move the plane cursor (saturation right, value
        /// up) by a fraction of the plane per call.</summary>
        public void NudgePlane(double dx, double dy)
        {
            if (dx == 0 && dy == 0) return;
            _s = Math.Max(0, Math.Min(1, _s + dx));
            _v = Math.Max(0, Math.Min(1, _v + dy));
            Commit();
        }

        /// <summary>Pad control: move the hue bar by degrees.</summary>
        public void NudgeHue(double degrees)
        {
            if (degrees == 0) return;
            _h = (_h + degrees) % 360; if (_h < 0) _h += 360;
            Commit();
        }

        private void PlaceThumb()
        {
            if (SvPlane.ActualWidth <= 0) return;
            Canvas.SetLeft(SvThumb, _s * SvPlane.ActualWidth - SvThumb.Width / 2);
            Canvas.SetTop(SvThumb, (1 - _v) * SvPlane.ActualHeight - SvThumb.Height / 2);
        }

        private void HueSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (_syncing) return;
            _h = e.NewValue;
            Commit();
        }

        private void SvPlane_MouseDown(object sender, MouseButtonEventArgs e)
        {
            _dragging = true;
            SvPlane.CaptureMouse();
            PickFromPlane(e.GetPosition(SvPlane));
        }

        private void SvPlane_MouseMove(object sender, MouseEventArgs e)
        {
            if (_dragging) PickFromPlane(e.GetPosition(SvPlane));
        }

        private void SvPlane_MouseUp(object sender, MouseButtonEventArgs e)
        {
            _dragging = false;
            SvPlane.ReleaseMouseCapture();
        }

        private void PickFromPlane(Point p)
        {
            _s = Math.Max(0, Math.Min(1, p.X / SvPlane.ActualWidth));
            _v = 1 - Math.Max(0, Math.Min(1, p.Y / SvPlane.ActualHeight));
            Commit();
        }

        private void HexBox_KeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.Enter) { ApplyHex(); e.Handled = true; }
        }

        private void HexBox_LostFocus(object sender, RoutedEventArgs e) => ApplyHex();

        private void ApplyHex()
        {
            if (_syncing) return;
            string t = HexBox.Text.Trim().TrimStart('#');
            if (t.Length == 6 && int.TryParse(t, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out int rgb))
                SelectedColor = Color.FromRgb((byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb);
            else
                SyncFromColor(SelectedColor);   // reject: show the current value again
        }

        private static void RgbToHsv(Color c, out double h, out double s, out double v)
        {
            double r = c.R / 255.0, g = c.G / 255.0, b = c.B / 255.0;
            double max = Math.Max(r, Math.Max(g, b)), min = Math.Min(r, Math.Min(g, b)), d = max - min;
            v = max;
            s = max <= 0 ? 0 : d / max;
            if (d <= 0) h = 0;
            else if (max == r) h = 60 * (((g - b) / d) % 6);
            else if (max == g) h = 60 * ((b - r) / d + 2);
            else h = 60 * ((r - g) / d + 4);
            if (h < 0) h += 360;
        }

        private static Color HsvToRgb(double h, double s, double v)
        {
            double c = v * s, x = c * (1 - Math.Abs((h / 60) % 2 - 1)), m = v - c;
            double r, g, b;
            if (h < 60) { r = c; g = x; b = 0; }
            else if (h < 120) { r = x; g = c; b = 0; }
            else if (h < 180) { r = 0; g = c; b = x; }
            else if (h < 240) { r = 0; g = x; b = c; }
            else if (h < 300) { r = x; g = 0; b = c; }
            else { r = c; g = 0; b = x; }
            return Color.FromRgb((byte)Math.Round((r + m) * 255), (byte)Math.Round((g + m) * 255), (byte)Math.Round((b + m) * 255));
        }
    }
}
