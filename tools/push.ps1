# Uploads one file to the console through xbdm (xbcp can't upload to this console).
#   .\tools\push.ps1 local.ini hdd:\PlexClient\settings.ini
param([Parameter(Mandatory)] [string]$Local, [Parameter(Mandatory)] [string]$Remote)

# xbdm.dll is 32-bit, so re-run this script under 32-bit PowerShell.
if ([Environment]::Is64BitProcess) {
    & "$env:WINDIR\SysWOW64\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -Local $Local -Remote $Remote
    exit $LASTEXITCODE
}

$ErrorActionPreference = 'Stop'
$bin = "$([Environment]::GetEnvironmentVariable('XEDK', 'Machine'))\bin\win32"
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class XbdmPush {
    [DllImport(@"$bin\xbdm.dll", CharSet = CharSet.Ansi)] public static extern int DmSetXboxName(string name);
    [DllImport(@"$bin\xbdm.dll", CharSet = CharSet.Ansi)] public static extern int DmSendFile(string local, string remote);
}
"@
[void][XbdmPush]::DmSetXboxName((Get-ItemProperty 'HKCU:\Software\Microsoft\XenonSDK').XboxName)
$hr = [XbdmPush]::DmSendFile((Resolve-Path $Local).Path, $Remote)
if ($hr -lt 0) { throw ("Upload {0} failed: 0x{1:X8}" -f $Remote, $hr) }
Write-Host "Uploaded $Remote"
