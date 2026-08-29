using System;
using System.IO;
using System.Windows;

namespace Pcsx5Ui
{
    public partial class App : Application
    {
        public App()
        {
            AppDomain.CurrentDomain.UnhandledException += (s, e) =>
            {
                try
                {
                    File.AppendAllText("pcsx5_ui_crash.log", $"[{DateTime.UtcNow}] AppDomain Exception: {e.ExceptionObject}\n");
                }
                catch { }
            };

            this.DispatcherUnhandledException += (s, e) =>
            {
                try
                {
                    File.AppendAllText("pcsx5_ui_crash.log", $"[{DateTime.UtcNow}] Dispatcher Exception: {e.Exception}\n");
                }
                catch { }
            };
        }
    }
}
