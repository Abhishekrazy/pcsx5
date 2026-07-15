using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace Pcsx5Ui
{
    internal static class WindowsHidNative
    {
        internal const int DigcfPresent = 0x02;
        internal const int DigcfDeviceInterface = 0x10;
        internal const uint GenericRead = 0x80000000;
        internal const uint GenericWrite = 0x40000000;
        internal const uint FileShareRead = 0x1;
        internal const uint FileShareWrite = 0x2;
        internal const uint OpenExisting = 3;

        [StructLayout(LayoutKind.Sequential)]
        internal struct SpDeviceInterfaceData
        {
            public int CbSize;
            public Guid InterfaceClassGuid;
            public int Flags;
            public nint Reserved;
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct HiddAttributes
        {
            public int Size;
            public ushort VendorId;
            public ushort ProductId;
            public ushort VersionNumber;
        }

        [DllImport("hid.dll")]
        internal static extern void HidD_GetHidGuid(out Guid hidGuid);

        [DllImport("hid.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool HidD_GetAttributes(SafeFileHandle hidDeviceObject, ref HiddAttributes attributes);

        [DllImport("hid.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool HidD_GetFeature(SafeFileHandle hidDeviceObject, [In, Out] byte[] reportBuffer, int reportBufferLength);

        [DllImport("setupapi.dll", EntryPoint = "SetupDiGetClassDevsW")]
        internal static extern nint SetupDiGetClassDevs(ref Guid classGuid, nint enumerator, nint hwndParent, int flags);

        [DllImport("setupapi.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetupDiEnumDeviceInterfaces(
            nint deviceInfoSet,
            nint deviceInfoData,
            ref Guid interfaceClassGuid,
            int memberIndex,
            ref SpDeviceInterfaceData deviceInterfaceData);

        [DllImport("setupapi.dll", EntryPoint = "SetupDiGetDeviceInterfaceDetailW")]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetupDiGetDeviceInterfaceDetail(
            nint deviceInfoSet,
            ref SpDeviceInterfaceData deviceInterfaceData,
            nint deviceInterfaceDetailData,
            int deviceInterfaceDetailDataSize,
            out int requiredSize,
            nint deviceInfoData);

        [DllImport("setupapi.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        internal static extern bool SetupDiDestroyDeviceInfoList(nint deviceInfoSet);

        [DllImport("kernel32.dll", EntryPoint = "CreateFileW", SetLastError = true, CharSet = CharSet.Unicode)]
        internal static extern SafeFileHandle CreateFile(
            string fileName,
            uint desiredAccess,
            uint shareMode,
            nint securityAttributes,
            uint creationDisposition,
            uint flagsAndAttributes,
            nint templateFile);

        internal static List<string> EnumerateHidDevicePaths()
        {
            var paths = new List<string>();
            HidD_GetHidGuid(out var hidGuid);
            var deviceInfoSet = SetupDiGetClassDevs(ref hidGuid, 0, 0, DigcfPresent | DigcfDeviceInterface);
            if (deviceInfoSet == -1 || deviceInfoSet == 0)
            {
                return paths;
            }

            try
            {
                var interfaceData = new SpDeviceInterfaceData
                {
                    CbSize = Marshal.SizeOf<SpDeviceInterfaceData>(),
                };

                for (var index = 0; SetupDiEnumDeviceInterfaces(deviceInfoSet, 0, ref hidGuid, index, ref interfaceData); index++)
                {
                    SetupDiGetDeviceInterfaceDetail(deviceInfoSet, ref interfaceData, 0, 0, out var requiredSize, 0);
                    if (requiredSize <= 0)
                    {
                        continue;
                    }

                    var detailBuffer = Marshal.AllocHGlobal(requiredSize);
                    try
                    {
                        Marshal.WriteInt32(detailBuffer, 8);
                        if (SetupDiGetDeviceInterfaceDetail(deviceInfoSet, ref interfaceData, detailBuffer, requiredSize, out _, 0) &&
                            Marshal.PtrToStringUni(detailBuffer + 4) is string path && path.Length > 0)
                        {
                            paths.Add(path);
                        }
                    }
                    finally
                    {
                        Marshal.FreeHGlobal(detailBuffer);
                    }
                }
            }
            finally
            {
                SetupDiDestroyDeviceInfoList(deviceInfoSet);
            }

            return paths;
        }
    }
}
