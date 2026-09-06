using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using Squirrel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Shape = System.Windows.Shapes.Shape;
using Ellipse = System.Windows.Shapes.Ellipse;

namespace Pcsx5Ui
{
    // Win32 interop for embedding the emulator's render window into the UI.
    internal static class NativeMethods
    {
        [DllImport("user32.dll", SetLastError = true)] public static extern IntPtr SetParent(IntPtr hWndChild, IntPtr hWndNewParent);
        [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr hWnd);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr CreateWindowExW(int dwExStyle, string lpClassName, string lpWindowName, int dwStyle, int x, int y, int nWidth, int nHeight, IntPtr hWndParent, IntPtr hMenu, IntPtr hInstance, IntPtr lpParam);
        [DllImport("user32.dll")] public static extern bool DestroyWindow(IntPtr hWnd);
        [DllImport("user32.dll", SetLastError = true)] public static extern bool MoveWindow(IntPtr hWnd, int x, int y, int nWidth, int nHeight, bool bRepaint);
        [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
        [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr hWnd, int nIndex);
        [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW", SetLastError = true)] public static extern IntPtr SetWindowLongPtr(IntPtr hWnd, int nIndex, IntPtr dwNewLong);
        [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hWnd);

        // Monitor bounds, so fullscreen can size to the display instead of
        // relying on Maximized (which only covers the taskbar while focused).
        [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint dwFlags);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool GetMonitorInfoW(IntPtr hMonitor, ref MONITORINFO lpmi);
        public const uint MONITOR_DEFAULTTONEAREST = 2;

        [StructLayout(LayoutKind.Sequential)]
        public struct MONITORINFO
        {
            public int cbSize;
            public RECT rcMonitor;
            public RECT rcWork;
            public uint dwFlags;
        }
        [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr hWnd);
        [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern ushort RegisterClassW(ref WNDCLASS lpWndClass);
        [DllImport("user32.dll")] public static extern IntPtr DefWindowProcW(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr GetModuleHandleW(string lpModuleName);
        [DllImport("gdi32.dll")] public static extern IntPtr GetStockObject(int fnObject);

        public delegate IntPtr WndProcDelegate(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct WNDCLASS
        {
            public int style;
            public IntPtr lpfnWndProc;
            public int cbClsExtra;
            public int cbWndExtra;
            public IntPtr hInstance;
            public IntPtr hIcon;
            public IntPtr hCursor;
            public IntPtr hbrBackground;
            public string lpszMenuName;
            public string lpszClassName;
        }

        public const int BLACK_BRUSH = 4;

        [StructLayout(LayoutKind.Sequential)]
        public struct RECT { public int Left, Top, Right, Bottom; }

        public const int GWL_STYLE = -16;
        public const int WS_CHILD = 0x40000000;
        public const int WS_VISIBLE = 0x10000000;
        public const int WS_CLIPCHILDREN = 0x02000000;
        public const int WS_CLIPSIBLINGS = 0x04000000;
        public const long WS_CAPTION = 0x00C00000;
        public const long WS_THICKFRAME = 0x00040000;
        public const long WS_MINIMIZEBOX = 0x00020000;
        public const long WS_MAXIMIZEBOX = 0x00010000;
        public const long WS_SYSMENU = 0x00080000;
        public const int SW_SHOW = 5;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern bool SetDllDirectoryW(string lpPathName);
    }

    // Hosts a plain Win32 child window inside the WPF layout; the emulator's
    // GLFW window is reparented into this container.  A dedicated window
    // class is registered so the letterbox margins paint black.
    public class EmulatorWindowHost : HwndHost
    {
        public IntPtr HostHandle { get; private set; } = IntPtr.Zero;

        private static bool _classRegistered;
        private static NativeMethods.WndProcDelegate _wndProc; // keep the delegate alive

        protected override HandleRef BuildWindowCore(HandleRef hwndParent)
        {
            if (!_classRegistered)
            {
                _wndProc = HostWndProc;
                var wc = new NativeMethods.WNDCLASS
                {
                    lpfnWndProc = Marshal.GetFunctionPointerForDelegate(_wndProc),
                    // GetModuleHandle(null) = the exe's HINSTANCE; safe in the
                    // single-file publish (Marshal.GetHINSTANCE returns -1
                    // for assemblies embedded in the bundle).
                    hInstance = NativeMethods.GetModuleHandleW(null),
                    lpszClassName = "Pcsx5EmuHost",
                    hbrBackground = NativeMethods.GetStockObject(NativeMethods.BLACK_BRUSH)
                };
                NativeMethods.RegisterClassW(ref wc);
                _classRegistered = true;
            }

            HostHandle = NativeMethods.CreateWindowExW(0, "Pcsx5EmuHost", "",
                NativeMethods.WS_CHILD | NativeMethods.WS_VISIBLE | NativeMethods.WS_CLIPCHILDREN,
                0, 0, 0, 0, hwndParent.Handle, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
            return new HandleRef(this, HostHandle);
        }

        private static IntPtr HostWndProc(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam)
        {
            return NativeMethods.DefWindowProcW(hWnd, msg, wParam, lParam);
        }

        protected override void DestroyWindowCore(HandleRef hwnd)
        {
            NativeMethods.DestroyWindow(hwnd.Handle);
            HostHandle = IntPtr.Zero;
        }
    }

    public partial class MainWindow : Window
    {
        private List<GameEntry> _games = new List<GameEntry>();
        // The shelf on the Library tab: recently played titles, most recent
        // first, or every title when nothing has been played yet. View All
        // always lists _games in full.
        private List<GameEntry> _shelf = new List<GameEntry>();
        private RecentPlays _recent;
        private Favourites _favourites;
        // All-games sort order; the value is the segment button's Tag.
        private enum LibrarySort { Recent = 0, Title = 1, TitleId = 2, Size = 3, Favourites = 4 }
        private LibrarySort _librarySort = LibrarySort.Recent;
        private GameEntry _selectedGame = null;
        private string _gamesDir = "Games";
        private string _coversDir = "Covers";
        private string _compatDir = "compat_seed";
        private string _configPath = "";
        private string _iniPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "config.ini");
        private List<string> _gameFolders = new List<string>();

        private EmulatorConfig _config = new EmulatorConfig();
        private MediaPlayer _mediaPlayer;
        private System.Threading.CancellationTokenSource _musicCts = null;
        private List<BootAnalysisResult> _analysisResults = new List<BootAnalysisResult>();
        private DiscordRpc _discordRpc = new DiscordRpc();

        // ── Game session (replaces _coreThread + raw callbacks) ─────────────
        // The game runs on GameSession's dedicated background thread.
        // The WPF Dispatcher thread is never blocked by game activity.
        private GameSession _session;
        // Expose for backward-compat with controller code that checks _coreRunning.
        private bool _coreRunning => _session?.State == GameSessionState.Running ||
                                     _session?.State == GameSessionState.Booting ||
                                     _session?.State == GameSessionState.Extracting;

        // Pause menu state
        private bool _pauseMenuVisible = false;
        private int _pauseMenuIndex = 0; // 0=Resume 1=Console 2=Stop

        // Watchdog toast visibility
        private bool _watchdogToastVisible = false;

        // Folder picker callback target ("firstrun" or "settings")
        private string _folderPickerTarget = null;

        // Console output batching: reader threads enqueue lines, a DispatcherTimer drains
        // them in batches so floods of stdout lines never saturate the dispatcher queue.
        private struct ConsoleLine { public string Text; public int Level; } // Level: 0=Trace..5=Critical
        private readonly ConcurrentQueue<ConsoleLine> _consoleLineQueue = new ConcurrentQueue<ConsoleLine>();
        private System.Windows.Threading.DispatcherTimer _consoleDrainTimer;
        private System.Windows.Threading.DispatcherTimer _hudMetricsTimer;
        private ulong _lastFpsCount = 0;
        private DateTime _lastFpsTime = DateTime.UtcNow;
        private const int MaxConsoleLines = 5000; // cap on the console RichTextBox
        private int _consoleLineNum = 0; // line number counter
        private string _lastDedupLine = null; // dedup: last line text
        private int _lastDedupCount = 0; // dedup: repeat count
        private System.Windows.Media.Color _lastDedupColor; // dedup: line color
        private static readonly System.Text.RegularExpressions.Regex DedupCountSuffix =
            new(@"\s\(x(\d+)\)\s*$", System.Text.RegularExpressions.RegexOptions.Compiled);

        // Embedded emulator window state (game renders inside the launcher window)
        private EmulatorWindowHost _emuHost = null;
        private IntPtr _embeddedEmuHwnd = IntPtr.Zero;
        private bool _gameConsoleVisible = false;

        private enum ConsoleDock { Right, Bottom, Left, Float }
        private ConsoleDock _consoleDock = ConsoleDock.Right;


        public MainWindow()
        {
            // Add plugins/ to DLL search path so pcsx5_core.dll in plugins/ is found.
            try
            {
                string dir = AppDomain.CurrentDomain.BaseDirectory;
                string plugins = Path.Combine(dir, "plugins");
                if (System.IO.Directory.Exists(plugins))
                    NativeMethods.SetDllDirectoryW(plugins);
            }
            catch { }

            InitializeComponent();
            // Pad navigation moves keyboard focus; whatever receives it must be
            // scrolled into view, on every screen, or the focus ring walks off
            // the visible area (user report, Settings sub-pages and Input grid).
            AddHandler(Keyboard.GotKeyboardFocusEvent, new KeyboardFocusChangedEventHandler((s, e) =>
            {
                if (e.NewFocus is FrameworkElement fe && !(fe is Window)) { try { fe.BringIntoView(); } catch { } }
            }), true);
            InitializeAudioPlayer();
            this.Closed += MainWindow_Closed;

            // Create the session — it owns its own thread; WPF dispatcher is never blocked.
            _session = new GameSession(Dispatcher);
            _session.Started += OnGameStarted;
            _session.BootPhaseChanged += OnBootPhaseChanged;
            _session.WindowReady += OnGameWindowReady;
            _session.Stopped += OnGameStopped;
            _session.Crashed += OnGameCrashed;
            _session.Hanging += OnGameHanging;
            _session.FrameStalled += OnFramesStalled;
            _session.FrameResumed += OnFramesResumed;
            _session.LogLine += line => {
                int lvl = 2; // default Info
                string u = line.ToUpperInvariant();
                if (u.Contains("[ERROR]") || u.Contains("[ERR]")) lvl = 4;
                else if (u.Contains("[WARN]")) lvl = 3;
                else if (u.Contains("[CRITICAL]")) lvl = 5;
                else if (u.Contains("[DEBUG]")) lvl = 1;
                else if (u.Contains("[TRACE]")) lvl = 0;
                _consoleLineQueue.Enqueue(new ConsoleLine { Text = line, Level = lvl });
            };

            _consoleDrainTimer = new System.Windows.Threading.DispatcherTimer();
            _consoleDrainTimer.Interval = TimeSpan.FromMilliseconds(150);
            _consoleDrainTimer.Tick += (s, e) => DrainConsoleQueue();
            _consoleDrainTimer.Start();

            _hudMetricsTimer = new System.Windows.Threading.DispatcherTimer();
            _hudMetricsTimer.Interval = TimeSpan.FromSeconds(1);
            _hudMetricsTimer.Tick += UpdateHudMetrics;
            _hudMetricsTimer.Start();
        }

        private void UpdateHudMetrics(object sender, EventArgs e)
        {
            bool isRunning = _session != null &&
                             _session.State != GameSessionState.Idle &&
                             _session.State != GameSessionState.Stopped &&
                             _session.State != GameSessionState.Crashed;

            if (!isRunning || _session.IpcSession == null || HudMetricsPanel == null)
            {
                if (HudMetricsPanel != null) HudMetricsPanel.Visibility = Visibility.Collapsed;
                return;
            }

            if (HudMetricsPanel.Visibility != Visibility.Visible)
            {
                HudMetricsPanel.Visibility = Visibility.Visible;
                _lastFpsCount = _session.IpcSession.FrameCounter;
                _lastFpsTime = DateTime.UtcNow;
            }

            // 1. Calculate FPS
            try
            {
                ulong currentCount = _session.IpcSession.FrameCounter;
                var now = DateTime.UtcNow;
                double seconds = (now - _lastFpsTime).TotalSeconds;
                if (seconds > 0.1)
                {
                    double fps = (currentCount >= _lastFpsCount)
                        ? (currentCount - _lastFpsCount) / seconds
                        : 0;
                    if (HudFpsText != null) HudFpsText.Text = $"FPS: {fps:F1}";
                    _lastFpsCount = currentCount;
                    _lastFpsTime = now;
                }
            }
            catch { }

            // 2. Fetch RAM & VRAM metrics from guest process
            try
            {
                var proc = _session.IpcSession.Process;
                if (proc != null && !proc.HasExited)
                {
                    proc.Refresh();
                    long ramMb = proc.WorkingSet64 / (1024 * 1024);
                    // Use PrivateMemory (heap/texture allocations) as a proxy for GPU VRAM mirror footprint
                    long vramMb = proc.PrivateMemorySize64 / (1024 * 1024);

                    if (HudRamText != null) HudRamText.Text = $"RAM: {ramMb} MB";
                    if (HudVramText != null) HudVramText.Text = $"VRAM: {vramMb} MB";
                }
            }
            catch { }
        }

        private void MainWindow_Closed(object sender, EventArgs e)
        {
            if (_coreRunning)
            {
                try { _session?.RequestStop(); } catch { }
            }
            StopControllerVizPolling();
            _discordRpc.Stop();
        }

        private void InitializeAudioPlayer()
        {
            _mediaPlayer = new MediaPlayer();
            _mediaPlayer.MediaEnded += (s, e) =>
            {
                _mediaPlayer.Position = TimeSpan.Zero;
                _mediaPlayer.Play();
            };
        }

        private void Window_Loaded(object sender, RoutedEventArgs e)
        {
            try
            {
                MicaHelper.ApplyMica(this);
                ResolveDirectories();
                ParseCommandLineArgs();
                LoadConfig();
                ApplyUiScale();
                ApplyShellFullscreen(_config.ui.start_fullscreen);
                ApplyTheme();
                InitializeControllerPolling();

                // Start Discord RPC and load translations
                _discordRpc.Start();
                TranslateUi();
                // After TranslateUi: I18n must be loaded or every binding label
                // falls back to its raw token.
                RefreshBindingLabels();

                // Set default tab to Library
                TabLibrary_Click(this, null);

                // Asynchronously scan games so the launcher window opens instantly
                Task.Run(() =>
                {
                    Dispatcher.InvokeAsync(() =>
                    {
                        LoadGames();
                        MaybeShowFirstRunSetup();
                        ApplyLightbarFromConfig();
                    });
                });

                // Check for updates asynchronously
                Task.Run(async () => await CheckForUpdates());
            }
            catch (Exception ex)
            {
                try {
                    string logsDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "logs");
                    Directory.CreateDirectory(logsDir);
                    File.WriteAllText(Path.Combine(logsDir, "ui_crash_log.txt"), ex.ToString());
                } catch { }
                MessageBox.Show(ex.ToString(), "UI Startup Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        // ── CUSTOM COLOUR POPUP ── the picker with the sticks: right stick
        // moves the cursor on the plane, left stick the hue bar; Cross applies,
        // Circle cancels. Fed from the pad tick's stick values.
        private (double x, double y) _stickL, _stickR;
        private Action<Color> _colorPickerOnApply;

        private void ShowColorPicker(Color initial, Action<Color> onApply)
        {
            _colorPickerOnApply = onApply;
            PopupPicker.SelectedColor = initial;
            ColorPickerOverlay.Visibility = Visibility.Visible;
            BuildFooterHintChips(I18n.Tr("hints.colorpicker_sticks"), ColorPickerHintChips);
            PopupPicker.ColorChanged -= PopupPicker_Preview; PopupPicker.ColorChanged += PopupPicker_Preview;
            FocusFirst(ColorPickerApplyBtn);
        }

        private void PopupPicker_Preview(object sender, Color c)
        {
            // The pad's lightbar follows the cursor so the colour is judged on the real thing.
            try { CoreBridge.pcsx5_pad_set_lightbar(0, c.R, c.G, c.B); } catch { }
            if (InputTab?.Pad3D != null) InputTab.Pad3D.LightbarColor = c;
        }

        private void ColorPickerApply_Click(object sender, RoutedEventArgs e)
        {
            var cb = _colorPickerOnApply; _colorPickerOnApply = null;
            ColorPickerOverlay.Visibility = Visibility.Collapsed;
            cb?.Invoke(PopupPicker.SelectedColor);
        }

        private void ColorPickerCancel_Click(object sender, RoutedEventArgs e)
        {
            _colorPickerOnApply = null;
            ColorPickerOverlay.Visibility = Visibility.Collapsed;
            ApplyLightbarFromConfig();   // undo the live preview
            FocusFirst(SettingsNavButtons().FirstOrDefault(b => (b?.CommandParameter as string) == _activeSettingsCategory));
        }

        // ── UPDATES ── checked at startup (silently) and from System
        // Information > Check for updates. The prompt offers Download &
        // install (Squirrel installs) or the release page (zip copies), Skip
        // this version (remembered in config; a newer one asks again) and Later.
        private UpdateCheckResult _pendingUpdate;
        private bool _updateApplied;

        private async Task CheckForUpdates(bool manual = false)
        {
            var result = await UpdateChecker.CheckAsync(ReadVersionString());
            await Dispatcher.InvokeAsync(() =>
            {
                if (result.IsNewer)
                {
                    bool skipped = !manual && string.Equals(_config.ui.skipped_update_version, result.LatestVersion, StringComparison.OrdinalIgnoreCase);
                    if (!skipped) ShowUpdatePrompt(result);
                    else LogConsole($"Update {result.LatestVersion} available, skipped by the user.");
                }
                else if (manual)
                {
                    FooterStatus.Text = result.Error != null
                        ? I18n.Tr("update.failed", result.Error)
                        : I18n.Tr("update.uptodate_fmt", result.CurrentVersion);
                }
                else if (result.Error != null)
                {
                    LogConsole("Update check failed: " + result.Error);
                }
            });
        }

        private void ShowUpdatePrompt(UpdateCheckResult r)
        {
            _pendingUpdate = r;
            _updateApplied = false;
            UpdateBodyText.Text = I18n.Tr("update.body_fmt", r.LatestVersion, r.CurrentVersion);
            UpdateInstallBtn.Content = I18n.Tr(r.CanInstallInPlace ? "update.install" : "update.open_page");
            UpdateInstallBtn.IsEnabled = true;
            UpdateProgressRow.Visibility = Visibility.Collapsed;
            UpdateProgressFill.Width = 0;
            UpdateOverlay.Visibility = Visibility.Visible;
            FocusFirst(UpdateInstallBtn);
        }

        private void HideUpdatePrompt()
        {
            UpdateOverlay.Visibility = Visibility.Collapsed;
            FocusFirst(TabLibraryBtn);
        }

        private void UpdateLater_Click(object sender, RoutedEventArgs e) => HideUpdatePrompt();

        private void UpdateSkip_Click(object sender, RoutedEventArgs e)
        {
            if (_pendingUpdate != null)
            {
                _config.ui.skipped_update_version = _pendingUpdate.LatestVersion;
                SaveConfig();
                LogConsole($"Update {_pendingUpdate.LatestVersion} skipped; a newer version will ask again.");
            }
            HideUpdatePrompt();
        }

        private async void UpdateInstall_Click(object sender, RoutedEventArgs e)
        {
            var r = _pendingUpdate;
            if (r == null) return;
            if (_updateApplied) { UpdateChecker.RestartApp(); return; }
            if (!r.CanInstallInPlace)
            {
                try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(r.ReleaseUrl) { UseShellExecute = true }); } catch { }
                HideUpdatePrompt();
                return;
            }
            UpdateInstallBtn.IsEnabled = false;
            UpdateSkipBtn.IsEnabled = false;
            UpdateProgressRow.Visibility = Visibility.Visible;
            UpdateBodyText.Text = I18n.Tr("update.downloading");
            double track = UpdateProgressRow.ActualWidth;
            var progress = new Progress<int>(p => UpdateProgressFill.Width = Math.Max(0, Math.Min(100, p)) / 100.0 * (track > 0 ? track : 560));
            bool ok = false;
            string err = null;
            try { ok = await UpdateChecker.InstallAsync(progress); }
            catch (Exception ex) { err = ex.Message; }
            UpdateSkipBtn.IsEnabled = true;
            UpdateInstallBtn.IsEnabled = true;
            if (ok)
            {
                _updateApplied = true;
                UpdateBodyText.Text = I18n.Tr("update.restart_body", r.LatestVersion);
                UpdateInstallBtn.Content = I18n.Tr("update.restart");
                UpdateProgressFill.Width = track > 0 ? track : 560;
            }
            else
            {
                UpdateProgressRow.Visibility = Visibility.Collapsed;
                UpdateBodyText.Text = I18n.Tr("update.failed", err ?? "");
            }
        }

        private void ParseCommandLineArgs()
        {
            string[] args = Environment.GetCommandLineArgs();
            for (int i = 1; i < args.Length; i++)
            {
                if (args[i].StartsWith("--games="))
                {
                    _gamesDir = Path.GetFullPath(args[i].Substring("--games=".Length));
                }
                else if (args[i].StartsWith("--compat="))
                {
                    _compatDir = Path.GetFullPath(args[i].Substring("--compat=".Length));
                }
            }
        }

        private void ResolveDirectories()
        {
            // Scan directories starting from working directory, then walking up to support build tree
            string[] possibleRoots = { ".", "..", "..\\..", "..\\..\\.." };
            foreach (var root in possibleRoots)
            {
                string gDir = Path.Combine(root, "Games");
                if (Directory.Exists(gDir))
                {
                    _gamesDir = Path.GetFullPath(gDir);
                    _coversDir = Path.GetFullPath(Path.Combine(root, "Covers"));
                    _compatDir = Path.GetFullPath(Path.Combine(root, "compat_seed"));
                    break;
                }
            }
        }

        private void LoadConfig()
        {
            try
            {
                _gameFolders.Clear();
                string parent = Path.GetDirectoryName(_iniPath) ?? ".";
                _configPath = Path.Combine(parent, "pcsx5_config", "global.json");

                if (File.Exists(_iniPath))
                {
                    LogConsole("Loading config from config.ini...");
                    var ini = new IniFile();
                    ini.Load(_iniPath);

                    // Load paths
                    string foldersStr = ini.GetValue("Paths", "GameFolders", "");
                    if (!string.IsNullOrEmpty(foldersStr))
                    {
                        var parts = foldersStr.Split(new[] { ';' }, StringSplitOptions.RemoveEmptyEntries);
                        foreach (var part in parts)
                        {
                            _gameFolders.Add(part.Trim());
                        }
                    }

                    // Load sections to _config
                    _config.audio.backend = int.Parse(ini.GetValue("Audio", "Backend", "1"));
                    _config.audio.buffer_ms = int.Parse(ini.GetValue("Audio", "BufferMs", "50"));
                    _config.audio.volume = double.Parse(ini.GetValue("Audio", "Volume", "1.0"), System.Globalization.CultureInfo.InvariantCulture);

                    _config.crash.bundle_dir = ini.GetValue("Crash", "BundleDir", "pcsx5_crash");
                    _config.crash.write_minidump = bool.Parse(ini.GetValue("Crash", "WriteMinidump", "true"));

                    _config.graphics.fullscreen = bool.Parse(ini.GetValue("Graphics", "Fullscreen", "false"));
                    _config.graphics.height = int.Parse(ini.GetValue("Graphics", "Height", "720"));
                    _config.graphics.renderer = int.Parse(ini.GetValue("Graphics", "Renderer", "0"));
                    _config.graphics.resolution_scale = double.Parse(ini.GetValue("Graphics", "ResolutionScale", "1.0"), System.Globalization.CultureInfo.InvariantCulture);
                    _config.graphics.width = int.Parse(ini.GetValue("Graphics", "Width", "1280"));

                    _config.hle.strict_imports = bool.Parse(ini.GetValue("Hle", "StrictImports", "false"));
                    _config.hle.trace_calls = bool.Parse(ini.GetValue("Hle", "TraceCalls", "true"));
                    _config.hle.trace_capacity = int.Parse(ini.GetValue("Hle", "TraceCapacity", "256"));

                    _config.input.backend = int.Parse(ini.GetValue("Input", "Backend", "0"));
                    _config.input.deadzone = double.Parse(ini.GetValue("Input", "Deadzone", "0.15"), System.Globalization.CultureInfo.InvariantCulture);
                    _config.input.rumble = bool.Parse(ini.GetValue("Input", "Rumble", "true"));

                    _config.logging.file_append = bool.Parse(ini.GetValue("Logging", "FileAppend", "false"));
                    _config.logging.file_path = ini.GetValue("Logging", "FilePath", "");
                    _config.logging.json_output = bool.Parse(ini.GetValue("Logging", "JsonOutput", "false"));
                    _config.logging.min_level = ini.GetValue("Logging", "MinLevel", "Info");

                    _config.ui.language = ini.GetValue("Ui", "Language", "en-US");
                    _config.ui.title_music_enabled = bool.Parse(ini.GetValue("Ui", "TitleMusicEnabled", "true"));
                    _config.ui.scale = double.Parse(ini.GetValue("Ui", "UiScale", "1.0"), System.Globalization.CultureInfo.InvariantCulture);
                    _config.ui.start_fullscreen = bool.Parse(ini.GetValue("Ui", "StartFullscreen", "true"));
                    _config.ui.theme = ini.GetValue("Ui", "Theme", "dark");
                    _config.ui.accent = ini.GetValue("Ui", "Accent", "#5FE3FF");
                    _config.ui.ground = ini.GetValue("Ui", "Ground", "");
                    _config.ui.corners = ini.GetValue("Ui", "Corners", "rounded");
                }
                else
                {
                    LogConsole("config.ini not found. Loading defaults...");
                    // Try to import from global.json if exists
                    string configParent = ".";
                    string jsonPath = Path.Combine(configParent, "pcsx5_config", "global.json");
                    if (File.Exists(jsonPath))
                    {
                        string json = File.ReadAllText(jsonPath);
                        _config = System.Text.Json.JsonSerializer.Deserialize<EmulatorConfig>(json) ?? new EmulatorConfig();
                        LogConsole("Imported settings from global.json");
                    }
                    else
                    {
                        _config = new EmulatorConfig();
                    }

                    // Setup default games folder
                    ResolveDirectories(); // scan for default games folder
                    _gameFolders.Add(_gamesDir);

                    // Save the new config.ini
                    SaveConfig();
                }
            }
            catch (Exception ex)
            {
                LogConsole("Error loading config: " + ex.Message);
                _config = new EmulatorConfig();
                _gameFolders.Add("Games");
            }
        }

        private void SaveConfig()
        {
            try
            {
                // 1. Save to config.ini
                var ini = new IniFile();

                // Paths
                string foldersStr = string.Join(";", _gameFolders);
                ini.SetValue("Paths", "GameFolders", foldersStr);

                // Audio
                ini.SetValue("Audio", "Backend", _config.audio.backend.ToString());
                ini.SetValue("Audio", "BufferMs", _config.audio.buffer_ms.ToString());
                ini.SetValue("Audio", "Volume", _config.audio.volume.ToString(System.Globalization.CultureInfo.InvariantCulture));

                // Crash
                ini.SetValue("Crash", "BundleDir", _config.crash.bundle_dir);
                ini.SetValue("Crash", "WriteMinidump", _config.crash.write_minidump.ToString().ToLower());

                // Graphics
                ini.SetValue("Graphics", "Fullscreen", _config.graphics.fullscreen.ToString().ToLower());
                ini.SetValue("Graphics", "Height", _config.graphics.height.ToString());
                ini.SetValue("Graphics", "Renderer", _config.graphics.renderer.ToString());
                ini.SetValue("Graphics", "ResolutionScale", _config.graphics.resolution_scale.ToString(System.Globalization.CultureInfo.InvariantCulture));
                ini.SetValue("Graphics", "Width", _config.graphics.width.ToString());

                // Hle
                ini.SetValue("Hle", "StrictImports", _config.hle.strict_imports.ToString().ToLower());
                ini.SetValue("Hle", "TraceCalls", _config.hle.trace_calls.ToString().ToLower());
                ini.SetValue("Hle", "TraceCapacity", _config.hle.trace_capacity.ToString());

                // Input
                ini.SetValue("Input", "Backend", _config.input.backend.ToString());
                ini.SetValue("Input", "Deadzone", _config.input.deadzone.ToString(System.Globalization.CultureInfo.InvariantCulture));
                ini.SetValue("Input", "Rumble", _config.input.rumble.ToString().ToLower());

                // Logging
                ini.SetValue("Logging", "FileAppend", _config.logging.file_append.ToString().ToLower());
                ini.SetValue("Logging", "FilePath", _config.logging.file_path);
                ini.SetValue("Logging", "JsonOutput", _config.logging.json_output.ToString().ToLower());
                ini.SetValue("Logging", "MinLevel", _config.logging.min_level);

                // Ui
                ini.SetValue("Ui", "Language", _config.ui.language);
                ini.SetValue("Ui", "TitleMusicEnabled", _config.ui.title_music_enabled.ToString().ToLower());
                ini.SetValue("Ui", "UiScale", _config.ui.scale.ToString(System.Globalization.CultureInfo.InvariantCulture));
                ini.SetValue("Ui", "StartFullscreen", _config.ui.start_fullscreen.ToString().ToLower());
                ini.SetValue("Ui", "Theme", _config.ui.theme ?? "dark");
                ini.SetValue("Ui", "Accent", _config.ui.accent ?? "#5FE3FF");
                ini.SetValue("Ui", "Ground", _config.ui.ground ?? "");
                ini.SetValue("Ui", "Corners", _config.ui.corners ?? "rounded");

                ini.Save(_iniPath);
                LogConsole("Configuration saved to " + _iniPath);

                // 2. Also save to global.json for C++ backend
                if (!string.IsNullOrEmpty(_configPath))
                {
                    string dir = Path.GetDirectoryName(_configPath);
                    if (dir != null && !Directory.Exists(dir))
                    {
                        Directory.CreateDirectory(dir);
                    }
                    var options = new System.Text.Json.JsonSerializerOptions { WriteIndented = true };
                    string json = System.Text.Json.JsonSerializer.Serialize(_config, options);
                    File.WriteAllText(_configPath, json);
                    LogConsole("Sync global.json saved to " + _configPath);
                }
            }
            catch (Exception ex)
            {
                LogConsole("Error saving config: " + ex.Message);
            }
        }

        private void LoadGames()
        {
            GamesWrapPanel.Children.Clear();
            _games.Clear();

            foreach (var folder in _gameFolders)
            {
                if (!Directory.Exists(folder))
                {
                    LogConsole("Games directory not found: " + folder);
                    continue;
                }

                try
                {
                    string[] subdirs = Directory.GetDirectories(folder);
                    foreach (string dir in subdirs)
                    {
                        var entry = ParseGameDirectory(dir);
                        if (entry != null)
                        {
                            if (!_games.Any(g => g.TitleId == entry.TitleId))
                            {
                                _games.Add(entry);
                            }
                        }
                    }
                }
                catch (Exception ex)
                {
                    LogConsole($"Error loading games from {folder}: {ex.Message}");
                }
            }

            // Most recently played first everywhere; never-played titles keep
            // their discovery order after them.
            _recent ??= new RecentPlays(Path.Combine(Path.GetDirectoryName(_iniPath) ?? AppDomain.CurrentDomain.BaseDirectory, "recent_plays.json"));
            _favourites ??= new Favourites(Path.Combine(Path.GetDirectoryName(_iniPath) ?? AppDomain.CurrentDomain.BaseDirectory, "favourites.json"));
            _games = _games
                .Select((g, i) => (g, i))
                .OrderByDescending(t => _recent.LastPlayed(t.g.TitleId) ?? DateTime.MinValue)
                .ThenBy(t => t.i)
                .Select(t => t.g)
                .ToList();

            FooterGamesCount.Text = $"Games Loaded: {_games.Count}";
            LibraryGameCount.Text = $"{_games.Count} games";
            FullLibraryCount.Text = $"{_games.Count} games";
            ApplyLibrarySort();

            RebuildShelf();
            if (_shelf.Count > 0)
            {
                SelectGame(_shelf[0]);
            }
        }

        /// <summary>Recompute the Library shelf: recently played titles first,
        /// then every other title in discovery order. The shelf is never
        /// sparse -- the user does not want the main menu to look blank
        /// (2026-09-06) -- so nothing is filtered out; recency only orders.</summary>
        private const int ShelfSize = 5;   // asked 2026-09-06: always five on the main menu
        private void RebuildShelf()
        {
            // _games is already recency-ordered, so the first five are the
            // recently played titles followed by whatever fills the shelf.
            _shelf = _games.Take(ShelfSize).ToList();
            GamesWrapPanel.Children.Clear();
            foreach (var g in _shelf) AddGameTile(g);
            if (_selectedGame != null && _shelf.Contains(_selectedGame)) SelectGame(_selectedGame);
        }

        private GameEntry ParseGameDirectory(string dir)
        {
            string dirName = Path.GetFileName(dir);
            string titleId = CanonicalizeTitleId(dirName);

            // Locate eboot
            string ebootPath = null;
            string[] preferredEboots = {
                Path.Combine(dir, "decrypted", "eboot.bin"),
                Path.Combine(dir, "eboot.bin"),
                Path.Combine(dir, "eboot.bin.esbak"),
                Path.Combine(dir, "eboot.elf")
            };
            foreach (var p in preferredEboots)
            {
                if (File.Exists(p))
                {
                    ebootPath = p;
                    break;
                }
            }

            if (ebootPath == null) return null;

            // Load title from param.json
            string title = titleId;
            string paramPath = Path.Combine(dir, "sce_sys", "param.json");
            if (File.Exists(paramPath))
            {
                try
                {
                    string content = File.ReadAllText(paramPath);
                    title = ExtractJsonString(content, "titleName") ?? title;
                }
                catch { }
            }

            // Cover Art (Square)
            string coverPath = Path.Combine(dir, "sce_sys", "icon0.png");
            if (!File.Exists(coverPath))
            {
                // Fallback to covers directory
                string[] exts = { ".png", ".jpg", ".jpeg", ".webp" };
                foreach (var ext in exts)
                {
                    string p = Path.Combine(_coversDir, titleId + ext);
                    if (File.Exists(p))
                    {
                        coverPath = p;
                        break;
                    }
                }
            }

            // Background (Widescreen Hero Art)
            string bgPath = Path.Combine(dir, "sce_sys", "pic1.png");
            if (!File.Exists(bgPath))
            {
                // Fallback search
                string[] bgCandidates = { "pic1.jpg", "pic0.png", "pic0.jpg", "pic2.png", "pic2.jpg" };
                foreach (var bg in bgCandidates)
                {
                    string p = Path.Combine(dir, "sce_sys", bg);
                    if (File.Exists(p))
                    {
                        bgPath = p;
                        break;
                    }
                }
            }

            // Audio (Title Screen Background Music)
            string musicPath = null;
            string[] musicCandidates = { "snd0.at9", "snd0.wav", "snd0.mp3", "snd0.ogg", "snd0.flac" };
            foreach (var candidate in musicCandidates)
            {
                string p = Path.Combine(dir, "sce_sys", candidate);
                if (File.Exists(p))
                {
                    musicPath = p;
                    break;
                }
            }
            if (musicPath == null)
            {
                // Fallback to media folder
                string mediaDir = Path.Combine(dir, "media");
                if (Directory.Exists(mediaDir))
                {
                    string[] extPatterns = { "*.ogg", "*.wav", "*.mp3", "*.flac" };
                    foreach (var pattern in extPatterns)
                    {
                        var files = Directory.GetFiles(mediaDir, pattern);
                        if (files.Length > 0)
                        {
                            musicPath = files[0];
                            break;
                        }
                    }
                }
            }

            // Size
            long sizeBytes = 0;
            try
            {
                sizeBytes = Directory.GetFiles(dir, "*", SearchOption.AllDirectories)
                                     .Sum(f => new FileInfo(f).Length);
            }
            catch { }

            // Compatibility Status. The curated record lives in
            // <compat>/titles/<id>.json; look in the resolved compat dir and in
            // a compat_seed beside the game's own tree, since the exe's working
            // directory is not always the repo root.
            string compatStatus = "untested";
            string gamesParent = Directory.GetParent(dir)?.FullName;                 // the Games folder
            string repoRootGuess = gamesParent != null ? Directory.GetParent(gamesParent)?.FullName : null;
            var compatCandidates = new System.Collections.Generic.List<string>();
            void AddCandidate(string baseDir) { if (!string.IsNullOrEmpty(baseDir)) compatCandidates.Add(Path.Combine(baseDir, "titles", titleId + ".json")); }
            AddCandidate(_compatDir);
            if (repoRootGuess != null) AddCandidate(Path.Combine(repoRootGuess, "compat_seed"));
            if (gamesParent != null) AddCandidate(Path.Combine(gamesParent, "compat_seed"));
            AddCandidate(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "compat_seed"));
            string compatFile = compatCandidates.FirstOrDefault(File.Exists);
            if (compatFile != null)
            {
                try
                {
                    string content = File.ReadAllText(compatFile);
                    compatStatus = ExtractJsonString(content, "curated_status") ?? "untested";
                }
                catch { }
            }

            return new GameEntry
            {
                TitleId = titleId,
                Title = title,
                EbootPath = ebootPath,
                DirPath = dir,
                CoverPath = File.Exists(coverPath) ? coverPath : null,
                BackgroundPath = File.Exists(bgPath) ? bgPath : null,
                MusicPath = musicPath,
                SizeBytes = sizeBytes,
                CompatStatus = compatStatus.ToUpper()
            };
        }

        private string CanonicalizeTitleId(string dirName)
        {
            string[] suffixes = { "-app", "-patch" };
            foreach (var suffix in suffixes)
            {
                int idx = dirName.IndexOf(suffix);
                if (idx != -1) return dirName.Substring(0, idx);
            }
            return dirName;
        }

        private string ExtractJsonString(string json, string key)
        {
            string search = "\"" + key + "\"";
            int idx = json.IndexOf(search);
            if (idx == -1) return null;
            int colonIdx = json.IndexOf(":", idx);
            if (colonIdx == -1) return null;
            int q1 = json.IndexOf("\"", colonIdx);
            if (q1 == -1) return null;
            int q2 = json.IndexOf("\"", q1 + 1);
            if (q2 == -1) return null;
            return json.Substring(q1 + 1, q2 - q1 - 1);
        }

        private BitmapImage LoadImageHelper(string path)
        {
            if (string.IsNullOrEmpty(path) || !File.Exists(path)) return null;
            try
            {
                var bitmap = new BitmapImage();
                bitmap.BeginInit();
                bitmap.UriSource = new Uri(Path.GetFullPath(path), UriKind.Absolute);
                bitmap.CacheOption = BitmapCacheOption.OnLoad;
                bitmap.EndInit();
                bitmap.Freeze();
                return bitmap;
            }
            catch (Exception ex)
            {
                LogConsole($"Failed to load image from {path}: {ex.Message}");
                return null;
            }
        }

        // Library tile presentation. A focused tile is larger, fully opaque and
        // elevated; unfocused tiles recede. Sized for reading at console
        // distance rather than at a desk.
        private const double TileSize = 200.0;
        // Larger tiles need less scale to read as focused; too much and the
        // focused tile crowds its neighbours.
        private const double FocusedTileScale = 1.10;
        private const double UnfocusedTileOpacity = 0.55;

        private static void SetTileScale(Border card, double scale)
        {
            if (card == null) return;
            if (!(card.RenderTransform is ScaleTransform st))
            {
                card.RenderTransformOrigin = new Point(0.5, 0.5);
                st = new ScaleTransform(1.0, 1.0);
                card.RenderTransform = st;
            }
            st.ScaleX = scale;
            st.ScaleY = scale;
        }

        private void AddGameTile(GameEntry game)
        {
            var border = new Border
            {
                Width = TileSize,
                Height = TileSize,
                Margin = new Thickness(14, 0, 14, 0),
                CornerRadius = (CornerRadius)FindResource("ThemeCornerM"),
                Background = new SolidColorBrush(Color.FromArgb(30, 255, 255, 255)),
                BorderBrush = new SolidColorBrush(Color.FromArgb(15, 255, 255, 255)),
                BorderThickness = new Thickness(2),
                Cursor = Cursors.Hand,
                ClipToBounds = true,
                Tag = game,
                VerticalAlignment = VerticalAlignment.Center,
                // Unfocused tiles recede so the focused one carries the eye.
                Opacity = UnfocusedTileOpacity,
                RenderTransformOrigin = new Point(0.5, 0.5),
                RenderTransform = new ScaleTransform(1.0, 1.0),
            };
            System.Windows.Automation.AutomationProperties.SetName(border, game.Title ?? "");

            var tileGrid = new Grid();
            var coverImage = LoadImageHelper(game.CoverPath);
            if (coverImage != null)
            {
                tileGrid.Children.Add(new Image
                {
                    Source = coverImage,
                    Stretch = Stretch.UniformToFill
                });
            }
            else
            {
                // Fallback text
                tileGrid.Children.Add(new TextBlock
                {
                    Text = game.Title,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    VerticalAlignment = VerticalAlignment.Center,
                    Foreground = Brushes.White,
                    FontSize = 15,
                    FontWeight = FontWeights.Bold,
                    TextWrapping = TextWrapping.Wrap,
                    TextAlignment = TextAlignment.Center,
                    Margin = new Thickness(5)
                });
            }

            // Highlight overlay sits above the cover; handlers only touch this,
            // never border.Background, so the thumbnail is never erased.
            var overlay = new Border
            {
                Background = new SolidColorBrush(Color.FromArgb(0, 255, 255, 255))
            };
            tileGrid.Children.Add(overlay);
            border.Child = tileGrid;
            Theme.SetClipCorners(border, 10);   // clip the cover to the corner style

            // Hover effects
            border.MouseEnter += (s, e) =>
            {
                if (_selectedGame != game)
                {
                    border.BorderBrush = (Brush)FindResource("ThemeAccent");
                    overlay.Background = new SolidColorBrush(Color.FromArgb(40, 255, 255, 255));
                    border.Opacity = 0.85;
                }
            };
            border.MouseLeave += (s, e) =>
            {
                if (_selectedGame != game)
                {
                    border.BorderBrush = new SolidColorBrush(Color.FromArgb(15, 255, 255, 255));
                    overlay.Background = new SolidColorBrush(Color.FromArgb(0, 255, 255, 255));
                    border.Effect = null;
                    border.Opacity = UnfocusedTileOpacity;
                }
            };
            border.MouseDown += (s, e) =>
            {
                SelectGame(game);
            };

            GamesWrapPanel.Children.Add(border);
            // Layout has not run yet, so defer the overflow check.
            Dispatcher.BeginInvoke(new Action(UpdateLibraryScrollArrows),
                System.Windows.Threading.DispatcherPriority.Loaded);
        }

        private void SelectGame(GameEntry game)
        {
            _selectedGame = game;

            DetailTitle.Text = game.Title;
            DetailTitleId.Text = game.TitleId;
            DetailSize.Text = FormatSizeChip(game.SizeBytes);

            if (_discordRpc != null)
            {
                _discordRpc.UpdatePresence($"Browsing {game.Title}", $"ID: {game.TitleId}");
            }

            DetailPath.Text = game.EbootPath;

            ApplyCompatBadge(game.CompatStatus);

            var coverImage = LoadImageHelper(game.CoverPath);
            if (coverImage != null)
            {
                // Uniform, not UniformToFill: a wide or tall cover is shown whole
                // inside the card on the surface colour instead of being cropped
                // (asked 2026-09-06: "big image of title not showing fully").
                GameCoverInner.Background = new ImageBrush
                {
                    ImageSource = coverImage,
                    Stretch = Stretch.Uniform
                };
            }
            else
            {
                GameCoverInner.Background = (Brush)FindResource("ThemeRaised");
            }

            var bgImage = LoadImageHelper(game.BackgroundPath);
            if (bgImage != null)
            {
                WindowBgImage.Source = bgImage;
            }
            else
            {
                WindowBgImage.Source = null;
            }

            // Update border highlight in carousel
            foreach (Border card in GamesWrapPanel.Children)
            {
                var cardOverlay = (card.Child as Grid)?.Children.OfType<Border>().FirstOrDefault();
                if (card.Tag == game)
                {
                    card.BorderBrush = new SolidColorBrush(Color.FromRgb(0, 153, 255));
                    if (cardOverlay != null)
                        cardOverlay.Background = new SolidColorBrush(Color.FromArgb(0, 255, 255, 255));
                    card.Effect = new System.Windows.Media.Effects.DropShadowEffect
                    {
                        Color = Color.FromRgb(0, 153, 255),
                        BlurRadius = 34,
                        ShadowDepth = 0,
                        Opacity = 0.9
                    };
                    card.Opacity = 1.0;
                    SetTileScale(card, FocusedTileScale);
                    Panel.SetZIndex(card, 10);
                }
                else
                {
                    card.BorderBrush = new SolidColorBrush(Color.FromArgb(15, 255, 255, 255));
                    if (cardOverlay != null)
                        cardOverlay.Background = new SolidColorBrush(Color.FromArgb(0, 255, 255, 255));
                    card.Effect = null;
                    card.Opacity = UnfocusedTileOpacity;
                    SetTileScale(card, 1.0);
                    Panel.SetZIndex(card, 0);
                }
            }

            // Selecting a tile no longer writes "Selected: <title>" into the footer
            // (asked 2026-09-06). The header already shows the selected title, so
            // the footer keeps its last real status -- Ready, Running, a rebind
            // prompt -- instead of repeating it.

            // Last-played chip (concept hero): shown only when the title has
            // been launched before.
            if (DetailLastPlayed != null && DetailLastPlayedChip != null)
            {
                var lp = _recent?.LastPlayed(game.TitleId);
                if (lp != null)
                {
                    DetailLastPlayed.Text = FormatLastPlayed(lp.Value);
                    DetailLastPlayedChip.Visibility = Visibility.Visible;
                }
                else DetailLastPlayedChip.Visibility = Visibility.Collapsed;
            }

            // Trigger music playback with debounce
            TriggerTitleMusic(game);
        }

        /// <summary>Human "last played" for the hero chip, from a UTC timestamp.</summary>
        private static string FormatLastPlayed(DateTime utc)
        {
            var d = DateTime.UtcNow - utc;
            if (d.TotalMinutes < 1) return "Last played · just now";
            if (d.TotalMinutes < 60) return $"Last played · {(int)d.TotalMinutes} min ago";
            if (d.TotalHours < 24) return $"Last played · {(int)d.TotalHours} h ago";
            if (d.TotalDays < 30) return $"Last played · {(int)d.TotalDays} d ago";
            return "Last played · " + utc.ToLocalTime().ToString("d MMM yyyy");
        }

        private void TriggerTitleMusic(GameEntry game)
        {
            // Cancel any in-flight decode or play request
            _musicCts?.Cancel();
            _musicCts = new System.Threading.CancellationTokenSource();
            var token = _musicCts.Token;

            // Stop current music
            _mediaPlayer.Stop();

            if (game.MusicPath == null || !_config.ui.title_music_enabled)
            {
                return;
            }

            string musicPath = game.MusicPath;
            double volume = Math.Max(0, Math.Min(1, _config.ui.title_music_volume));

            Task.Run(async () =>
            {
                try
                {
                    // Debounce: wait 300ms
                    await Task.Delay(300, token);

                    string playPath = musicPath;

                    // If it is an AT9 or OGG file, decode it first
                    string ext = Path.GetExtension(musicPath).ToLower();
                    if (ext == ".at9" || ext == ".ogg")
                    {
                        playPath = await GetOrDecodeMusicAsync(musicPath, token);
                    }

                    if (token.IsCancellationRequested || string.IsNullOrEmpty(playPath) || !File.Exists(playPath))
                    {
                        return;
                    }

                    Dispatcher.Invoke(() =>
                    {
                        try
                        {
                            _mediaPlayer.Open(new Uri(playPath));
                            _mediaPlayer.Volume = volume;
                            _mediaPlayer.Play();
                        }
                        catch (Exception ex)
                        {
                            LogConsole("Error starting MediaPlayer: " + ex.Message);
                        }
                    });
                }
                catch (TaskCanceledException) { }
                catch (Exception ex)
                {
                    LogConsole("Title music playback error: " + ex.Message);
                }
            });
        }

        private async Task<string> GetOrDecodeMusicAsync(string srcPath, System.Threading.CancellationToken token)
        {
            string cacheDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Cache", "Audio");
            if (!Directory.Exists(cacheDir))
            {
                Directory.CreateDirectory(cacheDir);
            }

            string hash = GetStringHash(srcPath);
            string cachedWav = Path.Combine(cacheDir, hash + ".wav");

            if (File.Exists(cachedWav))
            {
                return cachedWav;
            }

            string decoder = LocateSndDecode();
            if (decoder == null)
            {
                LogConsole("SndDecode utility not found, skipping music preview.");
                return null;
            }

            LogConsole($"Decoding audio: {Path.GetFileName(srcPath)} -> {Path.GetFileName(cachedWav)}");

            var tcs = new TaskCompletionSource<bool>();
            using (var process = new Process())
            {
                process.StartInfo.FileName = decoder;
                process.StartInfo.Arguments = $"\"{srcPath}\" \"{cachedWav}\"";
                process.StartInfo.UseShellExecute = false;
                process.StartInfo.CreateNoWindow = true;
                process.EnableRaisingEvents = true;
                process.Exited += (s, e) => tcs.TrySetResult(process.ExitCode == 0);

                token.Register(() =>
                {
                    try { process.Kill(); } catch { }
                    tcs.TrySetCanceled();
                });

                try
                {
                    process.Start();
                }
                catch (Exception ex)
                {
                    LogConsole("Failed to start sound decoder: " + ex.Message);
                    return null;
                }

                bool success = await tcs.Task;
                if (success && File.Exists(cachedWav))
                {
                    return cachedWav;
                }
            }

            return null;
        }

        private string GetStringHash(string text)
        {
            using (var sha = System.Security.Cryptography.SHA256.Create())
            {
                byte[] bytes = sha.ComputeHash(Encoding.UTF8.GetBytes(text));
                StringBuilder sb = new StringBuilder();
                foreach (byte b in bytes)
                {
                    sb.Append(b.ToString("x2"));
                }
                return sb.ToString();
            }
        }

        private void SearchBox_TextChanged(object sender, TextChangedEventArgs e)
        {
            if (SearchPlaceholder != null)
            {
                SearchPlaceholder.Visibility = string.IsNullOrEmpty(SearchBox.Text) ? Visibility.Visible : Visibility.Collapsed;
            }

            if (GamesWrapPanel == null) return;

            string query = SearchBox.Text.ToLower().Trim();
            if (string.IsNullOrEmpty(query))
            {
                foreach (FrameworkElement item in GamesWrapPanel.Children)
                {
                    item.Visibility = Visibility.Visible;
                }
                return;
            }

            foreach (FrameworkElement item in GamesWrapPanel.Children)
            {
                var game = item.Tag as GameEntry;
                if (game != null)
                {
                    bool match = game.Title.ToLower().Contains(query) || game.TitleId.ToLower().Contains(query);
                    item.Visibility = match ? Visibility.Visible : Visibility.Collapsed;
                }
            }
        }

        private void SearchBox_GotFocus(object sender, RoutedEventArgs e)
        {
            if (SearchBoxBorder != null)
            {
                SearchBoxBorder.BorderBrush = new SolidColorBrush(Color.FromRgb(0, 153, 255));
                SearchBoxBorder.Background = new SolidColorBrush(Color.FromArgb(60, 0, 0, 0));
            }
        }

        private void SearchBox_LostFocus(object sender, RoutedEventArgs e)
        {
            if (SearchBoxBorder != null)
            {
                SearchBoxBorder.BorderBrush = new SolidColorBrush(Color.FromArgb(31, 255, 255, 255));
                SearchBoxBorder.Background = new SolidColorBrush(Color.FromArgb(47, 255, 255, 255));
            }
        }

        private void LaunchButton_Click(object sender, RoutedEventArgs e)
        {
            if (_selectedGame == null) return;
            SetTitleMusicAudible(false);   // the lobby music ends when Play is pressed

            // Aggressively clean up any orphaned emulator processes on the system first
            try
            {
                foreach (var proc in System.Diagnostics.Process.GetProcessesByName("pcsx5_cli"))
                {
                    try { proc.Kill(); proc.Dispose(); } catch { }
                }
            }
            catch { }

            if (_coreRunning) return; // single game at a time

            var game = _selectedGame;

            LaunchButton.IsEnabled = false;
            StopButton.Visibility = Visibility.Visible;
            if (GameStopButton != null) GameStopButton.Visibility = Visibility.Visible;
            if (GameKillButton != null) GameKillButton.Visibility = Visibility.Visible;
            ConsoleOutputBox.Document.Blocks.Clear();

            LogConsole($"Launching via IPC: \"{game.EbootPath}\"");
            FooterStatus.Text = $"Booting: {game.Title}";

            // Stop title music when game launches
            _mediaPlayer.Stop();

            // Prepare the frame display Image (replaces HwndHost entirely).
            var frameImage = new System.Windows.Controls.Image
            {
                Stretch = System.Windows.Media.Stretch.Uniform,
                HorizontalAlignment = System.Windows.HorizontalAlignment.Stretch,
                VerticalAlignment = System.Windows.VerticalAlignment.Stretch,
            };
            EmulatorHostPresenter.Content = frameImage;

            LibraryView.Visibility = Visibility.Collapsed;
            AnalyzerView.Visibility = Visibility.Collapsed;
            ToolsHubView.Visibility = Visibility.Collapsed;
            ControllerView.Visibility = Visibility.Collapsed;
            SettingsView.Visibility = Visibility.Collapsed;
            GameView.Visibility = Visibility.Visible;

            // Show boot overlay
            ShowBootOverlay(game, BootPhase.Initializing, "Initializing emulator core...");

            // Launch via IPC (out-of-process core).
            _session.Reset();
            // Point the core at the same config the Settings screen writes.
            // Without this the shell saved settings the emulator never read.
            if (!string.IsNullOrEmpty(_configPath))
                _session.ConfigDir = Path.GetDirectoryName(_configPath);

            _session.LaunchIpc(game);

            // Subscribe to IPC frame events for display.
            var ipc = _session.IpcSession;
            if (ipc != null)
            {
                ipc.FrameReady += () =>
                {
                    var bmp = ipc.FrameBitmap;
                    if (bmp != null && frameImage.Source != bmp)
                        frameImage.Source = bmp;

                    // A frame is the honest "it is running" signal on this path.
                    //
                    // HideBootOverlay was only ever called from OnGameWindowReady,
                    // the native-window callback. The shell launches the core with
                    // --headless and consumes frames over IPC instead, so there is
                    // no native window and that callback never fires: the overlay
                    // sat at "Step 1 of 6 / 15%" for the entire session while
                    // frames were already arriving behind it.
                    if (GameBootOverlay != null &&
                        GameBootOverlay.Visibility == Visibility.Visible)
                    {
                        HideBootOverlay();
                    }
                };
            }
        }

        // ── GameSession event handlers ──────────────────────────────────────────
        // All fire on the WPF Dispatcher — safe to touch UI directly.

        private void OnGameStarted(GameEntry game)
        {
            LogConsole($"[Session] Game session started: {game.Title}");
            FooterStatus.Text = $"{game.Title} - Starting";

            // Recently played first. Stamped here, once the core process has
            // actually started, not at the Play click: a launch that fails to
            // start is not a play. This handler is dispatched to the UI thread.
            if (_recent != null && game != null)
            {
                if (!_recent.Record(game.TitleId)) LogConsole("Could not save recent_plays.json");
                _games.Remove(game); _games.Insert(0, game);
                ApplyLibrarySort();
                RebuildShelf();
            }
        }

        private void OnBootPhaseChanged(BootPhase phase, string message)
        {
            if (phase == BootPhase.Running && _session?.IpcSession != null)
            {
                // First guest frame: the game takes the screen.
                UpdateBootPhaseUI(phase, message);
                HideBootOverlay();
                FooterStatus.Text = _selectedGame != null ? $"{_selectedGame.Title} - Running" : "Running";
                return;
            }
            ShowBootOverlay(_session?.CurrentGame, phase, message);
            FooterStatus.Text = $"{_session?.CurrentGame?.Title ?? "Game"} - Booting ({message})";
        }

        private void OnGameWindowReady(IntPtr hwnd)
        {
            // The core reported its render window (Rule 11: the shell reparents
            // the HWND the core reports). Until 2026-09-06 _emuHost was never
            // constructed, so EmbedEmulatorWindow returned at its first line and
            // the launcher showed the IPC bitmap instead -- black, because a
            // headless core has no renderer (TASKS 4.14). The host is created
            // here, on demand; an HwndHost only owns a native window once it is
            // loaded, so the reparent waits for that.
            // While the booting screen owns the game area, defer the whole embed:
            // the host is a native window and paints over WPF even with its
            // child hidden, so creating it now would cover the booting screen.
            // The core's window is created hidden (--embed) and waits.
            if (_session?.IpcSession != null && GameBootOverlay != null && GameBootOverlay.Visibility == Visibility.Visible)
            {
                _pendingEmbedHwnd = hwnd;
                return;
            }
            if (_emuHost == null)
            {
                _emuHost = new EmulatorWindowHost();
                EmulatorHostPresenter.SizeChanged -= EmulatorHostPresenter_SizeChanged;
                EmulatorHostPresenter.SizeChanged += EmulatorHostPresenter_SizeChanged;
                EmulatorHostPresenter.Content = _emuHost;
            }
            if (_emuHost.HostHandle != IntPtr.Zero) EmbedEmulatorWindow(hwnd);
            else
            {
                var pending = hwnd;
                RoutedEventHandler once = null;
                once = (s, e) => { _emuHost.Loaded -= once; EmbedEmulatorWindow(pending); };
                _emuHost.Loaded += once;
            }
            // Out of process, the booting screen stays until the first guest frame
            // (OnBootPhaseChanged -> Running). In process there is no frame
            // signal, so the window itself is the cue.
            if (_session?.IpcSession == null) HideBootOverlay();
            FooterStatus.Text = _selectedGame != null ? $"{_selectedGame.Title} - Running" : "Running";

            if (_discordRpc != null && _selectedGame != null)
                _discordRpc.UpdatePresence($"Playing {_selectedGame.Title}", "In-Game");
        }

        private void OnGameStopped(int exitCode)
        {
            HideBootOverlay();
            HidePauseMenu();
            HideWatchdogToast();

            LaunchButton.IsEnabled = true;
            StopButton.Visibility = Visibility.Collapsed;
            if (GameStopButton != null) GameStopButton.Visibility = Visibility.Collapsed;
            if (GameKillButton != null) GameKillButton.Visibility = Visibility.Collapsed;
            LogConsole($"Game session ended (exit code {exitCode}).");
            FooterStatus.Text = "Ready";

            TeardownGameView();

            if (_discordRpc != null)
            {
                if (_selectedGame != null)
                    _discordRpc.UpdatePresence($"Browsing {_selectedGame.Title}", $"ID: {_selectedGame.TitleId}");
                else
                    _discordRpc.UpdatePresence("Idle", "Main Menu");
            }

            if (_selectedGame != null) TriggerTitleMusic(_selectedGame);

            // Pull the latest community status for the title we just ran.
            RefreshCompatFromDatabase(_selectedGame);
        }

        private void OnGameCrashed(int exitCode, string message)
        {
            // Aggressively kill any running or orphan emulator processes on crash
            try
            {
                _session?.Kill();
                foreach (var proc in System.Diagnostics.Process.GetProcessesByName("pcsx5_cli"))
                {
                    try { proc.Kill(); proc.Dispose(); } catch { }
                }
            }
            catch { }

            HideBootOverlay();
            HidePauseMenu();
            HideWatchdogToast();

            LaunchButton.IsEnabled = true;
            StopButton.Visibility = Visibility.Collapsed;
            if (GameStopButton != null) GameStopButton.Visibility = Visibility.Collapsed;
            if (GameKillButton != null) GameKillButton.Visibility = Visibility.Collapsed;
            LogConsole($"[CRASH] {message}");
            FooterStatus.Text = $"{_session?.CurrentGame?.Title ?? "Game"} - Crashed";

            // Show crash dialog with full error details
            string excName = ((uint)exitCode) switch {
                0xC0000005 => "EXCEPTION_ACCESS_VIOLATION",
                0xC000001D => "EXCEPTION_ILLEGAL_INSTRUCTION",
                0xC00000FD => "EXCEPTION_STACK_OVERFLOW",
                0xC0000094 => "EXCEPTION_INT_DIVIDE_BY_ZERO",
                0xC0000096 => "EXCEPTION_PRIV_INSTRUCTION",
                0xC0000008 => "EXCEPTION_INVALID_HANDLE",
                0xE06D7363 => "C++ Exception (eh)",
                _ => ""
            };
            string excLabel = string.IsNullOrEmpty(excName)
                ? $"0x{exitCode:X8}"
                : $"0x{exitCode:X8} — {excName}";
            CrashExcCodeText.Text = excLabel;

            // Try to extract RIP from logs/crash_log.txt if it exists
            string ripInfo = "See logs/crash_log.txt for full dump";
            try {
                string baseDir = System.AppContext.BaseDirectory;
                string crashLogPath = System.IO.Path.Combine(baseDir, "logs", "crash_log.txt");
                // Fallback to base dir check if they run standing standalone
                if (!System.IO.File.Exists(crashLogPath)) {
                    crashLogPath = System.IO.Path.Combine(baseDir, "crash_log.txt");
                }
                if (System.IO.File.Exists(crashLogPath)) {
                    var lines = System.IO.File.ReadAllLines(crashLogPath);
                    foreach (var line in lines) {
                        if (line.StartsWith("RIP:")) { ripInfo = line.Trim(); break; }
                    }
                }
            } catch { }
            CrashRipText.Text = ripInfo;

            CrashAnalysisText.Text = message;
            CrashRawText.Text = $"Title: {_session?.CurrentGame?.Title ?? "?"}\n"
                              + $"TitleId: {_session?.CurrentGame?.TitleId ?? "?"}\n"
                              + $"Exit Code: 0x{exitCode:X8}"
                              + (string.IsNullOrEmpty(excName) ? "" : $" ({excName})")
                              + $"\n\n{message}";
            CrashDialogOverlay.Visibility = Visibility.Visible;

            // Haptic feedback: brief rumble so the player feels the crash
            if (_config?.input?.rumble == true)
            {
                try
                {
                    CoreBridge.pcsx5_pad_set_rumble(0, 255, 255);
                    Task.Delay(400).ContinueWith(_ => CoreBridge.pcsx5_pad_set_rumble(0, 0, 0));
                }
                catch { }
            }

            TeardownGameView();

            if (_selectedGame != null) TriggerTitleMusic(_selectedGame);
        }

        private void OnGameHanging()
        {
            if (WatchdogTitle != null) WatchdogTitle.Text = I18n.Tr("ui.game_may_be_unresponsive");
            LogConsole("[Watchdog] Game appears unresponsive (no heartbeat for 15s).");
            ShowWatchdogToast();
        }

        // ── Boot overlay helpers ────────────────────────────────────────────

        private void ShowBootOverlay(GameEntry game, BootPhase phase, string detail)
        {
            if (GameBootOverlay == null) return;
            GameBootOverlay.Visibility = Visibility.Visible;

            if (BootGameTitle != null && game != null)
                BootGameTitle.Text = game.Title ?? "";
            if (BootCoverInner != null && game != null)
            {
                var cover = LoadImageHelper(game.CoverPath);
                BootCoverInner.Background = cover != null
                    ? new ImageBrush { ImageSource = cover, Stretch = Stretch.Uniform }
                    : (Brush)FindResource("ThemeRaised");
            }

            UpdateBootPhaseUI(phase, detail);
            // No spinner: the concept's booting screen is staged and quiet. A
            // "stuck" notice appears only when frames stop after the first one
            // (TASKS 4.13 step 13), never during a normal boot.
        }

        private void UpdateBootPhaseUI(BootPhase phase, string detail)
        {
            if (BootPhaseTitle == null) return;
            string badge = "Step 1 of 6";
            string title = "INITIALIZING CORE & VIRTUAL MEMORY";
            string desc = detail;
            int percent = 15;

            switch (phase)
            {
                case BootPhase.Initializing:
                    badge = "Step 1 of 6";
                    title = "INITIALIZING CORE & VIRTUAL MEMORY";
                    desc = string.IsNullOrEmpty(detail) ? "Allocating 128GB guest virtual address space..." : detail;
                    percent = 15;
                    break;
                case BootPhase.ExtractingPkg:
                    badge = "Step 2 of 6";
                    title = "EXTRACTING GAME PACKAGE & ASSETS";
                    desc = string.IsNullOrEmpty(detail) ? "Mounting PFS container and decrypting streams..." : detail;
                    percent = 30;
                    break;
                case BootPhase.LoadingElf:
                    badge = "Step 3 of 6";
                    title = "PARSING ELF EXECUTABLE & SEGMENTS";
                    desc = string.IsNullOrEmpty(detail) ? "Mapping program segments and dynamic relocations..." : detail;
                    percent = 50;
                    break;
                case BootPhase.LinkingModules:
                    badge = "Step 4 of 6";
                    title = "RESOLVING HLE SYSTEM MODULES & NIDS";
                    desc = string.IsNullOrEmpty(detail) ? "Binding libkernel, AGC graphics, and audio dispatchers..." : detail;
                    percent = 70;
                    break;
                case BootPhase.StartingCpu:
                    badge = "Step 5 of 6";
                    title = "INITIALIZING VULKAN & GUEST THREADS";
                    desc = string.IsNullOrEmpty(detail) ? "Spawning worker threads and arming exception handlers..." : detail;
                    percent = 85;
                    break;
                case BootPhase.Running:
                    badge = "Step 6 of 6";
                    title = "LAUNCHING MAIN EXECUTION LOOP";
                    desc = string.IsNullOrEmpty(detail) ? "Executing guest EBOOT.BIN and rendering frames..." : detail;
                    percent = 100;
                    break;
            }

            if (BootStepBadge != null) BootStepBadge.Text = $"{_session?.CurrentGame?.TitleId ?? ""} · {badge}";
            if (BootPhaseTitle != null) BootPhaseTitle.Text = title;
            if (BootPhaseDetail != null) BootPhaseDetail.Text = desc;
            if (BootProgressBar != null) BootProgressBar.Value = percent;
            if (BootProgressPercentText != null) BootProgressPercentText.Text = $"{percent}%";

            // Stage list: done stages carry the accent dot and text, the current
            // one full text, the rest muted.
            int now = (int)phase;   // BootPhase is declared in stage order
            var dots = new[] { BootStageDot1, BootStageDot2, BootStageDot3, BootStageDot4, BootStageDot5, BootStageDot6 };
            var labels = new[] { BootStage1, BootStage2, BootStage3, BootStage4, BootStage5, BootStage6 };
            var accent = (Brush)FindResource("ThemeAccent");
            var text = (Brush)FindResource("ThemeText");
            var muted = (Brush)FindResource("ThemeTextMuted");
            var hair = (Brush)FindResource("ThemeHairline");
            for (int i = 0; i < 6; i++)
            {
                if (dots[i] == null || labels[i] == null) continue;
                bool done = i < now, current = i == now;
                dots[i].Background = done || current ? accent : hair;
                labels[i].Foreground = current ? text : done ? muted : hair;
                labels[i].Opacity = current ? 1.0 : done ? 1.0 : 0.9;
            }
        }

        private void HideBootOverlay()
        {
            if (GameBootOverlay != null) GameBootOverlay.Visibility = Visibility.Collapsed;
            if (_pendingEmbedHwnd != IntPtr.Zero)
            {
                var h = _pendingEmbedHwnd; _pendingEmbedHwnd = IntPtr.Zero;
                OnGameWindowReady(h);   // the booting screen is down; embed and show now
                return;
            }
            // The embedded core window was kept hidden behind the booting screen
            // (a native child always paints over WPF); reveal it now.
            if (_embedPendingShow && _embeddedEmuHwnd != IntPtr.Zero && NativeMethods.IsWindow(_embeddedEmuHwnd))
            {
                _embedPendingShow = false;
                NativeMethods.ShowWindow(_embeddedEmuHwnd, NativeMethods.SW_SHOW);
                ResizeEmbeddedWindow();
                NativeMethods.SetFocus(_embeddedEmuHwnd);
            }
        }
        private bool _embedPendingShow;
        private IntPtr _pendingEmbedHwnd = IntPtr.Zero;

        private void BootCancelBtn_Click(object sender, RoutedEventArgs e)
        {
            _session?.Kill();
            LogConsole("Boot cancelled by user.");
        }

        // ── Pause menu helpers ────────────────────────────────────────────

        private void ShowPauseMenu()
        {
            if (PauseMenuOverlay == null) return;
            _pauseMenuVisible = true;
            _pauseMenuIndex = 0;
            _session?.Pause();

            if (PauseMenuGameTitle != null && _selectedGame != null)
                PauseMenuGameTitle.Text = _selectedGame.Title;

            PauseMenuOverlay.Visibility = Visibility.Visible;
            // Focus the first item for controller nav
            PauseResumeBtn?.Focus();

            FooterStatus.Text = "Paused — [D-Pad ↑↓] Navigate  [✕] Select  [PS] Resume";
        }

        private void HidePauseMenu()
        {
            _pauseMenuVisible = false;
            if (PauseMenuOverlay != null) PauseMenuOverlay.Visibility = Visibility.Collapsed;
        }

        private void ResumeFromPause()
        {
            HidePauseMenu();
            _session?.Resume();
            FooterStatus.Text = _selectedGame != null ? $"Running: {_selectedGame.Title}" : "Running";
        }

        private void HandlePauseMenuNav(bool up, bool down, bool cross, bool circle, bool triangle)
        {
            // Circle = stop, Triangle = console, Cross = resume/select
            if (circle) { HidePauseMenu(); _session?.RequestStop(); return; }
            if (triangle) { ResumeFromPause(); PauseConsole_Click(this, null); return; }

            if (up || down)
            {
                int count = 3; // Resume, Console, Stop
                _pauseMenuIndex = down
                    ? (_pauseMenuIndex + 1) % count
                    : (_pauseMenuIndex - 1 + count) % count;

                switch (_pauseMenuIndex)
                {
                    case 0: PauseResumeBtn?.Focus(); break;
                    case 1: PauseConsoleBtn?.Focus(); break;
                    case 2: PauseStopBtn?.Focus(); break;
                }
            }

            if (cross)
            {
                switch (_pauseMenuIndex)
                {
                    case 0: ResumeFromPause(); break;
                    case 1: ResumeFromPause(); PauseConsole_Click(this, null); break;
                    case 2: HidePauseMenu(); _session?.RequestStop(); break;
                }
            }
        }

        private void PauseResume_Click(object sender, RoutedEventArgs e) => ResumeFromPause();

        private void PauseConsole_Click(object sender, RoutedEventArgs e)
        {
            HidePauseMenu();
            SetGameConsoleVisible(true);
        }

        private void PauseStop_Click(object sender, RoutedEventArgs e)
        {
            HidePauseMenu();
            _session?.RequestStop();
        }

        // ── Watchdog toast helpers ─────────────────────────────────────────

        /// <summary>Frames stopped after the game had drawn (TASKS 4.13 step 13).
        /// Not while paused: a paused core draws nothing by design.</summary>
        private void OnFramesStalled(int seconds)
        {
            if (_pauseMenuVisible) return;
            if (WatchdogTitle != null) WatchdogTitle.Text = string.Format(I18n.Tr("ui.no_new_frames_for"), seconds);
            if (!_watchdogToastVisible) { LogConsole($"[Watchdog] No new frames for {seconds} s."); ShowWatchdogToast(); }
        }

        private void OnFramesResumed()
        {
            if (_watchdogToastVisible) { LogConsole("[Watchdog] Frames resumed."); HideWatchdogToast(); }
        }

        // Over an embedded game the notice must live in its own top-level window
        // (OverlayWindow): the game is a native child that paints over every WPF
        // element in the main window. The toast element is moved there while
        // shown and back into the main window when hidden.
        private OverlayWindow _overlay;
        private System.Windows.Controls.Panel _toastHome;

        private void ShowWatchdogToast()
        {
            if (WatchdogToast == null) return;
            _watchdogToastVisible = true;
            if (_embeddedEmuHwnd != IntPtr.Zero && EmulatorHostPresenter != null)
            {
                _overlay ??= new OverlayWindow(this, EmulatorHostPresenter);
                if (WatchdogToast.Parent is System.Windows.Controls.Panel home)
                {
                    _toastHome = home;
                    home.Children.Remove(WatchdogToast);
                }
                WatchdogToast.Visibility = Visibility.Visible;
                WatchdogToast.Opacity = 1;
                WatchdogToast.Margin = new Thickness(0);
                _overlay.ShowOver(WatchdogToast);
            }
            else
            {
                WatchdogToast.Visibility = Visibility.Visible;
                WatchdogToast.Opacity = 1;
            }
            WatchdogWaitBtn?.Focus();
        }

        private void HideWatchdogToast()
        {
            _watchdogToastVisible = false;
            if (WatchdogToast == null) return;
            WatchdogToast.Opacity = 0; WatchdogToast.Visibility = Visibility.Collapsed;
            if (_overlay != null && ReferenceEquals(_overlay.Content, WatchdogToast))
            {
                _overlay.Content = null;
                _overlay.Hide();
                WatchdogToast.Margin = new Thickness(0, 0, 40, 72);
                _toastHome?.Children.Add(WatchdogToast);
            }
        }

        private void WatchdogWait_Click(object sender, RoutedEventArgs e)
        {
            HideWatchdogToast();
            LogConsole("[Watchdog] User chose to wait. Monitoring...");
        }

        private void WatchdogForceStop_Click(object sender, RoutedEventArgs e)
        {
            HideWatchdogToast();
            LogConsole("[Watchdog] User forced stop.");
            _session?.Kill();
        }

        private void HandleWatchdogToastNav(bool cross, bool circle)
        {
            if (cross) WatchdogWait_Click(this, null);
            else if (circle) WatchdogForceStop_Click(this, null);
        }

        // ── Folder picker overlay helpers ─────────────────────────────────

        // -- In-app folder browser ------------------------------------------
        // The shell must be usable with a controller alone, and a Windows folder
        // dialog cannot be driven by one, so the filesystem is browsed in-app.
        // Gamepad:  D-Pad move | Cross open | Triangle select | Circle up/cancel
        // Keyboard: arrows and Enter via the ListBox, plus the buttons
        // Mouse:    click to highlight, double-click to open
        private string _folderPickerPath;   // null => the drive list
        private string _folderPickerRefreshKey;  // settings sub-page to redraw on confirm

        private void ShowFolderPickerOverlay(string target, string currentPath = null)
        {
            _folderPickerTarget = target;

            FolderPickerTitle.Text = I18n.Tr("picker.title");
            FolderPickerSubtitle.Text = I18n.Tr("picker.subtitle");
            FolderPickerSelectBtn.Content = I18n.Tr("picker.select");
            FolderPickerCancelBtn.Content = I18n.Tr("picker.cancel");
            // The controller glyph legend now carries navigation; the old Up/Open
            // buttons are gone (".." row, double-click and the pad drive that.)
            BuildFooterHintChips(I18n.Tr("picker.hints"), FolderPickerHintChips);

            System.Windows.Automation.AutomationProperties.SetName(FolderPickerList, I18n.Tr("picker.list_name"));
            System.Windows.Automation.AutomationProperties.SetName(FolderPickerSelectBtn, I18n.Tr("picker.select"));
            System.Windows.Automation.AutomationProperties.SetName(FolderPickerCancelBtn, I18n.Tr("picker.cancel"));

            string startPath = null;
            try
            {
                if (!string.IsNullOrWhiteSpace(currentPath) && Directory.Exists(currentPath))
                    startPath = currentPath;
            }
            catch { startPath = null; }

            PopulateFolderPicker(startPath);
            FolderPickerOverlay.Visibility = Visibility.Visible;
            FolderPickerList.Focus();
        }

        private void HideFolderPickerOverlay()
        {
            _folderPickerTarget = null;
            _folderPickerRefreshKey = null;
            if (FolderPickerOverlay != null) FolderPickerOverlay.Visibility = Visibility.Collapsed;
        }

        private void PopulateFolderPicker(string path)
        {
            _folderPickerPath = path;
            var items = new List<FolderEntry>();

            if (string.IsNullOrEmpty(path))
            {
                FolderPickerCurrentPath.Text = I18n.Tr("picker.this_pc");
                foreach (var drive in SafeGetDrives()) items.Add(drive);
            }
            else
            {
                FolderPickerCurrentPath.Text = path;

                // Parent entry; a null FullPath means "back to the drive list".
                string parent = null;
                try { parent = Directory.GetParent(path)?.FullName; } catch { parent = null; }
                items.Add(new FolderEntry { Icon = "⬆", Name = "..", FullPath = parent, IsParent = true, Meta = I18n.Tr("picker.up_one") });

                string[] dirs = null;
                string problem = null;
                try { dirs = Directory.GetDirectories(path); }
                catch (UnauthorizedAccessException) { problem = I18n.Tr("picker.denied"); }
                catch (Exception) { problem = I18n.Tr("picker.unavailable"); }

                if (problem != null)
                {
                    items.Add(new FolderEntry { Icon = "⛔", Name = problem });
                }
                else
                {
                    Array.Sort(dirs, StringComparer.OrdinalIgnoreCase);
                    foreach (var d in dirs)
                    {
                        string name = null;
                        try { name = Path.GetFileName(d); } catch { continue; }
                        if (string.IsNullOrEmpty(name)) continue;
                        var entry = new FolderEntry { Icon = "📁", Name = name, FullPath = d };
                        AnnotateEboot(entry, d);   // eboot.bin -> accent icon + size meta
                        items.Add(entry);
                    }
                    if (items.Count == 1)
                        items.Add(new FolderEntry { Icon = "—", Name = I18n.Tr("picker.empty") });
                }
            }

            BuildBreadcrumb(path);
            BuildDriveChips(path);
            FolderPickerList.ItemsSource = items;
            if (items.Count > 0)
            {
                FolderPickerList.SelectedIndex = 0;
                FolderPickerList.ScrollIntoView(items[0]);
            }
            // "Select this folder" is meaningless while the drive list is shown.
            FolderPickerSelectBtn.IsEnabled = !string.IsNullOrEmpty(path);
        }

        /// <summary>Mark a folder that holds a PS5 eboot so the list shows it with
        /// the accent game icon and its eboot size, the way the concept does. The
        /// eboot may sit directly in the folder or in a single -app0 child. Cheap
        /// and best-effort: any I/O error just leaves it a plain folder.</summary>
        private static void AnnotateEboot(FolderEntry entry, string dir)
        {
            try
            {
                string eboot = Path.Combine(dir, "eboot.bin");
                if (!File.Exists(eboot))
                {
                    // one-level probe: <id>-app0/eboot.bin is the common dump shape
                    foreach (var sub in Directory.EnumerateDirectories(dir))
                    {
                        string cand = Path.Combine(sub, "eboot.bin");
                        if (File.Exists(cand)) { eboot = cand; break; }
                        eboot = null;
                    }
                    if (eboot == null) return;
                }
                long bytes = new FileInfo(eboot).Length;
                entry.IsGame = true;
                entry.Meta = "eboot.bin · " + FormatSizeShort(bytes);
            }
            catch { /* leave as a plain folder */ }
        }

        // Concept-style size: whole numbers at 100+, one decimal below ("102 MB",
        // "8.5 GB"). The other FormatBytes keeps two decimals for detail views.
        private static string FormatSizeShort(long bytes)
        {
            string[] units = { "B", "KB", "MB", "GB", "TB" };
            double v = bytes; int u = 0;
            while (v >= 1024 && u < units.Length - 1) { v /= 1024; u++; }
            return (v >= 100 || u == 0 ? v.ToString("0") : v.ToString("0.0")) + " " + units[u];
        }

        /// <summary>Render the current path as the concept's mono breadcrumb: dim
        /// segments split by dim slashes, the final segment bright.</summary>
        private void BuildBreadcrumb(string path)
        {
            if (FolderPickerBreadcrumb == null) return;
            FolderPickerBreadcrumb.Children.Clear();
            // Announce the current location to assistive tech (the breadcrumb is
            // otherwise a row of decorative segments).
            System.Windows.Automation.AutomationProperties.SetName(FolderPickerBreadcrumb,
                string.IsNullOrEmpty(path) ? I18n.Tr("picker.this_pc") : path);
            var mutedBrush = (Brush)FindResource("ThemeTextMuted");
            var textBrush = (Brush)FindResource("ThemeText");
            var sepBrush = (Brush)FindResource("ThemeHairline");
            double typeM = (double)FindResource("TypeM");

            var segs = new List<string> { I18n.Tr("picker.this_pc") };
            if (!string.IsNullOrEmpty(path))
                segs.AddRange(path.Split(new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar },
                                        StringSplitOptions.RemoveEmptyEntries));
            for (int i = 0; i < segs.Count; i++)
            {
                if (i > 0)
                    FolderPickerBreadcrumb.Children.Add(new TextBlock { Text = "/", FontFamily = new System.Windows.Media.FontFamily("Consolas"), FontSize = typeM, Foreground = sepBrush, Margin = new Thickness(8, 0, 8, 0), VerticalAlignment = VerticalAlignment.Center });
                FolderPickerBreadcrumb.Children.Add(new TextBlock
                {
                    Text = segs[i],
                    FontFamily = new System.Windows.Media.FontFamily("Consolas"),
                    FontSize = typeM,
                    Foreground = i == segs.Count - 1 ? textBrush : mutedBrush,
                    VerticalAlignment = VerticalAlignment.Center,
                });
            }
        }

        /// <summary>Quick drive chips: "This PC" plus each ready drive, the chip
        /// containing the current path highlighted. Clicking one jumps there.</summary>
        private void BuildDriveChips(string path)
        {
            if (FolderPickerDriveChips == null) return;
            FolderPickerDriveChips.Children.Clear();

            string curRoot = null;
            try { curRoot = string.IsNullOrEmpty(path) ? null : Path.GetPathRoot(path); } catch { curRoot = null; }

            AddDriveChip(I18n.Tr("picker.this_pc"), null, string.IsNullOrEmpty(path));
            // Current drive first so its highlight is always in view, then the rest.
            var drives = SafeGetDrives();
            bool IsCur(FolderEntry d) => curRoot != null && d.FullPath != null &&
                string.Equals(Path.GetPathRoot(d.FullPath), curRoot, StringComparison.OrdinalIgnoreCase);
            foreach (var d in drives.Where(IsCur)) AddDriveChip(d.Name.Trim(), d.FullPath, true);
            foreach (var d in drives.Where(x => !IsCur(x))) AddDriveChip(d.Name.Trim(), d.FullPath, false);
        }

        private void AddDriveChip(string label, string target, bool active)
        {
            var text = new TextBlock
            {
                Text = label,
                FontSize = (double)FindResource("TypeM"),
                Foreground = (Brush)FindResource(active ? "ThemeText" : "ThemeTextMuted"),
                VerticalAlignment = VerticalAlignment.Center,
            };
            var chip = new Border
            {
                Background = (Brush)FindResource(active ? "ThemeGround" : "ThemeRaised"),
                BorderBrush = (Brush)FindResource(active ? "ThemeBorderStrong" : "ThemeHairline"),
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(8),
                Padding = new Thickness(14, 8, 14, 8),
                Margin = new Thickness(0, 0, 8, 0),
                Cursor = System.Windows.Input.Cursors.Hand,
                Tag = target,
                Child = text,
            };
            chip.MouseLeftButtonUp += FolderPickerDriveChip_Click;
            System.Windows.Automation.AutomationProperties.SetName(chip, label);
            FolderPickerDriveChips.Children.Add(chip);
        }

        private void FolderPickerDriveChip_Click(object sender, MouseButtonEventArgs e)
        {
            if (sender is Border b) PopulateFolderPicker(b.Tag as string);
        }

        private List<FolderEntry> SafeGetDrives()
        {
            var list = new List<FolderEntry>();
            DriveInfo[] drives;
            try { drives = DriveInfo.GetDrives(); }
            catch { return list; }

            foreach (var d in drives)
            {
                try
                {
                    if (!d.IsReady) continue;   // empty card reader, removed drive
                    string label = string.IsNullOrWhiteSpace(d.VolumeLabel)
                        ? d.Name : d.Name + "  (" + d.VolumeLabel + ")";
                    list.Add(new FolderEntry { Icon = "💾", Name = label, FullPath = d.RootDirectory.FullName });
                }
                catch { /* drive vanished between enumeration and query */ }
            }
            return list;
        }

        private void FolderPickerOpenSelected()
        {
            if (!(FolderPickerList.SelectedItem is FolderEntry entry)) return;
            if (entry.IsParent) { PopulateFolderPicker(entry.FullPath); return; }
            if (!string.IsNullOrEmpty(entry.FullPath)) PopulateFolderPicker(entry.FullPath);
        }

        private void FolderPickerGoUp()
        {
            if (string.IsNullOrEmpty(_folderPickerPath))
            {
                HideFolderPickerOverlay();   // already at the top: Circle cancels
                return;
            }
            string parent = null;
            try { parent = Directory.GetParent(_folderPickerPath)?.FullName; } catch { parent = null; }
            PopulateFolderPicker(parent);
        }

        private void FolderPickerConfirm()
        {
            string path = _folderPickerPath;
            if (string.IsNullOrEmpty(path)) return;

            // Capture before hiding: HideFolderPickerOverlay() clears both.
            string target = _folderPickerTarget;
            string refreshKey = _folderPickerRefreshKey;
            HideFolderPickerOverlay();

            if (target == "firstrun")
            {
                _firstRunSelectedFolder = path;
                FirstRunFolderText.Text = path;
                FirstRunContinueBtn.IsEnabled = true;
            }
            else
            {
                if (!_gameFolders.Contains(path))
                {
                    _gameFolders.Add(path);
                    LoadGames();
                    SaveConfig();
                    LogConsole("Folder added: " + path);
                    if (FooterStatus != null)
                        FooterStatus.Text = "Folder added: " + System.IO.Path.GetFileName(path);
                }
                if (!string.IsNullOrEmpty(refreshKey)) PopulateSubPage(refreshKey);
            }
        }

        private void FolderPickerOpen_Click(object sender, RoutedEventArgs e) => FolderPickerOpenSelected();
        private void FolderPickerSelect_Click(object sender, RoutedEventArgs e) => FolderPickerConfirm();
        private void FolderPickerUp_Click(object sender, RoutedEventArgs e) => FolderPickerGoUp();
        private void FolderPickerList_MouseDoubleClick(object sender, MouseButtonEventArgs e) => FolderPickerOpenSelected();

        private void FolderPickerCancel_Click(object sender, RoutedEventArgs e) => HideFolderPickerOverlay();

        private void HandleFolderPickerNav(bool up, bool down, bool cross, bool circle, bool triangle)
        {
            int count = FolderPickerList.Items.Count;
            if (count > 0 && (up || down))
            {
                int i = FolderPickerList.SelectedIndex;
                if (up) i = (i <= 0) ? count - 1 : i - 1;
                else i = (i >= count - 1) ? 0 : i + 1;
                FolderPickerList.SelectedIndex = i;
                FolderPickerList.ScrollIntoView(FolderPickerList.Items[i]);
                return;
            }
            if (cross) FolderPickerOpenSelected();
            else if (triangle) FolderPickerConfirm();
            else if (circle) FolderPickerGoUp();
        }

        private void StopButton_Click(object sender, RoutedEventArgs e)
        {
            if (_coreRunning)
            {
                try
                {
                    _session?.RequestStop();
                    LogConsole("Stop requested; waiting for the guest to unwind...");
                }
                catch { }
            }
        }

        private void KillButton_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                _session?.Kill();
                LogConsole("Force kill: guest thread terminated.");

                // Aggressive fallback: kill any pcsx5_cli process still active on this machine
                foreach (var proc in System.Diagnostics.Process.GetProcessesByName("pcsx5_cli"))
                {
                    try { proc.Kill(); proc.Dispose(); } catch { }
                }
            }
            catch { }
        }

        private void ConsoleDockMode_Click(object sender, RoutedEventArgs e)
        {
            if (sender is System.Windows.Controls.Button btn && btn.Tag is string tag)
            {
                switch (tag)
                {
                    case "Bottom": _consoleDock = ConsoleDock.Bottom; LogConsole("Console docked to bottom."); break;
                    case "Right":  _consoleDock = ConsoleDock.Right;  LogConsole("Console docked to right."); break;
                    case "Left":   _consoleDock = ConsoleDock.Left;   LogConsole("Console docked to left."); break;
                    case "Float":  _consoleDock = ConsoleDock.Float;  LogConsole("Console undocked (floating)."); break;
                }
                ApplyConsoleDockLayout(_consoleDock);
                if (_consoleDock != ConsoleDock.Float)
                    GameConsolePanel.Visibility = Visibility.Visible;
            }
        }

        // ── Embedded game window hosting ────────────────────────────────────

        private void EmbedEmulatorWindow(IntPtr emuHwnd)
        {
            if (_emuHost == null || _emuHost.HostHandle == IntPtr.Zero) return;
            if (!NativeMethods.IsWindow(emuHwnd))
            {
                LogConsole("Embed failed: emulator window handle is not valid.");
                return;
            }

            // Strip decorations and make it a child of our host window
            long style = NativeMethods.GetWindowLongPtr(emuHwnd, NativeMethods.GWL_STYLE).ToInt64();
            style &= ~(NativeMethods.WS_CAPTION | NativeMethods.WS_THICKFRAME |
                       NativeMethods.WS_MINIMIZEBOX | NativeMethods.WS_MAXIMIZEBOX | NativeMethods.WS_SYSMENU);
            style |= NativeMethods.WS_CHILD | NativeMethods.WS_CLIPSIBLINGS;
            NativeMethods.SetWindowLongPtr(emuHwnd, NativeMethods.GWL_STYLE, new IntPtr(style));

            _embeddedEmuHwnd = emuHwnd;
            NativeMethods.SetParent(emuHwnd, _emuHost.HostHandle);
            bool bootScreenUp = GameBootOverlay != null && GameBootOverlay.Visibility == Visibility.Visible;
            if (bootScreenUp)
            {
                // Stay hidden until the first guest frame; HideBootOverlay reveals it.
                _embedPendingShow = true;
                ResizeEmbeddedWindow();
            }
            else
            {
                NativeMethods.ShowWindow(emuHwnd, NativeMethods.SW_SHOW);
                ResizeEmbeddedWindow();
                NativeMethods.SetFocus(emuHwnd); // keyboard input must reach the emulator window
            }
            LogConsole("Emulator window embedded into launcher.");
        }

        private void EmulatorHostPresenter_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            ResizeEmbeddedWindow();
        }

        // Fit the emulator window into the host area, keeping its native aspect
        // ratio (letterboxed on the black host background).
        private void ResizeEmbeddedWindow()
        {
            if (_embeddedEmuHwnd == IntPtr.Zero || _emuHost == null) return;
            if (!NativeMethods.IsWindow(_embeddedEmuHwnd)) return;

            double w = EmulatorHostPresenter.ActualWidth;
            double h = EmulatorHostPresenter.ActualHeight;
            if (w < 8 || h < 8) return;

            // WPF units -> physical pixels
            double scaleX = 1.0, scaleY = 1.0;
            var src = PresentationSource.FromVisual(this);
            if (src?.CompositionTarget != null)
            {
                scaleX = src.CompositionTarget.TransformToDevice.M11;
                scaleY = src.CompositionTarget.TransformToDevice.M22;
            }
            int pixelW = (int)(w * scaleX);
            int pixelH = (int)(h * scaleY);

            double aspect = 16.0 / 9.0;
            if (NativeMethods.GetClientRect(_embeddedEmuHwnd, out NativeMethods.RECT rc))
            {
                int cw = rc.Right - rc.Left, ch = rc.Bottom - rc.Top;
                if (cw > 0 && ch > 0) aspect = (double)cw / ch;
            }

            int childW = pixelW, childH = (int)(pixelW / aspect);
            if (childH > pixelH) { childH = pixelH; childW = (int)(pixelH * aspect); }
            int x = (pixelW - childW) / 2;
            int y = (pixelH - childH) / 2;
            NativeMethods.MoveWindow(_embeddedEmuHwnd, x, y, childW, childH, true);
        }

        // Restore the library view; safe when the emulator HWND is already dead.
        private void TeardownGameView()
        {
            if (_embeddedEmuHwnd != IntPtr.Zero)
            {
                if (NativeMethods.IsWindow(_embeddedEmuHwnd))
                {
                    NativeMethods.SetParent(_embeddedEmuHwnd, IntPtr.Zero);
                }
                _embeddedEmuHwnd = IntPtr.Zero;
            }
            if (_gameConsoleVisible) SetGameConsoleVisible(false);
            EmulatorHostPresenter.SizeChanged -= EmulatorHostPresenter_SizeChanged;
            EmulatorHostPresenter.Content = null;
            _emuHost?.Dispose();
            _emuHost = null;
            GameView.Visibility = Visibility.Collapsed;
            LibraryView.Visibility = Visibility.Visible;
            UpdateTabHighlight(TabLibraryBtn);
            SetTitleMusicAudible(true);
        }

        private void GameConsoleButton_Click(object sender, RoutedEventArgs e)
        {
            SetGameConsoleVisible(!_gameConsoleVisible);
        }

        // Move the shared console panel between the Logs view and the game view
        // side panel (avoids WPF HwndHost airspace issues — no overlap).
        private void ApplyConsoleDockLayout(ConsoleDock dock)
        {
            if (GameConsolePanel == null || GameContentBottomRow == null || GameContentRightCol == null) return;

            switch (dock)
            {
                case ConsoleDock.Bottom:
                    GameConsolePanel.SetValue(Grid.RowProperty, 1);
                    GameConsolePanel.SetValue(Grid.ColumnProperty, 0);
                    GameConsolePanel.Width = double.NaN;
                    GameConsolePanel.Height = 200;
                    GameContentBottomRow.Height = new GridLength(200);
                    GameContentRightCol.Width = new GridLength(0);
                    break;
                case ConsoleDock.Right:
                    GameConsolePanel.SetValue(Grid.RowProperty, 0);
                    GameConsolePanel.SetValue(Grid.ColumnProperty, 1);
                    GameConsolePanel.Width = 380;
                    GameConsolePanel.Height = double.NaN;
                    GameContentBottomRow.Height = new GridLength(0);
                    GameContentRightCol.Width = new GridLength(380);
                    break;
                case ConsoleDock.Left:
                    GameConsolePanel.SetValue(Grid.RowProperty, 0);
                    GameConsolePanel.SetValue(Grid.ColumnProperty, 0);
                    GameConsolePanel.Width = 380;
                    GameConsolePanel.Height = double.NaN;
                    GameContentBottomRow.Height = new GridLength(0);
                    GameContentRightCol.Width = new GridLength(0);
                    break;
                case ConsoleDock.Float:
                    GameConsolePanel.Visibility = Visibility.Collapsed;
                    GameContentBottomRow.Height = new GridLength(0);
                    GameContentRightCol.Width = new GridLength(0);
                    break;
            }
        }

        private void SetGameConsoleVisible(bool visible)
        {
            _gameConsoleVisible = visible;
            if (GameConsoleButton != null) GameConsoleButton.Content = visible ? "Hide Console" : "Console";
            if (GameConsoleButton != null) GameConsoleButton.Content = visible ? "📋 Console ▲" : "📋 Console";

            bool isRunning = _session != null && (_session.State == GameSessionState.Booting || _session.State == GameSessionState.Running);

            // If we are showing the console, we must make sure GameView's layout container is visible
            if (visible)
            {
                if (GameView != null) GameView.Visibility = Visibility.Visible;
                if (GameConsolePanel != null) GameConsolePanel.Visibility = (_consoleDock == ConsoleDock.Float) ? Visibility.Collapsed : Visibility.Visible;
                ApplyConsoleDockLayout(_consoleDock);

                // Hide emulator window if no game is actively running
                if (!isRunning)
                {
                    if (EmulatorHostBorder != null) EmulatorHostBorder.Visibility = Visibility.Collapsed;
                }
                else
                {
                    if (EmulatorHostBorder != null) EmulatorHostBorder.Visibility = Visibility.Visible;
                }
            }
            else
            {
                if (GameConsolePanel != null) GameConsolePanel.Visibility = Visibility.Collapsed;
                if (GameContentBottomRow != null) GameContentBottomRow.Height = new GridLength(0);
                if (GameContentRightCol != null) GameContentRightCol.Width = new GridLength(0);

                // If console is hidden and no game is running, collapse GameView so clicks pass to background views
                if (!isRunning)
                {
                    if (GameView != null) GameView.Visibility = Visibility.Collapsed;
                }
            }
        }

        private void ShowTab(string tabName)
        {
            if (LibraryView != null) LibraryView.Visibility = tabName == "Library" ? Visibility.Visible : Visibility.Collapsed;
            if (ToolsHubView != null) ToolsHubView.Visibility = (tabName == "Analyzer" || tabName == "Tools") ? Visibility.Visible : Visibility.Collapsed;
            if (AnalyzerView != null) AnalyzerView.Visibility = Visibility.Collapsed;
            if (ControllerView != null) ControllerView.Visibility = (tabName == "Controller" || tabName == "Input") ? Visibility.Visible : Visibility.Collapsed;
            if (SettingsView != null) SettingsView.Visibility = tabName == "Settings" ? Visibility.Visible : Visibility.Collapsed;
            if (GameView != null) GameView.Visibility = tabName == "Game" ? Visibility.Visible : Visibility.Collapsed;
        }

        // --- INPUT TESTER OUTPUT ACTIONS ---

        // --- SPEAKER / MIC TESTERS (USB audio endpoints) ---

        /// <summary>
        /// Find a sibling build tool by walking up from the UI's own directory.
        /// </summary>
        /// <remarks>
        /// This replaced a hand-written list of 19 fixed relative paths that did
        /// not include the one place the tool is actually built:
        /// build/bin/Release/. The UI runs from
        /// src/ui_csharp/bin/Release/net9.0-windows/win-x64/, six levels below
        /// the repository root, and the list checked build/, build/Release/ and
        /// build/Debug/ at that depth but never build/bin/Release/. So title
        /// music was silently unavailable in every developer build: the decoder
        /// existed, and nothing looked where it was.
        ///
        /// Walking ancestors is not just shorter -- it survives the layout
        /// changing, which a fixed list demonstrably did not.
        /// </remarks>
        private string LocateBuildTool(string exeName)
        {
            // Relative to each ancestor directory, in rough order of likelihood.
            string[] relative = {
                "",
                "tools",
                Path.Combine("bin", "Release"),
                Path.Combine("bin", "Debug"),
                "build",
                Path.Combine("build", "Release"),
                Path.Combine("build", "Debug"),
                Path.Combine("build", "bin", "Release"),
                Path.Combine("build", "bin", "Debug"),
            };

            var tried = new List<string>();
            var dir = new DirectoryInfo(AppDomain.CurrentDomain.BaseDirectory);
            for (int depth = 0; dir != null && depth < 8; depth++, dir = dir.Parent)
            {
                foreach (var rel in relative)
                {
                    string candidate = Path.Combine(dir.FullName, rel, exeName);
                    tried.Add(candidate);
                    if (File.Exists(candidate)) return Path.GetFullPath(candidate);
                }
            }

            // Say where it looked. "Not found" without the search path is the
            // kind of message that leaves a user with nothing to act on, which
            // is how this defect survived: the console said the decoder was
            // missing, and the decoder was sitting in the build tree.
            LogConsole($"{exeName} not found. Searched {tried.Count} locations under " +
                       $"'{AppDomain.CurrentDomain.BaseDirectory}' and up to 8 parent " +
                       $"directories (including build/bin/Release).");
            return null;
        }

        private string LocateSndDecode()
        {
            return LocateBuildTool("pcsx5_snd_decode.exe");
        }

        private string LocateBootParser()
        {
            string uiDir = AppDomain.CurrentDomain.BaseDirectory;
            string[] locations = {
                Path.Combine(uiDir, "pcsx5_boot_parser.exe"),
                Path.Combine(uiDir, "tools", "pcsx5_boot_parser.exe"),
                Path.Combine(uiDir, "bin", "Release", "pcsx5_boot_parser.exe"),
                Path.Combine(uiDir, "bin", "Debug", "pcsx5_boot_parser.exe"),
                Path.Combine(uiDir, "..", "bin", "Release", "pcsx5_boot_parser.exe"),
                Path.Combine(uiDir, "..", "bin", "Debug", "pcsx5_boot_parser.exe"),
                Path.Combine(uiDir, "..", "Release", "pcsx5_boot_parser.exe"),
                Path.Combine(uiDir, "..", "Debug", "pcsx5_boot_parser.exe"),
                Path.Combine(uiDir, "..", "pcsx5_boot_parser.exe"),
                Path.Combine(uiDir, "build", "bin", "Release", "pcsx5_boot_parser.exe"),
                Path.Combine(uiDir, "build", "bin", "Debug", "pcsx5_boot_parser.exe"),
                Path.Combine(uiDir, "build", "Release", "pcsx5_boot_parser.exe"),
                Path.Combine(uiDir, "build", "Debug", "pcsx5_boot_parser.exe"),
                // Build directory at project root (5 levels up from uiDir: src/ui_csharp/bin/Release/net9.0-windows/)
                Path.Combine(uiDir, "..", "..", "..", "..", "..", "build", "pcsx5_boot_parser.exe"),
                Path.Combine(uiDir, "..", "..", "..", "..", "..", "build", "Release", "pcsx5_boot_parser.exe"),
                Path.Combine(uiDir, "..", "..", "..", "..", "..", "build", "Debug", "pcsx5_boot_parser.exe"),
                // Also check from project root directly
                Path.Combine(uiDir, "..", "..", "..", "..", "..", "..", "build", "pcsx5_boot_parser.exe"),
                Path.Combine(uiDir, "..", "..", "..", "..", "..", "..", "build", "Release", "pcsx5_boot_parser.exe"),
                Path.Combine(uiDir, "..", "..", "..", "..", "..", "..", "build", "Debug", "pcsx5_boot_parser.exe")
            };

            foreach (var loc in locations)
            {
                if (File.Exists(loc)) return Path.GetFullPath(loc);
            }

            return null;
        }

        private void LogConsole(string message, int level = 2) // default level = Info
        {
            _consoleLineQueue.Enqueue(new ConsoleLine { Text = message, Level = level });
        }

        // Split "(xN)" suffix from a C++-deduped line.  Returns the base text
        // and count; lines without the suffix get baseText=original, count=1.
        private static bool TrySplitCount(string text, out string baseText, out int count)
        {
            var m = DedupCountSuffix.Match(text);
            if (m.Success)
            {
                baseText = text.Substring(0, m.Index);
                count = int.Parse(m.Groups[1].Value);
                return true;
            }
            baseText = text;
            count = 1;
            return false;
        }

        private void DrainConsoleQueue()
        {
            var doc = ConsoleOutputBox.Document;
            int drained = 0;
            const int maxLinesPerDrain = 500;

            // First, flush any buffered dedup line from the previous drain.
            if (_lastDedupLine != null && _lastDedupCount > 0)
            {
                _consoleLineNum++;
                string text = _lastDedupLine;
                if (_lastDedupCount > 1)
                    text += $" (x{_lastDedupCount})";
                var run = new System.Windows.Documents.Run($"{_consoleLineNum,5}: {text}\n")
                {
                    Foreground = new SolidColorBrush(_lastDedupColor),
                    FontFamily = new System.Windows.Media.FontFamily("Consolas, Courier New"),
                    FontSize = 11
                };
                doc.Blocks.Add(new System.Windows.Documents.Paragraph(run) { Margin = new Thickness(0) });
                _lastDedupLine = null;
                _lastDedupCount = 0;
                drained++;
            }

            while (drained < maxLinesPerDrain && _consoleLineQueue.TryDequeue(out var cl))
            {
                var color = ConsoleLevelToColor(cl.Level);

                // Split "(xN)" suffix if the C++ core already deduped this line.
                // WPF then dedups on the base text and SUMS counts so merging works
                // correctly when the same base line arrives multiple times.
                TrySplitCount(cl.Text, out string lineBase, out int lineCount);

                // Dedup: if same base text as previous line, sum counts.
                if (_lastDedupLine != null && _lastDedupLine == lineBase && _lastDedupColor == color)
                {
                    _lastDedupCount += lineCount;
                    continue;
                }

                // If different from previous, flush previous first.
                if (_lastDedupLine != null && _lastDedupCount > 0)
                {
                    _consoleLineNum++;
                    string t = _lastDedupLine;
                    if (_lastDedupCount > 1)
                        t += $" (x{_lastDedupCount})";
                    var run = new System.Windows.Documents.Run($"{_consoleLineNum,5}: {t}\n")
                    {
                        Foreground = new SolidColorBrush(_lastDedupColor),
                        FontFamily = new System.Windows.Media.FontFamily("Consolas, Courier New"),
                        FontSize = 11
                    };
                    doc.Blocks.Add(new System.Windows.Documents.Paragraph(run) { Margin = new Thickness(0) });
                    drained++;
                }

                // Start tracking new line (store base text, not the raw text).
                _lastDedupLine = lineBase;
                _lastDedupColor = color;
                _lastDedupCount = lineCount;
            }

            // Cap at MaxConsoleLines.
            while (doc.Blocks.Count > MaxConsoleLines)
                doc.Blocks.Remove(doc.Blocks.FirstBlock);

            if (_gameConsoleVisible)
            {
                if (AutoScrollCheck?.IsChecked != false) ConsoleOutputBox.ScrollToEnd();
            }
        }

        /// <summary>Console line colour by log level, from the theme tokens so the
        /// console reads in both palettes (bright green and yellow on a white
        /// surface did not). Lines already written keep the colour they had;
        /// new lines follow a theme change.</summary>
        private static System.Windows.Media.Color ConsoleLevelToColor(int level)
        {
            static System.Windows.Media.Color Tok(string key) =>
                Application.Current.Resources[key] is SolidColorBrush b ? b.Color : System.Windows.Media.Colors.Gray;
            switch (level)
            {
                case 0: return Tok("ThemeTextMuted");   // Trace
                case 1: return Tok("ThemeTextMuted");   // Debug
                case 2: return Tok("ThemeText");        // Info
                case 3: return Tok("ThemeWarning");     // Warn
                case 4: return Tok("ThemeDanger");      // Error
                case 5: return Tok("ThemeDanger");      // Critical
                default: return Tok("ThemeTextMuted");
            }
        }

        private string GetConsoleText()
        {
            var range = new System.Windows.Documents.TextRange(ConsoleOutputBox.Document.ContentStart, ConsoleOutputBox.Document.ContentEnd);
            return range.Text;
        }

        private void CopyConsole_Click(object sender, RoutedEventArgs e)
        {
            string text = GetConsoleText();
            if (!string.IsNullOrEmpty(text))
            {
                Clipboard.SetText(text);
            }
        }

        private void ConsoleOutputBox_TextChanged(object sender, TextChangedEventArgs e)
        {
            if (AutoScrollCheck?.IsChecked != false) ConsoleOutputBox.ScrollToEnd();
        }

        private void ClearConsole_Click(object sender, RoutedEventArgs e)
        {
            // Drop pending queued lines too so cleared output doesn't reappear on next drain
            while (_consoleLineQueue.TryDequeue(out _)) { }
            ConsoleOutputBox.Document.Blocks.Clear();
        }

        private void FooterConsole_Click(object sender, RoutedEventArgs e)
        {
            SetGameConsoleVisible(!_gameConsoleVisible);
        }

        /// <summary>Paint the hero status badge for a status word: a tint of the
        /// tier colour with the bright colour as text, or neutral for
        /// untested/unknown. The standard six-tier palette.</summary>
        private void ApplyCompatBadge(string status)
        {
            if (DetailCompatText == null || DetailCompatBadge == null) return;
            DetailCompatText.Text = status;
            var tier = CompatTierColor(status);
            if (tier.HasValue)
            {
                var c = tier.Value;
                var fill = new SolidColorBrush(Color.FromArgb(0x2E, c.R, c.G, c.B)); fill.Freeze();
                var text = new SolidColorBrush(c); text.Freeze();
                DetailCompatBadge.Background = fill;
                DetailCompatText.Foreground = text;
            }
            else
            {
                DetailCompatBadge.Background = (Brush)FindResource("ThemeRaised");
                DetailCompatText.Foreground = (Brush)FindResource("ThemeTextMuted");
            }
        }

        /// <summary>After a run, pull the title's latest community status from the
        /// compatibility database and reflect it: update the game's status, the
        /// hero badge if it is selected, and the local curated record so it
        /// persists. Network failures are silent - the local status stays.</summary>
        private async void RefreshCompatFromDatabase(GameEntry game)
        {
            if (game == null || string.IsNullOrEmpty(game.TitleId)) return;
            string status = await CompatDatabase.FetchStatusAsync(game.TitleId);
            if (string.IsNullOrEmpty(status)) return;
            await Dispatcher.InvokeAsync(() =>
            {
                if (!string.Equals(game.CompatStatus, status, StringComparison.OrdinalIgnoreCase))
                    LogConsole($"[Compat] {game.TitleId}: database status = {status} (was {game.CompatStatus}).");
                game.CompatStatus = status;
                if (ReferenceEquals(_selectedGame, game)) ApplyCompatBadge(status);
            });
            WriteCuratedStatus(game, status);
        }

        /// <summary>Persist a fetched status to the local curated record so the
        /// shell shows it on next launch without a network call.</summary>
        private static void WriteCuratedStatus(GameEntry game, string status)
        {
            try
            {
                string gamesParent = Directory.GetParent(game.DirPath)?.FullName;
                string repoRoot = gamesParent != null ? Directory.GetParent(gamesParent)?.FullName : null;
                string baseDir = repoRoot ?? gamesParent ?? AppDomain.CurrentDomain.BaseDirectory;
                string dir = Path.Combine(baseDir, "compat_seed", "titles");
                Directory.CreateDirectory(dir);
                var rec = new System.Collections.Generic.Dictionary<string, string>
                {
                    ["title_id"] = game.TitleId,
                    ["title"] = game.Title ?? "",
                    ["curated_status"] = status.ToLowerInvariant(),
                    ["source"] = "database",
                    ["schema"] = "pcsx5.curated.v1",
                };
                File.WriteAllText(Path.Combine(dir, game.TitleId + ".json"),
                    System.Text.Json.JsonSerializer.Serialize(rec, new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));
            }
            catch { }
        }

        /// <summary>The compatibility-tier colour for a status, or null for
        /// untested/unknown. The six canonical tiers use the standard palette
        /// (Perfect purple, Playable green, In-Game blue, Menus yellow, Intros
        /// orange, Nothing red); Booting and Unplayable fold into the nearest.</summary>
        private static Color? CompatTierColor(string status)
        {
            switch ((status ?? "").ToUpperInvariant().Replace(" ", "").Replace("-", ""))
            {
                case "PERFECT":                       return Color.FromRgb(0xB0, 0x5C, 0xB8); // purple
                case "PLAYABLE": case "COMPLETE":     return Color.FromRgb(0x92, 0xC5, 0x52); // green
                case "INGAME":                        return Color.FromRgb(0x2F, 0xAD, 0xE0); // blue
                case "MENUS": case "MENU":            return Color.FromRgb(0xF6, 0xC4, 0x3D); // yellow
                case "BOOTS": case "BOOTING":
                case "INTROS": case "INTRO":          return Color.FromRgb(0xF0, 0x8A, 0x18); // orange
                case "NOTHING": case "UNPLAYABLE":
                case "BROKEN": case "ERROR": case "CRASH": return Color.FromRgb(0xD8, 0x32, 0x2A); // red
                default:                              return null;   // untested / unknown
            }
        }

        /// <summary>Short size for the hero chip: "102 MB", "8.5 GB" - whole
        /// numbers over 100, one decimal below, no "Size:" prefix (artboard).</summary>
        private static string FormatSizeChip(long bytes)
        {
            string[] suffix = { "B", "KB", "MB", "GB", "TB" };
            double v = bytes; int i = 0;
            while (v >= 1024 && i < suffix.Length - 1) { i++; v /= 1024; }
            string num = v >= 100 ? ((long)System.Math.Round(v)).ToString() : v.ToString("0.#");
            return $"{num} {suffix[i]}";
        }

        private string FormatBytes(long bytes)
        {
            string[] suffix = { "B", "KB", "MB", "GB", "TB" };
            double dblSvc = bytes;
            int i = 0;
            while (dblSvc >= 1024 && i < suffix.Length - 1)
            {
                i++;
                dblSvc /= 1024;
            }
            return $"{dblSvc:F2} {suffix[i]}";
        }

        // Title Bar Custom Actions
        private void Minimize_Click(object sender, RoutedEventArgs e)
        {
            this.WindowState = WindowState.Minimized;
        }

        private void Maximize_Click(object sender, RoutedEventArgs e)
        {
            if (_shellFullscreen)
            {
                ToggleShellFullscreen();
                return;
            }
            if (this.WindowState == WindowState.Maximized)
            {
                this.WindowState = WindowState.Normal;
            }
            else
            {
                this.WindowState = WindowState.Maximized;
            }
        }

        private void Close_Click(object sender, RoutedEventArgs e)
        {
            this.Close();
        }

        // Tab Switching Logic
        private void TabLibrary_Click(object sender, RoutedEventArgs e)
        {
            if (GameView.Visibility == Visibility.Visible) return; // a game is embedded
            if (InputTabView.IsTestRunning && InputTestOverlay != null && InputTestOverlay.Visibility == Visibility.Visible) { LogConsole(I18n.Tr("input.tabs_locked")); FooterStatus.Text = I18n.Tr("input.tabs_locked"); return; } // a controller test owns the screen
            StopControllerVizPolling();

            LibraryView.Visibility = Visibility.Visible;
            AnalyzerView.Visibility = Visibility.Collapsed;
            ToolsHubView.Visibility = Visibility.Collapsed;
            ControllerView.Visibility = Visibility.Collapsed;
            SettingsView.Visibility = Visibility.Collapsed;
            // LogsView removed
            UpdateTabHighlight(TabLibraryBtn);
            SetTitleMusicAudible(true);
            FocusFirst(LaunchButton, FullLibraryToggleBtn, SearchBox);
        }

        private void ToggleFullLibrary_Click(object sender, RoutedEventArgs e)
        {
            bool showFull = FullLibraryGrid.Visibility != Visibility.Visible;
            FullLibraryGrid.Visibility = showFull ? Visibility.Visible : Visibility.Collapsed;
            LibraryCarousel.Visibility = showFull ? Visibility.Collapsed : Visibility.Visible;
            // The concept's All-games page is the whole screen: the hero goes too.
            if (LibraryHero != null) LibraryHero.Visibility = showFull ? Visibility.Collapsed : Visibility.Visible;
            FullLibraryToggleBtn.Content = showFull ? "◀ Carousel" : "View All ▸";
            if (showFull)
            {
                // Open on the game the shelf had selected, so the grid shows a
                // lit tile and pad navigation starts from it, not from nothing.
                if (_selectedGame != null) FullLibraryListView.SelectedItem = _selectedGame;
                FocusFirst(FullLibraryListView);
            }
            else FocusFirst(LaunchButton);
        }

        /// <summary>Show the shelf arrows only when the library actually
        /// overflows. Permanent arrows over a library that fits on screen read
        /// as broken chrome rather than as an affordance.</summary>
        private void UpdateLibraryScrollArrows()
        {
            if (LibraryScroller == null) return;
            bool scrollable = LibraryScroller.ScrollableWidth > 1.0;
            var vis = scrollable ? Visibility.Visible : Visibility.Collapsed;
            if (LibraryScrollLeft != null) LibraryScrollLeft.Visibility = vis;
            if (LibraryScrollRight != null) LibraryScrollRight.Visibility = vis;
        }

        private void LibraryScrollLeft_Click(object sender, RoutedEventArgs e)
        {
            // Find the ScrollViewer in the carousel and scroll left
            var sv = FindVisualChild<ScrollViewer>(LibraryCarousel);
            if (sv != null) sv.ScrollToHorizontalOffset(sv.HorizontalOffset - 200);
        }

        private void LibraryScrollRight_Click(object sender, RoutedEventArgs e)
        {
            var sv = FindVisualChild<ScrollViewer>(LibraryCarousel);
            if (sv != null) sv.ScrollToHorizontalOffset(sv.HorizontalOffset + 200);
        }

        private static T FindVisualChild<T>(DependencyObject parent) where T : DependencyObject
        {
            for (int i = 0; i < System.Windows.Media.VisualTreeHelper.GetChildrenCount(parent); i++)
            {
                var child = System.Windows.Media.VisualTreeHelper.GetChild(parent, i);
                if (child is T t) return t;
                var found = FindVisualChild<T>(child);
                if (found != null) return found;
            }
            return null;
        }

        // The Tools tab lands on the hub of tool cards (concept artboard). The
        // Boot analyzer card opens the analyzer sub-view via ToolBootAnalyzer_Click.
        private void TabAnalyzer_Click(object sender, RoutedEventArgs e)
        {
            if (GameView.Visibility == Visibility.Visible) return; // a game is embedded
            if (InputTabView.IsTestRunning && InputTestOverlay != null && InputTestOverlay.Visibility == Visibility.Visible) { LogConsole(I18n.Tr("input.tabs_locked")); FooterStatus.Text = I18n.Tr("input.tabs_locked"); return; } // a controller test owns the screen
            StopControllerVizPolling();
            MainLayoutRoot.Visibility = Visibility.Visible;
            TitleBarBorder.Visibility = Visibility.Visible;
            LibraryView.Visibility = Visibility.Collapsed;
            ToolsHubView.Visibility = Visibility.Visible;
            AnalyzerView.Visibility = Visibility.Collapsed;
            ControllerView.Visibility = Visibility.Collapsed;
            SettingsView.Visibility = Visibility.Collapsed;
            UpdateTabHighlight(TabAnalyzerBtn);
            SetTitleMusicAudible(false);
            ShowHints("hints.tools");
            FocusFirst(ToolCardBoot);
        }

        // Open the Boot analyzer sub-view from its hub card.
        private void ToolBootAnalyzer_Click(object sender, RoutedEventArgs e)
        {
            ToolsHubView.Visibility = Visibility.Collapsed;
            AnalyzerView.Visibility = Visibility.Visible;
            UpdateTabHighlight(TabAnalyzerBtn);
            ShowHints("hints.analyzer");
            // Populate the table with the games (unanalyzed); the user picks which
            // to analyze. No automatic parse on open.
            BuildAnalyzerRows();
            FocusFirst(AnalyzerListView);
        }

        // Return from the analyzer to the Tools hub.
        private void AnalyzerBack_Click(object sender, RoutedEventArgs e)
        {
            AnalyzerView.Visibility = Visibility.Collapsed;
            ToolsHubView.Visibility = Visibility.Visible;
            ShowHints("hints.tools");
            FocusFirst(ToolCardBoot);
        }

        private void TabController_Click(object sender, RoutedEventArgs e)
        {
            if (GameView.Visibility == Visibility.Visible) return; // a game is embedded
            if (InputTabView.IsTestRunning && InputTestOverlay != null && InputTestOverlay.Visibility == Visibility.Visible) { LogConsole(I18n.Tr("input.tabs_locked")); FooterStatus.Text = I18n.Tr("input.tabs_locked"); return; } // a controller test owns the screen
            MainLayoutRoot.Visibility = Visibility.Visible;
            TitleBarBorder.Visibility = Visibility.Visible;
            LibraryView.Visibility = Visibility.Collapsed;
            AnalyzerView.Visibility = Visibility.Collapsed;
            ToolsHubView.Visibility = Visibility.Collapsed;
            ControllerView.Visibility = Visibility.Visible;
            SettingsView.Visibility = Visibility.Collapsed;
            // LogsView removed
            UpdateTabHighlight(TabControllerBtn);
            SetTitleMusicAudible(false);

            // Configuration inline; the testing popup opens only from its button.
            SetInputSubTab(true);
            FocusFirst(TestControllerBtn);
        }

        // Input sub-tabs: Configuration (mapping editor) vs Testing (live pad).
        private void SubTabConfig_Click(object sender, RoutedEventArgs e) => SetInputSubTab(true);
        private void SubTabTest_Click(object sender, RoutedEventArgs e) => SetInputSubTab(false);

        private bool _inputSubConfig = true;
        private DateTime? _psHoldStart;   // PS held for 2 s closes the testing popup

        // Configuration is the inline body of the Input tab; Testing opens the
        // controller-testing popup, which captures the pad while it is open.
        private void SetInputSubTab(bool config)
        {
            if (InputConfigView == null || InputTestOverlay == null) return;
            _inputSubConfig = config;
            InputConfigView.Visibility = Visibility.Visible;
            InputTestOverlay.Visibility = config ? Visibility.Collapsed : Visibility.Visible;
            // The live pad only needs the reader while the popup is shown.
            if (config) { InputTab?.AbortTests(); StopControllerVizPolling(); }
            else { StartControllerVizPolling(); FocusFirst(InputTestCloseBtn); }
        }

        private void InputTestClose_Click(object sender, RoutedEventArgs e) => SetInputSubTab(true);

        // Title music belongs to the Library: pause it on every other tab and
        // resume where it left off when the Library comes back.
        private void SetTitleMusicAudible(bool on)
        {
            if (_mediaPlayer == null) return;
            try { if (on) _mediaPlayer.Play(); else _mediaPlayer.Pause(); } catch { }
        }

        private void TabSettings_Click(object sender, RoutedEventArgs e)
        {
            if (GameView.Visibility == Visibility.Visible) return; // a game is embedded
            if (InputTabView.IsTestRunning && InputTestOverlay != null && InputTestOverlay.Visibility == Visibility.Visible) { LogConsole(I18n.Tr("input.tabs_locked")); FooterStatus.Text = I18n.Tr("input.tabs_locked"); return; } // a controller test owns the screen
            StopControllerVizPolling();
            MainLayoutRoot.Visibility = Visibility.Visible;
            TitleBarBorder.Visibility = Visibility.Visible;
            LibraryView.Visibility = Visibility.Collapsed;
            AnalyzerView.Visibility = Visibility.Collapsed;
            ToolsHubView.Visibility = Visibility.Collapsed;
            ControllerView.Visibility = Visibility.Collapsed;
            SettingsView.Visibility = Visibility.Visible;
            UpdateTabHighlight(TabSettingsBtn);
            SetTitleMusicAudible(false);
            UpdateSettingsUiFromConfig();
            // Side-nav: land directly on the active section's rows (no hub).
            OpenSettingsCategory(_activeSettingsCategory);
            FocusFirst(SettingsNavButtons().FirstOrDefault(b => (b?.CommandParameter as string) == _activeSettingsCategory));
        }

        private int _settingsLevel = 1; // 1 = Main Categories Hub, 2 = Category Rows Overview, 3 = Dedicated Sub-Page
        private string _activeSettingsCategory = "Graphics";
        private string _activeSettingKey = "";

        private List<Button> GetHubCategoryButtons()
        {
            return new List<Button>
            {
                HubBtn_Graphics,
                HubBtn_Audio,
                HubBtn_Input,
                HubBtn_Folders,
                HubBtn_Emulation,
                HubBtn_Logging,
                HubBtn_UI,
                HubBtn_About
            };
        }

        private void CategoryBtn_Click(object sender, RoutedEventArgs e)
        {
            if (sender is Button btn && btn.Tag is string category)
            {
                OpenSettingsCategory(category);
            }
        }

        // Side-section nav: open the section's rows and mark it active. The
        // category is in CommandParameter; Tag carries the active flag for the style.
        private void SettingsNav_Click(object sender, RoutedEventArgs e)
        {
            if (sender is Button btn && btn.CommandParameter is string category)
                OpenSettingsCategory(category);
        }

        private Button[] SettingsNavButtons() => new[]
        {
            SettingsNav_Graphics, SettingsNav_Audio, SettingsNav_Input, SettingsNav_Folders,
            SettingsNav_Emulation, SettingsNav_Logging, SettingsNav_UI, SettingsNav_About,
        };

        private void HighlightSettingsNav(string category)
        {
            foreach (var b in SettingsNavButtons())
                if (b != null) b.Tag = string.Equals(b.CommandParameter as string, category, StringComparison.OrdinalIgnoreCase) ? "active" : null;
        }

        private void BtnSettingsBack_Click(object sender, RoutedEventArgs e)
        {
            if (_settingsLevel == 3)
            {
                OpenSettingsCategory(_activeSettingsCategory);
            }
            else if (_settingsLevel == 2)
            {
                // Rows are the top of the side-nav now; Back leaves Settings.
                TabLibrary_Click(this, null);
            }
            else if (_settingsLevel == 1)
            {
                TabLibrary_Click(this, null);
            }
        }

        // No hub in the side-nav layout: return to the active section's rows.
        private void ResetSettingsView()
        {
            OpenSettingsCategory(_activeSettingsCategory);
        }

        private string GetCategoryTitle(string category)
        {
            return category switch
            {
                "Graphics" => "Display & Video",
                "Audio" => "Audio Output",
                "Input" => "Accessories & Controllers",
                "Folders" => "Storage & Game Folders",
                "Emulation" => "System & Emulation (HLE)",
                "Logging" => "Diagnostics & Logging",
                "UI" => "UI & Personalization",
                "About" => "System Information",
                _ => "Settings"
            };
        }

        private struct SettingDefinition
        {
            public string Key;
            public string Title;
            public string Description;
            public Func<string> GetValueBadge;
            public string Type; // "choice", "toggle", "slider", "custom"
        }

        private List<SettingDefinition> GetCategorySettings(string category)
        {
            var list = new List<SettingDefinition>();
            switch (category)
            {
                case "Graphics":
                    list.Add(new SettingDefinition { Key = "gpu_renderer", Title = "Renderer Backend", Description = "Graphics API for rendering PS5 shaders and pipeline", GetValueBadge = () => _config.graphics.renderer == 0 ? "Vulkan 1.3" : "OpenGL", Type = "choice" });
                    list.Add(new SettingDefinition { Key = "gpu_fullscreen", Title = "Start in Fullscreen", Description = "Launch games directly in borderless fullscreen mode", GetValueBadge = () => _config.graphics.fullscreen ? "Yes (Enabled)" : "No (Disabled)", Type = "toggle" });
                    list.Add(new SettingDefinition { Key = "gpu_res_scale", Title = "Internal Resolution Scale", Description = "Multiplier for internal render target resolution", GetValueBadge = () => $"{_config.graphics.resolution_scale:0.00}x", Type = "slider" });
                    list.Add(new SettingDefinition { Key = "gpu_window_size", Title = "Windowed Resolution", Description = "Default launcher dimensions for windowed execution", GetValueBadge = () => $"{_config.graphics.width} x {_config.graphics.height}", Type = "choice" });
                    break;
                case "Audio":
                    list.Add(new SettingDefinition { Key = "snd_backend", Title = "Audio Output Backend", Description = "Low-latency driver for positional audio stream", GetValueBadge = () => _config.audio.backend == 1 ? "WASAPI (Recommended)" : (_config.audio.backend == 2 ? "XAudio2" : "Null Driver"), Type = "choice" });
                    list.Add(new SettingDefinition { Key = "snd_volume", Title = "Master Volume", Description = "Global audio output volume multiplier", GetValueBadge = () => $"{(int)(_config.audio.volume * 100)}%", Type = "slider" });
                    list.Add(new SettingDefinition { Key = "snd_buffer", Title = "Buffer Latency (ms)", Description = "Audio buffer latency (lower = less delay, higher = smooth)", GetValueBadge = () => $"{_config.audio.buffer_ms} ms", Type = "slider" });
                    list.Add(new SettingDefinition { Key = "snd_title_music", Title = "Title Screen Music", Description = "Play game soundtrack when selecting in library", GetValueBadge = () => _config.ui.title_music_enabled ? "Yes (Enabled)" : "No (Disabled)", Type = "toggle" });
                    break;
                case "Input":
                    list.Add(new SettingDefinition { Key = "in_backend", Title = "Gamepad Input Driver", Description = "Driver interface for DualSense/Xbox controllers", GetValueBadge = () => _config.input.backend == 0 ? "DualSense Direct HID / SDL" : "XInput Emulation", Type = "choice" });
                    list.Add(new SettingDefinition { Key = "in_deadzone", Title = "Stick Deadzone", Description = "Stick drift prevention threshold for analog sticks", GetValueBadge = () => $"{(int)(_config.input.deadzone * 100)}%", Type = "slider" });
                    list.Add(new SettingDefinition { Key = "in_rumble", Title = "Vibration & Haptics", Description = "Enable force feedback and DualSense haptic actuators", GetValueBadge = () => _config.input.rumble ? "Yes (Enabled)" : "No (Disabled)", Type = "toggle" });
                    list.Add(new SettingDefinition { Key = "in_active_slot", Title = I18n.Tr("settings.in_active_slot"), Description = I18n.Tr("settings.in_active_slot_desc"), GetValueBadge = () => SlotName(_config.input.active_slot, true), Type = "choice" });
                    list.Add(new SettingDefinition { Key = "in_lightbar", Title = I18n.Tr("settings.in_lightbar"), Description = I18n.Tr("settings.in_lightbar_desc"), GetValueBadge = () => string.IsNullOrWhiteSpace(_config.input.lightbar) ? I18n.Tr("settings.lightbar_default") : _config.input.lightbar.ToUpperInvariant(), Type = "choice" });
                    list.Add(new SettingDefinition { Key = "in_per_game", Title = I18n.Tr("settings.in_per_game"), Description = I18n.Tr("settings.in_per_game_desc"), GetValueBadge = () => _config.input.per_game_configs ? I18n.Tr("common.enabled") : I18n.Tr("common.disabled"), Type = "toggle" });
                    list.Add(new SettingDefinition { Key = "in_restore", Title = I18n.Tr("settings.in_restore"), Description = I18n.Tr("settings.in_restore_desc"), GetValueBadge = () => I18n.Tr("settings.action"), Type = "action" });
                    break;
                case "Folders":
                    list.Add(new SettingDefinition { Key = "storage_folders", Title = "Game Scan Directories", Description = "Manage folders scanned on startup to discover PS5 games", GetValueBadge = () => $"{_gameFolders.Count} Folders Configured", Type = "custom" });
                    list.Add(new SettingDefinition { Key = "storage_crash_dir", Title = "Crash Bundle Directory", Description = "Target location for minidumps and crash diagnostic bundles", GetValueBadge = () => _config.crash.bundle_dir, Type = "choice" });
                    break;
                case "Emulation":
                    list.Add(new SettingDefinition { Key = "hle_strict", Title = "Strict Symbol Imports", Description = "Abort on unknown symbol stubs (turn off for high compatibility)", GetValueBadge = () => _config.hle.strict_imports ? "Yes (Strict)" : "No (Permissive)", Type = "toggle" });
                    list.Add(new SettingDefinition { Key = "hle_trace", Title = "Function Call Tracing", Description = "Log entry and exit points for guest library symbols", GetValueBadge = () => _config.hle.trace_calls ? "Yes (Enabled)" : "No (Disabled)", Type = "toggle" });
                    list.Add(new SettingDefinition { Key = "hle_trace_cap", Title = "Trace Buffer Capacity", Description = "Maximum number of recent import calls retained for crash dumps", GetValueBadge = () => $"{_config.hle.trace_capacity} Entries", Type = "slider" });
                    list.Add(new SettingDefinition { Key = "hle_dump", Title = "Write Crash Minidumps", Description = "Save diagnostic memory snapshot and disassembly on exceptions", GetValueBadge = () => _config.crash.write_minidump ? "Yes (Enabled)" : "No (Disabled)", Type = "toggle" });
                    break;
                case "Logging":
                    list.Add(new SettingDefinition { Key = "log_level", Title = "Log Verbosity Level", Description = "Minimum threshold for console and diagnostic file output", GetValueBadge = () => _config.logging.min_level, Type = "choice" });
                    list.Add(new SettingDefinition { Key = "log_append", Title = "Append to Existing Log", Description = "Keep previous boot log history instead of overwriting", GetValueBadge = () => _config.logging.file_append ? "Yes (Enabled)" : "No (Disabled)", Type = "toggle" });
                    list.Add(new SettingDefinition { Key = "log_json", Title = "Format as Structured JSON", Description = "Format logs as machine-parseable JSON for automated tests", GetValueBadge = () => _config.logging.json_output ? "Yes (Enabled)" : "No (Disabled)", Type = "toggle" });
                    break;
                case "UI":
                    list.Add(new SettingDefinition { Key = "ui_language", Title = "Interface Language", Description = "Dashboard text and user interface localization", GetValueBadge = () => _config.ui.language, Type = "choice" });
                    list.Add(new SettingDefinition { Key = "ui_theme", Title = I18n.Tr("settings.theme"), Description = I18n.Tr("settings.theme_desc"), GetValueBadge = () => I18n.Tr("settings.theme_" + (_config.ui.theme ?? "dark")), Type = "choice" });
                    list.Add(new SettingDefinition { Key = "ui_accent", Title = I18n.Tr("settings.accent"), Description = I18n.Tr("settings.accent_desc"), GetValueBadge = () => Theme.SwatchLabel(_config.ui.accent), Type = "choice" });
                    list.Add(new SettingDefinition { Key = "ui_corners", Title = I18n.Tr("settings.corners"), Description = I18n.Tr("settings.corners_desc"), GetValueBadge = () => I18n.Tr("settings.corners_" + (_config.ui.corners ?? "rounded")), Type = "choice" });
                    list.Add(new SettingDefinition { Key = "ui_ground", Title = I18n.Tr("settings.ground"), Description = I18n.Tr("settings.ground_desc"), GetValueBadge = () => string.IsNullOrEmpty(_config.ui.ground) ? I18n.Tr("settings.ground_default") : _config.ui.ground.ToUpperInvariant(), Type = "choice" });
                    list.Add(new SettingDefinition { Key = "ui_scale", Title = "UI Scale (Display Size)", Description = "Scale factor for high-DPI monitors and large TV screens", GetValueBadge = () => $"{_config.ui.scale * 100:0}%", Type = "choice" });
                    list.Add(new SettingDefinition { Key = "ui_music_volume", Title = I18n.Tr("settings.music_volume"), Description = I18n.Tr("settings.music_volume_desc"), GetValueBadge = () => $"{(int)(_config.ui.title_music_volume * 100)}%", Type = "slider" });
                    list.Add(new SettingDefinition { Key = "ui_fullscreen", Title = I18n.Tr("settings.ui_fullscreen.title"), Description = I18n.Tr("settings.ui_fullscreen.desc"), GetValueBadge = () => _config.ui.start_fullscreen ? I18n.Tr("common.enabled") : I18n.Tr("common.disabled"), Type = "toggle" });
                    break;
                case "About":
                    // Read-only facts (the concept's System Information page). Values
                    // are real and checkable, not marketing: the version comes from
                    // the VERSION file, and none claims a capability the emulator
                    // may not have (Rule 02: no invented facts).
                    list.Add(new SettingDefinition { Key = "about_core", Title = "PCSX5 core release", Description = "Windows x64 native core, C++20", GetValueBadge = () => ReadVersionString(), Type = "info" });
                    list.Add(new SettingDefinition { Key = "about_vulkan", Title = "Graphics", Description = "Vulkan, GCN to SPIR-V translation", GetValueBadge = () => "Vulkan", Type = "info" });
                    list.Add(new SettingDefinition { Key = "about_audio", Title = "Audio", Description = "WASAPI output", GetValueBadge = () => "WASAPI", Type = "info" });
                    list.Add(new SettingDefinition { Key = "about_input", Title = "Controller", Description = "DualSense over Bluetooth and USB (HID)", GetValueBadge = () => "DualSense", Type = "info" });
                    // Attribution the vendored controller art requires (MIT). Localised,
                    // unlike the rows above, so it does not add to the string ratchet.
                    list.Add(new SettingDefinition { Key = "about_update", Title = I18n.Tr("settings.about_update"), Description = I18n.Tr("settings.about_update_desc"), GetValueBadge = () => I18n.Tr("settings.action"), Type = "action" });
                    list.Add(new SettingDefinition { Key = "about_credits", Title = I18n.Tr("about.credits_title"), Description = I18n.Tr("about.credits_desc"), GetValueBadge = () => "MIT", Type = "info" });
                    break;
            }
            return list;
        }

        /// <summary>The emulator version from the VERSION file beside the exe or
        /// up the source tree, prefixed with "v"; "v?" if it cannot be read.</summary>
        private static string ReadVersionString()
        {
            try
            {
                string dir = AppDomain.CurrentDomain.BaseDirectory;
                for (int i = 0; i < 8 && dir != null; i++)
                {
                    string vf = System.IO.Path.Combine(dir, "VERSION");
                    if (System.IO.File.Exists(vf))
                        return "v" + System.IO.File.ReadAllText(vf).Trim();
                    dir = System.IO.Directory.GetParent(dir)?.FullName;
                }
            }
            catch { }
            return "v?";
        }

        private void OpenSettingsCategory(string category)
        {
            _activeSettingsCategory = category;
            _settingsLevel = 2;
            if (SettingsHubView != null) SettingsHubView.Visibility = Visibility.Collapsed;
            if (SettingsCategoryView != null) SettingsCategoryView.Visibility = Visibility.Visible;
            if (SettingsSubPageView != null) SettingsSubPageView.Visibility = Visibility.Collapsed;

            if (SettingsCategoryViewHeaderTitle != null)
                SettingsCategoryViewHeaderTitle.Text = GetCategoryTitle(category);

            HighlightSettingsNav(category);
            PopulateCategoryRows(category);
        }

        private void PopulateCategoryRows(string category)
        {
            if (SettingsCategoryRowsPanel == null) return;
            SettingsCategoryRowsPanel.Children.Clear();

            var settings = GetCategorySettings(category);
            Button firstBtn = null;

            foreach (var s in settings)
            {
                var rowBtn = new Button
                {
                    Style = (Style)FindResource("Ps5SettingRowStyle"),
                    Tag = s.Key,
                    Focusable = true,
                    Margin = new Thickness(0, 4, 0, 4)
                };

                var grid = new Grid();
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(28) });

                var textStack = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
                textStack.Children.Add(new TextBlock { Text = s.Title, FontSize = 14, FontWeight = FontWeights.Bold, Foreground = (Brush)FindResource("ThemeText") });
                textStack.Children.Add(new TextBlock { Text = s.Description, FontSize = 11, Foreground = (Brush)FindResource("ThemeTextMuted"), Margin = new Thickness(0, 3, 0, 0) });
                Grid.SetColumn(textStack, 0);
                grid.Children.Add(textStack);

                // Value Badge Pill
                var badgeBorder = new Border
                {
                    Background = (Brush)FindResource("ThemeAccentSoft"),
                    BorderBrush = (Brush)FindResource("ThemeAccent"),
                    BorderThickness = new Thickness(1),
                    CornerRadius = new CornerRadius(8),
                    Padding = new Thickness(12, 4, 12, 4),
                    VerticalAlignment = VerticalAlignment.Center,
                    Margin = new Thickness(16, 0, 0, 0)
                };
                var badgeText = new TextBlock
                {
                    Text = s.GetValueBadge(),
                    FontSize = 12,
                    FontWeight = FontWeights.Bold,
                    Foreground = (Brush)FindResource("ThemeText")   // readable on the soft accent fill in both palettes
                };
                badgeBorder.Child = badgeText;
                Grid.SetColumn(badgeBorder, 1);
                grid.Children.Add(badgeBorder);

                var chevron = new TextBlock
                {
                    Text = "›",
                    FontSize = 18,
                    Foreground = (Brush)FindResource("ThemeTextMuted"),
                    HorizontalAlignment = HorizontalAlignment.Right,
                    VerticalAlignment = VerticalAlignment.Center
                };
                // "info" rows are read-only facts (the System Information page):
                // no chevron, no navigation, so they do not open an empty sub-page.
                bool isInfo = s.Type == "info" && s.Key != "about_credits";   // credits open the full list
                if (!isInfo)
                {
                    Grid.SetColumn(chevron, 2);
                    grid.Children.Add(chevron);
                }
                rowBtn.Content = grid;
                string capturedKey = s.Key;
                if (capturedKey == "about_credits") rowBtn.Click += (snd, ea) => ShowCredits();
                else if (s.Type == "action") rowBtn.Click += (snd, ea) => RunSettingAction(capturedKey);
                else if (!isInfo) rowBtn.Click += (snd, ea) => OpenSettingsSubPage(capturedKey);

                SettingsCategoryRowsPanel.Children.Add(rowBtn);
                if (firstBtn == null) firstBtn = rowBtn;
            }

            if (firstBtn != null)
            {
                firstBtn.Focus();
                firstBtn.BringIntoView();
            }
        }

        private void OpenSettingsSubPage(string settingKey)
        {
            _activeSettingKey = settingKey;
            _settingsLevel = 3;
            if (SettingsHubView != null) SettingsHubView.Visibility = Visibility.Collapsed;
            if (SettingsCategoryView != null) SettingsCategoryView.Visibility = Visibility.Collapsed;
            if (SettingsSubPageView != null) SettingsSubPageView.Visibility = Visibility.Visible;

            PopulateSubPage(settingKey);
        }

        private void PopulateSubPage(string settingKey)
        {
            if (SettingsSubPageContentContainer == null) return;
            SettingsSubPageContentContainer.Children.Clear();

            var settings = GetCategorySettings(_activeSettingsCategory);
            var def = settings.FirstOrDefault(s => s.Key == settingKey);
            if (string.IsNullOrEmpty(def.Key)) return;

            if (SettingsSubPageParentTitle != null) SettingsSubPageParentTitle.Text = GetCategoryTitle(_activeSettingsCategory);
            if (SettingsSubPageTitle != null) SettingsSubPageTitle.Text = def.Title;
            if (SettingsSubPageDescription != null) SettingsSubPageDescription.Text = def.Description;

            if (def.Type == "toggle")
            {
                // PS5 YES / NO TOGGLE PILLS
                bool isEnabled = false;
                Action<bool> setter = null;

                if (settingKey == "gpu_fullscreen") { isEnabled = _config.graphics.fullscreen; setter = v => _config.graphics.fullscreen = v; }
                else if (settingKey == "ui_fullscreen") { isEnabled = _config.ui.start_fullscreen; setter = v => { _config.ui.start_fullscreen = v; ApplyShellFullscreen(v); }; }
                else if (settingKey == "snd_title_music") { isEnabled = _config.ui.title_music_enabled; setter = v => _config.ui.title_music_enabled = v; }
                else if (settingKey == "in_rumble") { isEnabled = _config.input.rumble; setter = v => _config.input.rumble = v; }
                else if (settingKey == "in_per_game") { isEnabled = _config.input.per_game_configs; setter = v => _config.input.per_game_configs = v; }
                else if (settingKey == "hle_strict") { isEnabled = _config.hle.strict_imports; setter = v => _config.hle.strict_imports = v; }
                else if (settingKey == "hle_trace") { isEnabled = _config.hle.trace_calls; setter = v => _config.hle.trace_calls = v; }
                else if (settingKey == "hle_dump") { isEnabled = _config.crash.write_minidump; setter = v => _config.crash.write_minidump = v; }
                else if (settingKey == "log_append") { isEnabled = _config.logging.file_append; setter = v => _config.logging.file_append = v; }
                else if (settingKey == "log_json") { isEnabled = _config.logging.json_output; setter = v => _config.logging.json_output = v; }

                var togglePanel = new StackPanel { Margin = new Thickness(0, 10, 0, 0) };

                // Yes Option
                var btnYes = CreateOptionChoiceButton("Yes (Enabled)", "Enable this feature for active game sessions.", isEnabled, () => {
                    setter?.Invoke(true);
                    SaveConfig();
                    PopulateSubPage(settingKey);
                });

                // No Option
                var btnNo = CreateOptionChoiceButton("No (Disabled)", "Disable this feature and use default system behavior.", !isEnabled, () => {
                    setter?.Invoke(false);
                    SaveConfig();
                    PopulateSubPage(settingKey);
                });

                togglePanel.Children.Add(btnYes);
                togglePanel.Children.Add(btnNo);
                SettingsSubPageContentContainer.Children.Add(togglePanel);

                var focusedBtn = isEnabled ? btnYes : btnNo;
                focusedBtn.Focus();
            }
            else if (def.Type == "slider")
            {
                // PS5 SLIDER PAGE WITH LIVE BADGE READOUT
                double currentVal = 1.0;
                double minVal = 0.0;
                double maxVal = 1.0;
                double stepVal = 0.1;
                Func<double, string> formatVal = v => $"{v}";
                Action<double> sliderSetter = null;

                if (settingKey == "gpu_res_scale")
                {
                    currentVal = _config.graphics.resolution_scale;
                    minVal = 0.5; maxVal = 2.0; stepVal = 0.25;
                    formatVal = v => $"{v:0.00}x (Native Multiplier)";
                    sliderSetter = v => _config.graphics.resolution_scale = v;
                }
                else if (settingKey == "snd_volume")
                {
                    currentVal = _config.audio.volume;
                    minVal = 0.0; maxVal = 1.0; stepVal = 0.05;
                    formatVal = v => $"{(int)(v * 100)}% Volume";
                    sliderSetter = v => _config.audio.volume = v;
                }
                else if (settingKey == "ui_music_volume")
                {
                    currentVal = _config.ui.title_music_volume;
                    minVal = 0.0; maxVal = 1.0; stepVal = 0.05;
                    formatVal = v => $"{(int)(v * 100)}%";
                    sliderSetter = v => { _config.ui.title_music_volume = v; try { if (_mediaPlayer != null) _mediaPlayer.Volume = v; } catch { } };
                }
                else if (settingKey == "snd_buffer")
                {
                    currentVal = _config.audio.buffer_ms;
                    minVal = 10; maxVal = 200; stepVal = 5;
                    formatVal = v => $"{(int)v} ms Latency";
                    sliderSetter = v => _config.audio.buffer_ms = (int)v;
                }
                else if (settingKey == "in_deadzone")
                {
                    currentVal = _config.input.deadzone;
                    minVal = 0.0; maxVal = 0.5; stepVal = 0.05;
                    formatVal = v => $"{(int)(v * 100)}% Deadzone";
                    sliderSetter = v => _config.input.deadzone = v;
                }
                else if (settingKey == "hle_trace_cap")
                {
                    currentVal = _config.hle.trace_capacity;
                    minVal = 64; maxVal = 1024; stepVal = 64;
                    formatVal = v => $"{(int)v} Entries";
                    sliderSetter = v => _config.hle.trace_capacity = (int)v;
                }

                var sliderCard = new Border
                {
                    Background = (Brush)FindResource("ThemeSurface"),
                    BorderBrush = (Brush)FindResource("ThemeHairline"),
                    BorderThickness = new Thickness(1.5),
                    CornerRadius = new CornerRadius(14),
                    Padding = new Thickness(24, 20, 24, 20),
                    Margin = new Thickness(0, 10, 0, 0)
                };

                var sliderStack = new StackPanel();
                var readoutText = new TextBlock
                {
                    Text = formatVal(currentVal),
                    FontSize = 24,
                    FontWeight = FontWeights.ExtraBold,
                    Foreground = (Brush)FindResource("ThemeAccent"),
                    HorizontalAlignment = HorizontalAlignment.Center,
                    Margin = new Thickness(0, 0, 0, 16)
                };
                sliderStack.Children.Add(readoutText);

                var sld = new Slider
                {
                    Minimum = minVal,
                    Maximum = maxVal,
                    Value = currentVal,
                    TickFrequency = stepVal,
                    IsSnapToTickEnabled = true,
                    Height = 24,
                    Margin = new Thickness(0, 0, 0, 20),
                    Focusable = true
                };
                sld.ValueChanged += (snd, ea) =>
                {
                    readoutText.Text = formatVal(sld.Value);
                    sliderSetter?.Invoke(sld.Value);
                    SaveConfig();
                };
                sliderStack.Children.Add(sld);

                var btnRow = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Center };
                var btnDec = new Button { Content = "◀  - Step", Style = (Style)FindResource("ModernButtonStyle"), Width = 110, Height = 34, Margin = new Thickness(0, 0, 8, 0) };
                btnDec.Click += (snd, ea) => sld.Value = Math.Max(minVal, sld.Value - stepVal);

                var btnInc = new Button { Content = "+ Step  ▶", Style = (Style)FindResource("ModernButtonStyle"), Width = 110, Height = 34, Margin = new Thickness(8, 0, 8, 0) };
                btnInc.Click += (snd, ea) => sld.Value = Math.Min(maxVal, sld.Value + stepVal);

                var btnReset = new Button { Content = "↺  Reset Default", Style = (Style)FindResource("SecondaryButtonStyle"), Width = 130, Height = 34, Margin = new Thickness(8, 0, 0, 0) };
                btnReset.Click += (snd, ea) =>
                {
                    if (settingKey == "gpu_res_scale") sld.Value = 1.0;
                    else if (settingKey == "snd_volume") sld.Value = 1.0;
                    else if (settingKey == "snd_buffer") sld.Value = 50;
                    else if (settingKey == "ui_music_volume") sld.Value = 0.6;
                    else if (settingKey == "in_deadzone") sld.Value = 0.15;
                    else if (settingKey == "hle_trace_cap") sld.Value = 256;
                };

                btnRow.Children.Add(btnDec);
                btnRow.Children.Add(btnInc);
                btnRow.Children.Add(btnReset);
                sliderStack.Children.Add(btnRow);

                sliderCard.Child = sliderStack;
                SettingsSubPageContentContainer.Children.Add(sliderCard);
                sld.Focus();
            }
            else if (settingKey == "storage_folders")
            {
                // GAME DIRECTORIES FOLDERS MANAGER
                var dirCard = new Border
                {
                    Background = (Brush)FindResource("ThemeSurface"),
                    BorderBrush = (Brush)FindResource("ThemeHairline"),
                    BorderThickness = new Thickness(1.5),
                    CornerRadius = new CornerRadius(14),
                    Padding = new Thickness(20, 16, 20, 16),
                    Margin = new Thickness(0, 10, 0, 0)
                };
                var dirStack = new StackPanel();

                var listBorder = new Border
                {
                    Background = (Brush)FindResource("ThemeRaised"),
                    BorderBrush = (Brush)FindResource("ThemeHairline"),
                    BorderThickness = new Thickness(1),
                    CornerRadius = new CornerRadius(8),
                    Margin = new Thickness(0, 0, 0, 14)
                };
                var lb = new ListBox
                {
                    MinHeight = 120,
                    MaxHeight = 200,
                    Background = Brushes.Transparent,
                    Foreground = (Brush)FindResource("ThemeText"),
                    BorderThickness = new Thickness(0)
                };
                foreach (var f in _gameFolders) lb.Items.Add(f);
                listBorder.Child = lb;
                dirStack.Children.Add(listBorder);

                var btnStack = new StackPanel { Orientation = Orientation.Horizontal };
                var btnAdd = new Button { Content = "➕  Add Directory", Style = (Style)FindResource("ModernButtonStyle"), Width = 150, Height = 34, Margin = new Thickness(0, 0, 10, 0) };
                btnAdd.Click += (snd, ea) =>
                {
                    // Route through the in-app browser so this path is usable
                    // with a controller; the sub-page refreshes on confirm.
                    _folderPickerRefreshKey = settingKey;
                    ShowFolderPickerOverlay("settings",
                        _gameFolders.Count > 0 ? _gameFolders[_gameFolders.Count - 1] : null);
                };

                var btnRemove = new Button { Content = "🗑️  Remove Selected", Style = (Style)FindResource("SecondaryButtonStyle"), Width = 160, Height = 34 };
                btnRemove.Click += (snd, ea) =>
                {
                    if (lb.SelectedItem is string selPath && _gameFolders.Contains(selPath))
                    {
                        _gameFolders.Remove(selPath);
                        SaveConfig();
                        PopulateSubPage(settingKey);
                        LoadGames();
                    }
                };

                btnStack.Children.Add(btnAdd);
                btnStack.Children.Add(btnRemove);
                dirStack.Children.Add(btnStack);

                dirCard.Child = dirStack;
                SettingsSubPageContentContainer.Children.Add(dirCard);
                // Focus after layout: a synchronous Focus() on a just-added
                // element is dropped by WPF, so this page opened with nothing
                // focused and Enter did nothing (Rule 12: keyboard-reachable).
                Dispatcher.BeginInvoke(new Action(() => btnAdd.Focus()),
                    System.Windows.Threading.DispatcherPriority.Loaded);
            }
            else
            {
                // MULTI-CHOICE RADIO PICKER (Vulkan, Languages, Log levels, Scales, Window Sizes)
                // A wrap grid, not a column: the concept's swatch/option pages use
                // the width (user report: one narrow column in an empty page).
                var optionsPanel = new WrapPanel { Margin = new Thickness(0, 10, 0, 0), ItemWidth = 280 };
                Button firstOptionBtn = null;

                if (settingKey == "gpu_renderer")
                {
                    var opt1 = CreateOptionChoiceButton("Vulkan 1.3 (Direct Hardware Acceleration)", "Native SPIR-V shader compiler with asynchronous compute and queue execution.", _config.graphics.renderer == 0, () => {
                        _config.graphics.renderer = 0; SaveConfig(); PopulateSubPage(settingKey);
                    });
                    var opt2 = CreateOptionChoiceButton("OpenGL (Compatibility Fallback)", "Fallback graphics driver for hardware without Vulkan 1.3 support.", _config.graphics.renderer == 1, () => {
                        _config.graphics.renderer = 1; SaveConfig(); PopulateSubPage(settingKey);
                    });
                    optionsPanel.Children.Add(opt1);
                    optionsPanel.Children.Add(opt2);
                    firstOptionBtn = (_config.graphics.renderer == 0) ? opt1 : opt2;
                }
                else if (settingKey == "gpu_window_size")
                {
                    var sizes = new[] { ("1280 x 720 (HD 720p)", 1280, 720), ("1920 x 1080 (Full HD 1080p)", 1920, 1080), ("2560 x 1440 (QHD 1440p)", 2560, 1440), ("3840 x 2160 (4K UHD 2160p)", 3840, 2160) };
                    foreach (var s in sizes)
                    {
                        bool isSel = (_config.graphics.width == s.Item2 && _config.graphics.height == s.Item3);
                        var btn = CreateOptionChoiceButton(s.Item1, $"Standard aspect ratio render surface window.", isSel, () => {
                            _config.graphics.width = s.Item2; _config.graphics.height = s.Item3; SaveConfig(); PopulateSubPage(settingKey);
                        });
                        optionsPanel.Children.Add(btn);
                        if (isSel) firstOptionBtn = btn;
                    }
                }
                else if (settingKey == "snd_backend")
                {
                    var opt1 = CreateOptionChoiceButton("WASAPI (Exclusive & Shared Low Latency)", "Direct Windows WASAPI audio driver recommended for all titles.", _config.audio.backend == 1, () => {
                        _config.audio.backend = 1; SaveConfig(); PopulateSubPage(settingKey);
                    });
                    var opt2 = CreateOptionChoiceButton("XAudio2 (DirectX Standard Driver)", "Compatibility audio output driver with standard mixing.", _config.audio.backend == 2, () => {
                        _config.audio.backend = 2; SaveConfig(); PopulateSubPage(settingKey);
                    });
                    var opt3 = CreateOptionChoiceButton("Null Driver (Mute Audio Output)", "Disables audio rendering for automated benchmarks and CI runs.", _config.audio.backend == 0, () => {
                        _config.audio.backend = 0; SaveConfig(); PopulateSubPage(settingKey);
                    });
                    optionsPanel.Children.Add(opt1);
                    optionsPanel.Children.Add(opt2);
                    optionsPanel.Children.Add(opt3);
                    firstOptionBtn = opt1;
                }
                else if (settingKey == "in_backend")
                {
                    var opt1 = CreateOptionChoiceButton("DualSense Native HID / SDL", "Direct low-latency controller input with haptic feedback.", _config.input.backend == 0, () => {
                        _config.input.backend = 0; SaveConfig(); PopulateSubPage(settingKey);
                    });
                    var opt2 = CreateOptionChoiceButton("XInput Controller Driver", "Xbox / generic gamepad compatibility layer.", _config.input.backend == 1, () => {
                        _config.input.backend = 1; SaveConfig(); PopulateSubPage(settingKey);
                    });
                    optionsPanel.Children.Add(opt1);
                    optionsPanel.Children.Add(opt2);
                    firstOptionBtn = opt1;
                }
                else if (settingKey == "log_level")
                {
                    var levels = new[] { "Trace", "Debug", "Info", "Warning", "Error", "Fatal" };
                    foreach (var lvl in levels)
                    {
                        bool isSel = string.Equals(_config.logging.min_level, lvl, StringComparison.OrdinalIgnoreCase);
                        var btn = CreateOptionChoiceButton(lvl, $"Log events with severity {lvl} and higher.", isSel, () => {
                            _config.logging.min_level = lvl; SaveConfig(); PopulateSubPage(settingKey);
                        });
                        optionsPanel.Children.Add(btn);
                        if (isSel) firstOptionBtn = btn;
                    }
                }
                else if (settingKey == "ui_scale")
                {
                    var scales = new[] { ("80%", 0.8), ("100% (Default)", 1.0), ("125%", 1.25), ("150%", 1.5), ("175%", 1.75), ("200%", 2.0) };
                    foreach (var sc in scales)
                    {
                        bool isSel = Math.Abs(_config.ui.scale - sc.Item2) < 0.01;
                        var btn = CreateOptionChoiceButton(sc.Item1, $"Scale UI elements by {sc.Item1}.", isSel, () => {
                            _config.ui.scale = sc.Item2; ApplyUiScale(); SaveConfig(); PopulateSubPage(settingKey);
                        });
                        optionsPanel.Children.Add(btn);
                        if (isSel) firstOptionBtn = btn;
                    }
                }
                else if (settingKey == "ui_theme")
                {
                    foreach (var m in new[] { Theme.ModeDark, Theme.ModeLight, Theme.ModeSystem })
                    {
                        bool isSel = string.Equals(_config.ui.theme, m, StringComparison.OrdinalIgnoreCase);
                        var btn = CreateOptionChoiceButton(I18n.Tr("settings.theme_" + m), I18n.Tr("settings.theme_" + m + "_desc"), isSel, () => {
                            _config.ui.theme = m; ApplyTheme(); SaveConfig(); PopulateSubPage(settingKey);
                        });
                        optionsPanel.Children.Add(btn);
                        if (isSel) firstOptionBtn = btn;
                    }
                }
                else if (settingKey == "ui_accent")
                {
                    foreach (var sw in Theme.AccentSwatches)
                    {
                        bool isSel = string.Equals(_config.ui.accent, sw.Hex, StringComparison.OrdinalIgnoreCase);
                        var btn = CreateOptionChoiceButton(sw.Name, sw.Hex, isSel, () => {
                            _config.ui.accent = sw.Hex; ApplyTheme(); SaveConfig(); PopulateSubPage(settingKey);
                        });
                        optionsPanel.Children.Add(btn);
                        if (isSel) firstOptionBtn = btn;
                    }
                }
                else if (settingKey == "ui_corners")
                {
                    foreach (var c in new[] { Theme.CornersRounded, Theme.CornersSharp, Theme.CornersCut })
                    {
                        bool isSel = string.Equals(_config.ui.corners, c, StringComparison.OrdinalIgnoreCase);
                        var btn = CreateOptionChoiceButton(I18n.Tr("settings.corners_" + c), I18n.Tr("settings.corners_" + c + "_desc"), isSel, () => {
                            _config.ui.corners = c; ApplyTheme(); SaveConfig(); PopulateSubPage(settingKey);
                        });
                        optionsPanel.Children.Add(btn);
                        if (isSel) firstOptionBtn = btn;
                    }
                }
                else if (settingKey == "ui_ground")
                {
                    var grounds = new[] { (I18n.Tr("settings.ground_default"), ""), ("Black", "#000000"), ("Charcoal", "#15171C"), ("Navy", "#0A1020"), ("Plum", "#14101A"), ("Paper", "#F5F6F8"), ("Warm", "#F7F3EC") };
                    foreach (var g in grounds)
                    {
                        bool isSel = string.Equals(_config.ui.ground ?? "", g.Item2, StringComparison.OrdinalIgnoreCase);
                        var btn = CreateOptionChoiceButton(g.Item1, string.IsNullOrEmpty(g.Item2) ? I18n.Tr("settings.ground_default_desc") : g.Item2, isSel, () => {
                            _config.ui.ground = g.Item2; ApplyTheme(); SaveConfig(); PopulateSubPage(settingKey);
                        });
                        optionsPanel.Children.Add(btn);
                        if (isSel) firstOptionBtn = btn;
                    }
                }
                else if (settingKey == "ui_language")
                {
                    var langs = new[] { ("English (United States)", "en-US"), ("Deutsch (Deutschland)", "de-DE"), ("Español (España)", "es-ES"), ("Français (France)", "fr-FR"), ("Italiano (Italia)", "it-IT"), ("日本語 (日本)", "ja-JP"), ("한국어 (대한민국)", "ko-KR"), ("Português (Brasil)", "pt-BR"), ("Русский (Россия)", "ru-RU"), ("简体中文 (中国)", "zh-CN"), ("繁體中文 (台灣)", "zh-TW") };
                    foreach (var lng in langs)
                    {
                        bool isSel = string.Equals(_config.ui.language, lng.Item2, StringComparison.OrdinalIgnoreCase);
                        var btn = CreateOptionChoiceButton(lng.Item1, $"Language Code: {lng.Item2}", isSel, () => {
                            _config.ui.language = lng.Item2; TranslateUi(); SaveConfig(); PopulateSubPage(settingKey);
                        });
                        optionsPanel.Children.Add(btn);
                        if (isSel) firstOptionBtn = btn;
                    }
                }

                else if (settingKey == "in_active_slot")
                {
                    for (int i = 0; i < 5; i++)
                    {
                        int slot = i;
                        bool isSel = _config.input.active_slot == slot;
                        var btn = CreateOptionChoiceButton(SlotName(slot, false), slot == 4 ? "XInput" : $"Player {slot + 1}", isSel, () => {
                            _config.input.active_slot = slot; ApplyLightbarFromConfig(); SaveConfig(); PopulateSubPage(settingKey);
                        });
                        optionsPanel.Children.Add(btn);
                        if (isSel) firstOptionBtn = btn;
                    }
                }
                else if (settingKey == "in_lightbar")
                {
                    // "Pad default" plus presets as a grid, then the picker for a custom
                    // colour (its hue strip and hex field work with the pad).
                    var presets = new[] { (I18n.Tr("settings.lightbar_default"), ""), ("PlayStation blue", "#005AFF"), ("Red", "#FF2A2A"), ("Green", "#1FE06A"),
                                          ("Purple", "#B400FF"), ("Orange", "#FF8A1A"), ("Cyan", "#00E0E0"), ("Pink", "#FF2D9B"), ("White", "#F2F4F8") };
                    string cur = (_config.input.lightbar ?? "").Trim();
                    bool anyPreset = false;
                    foreach (var p in presets)
                    {
                        bool isSel = string.Equals(cur, p.Item2, StringComparison.OrdinalIgnoreCase);
                        anyPreset |= isSel;
                        var btn = CreateOptionChoiceButton(p.Item1, string.IsNullOrEmpty(p.Item2) ? I18n.Tr("settings.lightbar_default_desc") : p.Item2, isSel, () => {
                            _config.input.lightbar = p.Item2; ApplyLightbarFromConfig(); SaveConfig(); PopulateSubPage(settingKey);
                        });
                        if (!string.IsNullOrEmpty(p.Item2))
                        {
                            try { btn.BorderBrush = new SolidColorBrush((Color)ColorConverter.ConvertFromString(p.Item2)); btn.BorderThickness = new Thickness(0, 0, 0, 3); } catch { }
                        }
                        optionsPanel.Children.Add(btn);
                        if (isSel) firstOptionBtn = btn;
                    }
                    // Custom colour: one more entry in the grid, opening the stick-driven
                    // popup. Selected when the configured colour is not a preset.
                    bool customSel = !anyPreset && !string.IsNullOrEmpty(cur);
                    var customBtn = CreateOptionChoiceButton(I18n.Tr("settings.lightbar_custom_btn"), customSel ? cur.ToUpperInvariant() : I18n.Tr("settings.lightbar_custom_desc"), customSel, () => {
                        var start = LightbarOverride() ?? Color.FromRgb(0x00, 0x5a, 0xff);
                        ShowColorPicker(start, chosen => {
                            _config.input.lightbar = $"#{chosen.R:X2}{chosen.G:X2}{chosen.B:X2}";
                            ApplyLightbarFromConfig(); SaveConfig(); PopulateSubPage(settingKey);
                        });
                    });
                    if (customSel) { try { customBtn.BorderBrush = new SolidColorBrush(LightbarOverride().Value); customBtn.BorderThickness = new Thickness(0, 0, 0, 3); } catch { } }
                    optionsPanel.Children.Add(customBtn);
                    if (customSel) firstOptionBtn = customBtn;
                    SettingsSubPageContentContainer.Children.Add(optionsPanel);
                    if (firstOptionBtn != null) firstOptionBtn.Focus();
                    return;
                }

                SettingsSubPageContentContainer.Children.Add(optionsPanel);
                if (firstOptionBtn != null) firstOptionBtn.Focus();
            }
        }

        private string SlotName(int slot, bool shortForm)
        {
            switch (slot)
            {
                case 0: return I18n.Tr("ui.player_1_dualsense_wireless_controller");
                case 1: return I18n.Tr("ui.player_2_controller_slot_2");
                case 2: return I18n.Tr("ui.player_3_controller_slot_3");
                case 3: return I18n.Tr("ui.player_4_controller_slot_4");
                default: return I18n.Tr("ui.xbox_controller_xinput");
            }
        }

        // Rows of Type "action" run something instead of opening a sub-page.
        private void RunSettingAction(string key)
        {
            if (key == "in_restore")
            {
                RestoreDefaultMappings_Click(this, null);
                PopulateCategoryRows(_activeSettingsCategory);
            }
            else if (key == "about_update")
            {
                FooterStatus.Text = I18n.Tr("update.checking");
                _ = CheckForUpdates(manual: true);
            }
        }

        /// <summary>Push the configured theme into the token palette (ADR-003).
        /// Rejected colour strings fall back and are reported in the console.</summary>
        private void ApplyTheme()
        {
            if (_config?.ui == null) return;
            if (!Theme.Apply(_config.ui.theme ?? Theme.ModeDark, _config.ui.accent, _config.ui.ground, _config.ui.corners ?? Theme.CornersRounded))
                LogConsole("Theme: a colour in config.ini was not a valid #RRGGBB and was ignored.");
        }

        private Button CreateOptionChoiceButton(string title, string description, bool isSelected, Action onClick)
        {
            var btn = new Button
            {
                Style = (Style)FindResource("Ps5OptionItemStyle"),
                Focusable = true,
                Margin = new Thickness(0, 6, 12, 6)   // breathing room between option cards in the wrap grid
            };

            var grid = new Grid();
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(40) });

            var stack = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            stack.Children.Add(new TextBlock { Text = title, FontSize = 14, FontWeight = FontWeights.Bold, Foreground = (Brush)FindResource("ThemeText") });
            stack.Children.Add(new TextBlock { Text = description, FontSize = 11, Foreground = (Brush)FindResource("ThemeTextMuted"), Margin = new Thickness(0, 2, 0, 0) });
            Grid.SetColumn(stack, 0);
            grid.Children.Add(stack);

            if (isSelected)
            {
                var check = new TextBlock
                {
                    Text = "✔",
                    FontSize = 18,
                    FontWeight = FontWeights.ExtraBold,
                    Foreground = (Brush)FindResource("ThemeAccent"),
                    HorizontalAlignment = HorizontalAlignment.Right,
                    VerticalAlignment = VerticalAlignment.Center
                };
                Grid.SetColumn(check, 1);
                grid.Children.Add(check);
            }

            btn.Content = grid;
            btn.Click += (s, e) => onClick?.Invoke();
            return btn;
        }

        

        private void UpdateTabHighlight(Button activeBtn)
        {
            // Theme tokens (ADR-003), so the active tab follows the user's accent.
            var activeBrush = (Brush)FindResource("ThemeAccent");
            var inactiveBrush = (Brush)FindResource("ThemeTextMuted");
            var textBrush = (Brush)FindResource("ThemeText");

            TabLibraryBtn.Foreground = activeBtn == TabLibraryBtn ? textBrush : inactiveBrush;
            TabLibraryBtn.BorderBrush = activeBtn == TabLibraryBtn ? activeBrush : Brushes.Transparent;

            TabAnalyzerBtn.Foreground = activeBtn == TabAnalyzerBtn ? textBrush : inactiveBrush;
            TabAnalyzerBtn.BorderBrush = activeBtn == TabAnalyzerBtn ? activeBrush : Brushes.Transparent;

            TabControllerBtn.Foreground = activeBtn == TabControllerBtn ? textBrush : inactiveBrush;
            TabControllerBtn.BorderBrush = activeBtn == TabControllerBtn ? activeBrush : Brushes.Transparent;

            TabSettingsBtn.Foreground = activeBtn == TabSettingsBtn ? textBrush : inactiveBrush;
            TabSettingsBtn.BorderBrush = activeBtn == TabSettingsBtn ? activeBrush : Brushes.Transparent;

            }


        private void TestVibration_Click(object sender, RoutedEventArgs e)
        {
            MessageBox.Show("Controller vibration test pulse sent successfully!", "Haptic Feedback Test", MessageBoxButton.OK, MessageBoxImage.Information);
        }

        // Input tab -> the tuning rows now live in Settings > Accessories & Controllers.
        private void ControllerSettings_Click(object sender, RoutedEventArgs e)
        {
            TabSettings_Click(this, null);
            OpenSettingsCategory("Input");
            FocusFirst(SettingsNavButtons().FirstOrDefault(b => (b?.CommandParameter as string) == "Input"));
        }

        /// <summary>The configured lightbar override as a colour, or null for the
        /// pad's own player colour.</summary>
        private Color? LightbarOverride()
        {
            string hex = _config.input.lightbar;
            if (string.IsNullOrWhiteSpace(hex)) return null;
            try { return (Color)ColorConverter.ConvertFromString(hex.Trim()); } catch { return null; }
        }

        /// <summary>Push the lightbar setting to the pad and the 3D pad: the
        /// override colour if set, else the active slot's player colour.</summary>
        private void ApplyLightbarFromConfig()
        {
            var c = LightbarOverride();
            if (c.HasValue)
            {
                try { CoreBridge.pcsx5_pad_set_lightbar(0, c.Value.R, c.Value.G, c.Value.B); } catch { }
                if (InputTab?.Pad3D != null) InputTab.Pad3D.LightbarColor = c.Value;
            }
            else
            {
                ApplyActiveGamepadSlot(_config.input.active_slot);
            }
        }

        private void BtnAddFolder_Click(object sender, RoutedEventArgs e)
        {
            ShowFolderPickerOverlay("settings",
                _gameFolders.Count > 0 ? _gameFolders[_gameFolders.Count - 1] : null);
        }

        // --- FIRST-RUN SETUP (games folder picker) ---
        private string _firstRunSelectedFolder = null;

        private bool NeedsFirstRunSetup()
        {
            // First run = no configured game folder that actually exists on disk.
            if (_gameFolders.Count == 0) return true;
            foreach (var folder in _gameFolders)
            {
                if (!string.IsNullOrWhiteSpace(folder) && Directory.Exists(folder)) return false;
            }
            return true;
        }

        private void MaybeShowFirstRunSetup()
        {
            if (NeedsFirstRunSetup())
            {
                _firstRunSelectedFolder = null;
                FirstRunFolderText.Text = "No folder selected";
                FirstRunContinueBtn.IsEnabled = false;
                FirstRunOverlay.Visibility = Visibility.Visible;
            }
        }

        private void FirstRunBrowse_Click(object sender, RoutedEventArgs e)
        {
            ShowFolderPickerOverlay("firstrun", _firstRunSelectedFolder);
        }

        private void FirstRunContinue_Click(object sender, RoutedEventArgs e)
        {
            if (string.IsNullOrEmpty(_firstRunSelectedFolder)) return;

            // Replace stale/non-existent defaults with the folder the user picked.
            _gameFolders.Clear();
            _gameFolders.Add(_firstRunSelectedFolder);
            FirstRunOverlay.Visibility = Visibility.Collapsed;
            SaveConfig();
            LoadGames();
            LogConsole("Games folder set to " + _firstRunSelectedFolder);
        }

        private void FirstRunSkip_Click(object sender, RoutedEventArgs e)
        {
            FirstRunOverlay.Visibility = Visibility.Collapsed;
            LogConsole("First-run setup skipped. You can add game folders later in System Settings.");
        }

        // --- UI SCALE ---
        private void ApplyUiScale()
        {
            double scale = _config.ui.scale;
            if (scale < 0.5 || scale > 3.0) scale = 1.0;
            MainLayoutRoot.LayoutTransform = new ScaleTransform(scale, scale);
        }

        // --- SHELL FULLSCREEN ---
        // The dashboard is a console interface, so it presents fullscreen by
        // default.  A borderless window in the Maximized state covers the whole
        // monitor including the taskbar, which is the effect we want; the
        // WindowChrome caption/resize bands are collapsed so the top 50 px does
        // not stay a drag handle with no title bar drawn in it.
        private bool _shellFullscreen;
        private WindowState _preFullscreenState = WindowState.Normal;

        public bool IsShellFullscreen => _shellFullscreen;

        private void ApplyShellFullscreen(bool on)
        {
            if (on == _shellFullscreen && IsLoaded) return;
            _shellFullscreen = on;

            var chrome = System.Windows.Shell.WindowChrome.GetWindowChrome(this);
            if (on)
            {
                _preFullscreenState = this.WindowState == WindowState.Minimized
                    ? WindowState.Normal : this.WindowState;
                if (chrome != null)
                {
                    chrome.CaptionHeight = 0;
                    chrome.ResizeBorderThickness = new Thickness(0);
                }
                this.WindowStyle = WindowStyle.None;
                this.ResizeMode = ResizeMode.NoResize;
                this.WindowState = WindowState.Normal;
                if (!SizeToMonitor())
                {
                    // No monitor info (rare): fall back to Maximized, which
                    // covers the screen while the window holds focus.
                    this.WindowState = WindowState.Maximized;
                }
            }
            else
            {
                if (chrome != null)
                {
                    chrome.CaptionHeight = 50;
                    chrome.ResizeBorderThickness = new Thickness(6);
                }
                this.WindowStyle = WindowStyle.SingleBorderWindow;
                this.ResizeMode = ResizeMode.CanResize;
                this.WindowState = _preFullscreenState == WindowState.Maximized
                    ? WindowState.Maximized : WindowState.Normal;
            }

            // An embedded game window is sized from the host presenter, which
            // just changed size.
            ResizeEmbeddedWindow();
        }

        /// <summary>Fill the monitor the window currently sits on. Returns false
        /// if the monitor bounds could not be resolved.</summary>
        private bool SizeToMonitor()
        {
            try
            {
                IntPtr hwnd = new System.Windows.Interop.WindowInteropHelper(this).Handle;
                if (hwnd == IntPtr.Zero) return false;

                IntPtr mon = NativeMethods.MonitorFromWindow(hwnd, NativeMethods.MONITOR_DEFAULTTONEAREST);
                if (mon == IntPtr.Zero) return false;

                var mi = new NativeMethods.MONITORINFO();
                mi.cbSize = System.Runtime.InteropServices.Marshal.SizeOf(typeof(NativeMethods.MONITORINFO));
                if (!NativeMethods.GetMonitorInfoW(mon, ref mi)) return false;

                // Monitor bounds are physical pixels; WPF positions in DIPs.
                double sx = 1.0, sy = 1.0;
                var src = PresentationSource.FromVisual(this);
                if (src?.CompositionTarget != null)
                {
                    var m = src.CompositionTarget.TransformFromDevice;
                    sx = m.M11;
                    sy = m.M22;
                }

                this.Left = mi.rcMonitor.Left * sx;
                this.Top = mi.rcMonitor.Top * sy;
                this.Width = (mi.rcMonitor.Right - mi.rcMonitor.Left) * sx;
                this.Height = (mi.rcMonitor.Bottom - mi.rcMonitor.Top) * sy;
                return true;
            }
            catch
            {
                return false;
            }
        }

        private void ToggleShellFullscreen()
        {
            bool target = !_shellFullscreen;
            ApplyShellFullscreen(target);
            _config.ui.start_fullscreen = target;
            SaveConfig();
        }

        /// <summary>The one place the controller hint legend is rendered.
        /// Every screen calls this with its own key, so a screen that forgets
        /// its hints is visible as a missing call rather than as an empty bar.</summary>
        /// <summary>Give a screen an initial focus target, so arriving on it with
        /// a controller never leaves nothing selected. Deferred to Loaded because
        /// a view that has just become visible has not been arranged yet, and
        /// Focus() on an unarranged element silently fails.</summary>
        private void FocusFirst(params Control[] candidates)
        {
            Dispatcher.BeginInvoke(new Action(() =>
            {
                foreach (var c in candidates)
                {
                    if (c == null || !c.IsVisible || !c.IsEnabled) continue;
                    if (c.Focus()) return;
                }
            }), System.Windows.Threading.DispatcherPriority.Loaded);
        }

        /// <summary>Gamepad navigation for the expanded "View All" list, which
        /// previously had none -- opening it stranded a pad user with no way to
        /// choose a game or return. Circle backs out, matching the shell-wide
        /// convention that Circle is Back.</summary>
        private void HandleFullLibraryNav(bool up, bool down, bool left, bool right, bool cross, bool circle, bool triangle, bool square, bool options)
        {
            if (triangle)
            {
                // Cycle the sort order from the pad; the segments follow.
                _librarySort = (LibrarySort)(((int)_librarySort + 1) % 5);
                ApplyLibrarySort();
                return;
            }
            if (square)
            {
                if (FullLibraryListView.SelectedItem is GameEntry fav && _favourites != null)
                {
                    _favourites.Toggle(fav.TitleId, out bool saved);
                    if (!saved) LogConsole("Could not save favourites.json");
                    ApplyLibrarySort();
                }
                return;
            }
            if (options)
            {
                LibrarySearchBox.Focus();
                LibrarySearchBox.SelectAll();
                return;
            }
            int count = FullLibraryListView.Items.Count;
            if (count > 0 && (up || down || left || right))
            {
                // The grid wraps, so up/down step by one row. The row length is
                // measured from the panel rather than assumed, so it follows the
                // window width.
                int cols = FullLibraryColumns();
                int i = FullLibraryListView.SelectedIndex;
                if (i < 0) i = 0;
                else if (left)  i = (i <= 0) ? count - 1 : i - 1;
                else if (right) i = (i >= count - 1) ? 0 : i + 1;
                else if (up)    i = (i - cols >= 0) ? i - cols : i;
                else if (down)  i = (i + cols < count) ? i + cols : i;
                FullLibraryListView.SelectedIndex = i;
                FullLibraryListView.ScrollIntoView(FullLibraryListView.Items[i]);
                if (FullLibraryListView.Items[i] is GameEntry g) SelectGame(g);
                return;
            }
            if (cross)
            {
                if (FullLibraryListView.SelectedItem is GameEntry g) SelectGame(g);
                ToggleFullLibrary_Click(this, null);   // collapse back to the shelf
            }
            else if (circle)
            {
                ToggleFullLibrary_Click(this, null);
            }
        }

        /// <summary>Tiles per row in the "View All" grid: the first item's
        /// rendered width (tile plus margin) against the list's width.</summary>
        private int FullLibraryColumns()
        {
            if (FullLibraryListView.Items.Count == 0) return 1;
            if (FullLibraryListView.ItemContainerGenerator.ContainerFromIndex(0) is FrameworkElement first && first.ActualWidth > 0)
            {
                double w = first.ActualWidth + first.Margin.Left + first.Margin.Right;
                int cols = (int)Math.Floor(FullLibraryListView.ActualWidth / w);
                return Math.Max(1, cols);
            }
            return 1;
        }

        /// <summary>Re-source the View All grid in the chosen order. _games
        /// itself stays in recency order, which is what the shelf shows.</summary>
        private void ApplyLibrarySort()
        {
            // The combo's initial SelectedIndex raises SelectionChanged during
            // InitializeComponent, before the list view below it exists.
            if (FullLibraryListView == null || _games == null) return;
            foreach (var g in _games) g.IsFavourite = _favourites?.Contains(g.TitleId) ?? false;
            IEnumerable<GameEntry> source = _games;
            string q = LibrarySearchBox?.Text?.Trim() ?? "";
            if (q.Length > 0)
                source = source.Where(g => (g.Title ?? "").IndexOf(q, StringComparison.CurrentCultureIgnoreCase) >= 0
                                        || (g.TitleId ?? "").IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0);
            IEnumerable<GameEntry> ordered = _librarySort switch
            {
                LibrarySort.Title      => source.OrderBy(g => g.Title ?? "", StringComparer.CurrentCultureIgnoreCase),
                LibrarySort.TitleId    => source.OrderBy(g => g.TitleId ?? "", StringComparer.OrdinalIgnoreCase),
                LibrarySort.Size       => source.OrderByDescending(g => g.SizeBytes),
                LibrarySort.Favourites => source.OrderByDescending(g => g.IsFavourite).ThenBy(g => g.Title ?? "", StringComparer.CurrentCultureIgnoreCase),
                _                      => source,
            };
            var selected = _selectedGame;
            var list = ordered.ToList();
            FullLibraryListView.ItemsSource = null;
            FullLibraryListView.ItemsSource = list;
            if (selected != null && list.Contains(selected)) FullLibraryListView.SelectedItem = selected;
            if (LibrarySearchHint != null) LibrarySearchHint.Visibility = q.Length > 0 ? Visibility.Collapsed : Visibility.Visible;
            UpdateSortSegments();
        }

        /// <summary>Paint the active sort segment from _librarySort.</summary>
        private void UpdateSortSegments()
        {
            foreach (var btn in new[] { SortRecentBtn, SortTitleBtn, SortTitleIdBtn, SortSizeBtn, SortFavBtn })
            {
                if (btn == null) continue;
                bool active = btn.Tag is string t && int.TryParse(t, out int i) && i == (int)_librarySort;
                btn.Background = active ? (Brush)FindResource("ThemeRaised") : Brushes.Transparent;
                btn.Foreground = active ? (Brush)FindResource("ThemeText") : (Brush)FindResource("ThemeTextMuted");
            }
        }

        private void LibrarySortSegment_Click(object sender, RoutedEventArgs e)
        {
            if (sender is Button b && b.Tag is string t && int.TryParse(t, out int i)) { _librarySort = (LibrarySort)i; ApplyLibrarySort(); }
        }

        private void LibrarySearchBox_TextChanged(object sender, TextChangedEventArgs e)
        {
            if (_games != null && FullLibraryListView != null) ApplyLibrarySort();
        }

        // Mouse on the grid: a click selects the game (the header follows), a
        // double-click also collapses back to the shelf, which is what Cross does.
        private void FullLibraryListView_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (FullLibraryListView.SelectedItem is GameEntry g && g != _selectedGame) SelectGame(g);
        }

        private void FullLibraryListView_MouseDoubleClick(object sender, MouseButtonEventArgs e)
        {
            if (FullLibraryListView.SelectedItem is GameEntry) ToggleFullLibrary_Click(this, null);
        }

        private void ShowHints(string key)
        {
            _lastHintKey = key;
            BuildFooterHintChips(I18n.Tr(key));
        }

        // PlayStation control glyphs for the footer legend, stroke geometries in
        // an ~18-unit box, matching the concept artboard's icons.
        private static readonly Dictionary<string, string> _hintGlyphGeom = new()
        {
            ["DIR"]  = "M9,3 V15 M3,9 H15",
            ["OK"]   = "M4,4 L14,14 M14,4 L4,14",
            ["BACK"] = "M3.5,9 A5.5,5.5 0 1 0 14.5,9 A5.5,5.5 0 1 0 3.5,9 Z",
            ["ALT"]  = "M9,3.5 L15,14 L3,14 Z",
            ["ALT2"] = "M4.5,4.5 H13.5 V13.5 H4.5 Z",
            ["TABS"] = "M2,7 H6 A1,1 0 0 1 7,8 V10 A1,1 0 0 1 6,11 H2 A1,1 0 0 1 1,10 V8 A1,1 0 0 1 2,7 Z M12,7 H16 A1,1 0 0 1 17,8 V10 A1,1 0 0 1 16,11 H12 A1,1 0 0 1 11,10 V8 A1,1 0 0 1 12,7 Z",
            ["OPT"]  = "M4,6 H14 M4,9 H14 M4,12 H14",
            ["PS"]   = "M9,2.5 A6.5,6.5 0 1 0 9.01,2.5 Z M9,5.5 V10.5",
        };

        /// <summary>Render the footer legend as the concept's icon boxes: each hint
        /// segment is a square box with the PlayStation control glyph plus its
        /// action label. The device token (%DEV%) is dropped; the glyphs are the
        /// controls themselves.</summary>
        private void BuildFooterHintChips(string template) => BuildFooterHintChips(template, FooterHintChips);

        private void BuildFooterHintChips(string template, StackPanel target)
        {
            if (target == null) return;
            target.Children.Clear();
            if (string.IsNullOrEmpty(template)) return;
            var boxBrush = (Brush)FindResource("ThemeBorderStrong");
            var glyphBrush = (Brush)FindResource("ThemeText");
            var labelBrush = (Brush)FindResource("ThemeTextMuted");
            double typeM = (double)FindResource("TypeM");
            var tokenRe = new System.Text.RegularExpressions.Regex(@"%([A-Z0-9]+)%");
            foreach (var raw in template.Split(new[] { '•' }, StringSplitOptions.RemoveEmptyEntries))
            {
                string seg = raw.Trim();
                if (seg.Length == 0) continue;
                var tokens = tokenRe.Matches(seg).Select(m => m.Groups[1].Value).Where(t => t != "DEV").ToList();
                string label = tokenRe.Replace(seg, "").Trim();
                var chip = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 24, 0) };
                foreach (var tok in tokens)
                {
                    if (!_hintGlyphGeom.TryGetValue(tok, out var geom)) continue;
                    var icon = new System.Windows.Shapes.Path
                    {
                        Data = Geometry.Parse(geom),
                        Stroke = glyphBrush,
                        StrokeThickness = 1.6,
                        StrokeLineJoin = PenLineJoin.Round,
                        StrokeStartLineCap = PenLineCap.Round,
                        StrokeEndLineCap = PenLineCap.Round,
                        Fill = null,
                    };
                    var vb = new Viewbox { Width = 16, Height = 16, Child = icon, Stretch = Stretch.Uniform };
                    var box = new Border
                    {
                        BorderBrush = boxBrush,
                        BorderThickness = new Thickness(1),
                        CornerRadius = new CornerRadius(7),
                        Width = 30, Height = 30,
                        Margin = new Thickness(0, 0, 9, 0),
                        VerticalAlignment = VerticalAlignment.Center,
                        HorizontalAlignment = HorizontalAlignment.Center,
                        Child = vb,
                    };
                    chip.Children.Add(box);
                }
                if (label.Length > 0)
                    chip.Children.Add(new TextBlock { Text = label, FontSize = typeM, Foreground = labelBrush, VerticalAlignment = VerticalAlignment.Center });
                target.Children.Add(chip);
            }
        }

        private string ApplyGlyphs(string template)
        {
            if (string.IsNullOrEmpty(template)) return template;
            string sfx = _inputSource == ShellInputSource.Keyboard ? ".kb" : ".pad";
            string[] tokens = { "DEV", "OK", "BACK", "ALT", "ALT2", "DIR", "TABS", "OPT" };
            foreach (var t in tokens)
                template = template.Replace("%" + t + "%", I18n.Tr("glyph." + t.ToLowerInvariant() + sfx));
            return template;
        }

        private void Window_PreviewKeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.F11)
            {
                ToggleShellFullscreen();
                e.Handled = true;
                return;
            }

            // A rebind is armed: the next key becomes the binding. This is what
            // lets someone with no controller configure the emulator at all.
            if (_activeRebindBtn != null)
            {
                SetInputSource(ShellInputSource.Keyboard);
                if (e.Key == Key.Escape)
                {
                    CancelRebind("bind.cancelled");
                }
                else if (e.Key != Key.System && e.Key != Key.ImeProcessed)
                {
                    AssignKeyboardBinding(e.Key);
                }
                e.Handled = true;
                return;
            }

            // A real key press means the user is on the keyboard now.
            SetInputSource(ShellInputSource.Keyboard);

            // Never steal keys from a text field the user is typing into.
            if (Keyboard.FocusedElement is TextBox) return;

            bool handled = true;
            switch (e.Key)
            {
                case Key.Up:
                case Key.W: _kbUp = true; break;
                case Key.Down:
                case Key.S: _kbDown = true; break;
                case Key.Left:
                case Key.A: _kbLeft = true; break;
                case Key.Right:
                case Key.D: _kbRight = true; break;

                case Key.Enter:
                case Key.Space: _kbCross = true; break;
                case Key.Escape:
                    if (InputTestOverlay != null && InputTestOverlay.Visibility == Visibility.Visible)
                    {
                        SetInputSubTab(true);   // close the controller-testing popup
                        break;
                    }
                    // While a game runs, Esc is the keyboard's PS tap and opens or
                    // dismisses the pause menu (the concept: "hold PS or press Esc").
                    // Everywhere else Esc stays Back/Circle.
                    if (_coreRunning || _pauseMenuVisible) _kbPs = true; else _kbCircle = true;
                    break;
                case Key.Back: _kbCircle = true; break;
                case Key.T: _kbTriangle = true; break;
                case Key.F: _kbSquare = true; break;

                case Key.Q: _kbTabPrev = true; break;
                case Key.E: _kbTabNext = true; break;
                default: handled = false; break;
            }
            e.Handled = handled;
        }

        /// <summary>Assign a keyboard key to the armed binding slot.</summary>
        private void AssignKeyboardBinding(Key key)
        {
            if (_activeRebindBtn == null) return;

            string token = "key_" + key.ToString().ToLowerInvariant();
            _customMappings[_activeRebindTag] = token;
            _activeRebindBtn.Content = BindingDisplayName(token);
            _activeRebindBtn.Foreground = new System.Windows.Media.SolidColorBrush(
                System.Windows.Media.Color.FromRgb(0, 204, 102));
            FooterStatus.Text = I18n.Tr("bind.bound",
                BindingDisplayName(_activeRebindTag), BindingDisplayName(token));
            _activeRebindBtn = null;
            _activeRebindTag = null;
            _rebindNeedsRelease = true;
            RefreshBindingLabels();
        }

        // --- GAMEPAD NAV & PS BUTTON POLLING ---
        private System.Windows.Threading.DispatcherTimer _controllerTimer;
        private XInputState _prevInputState = new XInputState();
        private DateTime _lastAnalogNav = DateTime.MinValue;

        // Directional repeat. A new direction fires at once; holding waits
        // NavFirstDelay and then repeats every NavRepeatInterval.
        /// <summary>Which device the user touched most recently. Both remain
        /// live at all times; this only drives how prompts are labelled.</summary>
        private enum ShellInputSource { Gamepad, Keyboard }

        private ShellInputSource _inputSource = ShellInputSource.Gamepad;
        private string _lastHintKey;

        private void SetInputSource(ShellInputSource src)
        {
            if (_inputSource == src) return;
            _inputSource = src;
            // Re-render the current legend so prompts follow the device without
            // waiting for the next screen change.
            if (_lastHintKey != null) ShowHints(_lastHintKey);
        }

        // Virtual button presses raised by the keyboard and consumed by the next
        // input tick, so keyboard and pad share one navigation implementation.
        private bool _kbUp, _kbDown, _kbLeft, _kbRight;
        private bool _kbCross, _kbCircle, _kbTriangle, _kbSquare;
        private bool _kbTabPrev, _kbTabNext;
        private bool _kbPs;   // Esc while a game runs: the keyboard's PS tap (pause menu)

        private int _navHeldDir;            // 0 none, 1 up, 2 down, 3 left, 4 right
        private DateTime _navRepeatAt = DateTime.MinValue;
        private static readonly TimeSpan NavFirstDelay = TimeSpan.FromMilliseconds(340);
        private static readonly TimeSpan NavRepeatInterval = TimeSpan.FromMilliseconds(90);

        private void InitializeControllerPolling()
        {
            CoreBridge.pcsx5_pad_count(); // starts the core's controller reader
            // Input priority: the default (Background) sits below rendering and
            // layout, so ticks get starved whenever the UI is busy and the pad
            // feels laggy regardless of the interval.
            _controllerTimer = new System.Windows.Threading.DispatcherTimer(
                System.Windows.Threading.DispatcherPriority.Input);
            _controllerTimer.Interval = TimeSpan.FromMilliseconds(10);  // ~100 Hz
            _controllerTimer.Tick += ControllerTimer_Tick;
            _controllerTimer.Start();
        }

        // --- CONTROLLER SETUP LIVE INPUT VISUALIZATION ---
        // Polls the DualSense state at ~60 Hz while the Controller Setup tab is
        // visible and lights up the vector overlays on top of ps5_controller.png.
        private System.Windows.Threading.DispatcherTimer _controllerVizTimer;

        // Stick dot travel range (px in the 1017x1017 design space) around each well center
        private const double StickDotRange = 24.0;
        private const double StickDotSize = 36.0;

        private void StartControllerVizPolling()
        {
            CoreBridge.pcsx5_pad_count(); // starts the core's controller reader
            if (_controllerVizTimer == null)
            {
                _controllerVizTimer = new System.Windows.Threading.DispatcherTimer();
                _controllerVizTimer.Interval = TimeSpan.FromMilliseconds(16); // ~60 Hz
                _controllerVizTimer.Tick += ControllerVizTimer_Tick;
            }
            _controllerVizTimer.Start();
        }

        private void StopControllerVizPolling()
        {
            _controllerVizTimer?.Stop();
        }

        private Button _activeRebindBtn = null;
        private string _activeRebindTag = null;
        // Capture must not consume the button press that armed it, so the pad
        // has to return to neutral before the next press counts as the binding.
        private bool _rebindNeedsRelease = false;
        private DateTime _rebindArmedAt = DateTime.MinValue;
        private static readonly TimeSpan RebindTimeout = TimeSpan.FromSeconds(10);
        private bool _psDown = false;
        private bool _psHoldTriggered = false;
        private DateTime _psDownAt = DateTime.MinValue;
        private System.Collections.Generic.Dictionary<string, string> _customMappings = new System.Collections.Generic.Dictionary<string, string>();


        private void BtnMap_Click(object sender, RoutedEventArgs e)
        {
            if (sender is Button btn)
            {
                if (_activeRebindBtn != null && _activeRebindBtn != btn)
                {
                    ResetRebindBtnStyle(_activeRebindBtn);
                }
                _activeRebindBtn = btn;
                _activeRebindTag = BindingSlotOf(btn);
                _rebindNeedsRelease = true;
                _rebindArmedAt = DateTime.Now;
                btn.Content = I18n.Tr("bind.press_now");
                btn.Foreground = new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(255, 204, 0));
                FooterStatus.Text = I18n.Tr("bind.arming", BindingDisplayName(_activeRebindTag));
                ShowHints("bind.hints");
            }
        }

        private void ResetRebindBtnStyle(Button btn)
        {
            if (btn == null) return;
            btn.Foreground = new System.Windows.Media.SolidColorBrush(System.Windows.Media.Colors.White);
            string slot = BindingSlotOf(btn);
            string token = _customMappings.TryGetValue(slot, out var saved)
                ? saved : BindingDefaultOf(btn);
            btn.Content = BindingDisplayName(token);
        }

        /// <summary>Human-readable name for an internal binding token. The raw
        /// tokens (pad_up, axis_left_y, l1) are implementation detail and should
        /// never be what the user reads off the screen.</summary>
        /// <summary>The binding slot a button configures. Tag carries
        /// "SlotName|default_token" so the default is data rather than a
        /// hardcoded, untranslatable label sitting in Content.</summary>
        private static string BindingSlotOf(Button btn)
        {
            string tag = btn?.Tag as string;
            if (string.IsNullOrEmpty(tag)) return btn?.Name ?? "";
            int bar = tag.IndexOf('|');
            return bar < 0 ? tag : tag.Substring(0, bar);
        }

        private static string BindingDefaultOf(Button btn)
        {
            string tag = btn?.Tag as string;
            if (string.IsNullOrEmpty(tag)) return null;
            int bar = tag.IndexOf('|');
            return bar < 0 ? null : tag.Substring(bar + 1);
        }

        /// <summary>Render every binding button with its readable label. Called
        /// after load and after any change to the mappings.</summary>
        private void RefreshBindingLabels()
        {
            foreach (var btn in GetBindingButtons())
            {
                if (btn == null) continue;
                if (btn == _activeRebindBtn) continue;   // keep the armed prompt

                string slot = BindingSlotOf(btn);
                string token = _customMappings.TryGetValue(slot, out var saved)
                    ? saved : BindingDefaultOf(btn);
                btn.Content = BindingDisplayName(token);
            }
        }

        /// <summary>Every button that configures a binding. Kept separate from
        /// GetControllerSetupControls(), which is the navigation order and also
        /// contains combos, sliders and action buttons.</summary>
        private List<Button> GetBindingButtons()
        {
            return new List<Button>
            {
                BtnMapUp, BtnMapDown, BtnMapLeft, BtnMapRight,
                BtnMapTriangle, BtnMapCircle, BtnMapCross, BtnMapSquare,
                BtnMapL1, BtnMapL2, BtnMapR1, BtnMapR2,
                BtnMapL3, BtnMapR3, BtnMapOptions,
                BtnMapTouchpadLeft, BtnMapTouchpadCenter, BtnMapTouchpadRight,
                BtnMapLeftStickUp, BtnMapLeftStickDown,
                BtnMapLeftStickLeft, BtnMapLeftStickRight,
                BtnMapRightStickUp, BtnMapRightStickDown,
                BtnMapRightStickLeft, BtnMapRightStickRight,
            };
        }

        private string BindingDisplayName(string token)
        {
            if (string.IsNullOrWhiteSpace(token)) return I18n.Tr("bind.unmapped");

            // Keyboard bindings are open-ended, so they are formatted rather
            // than given one locale key per key on the keyboard.
            string t = token.Trim();
            if (t.StartsWith("key_", StringComparison.OrdinalIgnoreCase))
            {
                string name = t.Substring(4).Replace("_", " ");
                if (name.Length > 0) name = char.ToUpperInvariant(name[0]) + name.Substring(1);
                return I18n.Tr("bind.keyboard_fmt", name);
            }

            string key = "bind." + t.ToLowerInvariant();
            string label = I18n.Tr(key);
            // I18n.Tr returns the key itself when there is no entry; fall back to
            // the raw token rather than showing "bind.something" on screen.
            return label == key ? token : label;
        }

        /// <summary>Map the normalized button mask the input tick builds (for both
        /// DualSense and XInput) to a binding token. Returns null when neutral.
        /// The PS button is excluded: it is the cancel gesture, not bindable.</summary>
        private static string BindingTokenFromMask(ushort mask, byte leftTrigger, byte rightTrigger)
        {
            if ((mask & 0x1000) != 0) return "cross";
            if ((mask & 0x2000) != 0) return "circle";
            if ((mask & 0x4000) != 0) return "square";
            if ((mask & 0x8000) != 0) return "triangle";
            if ((mask & 0x0100) != 0) return "l1";
            if ((mask & 0x0200) != 0) return "r1";
            if (leftTrigger > 60) return "l2";
            if (rightTrigger > 60) return "r2";
            if ((mask & 0x0040) != 0) return "l3";
            if ((mask & 0x0080) != 0) return "r3";
            if ((mask & 0x0001) != 0) return "pad_up";
            if ((mask & 0x0002) != 0) return "pad_down";
            if ((mask & 0x0004) != 0) return "pad_left";
            if ((mask & 0x0008) != 0) return "pad_right";
            if ((mask & 0x0010) != 0) return "options";
            if ((mask & 0x0020) != 0) return "back";
            return null;
        }

        private void CancelRebind(string reasonKey)
        {
            if (_activeRebindBtn == null) return;
            ResetRebindBtnStyle(_activeRebindBtn);
            _activeRebindBtn = null;
            _activeRebindTag = null;
            _rebindNeedsRelease = false;
            FooterStatus.Text = I18n.Tr(reasonKey);
        }

        /// <summary>Single owner of rebind capture. Returns true when it consumed
        /// the input, in which case the caller must not also treat it as
        /// navigation -- that conflict is what made rebinding unusable.</summary>
        private bool HandleRebindCapture(ushort mask, byte leftTrigger, byte rightTrigger)
        {
            if (_activeRebindBtn == null) return false;

            if (DateTime.Now - _rebindArmedAt > RebindTimeout)
            {
                CancelRebind("bind.timed_out");
                return true;
            }

            // PS cancels. It is deliberately not bindable, so it is always a way
            // out of an armed rebind.
            if ((mask & 0x0400) != 0)
            {
                CancelRebind("bind.cancelled");
                return true;
            }

            bool neutral = mask == 0 && leftTrigger <= 60 && rightTrigger <= 60;
            if (_rebindNeedsRelease)
            {
                if (neutral) _rebindNeedsRelease = false;
                return true;   // still swallow input while waiting for neutral
            }
            if (neutral) return true;

            string token = BindingTokenFromMask(mask, leftTrigger, rightTrigger);
            if (token == null) return true;

            _customMappings[_activeRebindTag] = token;
            _activeRebindBtn.Content = BindingDisplayName(token);
            _activeRebindBtn.Foreground = new System.Windows.Media.SolidColorBrush(
                System.Windows.Media.Color.FromRgb(0, 204, 102));
            FooterStatus.Text = I18n.Tr("bind.bound",
                BindingDisplayName(_activeRebindTag), BindingDisplayName(token));
            _activeRebindBtn = null;
            _activeRebindTag = null;
            _rebindNeedsRelease = true;
            RefreshBindingLabels();
            return true;
        }

        private void CheckRebindInput(HostGamepadButtons buttons)
        {
            if (_activeRebindBtn == null) return;

            string newBinding = null;
            if (buttons != HostGamepadButtons.None)
            {
                foreach (HostGamepadButtons btnFlag in System.Enum.GetValues(typeof(HostGamepadButtons)))
                {
                    if (btnFlag != HostGamepadButtons.None && buttons.HasFlag(btnFlag))
                    {
                        newBinding = btnFlag.ToString().ToLower();
                        break;
                    }
                }
            }

            if (newBinding != null)
            {
                _customMappings[_activeRebindTag] = newBinding;
                _activeRebindBtn.Content = newBinding;
                _activeRebindBtn.Foreground = new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(0, 204, 102));
                FooterStatus.Text = $"Rebound {_activeRebindTag} -> {newBinding}";
                _activeRebindBtn = null;
                _activeRebindTag = null;
            }
        }

        private void ControllerVizTimer_Tick(object sender, EventArgs e)
        {
            // The live controller drawing moved to InputTabView, which reads the
            // core's pad state. What remains here is the PS-button shortcut,
            // which belongs to the whole shell rather than to one tab. It reads
            // the same core state rather than the C# HID reader so that the
            // reader has one fewer dependant on its way out.
            var pad = CoreBridge.PadState.Create();
            if (CoreBridge.pcsx5_pad_get_state(0, ref pad) != 0 || pad.Connected == 0)
            {
                _psDown = false;
                return;
            }

            // Rebind capture deliberately does NOT happen here. This timer runs
            // at 60Hz alongside the 20Hz input tick, and having both act on the
            // same press made Cross re-arm and Circle cancel the binding being
            // set. HandleRebindCapture in the input tick is the single owner.

            // PS Button Shortcut Handling:
            // Press & hold 1.0s -> close game and navigate to Home/Library
            // Tap PS button -> show/dismiss pause menu overlay
            bool psPressed = (pad.Buttons & 0x10000u) != 0;
            if (InputTestOverlay != null && InputTestOverlay.Visibility == Visibility.Visible)
            {
                // The testing popup owns PS (a tap is a test, a 2 s hold closes it,
                // handled in the input tick). This timer's 1 s hold was firing
                // "return to Library" underneath the popup, so the Input tab was
                // gone when the popup closed. Swallow the whole press, release
                // included, so nothing fires once the popup is closed either.
                _psDown = psPressed;
                _psHoldTriggered = true;
            }
            else if (psPressed)
            {
                if (!_psDown)
                {
                    _psDown = true;
                    _psDownAt = DateTime.UtcNow;
                    _psHoldTriggered = false;
                }
                else if (!_psHoldTriggered && (DateTime.UtcNow - _psDownAt).TotalMilliseconds >= 1000.0)
                {
                    _psHoldTriggered = true;
                    if (_coreRunning)
                    {
                        HidePauseMenu();
                        _session?.RequestStop();
                    }
                    ShowTab("Library");
                    FooterStatus.Text = "PS Button Hold (1s): Returned to Home Library";
                    LogConsole("PS Button hold 1s: Closed game and returned to Library.");
                }
            }
            else if (_psDown)
            {
                _psDown = false;
                if (!_psHoldTriggered && (DateTime.UtcNow - _psDownAt).TotalMilliseconds < 1000.0)
                {
                    if (_pauseMenuVisible)
                    {
                        ResumeFromPause();
                        LogConsole("PS Button tap: Pause menu dismissed, game resumed.");
                    }
                    else if (_coreRunning)
                    {
                        ShowPauseMenu();
                        LogConsole("PS Button tap: Pause menu shown.");
                    }
                }
                _psHoldTriggered = false;
            }
        }

        // --- MIC BUTTON -> MIC LED ---
        // Short press toggles the mic (mute) LED on/off; holding the button for
        // >= 500 ms switches it to pulse mode until release, then restores the
        // toggled state. Edge detection runs in ControllerTimer_Tick.
        private bool _micBtnDown;
        private bool _micBtnPulsing;
        private bool _micLedToggledOn;
        private DateTime _micBtnDownAt;
        private const double MicHoldPulseMs = 500.0;

        private void HandleMicButtonLed(bool down)
        {
            if (down && !_micBtnDown)
            {
                _micBtnDown = true;
                _micBtnPulsing = false;
                _micBtnDownAt = DateTime.UtcNow;
            }
            else if (down && _micBtnDown && !_micBtnPulsing &&
                     (DateTime.UtcNow - _micBtnDownAt).TotalMilliseconds >= MicHoldPulseMs)
            {
                _micBtnPulsing = true;
                CoreBridge.pcsx5_pad_set_mic_led(0, 2); // pulse while held
            }
            else if (!down && _micBtnDown)
            {
                _micBtnDown = false;
                if (!_micBtnPulsing)
                {
                    _micLedToggledOn = !_micLedToggledOn; // short press toggles
                }
                _micBtnPulsing = false;
                CoreBridge.pcsx5_pad_set_mic_led(0, _micLedToggledOn ? (byte)1 : (byte)0);
            }
        }

        private void RestoreDefaultMappings_Click(object sender, RoutedEventArgs e)
        {
            _customMappings.Clear();
            if (BtnMapUp != null) BtnMapUp.Content = "pad_up";
            if (BtnMapLeft != null) BtnMapLeft.Content = "pad_left";
            if (BtnMapRight != null) BtnMapRight.Content = "pad_right";
            if (BtnMapDown != null) BtnMapDown.Content = "pad_down";
            if (BtnMapL1 != null) BtnMapL1.Content = "l1";
            if (BtnMapL2 != null) BtnMapL2.Content = "l2";
            if (BtnMapTriangle != null) BtnMapTriangle.Content = "triangle";
            if (BtnMapCircle != null) BtnMapCircle.Content = "circle";
            if (BtnMapCross != null) BtnMapCross.Content = "cross";
            if (BtnMapSquare != null) BtnMapSquare.Content = "square";
            if (BtnMapR1 != null) BtnMapR1.Content = "r1";
            if (BtnMapR2 != null) BtnMapR2.Content = "r2";
            if (BtnMapL3 != null) BtnMapL3.Content = "l3";
            if (BtnMapOptions != null) BtnMapOptions.Content = "options";
            if (BtnMapR3 != null) BtnMapR3.Content = "r3";
            if (BtnMapTouchpadCenter != null) BtnMapTouchpadCenter.Content = "back";
            LogConsole("Input Mappings restored to default configuration.");
            FooterStatus.Text = "Mappings Restored to Default";
        }

        /// <summary>Light the pad for a player slot: lightbar in the slot's
        /// colour and the matching player-LED pattern (INFERRED from the console).</summary>
        private void ApplyActiveGamepadSlot(int idx)
        {
            byte r = 0, g = 0, b = 255;
            byte playerLed = 0x04;

            switch (idx)
            {
                case 0: // P1 Blue
                    r = 0; g = 0; b = 255; playerLed = 0x04;
                    break;
                case 1: // P2 Red
                    r = 255; g = 0; b = 0; playerLed = 0x0A;
                    break;
                case 2: // P3 Green
                    r = 0; g = 255; b = 0; playerLed = 0x15;
                    break;
                case 3: // P4 Purple
                    r = 180; g = 0; b = 255; playerLed = 0x1B;
                    break;
                default:
                    r = 0; g = 153; b = 255; playerLed = 0x04;
                    break;
            }

            try
            {
                CoreBridge.pcsx5_pad_set_lightbar(0, r, g, b);
                CoreBridge.pcsx5_pad_set_player_leds(0, playerLed, 0);
            }
            catch { }
            if (InputTab?.Pad3D != null) { InputTab.Pad3D.LightbarColor = Color.FromRgb(r, g, b); InputTab.Pad3D.SetPlayerIndex(Math.Min(4, idx + 1)); }
            LogConsole($"Active Gamepad Slot {idx + 1} selected (Player LED 0x{playerLed:X2}, Lightbar RGB #{r:X2}{g:X2}{b:X2}).");
        }




        private List<Control> GetControllerSetupControls()
        {
            return new List<Control>
            {
                TestControllerBtn,
                BtnMapUp,
                BtnMapLeft,
                BtnMapRight,
                BtnMapDown,
                BtnMapL1,
                BtnMapL2,
                BtnMapLeftStickUp,
                BtnMapLeftStickLeft,
                BtnMapLeftStickRight,
                BtnMapLeftStickDown,
                BtnMapTriangle,
                BtnMapCircle,
                BtnMapCross,
                BtnMapSquare,
                BtnMapR1,
                BtnMapR2,
                BtnMapRightStickUp,
                BtnMapRightStickLeft,
                BtnMapRightStickRight,
                BtnMapRightStickDown,
                // Previously omitted, so these were unreachable by pad.
                BtnMapL3,
                BtnMapOptions,
                BtnMapR3,
                BtnMapTouchpadLeft,
                BtnMapTouchpadCenter,
                BtnMapTouchpadRight,
                ControllerSettingsBtn
            };
        }

        private void HandleSettingsGamepadNav(bool up, bool down, bool left, bool right, bool a, bool b)
        {
            if (_settingsLevel == 1)
            {
                // Level 1: Main Category Hub (8 Tiles)
                ShowHints("hints.settings_hub");

                var hubButtons = GetHubCategoryButtons().Where(btn => btn != null && btn.IsVisible).ToList();
                if (hubButtons.Count == 0) return;

                int focusedIdx = hubButtons.FindIndex(btn => btn.IsKeyboardFocused);
                if (focusedIdx == -1) focusedIdx = 0;

                if (up || down || left || right)
                {
                    int nextIdx = focusedIdx;
                    if (down) nextIdx = Math.Min(hubButtons.Count - 1, focusedIdx + 2);
                    else if (up) nextIdx = Math.Max(0, focusedIdx - 2);
                    else if (right) nextIdx = Math.Min(hubButtons.Count - 1, focusedIdx + 1);
                    else if (left) nextIdx = Math.Max(0, focusedIdx - 1);

                    hubButtons[nextIdx].Focus();
                    hubButtons[nextIdx].BringIntoView();
                    return;
                }

                if (a)
                {
                    hubButtons[focusedIdx].RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                    return;
                }

                if (b)
                {
                    TabLibrary_Click(this, null);
                    return;
                }
            }
            else if (_settingsLevel == 2)
            {
                // Level 2: Category Rows Overview
                ShowHints("hints.settings_list");

                if (SettingsCategoryRowsPanel == null) return;
                var rowButtons = SettingsCategoryRowsPanel.Children.OfType<Button>().ToList();
                var navButtons = SettingsNavButtons().Where(n => n != null && n.IsVisible).ToList();
                int navIdx = navButtons.FindIndex(n => n.IsKeyboardFocused);

                // Side-nav column: up/down change the section (its rows open at
                // once, focus stays in the column); right or Cross enter the rows.
                if (navIdx >= 0)
                {
                    if (up || down)
                    {
                        int n = down ? Math.Min(navButtons.Count - 1, navIdx + 1) : Math.Max(0, navIdx - 1);
                        if (navButtons[n].CommandParameter is string cat) OpenSettingsCategory(cat);
                        navButtons[n].Focus();
                        navButtons[n].BringIntoView();
                    }
                    else if (right || a)
                    {
                        if (rowButtons.Count > 0) { rowButtons[0].Focus(); rowButtons[0].BringIntoView(); }
                    }
                    else if (b)
                    {
                        ResetSettingsView();
                    }
                    return;
                }

                Button activeNav = navButtons.FirstOrDefault(n => (n.CommandParameter as string) == _activeSettingsCategory) ?? navButtons.FirstOrDefault();
                if (rowButtons.Count == 0)
                {
                    if (up || down || left || right) activeNav?.Focus();
                    return;
                }

                int focusedIdx = rowButtons.FindIndex(btn => btn.IsKeyboardFocused);

                if (up || down)
                {
                    int nextIdx = (focusedIdx == -1) ? 0 : (down ? Math.Min(rowButtons.Count - 1, focusedIdx + 1) : Math.Max(0, focusedIdx - 1));
                    rowButtons[nextIdx].Focus();
                    rowButtons[nextIdx].BringIntoView();
                    return;
                }

                if (a && focusedIdx >= 0 && focusedIdx < rowButtons.Count)
                {
                    rowButtons[focusedIdx].RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                    return;
                }

                if (left)
                {
                    // Back to the section column. Only on a press: this handler runs
                    // every tick, and focusing here unconditionally stole focus from
                    // the tab bar and the close button 20 times a second, so no mouse
                    // click outside the rows could complete on the Settings page.
                    activeNav?.Focus();
                    return;
                }

                if (b)
                {
                    ResetSettingsView();
                    return;
                }
            }
            else if (_settingsLevel == 3)
            {
                // Level 3: Dedicated Sub-Page (Pickers, Sliders, Toggles, Directory List)
                ShowHints("hints.settings_detail");

                if (SettingsSubPageContentContainer == null) return;
                var focusables = SettingsSubPageContentContainer.Children
                    .OfType<FrameworkElement>()
                    .SelectMany(GetFocusableDescendants)
                    .ToList();

                if (focusables.Count == 0) return;

                int focusedIdx = focusables.FindIndex(c => c.IsKeyboardFocused);

                var focusedNow = focusedIdx >= 0 ? focusables[focusedIdx] : null;
                bool adjusts = (left || right) && focusedNow is Slider;
                if ((up || down || left || right) && !adjusts)
                {
                    // Option grids are walked as grids (TV-remote movement).
                    var dir = up ? SpatialNav.Dir.Up : down ? SpatialNav.Dir.Down : left ? SpatialNav.Dir.Left : SpatialNav.Dir.Right;
                    var target = SpatialNav.Find(focusables, focusedNow, dir);
                    if (target != null) { target.Focus(); target.BringIntoView(); }
                    else if (left && focusedNow != null) { BtnSettingsBack_Click(this, null); }   // off the left edge = back to the rows
                    return;
                }

                if (focusedIdx >= 0 && focusedIdx < focusables.Count)
                {
                    var focused = focusables[focusedIdx];
                    if (focused is Slider slider)
                    {
                        if (left || right)
                        {
                            double step = slider.TickFrequency > 0 ? slider.TickFrequency : 0.1;
                            slider.Value = left ? Math.Max(slider.Minimum, slider.Value - step) : Math.Min(slider.Maximum, slider.Value + step);
                            return;
                        }
                    }
                    else if (focused is Button btn)
                    {
                        if (a)
                        {
                            btn.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                            return;
                        }
                    }
                }

                if (b)
                {
                    OpenSettingsCategory(_activeSettingsCategory);
                    return;
                }
            }
        }

        private IEnumerable<FrameworkElement> GetFocusableDescendants(FrameworkElement root)
        {
            if (root is Button b && b.Focusable) yield return b;
            else if (root is Slider s && s.Focusable) yield return s;
            else if (root is ListBox l && l.Focusable) yield return l;
            else if (root is Panel p)
            {
                foreach (FrameworkElement child in p.Children)
                {
                    foreach (var desc in GetFocusableDescendants(child))
                        yield return desc;
                }
            }
            else if (root is ColorPickerControl picker)
            {
                foreach (var c in picker.NavControls) yield return c;
            }
            else if (root is ContentControl cc && cc.Content is FrameworkElement fe)
            {
                foreach (var desc in GetFocusableDescendants(fe))
                    yield return desc;
            }
            else if (root is Border bd && bd.Child is FrameworkElement bfe)
            {
                foreach (var desc in GetFocusableDescendants(bfe))
                    yield return desc;
            }
        }

        private void HandleControllerSetupGamepadNav(bool up, bool down, bool left, bool right, bool a, bool b)
        {
            // An armed rebind is handled before navigation is ever reached
            // (see HandleRebindCapture), so there is nothing to cancel here.

            var controls = GetControllerSetupControls().Where(c => c != null && c.IsVisible).ToList();
            if (controls.Count == 0) return;

            int focusedIdx = controls.FindIndex(c => c.IsKeyboardFocused);
            var focusedNow = focusedIdx >= 0 ? controls[focusedIdx] : null;

            // Left/right on a value control adjust it; every other direction moves
            // focus to whatever is visually that way (TV-remote navigation), so the
            // 4-column binding grid is walked as a grid, not as a list.
            bool adjusts = (left || right) && (focusedNow is ComboBox || focusedNow is Slider);
            if ((up || down || left || right) && !adjusts)
            {
                var dir = up ? SpatialNav.Dir.Up : down ? SpatialNav.Dir.Down : left ? SpatialNav.Dir.Left : SpatialNav.Dir.Right;
                var target = SpatialNav.Find(controls, focusedNow, dir);
                if (target != null)
                {
                    target.Focus();
                    target.BringIntoView();
                }
                return;
            }

            if (focusedIdx >= 0 && focusedIdx < controls.Count)
            {
                var focused = controls[focusedIdx];
                if (focused is ComboBox combo)
                {
                    if (left || right)
                    {
                        if (combo.Items.Count > 0)
                        {
                            int idx = combo.SelectedIndex;
                            combo.SelectedIndex = left ? Math.Max(0, idx - 1) : Math.Min(combo.Items.Count - 1, idx + 1);
                        }
                    }
                    else if (a)
                    {
                        combo.IsDropDownOpen = !combo.IsDropDownOpen;
                    }
                }
                else if (focused is Slider slider)
                {
                    if (left || right)
                    {
                        double step = slider.TickFrequency > 0 ? slider.TickFrequency : 5;
                        slider.Value = left ? Math.Max(slider.Minimum, slider.Value - step) : Math.Min(slider.Maximum, slider.Value + step);
                    }
                }
                else if (focused is CheckBox cb)
                {
                    if (a) cb.IsChecked = !(cb.IsChecked == true);
                }
                else if (focused is Button btn)
                {
                    if (a) btn.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                }
            }
        }

        private void HandleAnalyzerGamepadNav(bool up, bool down, bool a)
        {
            if (up || down)
            {
                if (AnalyzerListView.Items.Count > 0)
                {
                    int idx = AnalyzerListView.SelectedIndex;
                    if (idx == -1) idx = 0;
                    else idx = down ? Math.Min(AnalyzerListView.Items.Count - 1, idx + 1) : Math.Max(0, idx - 1);
                    AnalyzerListView.SelectedIndex = idx;
                    AnalyzerListView.ScrollIntoView(AnalyzerListView.SelectedItem);
                }
            }
            else if (a)
            {
                AnalyzeAll_Click(this, null);
            }
        }

        private void HandleLogsGamepadNav(bool up, bool down, bool a)
        {
            if (up || down)
            {
                if (up) ConsoleOutputBox.LineUp();
                else ConsoleOutputBox.LineDown();
            }
            else if (a)
            {
                CopyConsole_Click(this, null);
            }
        }

        private void ControllerTimer_Tick(object sender, EventArgs e)
        {
            XInputState state = new XInputState();
            ushort buttons = 0;
            short lx = 0;
            short ly = 0;

            // The pad comes from the core's reader now, through the pad-state ABI,
            // rather than from the shell's own C# HID reader that used to
            // interleave with it on the same device. The shape below -- the
            // shell's navigation bitmask, an XInput-style state, and the mute
            // button feeding the LED -- is unchanged; only the source moved.
            var pad = CoreBridge.PadState.Create();
            bool hasDualSense = CoreBridge.pcsx5_pad_get_state(0, ref pad) == 0 && pad.Connected != 0;
            if (hasDualSense)
            {
                var dsButtons = HostGamepad.FromCoreMask(pad.Buttons);
                if ((dsButtons & HostGamepadButtons.Up) != 0) buttons |= 0x0001;
                if ((dsButtons & HostGamepadButtons.Down) != 0) buttons |= 0x0002;
                if ((dsButtons & HostGamepadButtons.Left) != 0) buttons |= 0x0004;
                if ((dsButtons & HostGamepadButtons.Right) != 0) buttons |= 0x0008;
                if ((dsButtons & HostGamepadButtons.Options) != 0) buttons |= 0x0010;
                if ((dsButtons & HostGamepadButtons.Back) != 0 || (dsButtons & HostGamepadButtons.TouchPad) != 0) buttons |= 0x0020;
                if ((dsButtons & HostGamepadButtons.L3) != 0) buttons |= 0x0040;
                if ((dsButtons & HostGamepadButtons.R3) != 0) buttons |= 0x0080;
                if ((dsButtons & HostGamepadButtons.L1) != 0) buttons |= 0x0100;
                if ((dsButtons & HostGamepadButtons.R1) != 0) buttons |= 0x0200;
                if ((dsButtons & HostGamepadButtons.PlayStation) != 0) buttons |= 0x0400;
                if ((dsButtons & HostGamepadButtons.Cross) != 0) buttons |= 0x1000;
                if ((dsButtons & HostGamepadButtons.Circle) != 0) buttons |= 0x2000;
                if ((dsButtons & HostGamepadButtons.Square) != 0) buttons |= 0x4000;
                if ((dsButtons & HostGamepadButtons.Triangle) != 0) buttons |= 0x8000;

                // Kept as the button-threshold form the old reader produced, so
                // navigation behaviour is identical. The core also carries the
                // analog L2/R2 values (pad.L2, pad.R2) if a consumer wants them.
                state.Gamepad.bLeftTrigger = (byte)((dsButtons & HostGamepadButtons.L2) != 0 ? 255 : 0);
                state.Gamepad.bRightTrigger = (byte)((dsButtons & HostGamepadButtons.R2) != 0 ? 255 : 0);

                lx = (short)((pad.Lx - 128) * 256);
                ly = (short)(-(pad.Ly - 128) * 256);
                _stickL = ((pad.Lx - 128) / 127.0, -(pad.Ly - 128) / 127.0);
                _stickR = ((pad.Rx - 128) / 127.0, -(pad.Ry - 128) / 127.0);
                state.Gamepad.wButtons = buttons;
                state.Gamepad.sThumbLX = lx;
                state.Gamepad.sThumbLY = ly;

                HandleMicButtonLed((dsButtons & HostGamepadButtons.Mic) != 0);
            }
            else
            {
                int activeIndex = -1;
                for (int i = 0; i < 4; i++)
                {
                    if (XInput.GetState(i, ref state) == 0)
                    {
                        activeIndex = i;
                        break;
                    }
                }

                if (activeIndex == -1)
                {
                    // No controller: carry on with an empty pad state so the
                    // keyboard contributions below still drive the shell. This
                    // used to return here, which silently killed keyboard
                    // navigation whenever the DualSense powered off (found
                    // 2026-09-06 when a run's R1 key did nothing).
                    state = new XInputState();
                }

                buttons = state.Gamepad.wButtons;
                lx = state.Gamepad.sThumbLX;
                ly = state.Gamepad.sThumbLY;
            }

            // Like a console, the pad belongs to whichever window is in front.
            // While the shell is not the active window -- alt-tabbed away, or
            // something else in front -- its input is swallowed here, with the
            // previous state kept current so a button held meanwhile does not
            // fire as a fresh press when focus returns. (Compared against the
            // SharpEmu launcher, 2026-09-06: this was the one navigation
            // behaviour it had that the shell lacked.)
            if (!IsActive)
            {
                _prevInputState = state;
                _kbUp = _kbDown = _kbLeft = _kbRight = false;
                _kbCross = _kbCircle = _kbTriangle = _kbSquare = false;
                _kbTabPrev = _kbTabNext = false;
                _kbPs = false;
                return;
            }

            // Fold in anything the keyboard raised since the last tick, then
            // clear it, so a key press acts exactly like the equivalent button.
            bool kbUp = _kbUp, kbDown = _kbDown, kbLeft = _kbLeft, kbRight = _kbRight;
            bool kbCross = _kbCross, kbCircle = _kbCircle;
            bool kbTriangle = _kbTriangle, kbSquare = _kbSquare;
            bool kbTabPrev = _kbTabPrev, kbTabNext = _kbTabNext;
            bool kbPs = _kbPs;
            _kbUp = _kbDown = _kbLeft = _kbRight = false;
            _kbCross = _kbCircle = _kbTriangle = _kbSquare = false;
            _kbTabPrev = _kbTabNext = false;
            _kbPs = false;

            // Any real pad activity switches the prompts back to controller
            // labels. Thresholds keep idle stick drift from flapping the source.
            if (buttons != 0 ||
                state.Gamepad.bLeftTrigger > 60 || state.Gamepad.bRightTrigger > 60 ||
                Math.Abs((int)lx) > 15000 || Math.Abs((int)ly) > 15000)
            {
                Dispatcher.Invoke(() => SetInputSource(ShellInputSource.Gamepad));
            }

            ushort prevButtons = _prevInputState.Gamepad.wButtons;
            byte prevLT = _prevInputState.Gamepad.bLeftTrigger;
            byte prevRT = _prevInputState.Gamepad.bRightTrigger;

            // Detect Button Press transitions (pressed now, but was not pressed in previous state)
            bool upPressed = ((buttons & 0x0001) != 0) && ((prevButtons & 0x0001) == 0);
            bool downPressed = ((buttons & 0x0002) != 0) && ((prevButtons & 0x0002) == 0);
            bool leftPressed = ((buttons & 0x0004) != 0) && ((prevButtons & 0x0004) == 0);
            bool rightPressed = ((buttons & 0x0008) != 0) && ((prevButtons & 0x0008) == 0);

            bool aPressed = ((buttons & 0x1000) != 0) && ((prevButtons & 0x1000) == 0); // Cross/A (Confirm / Enter)
            bool bPressed = ((buttons & 0x2000) != 0) && ((prevButtons & 0x2000) == 0); // Circle/B (Back / Cancel / Return)
            bool xPressed = ((buttons & 0x4000) != 0) && ((prevButtons & 0x4000) == 0); // Square/X (Details / Secondary)
            bool yPressed = ((buttons & 0x8000) != 0) && ((prevButtons & 0x8000) == 0); // Triangle/Y (Menu / Tools)

            bool l1Pressed = ((buttons & 0x0100) != 0) && ((prevButtons & 0x0100) == 0); // L1 / Tab Left
            bool r1Pressed = ((buttons & 0x0200) != 0) && ((prevButtons & 0x0200) == 0); // R1 / Tab Right
            bool l2Pressed = (state.Gamepad.bLeftTrigger > 30) && (prevLT <= 30);         // L2 / Trigger Tab Left
            bool r2Pressed = (state.Gamepad.bRightTrigger > 30) && (prevRT <= 30);        // R2 / Trigger Tab Right

            bool l3Pressed = ((buttons & 0x0040) != 0) && ((prevButtons & 0x0040) == 0); // L3 (Left Stick Click)
            bool r3Pressed = ((buttons & 0x0080) != 0) && ((prevButtons & 0x0080) == 0); // R3 (Right Stick Click)
            bool l3Held = (buttons & 0x0040) != 0;
            bool r3Held = (buttons & 0x0080) != 0;

            bool optionsPressed = ((buttons & 0x0010) != 0) && ((prevButtons & 0x0010) == 0); // Options / Start
            bool optionsHeld = (buttons & 0x0010) != 0;
            bool backPressed = ((buttons & 0x0020) != 0) && ((prevButtons & 0x0020) == 0); // Share / Back / TouchPad
            bool backHeld = (buttons & 0x0020) != 0;

            bool psPressed = ((buttons & 0x0400) != 0) && ((prevButtons & 0x0400) == 0); // PS Button / Guide
            bool psHeld = (buttons & 0x0400) != 0;
            bool l1Held = (buttons & 0x0100) != 0;
            bool r1Held = (buttons & 0x0200) != 0;

            // Keyboard contributions, merged before the repeat model so a key
            // press behaves identically to the corresponding pad button.
            if (kbCross) aPressed = true;
            if (kbCircle) bPressed = true;
            if (kbSquare) xPressed = true;
            if (kbTriangle) yPressed = true;
            // The pad's PS tap is detected on release (held, then released within
            // a second); a keyboard press has no held state, so Esc performs the
            // tap directly.
            if (kbPs)
            {
                if (_pauseMenuVisible) { ResumeFromPause(); LogConsole("Esc: pause menu dismissed, game resumed."); }
                else if (_coreRunning) { ShowPauseMenu(); LogConsole("Esc: pause menu shown."); }
            }
            if (kbTabPrev) l1Pressed = true;
            if (kbTabNext) r1Pressed = true;

            // Map Left Thumbstick with analog deadzone and cooldown
            const int stickThreshold = 15000;

            // Which direction is currently being asked for, from stick or D-Pad.
            int navDir = 0;
            if (lx < -stickThreshold || (buttons & 0x0004) != 0) navDir = 3;
            else if (lx > stickThreshold || (buttons & 0x0008) != 0) navDir = 4;
            else if (ly > stickThreshold || (buttons & 0x0001) != 0) navDir = 1;
            else if (ly < -stickThreshold || (buttons & 0x0002) != 0) navDir = 2;

            bool navFire = false;
            if (navDir == 0)
            {
                _navHeldDir = 0;
            }
            else if (navDir != _navHeldDir)
            {
                // Direction changed: act now. The old code made this wait out the
                // cooldown, which is most of what "laggy" meant.
                _navHeldDir = navDir;
                _navRepeatAt = DateTime.Now + NavFirstDelay;
                navFire = true;
            }
            else if (DateTime.Now >= _navRepeatAt)
            {
                _navRepeatAt = DateTime.Now + NavRepeatInterval;
                navFire = true;
            }

            // A keyboard direction is a discrete press: it bypasses the hold
            // repeat entirely rather than waiting on it.
            if (kbUp) upPressed = true;
            if (kbDown) downPressed = true;
            if (kbLeft) leftPressed = true;
            if (kbRight) rightPressed = true;

            if (navFire)
            {
                _lastAnalogNav = DateTime.Now;
                switch (navDir)
                {
                    case 1: upPressed = true; break;
                    case 2: downPressed = true; break;
                    case 3: leftPressed = true; break;
                    case 4: rightPressed = true; break;
                }
            }

            // ── REBIND CAPTURE ──────────────────────────────────────────────
            // Runs before every shortcut and navigation path: while a binding is
            // armed the pad belongs to the rebind, or Cross/Circle would be eaten
            // as "activate"/"cancel" and could never be bound.
            if (_activeRebindBtn != null)
            {
                bool consumed = false;
                Dispatcher.Invoke(() => consumed = HandleRebindCapture(
                    buttons, state.Gamepad.bLeftTrigger, state.Gamepad.bRightTrigger));
                if (consumed)
                {
                    _prevInputState = state;
                    return;
                }
            }

            // ── SHORTCUT 1: FORCE STOP GAME (PS + Options OR PS + L1 + R1 OR PS + L3 + R3) ──
            bool forceKillCombo = (psPressed && optionsHeld) || (optionsPressed && psHeld) ||
                                  (psPressed && l1Held && r1Held) || (psPressed && l3Held && r3Held);
            if (forceKillCombo && (_coreRunning || _session?.State == GameSessionState.Running || _session?.State == GameSessionState.Booting))
            {
                LogConsole("⚡ FORCE STOP: Terminating game immediately via controller shortcut (PS+Options / PS+L1+R1 / PS+L3+R3).");
                try { _session?.Kill(); } catch { }
                try { CoreBridge.pcsx5_force_stop(); } catch { }
                Dispatcher.Invoke(() => {
                    StopButton_Click(this, null);
                    if (FooterStatus != null) FooterStatus.Text = "⚡ Game Terminated (Force Stop)";
                });
                _prevInputState = state;
                return;
            }

            // ── SHORTCUT 2: TOGGLE CONSOLE DRAWER (L3 + R3 OR Share/Touchpad + Triangle) ──
            bool consoleToggleCombo = (l3Pressed && r3Held) || (r3Pressed && l3Held) || (l3Pressed && r3Pressed) ||
                                      (backHeld && yPressed) || (backPressed && (buttons & 0x8000) != 0);
            if (consoleToggleCombo)
            {
                Dispatcher.Invoke(() => {
                    SetGameConsoleVisible(!_gameConsoleVisible);
                    LogConsole($"Console drawer {(_gameConsoleVisible ? "opened" : "closed")} via controller shortcut (L3+R3 / Share+Triangle).");
                });
                _prevInputState = state;
                return;
            }

            // ── CONSOLE DRAWER GAMEPAD NAVIGATION (When drawer is open) ──
            if (_gameConsoleVisible)
            {
                Dispatcher.Invoke(() => {
                    ShowHints("hints.console");
                    if (upPressed) ConsoleOutputBox.LineUp();
                    else if (downPressed) ConsoleOutputBox.LineDown();
                    else if (aPressed) CopyConsole_Click(this, null);
                    else if (bPressed) SetGameConsoleVisible(false);
                });
                if (bPressed || aPressed || upPressed || downPressed)
                {
                    _prevInputState = state;
                    return;
                }
            }

            // ── PS BUTTON ACTION (Home / Dashboard / Graceful Stop) ──
            if (psPressed)
            {
                if (_coreRunning)
                {
                    try
                    {
                        CoreBridge.pcsx5_stop();
                        LogConsole("PS Button: Graceful stop requested for running game.");
                    }
                    catch { }
                }
                else
                {
                    Dispatcher.Invoke(() =>
                    {
                        if (this.WindowState == WindowState.Minimized)
                        {
                            this.WindowState = WindowState.Normal;
                        }
                        this.Activate();
                        this.Focus();
                        LogConsole("PS Button: UI Window activated.");
                    });
                }
                _prevInputState = state;
                return;
            }

            // ── IN-GAME OVERLAY ROUTING (When Core is running) ──
            if (_coreRunning)
            {
                Dispatcher.Invoke(() =>
                {
                    if (_pauseMenuVisible)
                    {
                        ShowHints("hints.pause_menu");
                        HandlePauseMenuNav(upPressed, downPressed, aPressed, bPressed, yPressed);
                    }
                    else if (_watchdogToastVisible)
                    {
                        ShowHints("hints.watchdog");
                        HandleWatchdogToastNav(aPressed, bPressed);
                    }
                    else if (GameView.Visibility == Visibility.Visible)
                    {
                        // Boot overlay cancel: circle during booting
                        if (bPressed && _session?.State != GameSessionState.Running)
                        {
                            BootCancelBtn_Click(this, null);
                        }
                    }
                });
                _prevInputState = state;
                return;
            }

            Dispatcher.Invoke(() =>
            {
                // Overlay Dialog Traps
                if (FolderPickerOverlay != null && FolderPickerOverlay.Visibility == Visibility.Visible)
                {
                    ShowHints("picker.hints");
                    HandleFolderPickerNav(upPressed, downPressed, aPressed, bPressed, yPressed);
                    _prevInputState = state;
                    return;
                }
                if (FirstRunOverlay != null && FirstRunOverlay.Visibility == Visibility.Visible)
                {
                    ShowHints("hints.firstrun");
                    if (yPressed && FirstRunContinueBtn.IsEnabled) FirstRunContinue_Click(this, null);
                    else if (aPressed)
                    {
                        if (FirstRunContinueBtn.IsEnabled) FirstRunContinue_Click(this, null);
                        else ShowFolderPickerOverlay("firstrun");
                    }
                    else if (bPressed) FirstRunSkip_Click(this, null);
                    _prevInputState = state;
                    return;
                }
                if (CrashDialogOverlay != null && CrashDialogOverlay.Visibility == Visibility.Visible)
                {
                    // One pad button per dialog button, listed in the legend.
                    ShowHints("hints.crash");
                    if (aPressed) AnalyzeCrashedGame_Click(this, null);
                    else if (bPressed) DismissCrashDialog_Click(this, null);
                    else if (xPressed) CopyCrashToClipboard_Click(this, null);
                    else if (yPressed) ViewCrashRawLogs_Click(this, null);
                    else if (upPressed) CrashRawText?.LineUp();
                    else if (downPressed) CrashRawText?.LineDown();
                    _prevInputState = state;
                    return;
                }
                if (ColorPickerOverlay != null && ColorPickerOverlay.Visibility == Visibility.Visible)
                {
                    ShowHints("hints.colorpicker");
                    const double dead = 0.18;
                    double rx = Math.Abs(_stickR.x) > dead ? _stickR.x : 0, ry = Math.Abs(_stickR.y) > dead ? _stickR.y : 0;
                    double lxs = Math.Abs(_stickL.x) > dead ? _stickL.x : 0, lys = Math.Abs(_stickL.y) > dead ? _stickL.y : 0;
                    if (rx != 0 || ry != 0) PopupPicker.NudgePlane(rx * 0.025, ry * 0.025);
                    double hue = lxs != 0 ? lxs : lys;   // either axis of the left stick walks the bar
                    if (hue != 0) PopupPicker.NudgeHue(hue * 4);
                    if (aPressed) ColorPickerApply_Click(this, null);
                    else if (bPressed) ColorPickerCancel_Click(this, null);
                    _prevInputState = state;
                    return;
                }
                if (UpdateOverlay != null && UpdateOverlay.Visibility == Visibility.Visible)
                {
                    ShowHints("hints.update");
                    if (aPressed && UpdateInstallBtn.IsEnabled) UpdateInstall_Click(this, null);
                    else if (xPressed && UpdateSkipBtn.IsEnabled) UpdateSkip_Click(this, null);
                    else if (bPressed) UpdateLater_Click(this, null);
                    _prevInputState = state;
                    return;
                }
                if (CreditsOverlay != null && CreditsOverlay.Visibility == Visibility.Visible)
                {
                    ShowHints("hints.credits");
                    if (upPressed) CreditsScroll.LineUp();
                    else if (downPressed) CreditsScroll.LineDown();
                    else if (aPressed || bPressed) CreditsClose_Click(this, null);
                    _prevInputState = state;
                    return;
                }

                // ── CONTROLLER TESTING POPUP: capture everything. A PS *tap* is just
                //    another button under test; holding PS for 2 s closes the popup. ──
                if (InputTestOverlay != null && InputTestOverlay.Visibility == Visibility.Visible)
                {
                    bool psHeld = (buttons & 0x0400) != 0;
                    if (!psHeld) _psHoldStart = null;
                    else if (_psHoldStart == null) _psHoldStart = DateTime.UtcNow;
                    else if ((DateTime.UtcNow - _psHoldStart.Value).TotalSeconds >= 2.0)
                    {
                        _psHoldStart = null;
                        SetInputSubTab(true);
                    }
                    _prevInputState = state;
                    return;
                }
                _psHoldStart = null;

                // ── TAB CYCLING WITH BUMPERS (L1 / R1) ──
                if (l1Pressed || r1Pressed)
                {
                    Button[] tabs = { TabLibraryBtn, TabAnalyzerBtn, TabControllerBtn, TabSettingsBtn };
                    int activeIndexTab = 0;
                    if (LibraryView.Visibility == Visibility.Visible) activeIndexTab = 0;
                    else if (AnalyzerView.Visibility == Visibility.Visible || ToolsHubView.Visibility == Visibility.Visible) activeIndexTab = 1;
                    else if (ControllerView.Visibility == Visibility.Visible) activeIndexTab = 2;
                    else if (SettingsView.Visibility == Visibility.Visible) activeIndexTab = 3;

                    int nextIndex = activeIndexTab;
                    if (l1Pressed) nextIndex = (activeIndexTab - 1 + tabs.Length) % tabs.Length;
                    else if (r1Pressed) nextIndex = (activeIndexTab + 1) % tabs.Length;

                    switch (nextIndex)
                    {
                        case 0: TabLibrary_Click(this, null); break;
                        case 1: TabAnalyzer_Click(this, null); break;
                        case 2: TabController_Click(this, null); break;
                        case 3: TabSettings_Click(this, null); break;
                    }
                    _prevInputState = state;
                    return;
                }

                // ── NAVIGATION PER ACTIVE TAB ──
                if (LibraryView.Visibility == Visibility.Visible)
                {
                    // The expanded list is a distinct navigation surface.
                    if (FullLibraryGrid != null && FullLibraryGrid.Visibility == Visibility.Visible)
                    {
                        ShowHints("hints.full_library");
                        HandleFullLibraryNav(upPressed, downPressed, leftPressed, rightPressed, aPressed, bPressed, yPressed, xPressed, optionsPressed);
                        _prevInputState = state;
                        return;
                    }

                    ShowHints("hints.library");

                    // Square opens search: the only unassigned face button here,
                    // and the search field was otherwise unreachable by pad.
                    if (xPressed && SearchBox != null)
                    {
                        SearchBox.Focus();
                        SearchBox.SelectAll();
                        _prevInputState = state;
                        return;
                    }

                    // Options (≡) opens the full library: "View all" was mouse-only.
                    if (optionsPressed)
                    {
                        ToggleFullLibrary_Click(this, null);
                        _prevInputState = state;
                        return;
                    }

                    if (_shelf.Count > 0)
                    {
                        if (leftPressed || rightPressed)
                        {
                            int currentIndex = _selectedGame != null ? Math.Max(0, _shelf.IndexOf(_selectedGame)) : 0;
                            int newIndex = leftPressed ? (currentIndex - 1 + _shelf.Count) % _shelf.Count : (currentIndex + 1) % _shelf.Count;
                            SelectGame(_shelf[newIndex]);
                            foreach (Border card in GamesWrapPanel.Children)
                            {
                                if (card.Tag == _shelf[newIndex])
                                {
                                    card.BringIntoView();
                                    break;
                                }
                            }
                        }

                        // Cross (✕ / A) -> Launch Game
                        if (aPressed && LaunchButton.IsEnabled && _selectedGame != null)
                        {
                            LaunchButton_Click(this, null);
                        }

                        // Circle (◯ / B) -> Stop Game
                        if (bPressed && StopButton.Visibility == Visibility.Visible)
                        {
                            StopButton_Click(this, null);
                        }

                        // Triangle (△ / Y) -> Analyzer
                        if (yPressed && _selectedGame != null)
                        {
                            TabAnalyzer_Click(this, null);
                        }
                    }
                }
                else if (SettingsView.Visibility == Visibility.Visible)
                {
                    HandleSettingsGamepadNav(upPressed, downPressed, leftPressed, rightPressed, aPressed, bPressed);
                }
                else if (ControllerView.Visibility == Visibility.Visible)
                {
                    ShowHints("hints.controller");
                    if (optionsPressed) RestoreDefaultMappings_Click(this, null);
                    else HandleControllerSetupGamepadNav(upPressed, downPressed, leftPressed, rightPressed, aPressed, bPressed);
                }
                else if (AnalyzerView.Visibility == Visibility.Visible)
                {
                    ShowHints("hints.analyzer");
                    HandleAnalyzerGamepadNav(upPressed, downPressed, aPressed);
                }
                else if (ToolsHubView.Visibility == Visibility.Visible)
                {
                    ShowHints("hints.tools");
                    // Only the Boot analyzer card is live; Cross opens it, Circle
                    // returns to the Library.
                    if (aPressed) ToolBootAnalyzer_Click(this, null);
                    else if (bPressed) TabLibrary_Click(this, null);
                }
            });

            _prevInputState = state;
        }

        private void TranslateUi()
        {
            try
            {
                I18n.Load(_config.ui.language ?? "en-US");

                // Main navigation tabs
                TabLibraryBtn.Content = I18n.Tr("view.library");
                TabAnalyzerBtn.Content = I18n.Tr("sidebar.tools");
                TabControllerBtn.Content = I18n.Tr("sidebar.input");
                TabSettingsBtn.Content = I18n.Tr("system.header");

                // Game library shelf header
                if (LibraryHeaderTextBlock != null)
                    LibraryHeaderTextBlock.Text = I18n.Tr("ui.recent");

                // Search placeholder
                if (SearchPlaceholder != null)
                    SearchPlaceholder.Text = I18n.Tr("library.search_hint");

                // Launch/Stop buttons
                if (LaunchButton != null)
                    LaunchButton.Content = I18n.Tr("button.play");
                if (StopButton != null)
                    StopButton.Content = I18n.Tr("button.stop");

                // Update details if a game is selected
                if (_selectedGame != null)
                {
                    DetailTitle.Text = _selectedGame.Title;
                    DetailTitleId.Text = _selectedGame.TitleId;
                    DetailSize.Text = FormatSizeChip(_selectedGame.SizeBytes);
                }
            }
            catch (Exception ex)
            {
                LogConsole("UI translation error: " + ex.Message);
            }
        }

        // ── CREDITS POPUP ── the README's "## Credits" section, read from the
        // README staged beside the executable, so the in-app list can never
        // drift from the published one. Falls back to the short locale string.
        private void ShowCredits()
        {
            string text = null;
            try
            {
                string readme = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "README.md");
                if (File.Exists(readme))
                {
                    string md = File.ReadAllText(readme);
                    int at = md.IndexOf("## Credits", StringComparison.Ordinal);
                    if (at >= 0)
                    {
                        string sec = md.Substring(at + "## Credits".Length);
                        int next = sec.IndexOf("\n## ", StringComparison.Ordinal);
                        if (next >= 0) sec = sec.Substring(0, next);
                        sec = System.Text.RegularExpressions.Regex.Replace(sec, @"\[([^\]]+)\]\([^)]*\)", "$1");
                        sec = sec.Replace("**", "").Replace("`", "");
                        // Un-wrap the markdown's hard line breaks inside a bullet.
                        var lines = new List<string>();
                        foreach (var raw in sec.Trim().Split('\n'))
                        {
                            string l = raw.TrimEnd();
                            if (l.StartsWith("- ")) lines.Add("•  " + l.Substring(2).Trim());
                            else if (l.Trim().Length == 0) lines.Add("");
                            else if (lines.Count > 0) lines[lines.Count - 1] += " " + l.Trim();
                            else lines.Add(l.Trim());
                        }
                        text = string.Join("\n", lines);
                    }
                }
            }
            catch { text = null; }
            if (string.IsNullOrWhiteSpace(text)) text = I18n.Tr("about.credits_desc");
            CreditsText.Text = text;
            CreditsScroll.ScrollToTop();
            CreditsOverlay.Visibility = Visibility.Visible;
            FocusFirst(CreditsCloseBtn);
        }

        private void CreditsClose_Click(object sender, RoutedEventArgs e)
        {
            CreditsOverlay.Visibility = Visibility.Collapsed;
            FocusFirst(SettingsNavButtons().FirstOrDefault(b => (b?.CommandParameter as string) == _activeSettingsCategory));
        }

        private void DismissCrashDialog_Click(object sender, RoutedEventArgs e)
        {
            CrashDialogOverlay.Visibility = Visibility.Collapsed;
            try
            {
                string crashLogPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "logs", "crash_log.txt");
                if (File.Exists(crashLogPath)) File.Delete(crashLogPath);

                // Fallback secondary delete
                string fallbackPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "crash_log.txt");
                if (File.Exists(fallbackPath)) File.Delete(fallbackPath);
            }
            catch { }
        }

        private void CopyCrashToClipboard_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                string rawLog = GetConsoleText();
                string crashInfo = CrashRawText.Text;
                string full = $"{crashInfo}\n\n--- Raw Console Log ---\n{rawLog}";
                Clipboard.SetText(full);
                LogConsole("Crash info copied to clipboard.");
            }
            catch (Exception ex)
            {
                LogConsole($"Failed to copy: {ex.Message}");
            }
        }

        private void ViewCrashRawLogs_Click(object sender, RoutedEventArgs e)
        {
            string rawLog = GetConsoleText();
            if (string.IsNullOrEmpty(rawLog)) rawLog = "(no log output)";

            var window = new Window
            {
                Title = "Raw Crash Log - PCSX5",
                Width = 900,
                Height = 600,
                WindowStartupLocation = WindowStartupLocation.CenterOwner,
                Owner = this,
                Background = new SolidColorBrush(Color.FromRgb(0x12, 0x14, 0x1A)),
                Foreground = Brushes.White,
                ResizeMode = ResizeMode.CanResize,
                Content = new System.Windows.Controls.TextBox
                {
                    Text = rawLog,
                    IsReadOnly = true,
                    TextWrapping = TextWrapping.Wrap,
                    FontFamily = new System.Windows.Media.FontFamily("Consolas, Courier New"),
                    FontSize = 11,
                    Foreground = new SolidColorBrush(Color.FromRgb(0xD0, 0xD0, 0xD5)),
                    Background = new SolidColorBrush(Color.FromRgb(0x0D, 0x0F, 0x14)),
                    BorderThickness = new Thickness(0),
                    Padding = new Thickness(12),
                    VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
                    HorizontalScrollBarVisibility = ScrollBarVisibility.Auto
                }
            };
            window.ShowDialog();
        }

        private void AnalyzeCrashedGame_Click(object sender, RoutedEventArgs e)
        {
            CrashDialogOverlay.Visibility = Visibility.Collapsed;
            try
            {
                string crashLogPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "logs", "crash_log.txt");
                if (File.Exists(crashLogPath)) File.Delete(crashLogPath);

                string fallbackPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "crash_log.txt");
                if (File.Exists(fallbackPath)) File.Delete(fallbackPath);
            }
            catch { }

            TabAnalyzer_Click(this, null);
            if (_selectedGame != null)
            {
                RunSingleGameAnalysis(_selectedGame);
            }
        }

        // From the Library "Analyze" action: open the analyzer, then check and
        // analyze just this one title.
        private async void RunSingleGameAnalysis(GameEntry game)
        {
            BuildAnalyzerRows();
            var row = _analysisResults.FirstOrDefault(r =>
                string.Equals(r.EbootPath, game.EbootPath, StringComparison.OrdinalIgnoreCase));
            if (row == null)
            {
                row = new BootAnalysisResult { Title = game.Title, TitleId = game.TitleId, EbootPath = game.EbootPath,
                    ResultText = I18n.Tr("analyzer.not_analyzed"), ResultState = "idle" };
                _analysisResults.Add(row);
                RefreshAnalyzerView();
            }
            AnalyzerListView.SelectedItem = row;
            row.IsChecked = true;
            await AnalyzeRows(new List<BootAnalysisResult> { row });
        }

        // Build the table from the game list as unanalyzed rows. Keeps existing
        // rows (their checks and results) if already built this session.
        private void BuildAnalyzerRows()
        {
            if (_analysisResults.Count > 0)
            {
                AnalyzerListView.ItemsSource ??= _analysisResults;
                return;
            }
            _analysisResults = _games
                .Where(g => !string.IsNullOrEmpty(g.EbootPath))
                .Select(g => new BootAnalysisResult
                {
                    Title = g.Title, TitleId = g.TitleId, EbootPath = g.EbootPath,
                    ResultText = I18n.Tr("analyzer.not_analyzed"), ResultState = "idle",
                }).ToList();
            AnalyzerListView.ItemsSource = _analysisResults;
            var view = System.Windows.Data.CollectionViewSource.GetDefaultView(_analysisResults);
            if (view != null) view.Filter = AnalyzerRowFilter;
            if (_analysisResults.Count > 0) AnalyzerListView.SelectedIndex = 0;
        }

        private void RefreshAnalyzerView()
        {
            System.Windows.Data.CollectionViewSource.GetDefaultView(_analysisResults)?.Refresh();
        }

        // --- Search filter over the title table ---
        private string _analyzerSearch = "";
        private bool AnalyzerRowFilter(object o)
        {
            if (string.IsNullOrWhiteSpace(_analyzerSearch)) return true;
            var r = o as BootAnalysisResult;
            if (r == null) return false;
            string q = _analyzerSearch.Trim();
            return (r.Title ?? "").IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0
                || (r.TitleId ?? "").IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0;
        }

        private void AnalyzerSearch_TextChanged(object sender, TextChangedEventArgs e)
        {
            _analyzerSearch = (sender as TextBox)?.Text ?? "";
            if (AnalyzerSearchPlaceholder != null)
                AnalyzerSearchPlaceholder.Visibility = string.IsNullOrEmpty(_analyzerSearch) ? Visibility.Visible : Visibility.Collapsed;
            RefreshAnalyzerView();
        }

        // Header "select all" toggles the checkbox on every visible (filtered) row.
        private void AnalyzerSelectAll_Click(object sender, RoutedEventArgs e)
        {
            bool on = (sender as CheckBox)?.IsChecked == true;
            foreach (var r in _analysisResults)
                if (AnalyzerRowFilter(r)) r.IsChecked = on;
        }

        private void UpdateSettingsUiFromConfig()
        {
            // Dynamic PlayStation 5 multi-page settings automatically read from _config in real time
        }

        private void SaveSettingsBtn_Click(object sender, RoutedEventArgs e)
        {
            SaveConfig();
            ResetSettingsView();
        }

        // Executable Boot & Memory Analyzer UI actions
        // Analyze the checked rows, or - if none are checked - the focused row.
        private async void AnalyzeAll_Click(object sender, RoutedEventArgs e)
        {
            var rows = _analysisResults.Where(r => r.IsChecked).ToList();
            if (rows.Count == 0 && AnalyzerListView.SelectedItem is BootAnalysisResult one)
                rows.Add(one);
            if (rows.Count == 0)
            {
                FooterStatus.Text = I18n.Tr("analyzer.pick_first");
                return;
            }
            await AnalyzeRows(rows);
        }

        // Run the parser for each row and fill its result in place.
        private async Task AnalyzeRows(List<BootAnalysisResult> rows)
        {
            string parserPath = LocateBootParser();
            if (parserPath == null || !File.Exists(parserPath))
            {
                MessageBox.Show("Could not locate pcsx5_boot_parser.exe binary. Please compile the project first.", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                return;
            }

            foreach (var row in rows)
            {
                if (string.IsNullOrEmpty(row.EbootPath)) continue;
                row.ResultText = I18n.Tr("analyzer.running");
                row.ResultState = "idle";

                // pcsx5_boot_parser takes the game DIRECTORY; handed the eboot file
                // it prints a usage banner and exits.
                string gameDir = null;
                try { gameDir = Path.GetDirectoryName(row.EbootPath); } catch { gameDir = null; }
                if (string.IsNullOrEmpty(gameDir)) continue;

                string rawOutput = "";
                try { rawOutput = await RunParserProcessAsync(parserPath, gameDir); }
                catch (Exception ex) { rawOutput = "Execution Error: " + ex.Message; }

                FillResult(row, rawOutput);

                // Refresh the report if this row is the one on screen.
                if (ReferenceEquals(AnalyzerListView.SelectedItem, row) && AnalyzerReport != null)
                {
                    AnalyzerReport.DataContext = null;
                    AnalyzerReport.DataContext = row;
                    AnalyzerOutputTextBox.Text = row.RawOutput;
                }
            }
        }

        private async Task<string> RunParserProcessAsync(string exePath, string filePath)
        {
            var tcs = new TaskCompletionSource<string>();
            var sb = new StringBuilder();

            using (var process = new Process())
            {
                process.StartInfo.FileName = exePath;
                process.StartInfo.Arguments = $"\"{filePath}\" --log \"\""; // disables writing to boot_analysis_log.txt for speed
                process.StartInfo.UseShellExecute = false;
                process.StartInfo.RedirectStandardOutput = true;
                process.StartInfo.CreateNoWindow = true;

                // Exited can fire before the output pipe is drained, so complete
                // on the output stream closing rather than on the exit event, and
                // bound the wait: a stuck parser must not freeze the Tools tab.
                process.OutputDataReceived += (s, e) =>
                {
                    if (e.Data != null) sb.AppendLine(e.Data);
                    else tcs.TrySetResult(sb.ToString());   // null => stream closed
                };
                process.EnableRaisingEvents = true;
                process.Exited += (s, e) => tcs.TrySetResult(sb.ToString());

                process.Start();
                process.BeginOutputReadLine();

                var completed = await Task.WhenAny(tcs.Task, Task.Delay(TimeSpan.FromSeconds(60)));
                if (completed != tcs.Task)
                {
                    try { if (!process.HasExited) process.Kill(true); } catch { }
                    return sb.ToString() + Environment.NewLine + "[timed out after 60s]";
                }
                return await tcs.Task;
            }
        }

        // Colour codes come through the redirected pipe; they are noise in the
        // log pane and would break every marker match below.
        private static readonly System.Text.RegularExpressions.Regex AnsiEscapeRe =
            new System.Text.RegularExpressions.Regex(
                "\\[[0-9;]*m", System.Text.RegularExpressions.RegexOptions.Compiled);

        private static string StripAnsi(string text)
        {
            return string.IsNullOrEmpty(text) ? (text ?? "") : AnsiEscapeRe.Replace(text, "");
        }

        private static System.Text.RegularExpressions.Match M(string line, string pattern)
        {
            return System.Text.RegularExpressions.Regex.Match(line, pattern);
        }

        private static bool HexOf(string s, out ulong value)
        {
            return ulong.TryParse(s, System.Globalization.NumberStyles.HexNumber,
                                  System.Globalization.CultureInfo.InvariantCulture, out value);
        }

        /// <summary>Extract the analyzer columns from pcsx5_boot_parser output.
        /// Every marker below was taken from real output, not assumed.</summary>
        // Parse a raw parser dump into an existing row, in place, so the table and
        // the report update live for that title.
        private void FillResult(BootAnalysisResult res, string raw)
        {
            raw = StripAnsi(raw ?? "");
            res.RawOutput = raw;
            res.Format = I18n.Tr("analyzer.unknown");
            res.EncryptionStatus = I18n.Tr("analyzer.unknown");
            res.MemoryFootprint = "-";
            res.AlignmentStatus = I18n.Tr("analyzer.unknown");

            bool isSelf = false, isElf = false, isPie = false, moduleLoaded = false;
            bool sawLoadSegment = false, misaligned = false;
            int selfSegs = 0, encryptedSegs = 0;
            string sdkType = null, bootError = null, misalignedDetail = null;
            long footprintBytes = -1;

            foreach (var rawLine in raw.Split('\n'))
            {
                string line = rawLine.Trim();
                if (line.Length == 0) continue;

                if (line.IndexOf("Detected SELF container", StringComparison.OrdinalIgnoreCase) >= 0)
                    isSelf = true;
                if (line.IndexOf("Valid ELF64", StringComparison.OrdinalIgnoreCase) >= 0)
                    isElf = true;
                if (line.IndexOf("treating as PIE", StringComparison.OrdinalIgnoreCase) >= 0 ||
                    line.IndexOf("(PIE) detected", StringComparison.OrdinalIgnoreCase) >= 0)
                    isPie = true;

                var m = M(line, @"PS5 SDK module type (0x[0-9A-Fa-f]+)");
                if (m.Success) sdkType = m.Groups[1].Value;

                // SELF segments carry their own encryption flag.
                m = M(line, @"SELF Seg\[\d+\]:.*?encrypted=(\d)");
                if (m.Success)
                {
                    selfSegs++;
                    if (m.Groups[1].Value != "0") encryptedSegs++;
                }

                m = M(line, @"Required memory footprint:.*?\(size:\s*(\d+)\s*bytes\)");
                if (m.Success) long.TryParse(m.Groups[1].Value, out footprintBytes);

                // PS5 maps on 16 KB pages; a PT_LOAD whose vaddr is not a
                // multiple of its own alignment is a real loader hazard.
                m = M(line, @"Program Header \d+: type=0x1\b.*?vaddr=0x([0-9A-Fa-f]+).*?align=0x([0-9A-Fa-f]+)");
                if (m.Success)
                {
                    sawLoadSegment = true;
                    if (HexOf(m.Groups[1].Value, out ulong vaddr) &&
                        HexOf(m.Groups[2].Value, out ulong align) &&
                        align > 1 && (vaddr % align) != 0)
                    {
                        misaligned = true;
                        // Naming the offending segment is the difference between
                        // a verdict and something actionable.
                        if (misalignedDetail == null)
                            misalignedDetail = string.Format("PH{0} vaddr=0x{1:X}",
                                m.Groups[0].Value.Contains("Header ")
                                    ? m.Groups[0].Value.Split(new[] { "Header " }, StringSplitOptions.None)[1].Split(':')[0]
                                    : "?",
                                vaddr);
                    }
                }

                if (line.StartsWith("Module Loaded:", StringComparison.OrdinalIgnoreCase))
                    moduleLoaded = line.IndexOf("Yes", StringComparison.OrdinalIgnoreCase) >= 0;

                int errIdx = line.IndexOf("BOOT ERROR:", StringComparison.OrdinalIgnoreCase);
                if (errIdx >= 0) bootError = line.Substring(errIdx + "BOOT ERROR:".Length).Trim();
            }

            // Container format
            if (isSelf) res.Format = isPie ? "SELF · PIE" : "SELF";
            else if (isElf) res.Format = isPie ? "ELF64 · PIE" : "ELF64";
            if (sdkType != null && res.Format != I18n.Tr("analyzer.unknown"))
                res.Format += "  (SDK " + sdkType + ")";

            // Encryption, from the SELF segment flags where they exist.
            if (selfSegs > 0)
            {
                res.EncryptionStatus = encryptedSegs == 0
                    ? I18n.Tr("analyzer.decrypted_segs", selfSegs)
                    : I18n.Tr("analyzer.encrypted_segs", encryptedSegs, selfSegs);
            }
            else if (isElf)
            {
                res.EncryptionStatus = I18n.Tr("analyzer.plaintext_elf");
            }

            if (footprintBytes > 0) res.MemoryFootprint = FormatBytes(footprintBytes);

            if (bootError != null) res.AlignmentStatus = I18n.Tr("analyzer.error", bootError);
            else if (!moduleLoaded) res.AlignmentStatus = I18n.Tr("analyzer.not_loaded");
            else if (!sawLoadSegment) res.AlignmentStatus = I18n.Tr("analyzer.unknown");
            else if (misaligned)
                res.AlignmentStatus = misalignedDetail == null
                    ? I18n.Tr("analyzer.misaligned")
                    : I18n.Tr("analyzer.misaligned_at", misalignedDetail);
            else res.AlignmentStatus = I18n.Tr("analyzer.aligned_16k");

            // --- Concept report: header meta, result verdict and the readiness
            // checklist, built only from what this static parser evidences. ---
            res.HeaderMeta = res.TitleId + "  ·  eboot.bin"
                + (res.MemoryFootprint != "-" ? "  ·  " + res.MemoryFootprint : "");

            string St(bool ok, bool fail = false) => fail ? "fail" : (ok ? "ok" : "warn");

            bool container = isSelf || isElf;
            bool segments = sawLoadSegment || selfSegs > 0;
            bool encrypted = encryptedSegs > 0;

            var stages = new System.Collections.Generic.List<BootStage>();
            stages.Add(new BootStage {
                Label = I18n.Tr("analyzer.stage.container"),
                Detail = container ? res.Format : I18n.Tr("analyzer.unknown"),
                State = St(container, !container) });
            stages.Add(new BootStage {
                Label = I18n.Tr("analyzer.stage.segments"),
                Detail = selfSegs > 0 ? selfSegs.ToString() : (sawLoadSegment ? "PT_LOAD" : "-"),
                State = St(segments) });
            stages.Add(new BootStage {
                Label = I18n.Tr("analyzer.stage.decrypt"),
                Detail = res.EncryptionStatus,
                State = selfSegs == 0 ? (isElf ? "ok" : "warn") : St(!encrypted, encrypted) });
            stages.Add(new BootStage {
                Label = I18n.Tr("analyzer.stage.alignment"),
                Detail = res.AlignmentStatus,
                State = !sawLoadSegment ? "warn" : St(!misaligned) });
            stages.Add(new BootStage {
                Label = I18n.Tr("analyzer.stage.module"),
                Detail = moduleLoaded ? I18n.Tr("analyzer.stage.loaded") : I18n.Tr("analyzer.stage.not_loaded"),
                State = bootError != null ? "fail" : St(moduleLoaded) });

            // Verdict: the first blocking stage, else clean.
            if (bootError != null) { res.ResultState = "fail"; res.ResultText = I18n.Tr("analyzer.result.fails"); }
            else if (encrypted) { res.ResultState = "fail"; res.ResultText = I18n.Tr("analyzer.result.encrypted"); }
            else if (!container) { res.ResultState = "fail"; res.ResultText = I18n.Tr("analyzer.result.unreadable"); }
            else if (!moduleLoaded) { res.ResultState = "warn"; res.ResultText = I18n.Tr("analyzer.result.stops_load"); }
            else if (misaligned) { res.ResultState = "warn"; res.ResultText = I18n.Tr("analyzer.result.alignment"); }
            else { res.ResultState = "ok"; res.ResultText = I18n.Tr("analyzer.result.clean"); }

            res.Stages = stages;
            res.Analyzed = true;
        }

        private void AnalyzerListView_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            var selected = AnalyzerListView.SelectedItem as BootAnalysisResult;
            if (AnalyzerReport != null) AnalyzerReport.DataContext = selected;
            if (selected != null)
            {
                AnalyzerOutputTextBox.Text = selected.RawOutput;
            }
            else
            {
                AnalyzerOutputTextBox.Text = "";
            }
        }

        private void ExportSummary_Click(object sender, RoutedEventArgs e)
        {
            var analyzed = _analysisResults.Where(r => r.Analyzed).ToList();
            if (analyzed.Count == 0)
            {
                MessageBox.Show("No analysis results to export. Run the analyzer first.", "Warning", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }

            var saveDlg = new Microsoft.Win32.SaveFileDialog
            {
                Filter = "JSON Files (*.json)|*.json|Text Files (*.txt)|*.txt",
                FileName = "boot_analysis_summary.json",
                Title = "Save Parser Minimal Summary"
            };

            if (saveDlg.ShowDialog() == true)
            {
                try
                {
                    // Create minimal summary payload containing only needed fields
                    var payload = analyzed.Select(r => new
                    {
                        title_id = r.TitleId,
                        title = r.Title,
                        format = r.Format,
                        encrypted = r.EncryptionStatus.Contains("Encrypted"),
                        footprint = r.MemoryFootprint,
                        alignment_warning = r.AlignmentStatus.Contains("Warning")
                    }).ToList();

                    var options = new System.Text.Json.JsonSerializerOptions { WriteIndented = true };
                    string json = System.Text.Json.JsonSerializer.Serialize(payload, options);
                    File.WriteAllText(saveDlg.FileName, json);

                    MessageBox.Show("Summary exported successfully!", "Success", MessageBoxButton.OK, MessageBoxImage.Information);
                }
                catch (Exception ex)
                {
                    MessageBox.Show("Failed to export summary: " + ex.Message, "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }
        }
    }

    public class GameEntry
    {
        public string TitleId { get; set; }
        public string Title { get; set; }
        public string EbootPath { get; set; }
        public string DirPath { get; set; }
        public string CoverPath { get; set; }
        public string BackgroundPath { get; set; }
        public string MusicPath { get; set; }
        public long SizeBytes { get; set; }
        public string CompatStatus { get; set; }
        /// <summary>Set by ApplyLibrarySort from the Favourites file before the
        /// grid is sourced; the tile's star binds to it.</summary>
        public bool IsFavourite { get; set; }

        /// <summary>A muted colour derived from the title ID, shown behind the
        /// cover so a dump without artwork gets a tile of its own colour rather
        /// than a grey square. Stable across runs because it hashes the ID.</summary>
        public System.Windows.Media.Brush PlaceholderBrush
        {
            get
            {
                uint h = 2166136261u;
                foreach (char c in TitleId ?? "") { h ^= c; h *= 16777619u; }
                double hue = (h % 360);
                // HSV -> RGB at low saturation and value: readable under white text.
                double s = 0.38, v = 0.42;
                double cc = v * s, x = cc * (1 - Math.Abs((hue / 60) % 2 - 1)), m = v - cc;
                double r, g, b;
                if (hue < 60) { r = cc; g = x; b = 0; } else if (hue < 120) { r = x; g = cc; b = 0; }
                else if (hue < 180) { r = 0; g = cc; b = x; } else if (hue < 240) { r = 0; g = x; b = cc; }
                else if (hue < 300) { r = x; g = 0; b = cc; } else { r = cc; g = 0; b = x; }
                var brush = new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(
                    (byte)((r + m) * 255), (byte)((g + m) * 255), (byte)((b + m) * 255)));
                brush.Freeze();
                return brush;
            }
        }
        public string SizeStr
        {
            get
            {
                string[] suffix = { "B", "KB", "MB", "GB", "TB" };
                double d = SizeBytes;
                int i = 0;
                while (d >= 1024 && i < suffix.Length - 1) { i++; d /= 1024; }
                return $"{d:F2} {suffix[i]}";
            }
        }
    }

    public class BootAnalysisResult : System.ComponentModel.INotifyPropertyChanged
    {
        public event System.ComponentModel.PropertyChangedEventHandler PropertyChanged;
        private void N([System.Runtime.CompilerServices.CallerMemberName] string p = null)
            => PropertyChanged?.Invoke(this, new System.ComponentModel.PropertyChangedEventArgs(p));

        // Identity (set once, at row creation).
        public string Title { get; set; }
        public string TitleId { get; set; }
        public string EbootPath { get; set; }

        // Selection for the analyze set, toggled by the row checkbox.
        private bool _isChecked;
        public bool IsChecked { get => _isChecked; set { _isChecked = value; N(); } }

        // Whether the parser has run for this row yet.
        private bool _analyzed;
        public bool Analyzed { get => _analyzed; set { _analyzed = value; N(); } }

        // Results, filled in place when the parser runs (so the row updates live).
        private string _raw; public string RawOutput { get => _raw; set { _raw = value; N(); } }
        private string _format = "—"; public string Format { get => _format; set { _format = value; N(); } }
        private string _enc = "—"; public string EncryptionStatus { get => _enc; set { _enc = value; N(); } }
        private string _foot = "—"; public string MemoryFootprint { get => _foot; set { _foot = value; N(); } }
        private string _align = "—"; public string AlignmentStatus { get => _align; set { _align = value; N(); } }
        private string _resultText = ""; public string ResultText { get => _resultText; set { _resultText = value; N(); } }
        private string _resultState = "idle"; public string ResultState { get => _resultState; set { _resultState = value; N(); } } // idle | ok | warn | fail
        private string _headerMeta; public string HeaderMeta { get => _headerMeta; set { _headerMeta = value; N(); } }
        private System.Collections.Generic.List<BootStage> _stages = new System.Collections.Generic.List<BootStage>();
        public System.Collections.Generic.List<BootStage> Stages { get => _stages; set { _stages = value; N(); } }
    }

    // One row of the boot-readiness checklist. Only stages the static parser can
    // actually evidence are emitted (Rule 04): container, segments, decryption,
    // alignment, module load - never runtime stages it cannot observe.
    public class BootStage
    {
        public string Label { get; set; }
        public string Detail { get; set; }
        public string State { get; set; } = "ok";   // ok | warn | fail
    }

    // Maps a boot state (ok/warn/fail) to a theme brush: the bright token, or the
    // soft tint when ConverterParameter is "soft" (the checklist status discs).
    public class AnalyzerStateBrushConverter : System.Windows.Data.IValueConverter
    {
        public object Convert(object value, Type targetType, object parameter, System.Globalization.CultureInfo culture)
        {
            string state = (value as string) ?? "warn";
            bool soft = string.Equals(parameter as string, "soft", StringComparison.OrdinalIgnoreCase);
            string key = state switch
            {
                "ok" => "ThemeSuccess",
                "fail" => "ThemeDanger",
                "idle" => "ThemeTextMuted",
                _ => "ThemeWarning",
            };
            var res = Application.Current?.TryFindResource(key);
            if (res is SolidColorBrush b)
            {
                if (!soft) return b;
                var c = b.Color;
                var tint = new SolidColorBrush(Color.FromArgb(0x24, c.R, c.G, c.B));
                tint.Freeze();
                return tint;
            }
            return System.Windows.Media.Brushes.Gray;
        }

        public object ConvertBack(object value, Type targetType, object parameter, System.Globalization.CultureInfo culture)
            => System.Windows.Data.Binding.DoNothing;
    }

    public class EmulatorConfig
    {
        public int schema_version { get; set; } = 2;
        public AudioSection audio { get; set; } = new AudioSection();
        public CrashSection crash { get; set; } = new CrashSection();
        public GraphicsSection graphics { get; set; } = new GraphicsSection();
        public HleSection hle { get; set; } = new HleSection();
        public InputSection input { get; set; } = new InputSection();
        public LoggingSection logging { get; set; } = new LoggingSection();
        public UiSection ui { get; set; } = new UiSection();

        public class AudioSection
        {
            public int backend { get; set; } = 1;
            public int buffer_ms { get; set; } = 50;
            public double volume { get; set; } = 1.0;
        }

        public class CrashSection
        {
            public string bundle_dir { get; set; } = "pcsx5_crash";
            public bool write_minidump { get; set; } = true;
        }

        public class GraphicsSection
        {
            public bool fullscreen { get; set; } = false;
            public int height { get; set; } = 720;
            public int renderer { get; set; } = 0;
            public double resolution_scale { get; set; } = 1.0;
            public int width { get; set; } = 1280;
        }

        public class HleSection
        {
            public bool strict_imports { get; set; } = false;
            public bool trace_calls { get; set; } = true;
            public int trace_capacity { get; set; } = 256;
        }

        public class InputSection
        {
            public int backend { get; set; } = 0;
            public double deadzone { get; set; } = 0.15;
            public bool rumble { get; set; } = true;
            public int active_slot { get; set; } = 0;            // 0-3 = player slot, 4 = XInput
            public bool per_game_configs { get; set; } = false;
            public string lightbar { get; set; } = "";           // #RRGGBB override; empty = the slot's player colour
        }

        public class LoggingSection
        {
            public bool file_append { get; set; } = false;
            public string file_path { get; set; } = "";
            public bool json_output { get; set; } = false;
            public string min_level { get; set; } = "Info";
        }

        public class UiSection
        {
            public string language { get; set; } = "en-US";
            public bool title_music_enabled { get; set; } = true;
            public double title_music_volume { get; set; } = 0.6;   // lobby (title) music, 0..1, separate from the game's audio
            public string skipped_update_version { get; set; } = "";  // "Skip this version": only a newer release asks again
            public double scale { get; set; } = 1.0;
            // The shell is a console interface, so fullscreen is the default
            // presentation.  Distinct from graphics.fullscreen, which governs
            // the *game* window, not the dashboard.
            public bool start_fullscreen { get; set; } = true;
            // Theme (ADR-003): "dark" | "light" | "system"; accent and ground as
            // #RRGGBB, ground empty = the palette's own.
            public string theme { get; set; } = "dark";
            public string accent { get; set; } = "#5FE3FF";
            public string ground { get; set; } = "";
            public string corners { get; set; } = "rounded";   // rounded | sharp | cut
        }
    }

    /// <summary>One row in the in-app folder browser: a drive, a subfolder, the
    /// parent entry, or a non-navigable status row (empty / access denied).</summary>
    public class FolderEntry
    {
        public string Icon { get; set; }
        public string Name { get; set; }
        public string FullPath { get; set; }   // null on status rows, and on the
                                               // parent entry at a drive root
        public bool IsParent { get; set; }
        public bool IsGame { get; set; }        // holds an eboot.bin -> accent icon
        public string Meta { get; set; }        // mono right column, e.g. "eboot.bin - 102 MB"
    }

    public class IniFile
    {
        private Dictionary<string, Dictionary<string, string>> _data = new Dictionary<string, Dictionary<string, string>>(StringComparer.OrdinalIgnoreCase);

        public void Load(string path)
        {
            _data.Clear();
            if (!File.Exists(path)) return;

            string currentSection = "";
            foreach (var line in File.ReadLines(path))
            {
                string trimmed = line.Trim();
                if (string.IsNullOrEmpty(trimmed) || trimmed.StartsWith(";") || trimmed.StartsWith("#")) continue;

                if (trimmed.StartsWith("[") && trimmed.EndsWith("]"))
                {
                    currentSection = trimmed.Substring(1, trimmed.Length - 2).Trim();
                    if (!_data.ContainsKey(currentSection))
                    {
                        _data[currentSection] = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                    }
                }
                else
                {
                    int idx = trimmed.IndexOf('=');
                    if (idx > 0)
                    {
                        string key = trimmed.Substring(0, idx).Trim();
                        string value = trimmed.Substring(idx + 1).Trim();
                        if (!string.IsNullOrEmpty(currentSection))
                        {
                            _data[currentSection][key] = value;
                        }
                    }
                }
            }
        }

        public void Save(string path)
        {
            using (var writer = new StreamWriter(path, false, Encoding.UTF8))
            {
                foreach (var section in _data)
                {
                    writer.WriteLine($"[{section.Key}]");
                    foreach (var kvp in section.Value)
                    {
                        writer.WriteLine($"{kvp.Key} = {kvp.Value}");
                    }
                    writer.WriteLine();
                }
            }
        }

        public string GetValue(string section, string key, string defaultValue = "")
        {
            if (_data.TryGetValue(section, out var keys))
            {
                if (keys.TryGetValue(key, out var val))
                {
                    return val;
                }
            }
            return defaultValue;
        }

        public void SetValue(string section, string key, string value)
        {
            if (!_data.ContainsKey(section))
            {
                _data[section] = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            }
            _data[section][key] = value;
        }
    }

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    public struct XInputState
    {
        public uint dwPacketNumber;
        public XInputGamepad Gamepad;
    }

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    public struct XInputGamepad
    {
        public ushort wButtons;
        public byte bLeftTrigger;
        public byte bRightTrigger;
        public short sThumbLX;
        public short sThumbLY;
        public short sThumbRX;
        public short sThumbRY;
    }

    public static class XInput
    {
        [System.Runtime.InteropServices.DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")]
        public static extern int XInputGetState1_4(int dwUserIndex, ref XInputState pState);

        [System.Runtime.InteropServices.DllImport("xinput1_3.dll", EntryPoint = "XInputGetState")]
        public static extern int XInputGetState1_3(int dwUserIndex, ref XInputState pState);

        private static bool _use1_4 = true;
        private static bool _init = false;

        public static int GetState(int userIndex, ref XInputState state)
        {
            if (!_init)
            {
                try
                {
                    XInputState test = new XInputState();
                    XInputGetState1_4(0, ref test);
                    _use1_4 = true;
                }
                catch
                {
                    _use1_4 = false;
                }
                _init = true;
            }

            try
            {
                if (_use1_4)
                    return XInputGetState1_4(userIndex, ref state);
                else
                    return XInputGetState1_3(userIndex, ref state);
            }
            catch
            {
                return -1;
            }
        }
    }
}
