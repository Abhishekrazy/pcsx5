using System;
using System.Runtime.InteropServices;

namespace Pcsx5Ui
{
    // P/Invoke bridge to the in-process emulator core (pcsx5_core.dll).
    // Mirrors the C API in src/core_api.h — keep the struct layout and the
    // callback signatures in sync with that header.
    internal static class CoreBridge
    {
        private const string DllName = "pcsx5_core.dll";

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
        internal struct Pcsx5Options
        {
            [MarshalAs(UnmanagedType.LPStr)] public string ConfigDir;
            [MarshalAs(UnmanagedType.LPStr)] public string CrashDir;
            [MarshalAs(UnmanagedType.LPStr)] public string LogFile;
            [MarshalAs(UnmanagedType.LPStr)] public string TitleId;
            [MarshalAs(UnmanagedType.LPStr)] public string ReportPath;
            [MarshalAs(UnmanagedType.LPStr)] public string RegressionReportPath;
            public int StrictImports;
            public int Embed;
            public int InProc;
        }

        // Callers must keep the delegate instances referenced for the whole
        // session (MainWindow stores them in instance fields) so the GC cannot
        // collect them while native code still holds the function pointers.
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void LogCallback(int level, int category, IntPtr msg, IntPtr user);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void WindowCallback(ulong hwnd, IntPtr user);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int pcsx5_init(ref Pcsx5Options options, LogCallback logCb, IntPtr logUser);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int pcsx5_load([MarshalAs(UnmanagedType.LPStr)] string ebootPath);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int pcsx5_run(WindowCallback windowCb, IntPtr windowUser);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void pcsx5_stop();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void pcsx5_force_stop();

        // ── Controller state (additive ABI, 2026-09-06) ────────────────────
        // Mirrors pcsx5_pad_state / pcsx5_pad_firmware in core_api.h field for
        // field. StructSize is checked by the core so a drift between the two
        // fails loudly instead of reading garbage.

        [StructLayout(LayoutKind.Sequential)]
        internal struct PadTouch
        {
            public ushort X;
            public ushort Y;
            public byte Id;
            public byte Active;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct PadState
        {
            public uint StructSize;
            public byte Connected;
            public byte Bluetooth;
            public byte Reserved0a, Reserved0b;
            public uint Buttons;
            public byte Lx, Ly, Rx, Ry;
            public byte L2, R2;
            public byte TouchCount;
            public byte Reserved1;
            public PadTouch Touch0;
            public PadTouch Touch1;
            public float AccelX, AccelY, AccelZ;
            public float GyroX, GyroY, GyroZ;
            public byte BatteryLevel;
            public byte BatteryCharging;
            public byte BatteryFull;
            public byte Headphone;
            public byte MicJack;
            public byte MicMuted;
            public byte UsbData;
            public byte UsbPower;
            public byte TriggerFeedbackL, TriggerFeedbackR;
            public byte Reserved2a, Reserved2b;

            public static PadState Create()
            {
                var s = new PadState();
                s.StructSize = (uint)Marshal.SizeOf<PadState>();
                return s;
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        internal unsafe struct PadFirmware
        {
            public uint StructSize;
            public byte Valid;
            public byte Reserved0a, Reserved0b, Reserved0c;
            public fixed byte BuildDate[12];
            public fixed byte BuildTime[9];
            public byte Reserved1a, Reserved1b, Reserved1c;
            public ushort FirmwareType;
            public ushort SoftwareSeries;
            public uint HardwareInfo;
            public uint MainVersion;
            public ushort UpdateVersion;
            public ushort Reserved2;
            public uint SblVersion;
            public uint DspVersion;
            public uint McuDspVersion;

            public static PadFirmware Create()
            {
                var f = new PadFirmware();
                f.StructSize = (uint)Marshal.SizeOf<PadFirmware>();
                return f;
            }

            // Renderings follow the reference implementation's conventions.
            public static string FormatVersion(uint v) => $"{(v >> 24) & 0xFF}.{(v >> 16) & 0xFF}.{v & 0xFFFF}";
            public static string FormatDsp(uint v) => $"{(v >> 16) & 0xFFFF:X4}_{v & 0xFFFF:X4}";
            public static string FormatUpdate(ushort v) => $"{(v >> 8) & 0xFF:X}.{v & 0xFF:X}";
        }

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int pcsx5_pad_count();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int pcsx5_pad_get_state(int index, ref PadState outState);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int pcsx5_pad_get_firmware(int index, ref PadFirmware outFirmware);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void pcsx5_pad_set_audio_levels(byte volume, byte preamp);

        // Both block for ~2 s: call from a worker thread, never the UI thread.
        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int pcsx5_pad_play_speaker_test();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int pcsx5_pad_play_haptics_test();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void pcsx5_pause();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void pcsx5_resume();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void pcsx5_shutdown();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int pcsx5_extract_pkg(
            [MarshalAs(UnmanagedType.LPStr)] string pkgPath,
            [MarshalAs(UnmanagedType.LPStr)] string outDir);

        // Retrieve the last guest-crash error string.  Returns 0 on success
        // (crash info written into buf), -1 if no crash has occurred.
        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int pcsx5_get_last_error(IntPtr buf, int bufSize);

        // LogCategory / LogLevel names, mirroring src/common/log.cpp so the
        // console panel shows the same [Category][Level] prefix the CLI prints.
        internal static string CategoryName(int category) => category switch
        {
            0 => "Loader",
            1 => "Memory",
            2 => "Kernel",
            3 => "HLE",
            4 => "GPU",
            5 => "Cpu",
            6 => "General",
            _ => "Unknown",
        };

        internal static string LevelName(int level) => level switch
        {
            0 => "Trace",
            1 => "Debug",
            2 => "Info",
            3 => "Warn",
            4 => "Error",
            5 => "Critical",
            _ => "Unknown",
        };
    }
}
