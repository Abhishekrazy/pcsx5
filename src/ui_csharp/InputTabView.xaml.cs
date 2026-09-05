using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using System.Windows.Threading;

namespace Pcsx5Ui
{
    /// <summary>
    /// Live controller view: device information, real-time input, motion
    /// graphs, a controller picker, and the input / speaker / haptics tests.
    /// </summary>
    /// <remarks>
    /// Reads the pad exclusively through <see cref="CoreBridge"/>, i.e. the
    /// core's own reader.  The previous tab read it through a second C# HID
    /// implementation that interleaved with the core's on the same device.
    ///
    /// Rendering follows the vendored VSCView theme in
    /// assets/gamepad/dualsense/layout.json: a 1150x850 canvas, sprites drawn
    /// centred at resolved positions, and small expressions that decide which
    /// sprite shows or how far it slides.  The JSON is consumed as data; the
    /// evaluator below implements just the grammar the theme uses.
    ///
    /// This class deliberately holds no emulator state and never touches
    /// MainWindow.  The one thing it exposes outward is
    /// <see cref="IsTestRunning"/>, so the shell can refuse tab switches while
    /// a test is in progress.
    /// </remarks>
    public partial class InputTabView : UserControl
    {
        // ── Test lock, observed by the shell's tab handlers ─────────────────
        public static bool IsTestRunning { get; private set; }
        public static event Action TestStateChanged;

        private static void SetTestRunning(bool on)
        {
            if (IsTestRunning == on) return;
            IsTestRunning = on;
            TestStateChanged?.Invoke();
        }

        // ── Polling ─────────────────────────────────────────────────────────
        private readonly DispatcherTimer _poll = new DispatcherTimer(DispatcherPriority.Render);
        private readonly DispatcherTimer _rescan = new DispatcherTimer();
        private int _padIndex = 0;
        private CoreBridge.PadState _state = CoreBridge.PadState.Create();
        private bool _haveState;

        // ── Theme ───────────────────────────────────────────────────────────
        private string _assetDir;
        private readonly Dictionary<string, BitmapImage> _sprites = new Dictionary<string, BitmapImage>(StringComparer.OrdinalIgnoreCase);
        private readonly List<ThemeNode> _nodes = new List<ThemeNode>();
        private readonly List<Point> _trail0 = new List<Point>();
        private readonly List<Point> _trail1 = new List<Point>();

        // ── Motion history (one sample per poll, ~60/s, 4 s window) ─────────
        private const int HistoryLen = 240;
        private readonly float[,] _accelHist = new float[3, HistoryLen];
        private readonly float[,] _gyroHist = new float[3, HistoryLen];
        private int _histPos;

        // ── Input test ──────────────────────────────────────────────────────
        private bool _inputTest;
        private uint _buttonsSeen;
        private byte _lxMin = 255, _lxMax, _lyMin = 255, _lyMax, _rxMin = 255, _rxMax, _ryMin = 255, _ryMax, _l2Max, _r2Max;

        public InputTabView()
        {
            InitializeComponent();
            Loaded += (s, e) => Start();
            Unloaded += (s, e) => Stop();
        }

        // ── lifecycle ───────────────────────────────────────────────────────
        private void Start()
        {
            _assetDir = ResolveAssetDir();
            if (_assetDir == null)
            {
                TestStatusText.Text = I18n.Tr("input.assets_missing");
            }
            else
            {
                LoadTheme(System.IO.Path.Combine(_assetDir, "layout.json"));
            }

            RescanPads();
            _rescan.Interval = TimeSpan.FromMilliseconds(500);
            _rescan.Tick += (s, e) => RescanPads();
            _rescan.Start();

            _poll.Interval = TimeSpan.FromMilliseconds(16);
            _poll.Tick += (s, e) => PollOnce();
            _poll.Start();
        }

        private void Stop()
        {
            _poll.Stop();
            _rescan.Stop();
            if (_inputTest) EndInputTest();
        }

        /// <summary>
        /// Same walk as I18n.Load: assets beside the app, else up to five
        /// parents, so a developer build finds the repository copy.
        /// </summary>
        private static string ResolveAssetDir()
        {
            string dir = AppDomain.CurrentDomain.BaseDirectory;
            for (int i = 0; i < 6 && dir != null; i++)
            {
                string candidate = System.IO.Path.Combine(dir, "assets", "gamepad", "dualsense");
                if (File.Exists(System.IO.Path.Combine(candidate, "layout.json"))) return candidate;
                dir = System.IO.Path.GetDirectoryName(dir);
            }
            return null;
        }

        // ── pads ────────────────────────────────────────────────────────────
        private void RescanPads()
        {
            int keep = _padIndex;
            var items = new List<KeyValuePair<int, string>>();
            for (int i = 0; i < 8; i++)
            {
                var st = CoreBridge.PadState.Create();
                if (CoreBridge.pcsx5_pad_get_state(i, ref st) == 0 && st.Connected != 0)
                {
                    string transport = st.Bluetooth != 0 ? I18n.Tr("input.bluetooth") : I18n.Tr("input.usb");
                    items.Add(new KeyValuePair<int, string>(i, string.Format(I18n.Tr("input.pad_label"), i + 1, transport)));
                }
            }

            // Rebuild only when the set changed, so the picker does not flicker.
            bool changed = items.Count != PadPicker.Items.Count;
            if (!changed)
            {
                for (int i = 0; i < items.Count; i++)
                {
                    if (!(PadPicker.Items[i] is ComboBoxItem cbi) || (int)cbi.Tag != items[i].Key) { changed = true; break; }
                }
            }
            if (!changed) return;

            PadPicker.Items.Clear();
            int selectIdx = -1;
            for (int i = 0; i < items.Count; i++)
            {
                PadPicker.Items.Add(new ComboBoxItem { Content = items[i].Value, Tag = items[i].Key });
                if (items[i].Key == keep) selectIdx = i;
            }
            if (items.Count == 0)
            {
                PadPicker.Items.Add(new ComboBoxItem { Content = I18n.Tr("input.no_controller"), Tag = -1, IsEnabled = false });
                PadPicker.SelectedIndex = 0;
            }
            else
            {
                PadPicker.SelectedIndex = selectIdx >= 0 ? selectIdx : 0;
            }
        }

        private void PadPicker_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (PadPicker.SelectedItem is ComboBoxItem cbi && cbi.Tag is int idx && idx >= 0)
            {
                _padIndex = idx;
                _trail0.Clear(); _trail1.Clear();
                RefreshFirmware();
            }
        }

        // ── poll + paint ────────────────────────────────────────────────────
        private void PollOnce()
        {
            var st = CoreBridge.PadState.Create();
            int rc = CoreBridge.pcsx5_pad_get_state(_padIndex, ref st);
            _haveState = rc == 0 && st.Connected != 0;
            _state = st;

            if (rc == -2)
            {
                // The struct_size guard tripped: the C# mirror and the core's
                // struct disagree.  Say so instead of drawing garbage.
                TestStatusText.Text = I18n.Tr("input.abi_mismatch");
                _poll.Stop();
                return;
            }

            UpdateDeviceInfo();
            PushHistory();
            PaintController();
            PaintGraphs();
            if (_inputTest) AccumulateInputTest();
        }

        private void UpdateDeviceInfo()
        {
            if (!_haveState)
            {
                TransportText.Text = I18n.Tr("input.not_connected");
                BatteryText.Text = ConnectionText.Text = HeadphoneText.Text = MicText.Text = UsbText.Text = "—";
                return;
            }
            var s = _state;
            TransportText.Text = s.Bluetooth != 0 ? I18n.Tr("input.bluetooth") : I18n.Tr("input.usb");

            string power = s.BatteryFull != 0 ? I18n.Tr("input.battery_full")
                         : s.BatteryCharging != 0 ? I18n.Tr("input.battery_charging")
                         : I18n.Tr("input.battery_discharging");
            BatteryText.Text = string.Format(I18n.Tr("input.battery_fmt"), s.BatteryLevel, power);
            ConnectionText.Text = s.Bluetooth != 0 ? I18n.Tr("input.bluetooth") : I18n.Tr("input.usb");
            HeadphoneText.Text = s.Headphone != 0 ? I18n.Tr("input.yes") : I18n.Tr("input.no");

            string mic = s.MicJack != 0 ? I18n.Tr("input.mic_jack") : I18n.Tr("input.mic_builtin");
            if (s.MicMuted != 0) mic += " · " + I18n.Tr("input.muted");
            MicText.Text = mic;

            var usb = new List<string>();
            if (s.UsbData != 0) usb.Add(I18n.Tr("input.usb_data"));
            if (s.UsbPower != 0) usb.Add(I18n.Tr("input.usb_power"));
            UsbText.Text = usb.Count == 0 ? I18n.Tr("input.no") : string.Join(" · ", usb);
        }

        private void PushHistory()
        {
            _accelHist[0, _histPos] = _state.AccelX; _accelHist[1, _histPos] = _state.AccelY; _accelHist[2, _histPos] = _state.AccelZ;
            _gyroHist[0, _histPos] = _state.GyroX;   _gyroHist[1, _histPos] = _state.GyroY;   _gyroHist[2, _histPos] = _state.GyroZ;
            _histPos = (_histPos + 1) % HistoryLen;
        }

        // ── firmware ────────────────────────────────────────────────────────
        private void FirmwareRefreshBtn_Click(object sender, RoutedEventArgs e) => RefreshFirmware();

        private unsafe void RefreshFirmware()
        {
            var fw = CoreBridge.PadFirmware.Create();
            int rc = CoreBridge.pcsx5_pad_get_firmware(_padIndex, ref fw);
            if (rc != 0 || fw.Valid == 0)
            {
                FirmwareText.Text = I18n.Tr("input.firmware_unavailable");
                return;
            }
            string date = FixedAscii(fw.BuildDate, 12), time = FixedAscii(fw.BuildTime, 9);
            FirmwareText.Text =
                string.Format(I18n.Tr("input.firmware_main"), CoreBridge.PadFirmware.FormatVersion(fw.MainVersion)) + "\n" +
                string.Format(I18n.Tr("input.firmware_sbl"),  CoreBridge.PadFirmware.FormatVersion(fw.SblVersion)) + "\n" +
                string.Format(I18n.Tr("input.firmware_dsp"),  CoreBridge.PadFirmware.FormatDsp(fw.DspVersion)) + "\n" +
                string.Format(I18n.Tr("input.firmware_model"), fw.HardwareInfo & 0xFFFF, (fw.HardwareInfo >> 8) & 0xFF) + "\n" +
                string.Format(I18n.Tr("input.firmware_built"), date, time);
        }

        private static unsafe string FixedAscii(byte* p, int n)
        {
            int len = 0;
            while (len < n && p[len] != 0) len++;
            return System.Text.Encoding.ASCII.GetString(p, len);
        }

        // ── tests ───────────────────────────────────────────────────────────
        private void InputTestBtn_Click(object sender, RoutedEventArgs e)
        {
            if (_inputTest) { EndInputTest(); return; }
            if (!_haveState) { TestStatusText.Text = I18n.Tr("input.not_connected"); return; }
            _inputTest = true;
            _buttonsSeen = 0;
            _lxMin = _lyMin = _rxMin = _ryMin = 255;
            _lxMax = _lyMax = _rxMax = _ryMax = _l2Max = _r2Max = 0;
            InputTestBtn.Content = I18n.Tr("input.test_stop");
            SpeakerTestBtn.IsEnabled = HapticsTestBtn.IsEnabled = false;
            SetTestRunning(true);
            TestStatusText.Text = I18n.Tr("input.test_input_running");
        }

        private void AccumulateInputTest()
        {
            var s = _state;
            _buttonsSeen |= s.Buttons;
            if (s.Lx < _lxMin) _lxMin = s.Lx; if (s.Lx > _lxMax) _lxMax = s.Lx;
            if (s.Ly < _lyMin) _lyMin = s.Ly; if (s.Ly > _lyMax) _lyMax = s.Ly;
            if (s.Rx < _rxMin) _rxMin = s.Rx; if (s.Rx > _rxMax) _rxMax = s.Rx;
            if (s.Ry < _ryMin) _ryMin = s.Ry; if (s.Ry > _ryMax) _ryMax = s.Ry;
            if (s.L2 > _l2Max) _l2Max = s.L2; if (s.R2 > _r2Max) _r2Max = s.R2;

            int buttons = 0; for (uint b = _buttonsSeen; b != 0; b &= b - 1) buttons++;
            bool sticks = _lxMax - _lxMin > 60 && _lyMax - _lyMin > 60 && _rxMax - _rxMin > 60 && _ryMax - _ryMin > 60;
            bool triggers = _l2Max > 60 && _r2Max > 60;
            TestStatusText.Text = string.Format(I18n.Tr("input.test_input_progress"),
                buttons, sticks ? I18n.Tr("input.ok") : I18n.Tr("input.pending"),
                triggers ? I18n.Tr("input.ok") : I18n.Tr("input.pending"));
        }

        private void EndInputTest()
        {
            _inputTest = false;
            InputTestBtn.Content = I18n.Tr("input.test_input");
            SpeakerTestBtn.IsEnabled = HapticsTestBtn.IsEnabled = true;
            SetTestRunning(false);
            TestStatusText.Text = I18n.Tr("input.test_input_done");
        }

        private async void SpeakerTestBtn_Click(object sender, RoutedEventArgs e)
            => await RunBlockingTest(() => CoreBridge.pcsx5_pad_play_speaker_test(), "input.test_speaker_running", "input.test_speaker_usb_hint");

        private async void HapticsTestBtn_Click(object sender, RoutedEventArgs e)
            => await RunBlockingTest(() => CoreBridge.pcsx5_pad_play_haptics_test(), "input.test_haptics_running", "input.test_haptics_usb_hint");

        private async Task RunBlockingTest(Func<int> test, string runningKey, string usbHintKey)
        {
            if (!_haveState) { TestStatusText.Text = I18n.Tr("input.not_connected"); return; }
            if (_state.Bluetooth == 0)
            {
                // Over USB the audio lanes are real Windows endpoints, not the
                // Bluetooth reports these tests drive.  Say so rather than
                // offering a button that silently does nothing.
                TestStatusText.Text = I18n.Tr(usbHintKey);
                return;
            }
            InputTestBtn.IsEnabled = SpeakerTestBtn.IsEnabled = HapticsTestBtn.IsEnabled = false;
            SetTestRunning(true);
            TestStatusText.Text = I18n.Tr(runningKey);
            int ok = 0;
            try { ok = await Task.Run(test); }   // blocks ~2 s in the core; never on the UI thread
            finally
            {
                SetTestRunning(false);
                InputTestBtn.IsEnabled = SpeakerTestBtn.IsEnabled = HapticsTestBtn.IsEnabled = true;
            }
            TestStatusText.Text = ok != 0 ? I18n.Tr("input.test_done_ask") : I18n.Tr("input.test_failed");
        }

        // ── theme: load ─────────────────────────────────────────────────────
        private enum NodeKind { Image, ShowHide, Slider, TrailPad, Basic3d }

        private sealed class ThemeNode
        {
            public NodeKind Kind;
            public double X, Y, W, H;       // resolved absolute canvas position
            public bool Center;
            public string Image, ShadowL, ShadowR, ShadowU, ShadowD;
            public string Input, InputX, InputY;
            public List<ThemeNode> Children = new List<ThemeNode>();
            // WPF elements
            public System.Windows.Controls.Image Img;
            public TranslateTransform Slide;
            public System.Windows.Controls.Image[] Shadows;
            public Ellipse Dot;
            public List<Ellipse> TrailDots;
            public ThemeNode Parent;
            public List<Ellipse> Trail => TrailDots;
        }

        private void LoadTheme(string path)
        {
            _nodes.Clear();
            PadCanvas.Children.Clear();
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            var root = doc.RootElement;
            foreach (var c in root.GetProperty("children").EnumerateArray())
                WalkNode(c, 0, 0, null);
            foreach (var n in _nodes) Materialise(n);
        }

        private void WalkNode(JsonElement el, double px, double py, ThemeNode parent)
        {
            double x = px + Num(el, "x"), y = py + Num(el, "y");
            string type = el.TryGetProperty("type", out var t) ? t.GetString() : "group";
            ThemeNode node = null;
            switch (type)
            {
                case "image":    node = new ThemeNode { Kind = NodeKind.Image }; break;
                case "showhide": node = new ThemeNode { Kind = NodeKind.ShowHide }; break;
                case "slider":   node = new ThemeNode { Kind = NodeKind.Slider }; break;
                case "trailpad": node = new ThemeNode { Kind = NodeKind.TrailPad }; break;
                case "basic3d1": node = new ThemeNode { Kind = NodeKind.Basic3d }; break;
            }
            if (node != null)
            {
                node.X = x; node.Y = y; node.W = Num(el, "width"); node.H = Num(el, "height");
                node.Center = el.TryGetProperty("center", out var c) && c.GetBoolean();
                node.Image = Str(el, "image"); node.ShadowL = Str(el, "shadowl"); node.ShadowR = Str(el, "shadowr");
                node.ShadowU = Str(el, "shadowu"); node.ShadowD = Str(el, "shadowd");
                node.Input = Str(el, "input"); node.InputX = Str(el, "inputX"); node.InputY = Str(el, "inputY");
                node.Parent = parent;
                _nodes.Add(node);
                parent?.Children.Add(node);
            }
            if (el.TryGetProperty("children", out var kids))
                foreach (var k in kids.EnumerateArray()) WalkNode(k, x, y, node ?? parent);
        }

        private static double Num(JsonElement el, string name) => el.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : 0;
        private static string Str(JsonElement el, string name) => el.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;

        private BitmapImage Sprite(string themePath)
        {
            if (string.IsNullOrEmpty(themePath)) return null;
            string file = themePath.Replace('\\', '/');
            file = file.Substring(file.LastIndexOf('/') + 1);
            if (_sprites.TryGetValue(file, out var cached)) return cached;
            // The body lives in body/, everything else in sprites/.  The theme's
            // base image name is colour-specific; ours is the white body.
            string full = file.StartsWith("DualSense_base", StringComparison.OrdinalIgnoreCase)
                ? System.IO.Path.Combine(_assetDir, "body", "DualSense_body_white_no_dynamic_parts.png")
                : System.IO.Path.Combine(_assetDir, "sprites", file);
            if (!File.Exists(full)) return null;
            var bmp = new BitmapImage();
            bmp.BeginInit();
            bmp.UriSource = new Uri(full);
            bmp.CacheOption = BitmapCacheOption.OnLoad;
            bmp.EndInit();
            bmp.Freeze();
            _sprites[file] = bmp;
            return bmp;
        }

        private void Materialise(ThemeNode n)
        {
            // Find the nearest slider ancestor: its transform moves this node.
            TranslateTransform slide = null;
            for (var p = n.Parent; p != null; p = p.Parent) if (p.Slide != null) { slide = p.Slide; break; }

            switch (n.Kind)
            {
                case NodeKind.Slider:
                    n.Slide = new TranslateTransform();
                    break;

                case NodeKind.Image:
                    n.Img = MakeImage(n.Image, n.X, n.Y, n.W, n.H, n.Center, slide);
                    break;

                case NodeKind.TrailPad:
                    n.Dot = new Ellipse { Width = n.W, Height = n.H, Fill = Brushes.DeepSkyBlue, Opacity = 0.9, IsHitTestVisible = false };
                    Canvas.SetLeft(n.Dot, n.X - n.W / 2); Canvas.SetTop(n.Dot, n.Y - n.H / 2);
                    n.Dot.Visibility = Visibility.Collapsed;
                    n.TrailDots = new List<Ellipse>();
                    for (int i = 0; i < 12; i++)
                    {
                        var e = new Ellipse { Width = n.W * 0.7, Height = n.H * 0.7, Fill = Brushes.DeepSkyBlue, Opacity = 0.0, IsHitTestVisible = false };
                        PadCanvas.Children.Add(e); n.TrailDots.Add(e);
                    }
                    PadCanvas.Children.Add(n.Dot);
                    break;

                case NodeKind.Basic3d:
                    // The theme's tilt indicator: a second, small pad drawn below
                    // the controller and leaned by the accelerometer. In VSCView's
                    // overlay it is the only motion readout; here the Motion panel
                    // carries real graphs, and on screen the extra pad read as an
                    // unexplained teal silhouette. Not drawn. The node is still
                    // parsed so the layout walk stays faithful to the file.
                    n.Shadows = new System.Windows.Controls.Image[4];
                    break;
            }
        }

        private System.Windows.Controls.Image MakeImage(string sprite, double x, double y, double w, double h, bool center, TranslateTransform slide)
        {
            var bmp = Sprite(sprite);
            if (bmp == null) return null;
            if (w <= 0) w = bmp.PixelWidth;
            if (h <= 0) h = bmp.PixelHeight;
            var img = new System.Windows.Controls.Image { Source = bmp, Width = w, Height = h, Stretch = Stretch.Fill, IsHitTestVisible = false };
            RenderOptions.SetBitmapScalingMode(img, BitmapScalingMode.HighQuality);
            Canvas.SetLeft(img, center ? x - w / 2 : x);
            Canvas.SetTop(img, center ? y - h / 2 : y);
            if (slide != null) img.RenderTransform = slide;
            PadCanvas.Children.Add(img);
            return img;
        }

        // ── theme: paint ────────────────────────────────────────────────────
        private void PaintController()
        {
            foreach (var n in _nodes)
            {
                switch (n.Kind)
                {
                    case NodeKind.ShowHide:
                        {
                            bool on = _haveState && Eval(n.Input) > 0.5;
                            SetVisible(n, on);
                            break;
                        }
                    case NodeKind.Slider:
                        n.Slide.X = _haveState ? Eval(n.InputX) : 0;
                        n.Slide.Y = _haveState ? Eval(n.InputY) : 0;
                        break;

                    case NodeKind.TrailPad:
                        {
                            bool on = _haveState && Eval(n.Input) > 0.5;
                            var trail = n.Input != null && n.Input.Contains(":1:") ? _trail1 : _trail0;
                            if (on)
                            {
                                double dx = Eval(n.InputX), dy = Eval(n.InputY);
                                var p = new Point(n.X + dx, n.Y + dy);
                                Canvas.SetLeft(n.Dot, p.X - n.W / 2); Canvas.SetTop(n.Dot, p.Y - n.H / 2);
                                n.Dot.Visibility = Visibility.Visible;
                                trail.Add(p);
                                if (trail.Count > n.TrailDots.Count) trail.RemoveAt(0);
                            }
                            else
                            {
                                n.Dot.Visibility = Visibility.Collapsed;
                                if (trail.Count > 0) trail.RemoveAt(0);
                            }
                            for (int i = 0; i < n.TrailDots.Count; i++)
                            {
                                var d = n.TrailDots[i];
                                if (i < trail.Count)
                                {
                                    var p = trail[i];
                                    Canvas.SetLeft(d, p.X - d.Width / 2); Canvas.SetTop(d, p.Y - d.Height / 2);
                                    d.Opacity = 0.05 + 0.4 * (i + 1) / trail.Count;
                                }
                                else d.Opacity = 0;
                            }
                            break;
                        }
                    case NodeKind.Basic3d:
                        {
                            // Tilt as shadow opacity.  INFERRED: VSCView's exact
                            // mapping was not read; this reproduces the visible
                            // effect -- the pad appears to lean the way it is held.
                            float ax = _haveState ? _state.AccelX : 0, ay = _haveState ? _state.AccelY : 0;
                            if (n.Shadows[0] != null) n.Shadows[0].Opacity = Clamp01(-ax);
                            if (n.Shadows[1] != null) n.Shadows[1].Opacity = Clamp01(ax);
                            if (n.Shadows[2] != null) n.Shadows[2].Opacity = Clamp01(ay);
                            if (n.Shadows[3] != null) n.Shadows[3].Opacity = Clamp01(-ay);
                            break;
                        }
                }
            }
        }

        private static void SetVisible(ThemeNode n, bool on)
        {
            var v = on ? Visibility.Visible : Visibility.Collapsed;
            if (n.Img != null) n.Img.Visibility = v;
            foreach (var c in n.Children) SetVisible(c, on);
        }

        private static double Clamp01(double v) => v < 0 ? 0 : v > 1 ? 1 : v;

        // ── theme: expression evaluator ─────────────────────────────────────
        // Grammar actually used by the theme:
        //   ident            e.g. quad_right:s, stick_left:x, triggers:l:analog
        //   number
        //   a * b, a + b, a - b, a > b
        //   a OR b, a AND b
        //   If(cond, a, b)
        //   ( ... )
        // Booleans are 1/0.  Unknown identifiers evaluate to 0, so a term the
        // pad does not report (touch_left:*) drops out instead of throwing.
        private double Eval(string expr)
        {
            if (string.IsNullOrWhiteSpace(expr)) return 0;
            var p = new Parser(expr, this);
            return p.ParseOr();
        }

        private double Input(string id)
        {
            if (!_haveState) return 0;
            var s = _state;
            switch (id)
            {
                case "quad_right:n": return Bit(s.Buttons, 0x1000);   // triangle
                case "quad_right:e": return Bit(s.Buttons, 0x2000);   // circle
                case "quad_right:s": return Bit(s.Buttons, 0x4000);   // cross
                case "quad_right:w": return Bit(s.Buttons, 0x8000);   // square
                case "quad_left:n":  return Bit(s.Buttons, 0x10);
                case "quad_left:e":  return Bit(s.Buttons, 0x20);
                case "quad_left:s":  return Bit(s.Buttons, 0x40);
                case "quad_left:w":  return Bit(s.Buttons, 0x80);
                case "bumpers:l":    return Bit(s.Buttons, 0x400);
                case "bumpers:r":    return Bit(s.Buttons, 0x800);
                case "triggers:l:stage2": return Bit(s.Buttons, 0x100);
                case "triggers:r:stage2": return Bit(s.Buttons, 0x200);
                case "triggers:l:analog": return s.L2 / 255.0;
                case "triggers:r:analog": return s.R2 / 255.0;
                case "menu:l":       return Bit(s.Buttons, 0x1);       // create
                case "menu:r":       return Bit(s.Buttons, 0x8);       // options
                case "home":         return Bit(s.Buttons, 0x10000);
                case "mute":         return s.MicMuted;
                case "stick_left:click":  return Bit(s.Buttons, 0x2);
                case "stick_right:click": return Bit(s.Buttons, 0x4);
                case "stick_left:x":  return (s.Lx - 128) / 127.0;
                case "stick_left:y":  return (s.Ly - 128) / 127.0;
                case "stick_right:x": return (s.Rx - 128) / 127.0;
                case "stick_right:y": return (s.Ry - 128) / 127.0;
                case "touch_center:click":
                case "touch_right:click":
                case "touch_left:click": return Bit(s.Buttons, 0x100000);
                case "touch_center:0:touch": return s.Touch0.Active;
                case "touch_center:1:touch": return s.Touch1.Active;
                case "touch_center:0:x": return s.Touch0.X / 1919.0 * 2 - 1;
                case "touch_center:0:y": return s.Touch0.Y / 941.0 * 2 - 1;
                case "touch_center:1:x": return s.Touch1.X / 1919.0 * 2 - 1;
                case "touch_center:1:y": return s.Touch1.Y / 941.0 * 2 - 1;
                default: return 0;   // touch_left:*, touch_right:* and anything unmodelled
            }
        }

        private static double Bit(uint v, uint mask) => (v & mask) != 0 ? 1 : 0;

        private sealed class Parser
        {
            private readonly string _s; private int _i; private readonly InputTabView _v;
            public Parser(string s, InputTabView v) { _s = s; _v = v; }

            private void Ws() { while (_i < _s.Length && char.IsWhiteSpace(_s[_i])) _i++; }
            private bool Take(string tok)
            {
                Ws();
                if (string.Compare(_s, _i, tok, 0, tok.Length, StringComparison.OrdinalIgnoreCase) == 0)
                {
                    // Word tokens must not be a prefix of an identifier.
                    if (char.IsLetter(tok[0]) && _i + tok.Length < _s.Length && (char.IsLetterOrDigit(_s[_i + tok.Length]) || _s[_i + tok.Length] == '_' || _s[_i + tok.Length] == ':')) return false;
                    _i += tok.Length; return true;
                }
                return false;
            }

            public double ParseOr()
            {
                double a = ParseAnd();
                while (Take("OR")) { double b = ParseAnd(); a = (a > 0.5 || b > 0.5) ? 1 : 0; }
                return a;
            }
            private double ParseAnd()
            {
                double a = ParseCmp();
                while (Take("AND")) { double b = ParseCmp(); a = (a > 0.5 && b > 0.5) ? 1 : 0; }
                return a;
            }
            private double ParseCmp()
            {
                double a = ParseAdd();
                for (; ; )
                {
                    if (Take(">=")) a = a >= ParseAdd() ? 1 : 0;
                    else if (Take("<=")) a = a <= ParseAdd() ? 1 : 0;
                    else if (Take(">")) a = a > ParseAdd() ? 1 : 0;
                    else if (Take("<")) a = a < ParseAdd() ? 1 : 0;
                    else return a;
                }
            }
            private double ParseAdd()
            {
                double a = ParseMul();
                for (; ; )
                {
                    if (Take("+")) a += ParseMul();
                    else if (Take("-")) a -= ParseMul();
                    else return a;
                }
            }
            private double ParseMul()
            {
                double a = ParseUnary();
                for (; ; )
                {
                    if (Take("*")) a *= ParseUnary();
                    else if (Take("/")) { double b = ParseUnary(); a = b == 0 ? 0 : a / b; }
                    else return a;
                }
            }
            private double ParseUnary()
            {
                if (Take("-")) return -ParseUnary();
                return ParsePrimary();
            }
            private double ParsePrimary()
            {
                Ws();
                if (Take("("))
                {
                    double v = ParseOr(); Take(")"); return v;
                }
                if (Take("If"))
                {
                    Take("("); double c = ParseOr(); Take(","); double a = ParseOr(); Take(","); double b = ParseOr(); Take(")");
                    return c > 0.5 ? a : b;
                }
                int start = _i;
                if (_i < _s.Length && (char.IsDigit(_s[_i]) || _s[_i] == '.'))
                {
                    while (_i < _s.Length && (char.IsDigit(_s[_i]) || _s[_i] == '.')) _i++;
                    return double.Parse(_s.Substring(start, _i - start), CultureInfo.InvariantCulture);
                }
                while (_i < _s.Length && (char.IsLetterOrDigit(_s[_i]) || _s[_i] == '_' || _s[_i] == ':')) _i++;
                if (_i == start) { _i++; return 0; }   // skip an unexpected character rather than loop
                return _v.Input(_s.Substring(start, _i - start));
            }
        }

        // ── graphs ──────────────────────────────────────────────────────────
        // The core hands over the sensor values exactly as DualSenseWindows
        // reports them: raw integer counts, not g or rad/s. Their scale is not
        // established here, so the graphs autoscale to the largest magnitude
        // seen in the window rather than pretend a unit. The first version
        // divided by an assumed "full scale" of 2 g / 8 rad/s and clamped, which
        // turned the small noise on a near-zero axis into a full-height square
        // wave -- a graph that looked alive and meant nothing.
        private double _accelScale = 1, _gyroScale = 1;

        private void PaintGraphs()
        {
            _accelScale = DrawGraph(AccelGraph, _accelHist, _accelScale);
            _gyroScale = DrawGraph(GyroGraph, _gyroHist, _gyroScale);
        }

        private static readonly Brush[] AxisBrushes = { Brushes.OrangeRed, Brushes.LimeGreen, Brushes.DeepSkyBlue };

        /// <summary>Draws three axes over the history window. Returns the scale
        /// used, which decays slowly so a burst of motion does not leave the
        /// graph flattened forever afterwards.</summary>
        private double DrawGraph(Canvas canvas, float[,] hist, double prevScale)
        {
            canvas.Children.Clear();
            double w = canvas.ActualWidth, h = canvas.ActualHeight;
            if (w < 10 || h < 10) return prevScale;

            double peak = 1;
            for (int axis = 0; axis < 3; axis++)
                for (int i = 0; i < HistoryLen; i++)
                    peak = Math.Max(peak, Math.Abs(hist[axis, i]));
            // Rise immediately, fall over ~2 s so the trace stays readable.
            double scale = peak > prevScale ? peak : prevScale * 0.985 + peak * 0.015;
            if (scale < 1) scale = 1;

            canvas.Children.Add(new Line { X1 = 0, X2 = w, Y1 = h / 2, Y2 = h / 2, Stroke = Brushes.Gray, StrokeThickness = 0.5, Opacity = 0.6 });
            for (int axis = 0; axis < 3; axis++)
            {
                var pl = new Polyline { Stroke = AxisBrushes[axis], StrokeThickness = 1.2, IsHitTestVisible = false };
                for (int i = 0; i < HistoryLen; i++)
                {
                    int idx = (_histPos + i) % HistoryLen;
                    double v = hist[axis, idx] / scale;
                    pl.Points.Add(new Point(w * i / (HistoryLen - 1), h / 2 - v * (h / 2 - 2)));
                }
                canvas.Children.Add(pl);
            }
            return scale;
        }
    }
}
