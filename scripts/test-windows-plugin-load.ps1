param(
    [Parameter(Mandatory = $true)]
    [string]$ObsDirectory,

    [Parameter(Mandatory = $true)]
    [string]$PluginPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$obsBin = Join-Path $ObsDirectory "bin\64bit"
$obsExe = Join-Path $obsBin "obs64.exe"
if (-not (Test-Path -LiteralPath $obsExe)) {
    throw "OBS was not found at $obsExe"
}
if (-not (Test-Path -LiteralPath $PluginPath)) {
    throw "Plugin was not found at $PluginPath"
}

Add-Type @"
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class SmoothMediaLibraryProbe
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool SetDllDirectory(string path);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr LoadLibrary(string path);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FreeLibrary(IntPtr module);

    public static void Load(string dependencyDirectory, string pluginPath)
    {
        if (!SetDllDirectory(dependencyDirectory))
            throw new Win32Exception(Marshal.GetLastWin32Error());

        IntPtr module = LoadLibrary(pluginPath);
        if (module == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error());

        if (!FreeLibrary(module))
            throw new Win32Exception(Marshal.GetLastWin32Error());
    }
}
"@

$resolvedPlugin = (Resolve-Path -LiteralPath $PluginPath).Path
[SmoothMediaLibraryProbe]::Load($obsBin, $resolvedPlugin)
Write-Output "Windows loaded $resolvedPlugin with the OBS runtime at $obsBin"
