using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Threading;

namespace Pcsx5Ui
{
    // ── Game session lifecycle ─────────────────────────────────────────────────
    //
    // All interaction with pcsx5_core.dll happens on the dedicated _gameThread.
    // The WPF Dispatcher thread is NEVER blocked by game execution.
    //
    // State machine:
    //   Idle → Extracting (PKG only) → Booting → Running → Stopped / Crashed
    //
    // Events fire on the WPF Dispatcher (BeginInvoke) so subscribers can touch
    // UI elements directly without cross-thread marshalling.
    // ──────────────────────────────────────────────────────────────────────────

    public enum GameSessionState
    {
        Idle,
        Extracting,   // PKG extraction in progress
        Booting,      // pcsx5_init + pcsx5_load
        Running,      // pcsx5_run returned (guest CPU executing)
        Stopped,      // Clean exit / user-requested stop
        Crashed       // Abnormal exit (non-zero code or exception)
    }

    public enum BootPhase
    {
        Initializing,
        ExtractingPkg,
        LoadingElf,
        LinkingModules,
        StartingCpu,
        Running
    }

    public sealed class GameSession
    {
        // ── Events (always raised on the WPF Dispatcher) ──────────────────────

        /// <summary>Game thread started, boot overlay should appear.</summary>
        public event Action<GameEntry> Started;

        /// <summary>Boot phase changed (Initializing → Running).</summary>
        public event Action<BootPhase, string> BootPhaseChanged;

        /// <summary>The emulator's render HWND is ready to be embedded.</summary>
        public event Action<IntPtr> WindowReady;

        /// <summary>Game exited cleanly (user stop or natural end).</summary>
        public event Action<int /*exitCode*/> Stopped;

        /// <summary>
        /// Game thread threw an unhandled exception or exited with a non-zero
        /// code that indicates a crash. The emulator shell stays alive.
        /// </summary>
        public event Action<int /*exitCode*/, string /*message*/> Crashed;

        /// <summary>
        /// Watchdog detected the game has not produced a heartbeat for
        /// WatchdogTimeoutSeconds. UI should surface a "Game may be hung" toast.
        /// </summary>
        public event Action Hanging;

        /// <summary>A log line was emitted by the core.</summary>
        public event Action<string> LogLine;

        // ── Public state ──────────────────────────────────────────────────────

        public volatile GameSessionState State = GameSessionState.Idle;
        public GameEntry CurrentGame { get; private set; }

        /// <summary>Set to true when the game thread has received a stop request.</summary>
        public bool StopRequested => _stopRequested;

        // ── Private ───────────────────────────────────────────────────────────

        private Thread _gameThread;
        private volatile bool _stopRequested;
        private readonly Dispatcher _dispatcher;
        private IpcSession _ipc;

        // Watchdog: tracks the last time we received a log line (proxy for
        // "game is alive"). If more than WatchdogTimeoutSeconds elapse without
        // any activity while the game is running, Hanging is raised once.
        private DateTime _lastHeartbeat = DateTime.UtcNow;
        private volatile bool _hangingRaised;
        private DispatcherTimer _watchdogTimer;
        private const double WatchdogTimeoutSeconds = 15.0;

        // Delegate storage (prevent GC collection while native code holds them)
        private readonly CoreBridge.LogCallback _logCallback;
        private readonly CoreBridge.WindowCallback _windowCallback;

        public GameSession(Dispatcher dispatcher)
        {
            _dispatcher = dispatcher ?? throw new ArgumentNullException(nameof(dispatcher));
            _logCallback = OnCoreLog;
            _windowCallback = OnCoreWindow;
        }

        // ── Public API ────────────────────────────────────────────────────────

        /// <summary>
        /// Start a new game session. Safe to call only when State == Idle.
        /// </summary>
        internal void Launch(GameEntry game, CoreBridge.Pcsx5Options options)

        {
            if (State != GameSessionState.Idle)
                throw new InvalidOperationException("A session is already running.");

            CurrentGame = game ?? throw new ArgumentNullException(nameof(game));
            _stopRequested = false;
            _hangingRaised = false;
            _lastHeartbeat = DateTime.UtcNow;
            State = GameSessionState.Booting;

            _gameThread = new Thread(() => GameThreadProc(options, game))
            {
                Name = "pcsx5-game",
                IsBackground = true
            };
            _gameThread.Start();

            _dispatcher.BeginInvoke(() => Started?.Invoke(game));

            StartWatchdog();
        }

        /// <summary>Launch the game via IPC (out-of-process core).</summary>
        internal void LaunchIpc(GameEntry game)
        {
            if (State != GameSessionState.Idle)
                throw new InvalidOperationException("A session is already running.");

            CurrentGame = game ?? throw new ArgumentNullException(nameof(game));
            _stopRequested = false;
            _hangingRaised = false;
            _lastHeartbeat = DateTime.UtcNow;
            State = GameSessionState.Booting;
            _dispatcher.BeginInvoke(() => Started?.Invoke(game));

            _ipc = new IpcSession(_dispatcher);
            _ipc.LogLine += line => Log(line);
            _ipc.Crashed += (code, msg) =>
                _dispatcher.BeginInvoke(() => Crashed?.Invoke(code, msg));
            _ipc.Stopped += code =>
                _dispatcher.BeginInvoke(() => Stopped?.Invoke(code));
            _ipc.FrameReady += () => { }; // signal for frame display

            // Run IPC connect on background thread so the UI stays responsive.
            Task.Run(() =>
            {
                if (_ipc.Launch(game.EbootPath, game.TitleId))
                {
                    State = GameSessionState.Running;
                }
                else
                {
                    _dispatcher.BeginInvoke(() =>
                        Crashed?.Invoke(-1, _ipc.LastError ?? "IPC launch failed"));
                }
            });

            StartWatchdog();
        }

        /// <summary>Access the IPC session for frame display binding.</summary>
        internal IpcSession IpcSession => _ipc;

        /// <summary>
        /// Request the game to stop gracefully. Non-blocking; wait for Stopped/Crashed event.
        /// </summary>
        public void RequestStop()
        {
            if (State == GameSessionState.Idle || State == GameSessionState.Stopped || State == GameSessionState.Crashed)
                return;

            _stopRequested = true;
            if (_ipc != null)
            {
                try { _ipc.RequestStop(); } catch { }
            }
            else
            {
                try { CoreBridge.pcsx5_stop(); } catch { }
            }
        }

        /// <summary>
        /// Immediately terminate the game thread (hard kill). Use only when
        /// RequestStop() does not respond within a few seconds.
        /// </summary>
        public void Kill()
        {
            if (State == GameSessionState.Idle || State == GameSessionState.Stopped || State == GameSessionState.Crashed)
                return;

            _stopRequested = true;
            if (_ipc != null)
            {
                // IPC: kill child process on bg thread so UI doesn't block.
                var ipc = _ipc; _ipc = null;
                Task.Run(() => { try { ipc.Kill(); ipc.Dispose(); } catch { } });
                _dispatcher.BeginInvoke(() => Stopped?.Invoke(-1));
            }
            else
            {
                try { CoreBridge.pcsx5_force_stop(); } catch { }
            }
        }

        /// <summary>Pause the running game (no-op if not running).</summary>
        public void Pause()
        {
            if (State == GameSessionState.Running)
                try { CoreBridge.pcsx5_pause(); } catch { }
        }

        /// <summary>Resume the paused game.</summary>
        public void Resume()
        {
            if (State == GameSessionState.Running)
                try { CoreBridge.pcsx5_resume(); } catch { }
        }

        // ── Game thread ───────────────────────────────────────────────────────

        private void GameThreadProc(CoreBridge.Pcsx5Options options, GameEntry game)
        {
            int exitCode = -1;
            string crashMessage = null;

            try
            {
                // ── Phase 1: Init ─────────────────────────────────────────────
                RaisePhase(BootPhase.Initializing, "Initializing emulator core...");
                int rc = CoreBridge.pcsx5_init(ref options, _logCallback, IntPtr.Zero);
                if (rc != 0)
                {
                    crashMessage = $"pcsx5_init failed with code 0x{rc:X8}. The core DLL may be missing or incompatible.";
                    exitCode = rc;
                    goto Teardown;
                }

                // ── Phase 2: PKG extraction (if applicable) ───────────────────
                string ebootToRun = game.EbootPath;
                if (!string.IsNullOrEmpty(ebootToRun) && ebootToRun.EndsWith(".pkg", StringComparison.OrdinalIgnoreCase))
                {
                    State = GameSessionState.Extracting;
                    RaisePhase(BootPhase.ExtractingPkg, "Extracting PKG archive...");

                    string cacheDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Cache", "PKG_Extracted", game.TitleId);
                    Directory.CreateDirectory(cacheDir);

                    int extRc = CoreBridge.pcsx5_extract_pkg(ebootToRun, cacheDir);
                    string candidate = Path.Combine(cacheDir, "eboot.bin");
                    if (!File.Exists(candidate))
                    {
                        var found = Directory.GetFiles(cacheDir, "eboot.bin", SearchOption.AllDirectories);
                        if (found.Length > 0) candidate = found[0];
                    }

                    if (extRc == 0 && File.Exists(candidate))
                    {
                        ebootToRun = candidate;
                        Log($"[PKG] Resolved executable: {ebootToRun}");
                    }
                    else
                    {
                        Log($"[PKG] Extraction notice (code {extRc}). Attempting direct load...");
                    }

                    State = GameSessionState.Booting;
                }

                // ── Phase 3: Load ELF/SELF ────────────────────────────────────
                RaisePhase(BootPhase.LoadingElf, "Loading executable...");
                rc = CoreBridge.pcsx5_load(ebootToRun);
                if (rc != 0)
                {
                    crashMessage = $"Failed to load '{Path.GetFileName(ebootToRun)}' (code 0x{rc:X8}). The file may be encrypted or corrupted.";
                    exitCode = rc;
                    goto Teardown;
                }

                // ── Phase 4: Run ──────────────────────────────────────────────
                RaisePhase(BootPhase.LinkingModules, "Linking guest modules...");
                State = GameSessionState.Running;
                RaisePhase(BootPhase.Running, "Guest CPU executing...");

                exitCode = CoreBridge.pcsx5_run(_windowCallback, IntPtr.Zero);

                // Check for guest crash info from the core (VEH captured
                // the crash without killing the emulator process).
                IntPtr crashBuf = Marshal.AllocHGlobal(512);
                try
                {
                    if (CoreBridge.pcsx5_get_last_error(crashBuf, 512) == 0)
                    {
                        string coreCrash = Marshal.PtrToStringAnsi(crashBuf) ?? "";
                        if (!string.IsNullOrEmpty(coreCrash))
                        {
                            crashMessage = coreCrash;
                        }
                    }
                }
                finally { Marshal.FreeHGlobal(crashBuf); }
            }
            catch (Exception ex)
            {
                crashMessage = $"Unhandled exception in game thread: {ex.GetType().Name}: {ex.Message}";
                exitCode = -2;
            }

        Teardown:
            try { CoreBridge.pcsx5_shutdown(); } catch { }

            bool wasCrash = crashMessage != null || (exitCode != 0 && !_stopRequested);
            string finalMessage = crashMessage ?? (exitCode == 0
                ? "Game exited normally."
                : $"Game exited with code 0x{exitCode:X8}.");

            Log(finalMessage);

            StopWatchdog();
            State = wasCrash ? GameSessionState.Crashed : GameSessionState.Stopped;
            var capCode = exitCode;
            var capMsg = finalMessage;

            if (wasCrash)
                _dispatcher.BeginInvoke(() => Crashed?.Invoke(capCode, capMsg));
            else
                _dispatcher.BeginInvoke(() => Stopped?.Invoke(capCode));
        }

        // ── Core callbacks ────────────────────────────────────────────────────

        private void OnCoreLog(int level, int category, IntPtr msg, IntPtr user)
        {
            string text = Marshal.PtrToStringAnsi(msg) ?? "";
            string line = $"[{CoreBridge.CategoryName(category)}][{CoreBridge.LevelName(level)}] {text}";

            // Heartbeat: any log line = the game is alive
            _lastHeartbeat = DateTime.UtcNow;
            _hangingRaised = false;

            // Phase sniffing from log keywords
            if (State == GameSessionState.Booting)
            {
                if (text.Contains("link") || text.Contains("reloc") || text.Contains("module"))
                    RaisePhase(BootPhase.LinkingModules, "Linking guest modules...");
                else if (text.Contains("cpu") || text.Contains("thread") || text.Contains("entry"))
                    RaisePhase(BootPhase.StartingCpu, "Starting guest CPU...");
            }

            Log(line);
        }

        private void OnCoreWindow(ulong hwnd, IntPtr user)
        {
            var handle = new IntPtr((long)hwnd);
            _dispatcher.InvokeAsync(() => WindowReady?.Invoke(handle));
        }

        // ── Watchdog ──────────────────────────────────────────────────────────

        private void StartWatchdog()
        {
            _dispatcher.BeginInvoke(() =>
            {
                _watchdogTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(5) };
                _watchdogTimer.Tick += WatchdogTick;
                _watchdogTimer.Start();
            });
        }

        private void StopWatchdog()
        {
            _dispatcher.BeginInvoke(() =>
            {
                _watchdogTimer?.Stop();
                _watchdogTimer = null;
            });
        }

        private void WatchdogTick(object sender, EventArgs e)
        {
            if (State != GameSessionState.Running) return;
            double elapsed = (DateTime.UtcNow - _lastHeartbeat).TotalSeconds;
            if (elapsed >= WatchdogTimeoutSeconds && !_hangingRaised)
            {
                _hangingRaised = true;
                Hanging?.Invoke();
            }
        }

        // ── Helpers ───────────────────────────────────────────────────────────

        private void RaisePhase(BootPhase phase, string message)
        {
            Log($"[Boot] {message}");
            _dispatcher.BeginInvoke(() => BootPhaseChanged?.Invoke(phase, message));
        }

        private void Log(string line)
        {
            _dispatcher.BeginInvoke(() => LogLine?.Invoke(line));
        }

        /// <summary>
        /// Resets the session to Idle so a new game can be launched.
        /// Force-cleans any stale IPC session even if the state machine
        /// still shows the previous session as running.
        /// </summary>
        public void Reset()
        {
            // Dispose the old IPC session (shared memory, pipe, child process).
            if (_ipc != null) { try { _ipc.Dispose(); } catch { } _ipc = null; }
            _gameThread = null;
            CurrentGame = null;
            _stopRequested = false;
            _hangingRaised = false;
            State = GameSessionState.Idle;
        }
    }
}
