using System;
using System.IO;
using System.Linq;
using System.Threading;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using System.Collections.Generic;

namespace Pcsx5Ui
{
    [Flags]
    public enum HostGamepadButtons : uint
    {
        None = 0,
        Up = 1 << 0,
        Down = 1 << 1,
        Left = 1 << 2,
        Right = 1 << 3,
        Cross = 1 << 4,
        Circle = 1 << 5,
        Square = 1 << 6,
        Triangle = 1 << 7,
        L1 = 1 << 8,
        R1 = 1 << 9,
        L2 = 1 << 10,
        R2 = 1 << 11,
        L3 = 1 << 12,
        R3 = 1 << 13,
        Options = 1 << 14,
        TouchPad = 1 << 15,
        Back = 1 << 16, // DualSense "Create" button
    }

    public readonly struct HostGamepadState
    {
        public bool Connected { get; }
        public HostGamepadButtons Buttons { get; }
        public byte LeftX { get; }
        public byte LeftY { get; }
        public byte RightX { get; }
        public byte RightY { get; }
        public byte LeftTrigger { get; }
        public byte RightTrigger { get; }

        public HostGamepadState(
            bool Connected,
            HostGamepadButtons Buttons,
            byte LeftX,
            byte LeftY,
            byte RightX,
            byte RightY,
            byte LeftTrigger,
            byte RightTrigger)
        {
            this.Connected = Connected;
            this.Buttons = Buttons;
            this.LeftX = LeftX;
            this.LeftY = LeftY;
            this.RightX = RightX;
            this.RightY = RightY;
            this.LeftTrigger = LeftTrigger;
            this.RightTrigger = RightTrigger;
        }
    }

    public enum DualSenseTriggerSide
    {
        Left,
        Right,
        Both,
    }

    // Adaptive trigger effect modes, per the DualSense-Windows (ds5w.h) trigger
    // effect types. Params are packed into the 10-byte effect block of the
    // output report (first 6 bytes used, rest padded with zeros).
    public enum DualSenseTriggerMode
    {
        Off = 0x00,        // NoResitance in ds5w.h
        Feedback = 0x01,   // ContinuousResitance: startPosition + force
        Weapon = 0x02,     // SectionResitance: startPosition + endPosition
        Vibration = 0x26,  // EffectEx: startPosition, keepEffect, begin/middle/end force, frequency
    }

    internal static class WindowsDualSenseReader
    {
        private const ushort SonyVendorId = 0x054C;
        private const ushort DualSenseProductId = 0x0CE6;
        private const ushort DualSenseEdgeProductId = 0x0DF2;

        private static readonly object Gate = new object();
        private static HostGamepadState _state;
        private static bool _started;

        private static string _devicePath;
        private static bool _bluetooth;
        private static bool _outputReady;
        private static bool _lightbarSetupPending;
        private static byte _outputSequence;
        private static FileStream _outputStream;
        private static byte _motorLeft;
        private static byte _motorRight;
        private static byte _lightbarRed;
        private static byte _lightbarGreen;
        private static byte _lightbarBlue = 64; // PS-style blue default
        private static byte _playerLeds = 0x04; // center LED = player 1

        // Adaptive trigger effect blocks: 7 bytes each (type + 6 params), packed
        // into the 10-byte trigger effect zones of the output report.
        private static readonly byte[] _leftTriggerEffect = new byte[7];
        private static readonly byte[] _rightTriggerEffect = new byte[7];

        // True while the controller is connected over Bluetooth (output uses the
        // 0x31 report + CRC32 instead of the plain USB 0x02 report).
        internal static bool IsBluetooth
        {
            get { lock (Gate) { return _outputReady && _bluetooth; } }
        }

        internal static void EnsureStarted()
        {
            if (!OperatingSystem.IsWindows())
            {
                return;
            }

            lock (Gate)
            {
                if (_started)
                {
                    return;
                }

                _started = true;
                var thread = new Thread(ReadLoop)
                {
                    IsBackground = true,
                    Name = "DualSenseReader",
                };
                thread.Start();
            }
        }

        internal static bool TryGetState(out HostGamepadState state)
        {
            lock (Gate)
            {
                state = _state;
            }

            return state.Connected;
        }

        private static void SetState(in HostGamepadState state)
        {
            lock (Gate)
            {
                _state = state;
            }
        }

        internal static void SetRumble(byte largeMotor, byte smallMotor)
        {
            lock (Gate)
            {
                if (_motorLeft == largeMotor && _motorRight == smallMotor)
                {
                    return;
                }

                _motorLeft = largeMotor;
                _motorRight = smallMotor;
                SendOutputLocked();
            }
        }

        internal static void SetLightbar(byte red, byte green, byte blue)
        {
            lock (Gate)
            {
                if (_lightbarRed == red && _lightbarGreen == green && _lightbarBlue == blue)
                {
                    return;
                }

                _lightbarRed = red;
                _lightbarGreen = green;
                _lightbarBlue = blue;
                SendOutputLocked();
            }
        }

        internal static void ResetLightbar() => SetLightbar(0, 0, 64);

        // Player indicator LEDs bitmask (5 LEDs under the touchpad). Common
        // patterns: player 1 = 0x04, 2 = 0x0A, 3 = 0x15, 4 = 0x1B, 5 = 0x1F.
        internal static void SetPlayerLeds(byte bitmask)
        {
            lock (Gate)
            {
                if (_playerLeds == bitmask)
                {
                    return;
                }

                _playerLeds = bitmask;
                SendOutputLocked();
            }
        }

        // Sets an adaptive trigger effect. Parameter meaning depends on the mode
        // (see ds5w.h TriggerEffect union):
        //   Off:       all params ignored
        //   Feedback:  param1 = start position, param2 = force
        //   Weapon:    param1 = start position, param2 = end position (>= start)
        //   Vibration: param1 = start position, param2 = frequency,
        //              force = vibration strength (begin/middle/end forces)
        internal static void SetAdaptiveTrigger(
            DualSenseTriggerSide side,
            DualSenseTriggerMode mode,
            byte param1 = 0,
            byte param2 = 0,
            byte force = 0xFF)
        {
            var effect = new byte[7];
            effect[0] = (byte)mode;
            switch (mode)
            {
                case DualSenseTriggerMode.Feedback: // Continuous resistance
                    effect[1] = param1; // startPosition
                    effect[2] = param2; // force
                    break;
                case DualSenseTriggerMode.Weapon: // Section resistance
                    effect[1] = param1; // startPosition
                    effect[2] = param2; // endPosition
                    break;
                case DualSenseTriggerMode.Vibration: // EffectEx
                    effect[1] = param1;   // startPosition
                    effect[2] = 0x00;     // keepEffect = false
                    effect[3] = force;    // beginForce
                    effect[4] = force;    // middleForce
                    effect[5] = force;    // endForce
                    effect[6] = param2;   // frequency
                    break;
                default: // Off
                    break;
            }

            lock (Gate)
            {
                if (side == DualSenseTriggerSide.Left || side == DualSenseTriggerSide.Both)
                {
                    Array.Copy(effect, _leftTriggerEffect, effect.Length);
                }
                if (side == DualSenseTriggerSide.Right || side == DualSenseTriggerSide.Both)
                {
                    Array.Copy(effect, _rightTriggerEffect, effect.Length);
                }
                SendOutputLocked();
            }
        }

        // Clears any adaptive trigger effect on the given side(s).
        internal static void ResetTrigger(DualSenseTriggerSide side) =>
            SetAdaptiveTrigger(side, DualSenseTriggerMode.Off);

        private static void OnDeviceIdentified(string path, bool bluetooth)
        {
            lock (Gate)
            {
                _devicePath = path;
                _bluetooth = bluetooth;
                _outputReady = true;
                _lightbarSetupPending = true;
                SendOutputLocked();
            }
        }

        private static void OnDeviceLost()
        {
            lock (Gate)
            {
                _devicePath = null;
                _outputReady = false;
                _motorLeft = 0;
                _motorRight = 0;
                _outputStream?.Dispose();
                _outputStream = null;
            }
        }

        private static void SendOutputLocked()
        {
            if (!_outputReady || _devicePath is null)
            {
                return;
            }

            try
            {
                if (_outputStream is null)
                {
                    var handle = WindowsHidNative.CreateFile(
                        _devicePath,
                        WindowsHidNative.GenericRead | WindowsHidNative.GenericWrite,
                        WindowsHidNative.FileShareRead | WindowsHidNative.FileShareWrite,
                        0, WindowsHidNative.OpenExisting, 0, 0);
                    if (handle.IsInvalid)
                    {
                        handle.Dispose();
                        return;
                    }

                    _outputStream = new FileStream(handle, FileAccess.Write, bufferSize: 1);
                }

                var report = BuildOutputReportLocked();
                _outputStream.Write(report, 0, report.Length);
                _outputStream.Flush();
            }
            catch (Exception)
            {
                _outputStream?.Dispose();
                _outputStream = null;
            }
        }

        private static byte[] BuildOutputReportLocked()
        {
            Span<byte> common = stackalloc byte[47];
            common[0] = 0x03;             // flags0: enable both rumble motors
            common[0] |= 0x04 | 0x08;     // flags0: enable right + left trigger effects
            common[1] = 0x04 | 0x10;      // flags1: lightbar setup + player indicator
            common[2] = _motorRight;
            common[3] = _motorLeft;

            // Right trigger effect block: common[10] = type, common[11..16] = params
            common[10] = _rightTriggerEffect[0];
            for (var i = 1; i < 7; i++)
            {
                common[10 + i] = _rightTriggerEffect[i];
            }

            // Left trigger effect block: common[21] = type, common[22..27] = params
            common[21] = _leftTriggerEffect[0];
            for (var i = 1; i < 7; i++)
            {
                common[21 + i] = _leftTriggerEffect[i];
            }

            if (_lightbarSetupPending)
            {
                common[38] |= 0x02;
                common[41] = 0x01;
                _lightbarSetupPending = false;
            }

            common[43] = _playerLeds;
            common[44] = _lightbarRed;
            common[45] = _lightbarGreen;
            common[46] = _lightbarBlue;

            if (!_bluetooth)
            {
                var usbReport = new byte[48];
                usbReport[0] = 0x02;
                common.CopyTo(usbReport.AsSpan(1));
                return usbReport;
            }

            var btReport = new byte[78];
            btReport[0] = 0x31;
            btReport[1] = (byte)((_outputSequence & 0x0F) << 4);
            _outputSequence = (byte)((_outputSequence + 1) & 0x0F);
            btReport[2] = 0x10;
            common.CopyTo(btReport.AsSpan(3));
            var crc = Crc32(0xA2, btReport.AsSpan(0, 74));
            btReport[74] = (byte)crc;
            btReport[75] = (byte)(crc >> 8);
            btReport[76] = (byte)(crc >> 16);
            btReport[77] = (byte)(crc >> 24);
            return btReport;
        }

        private static uint Crc32(byte seed, ReadOnlySpan<byte> data)
        {
            var crc = Crc32Update(0xFFFFFFFFu, seed);
            foreach (var value in data)
            {
                crc = Crc32Update(crc, value);
            }

            return ~crc;
        }

        private static uint Crc32Update(uint crc, byte value)
        {
            crc ^= value;
            for (var bit = 0; bit < 8; bit++)
            {
                crc = (crc >> 1) ^ (0xEDB88320u & (uint)-(int)(crc & 1));
            }

            return crc;
        }

        private static void ReadLoop()
        {
            var announcedConnect = false;
            while (true)
            {
                SafeFileHandle handle = null;
                try
                {
                    handle = OpenDualSense(out var devicePath);
                    if (handle is null || devicePath is null)
                    {
                        SetState(default);
                        announcedConnect = false;
                        Thread.Sleep(1000);
                        continue;
                    }

                    var feature = new byte[41];
                    feature[0] = 0x05;
                    _ = WindowsHidNative.HidD_GetFeature(handle, feature, feature.Length);

                    if (!announcedConnect)
                    {
                        Console.Error.WriteLine("[LOADER][INFO] DualSense controller connected.");
                        announcedConnect = true;
                    }

                    using (var stream = new FileStream(handle, FileAccess.Read, bufferSize: 1))
                    {
                        handle = null;
                        var buffer = new byte[256];
                        var transportKnown = false;
                        while (true)
                        {
                            var read = stream.Read(buffer, 0, buffer.Length);
                            if (read <= 0)
                            {
                                break;
                            }

                            if (TryParseReport(buffer.AsSpan(0, read), out var state))
                            {
                                if (!transportKnown)
                                {
                                    transportKnown = true;
                                    OnDeviceIdentified(devicePath, bluetooth: buffer[0] == 0x31);
                                }

                                SetState(state);
                            }
                        }
                    }
                }
                catch (Exception)
                {
                }
                finally
                {
                    handle?.Dispose();
                }

                if (announcedConnect)
                {
                    Console.Error.WriteLine("[LOADER][INFO] DualSense controller disconnected.");
                    announcedConnect = false;
                }

                OnDeviceLost();
                SetState(default);
                Thread.Sleep(1000);
            }
        }

        private static SafeFileHandle OpenDualSense(out string devicePath)
        {
            devicePath = null;
            foreach (var path in WindowsHidNative.EnumerateHidDevicePaths())
            {
                using (var probe = WindowsHidNative.CreateFile(
                    path, 0, WindowsHidNative.FileShareRead | WindowsHidNative.FileShareWrite, 0, WindowsHidNative.OpenExisting, 0, 0))
                {
                    if (probe.IsInvalid)
                    {
                        continue;
                    }

                    var attributes = new WindowsHidNative.HiddAttributes { Size = Marshal.SizeOf<WindowsHidNative.HiddAttributes>() };
                    if (!WindowsHidNative.HidD_GetAttributes(probe, ref attributes) ||
                        attributes.VendorId != SonyVendorId ||
                        (attributes.ProductId != DualSenseProductId && attributes.ProductId != DualSenseEdgeProductId))
                    {
                        continue;
                    }
                }

                var handle = WindowsHidNative.CreateFile(
                    path,
                    WindowsHidNative.GenericRead | WindowsHidNative.GenericWrite,
                    WindowsHidNative.FileShareRead | WindowsHidNative.FileShareWrite,
                    0, WindowsHidNative.OpenExisting, 0, 0);
                if (handle.IsInvalid)
                {
                    handle.Dispose();
                    handle = WindowsHidNative.CreateFile(
                        path,
                        WindowsHidNative.GenericRead,
                        WindowsHidNative.FileShareRead | WindowsHidNative.FileShareWrite,
                        0, WindowsHidNative.OpenExisting, 0, 0);
                }

                if (!handle.IsInvalid)
                {
                    devicePath = path;
                    return handle;
                }

                handle.Dispose();
            }

            return null;
        }

        private static bool TryParseReport(ReadOnlySpan<byte> report, out HostGamepadState state)
        {
            int offset;
            if (report.Length >= 11 && report[0] == 0x01)
            {
                offset = 1;
            }
            else if (report.Length >= 12 && report[0] == 0x31)
            {
                offset = 2;
            }
            else
            {
                state = default;
                return false;
            }

            var leftX = report[offset + 0];
            var leftY = report[offset + 1];
            var rightX = report[offset + 2];
            var rightY = report[offset + 3];
            var l2 = report[offset + 4];
            var r2 = report[offset + 5];
            var buttons0 = report[offset + 7];
            var buttons1 = report[offset + 8];
            var buttons2 = report[offset + 9];

            var buttons = HostGamepadButtons.None;
            buttons |= (buttons0 & 0x10) != 0 ? HostGamepadButtons.Square : 0;
            buttons |= (buttons0 & 0x20) != 0 ? HostGamepadButtons.Cross : 0;
            buttons |= (buttons0 & 0x40) != 0 ? HostGamepadButtons.Circle : 0;
            buttons |= (buttons0 & 0x80) != 0 ? HostGamepadButtons.Triangle : 0;
            buttons |= HatToButtons(buttons0 & 0x0F);
            buttons |= (buttons1 & 0x01) != 0 ? HostGamepadButtons.L1 : 0;
            buttons |= (buttons1 & 0x02) != 0 ? HostGamepadButtons.R1 : 0;
            buttons |= (buttons1 & 0x04) != 0 ? HostGamepadButtons.L2 : 0;
            buttons |= (buttons1 & 0x08) != 0 ? HostGamepadButtons.R2 : 0;
            buttons |= (buttons1 & 0x10) != 0 ? HostGamepadButtons.Back : 0;
            buttons |= (buttons1 & 0x20) != 0 ? HostGamepadButtons.Options : 0;
            buttons |= (buttons1 & 0x40) != 0 ? HostGamepadButtons.L3 : 0;
            buttons |= (buttons1 & 0x80) != 0 ? HostGamepadButtons.R3 : 0;
            buttons |= (buttons2 & 0x02) != 0 ? HostGamepadButtons.TouchPad : 0;

            state = new HostGamepadState(
                Connected: true,
                Buttons: buttons,
                LeftX: leftX,
                LeftY: leftY,
                RightX: rightX,
                RightY: rightY,
                LeftTrigger: l2,
                RightTrigger: r2);
            return true;
        }

        private static HostGamepadButtons HatToButtons(int hat) => hat switch
        {
            0 => HostGamepadButtons.Up,
            1 => HostGamepadButtons.Up | HostGamepadButtons.Right,
            2 => HostGamepadButtons.Right,
            3 => HostGamepadButtons.Right | HostGamepadButtons.Down,
            4 => HostGamepadButtons.Down,
            5 => HostGamepadButtons.Down | HostGamepadButtons.Left,
            6 => HostGamepadButtons.Left,
            7 => HostGamepadButtons.Left | HostGamepadButtons.Up,
            _ => 0,
        };
    }
}
