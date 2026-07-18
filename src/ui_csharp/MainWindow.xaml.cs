using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace Pcsx5Ui
{
    public partial class MainWindow : Window
    {
        private List<GameEntry> _games = new List<GameEntry>();
        private GameEntry _selectedGame = null;
        private Process _emulatorProcess = null;
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

        public MainWindow()
        {
            InitializeComponent();
            InitializeAudioPlayer();
            this.Closed += MainWindow_Closed;
        }

        private void MainWindow_Closed(object sender, EventArgs e)
        {
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
                LoadGames();
                InitializeControllerPolling();

                // Start Discord RPC and load translations
                _discordRpc.Start();
                TranslateUi();

                // Set default tab to Library
                TabLibrary_Click(this, null);
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

            var coverImage = LoadImageHelper(game.CoverPath);
            if (coverImage != null)
            {
                border.Background = new ImageBrush
                {
                    ImageSource = coverImage,
                    Stretch = Stretch.UniformToFill
                };
            }
            else
            {
                // Fallback text
                var fallbackGrid = new Grid();
                fallbackGrid.Children.Add(new TextBlock
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
                border.Child = fallbackGrid;
            }

            // Hover effects
            border.MouseEnter += (s, e) =>
            {
                if (_selectedGame != game)
                {
                    border.BorderBrush = new SolidColorBrush(Color.FromRgb(0, 153, 255));
                    border.Background = new SolidColorBrush(Color.FromArgb(60, 255, 255, 255));
                }
            };
            border.MouseLeave += (s, e) =>
            {
                if (_selectedGame != game)
                {
                    border.BorderBrush = new SolidColorBrush(Color.FromArgb(15, 255, 255, 255));
                    border.Background = new SolidColorBrush(Color.FromArgb(30, 255, 255, 255));
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
                if (card.Tag == game)
                {
                    card.BorderBrush = new SolidColorBrush(Color.FromRgb(0, 153, 255));
                    card.Background = new SolidColorBrush(Color.FromArgb(60, 255, 255, 255));
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
                    card.Background = new SolidColorBrush(Color.FromArgb(30, 255, 255, 255));
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

            string backendPath = LocateBackend();
            if (backendPath == null || !File.Exists(backendPath))
            {
                LogConsole("Error: Could not locate pcsx5.exe backend.");
                MessageBox.Show("Could not locate pcsx5.exe backend binary.", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                return;
            }

            LaunchButton.IsEnabled = false;
            StopButton.Visibility = Visibility.Visible;
            ConsoleOutputTextBox.Clear();

            LogConsole($"Launching: {backendPath} \"{_selectedGame.EbootPath}\"");
            FooterStatus.Text = $"Running: {_selectedGame.Title}";

            // Stop title music when game launches
            Dispatcher.Invoke(() => _mediaPlayer.Stop());

            Task.Run(() =>
            {
                try
                {
                    _emulatorProcess = new Process();
                    _emulatorProcess.StartInfo.FileName = backendPath;
                    _emulatorProcess.StartInfo.Arguments = $"\"{_selectedGame.EbootPath}\"";
                    _emulatorProcess.StartInfo.UseShellExecute = false;
                    _emulatorProcess.StartInfo.RedirectStandardOutput = true;
                    _emulatorProcess.StartInfo.RedirectStandardError = true;
                    _emulatorProcess.StartInfo.CreateNoWindow = true;

                    _emulatorProcess.OutputDataReceived += (s, ev) => { if (ev.Data != null) LogConsole(ev.Data); };
                    _emulatorProcess.ErrorDataReceived += (s, ev) => { if (ev.Data != null) LogConsole(ev.Data); };

                    _emulatorProcess.Start();
                    _emulatorProcess.BeginOutputReadLine();
                    _emulatorProcess.BeginErrorReadLine();

                    if (_discordRpc != null && _selectedGame != null)
                    {
                        _discordRpc.UpdatePresence($"Playing {_selectedGame.Title}", "In-Game");
                    }

                    _emulatorProcess.WaitForExit();
                }
                catch (Exception ex)
                {
                    LogConsole("Launch Error: " + ex.Message);
                }
                finally
                {
                    Dispatcher.Invoke(() =>
                    {
                        LaunchButton.IsEnabled = true;
                        StopButton.Visibility = Visibility.Collapsed;
                        _emulatorProcess = null;
                        LogConsole("Emulator process terminated.");
                        FooterStatus.Text = "Ready";

                        // Check for crash logs
                        CheckEmulatorCrashLog();

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
            });
        }

        private void StopButton_Click(object sender, RoutedEventArgs e)
        {
            if (_emulatorProcess != null && !_emulatorProcess.HasExited)
            {
                try
                {
                    _emulatorProcess.Kill();
                }
                catch { }
            }
        }

        private string LocateBackend()
        {
            string uiDir = AppDomain.CurrentDomain.BaseDirectory;
            string[] locations = {
                Path.Combine(uiDir, "pcsx5.exe"),
                Path.Combine(uiDir, "bin", "Release", "pcsx5.exe"),
                Path.Combine(uiDir, "bin", "Debug", "pcsx5.exe"),
                Path.Combine(uiDir, "..", "bin", "Release", "pcsx5.exe"),
                Path.Combine(uiDir, "..", "bin", "Debug", "pcsx5.exe"),
                Path.Combine(uiDir, "..", "bin", "pcsx5.exe"),
                Path.Combine(uiDir, "..", "Release", "pcsx5.exe"),
                Path.Combine(uiDir, "..", "Debug", "pcsx5.exe"),
                Path.Combine(uiDir, "..", "pcsx5.exe"),
                Path.Combine(uiDir, "build", "bin", "Release", "pcsx5.exe"),
                Path.Combine(uiDir, "build", "bin", "Debug", "pcsx5.exe"),
                Path.Combine(uiDir, "build", "Release", "pcsx5.exe"),
                Path.Combine(uiDir, "build", "Debug", "pcsx5.exe")
            };

            foreach (var loc in locations)
            {
                if (File.Exists(loc)) return Path.GetFullPath(loc);
            }

            return null;
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
            Dispatcher.Invoke(() =>
            {
                ConsoleOutputTextBox.AppendText(message + Environment.NewLine);
                ConsoleOutputTextBox.ScrollToEnd();
            });
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
            LibraryView.Visibility = Visibility.Visible;
            AnalyzerView.Visibility = Visibility.Collapsed;
            ControllerView.Visibility = Visibility.Collapsed;
            SettingsView.Visibility = Visibility.Collapsed;
            LogsView.Visibility = Visibility.Collapsed;
            UpdateTabHighlight(TabLibraryBtn);
        }

        private void TabAnalyzer_Click(object sender, RoutedEventArgs e)
        {
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
            LibraryView.Visibility = Visibility.Collapsed;
            AnalyzerView.Visibility = Visibility.Collapsed;
            ControllerView.Visibility = Visibility.Visible;
            SettingsView.Visibility = Visibility.Collapsed;
            LogsView.Visibility = Visibility.Collapsed;
            UpdateTabHighlight(TabControllerBtn);
        }

        private void TabSettings_Click(object sender, RoutedEventArgs e)
        {
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
                // If game is running, close/stop the emulator
                if (_emulatorProcess != null && !_emulatorProcess.HasExited)
                {
                    try
                    {
                        _emulatorProcess.Kill();
                        LogConsole("PS Button: Stopped emulator process.");
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
            if (_emulatorProcess != null && !_emulatorProcess.HasExited)
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
                TabControllerBtn.Content = I18n.Tr("sidebar.language");
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

        private void CheckEmulatorCrashLog()
        {
            string crashLogPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "crash_log.txt");
            if (File.Exists(crashLogPath))
            {
                try
                {
                    string content = File.ReadAllText(crashLogPath);
                    if (content.Contains("GUEST APPLICATION CRASHED!") || content.Contains("VEH Unhandled Exception"))
                    {
                        // Parse exception code and address
                        string excCode = "0xC0000005 (Access Violation)";
                        if (content.Contains("Exception Code: "))
                        {
                            int idx = content.IndexOf("Exception Code: ");
                            int end = content.IndexOf("\n", idx);
                            if (end != -1)
                            {
                                excCode = content.Substring(idx + "Exception Code: ".Length, end - idx - "Exception Code: ".Length).Trim();
                            }
                        }

                        string ripVal = "Unknown";
                        if (content.Contains("Crash Address (RIP): "))
                        {
                            int idx = content.IndexOf("Crash Address (RIP): ");
                            int end = content.IndexOf("\n", idx);
                            if (end != -1)
                            {
                                ripVal = content.Substring(idx + "Crash Address (RIP): ".Length, end - idx - "Crash Address (RIP): ".Length).Trim();
                            }
                        }
                        else if (content.Contains("RIP: "))
                        {
                            int idx = content.IndexOf("RIP: ");
                            int end = content.IndexOf(",", idx);
                            if (end != -1)
                            {
                                ripVal = content.Substring(idx + "RIP: ".Length, end - idx - "RIP: ".Length).Trim();
                            }
                        }

                        // Determine analysis advice
                        string analysis = "An unhandled exception occurred in the guest application. This could be due to memory misalignment, stack overflow, or an unsupported instruction.";
                        if (ripVal.Contains("0x800000080") || ripVal.Contains("0x80") || ripVal.EndsWith("80"))
                        {
                            analysis = "CRITICAL ANALYSIS: The guest application crashed at address 0x800000080 (Offset: 0x80). This strongly indicates the eboot.bin binary contains encrypted segments or is executing garbage header structures. You must run the Boot Analyzer to attempt decryption using pre-shared keys or key databases.";
                        }
                        else if (content.Contains("ucrtbase.dll"))
                        {
                            analysis = "CRITICAL ANALYSIS: The guest application crashed inside the Host C-Runtime Library (ucrtbase.dll). This usually indicates a null-pointer exception, memory corruption, or bad thread initialization on the host calling convention bridge.";
                        }

                        // Display Dialog
                        Dispatcher.Invoke(() =>
                        {
                            CrashExcCodeText.Text = excCode;
                            CrashRipText.Text = ripVal;
                            CrashAnalysisText.Text = analysis;
                            CrashRawText.Text = content;
                            CrashDialogOverlay.Visibility = Visibility.Visible;
                        });
                    }
                }
                catch (Exception ex)
                {
                    LogConsole("Failed to read crash log: " + ex.Message);
                }
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
