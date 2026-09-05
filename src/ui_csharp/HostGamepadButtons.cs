using System;

namespace Pcsx5Ui
{
    /// <summary>
    /// The shell's own button flags, used by its pad-driven navigation and by
    /// the rebind capture.
    /// </summary>
    /// <remarks>
    /// This enum lived inside WindowsDualSenseReader.cs, the shell's former C#
    /// HID reader, and outlives it on purpose: <c>CheckRebindInput</c> derives
    /// the stored binding name from each member's name
    /// (<c>btnFlag.ToString().ToLower()</c> gives "cross", "l1", ...), so these
    /// identifiers are part of the configuration format and must not change.
    /// The reader's state structs were only ever consumed by the input tick,
    /// which now reads the core, and were deleted with it.
    /// </remarks>
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

    public static class HostGamepad
    {
        /// <summary>
        /// Translate the core's pad bitmask -- SCE_PAD bits, plus the PCSX5
        /// extension bit 0x00200000 for the mute button -- into the shell's flags.
        /// The SCE values are the ones the core's MapButtons emits.
        /// </summary>
        public static HostGamepadButtons FromCoreMask(uint sce)
        {
            HostGamepadButtons b = HostGamepadButtons.None;
            if ((sce & 0x00000010u) != 0) b |= HostGamepadButtons.Up;
            if ((sce & 0x00000020u) != 0) b |= HostGamepadButtons.Right;
            if ((sce & 0x00000040u) != 0) b |= HostGamepadButtons.Down;
            if ((sce & 0x00000080u) != 0) b |= HostGamepadButtons.Left;
            if ((sce & 0x00004000u) != 0) b |= HostGamepadButtons.Cross;
            if ((sce & 0x00002000u) != 0) b |= HostGamepadButtons.Circle;
            if ((sce & 0x00008000u) != 0) b |= HostGamepadButtons.Square;
            if ((sce & 0x00001000u) != 0) b |= HostGamepadButtons.Triangle;
            if ((sce & 0x00000400u) != 0) b |= HostGamepadButtons.L1;
            if ((sce & 0x00000800u) != 0) b |= HostGamepadButtons.R1;
            if ((sce & 0x00000100u) != 0) b |= HostGamepadButtons.L2;
            if ((sce & 0x00000200u) != 0) b |= HostGamepadButtons.R2;
            if ((sce & 0x00000002u) != 0) b |= HostGamepadButtons.L3;
            if ((sce & 0x00000004u) != 0) b |= HostGamepadButtons.R3;
            if ((sce & 0x00000008u) != 0) b |= HostGamepadButtons.Options;
            if ((sce & 0x00100000u) != 0) b |= HostGamepadButtons.TouchPad;
            if ((sce & 0x00000001u) != 0) b |= HostGamepadButtons.Back;
            if ((sce & 0x00010000u) != 0) b |= HostGamepadButtons.PlayStation;
            if ((sce & 0x00200000u) != 0) b |= HostGamepadButtons.Mic;
            return b;
        }
    }
}
