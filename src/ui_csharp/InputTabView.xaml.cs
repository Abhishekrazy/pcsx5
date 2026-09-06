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
    /// Rendering is the 3D DualSense in
    /// assets/gamepad/dualsense3d (one OBJ per part, see its README).
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

        // ── 3D pad assets ───────────────────────────────────────────────────
        private string _assetDir;   // assets/gamepad/dualsense3d
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

        private void LogFallback(string msg) { try { TestStatusText.Text = msg; } catch { } }

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
                try { Pad3D.Load(_assetDir); }
                catch (Exception ex) { LogFallback("3D pad failed to load: " + ex.Message); }
                if (!Pad3D.IsLoaded3D) TestStatusText.Text = I18n.Tr("input.assets_missing");
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
                string candidate = System.IO.Path.Combine(dir, "assets", "gamepad", "dualsense3d");
                if (File.Exists(System.IO.Path.Combine(candidate, "manifest.json"))) return candidate;
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
            Pad3D.SetPlayerIndex(_padIndex + 1);
            Pad3D.Update(ref _state, _haveState);
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
            // The core hands over the RAW int16 accelerometer; normalize it to g by the
            // pad's own resting magnitude (learnt slowly) so the graph reads in g on a
            // fixed scale instead of auto-zooming into sensor noise.
            double am = Math.Sqrt((double)_state.AccelX * _state.AccelX + (double)_state.AccelY * _state.AccelY + (double)_state.AccelZ * _state.AccelZ);
            if (am > 1) { if (_gRest <= 0) _gRest = am; else _gRest += (am - _gRest) * 0.02; }
            float gn = (float)(_gRest > 0 ? 1.0 / _gRest : 0);
            _accelHist[0, _histPos] = _state.AccelX * gn; _accelHist[1, _histPos] = _state.AccelY * gn; _accelHist[2, _histPos] = _state.AccelZ * gn;
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

        // ── graphs ──────────────────────────────────────────────────────────
        // The core hands over the sensor values exactly as DualSenseWindows
        // reports them: raw integer counts, not g or rad/s. Their scale is not
        // established here, so the graphs autoscale to the largest magnitude
        // seen in the window rather than pretend a unit. The first version
        // divided by an assumed "full scale" of 2 g / 8 rad/s and clamped, which
        // turned the small noise on a near-zero axis into a full-height square
        // wave -- a graph that looked alive and meant nothing.
        private double _accelScale = 1, _gyroScale = 1;
        private double _gRest;   // accelerometer magnitude at rest (= 1 g), raw units

        private void PaintGraphs()
        {
            _accelScale = DrawGraph(AccelGraph, _accelHist, _accelScale, 2.0);   // fixed +/-2 g
            _gyroScale = DrawGraph(GyroGraph, _gyroHist, _gyroScale);
        }

        private static readonly Brush[] AxisBrushes = { Brushes.OrangeRed, Brushes.LimeGreen, Brushes.DeepSkyBlue };

        /// <summary>Draws three axes over the history window. Returns the scale
        /// used, which decays slowly so a burst of motion does not leave the
        /// graph flattened forever afterwards.</summary>
        private double DrawGraph(Canvas canvas, float[,] hist, double prevScale, double fixedScale = 0)
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
            if (fixedScale > 0) scale = fixedScale;

            canvas.Children.Add(new Line { X1 = 0, X2 = w, Y1 = h / 2, Y2 = h / 2, Stroke = (Brush)Application.Current.Resources["ThemeHairline"], StrokeThickness = 0.5, Opacity = 0.6 });
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
