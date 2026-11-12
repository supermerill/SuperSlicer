using System;
using System.Runtime.InteropServices;

// WinVerifyTrust verifies the file digest and signature without showing UI or
// modifying a Windows certificate store. Only test mode uses this helper; the
// caller additionally pins and validates the self-signed code-signing certificate.
public static class SignPathTestSignature
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private class FileInfo
    {
        public uint Size = (uint)Marshal.SizeOf(typeof(FileInfo));
        [MarshalAs(UnmanagedType.LPWStr)] public string Path;
        public IntPtr Handle = IntPtr.Zero;
        public IntPtr KnownSubject = IntPtr.Zero;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private class TrustData
    {
        public uint Size = (uint)Marshal.SizeOf(typeof(TrustData));
        public IntPtr PolicyCallbackData = IntPtr.Zero;
        public IntPtr SipClientData = IntPtr.Zero;
        public uint UIChoice = 2; // WTD_UI_NONE
        public uint RevocationChecks = 0;
        public uint UnionChoice = 1; // WTD_CHOICE_FILE
        public IntPtr File;
        public uint StateAction = 1; // WTD_STATEACTION_VERIFY
        public IntPtr StateData = IntPtr.Zero;
        public IntPtr URLReference = IntPtr.Zero;
        public uint ProviderFlags = 0x1010; // cache-only URL retrieval, no revocation (pinned test cert)
        public uint UIContext = 0;
        public IntPtr SignatureSettings = IntPtr.Zero;
    }

    [DllImport("wintrust.dll", ExactSpelling = true)]
    private static extern int WinVerifyTrust(IntPtr window, ref Guid action, [In, Out] TrustData data);

    public static uint Verify(string path)
    {
        var file = new FileInfo { Path = path };
        var pointer = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(FileInfo)));
        var data = new TrustData { File = pointer };
        var action = new Guid("00AAC56B-CD44-11d0-8CC2-00C04FC295EE"); // GENERIC_VERIFY_V2
        Marshal.StructureToPtr(file, pointer, false);
        try
        {
            return unchecked((uint)WinVerifyTrust(new IntPtr(-1), ref action, data));
        }
        finally
        {
            data.StateAction = 2; // WTD_STATEACTION_CLOSE
            WinVerifyTrust(new IntPtr(-1), ref action, data);
            Marshal.DestroyStructure(pointer, typeof(FileInfo));
            Marshal.FreeHGlobal(pointer);
        }
    }
}
