using System;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;
using System.Windows.Threading;

namespace Pcsx5Ui
{
    /// <summary>
    /// Visual DualSense controller with animated button highlights,
    /// analog stick position overlays, trigger bars, and touchpad.
    /// Bind to a ControllerVisualizerState or call Update() per frame.
    /// </summary>
    public partial class ControllerVisualizer : UserControl
    {
        // ---- Button state model ----
        public class State : INotifyPropertyChanged
        {
            public bool Connected { get; set; }
            public bool UpPressed { get; set; }
            public bool DownPressed { get; set; }
            public bool LeftPressed { get; set; }
            public bool RightPressed { get; set; }
            public bool CrossPressed { get; set; }
            public bool CirclePressed { get; set; }
            public bool SquarePressed { get; set; }
            public bool TrianglePressed { get; set; }
            public bool L1Pressed { get; set; }
            public bool R1Pressed { get; set; }
            public bool L2Pressed { get; set; }
            public bool R2Pressed { get; set; }
            public bool L3Pressed { get; set; }
            public bool R3Pressed { get; set; }
            public bool CreatePressed { get; set; }
            public bool OptionsPressed { get; set; }
            public bool PsPressed { get; set; }
            public bool MutePressed { get; set; }
            public bool TouchPressed { get; set; }
            public byte L2Analog { get; set; }
            public byte R2Analog { get; set; }
            public byte LeftX { get; set; } = 128;
            public byte LeftY { get; set; } = 128;
            public byte RightX { get; set; } = 128;
            public byte RightY { get; set; } = 128;
            public int TouchFingerCount { get; set; }
            public double TouchFinger1X { get; set; }
            public double TouchFinger1Y { get; set; }
            public double TouchFinger2X { get; set; }
            public double TouchFinger2Y { get; set; }
            public int BatteryLevel { get; set; }
            public bool BatteryCharging { get; set; }

            public event PropertyChangedEventHandler PropertyChanged;
            public void NotifyAll() => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(""));
        }

        public static readonly DependencyProperty ViewModelProperty =
            DependencyProperty.Register(nameof(ViewModel), typeof(State), typeof(ControllerVisualizer),
                new PropertyMetadata(null, OnViewModelChanged));

        public State ViewModel
        {
            get => (State)GetValue(ViewModelProperty);
            set => SetValue(ViewModelProperty, value);
        }

        private static void OnViewModelChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        {
            if (d is ControllerVisualizer ctrl && e.NewValue is State s)
                ctrl.UpdateFromState(s);
        }

        public ControllerVisualizer()
        {
            InitializeComponent();
            if (DesignerProperties.GetIsInDesignMode(this)) return;
        }

        /// <summary>Update all visual elements from a State object.</summary>
        public void Update(State s)
        {
            UpdateFromState(s);
        }

        private void UpdateFromState(State s)
        {
            if (s == null) return;

            // Connection status
            StatusText.Text = s.Connected ? "Connected" : "Disconnected";
            StatusText.Foreground = new SolidColorBrush(s.Connected
                ? Color.FromRgb(0x44, 0xCC, 0x44)
                : Color.FromRgb(0xFF, 0x55, 0x55));

            // Battery
            BatteryText.Text = s.BatteryLevel > 0
                ? $"Battery: {s.BatteryLevel}%" + (s.BatteryCharging ? " (charging)" : "")
                : "";

            // Shoulder buttons
            SetBorderGlow(L1Border, s.L1Pressed, "#65A5D9");
            SetBorderGlow(R1Border, s.R1Pressed, "#65A5D9");
            SetBorderGlow(L2Border, s.L2Pressed, "#FF6633");
            SetBorderGlow(R2Border, s.R2Pressed, "#FF6633");

            // Trigger bars
            L2Bar.Width = s.L2Analog * 46.0 / 255.0;
            R2Bar.Width = s.R2Analog * 46.0 / 255.0;

            // Face buttons
            SetEllipseGlow(CrossBtn, s.CrossPressed, "#3399FF");
            SetEllipseGlow(CircleBtn, s.CirclePressed, "#FF8833");
            SetEllipseGlow(SquareBtn, s.SquarePressed, "#FF77AA");
            SetEllipseGlow(TriangleBtn, s.TrianglePressed, "#30CC44");

            // D-pad: we set fill colors via the rectangles directly
            SetRectFill(UpRect2, s.UpPressed, "#65A5D9");
            SetRectFill(DownRect2, s.DownPressed, "#65A5D9");
            SetRectFill(LeftRect2, s.LeftPressed, "#65A5D9");
            SetRectFill(RightRect2, s.RightPressed, "#65A5D9");

            // Stick positions
            MoveStickKnob(LeftStickKnob, LeftStickArea, s.LeftX, s.LeftY);
            MoveStickKnob(RightStickKnob, RightStickArea, s.RightX, s.RightY);
            SetEllipseGlow(LeftStickKnob, s.L3Pressed, "#FF5555");
            SetEllipseGlow(RightStickKnob, s.R3Pressed, "#FF5555");

            // Center buttons
            SetEllipseGlow(CreateBtn, s.CreatePressed, "#65A5D9");
            SetEllipseGlow(OptionsBtn, s.OptionsPressed, "#65A5D9");
            SetEllipseGlow(PsBtn, s.PsPressed, "#65A5D9");
            SetEllipseGlow(MuteBtn, s.MutePressed, "#FF5555");

            // Touchpad fingers
            TouchFinger1.Visibility = s.TouchFingerCount > 0 ? Visibility.Visible : Visibility.Hidden;
            TouchFinger2.Visibility = s.TouchFingerCount > 1 ? Visibility.Visible : Visibility.Hidden;
            if (s.TouchFingerCount > 0)
            {
                Canvas.SetLeft(TouchFinger1, 20 + s.TouchFinger1X * 160.0 / 1919.0);
                Canvas.SetTop(TouchFinger1, 382 + s.TouchFinger1Y * 36.0 / 941.0);
            }
            if (s.TouchFingerCount > 1)
            {
                Canvas.SetLeft(TouchFinger2, 20 + s.TouchFinger2X * 160.0 / 1919.0);
                Canvas.SetTop(TouchFinger2, 382 + s.TouchFinger2Y * 36.0 / 941.0);
            }
        }

        private void SetBorderGlow(Border border, bool pressed, string colorHex)
        {
            var c = (Color)ColorConverter.ConvertFromString(colorHex);
            border.Background = pressed
                ? new SolidColorBrush(Color.FromArgb(120, c.R, c.G, c.B))
                : new SolidColorBrush(Color.FromArgb(30, c.R, c.G, c.B));
            border.BorderBrush = pressed
                ? new SolidColorBrush(c)
                : new SolidColorBrush(Color.FromArgb(60, c.R, c.G, c.B));
            border.BorderThickness = pressed ? new Thickness(2) : new Thickness(1);
        }

        private void SetEllipseGlow(Ellipse ellipse, bool pressed, string colorHex)
        {
            var c = (Color)ColorConverter.ConvertFromString(colorHex);
            ellipse.Fill = pressed
                ? new SolidColorBrush(Color.FromArgb(200, c.R, c.G, c.B))
                : new SolidColorBrush(Color.FromArgb(40, c.R, c.G, c.B));
            ellipse.Stroke = pressed
                ? new SolidColorBrush(c)
                : new SolidColorBrush(Color.FromArgb(60, c.R, c.G, c.B));
            ellipse.StrokeThickness = pressed ? 3 : 1;
        }

        private void SetRectFill(Rectangle rect, bool pressed, string colorHex)
        {
            var c = (Color)ColorConverter.ConvertFromString(colorHex);
            rect.Fill = pressed
                ? new SolidColorBrush(Color.FromArgb(180, c.R, c.G, c.B))
                : new SolidColorBrush(Color.FromArgb(30, c.R, c.G, c.B));
        }

        private void MoveStickKnob(Ellipse knob, Grid area, byte x, byte y)
        {
            double nx = (x - 128.0) / 128.0;
            double ny = (y - 128.0) / 128.0;
            double areaW = area.ActualWidth > 0 ? area.ActualWidth : 60;
            double areaH = area.ActualHeight > 0 ? area.ActualHeight : 60;
            double maxOff = (areaW - knob.Width) / 2 - 4;
            double ox = nx * maxOff;
            double oy = ny * maxOff;
            knob.Margin = new Thickness(
                areaW / 2 - knob.Width / 2 + ox,
                areaH / 2 - knob.Height / 2 + oy,
                0, 0);
        }
    }
}
