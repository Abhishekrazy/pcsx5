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
        PlayStation = 1 << 17, // PS logo button
        Mic = 1 << 18, // mic mute button below the PS button
    }

    // One touchpad finger contact, decoded from the 4-byte touch point in the
    // input report (offsets match the native dualsense_hid.h DecodeTouchPoint).
    public readonly struct HostGamepadTouchPoint
    {
        public bool Active { get; }
        public byte Id { get; }
        public ushort X { get; } // 0..1919
        public ushort Y { get; } // 0..1079

        public HostGamepadTouchPoint(bool Active, byte Id, ushort X, ushort Y)
        {
            this.Active = Active;
            this.Id = Id;
            this.X = X;
            this.Y = Y;
        }
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
        public HostGamepadTouchPoint Touch0 { get; }
        public HostGamepadTouchPoint Touch1 { get; }
        public HostGamepadSensorVec3 Gyro { get; }
        public HostGamepadSensorVec3 Accel { get; }

        public HostGamepadState(
            bool Connected,
            HostGamepadButtons Buttons,
            byte LeftX,
            byte LeftY,
            byte RightX,
            byte RightY,
            byte LeftTrigger,
            byte RightTrigger,
            HostGamepadTouchPoint Touch0 = default,
            HostGamepadTouchPoint Touch1 = default,
            HostGamepadSensorVec3 Gyro = default,
            HostGamepadSensorVec3 Accel = default)
        {
            this.Connected = Connected;
            this.Buttons = Buttons;
            this.LeftX = LeftX;
            this.LeftY = LeftY;
            this.RightX = RightX;
            this.RightY = RightY;
            this.LeftTrigger = LeftTrigger;
            this.RightTrigger = RightTrigger;
            this.Touch0 = Touch0;
            this.Touch1 = Touch1;
            this.Gyro = Gyro;
            this.Accel = Accel;
        }
    }

    // One 3-axis sensor vector (gyro or accel), raw int16 little-endian as
    // reported (gyro: +/-2000 dps full scale, accel: ~8192 LSB/g).
    public readonly struct HostGamepadSensorVec3
    {
        public short X { get; }
        public short Y { get; }
        public short Z { get; }

        public HostGamepadSensorVec3(short X, short Y, short Z)
        {
            this.X = X;
            this.Y = Y;
            this.Z = Z;
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
        private static byte _outputSequence;
        private static FileStream _outputStream;
        private static byte _rawButtons0;
        private static byte _rawButtons1;
        private static byte _rawButtons2;
        private static byte _motorLeft;
        private static byte _motorRight;
        private static byte _lightbarRed;
        private static byte _lightbarGreen;
        private static byte _lightbarBlue = 64; // PS-style blue default
        private static byte _playerLeds = 0x04; // center LED = player 1
        private static byte _micLed; // 0 = off, 1 = on, 2 = pulse

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

        // Diagnostic: the three raw button bytes from the last input report
        // (hat/face, shoulders, PS/touch-click/mute). Used by the input tester
        // to verify bit-level parsing against hardware.
        internal static void GetRawButtonBytes(out byte b0, out byte b1, out byte b2)
        {
            lock (Gate)
            {
                b0 = _rawButtons0;
                b1 = _rawButtons1;
                b2 = _rawButtons2;
            }
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

        // Microphone LED: 0 = off, 1 = on, 2 = pulse.
        internal static void SetMicLed(byte mode)
        {
            if (mode > 2)
            {
                mode = 0;
            }

            lock (Gate)
            {
                if (_micLed == mode)
                {
                    return;
                }

                _micLed = mode;
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
            // Byte-for-byte the same layout as the native (working)
            // dualsense_hid.h SendOutputLocked:
            //   common[0]    enable flags 0: 0x01 right motor, 0x02 left motor
            //   common[1]    enable flags 1: 0x01 mic LED, 0x04 lightbar color,
            //                0x10 player LEDs, 0x40/0x80 trigger effects
            //   common[2/3]  right (small) / left (large) motor
            //   common[8]    mic LED mode
            //   common[10]   right trigger mode, [11..20] params
            //   common[21]   left trigger mode,  [22..31] params
            //   common[38]   LED flags, [41] LED on/off (0x02 = on),
            //                [42] brightness, [43] player LEDs (+0x20 instant)
            //   common[44..46] lightbar RGB
            Span<byte> common = stackalloc byte[47];
            common[0] = 0x03;              // enable both rumble motors
            common[0] |= 0x04 | 0x08;      // allow right + left trigger FFB
            common[1] = 0x40 | 0x80;       // enable right + left trigger effects
            common[1] |= 0x01;             // enable mic (mute) LED
            common[1] |= 0x04;             // enable lightbar (RGB LED) color
            common[1] |= 0x08;             // ResetLights: release LEDs from wireless
                                           // firmware control (required on BT; the
                                           // working DS5W library sets it every report)
            common[1] |= 0x10;             // enable player LEDs
            common[2] = _motorRight;
            common[3] = _motorLeft;
            common[8] = _micLed;

            // Right trigger effect block: common[10] = type, [11..] = params
            common[10] = _rightTriggerEffect[0];
            for (var i = 1; i < 7; i++)
            {
                common[10 + i] = _rightTriggerEffect[i];
            }

            // Left trigger effect block: common[21] = type, [22..] = params
            common[21] = _leftTriggerEffect[0];
            for (var i = 1; i < 7; i++)
            {
                common[21 + i] = _leftTriggerEffect[i];
            }

            common[38] = 0x03;
            common[41] = 0x02;             // 0x02 = LEDs on (0x01 disables all LEDs)
            common[42] = 0x00;             // brightness: high
            common[43] = (byte)(_playerLeds | 0x20); // 0x20 = instant (no fade)
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
            bool compact = false;
            if (report.Length >= 11 && report[0] == 0x01)
            {
                offset = 1;
            }
            else if (report.Length >= 12 && report[0] == 0x31)
            {
                offset = 2;
            }
            else if (report.Length >= 10 && report[0] == 0x01)
            {
                // Compact Bluetooth 0x01 report (10 bytes, per the official BT
                // report descriptor): sticks at 1..4, buttons at 5/6/7, L2/R2
                // analog at 8/9, no touch finger data. (Same layout the native
                // reader calls "DS4-style".)
                compact = true;
                offset = 1;
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
            byte l2, r2, buttons0, buttons1, buttons2;
            if (compact)
            {
                buttons0 = report[offset + 4];
                buttons1 = report[offset + 5];
                buttons2 = report[offset + 6];
                l2 = report[offset + 7];
                r2 = report[offset + 8];
            }
            else
            {
                l2 = report[offset + 4];
                r2 = report[offset + 5];
                buttons0 = report[offset + 7];
                buttons1 = report[offset + 8];
                buttons2 = report[offset + 9];
            }

            _rawButtons0 = buttons0;
            _rawButtons1 = buttons1;
            _rawButtons2 = buttons2;

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
            buttons |= (buttons2 & 0x01) != 0 ? HostGamepadButtons.PlayStation : 0;
            buttons |= (buttons2 & 0x02) != 0 ? HostGamepadButtons.TouchPad : 0;
            buttons |= (buttons2 & 0x04) != 0 ? HostGamepadButtons.Mic : 0;

            // Touch fingers: 4 bytes each at offset+32 / offset+36 (standard
            // DualSense layout, same as native dualsense_hid.h ParseReport).
            var touch0 = default(HostGamepadTouchPoint);
            var touch1 = default(HostGamepadTouchPoint);
            if (report.Length >= offset + 40)
            {
                touch0 = DecodeTouchPoint(report.Slice(offset + 32, 4));
                touch1 = DecodeTouchPoint(report.Slice(offset + 36, 4));
            }

            // Gyro at offset+15, accel at offset+21, three int16 LE each
            // (same offsets/order as native dualsense_hid.h ParseReport and
            // the Linux hid-playstation driver). Absent in the compact
            // 10-byte BT report.
            var gyro = default(HostGamepadSensorVec3);
            var accel = default(HostGamepadSensorVec3);
            if (!compact && report.Length >= offset + 27)
            {
                gyro = new HostGamepadSensorVec3(
                    ReadS16LE(report.Slice(offset + 15, 2)),
                    ReadS16LE(report.Slice(offset + 17, 2)),
                    ReadS16LE(report.Slice(offset + 19, 2)));
                accel = new HostGamepadSensorVec3(
                    ReadS16LE(report.Slice(offset + 21, 2)),
                    ReadS16LE(report.Slice(offset + 23, 2)),
                    ReadS16LE(report.Slice(offset + 25, 2)));
            }

            state = new HostGamepadState(
                Connected: true,
                Buttons: buttons,
                LeftX: leftX,
                LeftY: leftY,
                RightX: rightX,
                RightY: rightY,
                LeftTrigger: l2,
                RightTrigger: r2,
                Touch0: touch0,
                Touch1: touch1,
                Gyro: gyro,
                Accel: accel);
            return true;
        }

        private static short ReadS16LE(ReadOnlySpan<byte> p) => (short)(p[0] | (p[1] << 8));

        private static HostGamepadTouchPoint DecodeTouchPoint(ReadOnlySpan<byte> p)
        {
            // Bit 7 of byte 0 set = finger NOT touching; low 7 bits = contact id.
            // 12-bit X then 12-bit Y packed into bytes 1..3.
            return new HostGamepadTouchPoint(
                Active: (p[0] & 0x80) == 0,
                Id: (byte)(p[0] & 0x7F),
                X: (ushort)(p[1] | ((p[2] & 0x0F) << 8)),
                Y: (ushort)((p[2] >> 4) | (p[3] << 4)));
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
