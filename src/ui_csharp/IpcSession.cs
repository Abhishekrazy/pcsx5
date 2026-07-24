using System;
using System.Diagnostics;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace Pcsx5Ui
{
    // IPC session — manages a child emulator core process via shared memory + named pipe.
    // The UI side creates the shared memory and pipe, then launches pcsx5_core.exe as a
    // child process with --ipc-map / --ipc-pipe.  Frames, input, and commands flow
    // across the process boundary through the shared memory struct.
    public sealed class IpcSession : IDisposable
    {
        // ── Events (fired on the WPF Dispatcher) ───────────────────────────
        public event Action<string> LogLine;
        public event Action<int, string> Crashed;  // exit code + message
        public event Action<int> Stopped;           // normal exit code

        /// <summary>Fired when a new frame is available in shared memory.</summary>
        public event Action FrameReady;

        // ── Public state ───────────────────────────────────────────────────
        public bool IsRunning => _process != null && !_process.HasExited;
        public string LastError { get; private set; }

        // ── Private ────────────────────────────────────────────────────────
        private Process _process;
        private MemoryMappedFile _mmf;
        private MemoryMappedViewAccessor _view;
        private NamedPipeServerStream _pipe;
        private Thread _pipeWriterThread;
        private Thread _framePollerThread;
        private CancellationTokenSource _cts;

        private const int IpcMagic = 0x50504353;  // "PCSX"
        private const int IpcVersion = 1;
        private const int MaxW = 1920, MaxH = 1080;
        private const int FrameSize = MaxW * MaxH * 4;
        private const int SharedSize = 4096 + FrameSize; // header + frame

        // Shared memory field offsets (must match ipc_shared.h)
        private const int OffMagic = 0;
        private const int OffVersion = 4;
        private const int OffFrameCounter = 8;
        private const int OffFrameW = 16;
        private const int OffFrameH = 20;
        private const int OffFramePitch = 24;
        private const int OffFrameData = 32;
        private const int OffInputButtons = OffFrameData + FrameSize; // 829 prime, not 8k+28
        private const int OffInputLx = OffInputButtons + 8;
        private const int OffInputLy = OffInputLx + 1;
        private const int OffInputRx = OffInputLy + 1;
        private const int OffInputRy = OffInputRx + 1;
        private const int OffInputL2 = OffInputRy + 1;
        private const int OffInputR2 = OffInputL2 + 1;
        private const int OffGameState = OffInputR2 + 4;
        private const int OffCrashCode = OffGameState + 4;
        private const int OffCrashMsg = OffCrashCode + 4;

        // Pipe commands
        private const uint CmdStop = 0x01;
        private const uint CmdKill = 0x02;
        private const uint CmdPause = 0x03;
        private const uint CmdResume = 0x04;

        // Game states
        private const uint StateBoot = 0;
        private const uint StateRun = 1;
        private const uint StateCrashed = 3;
        private const uint StateExited = 4;

        // ── WriteableBitmap for frame display ─────────────────────────────
        private WriteableBitmap _frameBitmap;
        private readonly Dispatcher _dispatcher;
        private ulong _lastFrameCounter;

        public WriteableBitmap FrameBitmap => _frameBitmap;

        public IpcSession(Dispatcher dispatcher)
        {
            _dispatcher = dispatcher ?? Dispatcher.CurrentDispatcher;
        }

        // ── Launch ─────────────────────────────────────────────────────────
        public bool Launch(string ebootPath, string titleId)
        {
            _cts = new CancellationTokenSource();
            string mapName = "PCSX5_IPC_" + Guid.NewGuid().ToString("N");
            string pipeName = "PCSX5_IPC_" + Guid.NewGuid().ToString("N");

            // Create shared memory.
            _mmf = MemoryMappedFile.CreateNew(mapName, SharedSize,
                MemoryMappedFileAccess.ReadWrite);
            _view = _mmf.CreateViewAccessor(0, SharedSize,
                MemoryMappedFileAccess.ReadWrite);

            // Write header.
            _view.Write(OffMagic, IpcMagic);
            _view.Write(OffVersion, IpcVersion);
            _view.Write(OffGameState, StateBoot);

            // Create named pipe server.
            _pipe = new NamedPipeServerStream(pipeName, PipeDirection.InOut,
                1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous);

            // Launch child process.
            var psi = new ProcessStartInfo
            {
                FileName = LocateCoreExe(),
                Arguments = $"--ipc-map={mapName} --ipc-pipe={pipeName} --headless \"{ebootPath}\"",
                UseShellExecute = false,
                CreateNoWindow = true,
                EnvironmentVariables = { ["PCSX5_HEADLESS"] = "1" },
            };
            if (!string.IsNullOrEmpty(titleId))
                psi.Arguments = $"--title-id={titleId} " + psi.Arguments;

            try
            {
                psi.RedirectStandardError = true;
                psi.RedirectStandardOutput = true;
                _process = Process.Start(psi);
                // Forward child's stderr (log output) to the UI console.
                _process.ErrorDataReceived += (s, e) =>
                {
                    if (string.IsNullOrEmpty(e.Data)) return;
                    string line = StripAnsi(e.Data);
                    _dispatcher.BeginInvoke(() => LogLine?.Invoke(line));
                };
                _process.BeginErrorReadLine();
            }
            catch (Exception ex)
            {
                LastError = $"Failed to launch core process: {ex.Message}";
                Cleanup();
                return false;
            }

            // Wait for the core to connect to the pipe (10s timeout).
            try
            {
                var connectTask = Task.Run(() => _pipe.WaitForConnection(), _cts.Token);
                if (connectTask.Wait(10000, _cts.Token))
                {
                    // Connected successfully.
                }
                else if (_process.HasExited)
                {
                    LastError = $"Core process exited (code {_process.ExitCode}) before connecting";
                    Kill(); Cleanup();
                    return false;
                }
                else
                {
                    LastError = "Pipe connection timeout (10s)";
                    Kill(); Cleanup();
                    return false;
                }
            }
            catch (Exception ex)
            {
                LastError = $"Pipe connection failed: {ex.Message}";
                Kill(); Cleanup();
                return false;
            }

            // Start background threads.
            _framePollerThread = new Thread(FramePollerLoop)
            { Name = "IPC-FramePoller", IsBackground = true };
            _framePollerThread.Start();

            return true;
        }

        // ── Stop / Kill ────────────────────────────────────────────────────
        public void RequestStop()
        {
            if (!SendPipeCmd(CmdStop))
            {
                // Pipe not connected yet (still booting) — kill directly.
                KillProcess();
            }
        }

        public void Kill()
        {
            SendPipeCmd(CmdKill);
            KillProcess();
        }

        private void KillProcess()
        {
            if (_process != null && !_process.HasExited)
            {
                try { _process.Kill(); } catch { }
            }
        }

        private bool SendPipeCmd(uint cmd)
        {
            if (_pipe == null || !_pipe.IsConnected) return false;
            try
            {
                var buf = BitConverter.GetBytes(cmd);
                _pipe.Write(buf, 0, 4);
                return true;
            }
            catch { return false; }
        }

        // ── Input write ────────────────────────────────────────────────────
        public void WriteInput(ulong buttons, byte lx, byte ly, byte rx, byte ry,
                                byte l2, byte r2)
        {
            if (_view == null) return;
            _view.Write(OffInputButtons, buttons);
            _view.Write(OffInputLx, lx);
            _view.Write(OffInputLy, ly);
            _view.Write(OffInputRx, rx);
            _view.Write(OffInputRy, ry);
            _view.Write(OffInputL2, l2);
            _view.Write(OffInputR2, r2);
        }

        // ── Frame polling thread ───────────────────────────────────────────
        private void FramePollerLoop()
        {
            var token = _cts.Token;
            ulong lastCounter = 0;
            int emptyPolls = 0;

            while (!token.IsCancellationRequested)
            {
                Thread.Sleep(16); // ~60 fps polling

                if (_view == null) continue;
                ulong counter = _view.ReadUInt64(OffFrameCounter);
                if (counter == lastCounter)
                {
                    emptyPolls++;
                    if (emptyPolls > 300) // ~5s of no frames
                    {
                        CheckGameState();
                        emptyPolls = 0;
                    }
                    continue;
                }
                emptyPolls = 0;
                lastCounter = counter;
                _lastFrameCounter = counter;

                // Read frame dimensions.
                int w = _view.ReadInt32(OffFrameW);
                int h = _view.ReadInt32(OffFrameH);
                if (w <= 0 || h <= 0 || w > MaxW || h > MaxH) continue;

                _dispatcher.BeginInvoke((Action)(() =>
                {
                    UpdateFrameBitmap(w, h);
                    FrameReady?.Invoke();
                }));
            }
        }

        private void UpdateFrameBitmap(int w, int h)
        {
            if (_frameBitmap == null || _frameBitmap.PixelWidth != w || _frameBitmap.PixelHeight != h)
            {
                _frameBitmap = new WriteableBitmap(w, h, 96, 96, PixelFormats.Bgra32, null);
                // The first time, notify the host that the bitmap is ready.
            }

            if (_view == null) return;
            int stride = w * 4;
            byte[] pixels = new byte[stride * h];
            _view.ReadArray(OffFrameData, pixels, 0, pixels.Length);

            _frameBitmap.Lock();
            Marshal.Copy(pixels, 0, _frameBitmap.BackBuffer, pixels.Length);
            _frameBitmap.AddDirtyRect(new Int32Rect(0, 0, w, h));
            _frameBitmap.Unlock();
        }

        private void CheckGameState()
        {
            if (_view == null) return;
            uint state = _view.ReadUInt32(OffGameState);
            if (state == StateCrashed)
            {
                uint code = _view.ReadUInt32(OffCrashCode);
                byte[] msgBytes = new byte[256];
                _view.ReadArray(OffCrashMsg, msgBytes, 0, 256);
                string msg = Encoding.ASCII.GetString(msgBytes).TrimEnd('\0');
                LastError = msg;
                _dispatcher.BeginInvoke(() => Crashed?.Invoke((int)code, msg));
                _cts.Cancel();
            }
        }

        // ── ANSI escape code stripper ────────────────────────────────────
        private static string StripAnsi(string raw)
        {
            int idx;
            while ((idx = raw.IndexOf('\x1b')) >= 0)
            {
                int end = raw.IndexOf('m', idx);
                if (end < 0) { raw = raw.Substring(0, idx); break; }
                raw = raw.Substring(0, idx) + raw.Substring(end + 1);
            }
            return raw;
        }

        // ── Cleanup ────────────────────────────────────────────────────────
        public void Dispose()
        {
            _cts?.Cancel();
            Kill();
            if (_process != null)
            {
                try { _process.WaitForExit(5000); } catch { }
                int exitCode = _process.HasExited ? _process.ExitCode : -1;
                _process.Dispose();
                _process = null;
                if (exitCode >= 0)
                    _dispatcher.BeginInvoke(() => Stopped?.Invoke(exitCode));
            }
            Cleanup();
        }

        private void Cleanup()
        {
            _cts?.Cancel();
            _view?.Dispose();
            _mmf?.Dispose();
            _pipe?.Dispose();
            _view = null;
            _mmf = null;
            _pipe = null;
        }

        // ── Helper: find core executable ───────────────────────────────────
        private static string LocateCoreExe()
        {
            string dir = AppDomain.CurrentDomain.BaseDirectory;
            string[] candidates = {
                Path.Combine(dir, "pcsx5_cli.exe"),
                Path.Combine(dir, "..", "..", "..", "..", "bin", "Release", "pcsx5_cli.exe"),
                Path.Combine(dir, "..", "..", "..", "..", "..", "build", "bin", "Release", "pcsx5_cli.exe"),
            };
            foreach (var c in candidates)
            {
                string p = Path.GetFullPath(c);
                if (File.Exists(p)) return p;
            }
            return "pcsx5_cli.exe"; // hope it's on PATH
        }
    }
}
