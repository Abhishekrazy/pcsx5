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
        internal static extern void pcsx5_shutdown();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int pcsx5_extract_pkg(
            [MarshalAs(UnmanagedType.LPStr)] string pkgPath,
            [MarshalAs(UnmanagedType.LPStr)] string outDir);

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
