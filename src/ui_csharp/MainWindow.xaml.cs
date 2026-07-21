using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
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

        // In-process emulator core (pcsx5_core.dll via CoreBridge).  A single
        // dedicated thread owns the whole session (GLFW window + message loop
        // must stay on one thread); the WPF thread only talks to it through
        // the log/window callbacks and pcsx5_stop().
        private Thread _coreThread = null;
        private volatile bool _coreRunning = false;
        private readonly CoreBridge.LogCallback _coreLogCallback;
        private readonly CoreBridge.WindowCallback _coreWindowCallback;

        // Console output batching: reader threads enqueue lines, a DispatcherTimer drains
        // them in batches so floods of stdout lines never saturate the dispatcher queue.
        private readonly ConcurrentQueue<string> _consoleLineQueue = new ConcurrentQueue<string>();
        private System.Windows.Threading.DispatcherTimer _consoleDrainTimer;
        private const int MaxConsoleChars = 256 * 1024; // ~256KB cap on the console TextBox

        // Embedded emulator window state (game renders inside the launcher window)
        private EmulatorWindowHost _emuHost = null;
        private IntPtr _embeddedEmuHwnd = IntPtr.Zero;
        private bool _gameConsoleVisible = false;

        public MainWindow()
        {
            InitializeComponent();
            InitializeAudioPlayer();
            this.Closed += MainWindow_Closed;

            // Keep the callback delegates referenced for the process lifetime
            // so native code never calls into collected delegates.
            _coreLogCallback = OnCoreLog;
            _coreWindowCallback = OnCoreWindow;

            _consoleDrainTimer = new System.Windows.Threading.DispatcherTimer();
            _consoleDrainTimer.Interval = TimeSpan.FromMilliseconds(150);
            _consoleDrainTimer.Tick += (s, e) => DrainConsoleQueue();
            _consoleDrainTimer.Start();
        }

        private void MainWindow_Closed(object sender, EventArgs e)
        {
            if (_coreRunning)
            {
                try { CoreBridge.pcsx5_stop(); } catch { }
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
                LoadGames();
                InitializeControllerPolling();

                // Start Discord RPC and load translations
                _discordRpc.Start();
                TranslateUi();

                // Set default tab to Library
                TabLibrary_Click(this, null);

                // Ask for the games folder on first launch
                MaybeShowFirstRunSetup();
            }
            catch (Exception ex)
            {
                try { File.WriteAllText(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "ui_crash_log.txt"), ex.ToString()); } catch { }
                MessageBox.Show(ex.ToString(), "UI Startup Error", MessageBoxButton.OK, MessageBoxImage.Error);
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
                                AddGameTile(entry);
                            }
                        }
                    }
                }
                catch (Exception ex)
                {
                    LogConsole($"Error loading games from {folder}: {ex.Message}");
                }
            }

            FooterGamesCount.Text = $"Games Loaded: {_games.Count}";

            if (_games.Count > 0)
            {
                SelectGame(_games[0]);
            }
        }

        private GameEntry ParseGameDirectory(string dir)
        {
            string dirName = Path.GetFileName(dir);
            string titleId = CanonicalizeTitleId(dirName);

            // Locate eboot
            string ebootPath = null;
            string[] preferredEboots = {
                Path.Combine(dir, "decrypted", "eboot.bin"),
                Path.Combine(dir, "eboot.bin.esbak"),
                Path.Combine(dir, "eboot.bin"),
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

            // Compatibility Status
            string compatStatus = "untested";
            string compatFile = Path.Combine(_compatDir, "titles", titleId + ".json");
            if (File.Exists(compatFile))
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

        private void AddGameTile(GameEntry game)
        {
            var border = new Border
            {
                Width = 145,
                Height = 145,
                Margin = new Thickness(10, 0, 10, 0),
                CornerRadius = new CornerRadius(8),
                Background = new SolidColorBrush(Color.FromArgb(30, 255, 255, 255)),
                BorderBrush = new SolidColorBrush(Color.FromArgb(15, 255, 255, 255)),
                BorderThickness = new Thickness(1.5),
                Cursor = Cursors.Hand,
                ClipToBounds = true,
                Tag = game,
                VerticalAlignment = VerticalAlignment.Center
            };

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
                    FontSize = 11,
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

            // Hover effects
            border.MouseEnter += (s, e) =>
            {
                if (_selectedGame != game)
                {
                    border.BorderBrush = new SolidColorBrush(Color.FromRgb(0, 153, 255));
                    overlay.Background = new SolidColorBrush(Color.FromArgb(60, 255, 255, 255));
                }
            };
            border.MouseLeave += (s, e) =>
            {
                if (_selectedGame != game)
                {
                    border.BorderBrush = new SolidColorBrush(Color.FromArgb(15, 255, 255, 255));
                    overlay.Background = new SolidColorBrush(Color.FromArgb(0, 255, 255, 255));
                    border.Effect = null;
                }
            };
            border.MouseDown += (s, e) =>
            {
                SelectGame(game);
            };

            GamesWrapPanel.Children.Add(border);
        }

        private void SelectGame(GameEntry game)
        {
            _selectedGame = game;

            DetailTitle.Text = game.Title;
            DetailTitleId.Text = game.TitleId;
            DetailSize.Text = $"{I18n.Tr("library.size_label")} {FormatBytes(game.SizeBytes)}";

            if (_discordRpc != null)
            {
                _discordRpc.UpdatePresence($"Browsing {game.Title}", $"ID: {game.TitleId}");
            }

            DetailPath.Text = game.EbootPath;

            DetailCompatText.Text = game.CompatStatus;
            switch (game.CompatStatus)
            {
                case "PLAYABLE":
                case "COMPLETE":
                    DetailCompatBadge.Background = new SolidColorBrush(Color.FromRgb(46, 125, 50));
                    break;
                case "MENU":
                case "INTRO":
                    DetailCompatBadge.Background = new SolidColorBrush(Color.FromRgb(239, 108, 0));
                    break;
                default:
                    DetailCompatBadge.Background = new SolidColorBrush(Color.FromRgb(117, 117, 117));
                    break;
            }

            var coverImage = LoadImageHelper(game.CoverPath);
            if (coverImage != null)
            {
                GameCoverDisplayBorder.Background = new ImageBrush
                {
                    ImageSource = coverImage,
                    Stretch = Stretch.UniformToFill
                };
            }
            else
            {
                GameCoverDisplayBorder.Background = new SolidColorBrush(Color.FromArgb(30, 255, 255, 255));
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
                        cardOverlay.Background = new SolidColorBrush(Color.FromArgb(60, 255, 255, 255));
                    card.Effect = new System.Windows.Media.Effects.DropShadowEffect
                    {
                        Color = Color.FromRgb(0, 153, 255),
                        BlurRadius = 15,
                        ShadowDepth = 0,
                        Opacity = 0.6
                    };
                }
                else
                {
                    card.BorderBrush = new SolidColorBrush(Color.FromArgb(15, 255, 255, 255));
                    if (cardOverlay != null)
                        cardOverlay.Background = new SolidColorBrush(Color.FromArgb(0, 255, 255, 255));
                    card.Effect = null;
                }
            }

            FooterStatus.Text = $"Selected: {game.TitleId}";

            // Trigger music playback with debounce
            TriggerTitleMusic(game);
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
            double volume = _config.audio.volume;

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
            if (_coreRunning) return; // single game at a time (core globals are one-shot)

            var game = _selectedGame;

            LaunchButton.IsEnabled = false;
            StopButton.Visibility = Visibility.Visible;
            ConsoleOutputTextBox.Clear();

            LogConsole($"Launching in-process: \"{game.EbootPath}\"");
            FooterStatus.Text = $"Running: {game.Title}";

            // Stop title music when game launches
            Dispatcher.Invoke(() => _mediaPlayer.Stop());

            // Switch from the library to the embedded game view; the emulator's
            // window gets reparented into EmulatorHostPresenter once the core
            // reports its HWND through the window callback.
            GameBarTitle.Text = game.Title;
            _emuHost = new EmulatorWindowHost();
            EmulatorHostPresenter.Content = _emuHost;
            EmulatorHostPresenter.SizeChanged += EmulatorHostPresenter_SizeChanged;
            LibraryView.Visibility = Visibility.Collapsed;
            AnalyzerView.Visibility = Visibility.Collapsed;
            ControllerView.Visibility = Visibility.Collapsed;
            SettingsView.Visibility = Visibility.Collapsed;
            LogsView.Visibility = Visibility.Collapsed;
            GameView.Visibility = Visibility.Visible;

            string appDir = AppDomain.CurrentDomain.BaseDirectory;
            var options = new CoreBridge.Pcsx5Options
            {
                ConfigDir = Path.Combine(appDir, "pcsx5_config"),
                CrashDir = Path.Combine(appDir, "pcsx5_crash"),
                TitleId = game.TitleId,
                Embed = 1,
                InProc = 1,
            };

            // The core owns its GLFW window on this thread for the whole run;
            // UI updates flow back through the log/window callbacks only.
            _coreRunning = true;
            _coreThread = new Thread(() => CoreRunThread(options, game));
            _coreThread.IsBackground = true;
            _coreThread.Name = "pcsx5-core";
            _coreThread.Start();
        }

        // Runs entirely on the dedicated core thread: init -> load -> run
        // (blocking) -> shutdown, then posts UI teardown back to the WPF thread.
        private void CoreRunThread(CoreBridge.Pcsx5Options options, GameEntry game)
        {
            int rc = -1;
            try
            {
                rc = CoreBridge.pcsx5_init(ref options, _coreLogCallback, IntPtr.Zero);
                if (rc != 0)
                {
                    LogConsole($"Error: pcsx5_init failed (code {rc}).");
                }
                else
                {
                    rc = CoreBridge.pcsx5_load(game.EbootPath);
                    if (rc != 0)
                    {
                        LogConsole($"Error: pcsx5_load failed (code {rc}).");
                    }
                    else
                    {
                        if (_discordRpc != null)
                        {
                            _discordRpc.UpdatePresence($"Playing {game.Title}", "In-Game");
                        }
                        rc = CoreBridge.pcsx5_run(_coreWindowCallback, IntPtr.Zero);
                    }
                    CoreBridge.pcsx5_shutdown();
                }
            }
            catch (Exception ex)
            {
                LogConsole("Launch Error: " + ex.Message);
            }
            finally
            {
                int exitCode = rc;
                Dispatcher.Invoke(() =>
                {
                    _coreRunning = false;
                    _coreThread = null;
                    LaunchButton.IsEnabled = true;
                    StopButton.Visibility = Visibility.Collapsed;
                    LogConsole($"Emulator core terminated (exit code {exitCode}).");
                    FooterStatus.Text = "Ready";

                    // Unhost the emulator window and restore the library
                    TeardownGameView();

                    // Restore Discord status
                    if (_discordRpc != null)
                    {
                        if (_selectedGame != null)
                            _discordRpc.UpdatePresence($"Browsing {_selectedGame.Title}", $"ID: {_selectedGame.TitleId}");
                        else
                            _discordRpc.UpdatePresence("Idle", "Main Menu");
                    }

                    // Restart title music when game exits
                    if (_selectedGame != null)
                    {
                        TriggerTitleMusic(_selectedGame);
                    }
                });
            }
        }

        // Core log callback (fires on any core thread): format like the CLI's
        // [Category][Level] prefix and enqueue for the console panel.
        private void OnCoreLog(int level, int category, IntPtr msg, IntPtr user)
        {
            string text = Marshal.PtrToStringAnsi(msg) ?? "";
            LogConsole($"[{CoreBridge.CategoryName(category)}][{CoreBridge.LevelName(level)}] {text}");
        }

        // Core window callback: the presentation HWND, reparented into the
        // embedded game view on the UI thread.
        private void OnCoreWindow(ulong hwnd, IntPtr user)
        {
            IntPtr handle = new IntPtr((long)hwnd);
            Dispatcher.InvokeAsync(() => EmbedEmulatorWindow(handle));
        }

        private void StopButton_Click(object sender, RoutedEventArgs e)
        {
            if (_coreRunning)
            {
                try
                {
                    CoreBridge.pcsx5_stop();
                    LogConsole("Stop requested; waiting for the guest to unwind...");
                }
                catch { }
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
            NativeMethods.ShowWindow(emuHwnd, NativeMethods.SW_SHOW);
            ResizeEmbeddedWindow();
            NativeMethods.SetFocus(emuHwnd); // keyboard input must reach the emulator window
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
        }

        private void GameConsoleButton_Click(object sender, RoutedEventArgs e)
        {
            SetGameConsoleVisible(!_gameConsoleVisible);
        }

        // Move the shared console panel between the Logs view and the game view
        // side panel (avoids WPF HwndHost airspace issues — no overlap).
        private void SetGameConsoleVisible(bool visible)
        {
            _gameConsoleVisible = visible;
            if (visible)
            {
                LogsConsoleSlot.Child = null;
                GameConsoleSlot.Child = ConsoleBorder;
                GameConsolePanel.Visibility = Visibility.Visible;
                GameConsoleButton.Content = "Hide Console";
            }
            else
            {
                GameConsoleSlot.Child = null;
                LogsConsoleSlot.Child = ConsoleBorder;
                GameConsolePanel.Visibility = Visibility.Collapsed;
                GameConsoleButton.Content = "Console";
            }
        }

        // Toggle the in-app DualSense input tester panel (replaces the old
        // external dualsense_visual.exe launcher).
        private void ControllerTesterBtn_Click(object sender, RoutedEventArgs e)
        {
            bool show = ControllerTesterPanel.Visibility != Visibility.Visible;
            ControllerTesterPanel.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
            ControllerTesterBtn.Content = show ? "Hide Tester" : "Input Tester";
            if (show)
            {
                StartControllerVizPolling(); // make sure the 60 Hz poll is running
                UpdateTesterStatusText();
            }
            else
            {
                TesterStopAllOutputs();
            }
        }

        // --- INPUT TESTER OUTPUT ACTIONS ---
        private int _playerLedIndex = 0;
        private System.Windows.Threading.DispatcherTimer _rumbleStopTimer;

        private void UpdateTesterStatusText()
        {
            if (TesterStatusText == null) return;
            if (!WindowsDualSenseReader.TryGetState(out var state) || !state.Connected)
            {
                TesterStatusText.Text = "No DualSense connected. Output tests will have no effect.";
            }
            else if (WindowsDualSenseReader.IsBluetooth)
            {
                TesterStatusText.Text = "Connected via Bluetooth — rumble, lightbar and trigger effects are sent with the BT output report.";
            }
            else
            {
                TesterStatusText.Text = "Connected via USB.";
            }
        }

        private void PulseRumble(byte largeMotor, byte smallMotor)
        {
            WindowsDualSenseReader.SetRumble(largeMotor, smallMotor);
            if (_rumbleStopTimer == null)
            {
                _rumbleStopTimer = new System.Windows.Threading.DispatcherTimer();
                _rumbleStopTimer.Interval = TimeSpan.FromMilliseconds(1200);
                _rumbleStopTimer.Tick += (s, ev) =>
                {
                    _rumbleStopTimer.Stop();
                    WindowsDualSenseReader.SetRumble(0, 0);
                };
            }
            _rumbleStopTimer.Stop();
            _rumbleStopTimer.Start();
        }

        private void TesterRumbleLow_Click(object sender, RoutedEventArgs e) => PulseRumble(255, 0);

        private void TesterRumbleHigh_Click(object sender, RoutedEventArgs e) => PulseRumble(0, 255);

        private void TesterRumbleStop_Click(object sender, RoutedEventArgs e)
        {
            _rumbleStopTimer?.Stop();
            WindowsDualSenseReader.SetRumble(0, 0);
        }

        private void TesterLightbarApply_Click(object sender, RoutedEventArgs e)
        {
            WindowsDualSenseReader.SetLightbar(
                (byte)CtrlColorRSlider.Value,
                (byte)CtrlColorGSlider.Value,
                (byte)CtrlColorBSlider.Value);
        }

        private void TesterLightbarReset_Click(object sender, RoutedEventArgs e)
        {
            WindowsDualSenseReader.ResetLightbar();
        }

        private void TesterPlayerLed_Click(object sender, RoutedEventArgs e)
        {
            byte[] patterns = { 0x04, 0x0A, 0x15, 0x1B, 0x1F }; // players 1-5
            _playerLedIndex = (_playerLedIndex + 1) % patterns.Length;
            WindowsDualSenseReader.SetPlayerLeds(patterns[_playerLedIndex]);
            if (TesterStatusText != null)
            {
                TesterStatusText.Text = $"Player LED pattern {_playerLedIndex + 1} of {patterns.Length}";
            }
        }

        private void TesterTriggerOff_Click(object sender, RoutedEventArgs e)
        {
            WindowsDualSenseReader.ResetTrigger(DualSenseTriggerSide.Both);
            SetTesterStatus("Adaptive triggers released (Off).");
        }

        private void TesterTriggerFeedback_Click(object sender, RoutedEventArgs e)
        {
            // Continuous resistance from 25% travel with strong force
            WindowsDualSenseReader.SetAdaptiveTrigger(
                DualSenseTriggerSide.Both, DualSenseTriggerMode.Feedback, param1: 64, param2: 200);
            SetTesterStatus("Trigger effect: Feedback (continuous resistance).");
        }

        private void TesterTriggerWeapon_Click(object sender, RoutedEventArgs e)
        {
            // Section resistance ("weapon trigger click") between 25% and 75% travel
            WindowsDualSenseReader.SetAdaptiveTrigger(
                DualSenseTriggerSide.Both, DualSenseTriggerMode.Weapon, param1: 64, param2: 192);
            SetTesterStatus("Trigger effect: Weapon (section resistance).");
        }

        private void TesterTriggerVibration_Click(object sender, RoutedEventArgs e)
        {
            // Vibration effect starting at 20% travel, 40 Hz
            WindowsDualSenseReader.SetAdaptiveTrigger(
                DualSenseTriggerSide.Both, DualSenseTriggerMode.Vibration, param1: 51, param2: 40, force: 255);
            SetTesterStatus("Trigger effect: Vibration.");
        }

        private void SetTesterStatus(string message)
        {
            if (TesterStatusText != null)
            {
                TesterStatusText.Text = message + (WindowsDualSenseReader.IsBluetooth ? " (Bluetooth)" : "");
            }
            LogConsole("Tester: " + message);
        }

        // Restore neutral output state when the tester is closed or the tab is left.
        private void TesterStopAllOutputs()
        {
            _rumbleStopTimer?.Stop();
            WindowsDualSenseReader.SetRumble(0, 0);
            WindowsDualSenseReader.ResetTrigger(DualSenseTriggerSide.Both);
            WindowsDualSenseReader.ResetLightbar();
        }

        private void UpdateTesterReadouts(in HostGamepadState state)
        {
            if (TesterConnectionText == null) return; // panel not initialized yet

            if (!state.Connected)
            {
                TesterConnectionText.Text = "No controller connected";
                TesterButtonsText.Text = "Buttons: -";
                TesterL2Bar.Value = 0;
                TesterR2Bar.Value = 0;
                TesterL2Text.Text = "0";
                TesterR2Text.Text = "0";
                TesterLeftStickText.Text = "Left Stick:  X=- Y=-";
                TesterRightStickText.Text = "Right Stick: X=- Y=-";
                TesterTouchText.Text = "Touchpad: -";
                return;
            }

            TesterConnectionText.Text = WindowsDualSenseReader.IsBluetooth
                ? "DualSense connected (Bluetooth)"
                : "DualSense connected (USB)";

            var pressed = new List<string>();
            foreach (HostGamepadButtons flag in Enum.GetValues(typeof(HostGamepadButtons)))
            {
                if (flag != HostGamepadButtons.None && state.Buttons.HasFlag(flag))
                {
                    pressed.Add(flag.ToString());
                }
            }
            TesterButtonsText.Text = "Buttons: " + (pressed.Count > 0 ? string.Join(", ", pressed) : "-");

            TesterL2Bar.Value = state.LeftTrigger;
            TesterR2Bar.Value = state.RightTrigger;
            TesterL2Text.Text = state.LeftTrigger.ToString();
            TesterR2Text.Text = state.RightTrigger.ToString();
            TesterLeftStickText.Text = $"Left Stick:  X={state.LeftX} Y={state.LeftY}";
            TesterRightStickText.Text = $"Right Stick: X={state.RightX} Y={state.RightY}";
            TesterTouchText.Text = state.Buttons.HasFlag(HostGamepadButtons.TouchPad)
                ? "Touchpad: pressed"
                : "Touchpad: released";
        }

        private string LocateSndDecode()
        {
            string uiDir = AppDomain.CurrentDomain.BaseDirectory;
            string[] locations = {
                Path.Combine(uiDir, "pcsx5_snd_decode.exe"),
                Path.Combine(uiDir, "bin", "Release", "pcsx5_snd_decode.exe"),
                Path.Combine(uiDir, "bin", "Debug", "pcsx5_snd_decode.exe"),
                Path.Combine(uiDir, "..", "bin", "Release", "pcsx5_snd_decode.exe"),
                Path.Combine(uiDir, "..", "bin", "Debug", "pcsx5_snd_decode.exe"),
                Path.Combine(uiDir, "..", "bin", "pcsx5_snd_decode.exe"),
                Path.Combine(uiDir, "..", "Release", "pcsx5_snd_decode.exe"),
                Path.Combine(uiDir, "..", "Debug", "pcsx5_snd_decode.exe"),
                Path.Combine(uiDir, "..", "pcsx5_snd_decode.exe"),
                Path.Combine(uiDir, "build", "bin", "Release", "pcsx5_snd_decode.exe"),
                Path.Combine(uiDir, "build", "bin", "Debug", "pcsx5_snd_decode.exe"),
                Path.Combine(uiDir, "build", "Release", "pcsx5_snd_decode.exe"),
                Path.Combine(uiDir, "build", "Debug", "pcsx5_snd_decode.exe"),
                // Build directory at project root (5 levels up from uiDir: src/ui_csharp/bin/Release/net9.0-windows/)
                Path.Combine(uiDir, "..", "..", "..", "..", "..", "build", "pcsx5_snd_decode.exe"),
                Path.Combine(uiDir, "..", "..", "..", "..", "..", "build", "Release", "pcsx5_snd_decode.exe"),
                Path.Combine(uiDir, "..", "..", "..", "..", "..", "build", "Debug", "pcsx5_snd_decode.exe"),
                // Also check from project root directly
                Path.Combine(uiDir, "..", "..", "..", "..", "..", "..", "build", "pcsx5_snd_decode.exe"),
                Path.Combine(uiDir, "..", "..", "..", "..", "..", "..", "build", "Release", "pcsx5_snd_decode.exe"),
                Path.Combine(uiDir, "..", "..", "..", "..", "..", "..", "build", "Debug", "pcsx5_snd_decode.exe")
            };

            foreach (var loc in locations)
            {
                if (File.Exists(loc)) return Path.GetFullPath(loc);
            }

            return null;
        }

        private string LocateBootParser()
        {
            string uiDir = AppDomain.CurrentDomain.BaseDirectory;
            string[] locations = {
                Path.Combine(uiDir, "pcsx5_boot_parser.exe"),
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

        private void LogConsole(string message)
        {
            // Non-blocking: just enqueue; the drain timer appends batches on the UI thread.
            _consoleLineQueue.Enqueue(message);
        }

        private void DrainConsoleQueue()
        {
            if (_consoleLineQueue.IsEmpty) return;

            var sb = new StringBuilder();
            string line;
            int drained = 0;
            const int maxLinesPerDrain = 2000; // bounded work per tick; leftovers drain next tick
            while (drained < maxLinesPerDrain && _consoleLineQueue.TryDequeue(out line))
            {
                sb.AppendLine(line);
                drained++;
            }

            ConsoleOutputTextBox.AppendText(sb.ToString());

            // Cap the TextBox size: trim from the top when exceeded (checked per batch, not per line)
            int len = ConsoleOutputTextBox.Text.Length;
            if (len > MaxConsoleChars)
            {
                string text = ConsoleOutputTextBox.Text;
                int start = text.IndexOf('\n', len - MaxConsoleChars);
                ConsoleOutputTextBox.Text = start >= 0 ? text.Substring(start + 1) : text.Substring(len - MaxConsoleChars);
            }

            ConsoleOutputTextBox.ScrollToEnd();
        }

        private void CopyConsole_Click(object sender, RoutedEventArgs e)
        {
            if (!string.IsNullOrEmpty(ConsoleOutputTextBox.Text))
            {
                Clipboard.SetText(ConsoleOutputTextBox.Text);
            }
        }

        private void ConsoleOutputTextBox_TextChanged(object sender, TextChangedEventArgs e)
        {
            ConsoleOutputTextBox.ScrollToEnd();
        }

        private void ClearConsole_Click(object sender, RoutedEventArgs e)
        {
            // Drop pending queued lines too so cleared output doesn't reappear on next drain
            while (_consoleLineQueue.TryDequeue(out _)) { }
            ConsoleOutputTextBox.Clear();
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
            StopControllerVizPolling();
            LibraryView.Visibility = Visibility.Visible;
            AnalyzerView.Visibility = Visibility.Collapsed;
            ControllerView.Visibility = Visibility.Collapsed;
            SettingsView.Visibility = Visibility.Collapsed;
            LogsView.Visibility = Visibility.Collapsed;
            UpdateTabHighlight(TabLibraryBtn);
        }

        private void TabAnalyzer_Click(object sender, RoutedEventArgs e)
        {
            if (GameView.Visibility == Visibility.Visible) return; // a game is embedded
            StopControllerVizPolling();
            LibraryView.Visibility = Visibility.Collapsed;
            AnalyzerView.Visibility = Visibility.Visible;
            ControllerView.Visibility = Visibility.Collapsed;
            SettingsView.Visibility = Visibility.Collapsed;
            LogsView.Visibility = Visibility.Collapsed;
            UpdateTabHighlight(TabAnalyzerBtn);

            // Auto run analyzer on first opening if empty
            if (_analysisResults.Count == 0 && _games.Count > 0)
            {
                RunBootAnalyzer();
            }
        }

        private void TabController_Click(object sender, RoutedEventArgs e)
        {
            if (GameView.Visibility == Visibility.Visible) return; // a game is embedded
            LibraryView.Visibility = Visibility.Collapsed;
            AnalyzerView.Visibility = Visibility.Collapsed;
            ControllerView.Visibility = Visibility.Visible;
            SettingsView.Visibility = Visibility.Collapsed;
            LogsView.Visibility = Visibility.Collapsed;
            UpdateTabHighlight(TabControllerBtn);

            StartControllerVizPolling();
        }

        private void TabSettings_Click(object sender, RoutedEventArgs e)
        {
            if (GameView.Visibility == Visibility.Visible) return; // a game is embedded
            StopControllerVizPolling();
            LibraryView.Visibility = Visibility.Collapsed;
            AnalyzerView.Visibility = Visibility.Collapsed;
            ControllerView.Visibility = Visibility.Collapsed;
            SettingsView.Visibility = Visibility.Visible;
            LogsView.Visibility = Visibility.Collapsed;
            UpdateTabHighlight(TabSettingsBtn);
            UpdateSettingsUiFromConfig();
        }

        private void TabLogs_Click(object sender, RoutedEventArgs e)
        {
            if (GameView.Visibility == Visibility.Visible) return; // a game is embedded
            StopControllerVizPolling();
            LibraryView.Visibility = Visibility.Collapsed;
            AnalyzerView.Visibility = Visibility.Collapsed;
            ControllerView.Visibility = Visibility.Collapsed;
            SettingsView.Visibility = Visibility.Collapsed;
            LogsView.Visibility = Visibility.Visible;
            UpdateTabHighlight(TabLogsBtn);
        }

        private void UpdateTabHighlight(Button activeBtn)
        {
            var activeBrush = new SolidColorBrush(Color.FromRgb(0, 153, 255));
            var inactiveBrush = new SolidColorBrush(Color.FromRgb(160, 160, 165));

            TabLibraryBtn.Foreground = activeBtn == TabLibraryBtn ? Brushes.White : inactiveBrush;
            TabLibraryBtn.BorderBrush = activeBtn == TabLibraryBtn ? activeBrush : Brushes.Transparent;

            TabAnalyzerBtn.Foreground = activeBtn == TabAnalyzerBtn ? Brushes.White : inactiveBrush;
            TabAnalyzerBtn.BorderBrush = activeBtn == TabAnalyzerBtn ? activeBrush : Brushes.Transparent;

            TabControllerBtn.Foreground = activeBtn == TabControllerBtn ? Brushes.White : inactiveBrush;
            TabControllerBtn.BorderBrush = activeBtn == TabControllerBtn ? activeBrush : Brushes.Transparent;

            TabSettingsBtn.Foreground = activeBtn == TabSettingsBtn ? Brushes.White : inactiveBrush;
            TabSettingsBtn.BorderBrush = activeBtn == TabSettingsBtn ? activeBrush : Brushes.Transparent;

            TabLogsBtn.Foreground = activeBtn == TabLogsBtn ? Brushes.White : inactiveBrush;
            TabLogsBtn.BorderBrush = activeBtn == TabLogsBtn ? activeBrush : Brushes.Transparent;
        }


        private void TestVibration_Click(object sender, RoutedEventArgs e)
        {
            MessageBox.Show("Controller vibration test pulse sent successfully!", "Haptic Feedback Test", MessageBoxButton.OK, MessageBoxImage.Information);
        }

        private void CtrlColorSliders_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (CtrlColorBox != null && CtrlColorRSlider != null && CtrlColorGSlider != null && CtrlColorBSlider != null)
            {
                byte r = (byte)CtrlColorRSlider.Value;
                byte g = (byte)CtrlColorGSlider.Value;
                byte b = (byte)CtrlColorBSlider.Value;
                CtrlColorBox.Background = new SolidColorBrush(Color.FromRgb(r, g, b));
            }
        }

        private void BtnAddFolder_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new Microsoft.Win32.OpenFolderDialog();
            dialog.Title = "Select Game Folder";
            if (dialog.ShowDialog() == true)
            {
                string selectedPath = dialog.FolderName;
                if (!string.IsNullOrEmpty(selectedPath) && !_gameFolders.Contains(selectedPath))
                {
                    _gameFolders.Add(selectedPath);
                    RefreshGameFoldersList();
                    LoadGames(); // Re-scan games automatically
                    SaveConfig(); // Save configuration
                }
            }
        }

        private void BtnRemoveFolder_Click(object sender, RoutedEventArgs e)
        {
            if (GameFoldersListBox.SelectedItem != null)
            {
                string selected = GameFoldersListBox.SelectedItem.ToString();
                _gameFolders.Remove(selected);
                RefreshGameFoldersList();
                LoadGames(); // Re-scan games automatically
                SaveConfig(); // Save configuration
            }
        }

        private void RefreshGameFoldersList()
        {
            if (GameFoldersListBox != null)
            {
                GameFoldersListBox.Items.Clear();
                foreach (var folder in _gameFolders)
                {
                    GameFoldersListBox.Items.Add(folder);
                }
            }
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
            var dialog = new Microsoft.Win32.OpenFolderDialog();
            dialog.Title = "Select PS5 Games Folder";
            if (dialog.ShowDialog() == true && !string.IsNullOrEmpty(dialog.FolderName))
            {
                _firstRunSelectedFolder = dialog.FolderName;
                FirstRunFolderText.Text = _firstRunSelectedFolder;
                FirstRunContinueBtn.IsEnabled = true;
            }
        }

        private void FirstRunContinue_Click(object sender, RoutedEventArgs e)
        {
            if (string.IsNullOrEmpty(_firstRunSelectedFolder)) return;

            // Replace stale/non-existent defaults with the folder the user picked.
            _gameFolders.Clear();
            _gameFolders.Add(_firstRunSelectedFolder);
            FirstRunOverlay.Visibility = Visibility.Collapsed;
            RefreshGameFoldersList();
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
        private bool _suppressUiScaleEvent = false;

        private void ApplyUiScale()
        {
            double scale = _config.ui.scale;
            if (scale < 0.5 || scale > 3.0) scale = 1.0;
            MainLayoutRoot.LayoutTransform = new ScaleTransform(scale, scale);
        }

        private void UiScaleCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_suppressUiScaleEvent) return;
            if (UiScaleCombo.SelectedItem is ComboBoxItem item &&
                double.TryParse(item.Tag?.ToString(), System.Globalization.NumberStyles.Float,
                    System.Globalization.CultureInfo.InvariantCulture, out double scale))
            {
                _config.ui.scale = scale;
                ApplyUiScale();
            }
        }

        private void SelectUiScaleComboItem()
        {
            _suppressUiScaleEvent = true;
            try
            {
                foreach (ComboBoxItem item in UiScaleCombo.Items)
                {
                    if (double.TryParse(item.Tag?.ToString(), System.Globalization.NumberStyles.Float,
                            System.Globalization.CultureInfo.InvariantCulture, out double tag) &&
                        Math.Abs(tag - _config.ui.scale) < 0.001)
                    {
                        UiScaleCombo.SelectedItem = item;
                        return;
                    }
                }
                UiScaleCombo.SelectedIndex = 1; // 100%
            }
            finally
            {
                _suppressUiScaleEvent = false;
            }
        }

        // --- GAMEPAD NAV & PS BUTTON POLLING ---
        private System.Windows.Threading.DispatcherTimer _controllerTimer;
        private XInputState _prevInputState = new XInputState();
        private DateTime _lastAnalogNav = DateTime.MinValue;

        private void InitializeControllerPolling()
        {
            WindowsDualSenseReader.EnsureStarted();
            _controllerTimer = new System.Windows.Threading.DispatcherTimer();
            _controllerTimer.Interval = TimeSpan.FromMilliseconds(50); // poll at 20 FPS (every 50ms)
            _controllerTimer.Tick += ControllerTimer_Tick;
            _controllerTimer.Start();
        }

        // --- CONTROLLER SETUP LIVE INPUT VISUALIZATION ---
        // Polls the DualSense state at ~60 Hz while the Controller Setup tab is
        // visible and lights up the vector overlays on top of ps5_controller.png.
        private System.Windows.Threading.DispatcherTimer _controllerVizTimer;

        // Stick dot travel range (px in the 1017x1017 design space) around each well center
        private const double StickDotRange = 24.0;
        private const double LeftStickCenterX = 402.0;
        private const double LeftStickCenterY = 498.0;
        private const double RightStickCenterX = 641.0;
        private const double RightStickCenterY = 498.0;
        private const double StickDotSize = 36.0;

        private void StartControllerVizPolling()
        {
            WindowsDualSenseReader.EnsureStarted();
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
            ClearControllerVizOverlays();
            TesterStopAllOutputs();
        }

        private void ClearControllerVizOverlays()
        {
            foreach (var overlay in GetPadOverlays())
            {
                overlay.Opacity = 0;
            }
            PadTouchLeftOverlay.Opacity = 0;
            PadTouchCenterOverlay.Opacity = 0;
            PadTouchRightOverlay.Opacity = 0;
            MoveStickDot(LeftStickDot, LeftStickCenterX, LeftStickCenterY, 128, 128);
            MoveStickDot(RightStickDot, RightStickCenterX, RightStickCenterY, 128, 128);
        }

        private System.Collections.Generic.IEnumerable<Shape> GetPadOverlays()
        {
            yield return PadL1Overlay;
            yield return PadR1Overlay;
            yield return PadL3Overlay;
            yield return PadR3Overlay;
            yield return PadUpOverlay;
            yield return PadDownOverlay;
            yield return PadLeftOverlay;
            yield return PadRightOverlay;
            yield return PadCrossOverlay;
            yield return PadCircleOverlay;
            yield return PadSquareOverlay;
            yield return PadTriangleOverlay;
            yield return PadOptionsOverlay;
            yield return PadBackOverlay;
        }

        private void ControllerVizTimer_Tick(object sender, EventArgs e)
        {
            if (!WindowsDualSenseReader.TryGetState(out var state) || !state.Connected)
            {
                ClearControllerVizOverlays();
                UpdateTesterReadouts(default);
                return;
            }

            var b = state.Buttons;
            PadUpOverlay.Opacity = b.HasFlag(HostGamepadButtons.Up) ? 1 : 0;
            PadDownOverlay.Opacity = b.HasFlag(HostGamepadButtons.Down) ? 1 : 0;
            PadLeftOverlay.Opacity = b.HasFlag(HostGamepadButtons.Left) ? 1 : 0;
            PadRightOverlay.Opacity = b.HasFlag(HostGamepadButtons.Right) ? 1 : 0;
            PadCrossOverlay.Opacity = b.HasFlag(HostGamepadButtons.Cross) ? 1 : 0;
            PadCircleOverlay.Opacity = b.HasFlag(HostGamepadButtons.Circle) ? 1 : 0;
            PadSquareOverlay.Opacity = b.HasFlag(HostGamepadButtons.Square) ? 1 : 0;
            PadTriangleOverlay.Opacity = b.HasFlag(HostGamepadButtons.Triangle) ? 1 : 0;
            PadL1Overlay.Opacity = b.HasFlag(HostGamepadButtons.L1) ? 1 : 0;
            PadR1Overlay.Opacity = b.HasFlag(HostGamepadButtons.R1) ? 1 : 0;
            PadL3Overlay.Opacity = b.HasFlag(HostGamepadButtons.L3) ? 1 : 0;
            PadR3Overlay.Opacity = b.HasFlag(HostGamepadButtons.R3) ? 1 : 0;
            PadOptionsOverlay.Opacity = b.HasFlag(HostGamepadButtons.Options) ? 1 : 0;
            PadBackOverlay.Opacity = b.HasFlag(HostGamepadButtons.Back) ? 1 : 0;

            // Triggers light up proportionally to their analog value
            PadL2Overlay.Opacity = Math.Max(state.LeftTrigger / 255.0, b.HasFlag(HostGamepadButtons.L2) ? 1 : 0);
            PadR2Overlay.Opacity = Math.Max(state.RightTrigger / 255.0, b.HasFlag(HostGamepadButtons.R2) ? 1 : 0);

            // The DualSense report has a single touchpad click flag; light the whole pad.
            double touchOpacity = b.HasFlag(HostGamepadButtons.TouchPad) ? 1 : 0;
            PadTouchLeftOverlay.Opacity = touchOpacity;
            PadTouchCenterOverlay.Opacity = touchOpacity;
            PadTouchRightOverlay.Opacity = touchOpacity;

            MoveStickDot(LeftStickDot, LeftStickCenterX, LeftStickCenterY, state.LeftX, state.LeftY);
            MoveStickDot(RightStickDot, RightStickCenterX, RightStickCenterY, state.RightX, state.RightY);

            UpdateTesterReadouts(state);
        }

        private void MoveStickDot(Ellipse dot, double centerX, double centerY, byte axisX, byte axisY)
        {
            double offsetX = ((axisX - 128) / 128.0) * StickDotRange;
            double offsetY = ((axisY - 128) / 128.0) * StickDotRange;
            Canvas.SetLeft(dot, centerX - StickDotSize / 2 + offsetX);
            Canvas.SetTop(dot, centerY - StickDotSize / 2 + offsetY);
        }


        private void ControllerTimer_Tick(object sender, EventArgs e)
        {
            XInputState state = new XInputState();
            ushort buttons = 0;
            short lx = 0;
            short ly = 0;

            bool hasDualSense = WindowsDualSenseReader.TryGetState(out var dsState);
            if (hasDualSense)
            {
                if ((dsState.Buttons & HostGamepadButtons.Up) != 0) buttons |= 0x0001;
                if ((dsState.Buttons & HostGamepadButtons.Down) != 0) buttons |= 0x0002;
                if ((dsState.Buttons & HostGamepadButtons.Left) != 0) buttons |= 0x0004;
                if ((dsState.Buttons & HostGamepadButtons.Right) != 0) buttons |= 0x0008;
                if ((dsState.Buttons & HostGamepadButtons.Cross) != 0) buttons |= 0x1000;
                if ((dsState.Buttons & HostGamepadButtons.Circle) != 0) buttons |= 0x2000;
                if ((dsState.Buttons & HostGamepadButtons.L1) != 0) buttons |= 0x0100;
                if ((dsState.Buttons & HostGamepadButtons.R1) != 0) buttons |= 0x0200;
                if ((dsState.Buttons & HostGamepadButtons.Options) != 0 || (dsState.Buttons & HostGamepadButtons.TouchPad) != 0) buttons |= 0x0400;

                lx = (short)((dsState.LeftX - 128) * 256);
                ly = (short)(-(dsState.LeftY - 128) * 256);
                state.Gamepad.wButtons = buttons;
                state.Gamepad.sThumbLX = lx;
                state.Gamepad.sThumbLY = ly;
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
                    _prevInputState = new XInputState();
                    return; // No controller connected
                }

                buttons = state.Gamepad.wButtons;
                lx = state.Gamepad.sThumbLX;
                ly = state.Gamepad.sThumbLY;
            }

            ushort prevButtons = _prevInputState.Gamepad.wButtons;

            // Detect Button Press transitions (pressed now, but was not pressed in previous state)
            bool upPressed = ((buttons & 0x0001) != 0) && ((prevButtons & 0x0001) == 0);
            bool downPressed = ((buttons & 0x0002) != 0) && ((prevButtons & 0x0002) == 0);
            bool leftPressed = ((buttons & 0x0004) != 0) && ((prevButtons & 0x0004) == 0);
            bool rightPressed = ((buttons & 0x0008) != 0) && ((prevButtons & 0x0008) == 0);

            bool aPressed = ((buttons & 0x1000) != 0) && ((prevButtons & 0x1000) == 0); // Cross/A
            bool bPressed = ((buttons & 0x2000) != 0) && ((prevButtons & 0x2000) == 0); // Circle/B
            bool psPressed = ((buttons & 0x0400) != 0) && ((prevButtons & 0x0400) == 0); // PS Button / Guide
            bool l1Pressed = ((buttons & 0x0100) != 0) && ((prevButtons & 0x0100) == 0); // L1 / Tab Left
            bool r1Pressed = ((buttons & 0x0200) != 0) && ((prevButtons & 0x0200) == 0); // R1 / Tab Right

            // Map Left Thumbstick and D-Pad with a cooldown to support smooth continuous scrolling when held
            const int stickThreshold = 15000;

            if ((DateTime.Now - _lastAnalogNav).TotalMilliseconds > 250)
            {
                if (lx < -stickThreshold || (buttons & 0x0004) != 0) { leftPressed = true; _lastAnalogNav = DateTime.Now; }
                else if (lx > stickThreshold || (buttons & 0x0008) != 0) { rightPressed = true; _lastAnalogNav = DateTime.Now; }
                else if (ly > stickThreshold || (buttons & 0x0001) != 0) { upPressed = true; _lastAnalogNav = DateTime.Now; }
                else if (ly < -stickThreshold || (buttons & 0x0002) != 0) { downPressed = true; _lastAnalogNav = DateTime.Now; }
            }

            // PS Button Action (Home Button)
            if (psPressed)
            {
                // If game is running, ask the in-proc core to stop the guest
                if (_coreRunning)
                {
                    try
                    {
                        CoreBridge.pcsx5_stop();
                        LogConsole("PS Button: Stop requested for running game.");
                    }
                    catch { }
                }
                else
                {
                    // If no game is running, bring the UI window to focus
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

            // If game is running, do not navigate UI (controller input is passed to the game backend)
            if (_coreRunning)
            {
                _prevInputState = state;
                return;
            }

            // Shoulder buttons tab cycling
            if (l1Pressed || r1Pressed)
            {
                Dispatcher.Invoke(() =>
                {
                    Button[] tabs = { TabLibraryBtn, TabAnalyzerBtn, TabControllerBtn, TabSettingsBtn, TabLogsBtn };
                    int activeIndexTab = 0;
                    if (LibraryView.Visibility == Visibility.Visible) activeIndexTab = 0;
                    else if (AnalyzerView.Visibility == Visibility.Visible) activeIndexTab = 1;
                    else if (ControllerView.Visibility == Visibility.Visible) activeIndexTab = 2;
                    else if (SettingsView.Visibility == Visibility.Visible) activeIndexTab = 3;
                    else if (LogsView.Visibility == Visibility.Visible) activeIndexTab = 4;

                    int nextIndex = activeIndexTab;
                    if (l1Pressed) nextIndex = (activeIndexTab - 1 + tabs.Length) % tabs.Length;
                    else if (r1Pressed) nextIndex = (activeIndexTab + 1) % tabs.Length;

                    switch (nextIndex)
                    {
                        case 0: TabLibrary_Click(this, null); break;
                        case 1: TabAnalyzer_Click(this, null); break;
                        case 2: TabController_Click(this, null); break;
                        case 3: TabSettings_Click(this, null); break;
                        case 4: TabLogs_Click(this, null); break;
                    }
                });
            }

            // Navigate the UI if in Game Library
            if (LibraryView.Visibility == Visibility.Visible && _games.Count > 0)
            {
                if (leftPressed || rightPressed)
                {
                    int currentIndex = _selectedGame != null ? _games.IndexOf(_selectedGame) : -1;
                    if (currentIndex != -1)
                    {
                        int newIndex = currentIndex;
                        if (leftPressed)
                        {
                            newIndex = (currentIndex - 1 + _games.Count) % _games.Count;
                        }
                        else if (rightPressed)
                        {
                            newIndex = (currentIndex + 1) % _games.Count;
                        }

                        Dispatcher.Invoke(() =>
                        {
                            SelectGame(_games[newIndex]);
                            // Scroll to selected game item
                            foreach (Border card in GamesWrapPanel.Children)
                            {
                                if (card.Tag == _games[newIndex])
                                {
                                    card.BringIntoView();
                                    break;
                                }
                            }
                        });
                    }
                }

                if (aPressed)
                {
                    Dispatcher.Invoke(() =>
                    {
                        if (LaunchButton.IsEnabled && _selectedGame != null)
                        {
                            LaunchButton_Click(this, null);
                        }
                    });
                }

                if (bPressed)
                {
                    Dispatcher.Invoke(() =>
                    {
                        if (StopButton.Visibility == Visibility.Visible)
                        {
                            StopButton_Click(this, null);
                        }
                    });
                }
            }

            _prevInputState = state;
        }

        // Real-Time Settings Slider Text updates
        private void GpuResScaleSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (GpuResScaleText != null)
            {
                GpuResScaleText.Text = $"{e.NewValue:F2}x";
            }
        }

        private void SndVolumeSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (SndVolumeText != null)
            {
                SndVolumeText.Text = $"{(int)(e.NewValue * 100)}%";
            }
        }

        private void SndBufferSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (SndBufferText != null)
            {
                SndBufferText.Text = $"{(int)e.NewValue} ms";
            }
        }

        private void InDeadzoneSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (InDeadzoneText != null)
            {
                InDeadzoneText.Text = $"{(int)(e.NewValue * 100)}%";
            }
        }

        private void HleTraceCapSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (HleTraceCapText != null)
            {
                HleTraceCapText.Text = ((int)e.NewValue).ToString();
            }
        }

        private void SndTitleMusicCheck_Click(object sender, RoutedEventArgs e)
        {
            if (SndTitleMusicCheck.IsChecked == false)
            {
                _mediaPlayer.Stop();
            }
            else if (_selectedGame != null)
            {
                TriggerTitleMusic(_selectedGame);
            }
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
                TabLogsBtn.Content = I18n.Tr("sidebar.console");

                // Game library shelf header
                if (LibraryHeaderTextBlock != null)
                    LibraryHeaderTextBlock.Text = I18n.Tr("library.header");

                // Search placeholder
                if (SearchPlaceholder != null)
                    SearchPlaceholder.Text = I18n.Tr("library.search_hint");

                // Logs header
                if (LogsHeaderTextBlock != null)
                    LogsHeaderTextBlock.Text = I18n.Tr("console.title");

                // Language / localization settings section keeps the sidebar.language label
                if (UiLocalizationHeaderTextBlock != null)
                    UiLocalizationHeaderTextBlock.Text = I18n.Tr("sidebar.language").ToUpperInvariant();

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
                    DetailSize.Text = $"{I18n.Tr("library.size_label")} {FormatBytes(_selectedGame.SizeBytes)}";
                }
            }
            catch (Exception ex)
            {
                LogConsole("UI translation error: " + ex.Message);
            }
        }

        private void DismissCrashDialog_Click(object sender, RoutedEventArgs e)
        {
            CrashDialogOverlay.Visibility = Visibility.Collapsed;
            try
            {
                string crashLogPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "crash_log.txt");
                if (File.Exists(crashLogPath)) File.Delete(crashLogPath);
            }
            catch { }
        }

        private void AnalyzeCrashedGame_Click(object sender, RoutedEventArgs e)
        {
            CrashDialogOverlay.Visibility = Visibility.Collapsed;
            try
            {
                string crashLogPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "crash_log.txt");
                if (File.Exists(crashLogPath)) File.Delete(crashLogPath);
            }
            catch { }

            TabAnalyzer_Click(this, null);
            if (_selectedGame != null)
            {
                RunSingleGameAnalysis(_selectedGame);
            }
        }

        private async void RunSingleGameAnalysis(GameEntry game)
        {
            string parserPath = LocateBootParser();
            if (parserPath == null || !File.Exists(parserPath))
            {
                MessageBox.Show("Could not locate pcsx5_boot_parser.exe binary. Please compile the project first.", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                return;
            }

            _analysisResults.Clear();
            AnalyzerListView.ItemsSource = null;
            AnalyzerOutputTextBox.Text = $"Running analysis on {game.Title}...";

            string rawOutput = "";
            try
            {
                rawOutput = await RunParserProcessAsync(parserPath, game.EbootPath);
            }
            catch (Exception ex)
            {
                rawOutput = "Execution Error: " + ex.Message;
            }

            var res = ParseParserOutput(rawOutput, game);
            _analysisResults.Add(res);
            AnalyzerListView.ItemsSource = _analysisResults;
            AnalyzerListView.SelectedIndex = 0;
        }

        private void UpdateSettingsUiFromConfig()
        {
            try
            {
                // Directories
                RefreshGameFoldersList();

                // UI Language
                string lang = _config.ui.language ?? "en-US";
                foreach (ComboBoxItem item in UiLanguageCombo.Items)
                {
                    if (item.Tag?.ToString() == lang)
                    {
                        UiLanguageCombo.SelectedItem = item;
                        break;
                    }
                }

                // UI Scale
                SelectUiScaleComboItem();

                // Graphics
                GpuRendererCombo.SelectedIndex = Math.Clamp(_config.graphics.renderer, 0, 1);
                GpuFullscreenCheck.IsChecked = _config.graphics.fullscreen;
                GpuResScaleSlider.Value = Math.Clamp(_config.graphics.resolution_scale, 0.5, 2.0);
                GpuResScaleText.Text = $"{_config.graphics.resolution_scale:F2}x";
                GpuWidthText.Text = _config.graphics.width.ToString();
                GpuHeightText.Text = _config.graphics.height.ToString();

                // Audio
                SndBackendCombo.SelectedIndex = Math.Clamp(_config.audio.backend, 0, 2);
                SndVolumeSlider.Value = Math.Clamp(_config.audio.volume, 0.0, 1.0);
                SndVolumeText.Text = $"{(int)(_config.audio.volume * 100)}%";
                SndBufferSlider.Value = Math.Clamp(_config.audio.buffer_ms, 10, 200);
                SndBufferText.Text = $"{_config.audio.buffer_ms} ms";
                SndTitleMusicCheck.IsChecked = _config.ui.title_music_enabled;

                // Input
                InBackendCombo.SelectedIndex = Math.Clamp(_config.input.backend, 0, 1);
                InDeadzoneSlider.Value = Math.Clamp(_config.input.deadzone, 0.0, 0.5);
                InDeadzoneText.Text = $"{(int)(_config.input.deadzone * 100)}%";
                InRumbleCheck.IsChecked = _config.input.rumble;

                // HLE
                HleStrictCheck.IsChecked = _config.hle.strict_imports;
                HleTraceCheck.IsChecked = _config.hle.trace_calls;
                HleTraceCapSlider.Value = Math.Clamp(_config.hle.trace_capacity, 64, 1024);
                HleTraceCapText.Text = _config.hle.trace_capacity.ToString();
                HleWriteDumpCheck.IsChecked = _config.crash.write_minidump;

                // Logging
                int minLevelIdx = 2; // Default to Info
                string level = _config.logging.min_level?.ToLower() ?? "info";
                switch (level)
                {
                    case "trace": minLevelIdx = 0; break;
                    case "debug": minLevelIdx = 1; break;
                    case "info": minLevelIdx = 2; break;
                    case "warning":
                    case "warn": minLevelIdx = 3; break;
                    case "error": minLevelIdx = 4; break;
                    case "fatal": minLevelIdx = 5; break;
                }
                LogMinLevelCombo.SelectedIndex = minLevelIdx;
                LogFileAppendCheck.IsChecked = _config.logging.file_append;
                LogJsonCheck.IsChecked = _config.logging.json_output;
            }
            catch (Exception ex)
            {
                LogConsole("Error updating settings UI: " + ex.Message);
            }
        }

        private void SaveConfigFromUi()
        {
            try
            {
                // UI Language
                if (UiLanguageCombo.SelectedItem is ComboBoxItem selectedLang)
                {
                    string selectedTag = selectedLang.Tag?.ToString() ?? "en-US";
                    if (_config.ui.language != selectedTag)
                    {
                        _config.ui.language = selectedTag;
                        TranslateUi();
                    }
                }

                // Graphics
                _config.graphics.renderer = GpuRendererCombo.SelectedIndex;
                _config.graphics.fullscreen = GpuFullscreenCheck.IsChecked ?? false;
                _config.graphics.resolution_scale = GpuResScaleSlider.Value;
                if (int.TryParse(GpuWidthText.Text, out int w)) _config.graphics.width = w;
                if (int.TryParse(GpuHeightText.Text, out int h)) _config.graphics.height = h;

                // Audio
                _config.audio.backend = SndBackendCombo.SelectedIndex;
                _config.audio.volume = SndVolumeSlider.Value;
                _config.audio.buffer_ms = (int)SndBufferSlider.Value;
                _config.ui.title_music_enabled = SndTitleMusicCheck.IsChecked ?? true;

                // Input
                _config.input.backend = InBackendCombo.SelectedIndex;
                _config.input.deadzone = InDeadzoneSlider.Value;
                _config.input.rumble = InRumbleCheck.IsChecked ?? true;

                // HLE
                _config.hle.strict_imports = HleStrictCheck.IsChecked ?? false;
                _config.hle.trace_calls = HleTraceCheck.IsChecked ?? false;
                _config.hle.trace_capacity = (int)HleTraceCapSlider.Value;
                _config.crash.write_minidump = HleWriteDumpCheck.IsChecked ?? true;

                // Logging
                string[] levels = { "Trace", "Debug", "Info", "Warning", "Error", "Fatal" };
                if (LogMinLevelCombo.SelectedIndex >= 0 && LogMinLevelCombo.SelectedIndex < levels.Length)
                {
                    _config.logging.min_level = levels[LogMinLevelCombo.SelectedIndex];
                }
                _config.logging.file_append = LogFileAppendCheck.IsChecked ?? false;
                _config.logging.json_output = LogJsonCheck.IsChecked ?? false;

                SaveConfig();
            }
            catch (Exception ex)
            {
                LogConsole("Error saving settings from UI: " + ex.Message);
            }
        }

        private void SaveSettingsBtn_Click(object sender, RoutedEventArgs e)
        {
            SaveConfigFromUi();
        }

        // Executable Boot & Memory Analyzer UI actions
        private void AnalyzeAll_Click(object sender, RoutedEventArgs e)
        {
            RunBootAnalyzer();
        }

        private async void RunBootAnalyzer()
        {
            string parserPath = LocateBootParser();
            if (parserPath == null || !File.Exists(parserPath))
            {
                MessageBox.Show("Could not locate pcsx5_boot_parser.exe binary. Please compile the project first.", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                return;
            }

            _analysisResults.Clear();
            AnalyzerListView.ItemsSource = null;
            AnalyzerOutputTextBox.Text = "Running analysis on all games...";

            var gamesToAnalyze = _games.ToList();
            var results = new List<BootAnalysisResult>();

            foreach (var game in gamesToAnalyze)
            {
                if (string.IsNullOrEmpty(game.EbootPath) || !File.Exists(game.EbootPath))
                    continue;

                string rawOutput = "";
                try
                {
                    rawOutput = await RunParserProcessAsync(parserPath, game.EbootPath);
                }
                catch (Exception ex)
                {
                    rawOutput = "Execution Error: " + ex.Message;
                }

                var res = ParseParserOutput(rawOutput, game);
                results.Add(res);
            }

            _analysisResults = results;
            AnalyzerListView.ItemsSource = _analysisResults;

            if (_analysisResults.Count > 0)
            {
                AnalyzerListView.SelectedIndex = 0;
            }
            else
            {
                AnalyzerOutputTextBox.Text = "No executables parsed.";
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

                process.OutputDataReceived += (s, e) => { if (e.Data != null) sb.AppendLine(e.Data); };
                process.EnableRaisingEvents = true;
                process.Exited += (s, e) => tcs.SetResult(sb.ToString());

                process.Start();
                process.BeginOutputReadLine();

                return await tcs.Task;
            }
        }

        private BootAnalysisResult ParseParserOutput(string raw, GameEntry game)
        {
            var res = new BootAnalysisResult
            {
                Title = game.Title,
                TitleId = game.TitleId,
                EbootPath = game.EbootPath,
                RawOutput = raw,
                Format = "Unknown",
                EncryptionStatus = "Decrypted",
                MemoryFootprint = "-",
                AlignmentStatus = "OK"
            };

            using (var reader = new StringReader(raw))
            {
                string line;
                while ((line = reader.ReadLine()) != null)
                {
                    if (line.Contains("Format detected:"))
                    {
                        res.Format = line.Substring(line.IndexOf("Format detected:") + "Format detected:".Length).Trim();
                    }
                    else if (line.Contains("STATUS: PLAINTEXT / DECRYPTED (Key Applied)"))
                    {
                        res.EncryptionStatus = "Decrypted 🔑";
                    }
                    else if (line.Contains("STATUS: ENCRYPTED SEGMENTS DETECTED"))
                    {
                        res.EncryptionStatus = "Locked 🔒";
                    }
                    else if (line.Contains("STATUS: PLAINTEXT / DECRYPTED"))
                    {
                        res.EncryptionStatus = "Decrypted";
                    }
                    else if (line.Contains("BOOT ERROR:"))
                    {
                        string err = line.Substring(line.IndexOf("BOOT ERROR:") + "BOOT ERROR:".Length).Trim();
                        res.AlignmentStatus = "Error: " + err;
                    }
                    else if (line.Contains("WARNING: Segment vaddr is not 16KB page-aligned!"))
                    {
                        if (!res.AlignmentStatus.StartsWith("Error"))
                        {
                            res.AlignmentStatus = "Warning ⚠️";
                        }
                    }
                    else if (line.Contains("Memory footprint:"))
                    {
                        int idx = line.IndexOf("Footprint size:");
                        if (idx != -1)
                        {
                            res.MemoryFootprint = line.Substring(idx + "Footprint size:".Length).Replace(")", "").Trim();
                        }
                    }
                }
            }

            return res;
        }

        private void AnalyzerListView_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            var selected = AnalyzerListView.SelectedItem as BootAnalysisResult;
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
            if (_analysisResults.Count == 0)
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
                    var payload = _analysisResults.Select(r => new
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
    }

    public class BootAnalysisResult
    {
        public string Title { get; set; }
        public string TitleId { get; set; }
        public string Format { get; set; }
        public string EncryptionStatus { get; set; }
        public string MemoryFootprint { get; set; }
        public string AlignmentStatus { get; set; }
        public string RawOutput { get; set; }
        public string EbootPath { get; set; }
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
            public double scale { get; set; } = 1.0;
        }
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
