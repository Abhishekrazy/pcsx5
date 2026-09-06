using System;
using System.Windows;
using System.Windows.Media;

namespace Pcsx5Ui
{
    /// <summary>
    /// A transparent, borderless window that floats over the shell's game area
    /// and hosts one WPF element. Needed because the emulator renders into a
    /// native child window, and native windows paint over every WPF element in
    /// the same window (the "airspace" rule): a notice or menu drawn in the
    /// main window would sit invisibly under the game. The overlay follows the
    /// game area whenever the owner moves or resizes.
    /// </summary>
    public sealed class OverlayWindow : Window
    {
        private readonly Window _owner;
        private readonly FrameworkElement _anchor;

        public OverlayWindow(Window owner, FrameworkElement anchor)
        {
            _owner = owner;
            _anchor = anchor;
            Owner = owner;
            WindowStyle = WindowStyle.None;
            // Opaque and sized to its content, placed at the anchor's bottom-right:
            // a transparent full-area overlay would be a layered window, which
            // plain screen captures skip, and it would cover the game for input.
            AllowsTransparency = false;
            SizeToContent = SizeToContent.WidthAndHeight;
            SetResourceReference(BackgroundProperty, "ThemeSurface");
            ResizeMode = ResizeMode.NoResize;
            ShowInTaskbar = false;
            ShowActivated = false;
            Topmost = false;   // owned windows already stay above their owner
            owner.LocationChanged += (s, e) => Track();
            owner.SizeChanged += (s, e) => Track();
            anchor.SizeChanged += (s, e) => Track();
            owner.StateChanged += (s, e) => { if (owner.WindowState == WindowState.Minimized) Hide(); else if (IsVisible) Track(); };
        }

        /// <summary>Show the element over the anchor's screen rectangle.</summary>
        public void ShowOver(FrameworkElement content)
        {
            Content = content;
            Track();
            if (!IsVisible) Show();
            Track();   // ActualWidth/Height exist only after the first layout
            SizeChanged -= OnOwnSizeChanged; SizeChanged += OnOwnSizeChanged;
        }

        private void OnOwnSizeChanged(object sender, SizeChangedEventArgs e) => Track();

        /// <summary>Place this window at the anchor's bottom-right corner, inset
        /// by a margin, with the window's own size following its content.</summary>
        private void Track()
        {
            if (_anchor == null || !_anchor.IsVisible || PresentationSource.FromVisual(_anchor) == null) return;
            var bottomRight = _anchor.PointToScreen(new Point(_anchor.ActualWidth, _anchor.ActualHeight));
            // PointToScreen returns device pixels; the window wants DIPs.
            var m = PresentationSource.FromVisual(_owner)?.CompositionTarget?.TransformFromDevice ?? Matrix.Identity;
            var br = m.Transform(bottomRight);
            const double margin = 24;
            double w = ActualWidth > 0 ? ActualWidth : (Content as FrameworkElement)?.DesiredSize.Width ?? 0;
            double h = ActualHeight > 0 ? ActualHeight : (Content as FrameworkElement)?.DesiredSize.Height ?? 0;
            if (Content is FrameworkElement fe && (w <= 0 || h <= 0))
            {
                fe.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
                w = fe.DesiredSize.Width; h = fe.DesiredSize.Height;
            }
            Left = br.X - margin - w;
            Top = br.Y - margin - h;
        }
    }
}
