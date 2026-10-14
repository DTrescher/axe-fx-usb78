[CmdletBinding()]
param()

<#
.SYNOPSIS
Verifies the native Axe-Fx III render endpoint is the Windows playback default.

.DESCRIPTION
Finds the unique active render endpoint whose friendly name is exactly
"Speakers (Axe-Fx III)", then checks the Console, Multimedia, and
Communications defaults against that endpoint's enumerated ID. This tool is
read-only and uses the documented Windows Core Audio MMDevice APIs.

https://learn.microsoft.com/windows/win32/coreaudio/mmdevice-api
https://learn.microsoft.com/windows/win32/coreaudio/device-properties
#>

$ErrorActionPreference = 'Stop'

if (-not [Environment]::Is64BitProcess) {
    throw 'Run this tool from 64-bit Windows PowerShell or PowerShell.'
}

$typeName = 'AxeFxUsb78.Tools.EndpointVerifier'
if (-not ($typeName -as [type])) {
    $interopSource = @'
using System;
using System.Runtime.InteropServices;

namespace AxeFxUsb78.Tools
{
    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDeviceEnumerator
    {
        void EnumAudioEndpoints(EDataFlow dataFlow, uint stateMask,
            out IMMDeviceCollection devices);
        void GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role,
            out IMMDevice device);
    }

    [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDeviceCollection
    {
        void GetCount(out uint count);
        void Item(uint index, out IMMDevice device);
    }

    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDevice
    {
        void Activate(ref Guid iid, uint classContext, IntPtr activationParams,
            out IntPtr activatedInterface);
        void OpenPropertyStore(uint storageMode, out IPropertyStore properties);
        void GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        void GetState(out uint state);
    }

    [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IPropertyStore
    {
        void GetCount(out uint count);
        void GetAt(uint index, out PropertyKey key);
        void GetValue(ref PropertyKey key, out PropVariant value);
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct PropertyKey
    {
        public Guid FormatId;
        public uint PropertyId;
    }

    // Native x64 PROPVARIANT: 8-byte header and 16-byte value union.
    [StructLayout(LayoutKind.Explicit, Size = 24)]
    internal struct PropVariant
    {
        [FieldOffset(0)] public ushort Type;
        [FieldOffset(2)] public ushort Reserved1;
        [FieldOffset(4)] public ushort Reserved2;
        [FieldOffset(6)] public ushort Reserved3;
        [FieldOffset(8)] public IntPtr PointerValue;
    }

    internal enum EDataFlow : int
    {
        Render = 0,
        Capture = 1,
        All = 2
    }

    internal enum ERole : int
    {
        Console = 0,
        Multimedia = 1,
        Communications = 2
    }

    public sealed class EndpointStatus
    {
        public string EndpointId;
        public string FriendlyName;
        public string[] RoleNames;
        public string[] DefaultIds;
    }

    public static class EndpointVerifier
    {
        private const string ExpectedName = "Speakers (Axe-Fx III)";
        private const uint DeviceStateActive = 0x00000001;
        private const int PropVariantTypeLpwstr = 31;
        private static readonly Guid DeviceEnumeratorClassId =
            new Guid("BCDE0395-E52F-467C-8E3D-C4579291692E");
        private static readonly ERole[] Roles = new ERole[] {
            ERole.Console, ERole.Multimedia, ERole.Communications
        };
        private static readonly string[] RoleNames = new string[] {
            "Console", "Multimedia", "Communications"
        };

        [DllImport("ole32.dll", PreserveSig = true)]
        private static extern int PropVariantClear(ref PropVariant value);

        public static EndpointStatus GetStatus()
        {
            IMMDeviceEnumerator enumerator = CreateEnumerator();
            IMMDeviceCollection devices = null;
            try
            {
                enumerator.EnumAudioEndpoints(EDataFlow.Render,
                    DeviceStateActive, out devices);
                uint count;
                devices.GetCount(out count);

                string endpointId = null;
                string friendlyName = null;
                for (uint i = 0; i < count; ++i)
                {
                    IMMDevice device = null;
                    try
                    {
                        devices.Item(i, out device);
                        string candidateId;
                        device.GetId(out candidateId);
                        uint state;
                        device.GetState(out state);
                        if (state != DeviceStateActive)
                            continue;

                        string candidateName = ReadFriendlyName(device);
                        if (!String.Equals(candidateName, ExpectedName,
                            StringComparison.Ordinal))
                            continue;

                        if (endpointId != null)
                            throw new InvalidOperationException(
                                "More than one active render endpoint has the exact friendly name '" +
                                ExpectedName + "'; refusing an ambiguous verification.");
                        endpointId = candidateId;
                        friendlyName = candidateName;
                    }
                    finally
                    {
                        Release(device);
                    }
                }

                if (endpointId == null)
                    throw new InvalidOperationException(
                        "No active render endpoint has the exact friendly name '" +
                        ExpectedName + "'. No settings were changed.");

                return new EndpointStatus {
                    EndpointId = endpointId,
                    FriendlyName = friendlyName,
                    RoleNames = (string[])RoleNames.Clone(),
                    DefaultIds = ReadDefaults(enumerator)
                };
            }
            finally
            {
                Release(devices);
                Release(enumerator);
            }
        }

        private static string ReadFriendlyName(IMMDevice device)
        {
            IPropertyStore properties = null;
            PropVariant value = new PropVariant();
            bool haveValue = false;
            try
            {
                device.OpenPropertyStore(0, out properties); // STGM_READ
                PropertyKey key = new PropertyKey {
                    FormatId = new Guid("A45C254E-DF1C-4EFD-8020-67D146A850E0"),
                    PropertyId = 14 // PKEY_Device_FriendlyName
                };
                properties.GetValue(ref key, out value);
                haveValue = true;
                if (value.Type != PropVariantTypeLpwstr || value.PointerValue == IntPtr.Zero)
                    throw new InvalidOperationException(
                        "An endpoint friendly-name property was missing or had an unexpected type.");
                return Marshal.PtrToStringUni(value.PointerValue);
            }
            finally
            {
                if (haveValue)
                    PropVariantClear(ref value);
                Release(properties);
            }
        }

        private static string[] ReadDefaults(IMMDeviceEnumerator enumerator)
        {
            string[] result = new string[Roles.Length];
            for (int i = 0; i < Roles.Length; ++i)
            {
                IMMDevice device = null;
                try
                {
                    enumerator.GetDefaultAudioEndpoint(EDataFlow.Render,
                        Roles[i], out device);
                    device.GetId(out result[i]);
                }
                catch (COMException error)
                {
                    // A role may legitimately have no configured default.
                    if (error.ErrorCode == unchecked((int)0x80070490))
                        result[i] = null;
                    else
                        throw;
                }
                finally
                {
                    Release(device);
                }
            }
            return result;
        }

        private static IMMDeviceEnumerator CreateEnumerator()
        {
            Type type = Type.GetTypeFromCLSID(DeviceEnumeratorClassId, true);
            return (IMMDeviceEnumerator)Activator.CreateInstance(type);
        }

        private static void Release(object value)
        {
            if (value != null && Marshal.IsComObject(value))
            {
                try { Marshal.FinalReleaseComObject(value); }
                catch { }
            }
        }
    }
}
'@

    Add-Type -TypeDefinition $interopSource -Language CSharp -ErrorAction Stop
}

try {
    $status = [AxeFxUsb78.Tools.EndpointVerifier]::GetStatus()
    Write-Host ("Active render endpoint: {0}" -f $status.FriendlyName)
    Write-Host ("Enumerated endpoint ID: {0}" -f $status.EndpointId)

    $mismatches = @()
    for ($i = 0; $i -lt $status.RoleNames.Length; $i++) {
        $current = $status.DefaultIds[$i]
        if ([string]::IsNullOrEmpty($current)) { $current = '<none>' }
        $matches = [string]::Equals(
            $status.DefaultIds[$i], $status.EndpointId,
            [StringComparison]::OrdinalIgnoreCase)
        $label = if ($matches) { 'PASS' } else { 'MISMATCH' }
        Write-Host ("{0}: {1} default = {2}" -f $label, $status.RoleNames[$i], $current)
        if (-not $matches) { $mismatches += $status.RoleNames[$i] }
    }

    if ($mismatches.Count -ne 0) {
        throw ("FAS output verification failed for: {0}. This tool did not change any settings." -f ($mismatches -join ', '))
    }
    Write-Host 'PASS: all three playback roles use the active native Axe-Fx III endpoint.'
}
catch {
    Write-Error -ErrorRecord $_
    throw
}
