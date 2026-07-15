using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace Pcsx5Ui
{
    public static class MicaHelper
    {
        [DllImport("dwmapi.dll")]
        private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

        private const int DWMWA_SYSTEMBACKDROP_TYPE = 38;
        private const int DWMSBT_MICA = 2; // Mica Backdrop
        private const int DWMSBT_TABBED = 4; // Tabbed/Mica Alt
        private const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;

        public static void ApplyMica(Window window)
        {
            try
            {
                var helper = new WindowInteropHelper(window);
                helper.EnsureHandle();
                IntPtr hwnd = helper.Handle;

                // Set immersive dark mode first
                int dark = 1;
                DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ref dark, sizeof(int));

                // Apply Mica
                int value = DWMSBT_MICA;
                DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, ref value, sizeof(int));
            }
            catch
            {
                // Fallback for older Windows versions
            }
        }
    }
}
