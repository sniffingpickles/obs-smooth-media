param(
    [int]$ProcessId
)

$ErrorActionPreference = "Stop"

if (-not $ProcessId) {
    $process = Get-Process obs64 -ErrorAction Stop |
        Select-Object -First 1
    $ProcessId = $process.Id
}

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class ObsWindowCloser
{
    private delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(
        EnumWindowsProc callback,
        IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(
        IntPtr window,
        out uint processId);

    [DllImport("user32.dll")]
    private static extern bool PostMessage(
        IntPtr window,
        uint message,
        IntPtr wordParameter,
        IntPtr longParameter);

    public static int CloseWindowsForProcess(uint targetProcessId)
    {
        const uint WM_CLOSE = 0x0010;
        int count = 0;
        EnumWindows(delegate(IntPtr window, IntPtr parameter) {
            uint processId;
            GetWindowThreadProcessId(window, out processId);
            if (processId == targetProcessId &&
                PostMessage(window, WM_CLOSE, IntPtr.Zero, IntPtr.Zero)) {
                count++;
            }
            return true;
        }, IntPtr.Zero);
        return count;
    }
}
"@

$count = [ObsWindowCloser]::CloseWindowsForProcess([uint32]$ProcessId)
if ($count -eq 0) {
    throw "No top-level window for obs64 PID $ProcessId was visible in " +
        "this Windows session. Run the helper in OBS's interactive session " +
        "or use measure-obs-shutdown.ps1 from SSH."
}
[PSCustomObject]@{
    processId = $ProcessId
    windowsSignaled = $count
    utc = [DateTime]::UtcNow.ToString("o")
} | ConvertTo-Json
